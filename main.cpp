#define GL_GLEXT_PROTOTYPES
#include "libretro.h"
#include "libretro_vulkan.h"
#include "embedded_assets.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_vulkan.h>
#include <GL/gl.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
#include <random>
#include <cstdio>
#include <cstdarg>
#include <cerrno>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>

namespace {
constexpr int FRONTEND_WIDTH = 800;
constexpr int FRONTEND_HEIGHT = 800;
using retro_init_t = void (*)();
using retro_deinit_t = void (*)();
using retro_api_version_t = unsigned (*)();
using retro_get_system_info_t = void (*)(retro_system_info *);
using retro_get_system_av_info_t = void (*)(retro_system_av_info *);
using retro_set_environment_t = void (*)(retro_environment_t);
using retro_set_video_refresh_t = void (*)(retro_video_refresh_t);
using retro_set_audio_sample_t = void (*)(retro_audio_sample_t);
using retro_set_audio_sample_batch_t = void (*)(retro_audio_sample_batch_t);
using retro_set_input_poll_t = void (*)(retro_input_poll_t);
using retro_set_input_state_t = void (*)(retro_input_state_t);
using retro_load_game_t = retro_bool (*)(const retro_game_info *);
using retro_unload_game_t = void (*)();
using retro_run_t = void (*)();
using retro_get_memory_size_t = size_t (*)(unsigned);
using retro_get_memory_data_t = void *(*)(unsigned);

struct Core {
  using Handle =
#ifdef _WIN32
      HMODULE;
#else
      void *;
#endif
  Handle handle = nullptr;
  retro_init_t init = nullptr;
  retro_deinit_t deinit = nullptr;
  retro_get_system_info_t get_system_info = nullptr;
  retro_get_system_av_info_t get_system_av_info = nullptr;
  retro_set_environment_t set_environment = nullptr;
  retro_set_video_refresh_t set_video_refresh = nullptr;
  retro_set_audio_sample_t set_audio_sample = nullptr;
  retro_set_audio_sample_batch_t set_audio_sample_batch = nullptr;
  retro_set_input_poll_t set_input_poll = nullptr;
  retro_set_input_state_t set_input_state = nullptr;
  retro_load_game_t load_game = nullptr;
  retro_unload_game_t unload_game = nullptr;
  retro_run_t run = nullptr;
  retro_get_memory_size_t get_memory_size = nullptr;
  retro_get_memory_data_t get_memory_data = nullptr;

  template <typename T> bool load(T &function, const char *name) {
    function = reinterpret_cast<T>(
#ifdef _WIN32
        GetProcAddress(handle, name)
#else
        dlsym(handle, name)
#endif
    );
    return function != nullptr;
  }

};

Core::Handle open_dynamic_library(const std::filesystem::path &path) {
#ifdef _WIN32
  return LoadLibraryW(path.wstring().c_str());
#else
  return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void close_dynamic_library(Core::Handle handle) {
  if (!handle) return;
#ifdef _WIN32
  FreeLibrary(handle);
#else
  dlclose(handle);
#endif
}

std::string dynamic_library_error() {
#ifdef _WIN32
  const DWORD code = GetLastError();
  if (!code) return {};
  LPWSTR message = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
  std::wstring wide(message ? message : L"", length);
  if (message) LocalFree(message);
  return std::string(wide.begin(), wide.end());
#else
  const char *message = dlerror();
  return message ? message : "";
#endif
}

bool install_application_icon() {
#ifdef _WIN32
  return true;
#else
  const char *home = std::getenv("HOME");
  if (!home) return false;
  const std::filesystem::path icon_path =
      std::filesystem::path(home) / ".local" / "share" / "icons" /
      "flowmulator.svg";
  std::error_code error;
  std::filesystem::create_directories(icon_path.parent_path(), error);
  if (error) _exit(1);
  std::ofstream output(icon_path, std::ios::binary | std::ios::trunc);
  if (!output) return false;
  output.write(reinterpret_cast<const char *>(_binary__assets_flowmulator_svg_start),
               static_cast<std::streamsize>(embedded_size(
                   _binary__assets_flowmulator_svg_start,
                   _binary__assets_flowmulator_svg_end)));
  return static_cast<bool>(output);
#endif
}

std::vector<uint32_t> framebuffer(240 * 160);
bool logged_3ds_frame = false;
bool three_ds_hardware_render = false;
std::vector<uint32_t> crt_framebuffer(240 * 160);
unsigned frame_width = 240, frame_height = 160;
unsigned buttons = 0;
unsigned pixel_format = RETRO_PIXEL_FORMAT_XRGB8888;
SDL_AudioDeviceID audio_device = 0;
bool crt_filter = false;
bool gba_crt_filter = false;
bool nds_crt_filter = false;
bool gba_overlay = false;
bool nds_overlay = false;
int nds_layout_index = -1;
const std::array<const char *, 3> nds_layouts{
    "top/bottom", "bottom/top", "top only",
    };
enum class InputMode { Keyboard, Controller, RealWiimote };
InputMode gba_input_mode = InputMode::Keyboard;
InputMode nds_input_mode = InputMode::Keyboard;
InputMode wii_input_mode = InputMode::Keyboard;
void cycle_wii_input_mode() {
  wii_input_mode = wii_input_mode == InputMode::Keyboard
      ? InputMode::Controller
      : wii_input_mode == InputMode::Controller
          ? InputMode::RealWiimote
          : InputMode::Keyboard;
}
std::string input_swap_notification;
Uint32 input_swap_notification_until = 0;
SDL_GameController *game_controller = nullptr;
std::string game_controller_name;
std::array<SDL_GameControllerButton, 10> gba_controller_buttons{
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER};
std::array<SDL_GameControllerAxis, 10> gba_controller_axes{};
std::array<bool, 10> gba_controller_axis_positive{};
std::array<bool, 10> gba_controller_axis_bound{};
std::array<SDL_GameControllerButton, 12> nds_controller_buttons{
    SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
    SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
    SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
std::array<SDL_GameControllerAxis, 12> nds_controller_axes{};
std::array<bool, 12> nds_controller_axis_positive{};
std::array<bool, 12> nds_controller_axis_bound{};
std::array<SDL_GameControllerButton, 12> wii_controller_buttons =
    nds_controller_buttons;
std::array<SDL_GameControllerAxis, 12> wii_controller_axes{};
std::array<bool, 12> wii_controller_axis_positive{};
std::array<bool, 12> wii_controller_axis_bound{};
std::array<SDL_GameControllerButton, 4> wii_tilt_controller_buttons{
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
std::array<SDL_GameControllerAxis, 4> wii_tilt_controller_axes{};
std::array<bool, 4> wii_tilt_controller_axis_positive{};
std::array<bool, 4> wii_tilt_controller_axis_bound{};
SDL_GameControllerButton wii_shake_controller_button =
    SDL_CONTROLLER_BUTTON_X;
SDL_GameControllerAxis wii_shake_controller_axis = SDL_CONTROLLER_AXIS_INVALID;
bool wii_shake_controller_axis_positive = false;
bool wii_shake_controller_axis_bound = false;
void process_easter_egg_sequence(SDL_Keycode key);

SDL_Keycode controller_navigation_key(SDL_GameControllerButton button) {
  switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return SDLK_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return SDLK_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return SDLK_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return SDLK_RIGHT;
    case SDL_CONTROLLER_BUTTON_A:
    case SDL_CONTROLLER_BUTTON_START: return SDLK_RETURN;
    case SDL_CONTROLLER_BUTTON_B:
    case SDL_CONTROLLER_BUTTON_BACK: return SDLK_ESCAPE;
    default: return SDLK_UNKNOWN;
  }
}

bool translate_controller_event(SDL_Event &event) {
  if (event.type == SDL_KEYDOWN && !event.key.repeat)
    process_easter_egg_sequence(event.key.keysym.sym);
  if (event.type == SDL_MOUSEBUTTONDOWN &&
      event.button.button == SDL_BUTTON_RIGHT) {
    event.type = SDL_KEYDOWN;
    event.key.repeat = 0;
    event.key.keysym.sym = SDLK_ESCAPE;
    return true;
  }
  if (event.type != SDL_CONTROLLERBUTTONDOWN) return false;
  const SDL_Keycode key = controller_navigation_key(
      static_cast<SDL_GameControllerButton>(event.cbutton.button));
  if (key == SDLK_UNKNOWN) return false;
  event.type = SDL_KEYDOWN;
  event.key.repeat = 0;
  event.key.keysym.sym = key;
  return true;
}

bool controller_button_event(const SDL_Event &event,
                             SDL_GameControllerButton &button) {
  if (event.type != SDL_CONTROLLERBUTTONDOWN) return false;
  button = static_cast<SDL_GameControllerButton>(event.cbutton.button);
  return true;
}

bool controller_axis_event(const SDL_Event &event, SDL_GameControllerAxis &axis,
                           bool &positive) {
  if (event.type != SDL_CONTROLLERAXISMOTION ||
      std::abs(event.caxis.value) < 16000)
    return false;
  axis = static_cast<SDL_GameControllerAxis>(event.caxis.axis);
  positive = event.caxis.value > 0;
  return true;
}

bool translate_frontend_controller_event(SDL_Event &event,
                                         bool station_frontend = false) {
  if (event.type == SDL_KEYDOWN && !event.key.repeat)
    process_easter_egg_sequence(event.key.keysym.sym);
  if (event.type == SDL_MOUSEBUTTONDOWN &&
      event.button.button == SDL_BUTTON_RIGHT) {
    event.type = SDL_KEYDOWN;
    event.key.repeat = 0;
    event.key.keysym.sym = SDLK_ESCAPE;
    return true;
  }
  if (event.type != SDL_CONTROLLERBUTTONDOWN) return false;
  const auto button =
      static_cast<SDL_GameControllerButton>(event.cbutton.button);
  SDL_Keycode key = SDLK_UNKNOWN;
  if (button == SDL_CONTROLLER_BUTTON_BACK) {
    key = SDLK_t;
  } else if (button == SDL_CONTROLLER_BUTTON_Y) {
    key = SDLK_p;
  } else if (station_frontend &&
             button == SDL_CONTROLLER_BUTTON_START) {
    key = SDLK_ESCAPE;
  }
  if (key == SDLK_UNKNOWN) return translate_controller_event(event);
  event.type = SDL_KEYDOWN;
  event.key.repeat = 0;
  event.key.keysym.sym = key;
  return true;
}
enum class Theme { Sakura, Edgerunners, Berserk };
Theme active_theme = Theme::Sakura;
bool station_menu_rendering = false;
bool dark_page_rendering = false;

struct ScopedFlag {
  bool &value;
  bool previous;
  ScopedFlag(bool &value, bool state) : value(value), previous(value) {
    value = state;
  }
  ~ScopedFlag() { value = previous; }
};
bool quit_requested = false;
bool frontend_escape_requested = false;
int mouse_x = 0;
int mouse_y = 0;
int mouse_delta_x = 0;
int mouse_delta_y = 0;
bool mouse_pressed = false;
bool menu_selection_visible = true;
bool mouse_hover_selection = false;
int last_control_first = 0;
std::string core_save_directory;
std::string core_system_directory;
SDL_GLContext gl_context = nullptr;
SDL_Window *theme_window = nullptr;
SDL_Window *emulator_window = nullptr;
void draw_easter_eggs_notice(SDL_Renderer *renderer);

bool mouse_menu_event(SDL_Event &event, int &selected,
                      const SDL_Rect *items, size_t item_count,
                      bool select_on_hover = true) {
  if (event.type != SDL_MOUSEMOTION) mouse_hover_selection = false;
  if (event.type == SDL_KEYDOWN || event.type == SDL_CONTROLLERBUTTONDOWN)
    menu_selection_visible = true;
  int x = 0;
  int y = 0;
  bool activate = false;
  if (event.type == SDL_MOUSEMOTION && select_on_hover) {
    menu_selection_visible = false;
    x = event.motion.x;
    y = event.motion.y;
  } else if (event.type == SDL_MOUSEMOTION) {
    return false;
  } else if (event.type == SDL_MOUSEBUTTONDOWN &&
             event.button.button == SDL_BUTTON_LEFT) {
    menu_selection_visible = false;
    x = event.button.x;
    y = event.button.y;
    activate = true;
  } else {
    return false;
  }

  for (size_t i = 0; i < item_count; ++i) {
    if (x < items[i].x || x >= items[i].x + items[i].w ||
        y < items[i].y || y >= items[i].y + items[i].h)
      continue;
    selected = static_cast<int>(i);
    menu_selection_visible = true;
    if (event.type == SDL_MOUSEMOTION) mouse_hover_selection = true;
    if (activate) {
      mouse_hover_selection = true;
      event.type = SDL_KEYDOWN;
      event.key.repeat = 0;
      event.key.keysym.sym = SDLK_RETURN;
    }
    return true;
  }
  return false;
}

bool mouse_footer_back(const SDL_Event &event) {
  return event.type == SDL_MOUSEBUTTONDOWN &&
         event.button.button == SDL_BUTTON_LEFT &&
         event.button.x >= 200 && event.button.x < 600 &&
         event.button.y >= 700;
}

void update_control_mouse_hover(unsigned &selected, int first,
                                int visible_rows) {
  int x = 0;
  int y = 0;
  SDL_GetMouseState(&x, &y);
  const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
  const int hovered_row = (y - (controls_top - 7)) / 34;
  if (x >= 170 && x < 630 && hovered_row >= 0 &&
      hovered_row < visible_rows &&
      y < controls_top - 7 + visible_rows * 34) {
    selected = static_cast<unsigned>(first + hovered_row);
    menu_selection_visible = true;
  } else {
    menu_selection_visible = false;
  }
}

bool mouse_over_control_list(int first, int option_count) {
  int x = 0;
  int y = 0;
  SDL_GetMouseState(&x, &y);
  const int visible_rows = std::min(10, option_count - first);
  const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
  const int scrollable_rows = std::max(0, visible_rows - 3);
  return x >= 170 && x < 630 && y >= controls_top - 7 &&
         y < controls_top - 7 + scrollable_rows * 34;
}

const char *theme_window_title() {
  return active_theme == Theme::Edgerunners ? "Flowmulator - RUNNERS" :
         active_theme == Theme::Berserk ? "Flowmulator - BERSERK" :
         "Flowmulator - SAKURA";
}

const char *emulator_window_title() {
  return active_theme == Theme::Edgerunners
             ? "Flowmulator Emulator - RUNNERS"
             : active_theme == Theme::Berserk
                   ? "Flowmulator Emulator - BERSERK"
                   : "Flowmulator Emulator - SAKURA";
}

void hardware_context_reset() {}
uintptr_t hardware_current_framebuffer() { return 0; }
void *hardware_get_proc_address(const char *name) {
  return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
}
int title_edition = 0;
bool nds_cooked_title = false;
bool ur_cooked_active = false;
bool skywalker_active = false;
bool drilboor_active = false;
bool big_forehead_active = false;
bool all_easter_eggs_enabled = false;
bool drilboor_effect_active() {
  return all_easter_eggs_enabled || drilboor_active;
}
bool big_forehead_effect_active() {
  return all_easter_eggs_enabled || big_forehead_active;
}
bool ur_cooked_effect_active() {
  return all_easter_eggs_enabled || ur_cooked_active;
}
bool skywalker_effect_active() {
  return all_easter_eggs_enabled || skywalker_active;
}
int easter_eggs_notice_kind = 0;
size_t enable_sequence_progress = 0;
size_t disable_sequence_progress = 0;
Uint32 sequence_started = 0;
Uint32 easter_eggs_notice_until = 0;

void process_easter_egg_sequence(SDL_Keycode raw_key) {
  constexpr std::array<SDL_Keycode, 8> enable_sequence{
      SDLK_UP, SDLK_UP, SDLK_DOWN, SDLK_DOWN,
      SDLK_LEFT, SDLK_RIGHT, SDLK_LEFT, SDLK_RIGHT};
  constexpr std::array<SDL_Keycode, 8> disable_sequence{
      SDLK_UP, SDLK_DOWN, SDLK_UP, SDLK_DOWN,
      SDLK_LEFT, SDLK_LEFT, SDLK_RIGHT, SDLK_RIGHT};
  const Uint32 now = SDL_GetTicks();
  if (now - sequence_started > 5000) {
    enable_sequence_progress = 0;
    disable_sequence_progress = 0;
  }
  const SDL_Keycode key =
      raw_key == SDLK_w ? SDLK_UP :
      raw_key == SDLK_a ? SDLK_LEFT :
      raw_key == SDLK_s ? SDLK_DOWN :
      raw_key == SDLK_d ? SDLK_RIGHT : raw_key;
  if (key == enable_sequence[enable_sequence_progress]) {
    if (enable_sequence_progress == 0) sequence_started = now;
    ++enable_sequence_progress;
  } else {
    enable_sequence_progress = key == enable_sequence[0] ? 1 : 0;
    sequence_started = enable_sequence_progress ? now : 0;
  }
  if (key == disable_sequence[disable_sequence_progress]) {
    if (disable_sequence_progress == 0) sequence_started = now;
    ++disable_sequence_progress;
  } else {
    disable_sequence_progress = key == disable_sequence[0] ? 1 : 0;
  }
  if (enable_sequence_progress == enable_sequence.size()) {
    drilboor_active = true;
    big_forehead_active = true;
    ur_cooked_active = true;
    skywalker_active = true;
    nds_cooked_title = true;
    title_edition = 2;
    all_easter_eggs_enabled = true;
    easter_eggs_notice_kind = 1;
    easter_eggs_notice_until = now + 1800;
    enable_sequence_progress = 0;
    disable_sequence_progress = 0;
  } else if (disable_sequence_progress == disable_sequence.size()) {
    drilboor_active = false;
    big_forehead_active = false;
    ur_cooked_active = false;
    skywalker_active = false;
    nds_cooked_title = false;
    title_edition = 0;
    all_easter_eggs_enabled = false;
    easter_eggs_notice_kind = 2;
    easter_eggs_notice_until = now + 1800;
    enable_sequence_progress = 0;
    disable_sequence_progress = 0;
  }
}
unsigned audio_rate = 32768;
std::vector<int16_t> jackhammer_clip;
std::vector<size_t> jackhammer_voice_positions;
std::vector<int16_t> big_forehead_clip;
size_t big_forehead_position = 0;
std::vector<int16_t> skywalker_splash_clip;
std::vector<int16_t> skywalker_oooooh_clip;
struct SkywalkerVoice {
  double position = 0.0;
  float step = 1.0f;
  float gain = 1.8f;
  bool oooooh = false;
};
std::vector<SkywalkerVoice> skywalker_voices;
double forehead_mid_average = 0.0;
double forehead_side_average = 0.0;
GLuint jackhammer_texture = 0;
GLuint skywalker_texture = 0;
GLuint water_splash_texture = 0;
struct JackhammerFlight {
  double start_time = 0.0;
  float start_x = 0.0f;
  float start_y = 0.0f;
  float end_x = 0.0f;
  float end_y = 0.0f;
  size_t voice_index = SIZE_MAX;
};
std::vector<JackhammerFlight> jackhammer_flights;
double jackhammer_session_start = 0.0;
std::vector<double> jackhammer_spawn_times;
std::mt19937 jackhammer_rng{std::random_device{}()};
struct SkywalkerFlight {
  double start_time = 0.0;
  float start_x = 0.0f;
  float start_y = 0.0f;
  float size = 0.0f;
  float angle = 0.0f;
  double slide_duration = 1.5;
  double pause_duration = 2.0;
  double splash_duration = 3.1;
  double splash_hold_duration = 4.0;
  double splash_fade_duration = 1.25;
  float audio_speed = 1.0f;
  float audio_gain = 4.0f;
  bool center_sound_started = false;
  bool splash_started = false;
  std::array<float, 432> splash{};
};
std::vector<SkywalkerFlight> skywalker_flights;
std::array<int, 6> skywalker_speed_bag{0, 1, 2, 3, 4, 5};
size_t skywalker_speed_bag_index = skywalker_speed_bag.size();
bool skywalker_next_speed_slow_bias = false;
std::filesystem::path executable_directory();
std::filesystem::path runtime_directory() {
#ifndef _WIN32
  const char *home = std::getenv("HOME");
  if (home && *home)
    return std::filesystem::path(home) / ".cache" / "flowmulator" /
           "runtime";
#endif
  return executable_directory() / ".runtime";
}

void apply_hyprland_window_layout(int width, int height) {
#ifndef _WIN32
  if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return;
  const std::string size = std::to_string(width) + " " +
                           std::to_string(height);
  std::system("hyprctl dispatch setfloating active >/dev/null 2>&1");
  std::system(("hyprctl dispatch resizeactive exact " + size +
               " >/dev/null 2>&1").c_str());
  std::system("hyprctl dispatch centerwindow >/dev/null 2>&1");
  std::system(
      "hyprctl keyword windowrulev2 "
      "\"float,class:^(Flowmulator)$\" >/dev/null 2>&1");
  std::system(
      "hyprctl keyword windowrulev2 "
      "\"center,class:^(Flowmulator)$\" >/dev/null 2>&1");
  std::system(
      "hyprctl keyword windowrulev2 "
      "\"bordercolor rgb(f5a0c0),class:^(Flowmulator)$,title:^(Flowmulator - SAKURA)$\" "
      ">/dev/null 2>&1");
  std::system(
      "hyprctl keyword windowrulev2 "
      "\"bordercolor rgb(eaff00),class:^(Flowmulator)$,title:^(Flowmulator - RUNNERS)$\" "
      ">/dev/null 2>&1");
  std::system(
      "hyprctl keyword windowrulev2 "
      "\"bordercolor rgb(ffffff),class:^(Flowmulator)$,title:^(Flowmulator - BERSERK)$\" "
      ">/dev/null 2>&1");
#else
  (void)width;
  (void)height;
#endif
}

void apply_emulator_border_size(const char *window_class) {
#ifndef _WIN32
  if (!std::getenv("HYPRLAND_INSTANCE_SIGNATURE")) return;
  const int border_size = 2;
  const std::string command =
      "hyprctl eval 'o.window(\"" + std::string(window_class) +
      "\", { border_size = " + std::to_string(border_size) +
      " })' >/dev/null 2>&1";
  std::system(command.c_str());
#endif
}

std::pair<int, int> scaled_window_size(int reference_width,
                                       int reference_height) {
#ifndef _WIN32
  FILE *workspace = popen("hyprctl activeworkspace -j 2>/dev/null", "r");
  if (workspace) {
    std::string output;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), workspace))
      output += buffer;
    pclose(workspace);
    FILE *monitors = popen("hyprctl monitors -j 2>/dev/null", "r");
    std::string monitor_output;
    if (monitors) {
      char buffer[256];
      while (std::fgets(buffer, sizeof(buffer), monitors))
        monitor_output += buffer;
      pclose(monitors);
    }

    const size_t focused_position =
        monitor_output.find("\"focused\": true");
    const size_t monitor_start =
        focused_position == std::string::npos
            ? std::string::npos
            : monitor_output.rfind('{', focused_position);
    const size_t monitor_end =
        focused_position == std::string::npos
            ? std::string::npos
            : monitor_output.find('}', focused_position);
    if (monitor_start != std::string::npos &&
        monitor_end != std::string::npos &&
        monitor_output.substr(monitor_start, monitor_end - monitor_start)
                .find("\"scale\": 1.25") != std::string::npos) {
      if (reference_width == 730 && reference_height == 876)
        return {580, 696};
      if (reference_width == 1594 &&
          reference_height == 872)
        return {1274, 696};
    }
  }
#endif
  SDL_DisplayMode display_mode{};
  if (SDL_GetDesktopDisplayMode(0, &display_mode) != 0 ||
      display_mode.w <= 0 || display_mode.h <= 0) {
    return {reference_width, reference_height};
  }

  constexpr double reference_monitor_width = 1920.0;
  constexpr double reference_monitor_height = 1080.0;
  return {
      std::max(1, static_cast<int>(std::lround(
                      reference_width * display_mode.w /
                      reference_monitor_width))),
      std::max(1, static_cast<int>(std::lround(
                      reference_height * display_mode.h /
                      reference_monitor_height)))};
}

enum retro_log_level {
  RETRO_LOG_DEBUG,
  RETRO_LOG_INFO,
  RETRO_LOG_WARN,
  RETRO_LOG_ERROR
};

struct retro_log_callback {
  void (*log)(retro_log_level level, const char *format, ...);
};

void core_log(retro_log_level level, const char *format, ...) {
  const char *prefix = level == RETRO_LOG_ERROR ? "error" :
                       level == RETRO_LOG_WARN ? "warn" :
                       level == RETRO_LOG_DEBUG ? "debug" : "info";
  std::fprintf(stderr, "[core %s] ", prefix);
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
}

std::filesystem::path find_libretro_core(const char *name) {
#ifdef _WIN32
  const auto directory = executable_directory();
  for (const auto &candidate : {
           directory / "cores" / name, directory / name,
           directory / "libretro" / name}) {
    if (std::filesystem::exists(candidate)) return candidate;
  }
  return {};
#else
  const std::array<std::filesystem::path, 6> directories{
      runtime_directory() / "cores",
      runtime_directory(),
      "/usr/lib/libretro",
      "/usr/lib/x86_64-linux-gnu/libretro",
      "/usr/lib/aarch64-linux-gnu/libretro", "/usr/local/lib/libretro"};
  for (const auto &directory : directories) {
    const auto path = directory / name;
    if (std::filesystem::exists(path))
      return path;
  }
  return {};
#endif
}

bool install_dependencies_once() {
#ifdef _WIN32
  return true;
#else
  const char *home = std::getenv("HOME");
  if (!home) return false;
  const std::filesystem::path marker =
      std::filesystem::path(home) / ".config" / "flowmulator" /
      "dependencies-installed";
  auto cores_available = [] {
    const auto gba_path = find_libretro_core("mgba_libretro.so");
    const auto nds_path = find_libretro_core("desmume_libretro.so");
    void *gba_core = gba_path.empty()
        ? nullptr : open_dynamic_library(gba_path);
    void *nds_core = nds_path.empty()
        ? nullptr : open_dynamic_library(nds_path);
    close_dynamic_library(gba_core);
    close_dynamic_library(nds_core);
    return gba_core && nds_core;
  };
  if (std::filesystem::exists(marker) && cores_available()) return true;
  if (cores_available()) {
    std::error_code ready_error;
    std::filesystem::create_directories(marker.parent_path(), ready_error);
    if (!ready_error) {
      std::ofstream marker_file(marker);
      if (marker_file) return true;
    }

  }

  char temp_name[] = "/tmp/flowmulator-dependencies-XXXXXX";
  const int fd = mkstemp(temp_name);
  if (fd < 0) return false;
  const size_t script_size = static_cast<size_t>(
      _binary_Helper_sh_end -
      _binary_Helper_sh_start);
  size_t written = 0;
  while (written < script_size) {
    const ssize_t count = write(fd, _binary_Helper_sh_start + written,
                                script_size - written);
    if (count <= 0) {
      close(fd);
      unlink(temp_name);
      return false;
    }
    written += static_cast<size_t>(count);
  }
  fchmod(fd, 0700);
  close(fd);
  const std::string command =
      "FLOWMULATOR_AUTO_INSTALL=1 FLOWMULATOR_INSTALL_DIR=\"" +
      executable_directory().string() + "\" bash \"" +
      std::string(temp_name) + "\"";
  const int result = std::system(command.c_str());
  unlink(temp_name);
  if (result != 0) return false;
  std::error_code error;
  std::filesystem::create_directories(marker.parent_path(), error);
  if (error) _exit(1);
  std::ofstream marker_file(marker);
  return static_cast<bool>(marker_file);
#endif
}

bool extract_embedded_runtime() {
#ifndef _WIN32
  const auto runtime = runtime_directory();
  std::error_code error;
  std::filesystem::create_directories(runtime / "cores", error);
  if (error) return false;
  struct EmbeddedFile {
    const char *name;
    const uint8_t *start;
    const uint8_t *end;
    bool executable;
  };
  const EmbeddedFile files[] = {
      {"azahar.AppImage", _binary__runtime_azahar_AppImage_start,
       _binary__runtime_azahar_AppImage_end, true},
      {"cores/azahar_libretro.so", _binary__runtime_azahar_libretro_so_start,
       _binary__runtime_azahar_libretro_so_end, false},
      {"cores/mgba_libretro.so", _binary__runtime_mgba_libretro_so_start,
       _binary__runtime_mgba_libretro_so_end, false},
      {"cores/desmume_libretro.so",
       _binary__runtime_desmume_libretro_so_start,
       _binary__runtime_desmume_libretro_so_end, false},
      {"MiiFix.3dsx", _binary__runtime_MiiFix_3dsx_start,
       _binary__runtime_MiiFix_3dsx_end, false}};
  for (const auto &file : files) {
    const auto path = runtime / file.name;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char *>(file.start),
                 static_cast<std::streamsize>(
                     embedded_size(file.start, file.end)));
    if (!output) return false;
    if (file.executable) {
      std::filesystem::permissions(
          path, std::filesystem::perms::owner_exec |
                    std::filesystem::perms::owner_read |
                    std::filesystem::perms::owner_write,
          std::filesystem::perm_options::replace, error);
      if (error) return false;
    }
  }
  return true;
#else
  return true;
#endif
}
std::array<SDL_Keycode, 10> control_keys{
    SDLK_z, SDLK_x, SDLK_BACKSPACE, SDLK_RETURN, SDLK_UP,
    SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_s, SDLK_a};
SDL_Keycode turbo_key = SDLK_TAB;
bool turbo_toggle = false;
bool turbo_hold = false;
bool show_fps = false;
bool vsync_enabled = true;
const std::array<const char *, 10> control_names{
    "A BUTTON", "B BUTTON", "SELECT", "START", "UP",
    "DOWN", "LEFT", "RIGHT", "R BUTTON", "L BUTTON"};
std::array<SDL_Keycode, 12> nds_control_keys{
    SDLK_z, SDLK_x, SDLK_a, SDLK_s, SDLK_BACKSPACE, SDLK_RETURN,
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_q, SDLK_w};
const std::array<const char *, 12> nds_control_names{
    "A BUTTON", "B BUTTON", "X BUTTON", "Y BUTTON", "SELECT", "START",
    "UP", "DOWN", "LEFT", "RIGHT", "L BUTTON", "R BUTTON"};
std::array<SDL_Keycode, 12> wii_control_keys{
    SDLK_z, SDLK_x, SDLK_a, SDLK_s, SDLK_BACKSPACE, SDLK_RETURN,
    SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_q, SDLK_w};
const std::array<const char *, 12> wii_control_names{
    "A BUTTON", "B BUTTON", "1 BUTTON", "2 BUTTON", "MINUS", "PLUS",
    "UP", "DOWN", "LEFT", "RIGHT", "HOME", "C BUTTON"};
constexpr unsigned wii_visible_control_count = 11;
std::array<SDL_Keycode, 4> wii_tilt_keys{
    SDLK_i, SDLK_k, SDLK_j, SDLK_l};
const std::array<const char *, 4> wii_tilt_names{
    "TILT UP", "TILT DOWN", "TILT LEFT", "TILT RIGHT"};
SDL_Keycode wii_shake_key = SDLK_SPACE;
bool wii_nunchuk_enabled = false;
std::array<SDL_Keycode, 6> wii_nunchuk_keys{
    SDLK_q, SDLK_e, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT};
const std::array<const char *, 6> wii_nunchuk_names{
    "NUNCHUK C", "NUNCHUK Z", "NUNCHUK UP", "NUNCHUK DOWN",
    "NUNCHUK LEFT", "NUNCHUK RIGHT"};
std::array<SDL_GameControllerButton, 6> wii_nunchuk_controller_buttons{
    SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
    SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
std::array<SDL_GameControllerAxis, 6> wii_nunchuk_controller_axes{};
std::array<bool, 6> wii_nunchuk_controller_axis_positive{};
std::array<bool, 6> wii_nunchuk_controller_axis_bound{};
bool nds_active = false;
bool three_ds_active = false;
bool wii_active = false;
bool gamecube_active = false;
bool wii_vulkan = false;
unsigned wii_control_port = 1;
unsigned wii_active_port_count = 1;
bool wii_sideways = true;
int wii_resolution = 1;
int three_ds_resolution = 1;
constexpr unsigned turbo_hold_index = 10;
constexpr unsigned turbo_toggle_index = 11;

class VulkanHost {
 public:
  bool initialize(SDL_Window *window);
  bool ready() const { return instance_ && device_ && swapchain_; }
  void shutdown() { destroy(); }
  bool activate();
  void present();
  void set_frame_size(unsigned width, unsigned height) {
    frame_width_ = width;
    frame_height_ = height;
  }

  bool set_hw_render(retro_hw_render_callback *callback);
  bool set_negotiation(
      retro_hw_render_context_negotiation_interface_vulkan *interface);
  const retro_hw_render_interface_vulkan *render_interface() const {
    return &render_interface_;
  }

 private:
  static VulkanHost *active_;
  static const VkApplicationInfo *get_application_info();
  static bool create_device(
      retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
      VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
      const char **extensions, unsigned extension_count, const char **layers,
      unsigned layer_count, const VkPhysicalDeviceFeatures *features);
  static void context_reset() {}
  static void context_destroy() {}
  static void set_image(void *handle, const retro_vulkan_image *image,
                        uint32_t semaphore_count, const VkSemaphore *semaphores,
                        uint32_t source_queue_family);
  static uint32_t get_sync_index(void *handle);
  static uint32_t get_sync_index_mask(void *handle);
  static void wait_sync_index(void *handle);
  static void lock_queue(void *handle);
  static void unlock_queue(void *handle);
  static void set_command_buffers(void *, uint32_t, const VkCommandBuffer *) {}
  static void set_signal_semaphore(void *, VkSemaphore) {}

  template <typename T>
  T instance_function(const char *name) const {
    return reinterpret_cast<T>(get_instance_proc_addr_(instance_, name));
  }
  template <typename T>
  T device_function(const char *name) const {
    return reinterpret_cast<T>(get_device_proc_addr_(device_, name));
  }
  bool create_instance(SDL_Window *window);
  bool create_swapchain();
  bool negotiate_device(retro_vulkan_context *context, VkInstance instance,
                        VkPhysicalDevice gpu, VkSurfaceKHR surface,
                        PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                        const char **extensions, unsigned extension_count,
                        const char **layers, unsigned layer_count,
                        const VkPhysicalDeviceFeatures *features);
  void destroy();
  void copy_latest_image(uint32_t target_index);

  SDL_Window *window_ = nullptr;
  PFN_vkGetInstanceProcAddr get_instance_proc_addr_ = nullptr;
  PFN_vkGetDeviceProcAddr get_device_proc_addr_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkImage> swapchain_images_;
  std::vector<VkImageLayout> swapchain_layouts_;
  VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
  uint32_t swapchain_width_ = 0;
  uint32_t swapchain_height_ = 0;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer_ = VK_NULL_HANDLE;
  VkSemaphore acquire_semaphore_ = VK_NULL_HANDLE;
  VkSemaphore render_semaphore_ = VK_NULL_HANDLE;
  VkFence render_fence_ = VK_NULL_HANDLE;
  uint32_t frame_index_ = 0;
  unsigned frame_width_ = 640;
  unsigned frame_height_ = 480;
  retro_vulkan_image latest_image_{};
  bool have_image_ = false;
  std::mutex image_mutex_;
  std::mutex queue_mutex_;
  retro_hw_render_interface_vulkan render_interface_{};
  const retro_hw_render_context_negotiation_interface_vulkan *negotiation_ =
      nullptr;
  retro_hw_context_reset_t context_reset_callback_ = nullptr;
};

VulkanHost *VulkanHost::active_ = nullptr;
VulkanHost *active_vulkan_host = nullptr;

std::filesystem::path controls_path() {
#ifdef _WIN32
  const char *app_data = std::getenv("APPDATA");
  if (!app_data) app_data = std::getenv("USERPROFILE");
  if (!app_data) return {};
  return std::filesystem::path(app_data) / "Flowmulator" / "controls.cfg";
#else
  const char *home = std::getenv("HOME");
  if (!home) return {};
  return std::filesystem::path(home) / ".config" / "flowmulator" / "controls.cfg";
#endif
}

std::filesystem::path nds_controls_path() {
  const auto path = controls_path();
  return path.empty() ? std::filesystem::path{} :
                        path.parent_path() / "nds-controls.cfg";
}

std::filesystem::path three_ds_resolution_path() {
  const auto path = controls_path();
  return path.empty() ? std::filesystem::path{} :
                        path.parent_path() / "3ds-resolution.cfg";
}

void load_three_ds_resolution() {
  const auto path = three_ds_resolution_path();
  std::ifstream input(path);
  int value = 0;
  if (input >> value && value >= 1 && value <= 4)
    three_ds_resolution = value;
}

void save_three_ds_resolution() {
  const auto path = three_ds_resolution_path();
  if (path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return;
  std::ofstream output(path, std::ios::trunc);
  if (output) output << three_ds_resolution << '\n';
}

bool sync_azahar_controls(const std::filesystem::path &config_root) {
#ifndef _WIN32
  const char *home = std::getenv("HOME");
  if (!home) return false;
  const auto source_path =
      std::filesystem::path(home) / ".config" / "azahar-emu" / "qt-config.ini";
  const auto config_path = config_root / "azahar-emu" / "qt-config.ini";
  std::error_code error;
  std::filesystem::create_directories(config_path.parent_path(), error);
  if (error) return false;
  if (std::filesystem::exists(source_path)) {
    std::filesystem::copy_file(
        source_path, config_path,
        std::filesystem::copy_options::overwrite_existing, error);
    if (error) return false;
  }
  std::ifstream input(config_path);
  std::string config((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
  if (config.empty())
    config = "[Renderer]\n";
  static constexpr std::array<const char *, 12> names{
      "button_a", "button_b", "button_x", "button_y", "button_select",
      "button_start", "button_up", "button_down", "button_left",
      "button_right", "button_l", "button_r"};
  const auto qt_key_code = [](SDL_Keycode key) {
    switch (key) {
    case SDLK_BACKSPACE: return 0x01000003;
    case SDLK_TAB: return 0x01000001;
    case SDLK_RETURN: return 0x01000004;
    case SDLK_ESCAPE: return 0x01000000;
    case SDLK_SPACE: return 0x20;
    case SDLK_UP: return 0x01000013;
    case SDLK_DOWN: return 0x01000015;
    case SDLK_LEFT: return 0x01000012;
    case SDLK_RIGHT: return 0x01000014;
    default:
      return key >= SDLK_a && key <= SDLK_z
          ? static_cast<int>(key - SDLK_a + 'A')
          : static_cast<int>(key);
    }
  };
  const auto replace_setting = [&config](const std::string &prefix,
                                          const std::string &value) {
    size_t line_start = config.find(prefix);
    while (line_start != std::string::npos && line_start != 0 &&
           config[line_start - 1] != '\n') {
      line_start = config.find(prefix, line_start + 1);
    }
    if (line_start == std::string::npos) return;
    const size_t value_start = line_start + prefix.size();
    const size_t value_end = config.find('\n', value_start);
    config.replace(value_start, value_end == std::string::npos
                                      ? std::string::npos
                                      : value_end - value_start,
                   value);
  };
  const bool controller_mode = nds_input_mode == InputMode::Controller;
  for (size_t i = 0; i < names.size(); ++i) {
    const std::string prefix =
        "profiles\\1\\" + std::string(names[i]) + "=";
    const std::string default_prefix =
        "profiles\\1\\" + std::string(names[i]) + "\\default=";
    if (controller_mode) {
      replace_setting(prefix, "\"code:" +
                                  std::to_string(nds_controller_buttons[i]) +
                                  ",engine:gamepad\"");
    } else {
      replace_setting(prefix, "\"code:" +
                                  std::to_string(qt_key_code(nds_control_keys[i])) +
                                  ",engine:keyboard\"");
    }
    replace_setting(default_prefix, "false");
  }
  replace_setting("profiles\\1\\name\\default=", "false");
  replace_setting("profile\\default=", "false");
  const std::string resolution = std::to_string(three_ds_resolution);
  if (config.find("resolution_factor=") == std::string::npos) {
    if (config.back() != '\n') config += '\n';
    config += "resolution_factor=" + resolution + "\n";
  } else {
    replace_setting("resolution_factor=", resolution);
  }
  if (config.find("resolution_factor\\default=") == std::string::npos) {
    config += "resolution_factor\\default=false\n";
  } else {
    replace_setting("resolution_factor\\default=", "false");
  }
  for (const char *setting : {"showStatusBar", "showFilterBar"}) {
    const std::string prefix = std::string(setting) + "=";
    const size_t line_start = config.find(prefix);
    if (line_start == std::string::npos) continue;
    const size_t value_start = line_start + prefix.size();
    const size_t value_end = config.find('\n', value_start);
    config.replace(value_start, value_end == std::string::npos
                                      ? std::string::npos
                                      : value_end - value_start,
                   "false");
  }
  std::ofstream output(config_path, std::ios::trunc);
  if (!output) return false;
  output << config;
  if (!output) return false;

  const auto custom_directory = config_path.parent_path() / "custom";
  if (std::filesystem::exists(custom_directory)) {
    for (const auto &entry :
         std::filesystem::directory_iterator(custom_directory)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".ini")
        continue;
      std::ifstream custom_input(entry.path());
      if (!custom_input) return false;
      std::string custom_config((std::istreambuf_iterator<char>(custom_input)),
                                std::istreambuf_iterator<char>());
      const auto replace_custom_setting =
          [&custom_config](const std::string &prefix,
                           const std::string &value) {
            size_t line_start = custom_config.find(prefix);
            while (line_start != std::string::npos && line_start != 0 &&
                   custom_config[line_start - 1] != '\n') {
              line_start = custom_config.find(prefix, line_start + 1);
            }
            if (line_start == std::string::npos) return false;
            const size_t value_start = line_start + prefix.size();
            const size_t value_end = custom_config.find('\n', value_start);
            custom_config.replace(
                value_start,
                value_end == std::string::npos
                    ? std::string::npos
                    : value_end - value_start,
                value);
            return true;
          };
      if (!replace_custom_setting("resolution_factor=",
                                  std::to_string(three_ds_resolution))) {
        const std::string renderer_header = "[Renderer]\n";
        const size_t renderer_start = custom_config.find(renderer_header);
        if (renderer_start != std::string::npos) {
          custom_config.insert(renderer_start + renderer_header.size(),
                               "resolution_factor=" +
                                   std::to_string(three_ds_resolution) + "\n");
        }
      }
      if (!replace_custom_setting("resolution_factor\\use_global=", "false")) {
        const std::string renderer_header = "[Renderer]\n";
        const size_t renderer_start = custom_config.find(renderer_header);
        if (renderer_start != std::string::npos) {
          custom_config.insert(renderer_start + renderer_header.size(),
                               "resolution_factor\\use_global=false\n");
        }
      }
      std::ofstream custom_output(entry.path(), std::ios::trunc);
      if (!custom_output) return false;
      custom_output << custom_config;
      if (!custom_output) return false;
    }
  }
  return true;
#else
  (void)config_root;
  return false;
#endif
}

bool sync_azahar_user_data(const std::filesystem::path &config_root) {
#ifndef _WIN32
  const auto source =
      executable_directory() / "SAVES" / "Azahar";
  const auto destination = config_root / "azahar-emu";
  if (!std::filesystem::exists(source)) return true;
  std::error_code error;
  std::filesystem::create_directories(destination, error);
  if (error) return false;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(source)) {
    const auto relative = std::filesystem::relative(entry.path(), source, error);
    if (error) return false;
    const auto target = destination / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directories(target, error);
      if (error) return false;
    } else if (entry.is_regular_file()) {
      std::filesystem::create_directories(target.parent_path(), error);
      if (error) return false;
      std::filesystem::copy_file(
          entry.path(), target,
          std::filesystem::copy_options::overwrite_existing, error);
      if (error) return false;
    }
  }
  return true;
#else
  (void)config_root;
  return false;
#endif
}

bool launch_mii_fix(SDL_Renderer *renderer) {
#ifndef _WIN32
  (void)renderer;
  const auto runtime = runtime_directory();
  const auto fix_path = runtime / "MiiFix.3dsx";
  const auto config_root = runtime / "azahar-config";
  if (!std::filesystem::exists(fix_path) ||
      !std::filesystem::exists(runtime / "azahar.AppImage") ||
      !sync_azahar_user_data(config_root) ||
      !sync_azahar_controls(config_root)) {
    return false;
  }
  std::error_code error;
  const auto azahar = runtime / "azahar.AppImage";
  const pid_t worker_pid = fork();
  if (worker_pid < 0) return false;
  if (worker_pid > 0) return true;
  const pid_t pid = fork();
  if (pid < 0) _exit(1);
  if (pid == 0) {
    setpgid(0, 0);
    setenv("XDG_CONFIG_HOME", config_root.c_str(), 1);
    setenv("XDG_DATA_HOME", config_root.c_str(), 1);
    execl(azahar.c_str(), azahar.filename().c_str(),
          "--appimage-extract-and-run", "-w", fix_path.c_str(),
          static_cast<char *>(nullptr));
    _exit(127);
  }
  int status = 0;
  const auto wait_started = std::chrono::steady_clock::now();
  for (;;) {
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) break;
    if (result < 0 && errno != EINTR) break;
    const auto elapsed = std::chrono::steady_clock::now() - wait_started;
    if (elapsed >= std::chrono::minutes(5)) {
      kill(-pid, SIGTERM);
      waitpid(pid, &status, 0);
      break;
    }
    usleep(100000);
  }
  const auto source = config_root / "azahar-emu";
  const auto destination = executable_directory() / "SAVES" / "Azahar";
  std::filesystem::create_directories(destination, error);
  if (error) return false;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(source)) {
    const auto relative = std::filesystem::relative(entry.path(), source, error);
    if (error) return false;
    const auto target = destination / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directories(target, error);
    } else if (entry.is_regular_file()) {
      std::filesystem::create_directories(target.parent_path(), error);
      if (!error)
        std::filesystem::copy_file(
            entry.path(), target,
            std::filesystem::copy_options::overwrite_existing, error);
    }
    if (error) _exit(1);
  }
  _exit(WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : 1);
#else
  (void)renderer;
  return false;
#endif
}

std::filesystem::path wii_controls_path() {
  const auto path = controls_path();
  return path.empty() ? std::filesystem::path{} :
                        path.parent_path() / "wii-controls.cfg";
}

std::filesystem::path wii_port_controls_path(unsigned port) {
  const auto path = wii_controls_path();
  if (path.empty() || port <= 1) return path;
  return path.parent_path() / ("wii-controls-port" + std::to_string(port) +
                               ".cfg");
}

std::filesystem::path wii_port_count_path() {
  const auto path = wii_controls_path();
  return path.empty() ? std::filesystem::path{} :
                        path.parent_path() / "wii-port-count.cfg";
}

std::filesystem::path dolphin_config_directory() {
#ifdef _WIN32
  const char *app_data = std::getenv("APPDATA");
  if (!app_data) app_data = std::getenv("USERPROFILE");
  return app_data ? std::filesystem::path(app_data) / "Dolphin Emulator"
                  : std::filesystem::path{};
#else
  const char *home = std::getenv("HOME");
  return home ? std::filesystem::path(home) / ".config" / "dolphin-emu"
              : std::filesystem::path{};
#endif
}

enum class ProfileKind { Gba, Nds, Wii };

std::filesystem::path profile_config_path(ProfileKind kind) {
  const auto path = controls_path();
  if (path.empty()) return {};
  return kind == ProfileKind::Gba ? path :
         kind == ProfileKind::Nds ? nds_controls_path() : wii_controls_path();
}

std::filesystem::path profile_directory(ProfileKind kind) {
  const auto path = profile_config_path(kind);
  return path.empty() ? std::filesystem::path{} :
      path.parent_path() / (kind == ProfileKind::Gba ? "profiles-gba" :
                            kind == ProfileKind::Nds ? "profiles-nds" :
                                                       "profiles-wii");
}

std::filesystem::path profile_current_path(ProfileKind kind) {
  const auto path = profile_directory(kind);
  return path.empty() ? std::filesystem::path{} : path / "current";
}

std::vector<std::filesystem::path> profile_files(ProfileKind kind) {
  std::vector<std::filesystem::path> files;
  const auto directory = profile_directory(kind);
  std::error_code error;
  if (!std::filesystem::exists(directory, error)) return files;
  for (const auto &entry : std::filesystem::directory_iterator(directory, error))
    if (entry.is_regular_file() && entry.path().filename() != "current")
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  return files;
}

std::string profile_name(const std::filesystem::path &path) {
  return path.stem().string();
}

std::string active_profile_name(ProfileKind kind) {
  std::ifstream input(profile_current_path(kind));
  std::string name;
  std::getline(input, name);
  return name.empty() ? "DEFAULT" : name;
}

bool copy_profile_file(ProfileKind kind, const std::filesystem::path &source,
                       const std::filesystem::path &target) {
  const auto directory = profile_directory(kind);
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  std::filesystem::copy_file(source, target,
                              std::filesystem::copy_options::overwrite_existing,
                              error);
  return !error;
}

bool save_profile(ProfileKind kind) {
  const auto source = profile_config_path(kind);
  const auto current = profile_current_path(kind);
  if (source.empty() || !std::filesystem::exists(source)) return false;
  std::ifstream name_input(current);
  std::string name;
  std::getline(name_input, name);
  if (name.empty()) name = "DEFAULT";
  return copy_profile_file(kind, source,
                           profile_directory(kind) / (name + ".cfg"));
}

bool load_profile(ProfileKind kind, size_t index) {
  const auto files = profile_files(kind);
  if (index >= files.size()) return false;
  const auto source = files[index];
  const auto target = profile_config_path(kind);
  if (!copy_profile_file(kind, source, target)) return false;
  std::ofstream current(profile_current_path(kind), std::ios::trunc);
  if (!current) return false;
  current << profile_name(source) << '\n';
  return true;
}

bool create_profile(ProfileKind kind) {
  const auto source = profile_config_path(kind);
  if (source.empty() || !std::filesystem::exists(source)) return false;
  std::string name;
  for (unsigned number = 1;; ++number) {
    name = number == 1 ? "DEFAULT" : "PROFILE " + std::to_string(number);
    if (!std::filesystem::exists(profile_directory(kind) / (name + ".cfg")))
      break;
  }
  std::ofstream current(profile_current_path(kind), std::ios::trunc);
  if (!current) {
    std::error_code error;
    std::filesystem::create_directories(profile_directory(kind), error);
    current.open(profile_current_path(kind), std::ios::trunc);
  }
  if (!current) return false;
  current << name << '\n';
  return copy_profile_file(kind, source,
                           profile_directory(kind) / (name + ".cfg"));
}

bool delete_profile(ProfileKind kind) {
  const auto files = profile_files(kind);
  if (files.size() <= 1) return false;
  const std::string active = active_profile_name(kind);
  const auto it = std::find_if(files.begin(), files.end(),
      [&](const auto &file) { return profile_name(file) == active; });
  if (it == files.end()) return false;
  std::error_code error;
  std::filesystem::remove(*it, error);
  if (error) return false;
  const auto remaining = profile_files(kind);
  return load_profile(kind, 0);
}

bool rename_profile(ProfileKind kind, const std::string &new_name) {
  if (new_name.empty() || new_name.size() > 24 ||
      new_name == "." || new_name == "..")
    return false;
  for (const unsigned char character : new_name)
    if (!(std::isalnum(character) || character == ' ' ||
          character == '_' || character == '-'))
      return false;
  const std::string active = active_profile_name(kind);
  const auto source = profile_directory(kind) / (active + ".cfg");
  const auto target = profile_directory(kind) / (new_name + ".cfg");
  if (source == target || std::filesystem::exists(target))
    return false;
  std::error_code error;
  std::filesystem::rename(source, target, error);
  if (error) return false;
  std::ofstream current(profile_current_path(kind), std::ios::trunc);
  if (!current) {
    std::filesystem::rename(target, source, error);
    return false;
  }
  current << new_name << '\n';
  return static_cast<bool>(current);
}

void append_profile_text(std::string &profile_edit, const char *text) {
  for (const unsigned char character :
       std::string(text ? text : "")) {
    if (profile_edit.size() >= 24) break;
    if (std::isalnum(character) || character == ' ' ||
        character == '_' || character == '-') {
      profile_edit += static_cast<char>(std::toupper(character));
    }
  }
}

template <size_t N>
void load_controller_buttons(std::istream &input,
                             std::array<SDL_GameControllerButton, N> &buttons) {
  for (auto &button : buttons) {
    int value = 0;
    if (!(input >> value)) return;
    if (value >= SDL_CONTROLLER_BUTTON_A &&
        value <= SDL_CONTROLLER_BUTTON_DPAD_RIGHT)
      button = static_cast<SDL_GameControllerButton>(value);
  }
}

template <size_t N>
void save_controller_buttons(std::ostream &output,
                             const std::array<SDL_GameControllerButton, N> &buttons) {
  for (SDL_GameControllerButton button : buttons)
    output << static_cast<int>(button) << '\n';
}

template <size_t N>
void load_controller_axes(std::istream &input,
                          std::array<SDL_GameControllerAxis, N> &axes,
                          std::array<bool, N> &positive,
                          std::array<bool, N> &bound) {
  for (size_t i = 0; i < N; ++i) {
    int axis = 0;
    int direction = 0;
    int is_bound = 0;
    if (!(input >> axis >> direction >> is_bound)) return;
    if (axis >= SDL_CONTROLLER_AXIS_LEFTX &&
        axis <= SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
      axes[i] = static_cast<SDL_GameControllerAxis>(axis);
      positive[i] = direction != 0;
      bound[i] = is_bound != 0;
    }
  }
}

template <size_t N>
void save_controller_axes(
    std::ostream &output, const std::array<SDL_GameControllerAxis, N> &axes,
    const std::array<bool, N> &positive, const std::array<bool, N> &bound) {
  for (size_t i = 0; i < N; ++i)
    output << static_cast<int>(axes[i]) << ' ' << (positive[i] ? 1 : 0)
           << ' ' << (bound[i] ? 1 : 0) << '\n';
}

void load_controls() {
  const auto path = controls_path();
  if (path.empty()) return;
  std::ifstream input(path);
  if (!input) return;
  for (auto &key : control_keys) {
    int value = 0;
    if (!(input >> value)) return;
    key = static_cast<SDL_Keycode>(value);
  }
  int turbo = 0;
  if (input >> turbo) turbo_key = static_cast<SDL_Keycode>(turbo);
  int crt = 0;
  if (input >> crt) crt_filter = crt != 0;
  int overlay = 0;
  if (input >> overlay) gba_overlay = overlay != 0;
  int theme = 0;
  if (input >> theme)
    active_theme = theme == 1 ? Theme::Edgerunners :
                   theme == 2 ? Theme::Berserk : Theme::Sakura;
  int input_mode = 0;
  if (input >> input_mode)
    gba_input_mode = input_mode == 1 ? InputMode::Controller
                                     : InputMode::Keyboard;
  load_controller_buttons(input, gba_controller_buttons);
  load_controller_axes(input, gba_controller_axes, gba_controller_axis_positive,
                       gba_controller_axis_bound);
}

void save_controls() {
  const auto path = controls_path();
  if (path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return;
  std::ofstream output(path, std::ios::trunc);
  if (!output) return;
  for (SDL_Keycode key : control_keys) output << static_cast<int>(key) << '\n';
  output << static_cast<int>(turbo_key) << '\n';
  output << (crt_filter ? 1 : 0) << '\n';
  output << (gba_overlay ? 1 : 0) << '\n';
  output << (active_theme == Theme::Edgerunners ? 1 :
             active_theme == Theme::Berserk ? 2 : 0) << '\n';
  output << (gba_input_mode == InputMode::Controller ? 1 : 0) << '\n';
  save_controller_buttons(output, gba_controller_buttons);
  save_controller_axes(output, gba_controller_axes, gba_controller_axis_positive,
                        gba_controller_axis_bound);
}

bool edgerunners_theme() {
  return active_theme == Theme::Edgerunners;
}

bool berserk_theme() {
  return active_theme == Theme::Berserk;
}

void draw_text(SDL_Renderer *renderer, const std::string &text, int x, int y,
               int scale, SDL_Color color);

SDL_Color theme_text(SDL_Color color) {
  if (!edgerunners_theme() && !berserk_theme()) return color;
  if (berserk_theme()) {
    if (color.r == 255 && color.g == 225 && color.b == 235)
      return {6, 24, 63, color.a};
    if (color.r == 255 && color.g == 232 && color.b == 241)
      return {6, 24, 63, color.a};
    if (color.r >= 245 && color.g >= 220 && color.b >= 220)
      return {255, 255, 255, color.a};
    if (color.r == 117 && color.g == 48 && color.b == 87)
      return {255, 255, 255, color.a};
    if (color.r >= 180 && color.g >= 100 && color.b >= 120)
      return {255, 255, 255, color.a};
    return color;
  }
  if (color.r == 255 && color.g == 225 && color.b == 235)
    return {24, 25, 35, color.a};
  if (color.r == 255 && color.g == 232 && color.b == 241)
    return {24, 25, 35, color.a};
  if (color.r == 245 && color.g == 205 && color.b == 217)
    return {24, 25, 35, color.a};
  if (color.r >= 245 && color.g >= 220 && color.b >= 220)
    return {238, 255, 0, color.a};
  if (color.r >= 180 && color.g >= 100 && color.b >= 120)
    return {221, 210, 166, color.a};
  return color;
}

void toggle_theme() {
  active_theme = active_theme == Theme::Sakura ? Theme::Edgerunners :
                 active_theme == Theme::Edgerunners ? Theme::Berserk :
                 Theme::Sakura;
  save_controls();
  if (theme_window) SDL_SetWindowTitle(theme_window, theme_window_title());
}

void set_theme_draw_color(SDL_Renderer *renderer, uint8_t red, uint8_t green,
                          uint8_t blue, uint8_t alpha) {
  if (edgerunners_theme()) {
    if (red == 35 && green == 18 && blue == 40)
      red = green = blue = 0;
    else if (red == 112 && green == 42 && blue == 72)
      red = 234, green = 255, blue = 0;
    else if (red == 67 && green == 30 && blue == 48)
      red = 45, green = 47, blue = 69;
    else if (red == 117 && green == 48 && blue == 87)
      red = 234, green = 255, blue = 0;
    else if (red == 67 && green == 30 && blue == 48)
      red = 45, green = 47, blue = 69;
    else if (red == 255 && green == 232 && blue == 241)
      red = 24, green = 25, blue = 35;
    else if (red == 160 && green == 48 && blue == 120)
      red = 100, green = 100, blue = 80;
  } else if (berserk_theme()) {
    if (red == 35 && green == 18 && blue == 40)
      if (dark_page_rendering)
        red = 6, green = 24, blue = 63;
      else
        red = 165, green = 22, blue = 69;
    else if (red == 112 && green == 42 && blue == 72)
      red = 255, green = 255, blue = 255;
    else if (red == 67 && green == 30 && blue == 48)
      red = 0, green = 0, blue = 0;
    else if (red == 117 && green == 48 && blue == 87)
      if (dark_page_rendering)
        red = green = blue = 255;
      else
        red = 6, green = 24, blue = 63;
    else if (red == 255 && green == 232 && blue == 241)
      red = 6, green = 24, blue = 63;
    else if (red == 160 && green == 48 && blue == 120)
      red = 135, green = 18, blue = 52;
  }
  SDL_SetRenderDrawColor(renderer, red, green, blue, alpha);
}

#define SDL_SetRenderDrawColor(renderer, red, green, blue, alpha) \
  set_theme_draw_color(renderer, red, green, blue, alpha)

void draw_theme_footer(SDL_Renderer *renderer, int y = FRONTEND_HEIGHT - 35) {
  draw_text(renderer, std::string("THEME: ") +
              (edgerunners_theme() ? "RUNNERS" :
               berserk_theme() ? "BERSERK" : "SAKURA"),
            570, y, 2, theme_text({190, 116, 148, 255}));
}

SDL_Texture *load_theme_background(SDL_Renderer *renderer) {
  const uint8_t *start = edgerunners_theme()
      ? _binary__assets_edgerunners_background_bmp_start
      : berserk_theme() ? _binary__assets_berserk_background_bmp_start
      : _binary__assets_frontend_background_bmp_start;
  const uint8_t *end = edgerunners_theme()
      ? _binary__assets_edgerunners_background_bmp_end
      : berserk_theme() ? _binary__assets_berserk_background_bmp_end
      : _binary__assets_frontend_background_bmp_end;
  SDL_RWops *rw = SDL_RWFromConstMem(
      start, static_cast<int>(embedded_size(start, end)));
  SDL_Surface *surface = rw ? SDL_LoadBMP_RW(rw, SDL_TRUE) : nullptr;
  SDL_Texture *texture = surface
      ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr;
  if (surface) SDL_FreeSurface(surface);
  return texture;
}

void load_nds_controls() {
  const auto path = nds_controls_path();
  if (path.empty()) return;
  std::ifstream input(path);
  if (!input) return;
  for (auto &key : nds_control_keys) {
    int value = 0;
    if (!(input >> value)) return;
    key = static_cast<SDL_Keycode>(value);
  }
  int input_mode = 0;
  if (input >> input_mode)
    nds_input_mode = input_mode == 1 ? InputMode::Controller
                                     : InputMode::Keyboard;
  int overlay = 0;
  if (input >> overlay) nds_overlay = overlay != 0;
  load_controller_buttons(input, nds_controller_buttons);
  load_controller_axes(input, nds_controller_axes, nds_controller_axis_positive,
                       nds_controller_axis_bound);
}

void save_nds_controls() {
  const auto path = nds_controls_path();
  if (path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return;
  std::ofstream output(path, std::ios::trunc);
  if (!output) return;
  for (SDL_Keycode key : nds_control_keys)
    output << static_cast<int>(key) << '\n';
  output << (nds_input_mode == InputMode::Controller ? 1 : 0) << '\n';
  output << (nds_overlay ? 1 : 0) << '\n';
  save_controller_buttons(output, nds_controller_buttons);
  save_controller_axes(output, nds_controller_axes, nds_controller_axis_positive,
                        nds_controller_axis_bound);
}

void load_wii_controls(unsigned port = wii_control_port) {
  const auto path = wii_port_controls_path(port);
  if (path.empty()) return;
  if (port > 1 && !std::filesystem::exists(path)) {
    std::error_code copy_error;
    std::filesystem::copy_file(
        wii_port_controls_path(1), path,
        std::filesystem::copy_options::overwrite_existing, copy_error);
  }
  std::ifstream input(path);
  if (!input) return;
  for (auto &key : wii_control_keys) {
    int value = 0;
    if (!(input >> value)) return;
    key = static_cast<SDL_Keycode>(value);
  }
  int sideways = 1;
  if (input >> sideways) wii_sideways = sideways != 0;
  int resolution = 1;
  if (input >> resolution && resolution >= 1 && resolution <= 4)
    wii_resolution = resolution;
  for (auto &key : wii_tilt_keys) {
    int value = 0;
    if (!(input >> value)) break;
    key = static_cast<SDL_Keycode>(value);
  }
  int shake = 0;
  if (input >> shake) wii_shake_key = static_cast<SDL_Keycode>(shake);
  int input_mode = 0;
  if (input >> input_mode)
    wii_input_mode = input_mode == 2 ? InputMode::RealWiimote :
                     input_mode == 1 ? InputMode::Controller
                                     : InputMode::Keyboard;
  load_controller_buttons(input, wii_controller_buttons);
  load_controller_axes(input, wii_controller_axes, wii_controller_axis_positive,
                       wii_controller_axis_bound);
  load_controller_buttons(input, wii_tilt_controller_buttons);
  load_controller_axes(input, wii_tilt_controller_axes,
                       wii_tilt_controller_axis_positive,
                       wii_tilt_controller_axis_bound);
  int shake_button = 0;
  if (input >> shake_button &&
      shake_button >= SDL_CONTROLLER_BUTTON_A &&
      shake_button < SDL_CONTROLLER_BUTTON_MAX)
    wii_shake_controller_button =
        static_cast<SDL_GameControllerButton>(shake_button);
  int shake_axis = 0;
  int shake_direction = 0;
  if (input >> shake_axis >> shake_direction &&
      shake_axis >= SDL_CONTROLLER_AXIS_LEFTX &&
      shake_axis <= SDL_CONTROLLER_AXIS_TRIGGERRIGHT) {
    wii_shake_controller_axis =
        static_cast<SDL_GameControllerAxis>(shake_axis);
    wii_shake_controller_axis_positive = shake_direction != 0;
    wii_shake_controller_axis_bound = true;
  }
  int nunchuk_enabled = 0;
  if (input >> nunchuk_enabled)
    wii_nunchuk_enabled = nunchuk_enabled != 0;
  load_controller_buttons(input, wii_nunchuk_controller_buttons);
  load_controller_axes(input, wii_nunchuk_controller_axes,
                       wii_nunchuk_controller_axis_positive,
                       wii_nunchuk_controller_axis_bound);
}

void save_wii_controls(unsigned port = wii_control_port) {
  const auto path = wii_port_controls_path(port);
  if (path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return;
  std::ofstream output(path, std::ios::trunc);
  if (!output) return;
  for (SDL_Keycode key : wii_control_keys)
    output << static_cast<int>(key) << '\n';
  output << (wii_sideways ? 1 : 0) << '\n';
  output << wii_resolution << '\n';
  for (SDL_Keycode key : wii_tilt_keys)
    output << static_cast<int>(key) << '\n';
  output << static_cast<int>(wii_shake_key) << '\n';
  output << (wii_input_mode == InputMode::RealWiimote ? 2 :
             wii_input_mode == InputMode::Controller ? 1 : 0) << '\n';
  save_controller_buttons(output, wii_controller_buttons);
  save_controller_axes(output, wii_controller_axes, wii_controller_axis_positive,
                        wii_controller_axis_bound);
  save_controller_buttons(output, wii_tilt_controller_buttons);
  save_controller_axes(output, wii_tilt_controller_axes,
                        wii_tilt_controller_axis_positive,
                        wii_tilt_controller_axis_bound);
  output << static_cast<int>(wii_shake_controller_button) << '\n';
  output << static_cast<int>(wii_shake_controller_axis) << ' '
         << (wii_shake_controller_axis_positive ? 1 : 0) << '\n';
  output << (wii_nunchuk_enabled ? 1 : 0) << '\n';
  for (SDL_Keycode key : wii_nunchuk_keys)
    output << static_cast<int>(key) << '\n';
  save_controller_buttons(output, wii_nunchuk_controller_buttons);
  save_controller_axes(output, wii_nunchuk_controller_axes,
                        wii_nunchuk_controller_axis_positive,
                        wii_nunchuk_controller_axis_bound);
}

void load_wii_port_count() {
  const auto path = wii_port_count_path();
  if (path.empty()) return;
  std::ifstream input(path);
  unsigned count = 1;
  if (input >> count && count >= 1 && count <= 4)
    wii_active_port_count = count;
}

void save_wii_port_count() {
  const auto path = wii_port_count_path();
  if (path.empty()) return;
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return;
  std::ofstream output(path, std::ios::trunc);
  if (output) output << wii_active_port_count << '\n';
}

std::string dolphin_key_name(SDL_Keycode key) {
  std::string name;
  switch (key) {
    case SDLK_RETURN: name = "Return"; break;
    case SDLK_BACKSPACE: name = "BackSpace"; break;
    case SDLK_UP: name = "Up"; break;
    case SDLK_DOWN: name = "Down"; break;
    case SDLK_LEFT: name = "Left"; break;
    case SDLK_RIGHT: name = "Right"; break;
    case SDLK_SPACE: name = "space"; break;
    default: break;
  }
  if (name.empty() && key >= SDLK_a && key <= SDLK_z)
    name.assign(1, static_cast<char>(std::toupper(
                         static_cast<unsigned char>(key))));
  if (name.empty()) name = "Unknown";
  return '`' + name + '`';
}

std::string dolphin_hotkey_name(SDL_Keycode key) {
  switch (key) {
    case SDLK_TAB: return "Tab";
    case SDLK_RETURN: return "Return";
    case SDLK_BACKSPACE: return "BackSpace";
    case SDLK_UP: return "Up";
    case SDLK_DOWN: return "Down";
    case SDLK_LEFT: return "Left";
    case SDLK_RIGHT: return "Right";
    case SDLK_SPACE: return "Space";
    default:
      if (key >= SDLK_a && key <= SDLK_z)
        return std::string(1, static_cast<char>(
            std::toupper(static_cast<unsigned char>(key))));
      return "Unknown";
  }
}

std::string dolphin_controller_button_name(SDL_GameControllerButton button) {
  std::string name;
  switch (button) {
    case SDL_CONTROLLER_BUTTON_A: name = "Button A"; break;
    case SDL_CONTROLLER_BUTTON_B: name = "Button B"; break;
    case SDL_CONTROLLER_BUTTON_X: name = "Button X"; break;
    case SDL_CONTROLLER_BUTTON_Y: name = "Button Y"; break;
    case SDL_CONTROLLER_BUTTON_BACK: name = "Back"; break;
    case SDL_CONTROLLER_BUTTON_GUIDE: name = "Guide"; break;
    case SDL_CONTROLLER_BUTTON_START: name = "Start"; break;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: name = "Thumb L"; break;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: name = "Thumb R"; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: name = "Shoulder L"; break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: name = "Shoulder R"; break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: name = "Pad N"; break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: name = "Pad S"; break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: name = "Pad W"; break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: name = "Pad E"; break;
    case SDL_CONTROLLER_BUTTON_MISC1: name = "Misc 1"; break;
    case SDL_CONTROLLER_BUTTON_PADDLE1: name = "Paddle 1"; break;
    case SDL_CONTROLLER_BUTTON_PADDLE2: name = "Paddle 2"; break;
    case SDL_CONTROLLER_BUTTON_PADDLE3: name = "Paddle 3"; break;
    case SDL_CONTROLLER_BUTTON_PADDLE4: name = "Paddle 4"; break;
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: name = "Touchpad"; break;
    default: return {};
  }
  return '`' + name + '`';
}

std::string dolphin_controller_dpad_name(SDL_GameControllerButton button) {
  switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "`Pad N`";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "`Pad S`";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "`Pad W`";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "`Pad E`";
    default: return {};
  }
}

std::string dolphin_controller_axis_name(SDL_GameControllerAxis axis,
                                         bool positive) {
  std::string name;
  switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX:
      name = "Left X";
      break;
    case SDL_CONTROLLER_AXIS_LEFTY:
      name = "Left Y";
      positive = !positive;
      break;
    case SDL_CONTROLLER_AXIS_RIGHTX:
      name = "Right X";
      break;
    case SDL_CONTROLLER_AXIS_RIGHTY:
      name = "Right Y";
      positive = !positive;
      break;
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
      name = "Trigger L";
      break;
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
      name = "Trigger R";
      break;
    default:
      return {};
  }
  if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
      axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
    return '`' + name + '`';
  return '`' + name + (positive ? "+" : "-") + '`';
}

bool write_dolphin_controller_ports(bool gamecube) {
  const std::filesystem::path directory = dolphin_config_directory();
  if (directory.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  const std::filesystem::path path = directory / "Dolphin.ini";
  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  bool in_core = false;
  std::array<bool, 4> replaced{};
  while (std::getline(input, line)) {
    if (!line.empty() && line.front() == '[')
      in_core = line == "[Core]";
    if (in_core) {
      for (unsigned port = 1; port <= 4; ++port) {
        const std::string key = gamecube ? "Serial" : "Wiimote";
        if (line.rfind(key + std::to_string(port), 0) == 0) {
          line = key + std::to_string(port) + " = " +
                 (port <= wii_active_port_count
                      ? gamecube ? "6" : "1"
                      : "0");
          replaced[port - 1] = true;
        }
      }
    }
    lines.push_back(line);
  }
  size_t core_start = lines.size();
  size_t core_end = lines.size();
  for (size_t index = 0; index < lines.size(); ++index) {
    if (lines[index] == "[Core]") {
      core_start = index;
      core_end = lines.size();
      for (size_t next = index + 1; next < lines.size(); ++next) {
        if (!lines[next].empty() && lines[next].front() == '[') {
          core_end = next;
          break;
        }
      }
      break;
    }
  }
  if (core_start == lines.size()) {
    if (!lines.empty() && lines.back() != "") lines.push_back("");
    lines.push_back("[Core]");
    core_end = lines.size();
  }
  std::vector<std::string> missing;
  for (unsigned port = 1; port <= 4; ++port)
    if (!replaced[port - 1])
      missing.push_back(
          (gamecube ? "Serial" : "Wiimote") + std::to_string(port) + " = " +
          (port <= wii_active_port_count ? gamecube ? "6" : "1" : "0"));
  lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(core_end),
               missing.begin(), missing.end());
  std::ofstream output(path, std::ios::trunc);
  if (!output) return false;
  for (const std::string &entry : lines) output << entry << '\n';
  return static_cast<bool>(output);
}

bool enable_dolphin_wiimote_scanning() {
  const std::filesystem::path directory = dolphin_config_directory();
  if (directory.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  const std::filesystem::path path = directory / "Dolphin.ini";
  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  bool in_core = false;
  bool section_found = false;
  bool core_setting_found = false;
  while (std::getline(input, line)) {
    if (!line.empty() && line.front() == '[')
      in_core = line == "[Core]";
    if (in_core) section_found = true;
    if (line.rfind("WiimoteContinuousScanning", 0) == 0) {
      line = "WiimoteContinuousScanning = True";
      if (in_core) core_setting_found = true;
    }
    lines.push_back(line);
  }
  if (!section_found) {
    if (!lines.empty() && lines.back() != "") lines.push_back("");
    lines.push_back("[Core]");
  }
  if (!core_setting_found) {
    size_t core_end = lines.size();
    for (size_t index = 0; index < lines.size(); ++index) {
      if (lines[index] == "[Core]") {
        core_end = lines.size();
        for (size_t next = index + 1; next < lines.size(); ++next) {
          if (!lines[next].empty() && lines[next].front() == '[') {
            core_end = next;
            break;
          }
        }
        break;
      }
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(core_end),
                 "WiimoteContinuousScanning = True");
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output) return false;
  for (const std::string &entry : lines) output << entry << '\n';
  return static_cast<bool>(output);
}

int prepare_bluetooth_wiimote_scan() {
#ifdef _WIN32
  return -1;
#else
  if (std::system("command -v bluetoothctl >/dev/null 2>&1") != 0)
    return -1;
  std::system("bluetoothctl power on >/dev/null 2>&1");
  std::system("bluetoothctl agent on >/dev/null 2>&1");
  std::system("bluetoothctl default-agent >/dev/null 2>&1");
  const pid_t pid = fork();
  if (pid != 0) return pid;
  execlp("bluetoothctl", "bluetoothctl", "--timeout", "15", "scan", "on",
         static_cast<char *>(nullptr));
  _exit(127);
#endif
}

void stop_bluetooth_wiimote_scan(int pid) {
#ifndef _WIN32
  if (pid <= 0) return;
  if (waitpid(pid, nullptr, WNOHANG) == 0) {
    kill(pid, SIGTERM);
    waitpid(pid, nullptr, 0);
  }
#else
  (void)pid;
#endif
}

bool write_dolphin_wii_controls(bool gamecube = false) {
  const std::filesystem::path directory = dolphin_config_directory();
  if (directory.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  const std::filesystem::path path =
      directory / (gamecube ? "GCPadNew.ini" : "WiimoteNew.ini");
  std::ofstream output(path, std::ios::trunc);
  if (!output) return false;
  const unsigned original_port = wii_control_port;
  for (unsigned port = 1; port <= wii_active_port_count; ++port) {
    wii_control_port = port;
    load_wii_controls(port);
    const auto controller_binding = [&](size_t index) {
      if (wii_input_mode != InputMode::Controller)
        return dolphin_key_name(wii_control_keys[index]);
      if (wii_controller_axis_bound[index])
        return dolphin_controller_axis_name(
            wii_controller_axes[index], wii_controller_axis_positive[index]);
      return dolphin_controller_button_name(wii_controller_buttons[index]);
    };
    const auto tilt_binding = [&](size_t index) {
      if (wii_input_mode != InputMode::Controller)
        return dolphin_key_name(wii_tilt_keys[index]);
      if (wii_tilt_controller_axis_bound[index])
        return dolphin_controller_axis_name(
            wii_tilt_controller_axes[index],
            wii_tilt_controller_axis_positive[index]);
      return dolphin_controller_button_name(wii_tilt_controller_buttons[index]);
    };
    const auto shake_binding = [&] {
      if (wii_input_mode != InputMode::Controller)
        return dolphin_key_name(wii_shake_key);
      if (wii_shake_controller_axis_bound)
        return dolphin_controller_axis_name(
            wii_shake_controller_axis, wii_shake_controller_axis_positive);
      return dolphin_controller_button_name(wii_shake_controller_button);
    };
    const auto nunchuk_binding = [&](size_t index) {
      if (wii_input_mode != InputMode::Controller)
        return dolphin_key_name(wii_nunchuk_keys[index]);
      if (wii_nunchuk_controller_axis_bound[index])
        return dolphin_controller_axis_name(
            wii_nunchuk_controller_axes[index],
            wii_nunchuk_controller_axis_positive[index]);
      return dolphin_controller_button_name(
          wii_nunchuk_controller_buttons[index]);
    };
    const auto dpad_binding = [&](size_t index) {
      if (wii_input_mode != InputMode::Controller)
        return dolphin_key_name(wii_control_keys[index]);
      if (wii_controller_axis_bound[index])
        return dolphin_controller_axis_name(
            wii_controller_axes[index], wii_controller_axis_positive[index]);
      return dolphin_controller_dpad_name(wii_controller_buttons[index]);
    };
    if (!gamecube) {
      output << "[Wiimote" << port << "]\n"
             << "Source = "
             << (wii_input_mode == InputMode::RealWiimote ? "2" : "1")
             << '\n';
      if (wii_input_mode == InputMode::RealWiimote) {
        output << "Extension = "
               << (wii_nunchuk_enabled ? "Nunchuk" : "None") << "\n\n";
        continue;
      }
      output << "Device = " << (wii_input_mode == InputMode::Controller
                                    ? "SDL/0/" + game_controller_name
                                    : "XInput2/0/Virtual core pointer") << '\n'
             << "Buttons/A = " << controller_binding(0) << '\n'
             << "Buttons/B = " << controller_binding(1) << '\n'
             << "Buttons/1 = " << controller_binding(2) << '\n'
             << "Buttons/2 = " << controller_binding(3) << '\n'
             << "Buttons/- = " << controller_binding(4) << '\n'
             << "Buttons/+ = " << controller_binding(5) << '\n'
             << "Buttons/Home = " << controller_binding(10) << '\n'
             << "D-Pad/Up = " << dpad_binding(6) << '\n'
             << "D-Pad/Down = " << dpad_binding(7) << '\n'
             << "D-Pad/Left = " << dpad_binding(8) << '\n'
             << "D-Pad/Right = " << dpad_binding(9) << '\n'
             << "Options/Sideways Wiimote = "
             << (wii_sideways ? "True" : "False") << '\n'
             << "Tilt/Forward = " << tilt_binding(0) << '\n'
             << "Tilt/Backward = " << tilt_binding(1) << '\n'
             << "Tilt/Left = " << tilt_binding(2) << '\n'
             << "Tilt/Right = " << tilt_binding(3) << '\n'
             << "Shake/X = " << shake_binding() << '\n'
             << "Shake/Y = " << shake_binding() << '\n'
             << "Shake/Z = " << shake_binding() << '\n'
             << "IMUAccelerometer/Shake/X = " << shake_binding() << '\n'
             << "IMUAccelerometer/Shake/Y = " << shake_binding() << '\n'
             << "IMUAccelerometer/Shake/Z = " << shake_binding() << '\n'
             << "Extension = " << (wii_nunchuk_enabled ? "Nunchuk" : "None")
             << '\n'
             << "Nunchuk/Buttons/C = " << nunchuk_binding(0) << '\n'
             << "Nunchuk/Buttons/Z = " << nunchuk_binding(1) << '\n'
             << "Nunchuk/Stick/Up = " << nunchuk_binding(2) << '\n'
             << "Nunchuk/Stick/Down = " << nunchuk_binding(3) << '\n'
             << "Nunchuk/Stick/Left = " << nunchuk_binding(4) << '\n'
             << "Nunchuk/Stick/Right = " << nunchuk_binding(5) << '\n';
    } else {
      output << "[GCPad" << port << "]\n"
             << "Device = " << (wii_input_mode == InputMode::Controller
                                    ? "SDL/0/" + game_controller_name
                                    : "XInput2/0/Virtual core pointer") << '\n'
             << "Buttons/A = " << controller_binding(0) << '\n'
             << "Buttons/B = " << controller_binding(1) << '\n'
             << "Buttons/X = " << controller_binding(2) << '\n'
             << "Buttons/Y = " << controller_binding(3) << '\n'
             << "Buttons/Start = " << controller_binding(4) << '\n'
             << "Main Stick/Up = " << controller_binding(5) << '\n'
             << "Main Stick/Down = " << controller_binding(6) << '\n'
             << "Main Stick/Left = " << controller_binding(7) << '\n'
             << "Main Stick/Right = " << controller_binding(8) << '\n'
             << "Triggers/L = " << controller_binding(9) << '\n'
             << "Triggers/R = " << controller_binding(10) << '\n'
             << "Buttons/Z = " << controller_binding(11) << '\n';
    }
  }
  wii_control_port = original_port;
  load_wii_controls(wii_control_port);
  return static_cast<bool>(output) &&
         write_dolphin_controller_ports(gamecube);
}

bool write_dolphin_wii_resolution(bool gamecube = false) {
  const std::filesystem::path directory = dolphin_config_directory();
  if (directory.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  const std::filesystem::path path = directory / "GFX.ini";
  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  bool in_settings = false;
  bool replaced = false;
  bool aspect_replaced = false;
  bool widescreen_replaced = false;
  bool backend_replaced = false;
  while (std::getline(input, line)) {
    if (!line.empty() && line.front() == '[')
      in_settings = line == "[Settings]";
    if (in_settings && line.rfind("InternalResolution", 0) == 0) {
      line = "InternalResolution = " + std::to_string(wii_resolution);
      replaced = true;
    }
    if (in_settings && line.rfind("AspectRatio", 0) == 0) {
      // GameCube launches use the native 4:3 profile; Wii keeps Dolphin auto.
      line = "AspectRatio = " + std::string(gamecube ? "2" : "0");
      aspect_replaced = true;
    }
    if (in_settings && line.rfind("WidescreenHack", 0) == 0) {
      line = std::string("WidescreenHack = ") +
             (gamecube ? "False" : "True");
      widescreen_replaced = true;
    }
    if (in_settings && line.rfind("BackendMultithreading", 0) == 0) {
      line = "BackendMultithreading = False";
      backend_replaced = true;
    }
    lines.push_back(line);
  }
  if (!replaced) {
    if (!lines.empty() && lines.back() != "") lines.push_back("");
    lines.push_back("[Settings]");
    lines.push_back("InternalResolution = " +
                    std::to_string(wii_resolution));
  }
  if (!aspect_replaced)
    lines.push_back("AspectRatio = " + std::string(gamecube ? "2" : "0"));
  if (!widescreen_replaced)
    lines.push_back(std::string("WidescreenHack = ") +
                    (gamecube ? "False" : "True"));
  if (!backend_replaced)
    lines.push_back("BackendMultithreading = False");
  std::ofstream output(path, std::ios::trunc);
  if (!output) return false;
  for (const std::string &entry : lines) output << entry << '\n';
  return static_cast<bool>(output);
}

bool write_dolphin_wii_hotkeys() {
  const std::filesystem::path directory = dolphin_config_directory();
  if (directory.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  std::ofstream output(directory / "Hotkeys.ini", std::ios::trunc);
  if (!output) return false;
  output << "[Hotkeys]\n"
         << "Device = XInput2/0/Virtual core pointer\n"
         << "General/Stop = Escape\n"
         << "Emulation Speed/Disable Emulation Speed Limit = "
         << dolphin_hotkey_name(turbo_key) << '\n';
  return static_cast<bool>(output);
}

bool disable_dolphin_osd_messages() {
  const std::filesystem::path directory = dolphin_config_directory();
  if (directory.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  const std::filesystem::path path = directory / "Dolphin.ini";
  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  bool in_interface = false;
  bool replaced = false;
  while (std::getline(input, line)) {
    if (!line.empty() && line.front() == '[')
      in_interface = line == "[Interface]";
    if (in_interface && line.rfind("OnScreenDisplayMessages", 0) == 0) {
      line = "OnScreenDisplayMessages = False";
      replaced = true;
    }

    lines.push_back(line);
  }
  if (!replaced) {
    if (!lines.empty() && lines.back() != "") lines.push_back("");
    lines.push_back("[Interface]");
    lines.push_back("OnScreenDisplayMessages = False");
  }
  std::ofstream output(path, std::ios::trunc);
  if (!output) return false;
  for (const std::string &entry : lines) output << entry << '\n';
  return static_cast<bool>(output);
}

bool write_dolphin_window_size(int width, int height) {
  const std::filesystem::path directory = dolphin_config_directory();
  if (directory.empty()) return false;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) return false;
  const std::filesystem::path path = directory / "Dolphin.ini";
  std::ifstream input(path);
  std::vector<std::string> lines;
  std::string line;
  bool in_display = false;
  bool display_found = false;
  bool width_replaced = false;
  bool height_replaced = false;
  bool autosize_replaced = false;
  bool fullscreen_replaced = false;
  while (std::getline(input, line)) {
    if (!line.empty() && line.front() == '[')
      in_display = line == "[Display]";
    if (in_display) display_found = true;
    if (in_display && line.rfind("RenderWindowWidth", 0) == 0) {
      line = "RenderWindowWidth = " + std::to_string(width);
      width_replaced = true;
    } else if (in_display && line.rfind("RenderWindowHeight", 0) == 0) {
      line = "RenderWindowHeight = " + std::to_string(height);
      height_replaced = true;
    } else if (in_display && line.rfind("RenderWindowAutoSize", 0) == 0) {
      line = "RenderWindowAutoSize = False";
      autosize_replaced = true;
    } else if (in_display && line.rfind("Fullscreen", 0) == 0) {
      line = "Fullscreen = False";
      fullscreen_replaced = true;
    }
    lines.push_back(line);
  }
  if (!display_found) {
    if (!lines.empty() && lines.back() != "") lines.push_back("");
    lines.push_back("[Display]");
  }
  if (!width_replaced) lines.push_back("RenderWindowWidth = " + std::to_string(width));
  if (!height_replaced) lines.push_back("RenderWindowHeight = " + std::to_string(height));
  if (!autosize_replaced) lines.push_back("RenderWindowAutoSize = False");
  if (!fullscreen_replaced) lines.push_back("Fullscreen = False");
  std::ofstream output(path, std::ios::trunc);
  if (!output) return false;
  for (const std::string &entry : lines) output << entry << '\n';
  return static_cast<bool>(output);
}

const VkApplicationInfo *VulkanHost::get_application_info() {
  static const VkApplicationInfo application{
      VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "Flowmulator", 1,
      "Flowmulator", 1, (1u << 22) | (0u << 12)};
  return &application;
}

bool VulkanHost::initialize(SDL_Window *window) {
  window_ = window;
  active_ = this;
  active_vulkan_host = this;
  if (!create_instance(window)) {
    destroy();
    active_ = nullptr;
    active_vulkan_host = nullptr;
    return false;
  }
  return true;
}

bool VulkanHost::create_instance(SDL_Window *window) {
  if (SDL_Vulkan_LoadLibrary(nullptr) != 0) {
    std::cerr << "Could not load Vulkan: " << SDL_GetError() << '\n';
    return false;
  }
  get_instance_proc_addr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (!get_instance_proc_addr_) return false;

  unsigned extension_count = 0;
  if (!SDL_Vulkan_GetInstanceExtensions(window, &extension_count, nullptr))
    return false;
  std::vector<const char *> extensions(extension_count);
  if (!SDL_Vulkan_GetInstanceExtensions(window, &extension_count,
                                        extensions.data()))
    return false;
  VkApplicationInfo application = *get_application_info();
  VkInstanceCreateInfo create_info{};
  create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  create_info.pApplicationInfo = &application;
  create_info.enabledExtensionCount = extension_count;
  create_info.ppEnabledExtensionNames = extensions.data();
  auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(
      get_instance_proc_addr_(VK_NULL_HANDLE, "vkCreateInstance"));
  if (!create_instance ||
      create_instance(&create_info, nullptr, &instance_) != VK_SUCCESS) {
    std::cerr << "Could not create Vulkan instance\n";
    return false;
  }
  if (!SDL_Vulkan_CreateSurface(window, instance_, &surface_)) {
    std::cerr << "Could not create Vulkan surface: " << SDL_GetError() << '\n';
    return false;
  }
  auto enumerate = instance_function<PFN_vkEnumeratePhysicalDevices>(
      "vkEnumeratePhysicalDevices");
  uint32_t count = 0;
  if (!enumerate || enumerate(instance_, &count, nullptr) != VK_SUCCESS ||
      !count)
    return false;
  std::vector<VkPhysicalDevice> devices(count);
  enumerate(instance_, &count, devices.data());
  auto queue_properties =
      instance_function<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          "vkGetPhysicalDeviceQueueFamilyProperties");
  auto present_support =
      instance_function<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
          "vkGetPhysicalDeviceSurfaceSupportKHR");
  if (!queue_properties || !present_support) return false;
  for (VkPhysicalDevice device : devices) {
    uint32_t queue_count = 0;
    queue_properties(device, &queue_count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(queue_count);
    queue_properties(device, &queue_count, properties.data());
    for (uint32_t i = 0; i < queue_count; ++i) {
      VkBool32 supports_present = 0;
      if ((properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
          properties[i].queueCount &&
          present_support(device, i, surface_, &supports_present) == VK_SUCCESS &&
          supports_present) {
        gpu_ = device;
        queue_family_ = i;
        break;
      }
    }
    if (gpu_) break;
  }
  if (!gpu_) {
    std::cerr << "No Vulkan graphics/present queue is available\n";
    return false;
  }
  return true;
}

bool VulkanHost::set_hw_render(retro_hw_render_callback *callback) {
  if (!callback || callback->context_type != RETRO_HW_CONTEXT_VULKAN)
    return false;
  context_reset_callback_ = callback->context_reset;
  callback->context_reset = context_reset;
  callback->context_destroy = context_destroy;
  callback->get_current_framebuffer = nullptr;
  callback->get_proc_address = nullptr;
  callback->bottom_left_origin = true;
  callback->version_major = 1;
  callback->version_minor = 0;
  return true;
}

bool VulkanHost::set_negotiation(
    retro_hw_render_context_negotiation_interface_vulkan *interface) {
  if (!interface ||
      interface->interface_type !=
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN ||
      !interface->create_device)
    return false;
  negotiation_ = interface;
  return true;
}

bool VulkanHost::activate() {
  if (!negotiation_ || !negotiation_->create_device) return false;
  retro_vulkan_context context{};
  const char *required_extensions[] = {"VK_KHR_swapchain"};
  if (!negotiate_device(&context, instance_, gpu_, surface_,
                        get_instance_proc_addr_, required_extensions, 1,
                        nullptr, 0, nullptr))
    return false;
  gpu_ = context.gpu;
  device_ = context.device;
  queue_ = context.queue;
  queue_family_ = context.queue_family_index;
  get_device_proc_addr_ = context.device
      ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            get_instance_proc_addr_(instance_, "vkGetDeviceProcAddr"))
      : nullptr;
  if (!get_device_proc_addr_ || !create_swapchain()) return false;
  render_interface_ = {
      RETRO_HW_RENDER_INTERFACE_VULKAN,
      RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION,
      this,
      instance_,
      context.gpu,
      context.device,
      get_device_proc_addr_,
      get_instance_proc_addr_,
      context.queue,
      context.queue_family_index,
      set_image,
      get_sync_index,
      get_sync_index_mask,
      set_command_buffers,
      wait_sync_index,
      lock_queue,
      unlock_queue,
      set_signal_semaphore};
  if (context_reset_callback_) context_reset_callback_();
  return true;
}

bool VulkanHost::negotiate_device(
    retro_vulkan_context *context, VkInstance instance, VkPhysicalDevice gpu,
    VkSurfaceKHR surface, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    const char **extensions, unsigned extension_count, const char **layers,
    unsigned layer_count, const VkPhysicalDeviceFeatures *features) {
  if (!active_ || !active_->negotiation_ ||
      !active_->negotiation_->create_device)
    return false;
  std::vector<const char *> required;
  for (unsigned i = 0; i < extension_count; ++i)
    required.push_back(extensions[i]);
  required.push_back("VK_KHR_swapchain");
  return active_->negotiation_->create_device(
      context, instance, gpu, surface, get_instance_proc_addr,
      required.data(), static_cast<unsigned>(required.size()), layers,
      layer_count, features);
}

bool VulkanHost::create_swapchain() {
  if (!device_) return false;
  auto get_surface_capabilities =
      instance_function<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  auto get_surface_formats =
      instance_function<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(
          "vkGetPhysicalDeviceSurfaceFormatsKHR");
  auto get_present_modes =
      instance_function<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(
          "vkGetPhysicalDeviceSurfacePresentModesKHR");
  auto get_queue = device_function<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
  auto create_swapchain =
      device_function<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR");
  auto get_images =
      device_function<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
  if (!get_surface_capabilities || !get_surface_formats ||
      !get_present_modes || !get_queue || !create_swapchain || !get_images)
    return false;
  VkSurfaceCapabilitiesKHR capabilities{};
  uint32_t format_count = 0;
  get_surface_capabilities(gpu_, surface_, &capabilities);
  get_surface_formats(gpu_, surface_, &format_count, nullptr);
  if (!format_count) return false;
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  get_surface_formats(gpu_, surface_, &format_count, formats.data());
  VkSurfaceFormatKHR format = formats.front();
  for (const auto &candidate : formats) {
    if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
        candidate.format == VK_FORMAT_R8G8B8A8_UNORM) {
      format = candidate;
      break;
    }
  }
  int width = 960, height = 720;
  SDL_Vulkan_GetDrawableSize(window_, &width, &height);
  if (!width || !height) return false;
  swapchain_width_ = static_cast<uint32_t>(width);
  swapchain_height_ = static_cast<uint32_t>(height);
  uint32_t image_count = capabilities.minImageCount + 1;
  if (capabilities.maxImageCount && image_count > capabilities.maxImageCount)
    image_count = capabilities.maxImageCount;
  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = surface_;
  info.minImageCount = image_count;
  info.imageFormat = format.format;
  info.imageColorSpace = format.colorSpace;
  info.imageExtent = {swapchain_width_, swapchain_height_};
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = capabilities.currentTransform;
  info.compositeAlpha = 1;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  info.clipped = 1;
  if (create_swapchain(device_, &info, nullptr, &swapchain_) != VK_SUCCESS)
    return false;
  swapchain_format_ = format.format;
  uint32_t actual_count = 0;
  get_images(device_, swapchain_, &actual_count, nullptr);
  swapchain_images_.resize(actual_count);
  get_images(device_, swapchain_, &actual_count, swapchain_images_.data());
  swapchain_layouts_.assign(actual_count, VK_IMAGE_LAYOUT_UNDEFINED);
  get_queue(device_, queue_family_, 0, &queue_);
  auto create_pool = device_function<PFN_vkCreateCommandPool>(
      "vkCreateCommandPool");
  auto allocate = device_function<PFN_vkAllocateCommandBuffers>(
      "vkAllocateCommandBuffers");
  auto create_semaphore = device_function<PFN_vkCreateSemaphore>(
      "vkCreateSemaphore");
  auto create_fence =
      device_function<PFN_vkCreateFence>("vkCreateFence");
  if (!create_pool || !allocate || !create_semaphore || !create_fence)
    return false;
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_;
  if (create_pool(device_, &pool_info, nullptr, &command_pool_) != VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  allocate_info.commandPool = command_pool_;
  allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocate_info.commandBufferCount = 1;
  if (allocate(device_, &allocate_info, &command_buffer_) != VK_SUCCESS)
    return false;
  VkSemaphoreCreateInfo semaphore_info{};
  semaphore_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  if (create_semaphore(device_, &semaphore_info, nullptr, &acquire_semaphore_) !=
          VK_SUCCESS ||
      create_semaphore(device_, &semaphore_info, nullptr, &render_semaphore_) !=
          VK_SUCCESS)
    return false;
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.flags = 1;  // VK_FENCE_CREATE_SIGNALED_BIT
  if (create_fence(device_, &fence_info, nullptr, &render_fence_) != VK_SUCCESS)
    return false;
  return true;
}

uint32_t VulkanHost::get_sync_index(void *) {
  return active_ ? active_->frame_index_ % 2 : 0;
}

uint32_t VulkanHost::get_sync_index_mask(void *) { return 3; }

void VulkanHost::wait_sync_index(void *) {
  // Dolphin's queue wrapper serializes submissions with lock_queue().  The
  // host presents on that same queue, so an explicit idle wait here can
  // deadlock the driver's sync-object wait during the first frame.
}

void VulkanHost::lock_queue(void *) {
  if (active_) active_->queue_mutex_.lock();
}

void VulkanHost::unlock_queue(void *) {
  if (active_) active_->queue_mutex_.unlock();
}

void VulkanHost::set_image(void *handle, const retro_vulkan_image *image,
                           uint32_t, const VkSemaphore *, uint32_t) {
  auto *host = static_cast<VulkanHost *>(handle);
  if (!host || !image) return;
  std::lock_guard<std::mutex> lock(host->image_mutex_);
  host->latest_image_ = *image;
  host->have_image_ = true;
}

void VulkanHost::copy_latest_image(uint32_t target_index) {
  std::lock_guard<std::mutex> lock(image_mutex_);
  if (!have_image_ || target_index >= swapchain_images_.size()) return;
  auto reset = device_function<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");
  auto begin = device_function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
  auto end = device_function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer");
  auto barrier = device_function<PFN_vkCmdPipelineBarrier>(
      "vkCmdPipelineBarrier");
  auto blit = device_function<PFN_vkCmdBlitImage>("vkCmdBlitImage");
  if (!reset || !begin || !end || !barrier || !blit) return;
  reset(command_buffer_, 0);
  VkCommandBufferBeginInfo begin_info{};
  begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (begin(command_buffer_, &begin_info) != VK_SUCCESS) return;
  const VkImage source = latest_image_.create_info.image;
  VkImageMemoryBarrier barriers[2]{};
  barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barriers[0].oldLayout = latest_image_.image_layout;
  barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].srcQueueFamilyIndex = ~0u;
  barriers[0].dstQueueFamilyIndex = ~0u;
  barriers[0].image = source;
  barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  barriers[1] = barriers[0];
  barriers[1].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barriers[1].oldLayout = swapchain_layouts_[target_index];
  barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].image = swapchain_images_[target_index];
  barrier(command_buffer_, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2,
          barriers);
  VkImageBlit region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[1] = {static_cast<int32_t>(frame_width_),
                          static_cast<int32_t>(frame_height_), 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstOffsets[1] = {static_cast<int32_t>(swapchain_width_),
                          static_cast<int32_t>(swapchain_height_), 1};
  blit(command_buffer_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
       swapchain_images_[target_index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
       &region, 1);
  barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barriers[0].newLayout = latest_image_.image_layout;
  barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier(command_buffer_, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          0, 0, nullptr, 0, nullptr, 2, barriers);
  end(command_buffer_);
  swapchain_layouts_[target_index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

void VulkanHost::present() {
  if (!ready()) return;
  auto acquire = device_function<PFN_vkAcquireNextImageKHR>(
      "vkAcquireNextImageKHR");
  auto submit = device_function<PFN_vkQueueSubmit>("vkQueueSubmit");
  auto present_fn =
      device_function<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
  auto wait_fences =
      device_function<PFN_vkWaitForFences>("vkWaitForFences");
  auto reset_fences =
      device_function<PFN_vkResetFences>("vkResetFences");
  if (!acquire || !submit || !present_fn || !wait_fences || !reset_fences)
    return;
  wait_fences(device_, 1, &render_fence_, 1, UINT64_MAX);
  reset_fences(device_, 1, &render_fence_);
  uint32_t index = 0;
  VkResult result = acquire(device_, swapchain_, UINT64_MAX, acquire_semaphore_,
                            VK_NULL_HANDLE, &index);
  if (result == VK_ERROR_OUT_OF_DATE_KHR) return;
  copy_latest_image(index);
  VkFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &acquire_semaphore_;
  submit_info.pWaitDstStageMask = &wait_stage;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &command_buffer_;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &render_semaphore_;
  if (submit(queue_, 1, &submit_info, render_fence_) != VK_SUCCESS) return;
  VkPresentInfoKHR present_info{};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &render_semaphore_;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &swapchain_;
  present_info.pImageIndices = &index;
  present_fn(queue_, &present_info);
  ++frame_index_;
}

void VulkanHost::destroy() {
  if (device_) {
    auto wait = device_function<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    if (wait) wait(device_);
    auto destroy_semaphore = device_function<PFN_vkDestroySemaphore>(
        "vkDestroySemaphore");
    auto destroy_fence =
        device_function<PFN_vkDestroyFence>("vkDestroyFence");
    auto destroy_pool =
        device_function<PFN_vkDestroyCommandPool>("vkDestroyCommandPool");
    auto destroy_swapchain = device_function<PFN_vkDestroySwapchainKHR>(
        "vkDestroySwapchainKHR");
    if (destroy_semaphore) {
      if (acquire_semaphore_) destroy_semaphore(device_, acquire_semaphore_, nullptr);
      if (render_semaphore_) destroy_semaphore(device_, render_semaphore_, nullptr);
    }
    if (destroy_fence && render_fence_)
      destroy_fence(device_, render_fence_, nullptr);
    if (destroy_pool && command_pool_)
      destroy_pool(device_, command_pool_, nullptr);
    if (destroy_swapchain && swapchain_)
      destroy_swapchain(device_, swapchain_, nullptr);
    if (device_ && negotiation_ && negotiation_->destroy_device)
      negotiation_->destroy_device();
  }
  if (instance_) {
    auto destroy_surface = instance_function<PFN_vkDestroySurfaceKHR>(
        "vkDestroySurfaceKHR");
    if (destroy_surface && surface_) destroy_surface(instance_, surface_, nullptr);
    auto destroy_instance = reinterpret_cast<PFN_vkDestroyInstance>(
        get_instance_proc_addr_(instance_, "vkDestroyInstance"));
    if (destroy_instance) destroy_instance(instance_, nullptr);
  }
  if (get_instance_proc_addr_) SDL_Vulkan_UnloadLibrary();
  instance_ = VK_NULL_HANDLE;
  surface_ = VK_NULL_HANDLE;
  gpu_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  queue_ = VK_NULL_HANDLE;
  swapchain_ = VK_NULL_HANDLE;
  command_pool_ = VK_NULL_HANDLE;
  command_buffer_ = VK_NULL_HANDLE;
  acquire_semaphore_ = VK_NULL_HANDLE;
  render_semaphore_ = VK_NULL_HANDLE;
  render_fence_ = VK_NULL_HANDLE;
  swapchain_images_.clear();
  swapchain_layouts_.clear();
  have_image_ = false;
}

bool environment(unsigned command, void *data) {
  switch (command) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      if (!data) return false;
      static_cast<retro_log_callback *>(data)->log = core_log;
      return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      if (!data || core_save_directory.empty()) return false;
      *static_cast<const char **>(data) = core_save_directory.c_str();
      return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      if (!data) return false;
      auto *variable = static_cast<retro_variable *>(data);
      if (!variable->key) return false;
      if (std::strcmp(variable->key, "desmume_pointer_mouse") == 0) {
        variable->value = "enabled";
        return true;
      }
      if (std::strcmp(variable->key, "desmume_pointer_type") == 0) {
        variable->value = "touch";
        return true;
      }
      if (std::strcmp(variable->key, "desmume_screens_layout") == 0) {
        variable->value = nds_layouts[nds_layout_index];
        return true;
      }
      if (three_ds_active &&
          std::strcmp(variable->key, "citra_graphics_api") == 0) {
        variable->value = "OpenGL";
        return true;
      }
      if (three_ds_active) {
        static const std::array<std::pair<const char *, const char *>, 30>
            defaults{{
                {"citra_use_cpu_jit", "enabled"},
                {"citra_cpu_clock_percentage", "100"},
                {"citra_is_new_3ds", "disabled"},
                {"citra_region_value", "Auto"},
                {"citra_language_value", "English"},
                {"citra_audio_emulation", "HLE"},
                {"citra_input_type", "digital"},
                {"citra_use_hw_shader", "enabled"},
                {"citra_use_shader_jit", "enabled"},
                {"citra_shaders_accurate_mul", "enabled"},
                {"citra_use_disk_shader_cache", "enabled"},
                {"citra_resolution_factor", "1"},
                {"citra_texture_filter", "none"},
                {"citra_texture_sampling", "Game controlled"},
                {"citra_custom_textures", "disabled"},
                {"citra_dump_textures", "disabled"},
                {"citra_layout_option", "Default"},
                {"citra_render_3d", "Off"},
                {"citra_factor_3d", "0"},
                {"citra_swap_screen", "disabled"},
                {"citra_swap_screen_mode", "Toggle"},
                {"citra_large_screen_proportion", "5"},
                {"citra_use_virtual_sd", "enabled"},
                {"citra_use_libretro_save_path", "enabled"},
                {"citra_analog_function", "Circle Pad"},
                {"citra_analog_deadzone", "0.1"},
                {"citra_enable_mouse_touchscreen", "enabled"},
                {"citra_enable_touch_touchscreen", "enabled"},
                {"citra_enable_touch_pointer_timeout", "disabled"},
                {"citra_enable_motion", "disabled"},
            }};
        for (const auto &[key, value] : defaults)
          if (std::strcmp(variable->key, key) == 0) {
            variable->value = value;
            return true;
          }
      }
      if (wii_active &&
          (std::strcmp(variable->key, "dolphin_renderer") == 0 ||
           std::strcmp(variable->key, "video_backend") == 0)) {
        variable->value = "Hardware";
        return true;
      }
      return false;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
      return three_ds_active && data;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
      if (!data || !wii_vulkan) return false;
      *static_cast<retro_hw_context_type *>(data) = RETRO_HW_CONTEXT_VULKAN;
      return true;
    case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT:
      if (!data || !wii_vulkan) return false;
      if (static_cast<retro_hw_render_context_negotiation_interface *>(
              data)->interface_type ==
          RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN)
        static_cast<retro_hw_render_context_negotiation_interface *>(data)
            ->interface_version =
            RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION;
      return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      pixel_format = *static_cast<unsigned *>(data);
      return pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ||
             pixel_format == RETRO_PIXEL_FORMAT_RGB565 ||
             pixel_format == RETRO_PIXEL_FORMAT_0RGB1555;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      if (wii_vulkan && active_vulkan_host)
        return active_vulkan_host->set_hw_render(
            static_cast<retro_hw_render_callback *>(data));
      if (!data || !gl_context) return false;
      auto *callback = static_cast<retro_hw_render_callback *>(data);
      if (callback->context_type != RETRO_HW_CONTEXT_OPENGL &&
          callback->context_type != RETRO_HW_CONTEXT_OPENGL_CORE)
        return false;
      callback->context_reset = hardware_context_reset;
      callback->get_current_framebuffer = hardware_current_framebuffer;
      callback->get_proc_address = hardware_get_proc_address;
      callback->context_destroy = hardware_context_reset;
      callback->version_major = 3;
      callback->version_minor = 3;
      SDL_GL_MakeCurrent(SDL_GL_GetCurrentWindow(), gl_context);
      if (callback->context_reset) callback->context_reset();
      if (three_ds_active) three_ds_hardware_render = true;
      return true;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE:
      if (!data || !wii_vulkan || !active_vulkan_host)
        return false;
      *static_cast<const retro_hw_render_interface **>(data) =
          reinterpret_cast<const retro_hw_render_interface *>(
              active_vulkan_host->render_interface());
      return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE:
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_OLD:
      if (!data || !wii_vulkan || !active_vulkan_host) return false;
      return active_vulkan_host->set_negotiation(
          static_cast<retro_hw_render_context_negotiation_interface_vulkan *>(
              data));
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
      const auto *geometry = static_cast<const retro_game_geometry *>(data);
      frame_width = geometry->base_width;
      frame_height = geometry->base_height;
      framebuffer.resize(static_cast<size_t>(frame_width) * frame_height);
      return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      if (core_system_directory.empty()) {
#ifdef _WIN32
        core_system_directory =
            (runtime_directory() / "dolphin-emu").string();
#else
        core_system_directory =
            (runtime_directory() / "dolphin-emu").string();
#endif
      }
      *static_cast<const char **>(data) = core_system_directory.c_str();
      return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      return false;
    default:
      return false;
  }
}

void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
  if (data == RETRO_HW_FRAME_BUFFER_VALID) {
    if (wii_vulkan && active_vulkan_host)
      active_vulkan_host->set_frame_size(width, height);
    return;
  }
  if (!data || !width || !height) return;
  frame_width = width;
  frame_height = height;
  if (three_ds_active && !logged_3ds_frame) {
    std::cerr << "3DS video frame received: " << width << "x" << height
              << '\n';
    logged_3ds_frame = true;
  }
  framebuffer.resize(static_cast<size_t>(width) * height);
  const auto *source = static_cast<const uint8_t *>(data);
  for (unsigned y = 0; y < height; ++y) {
    const auto *row = reinterpret_cast<const uint8_t *>(
        reinterpret_cast<const uint8_t *>(source) + y * pitch);
    for (unsigned x = 0; x < width; ++x) {
      uint8_t r, g, b;
      if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
        const uint32_t color = reinterpret_cast<const uint32_t *>(row)[x];
        r = color >> 16; g = color >> 8; b = color;
      } else {
        const uint16_t color = reinterpret_cast<const uint16_t *>(row)[x];
        if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
          const unsigned red = color >> 11;
          const unsigned green = (color >> 5) & 0x3f;
          const unsigned blue = color & 0x1f;
          r = static_cast<uint8_t>((red << 3) | (red >> 2));
          g = static_cast<uint8_t>((green << 2) | (green >> 4));
          b = static_cast<uint8_t>((blue << 3) | (blue >> 2));
        } else {
          const unsigned red = (color >> 10) & 0x1f;
          const unsigned green = (color >> 5) & 0x1f;
          const unsigned blue = color & 0x1f;
          r = static_cast<uint8_t>((red << 3) | (red >> 2));
          g = static_cast<uint8_t>((green << 3) | (green >> 2));
          b = static_cast<uint8_t>((blue << 3) | (blue >> 2));
        }

      }
      framebuffer[static_cast<size_t>(y) * width + x] =
          0xff000000u | (static_cast<uint32_t>(r) << 16) |
          (static_cast<uint32_t>(g) << 8) | b;
    }
  }
}

GLuint compile_shader(GLenum type, const char *source) {
      const GLuint shader = glCreateShader(type);
      glShaderSource(shader, 1, &source, nullptr);
      glCompileShader(shader);
      GLint ok = 0;
      glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
      if (!ok) {
        char log[2048]{};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "CRT shader compile failed: " << log << '\n';
        glDeleteShader(shader);
        return 0;
      }
      return shader;
    }

GLuint create_crt_program() {
      static constexpr const char *vertex = R"GLSL(
        #version 120
        varying vec2 uv;
        void main() {
          uv = gl_MultiTexCoord0.st;
          gl_Position = gl_Vertex;
        }
      )GLSL";
      static constexpr const char *fragment = R"GLSL(
        #version 120
        uniform sampler2D frame;
        uniform vec2 texel;
        uniform vec4 screenRect;
        uniform vec4 sourceRect;
        uniform float enabled;
        varying vec2 uv;
        void main() {
          vec2 screenUv = vec2(
              (gl_FragCoord.x - screenRect.x) / screenRect.z,
              1.0 - (gl_FragCoord.y - screenRect.y) / screenRect.w);
          vec2 sampleUv = sourceRect.xy + screenUv * sourceRect.zw;
          if (sampleUv.x < 0.0 || sampleUv.x > 1.0 ||
              sampleUv.y < 0.0 || sampleUv.y > 1.0) {
            gl_FragColor = vec4(0.0);
            return;
          }
          if (enabled < 0.5) {
            gl_FragColor = texture2D(frame, sampleUv);
            return;
          }
          vec2 pixel = sampleUv / texel - 0.5;
          vec2 centerUv = sampleUv;
          vec3 left = texture2D(frame, centerUv - vec2(texel.x, 0.0)).rgb;
          vec3 center = texture2D(frame, centerUv).rgb;
          vec3 right = texture2D(frame, centerUv + vec2(texel.x, 0.0)).rgb;
          vec3 color = left * 0.04 + center * 0.92 + right * 0.04;

          float scanPhase = fract(gl_FragCoord.y * 0.5);
          float scanline = 0.76 + 0.24 *
                           smoothstep(0.18, 0.48, scanPhase) *
                           (1.0 - smoothstep(0.52, 0.82, scanPhase));
          float horizontalGrille = 0.90 + 0.10 *
                                   smoothstep(0.10, 0.42, scanPhase) *
                                   (1.0 - smoothstep(0.58, 0.90, scanPhase));

          float triadPhase = fract(gl_FragCoord.x / 3.0);
          vec3 mask;
          if (triadPhase < 0.3333)
            mask = vec3(1.0, 0.84, 0.84);
          else if (triadPhase < 0.6666)
            mask = vec3(0.84, 1.0, 0.84);
          else
            mask = vec3(0.84, 0.84, 1.0);
          float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
          color = mix(vec3(luminance), color, 1.24);
          color = min(color * 1.08, 1.0);
          color *= mask * scanline * horizontalGrille;
          gl_FragColor = vec4(color, 1.0);
        }
      )GLSL";
      const GLuint vs = compile_shader(GL_VERTEX_SHADER, vertex);
      const GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fragment);
      if (!vs || !fs) return 0;
      const GLuint program = glCreateProgram();
      glAttachShader(program, vs);
      glAttachShader(program, fs);
      glLinkProgram(program);
      glDeleteShader(vs);
      glDeleteShader(fs);
      GLint ok = 0;
      glGetProgramiv(program, GL_LINK_STATUS, &ok);
      if (!ok) {
        glDeleteProgram(program);
        return 0;
      }
      return program;
}

GLuint load_gba_overlay_texture() {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(
      GL_TEXTURE_2D, 0, GL_RGBA, 1920, 1080, 0, GL_RGBA,
      GL_UNSIGNED_BYTE, _binary__assets_gba_overlay_rgba_start);
  return texture;
}

GLuint load_nds_overlay_texture() {
  GLuint texture = 0;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(
      GL_TEXTURE_2D, 0, GL_RGBA, 1920, 1080, 0, GL_RGBA,
      GL_UNSIGNED_BYTE, _binary__assets_nds_overlay_rgba_start);
  return texture;
}

GLuint load_nds_top_overlay_texture() {
 GLuint texture = 0;
 glGenTextures(1, &texture);
 glBindTexture(GL_TEXTURE_2D, texture);
 glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
 glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
 glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
 glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
 glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
 glTexImage2D(
     GL_TEXTURE_2D, 0, GL_RGBA, 1920, 1080, 0, GL_RGBA,
     GL_UNSIGNED_BYTE, _binary__assets_nds_top_overlay_rgba_start);
 return texture;
}


size_t audio_batch(const int16_t *data, size_t frames) {
  if (audio_device && data) {
    const Uint32 queued = SDL_GetQueuedAudioSize(audio_device);
    const Uint32 max_queued = audio_rate * 2 * sizeof(int16_t) / 5;
    if (queued > max_queued) SDL_ClearQueuedAudio(audio_device);
    std::vector<int16_t> mixed(data, data + frames * 2);
    for (size_t i = 0; i < frames; ++i) {
      int left = mixed[i * 2];
      int right = mixed[i * 2 + 1];
      if (big_forehead_effect_active() || title_edition == 2) {
        const int mid = (left + right) / 2;
        const int side = (left - right) / 2;
        forehead_mid_average += (mid - forehead_mid_average) * 0.0015;
        forehead_side_average += (side - forehead_side_average) * 0.0015;
        const double transient_mid = (mid - forehead_mid_average) * 0.28;
        const double transient_side = (side - forehead_side_average) * 0.16;
        const int filtered = static_cast<int>(transient_mid);
        const int filtered_side = static_cast<int>(transient_side);
        left = static_cast<int>((filtered_side + filtered) * 0.75);
        right = static_cast<int>((-filtered_side + filtered) * 0.75);
      }
      if ((drilboor_effect_active() || title_edition == 1) &&
          !jackhammer_clip.empty() &&
          !jackhammer_voice_positions.empty()) {
        for (size_t &position : jackhammer_voice_positions) {
          if (position + 1 < jackhammer_clip.size()) {
            left += jackhammer_clip[position];
            right += jackhammer_clip[position + 1];
            position += 2;
          }
        }
      }
      if ((big_forehead_effect_active() || title_edition == 2) &&
          big_forehead_clip.size() >= 2) {
        left += static_cast<int>(big_forehead_clip[big_forehead_position] * 0.75);
        right += static_cast<int>(
            big_forehead_clip[big_forehead_position + 1] * 0.75);
        big_forehead_position =
            (big_forehead_position + 2) % big_forehead_clip.size();
      }
      for (SkywalkerVoice &voice : skywalker_voices) {
        const auto &clip = voice.oooooh ? skywalker_oooooh_clip
                                       : skywalker_splash_clip;
        const size_t sample = static_cast<size_t>(voice.position);
        if (sample + 1 < clip.size()) {
          left += static_cast<int>(clip[sample] *
                                   voice.gain);
          right += static_cast<int>(clip[sample + 1] *
                                    voice.gain);
          voice.position += 2.0 * voice.step;
        }
      }
      skywalker_voices.erase(
          std::remove_if(skywalker_voices.begin(), skywalker_voices.end(),
                         [](const SkywalkerVoice &voice) {
                           return voice.position + 1.0 >=
                                  static_cast<double>(
                                      voice.oooooh ? skywalker_oooooh_clip.size()
                                                    : skywalker_splash_clip.size());
                         }),
          skywalker_voices.end());
      mixed[i * 2] = static_cast<int16_t>(std::clamp(left, -32768, 32767));
      mixed[i * 2 + 1] = static_cast<int16_t>(
          std::clamp(right, -32768, 32767));
    }
    SDL_QueueAudio(audio_device, mixed.data(),
                   mixed.size() * sizeof(int16_t));
  }

  return frames;
}

bool load_audio_clip(const uint8_t *encoded, size_t encoded_size,
                     const SDL_AudioSpec &target,
                     std::vector<int16_t> &clip) {
  SDL_AudioSpec source{};
  Uint8 *data = nullptr;
  Uint32 length = 0;
  SDL_RWops *rw = SDL_RWFromConstMem(encoded, static_cast<int>(encoded_size));
  if (!rw || !SDL_LoadWAV_RW(rw, SDL_TRUE, &source, &data, &length)) return false;
  SDL_AudioCVT converter{};
  if (SDL_BuildAudioCVT(&converter, source.format, source.channels,
                        source.freq, target.format, target.channels,
                        target.freq) < 0) {
    SDL_FreeWAV(data);
    return false;
  }
  converter.len = static_cast<int>(length);
  const size_t buffer_size = converter.needed
      ? static_cast<size_t>(length) * converter.len_mult
      : length;
  converter.buf = static_cast<Uint8 *>(SDL_malloc(buffer_size));
  if (!converter.buf) {
    SDL_FreeWAV(data);
    return false;
  }
  std::memcpy(converter.buf, data, length);
  SDL_FreeWAV(data);
  if (SDL_ConvertAudio(&converter) < 0) {
    SDL_free(converter.buf);
    return false;
  }
  clip.assign(
      reinterpret_cast<int16_t *>(converter.buf),
      reinterpret_cast<int16_t *>(converter.buf + converter.len_cvt));
  SDL_free(converter.buf);
  return !clip.empty();
}

bool load_jackhammer_audio(const SDL_AudioSpec &target) {
  return load_audio_clip(_binary__assets_jackhammer_wav_start,
                         embedded_size(_binary__assets_jackhammer_wav_start,
                                       _binary__assets_jackhammer_wav_end),
                         target, jackhammer_clip);
}

bool load_jackhammer_texture() {
  glGenTextures(1, &jackhammer_texture);
  glBindTexture(GL_TEXTURE_2D, jackhammer_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 180, 285, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, _binary__assets_jackhammer_rgba_start);
  return true;
}

bool load_skywalker_texture() {
  glGenTextures(1, &skywalker_texture);
  glBindTexture(GL_TEXTURE_2D, skywalker_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 473, 1524, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, _binary__assets_skywalker_rgba_start);
  return true;
}

bool load_water_splash_texture() {
  glGenTextures(1, &water_splash_texture);
  glBindTexture(GL_TEXTURE_2D, water_splash_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 453, 350, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, _binary__assets_water_splash_rgba_start);
  return true;
}
bool load_skywalker_audio(const SDL_AudioSpec &target) {
  return load_audio_clip(
      _binary__assets_skywalker_splash_wav_start,
      embedded_size(_binary__assets_skywalker_splash_wav_start,
                    _binary__assets_skywalker_splash_wav_end),
      target, skywalker_splash_clip);
}

bool load_skywalker_oooooh_audio(const SDL_AudioSpec &target) {
  return load_audio_clip(
      _binary__assets_skywalker_oooooh_wav_start,
      embedded_size(_binary__assets_skywalker_oooooh_wav_start,
                    _binary__assets_skywalker_oooooh_wav_end),
      target, skywalker_oooooh_clip);
}
size_t audio_batch(const int16_t *data, size_t frames);

void audio_sample(int16_t left, int16_t right) {
  const int16_t sample[2] = {left, right};
  audio_batch(sample, 1);
}
void input_poll() {}
int16_t input_state(unsigned, unsigned device, unsigned, unsigned id) {
  if (nds_active && (device == RETRO_DEVICE_POINTER ||
                     device == RETRO_DEVICE_MOUSE)) {
    int output_width = 1;
    int output_height = 1;
    SDL_Window *window = emulator_window;
    if (window) {
      SDL_GetWindowSize(window, &output_width, &output_height);
      int focused_mouse_x = 0;
      int focused_mouse_y = 0;
      const Uint32 mouse_buttons =
          SDL_GetMouseState(&focused_mouse_x, &focused_mouse_y);
      if (SDL_GetMouseFocus() == window) {
        mouse_x = focused_mouse_x;
        mouse_y = focused_mouse_y;
      }
      mouse_pressed = (mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    }
    if (device == RETRO_DEVICE_MOUSE && id == RETRO_DEVICE_ID_MOUSE_X)
      return static_cast<int16_t>(std::clamp(mouse_delta_x, -32768, 32767));
    if (device == RETRO_DEVICE_MOUSE && id == RETRO_DEVICE_ID_MOUSE_Y)
      return static_cast<int16_t>(std::clamp(mouse_delta_y, -32768, 32767));
    float touch_x = 0.0f;
    float touch_y = 0.0f;
    float touch_width = static_cast<float>(output_width);
    float touch_height = static_cast<float>(output_height);
    float pointer_source_y = 0.0f;
    float pointer_source_height = 1.0f;
    if (nds_overlay) {
      const float scale = std::min(
          static_cast<float>(output_width) / 1920.0f,
          static_cast<float>(output_height) / 1080.0f);
      const float overlay_x = (output_width - 1920.0f * scale) * 0.5f;
      const float overlay_y = (output_height - 1080.0f * scale) * 0.5f;
      if (nds_layout_index == 0) {
        touch_x = overlay_x + 605.0f * scale;
        touch_y = overlay_y + 545.0f * scale;
        touch_width = 710.0f * scale;
        touch_height = 530.0f * scale;
        pointer_source_y = 0.5f;
        pointer_source_height = 0.5f;
      } else {
        touch_x = overlay_x + 327.0f * scale;
        touch_y = overlay_y + 68.0f * scale;
        touch_width = 1257.0f * scale;
        touch_height = touch_width * 3.0f / 4.0f;
        pointer_source_y = nds_layout_index == 1 ? 0.0f : 0.5f;
        pointer_source_height = 0.5f;
      }
    } else if (frame_width && frame_height) {
      const float aspect = static_cast<float>(frame_width) / frame_height;
      touch_width = static_cast<float>(output_width);
      touch_height = touch_width / aspect;
      if (touch_height > output_height) {
        touch_height = static_cast<float>(output_height);
        touch_width = touch_height * aspect;
      }
      touch_x = (output_width - touch_width) * 0.5f;
      touch_y = (output_height - touch_height) * 0.5f;
    }
    if (id == RETRO_DEVICE_ID_POINTER_X) {
      const float local_x = std::clamp(
          static_cast<float>(mouse_x) - touch_x, 0.0f, touch_width);
      const float normalized =
          local_x * 65535.0f / std::max(1.0f, touch_width);
      return static_cast<int16_t>(
          std::clamp(normalized - 32768.0f, -32768.0f, 32767.0f));
    }
    if (id == RETRO_DEVICE_ID_POINTER_Y) {
      const float local_y = std::clamp(
          static_cast<float>(mouse_y) - touch_y, 0.0f, touch_height);
      const float normalized =
          (pointer_source_y +
           local_y * pointer_source_height /
               std::max(1.0f, touch_height)) * 65535.0f;
      return static_cast<int16_t>(
          std::clamp(normalized - 32768.0f, -32768.0f, 32767.0f));
    }
    if (id == RETRO_DEVICE_ID_POINTER_PRESSED ||
        (device == RETRO_DEVICE_MOUSE && id == RETRO_DEVICE_ID_MOUSE_LEFT))
      return mouse_pressed ? 1 : 0;
  }
  return (buttons & (1u << id)) ? 1 : 0;
}

InputMode active_input_mode() {
  return nds_active ? nds_input_mode : wii_active ? wii_input_mode
                                                   : gba_input_mode;
}

void toggle_active_input_mode() {
  InputMode mode;
  if (nds_active) {
    nds_input_mode = nds_input_mode == InputMode::Keyboard
        ? InputMode::Controller : InputMode::Keyboard;
    save_nds_controls();
    mode = nds_input_mode;
  } else if (wii_active) {
    wii_input_mode = wii_input_mode == InputMode::Keyboard
        ? InputMode::Controller : InputMode::Keyboard;
    save_wii_controls();
    mode = wii_input_mode;
  } else {
    gba_input_mode = gba_input_mode == InputMode::Keyboard
        ? InputMode::Controller : InputMode::Keyboard;
    save_controls();
    mode = gba_input_mode;
  }
  input_swap_notification = "SWAPPED INPUT TO " +
                            std::string(mode == InputMode::Keyboard
                                            ? "KEYBOARD" : "CONTROLLER");
  input_swap_notification_until = SDL_GetTicks() + 1800;
}

void update_controller_buttons() {
  if (!game_controller || active_input_mode() != InputMode::Controller)
    return;
  SDL_GameControllerUpdate();
  buttons = 0;
  const auto press = [&](SDL_GameControllerButton button, unsigned id) {
    if (SDL_GameControllerGetButton(game_controller, button) && id < 32)
      buttons |= 1u << id;
  };
  const auto press_axis = [&](SDL_GameControllerAxis axis, bool positive,
                              unsigned id) {
    const int value = SDL_GameControllerGetAxis(game_controller, axis);
    if (((positive && value > 16000) || (!positive && value < -16000)) &&
        id < 32)
      buttons |= 1u << id;
  };
  static constexpr unsigned gba_ids[] = {
      RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
      RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
      RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L};
  static constexpr unsigned extended_ids[] = {
      RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y,
      RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
      RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
      RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R};
  if (nds_active) {
    for (size_t i = 0; i < nds_controller_buttons.size(); ++i) {
      if (nds_controller_axis_bound[i])
        press_axis(nds_controller_axes[i], nds_controller_axis_positive[i],
                   extended_ids[i]);
      else
        press(nds_controller_buttons[i], extended_ids[i]);
    }
  } else if (wii_active) {
    for (size_t i = 0; i < wii_controller_buttons.size(); ++i) {
      if (wii_controller_axis_bound[i])
        press_axis(wii_controller_axes[i], wii_controller_axis_positive[i],
                   extended_ids[i]);
      else
        press(wii_controller_buttons[i], extended_ids[i]);
    }
  } else {
    for (size_t i = 0; i < gba_controller_buttons.size(); ++i) {
      if (gba_controller_axis_bound[i])
        press_axis(gba_controller_axes[i], gba_controller_axis_positive[i],
                   gba_ids[i]);
      else
        press(gba_controller_buttons[i], gba_ids[i]);
    }
  }
}

void set_button(SDL_Keycode key, bool pressed) {
  unsigned id = 32;
  if (nds_active) {
    static constexpr unsigned ids[] = {
        RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y,
        RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
        RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R};
    for (unsigned i = 0; i < nds_control_keys.size(); ++i) {
      if (nds_control_keys[i] == key) {
        id = ids[i];
        break;
      }
    }
  } else if (wii_active) {
    static constexpr unsigned ids[] = {
        RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_X, RETRO_DEVICE_ID_JOYPAD_Y,
        RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
        RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R};
    for (unsigned i = 0; i < wii_control_keys.size(); ++i) {
      if (wii_control_keys[i] == key) {
        id = ids[i];
        break;
      }
    }
  } else {
    for (unsigned i = 0; i < control_keys.size(); ++i) {
      if (control_keys[i] == key) {
        static constexpr unsigned ids[] = {
            RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B,
            RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START,
            RETRO_DEVICE_ID_JOYPAD_UP, RETRO_DEVICE_ID_JOYPAD_DOWN,
            RETRO_DEVICE_ID_JOYPAD_LEFT, RETRO_DEVICE_ID_JOYPAD_RIGHT,
            RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_L};
        id = ids[i];
        break;
      }
    }
  }
  if (id < 32) {
    if (pressed) buttons |= 1u << id;
    else buttons &= ~(1u << id);
  }
}

std::filesystem::path executable_directory() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD length = GetModuleFileNameW(
      nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0) return std::filesystem::current_path();
  buffer.resize(length);
  return std::filesystem::path(buffer).parent_path();
#else
  char buffer[4096]{};
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return std::filesystem::current_path();
  buffer[length] = '\0';
  return std::filesystem::path(buffer).parent_path();
#endif
}

void draw_round_rect(SDL_Renderer *renderer, SDL_Rect rect, int radius);
void draw_text(SDL_Renderer *renderer, const std::string &text, int x, int y,
               int scale, SDL_Color color);
void draw_text_centered(SDL_Renderer *renderer, const std::string &text,
                        int center_x, int y, int scale, SDL_Color color);

std::string choose_rom(SDL_Renderer *renderer, const std::string &extension = ".gba") {
  ScopedFlag dark_page(dark_page_rendering, true);
  const std::string system_folder =
      extension == ".nds" ? "NDS" :
      extension == ".3ds" ? "3DS" :
      extension == ".wii" ? "WII" :
      extension == ".gamecube" ? "GAMECUBE" : "GBA";
  const auto is_supported = [&](const std::filesystem::path &path) {
    const auto file_extension = path.extension().string();
    return extension == ".wii" || extension == ".gamecube"
        ? file_extension == ".iso" || file_extension == ".wbfs" ||
          file_extension == ".rvz" ||
          (extension == ".gamecube" && file_extension == ".gcm") ||
          (extension == ".gamecube" && file_extension == ".gcz")
        : extension == ".3ds"
            ? file_extension == ".3ds" || file_extension == ".cia" ||
              file_extension == ".cci" || file_extension == ".cxi"
            : file_extension == extension;
  };
  const std::filesystem::path rom_directory =
      executable_directory() / "ROMS" / system_folder;
  std::vector<std::filesystem::path> roms;
  std::error_code error;
  if (std::filesystem::exists(rom_directory, error)) {
    for (const auto &entry : std::filesystem::directory_iterator(rom_directory, error)) {
      if (entry.is_regular_file() && is_supported(entry.path()))
        roms.push_back(entry.path());
    }
  }
  std::sort(roms.begin(), roms.end());
  if (!roms.empty()) {
    size_t selected = 0;
    size_t marquee_selection = roms.size();
    Uint32 marquee_started = 0;
    for (;;) {
      if (marquee_selection != selected) {
        marquee_selection = selected;
        marquee_started = SDL_GetTicks();
      }
      SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
      SDL_RenderClear(renderer);
      draw_text_centered(renderer, "SELECT ROM", 400, 46, 4, {245, 183, 202, 255});
      const size_t first = selected > 6 ? selected - 6 : 0;
      const size_t last = std::min(roms.size(), first + 8);
      for (size_t i = first; i < last; ++i) {
        const int row_top = 126 + static_cast<int>(i - first) * 48;
        const int text_y = row_top + 12;
        SDL_Rect row{95, row_top, 610, 38};
        const bool item_selected = menu_selection_visible && i == selected;
        SDL_SetRenderDrawColor(renderer, item_selected ? 112 : 67,
                               item_selected ? 42 : 30,
                               item_selected ? 72 : 48, 255);
        draw_round_rect(renderer, row, 8);
        std::string label = roms[i].stem().string();
        for (char &character : label) {
          character = static_cast<char>(
              std::toupper(static_cast<unsigned char>(character)));
          if (!((character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == ' ' || character == '-' ||
                character == '(' || character == ')' ||
                character == ','))
            character = ' ';
        }
        constexpr int label_x = 115;
        constexpr int label_right = 685;
        constexpr int label_scale = 2;
        const int label_width =
            static_cast<int>(label.size()) * 6 * label_scale - label_scale;
        const int overflow = std::max(0, label_width - (label_right - label_x));
        int scroll = 0;
        if (item_selected && overflow > 0) {
          const int travel_ms = overflow * 18;
          constexpr int start_pause_ms = 700;
          constexpr int end_pause_ms = 1800;
          const int cycle_ms = start_pause_ms + travel_ms + end_pause_ms;
          const int phase = static_cast<int>(
              (SDL_GetTicks() - marquee_started) % cycle_ms);
          if (phase >= start_pause_ms && phase < start_pause_ms + travel_ms)
            scroll = (phase - start_pause_ms) / 18;
          else if (phase >= start_pause_ms + travel_ms)
            scroll = overflow;
        }
        SDL_Rect label_clip{label_x, row_top, label_right - label_x, row.h};
        SDL_RenderSetClipRect(renderer, &label_clip);
        draw_text(renderer, label, label_x - scroll, text_y, label_scale,
              item_selected ? SDL_Color{255, 225, 235, 255}
                            : SDL_Color{245, 205, 217, 255});
        SDL_RenderSetClipRect(renderer, nullptr);
      }
      draw_text(renderer, "UP DOWN SELECT   ESC BACK", 250, 755, 2,
                {190, 116, 148, 255});
      draw_easter_eggs_notice(renderer);
      SDL_RenderPresent(renderer);

      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
          quit_requested = true;
          return {};
        }
        const size_t first = selected > 6 ? selected - 6 : 0;
        const size_t last = std::min(roms.size(), first + 8);
        std::array<SDL_Rect, 8> rows{};
        size_t row_count = 0;
        for (size_t i = first; i < last; ++i)
          rows[row_count++] =
              SDL_Rect{95, 126 + static_cast<int>(i - first) * 48, 610, 38};
        int hovered_row = 0;
        if (mouse_menu_event(event, hovered_row, rows.data(), row_count)) {
          selected = first + static_cast<size_t>(hovered_row);
          if (event.type == SDL_MOUSEMOTION) continue;
        }
        translate_controller_event(event);
        if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
        if (event.key.keysym.sym == SDLK_ESCAPE) return {};
        if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_w)
          selected = selected == 0 ? roms.size() - 1 : selected - 1;
        else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s)
          selected = (selected + 1) % roms.size();
        else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE)
          return roms[selected].string();
      }
      SDL_Delay(16);
    }
  }
  const std::string directory = (rom_directory.string() + "/");
  std::error_code directory_error;
  std::filesystem::create_directories(rom_directory, directory_error);
  const std::string command =
      "zenity --file-selection --filename='" + directory +
      "' --title='Choose a ROM' "
      "--file-filter='" + (extension == ".nds" ? "NDS ROMs | *.nds" :
                          extension == ".wii"
                              ? "Wii ROMs | *.iso *.wbfs *.rvz"
                              : extension == ".gamecube"
                              ? "GameCube ROMs | *.iso *.gcm *.gcz *.wbfs *.rvz"
                              : extension == ".3ds"
                              ? "3DS ROMs | *.3ds *.cia *.cci *.cxi"
                              : "GBA ROMs | *.gba") + "' "
      "--file-filter='All files | *' 2>/dev/null";
  FILE *pipe = popen(command.c_str(), "r");
  if (!pipe) return {};
  char buffer[4096]{};
  if (!fgets(buffer, sizeof(buffer), pipe)) { pclose(pipe); return {}; }
  pclose(pipe);
  std::string path = buffer;
  while (!path.empty() && (path.back() == '\n' || path.back() == '\r')) path.pop_back();
  return path;
}

std::string glyph(char c) {
  switch (c) {
    case 'A': return "01110""10001""10001""11111""10001""10001""10001";
    case 'B': return "11110""10001""10001""11110""10001""10001""11110";
    case 'C': return "01111""10000""10000""10000""10000""10000""01111";
    case 'D': return "11110""10001""10001""10001""10001""10001""11110";
    case 'E': return "11111""10000""10000""11110""10000""10000""11111";
    case 'F': return "11111""10000""10000""11110""10000""10000""10000";
    case 'G': return "01111""10000""10000""10111""10001""10001""01111";
    case 'H': return "10001""10001""10001""11111""10001""10001""10001";
    case 'I': return "11111""00100""00100""00100""00100""00100""11111";
    case 'J': return "00111""00010""00010""00010""10010""10010""01100";
    case 'K': return "10001""10010""10100""11000""10100""10010""10001";
    case 'L': return "10000""10000""10000""10000""10000""10000""11111";
    case 'M': return "10001""11011""10101""10101""10001""10001""10001";
    case 'N': return "10001""11001""10101""10101""10011""10001""10001";
    case 'O': return "01110""10001""10001""10001""10001""10001""01110";
    case 'P': return "11110""10001""10001""11110""10000""10000""10000";
    case 'Q': return "01110""10001""10001""10001""10101""10010""01101";
    case 'R': return "11110""10001""10001""11110""10100""10010""10001";
    case 'S': return "01111""10000""10000""01110""00001""00001""11110";
    case 'T': return "11111""00100""00100""00100""00100""00100""00100";
    case 'U': return "10001""10001""10001""10001""10001""10001""01110";
    case 'V': return "10001""10001""10001""10001""10001""01010""00100";
    case 'W': return "10001""10001""10001""10101""10101""11011""10001";
    case 'Y': return "10001""10001""01010""00100""00100""00100""00100";
    case 'X': return "10001""10001""01010""00100""01010""10001""10001";
    case 'Z': return "11111""00001""00010""00100""01000""10000""11111";
    case '0': return "01110""10001""10011""10101""11001""10001""01110";
    case '1': return "00100""01100""00100""00100""00100""00100""01110";
    case '2': return "01110""10001""00001""00010""00100""01000""11111";
    case '3': return "11110""00001""00001""01110""00001""00001""11110";
    case '4': return "00010""00110""01010""10010""11111""00010""00010";
    case '5': return "11111""10000""10000""11110""00001""00001""11110";
    case '6': return "01110""10000""10000""11110""10001""10001""01110";
    case '7': return "11111""00001""00010""00100""01000""01000""01000";
    case '8': return "01110""10001""10001""01110""10001""10001""01110";
    case '9': return "01110""10001""10001""01111""00001""00001""01110";
    case ' ': return "00000""00000""00000""00000""00000""00000""00000";
    case ':': return "00000""00100""00100""00000""00100""00100""00000";
    case '-': return "00000""00000""00000""11111""00000""00000""00000";
    case '+': return "00000""00100""00100""11111""00100""00100""00000";
    case '.': return "00000""00000""00000""00000""00000""00110""00110";
    case ',': return "00000""00000""00000""00000""00110""00100""01000";
    case '(': return "00010""00100""01000""01000""01000""00100""00010";
    case ')': return "01000""00100""00010""00010""00010""00100""01000";
    default: return "00000""00000""00000""00000""00000""00000""00000";
  }
}

void draw_text(SDL_Renderer *renderer, const std::string &text, int x, int y,
               int scale, SDL_Color color) {
  color = theme_text(color);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  for (char c : text) {
    const std::string pattern = glyph(c);
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col)
        if (pattern[row * 5 + col] == '1') {
          SDL_Rect pixel{x + col * scale, y + row * scale, scale, scale};
          SDL_RenderFillRect(renderer, &pixel);
        }
    x += 6 * scale;
  }
}

void draw_text_centered(SDL_Renderer *renderer, const std::string &text,
                        int center_x, int y, int scale, SDL_Color color) {
  const int width = static_cast<int>(text.size()) * 6 * scale - scale;
  draw_text(renderer, text, center_x - width / 2, y, scale, color);
}

void draw_text_centered_mid_scale(SDL_Renderer *renderer,
                                  const std::string &text, int center_x,
                                  int y, SDL_Color color) {
  constexpr int scale_numerator = 7;
  constexpr int scale_denominator = 2;
  color = theme_text(color);
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const int width = static_cast<int>(
      text.size() * 6 * scale_numerator / scale_denominator -
      scale_numerator / scale_denominator);
  int x = center_x - width / 2;
  for (char c : text) {
    const std::string pattern = glyph(c);
    for (int row = 0; row < 7; ++row)
      for (int col = 0; col < 5; ++col)
        if (pattern[row * 5 + col] == '1') {
          const int left = x + col * scale_numerator / scale_denominator;
          const int top = y + row * scale_numerator / scale_denominator;
          const int right = x + (col + 1) * scale_numerator / scale_denominator;
          const int bottom = y + (row + 1) * scale_numerator / scale_denominator;
          SDL_Rect pixel{left, top, right - left, bottom - top};
          SDL_RenderFillRect(renderer, &pixel);
        }
    x += 6 * scale_numerator / scale_denominator;
  }
}

void draw_gl_text(const std::string &text, int x, int y, int scale,
                  float red, float green, float blue) {
  glColor3f(red, green, blue);
  glBegin(GL_QUADS);
  for (char c : text) {
    const std::string pattern = glyph(c);
    for (int row = 0; row < 7; ++row) {
      for (int col = 0; col < 5; ++col) {
        if (pattern[row * 5 + col] != '1') continue;
        const float left = static_cast<float>(x + col * scale);
        const float top = static_cast<float>(y + row * scale);
        glVertex2f(left, top);
        glVertex2f(left + scale, top);
        glVertex2f(left + scale, top + scale);
        glVertex2f(left, top + scale);
      }
    }
    x += 6 * scale;
  }
  glEnd();
}

struct CookedPopup {
  bool active = false;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int close_x = 0;
  int close_y = 0;
  size_t kind = 0;
  size_t correct_option = 0;
  bool warning = false;
  int hovered_option = -1;
  std::array<size_t, 3> option_order{0, 1, 2};
};

static constexpr const char *cooked_titles[] = {
    "FREE ROBUX", "URGENT SYSTEM ALERT", "YOU WON NOTHING",
    "HOT GIRDS NEAR YOU", "YOUR SAVE IS COOKED", "MYSTERY GIFT",
    "VIRUS DETECTED", "PREMIUM ACCESS"};
static constexpr const char *cooked_options[][3] = {
    {"CLAIM", "CLOSE", "ACCEPT"},
    {"OK", "INSTALL", "UPGRADE"},
    {"CLOSE", "TRY AGAIN", "PAY NOW"},
    {"VIEW", "ACCEPT", "NO THANKS"},
    {"CLOSE", "DELETE SAVE", "PANIC"},
    {"OPEN", "SUBSCRIBE", "CLOSE"},
    {"CLOSE", "SCAN NOW", "DELETE"},
    {"CANCEL", "UPGRADE", "SUBSCRIBE"}};
static constexpr size_t cooked_close_options[] = {1, 0, 0, 2, 0, 2, 0, 0};
static constexpr const char *cooked_warning_titles[] = {
    "ROBUX CLAIM PORTAL", "SECURITY RESPONSE CENTER",
    "CONGRATULATIONS DEPARTMENT", "PRIVATE VIEWER ACCESS",
    "SAVE RECOVERY SERVICE", "MYSTERY REWARD DESK",
    "THREAT REMOVAL CONSOLE", "PREMIUM MEMBER GATE"};
static constexpr const char *cooked_warning_lines[] = {
    "VERIFY YOUR PLAYER ACCOUNT TO RECEIVE 8000 ROBUX",
    "YOUR DEVICE NEEDS AN EMERGENCY SECURITY SUBSCRIPTION",
    "YOU HAVE BEEN SELECTED FOR A LIMITED REWARD",
    "CONFIRM YOUR AGE TO UNLOCK THE PRIVATE CHANNEL",
    "UPLOAD YOUR SAVE TO RESTORE YOUR PROGRESS",
    "ENTER YOUR DETAILS TO REVEAL THE MYSTERY PRIZE",
    "AUTHORIZE THE CLEANUP TO REMOVE THE DETECTED THREAT",
    "UPGRADE YOUR ACCOUNT TO CONTINUE WITHOUT LIMITS"};

int cooked_option_width(const char *label) {
  return std::max(124, static_cast<int>(std::strlen(label)) * 12 + 20);
}

void draw_cooked_popup(const CookedPopup &popup, int output_width,
                       int output_height, bool fullscreen = false) {
  if (!popup.active) return;
  const int warning_margin = fullscreen ? 40 : 0;
  const int popup_x = fullscreen ? warning_margin : popup.x;
  const int popup_y = fullscreen ? warning_margin : popup.y;
  const int popup_width = fullscreen ? output_width - warning_margin * 2 :
                                      popup.width;
  const int popup_height = fullscreen ? output_height - warning_margin * 2 :
                                       popup.height;
  const GLboolean texture_was_enabled = glIsEnabled(GL_TEXTURE_2D);
  const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_DEPTH_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(fullscreen ? 0.44f : 0.24f, fullscreen ? 0.05f : 0.08f,
            fullscreen ? 0.05f : 0.34f, 1.0f);
  glBegin(GL_QUADS);
  glVertex2f(popup_x, popup_y);
  glVertex2f(popup_x + popup_width, popup_y);
  glVertex2f(popup_x + popup_width, popup_y + popup_height);
  glVertex2f(popup_x, popup_y + popup_height);
  glEnd();
  if (fullscreen) {
    glColor4f(0.95f, 0.16f, 0.08f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(popup_x + 24, popup_y + 24);
    glVertex2f(popup_x + popup_width - 24, popup_y + 82);
    glVertex2f(popup_x + popup_width - 24, popup_y + 24);
    glVertex2f(popup_x + 24, popup_y + 82);
    glEnd();
    glColor4f(0.12f, 0.02f, 0.02f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(popup_x + 54, popup_y + 132);
    glVertex2f(popup_x + popup_width - 54, popup_y + 132);
    glVertex2f(popup_x + popup_width - 54, popup_y + 220);
    glVertex2f(popup_x + 54, popup_y + 220);
    glEnd();
    glColor4f(0.82f, 0.68f, 0.08f, 1.0f);
    glBegin(GL_LINE_LOOP);
    glVertex2f(popup_x + 54, popup_y + 250);
    glVertex2f(popup_x + popup_width - 54, popup_y + 250);
    glVertex2f(popup_x + popup_width - 54, popup_y + 318);
    glVertex2f(popup_x + 54, popup_y + 318);
    glEnd();
    draw_gl_text(cooked_warning_titles[popup.kind], popup_x + 48, popup_y + 42,
                 3,
                 1.0f, 1.0f, 1.0f);
    draw_gl_text(cooked_warning_lines[popup.kind], popup_x + 72,
                 popup_y + 154, 3, 1.0f, 0.3f, 0.2f);
    draw_gl_text("SECURE YOUR BENEFITS BEFORE THE TIMER ENDS",
                 popup_x + 72, popup_y + 270, 3, 1.0f, 0.86f, 0.24f);
    draw_gl_text("EMAIL ______________________________",
                 popup_x + 72, popup_y + 390, 3, 0.95f, 0.95f, 0.95f);
    draw_gl_text("PASSWORD ___________________________",
                 popup_x + 72, popup_y + 450, 3, 0.95f, 0.95f, 0.95f);
    draw_gl_text("SUBSCRIBE    VERIFY    CLAIM REWARD",
                 popup_x + 72, popup_y + 540, 3, 0.35f, 1.0f, 0.45f);
    draw_gl_text("THIS OFFER EXPIRES IN 00:02",
                 popup_x + 72, popup_y + 620, 3, 1.0f, 0.35f, 0.2f);
    draw_gl_text("LOGO    TRUSTED    PREMIUM    FREE",
                 popup_x + 72, popup_y + 700, 3, 0.72f, 0.72f, 0.78f);
    glDisable(GL_BLEND);
    if (texture_was_enabled)
      glEnable(GL_TEXTURE_2D);
    if (depth_was_enabled)
      glEnable(GL_DEPTH_TEST);
    return;
  }
  glColor4f(1.0f, 0.82f, 0.1f, 1.0f);
  glBegin(GL_LINE_LOOP);
  glVertex2f(popup_x, popup_y);
  glVertex2f(popup_x + popup_width, popup_y);
  glVertex2f(popup_x + popup_width, popup_y + popup_height);
  glVertex2f(popup_x, popup_y + popup_height);
  glEnd();
  static constexpr const char *messages[] = {
      "CHOOSE AN OPTION", "CHOOSE AN OPTION", "CHOOSE AN OPTION",
      "CHOOSE AN OPTION", "CHOOSE AN OPTION", "CHOOSE AN OPTION",
      "CHOOSE AN OPTION", "CHOOSE AN OPTION"};
  draw_gl_text(cooked_titles[popup.kind], popup_x + 18, popup_y + 22, 3,
               1.0f, 0.82f, 0.1f);
  draw_gl_text(messages[popup.kind], popup_x + 18, popup_y + 72, 2,
               0.95f, 0.75f, 0.88f);
  const int option_width = std::max(
      {cooked_option_width(cooked_options[popup.kind][0]),
       cooked_option_width(cooked_options[popup.kind][1]),
       cooked_option_width(cooked_options[popup.kind][2])});
  const int option_spacing = option_width + 11;
  const int option_y = popup_y + popup_height - 52;
  for (size_t option = 0; option < 3; ++option) {
    const int option_x = popup_x + 18 +
                         static_cast<int>(option) * option_spacing;
    glColor4f(0.82f, 0.08f, 0.18f, 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(option_x, option_y);
    glVertex2f(option_x + option_width, option_y);
    glVertex2f(option_x + option_width, option_y + 28);
    glVertex2f(option_x, option_y + 28);
    glEnd();
    draw_gl_text(cooked_options[popup.kind][popup.option_order[option]],
                 option_x + 10, option_y + 7, 2,
                 1.0f, 1.0f, 1.0f);
  }
  glDisable(GL_BLEND);
  if (texture_was_enabled)
    glEnable(GL_TEXTURE_2D);
  if (depth_was_enabled)
    glEnable(GL_DEPTH_TEST);
}

void draw_blossom(SDL_Renderer *renderer, int x, int y, int size) {
  SDL_SetRenderDrawColor(renderer, edgerunners_theme() ? 234 :
                         berserk_theme() ? 255 : 248,
                         edgerunners_theme() ? 255 :
                         berserk_theme() ? 255 : 174,
                         edgerunners_theme() ? 0 :
                         berserk_theme() ? 255 : 202, 255);
  for (int i = 0; i < 5; ++i) {
    const int px = x + static_cast<int>(std::cos(i * 1.2566) * size * .55);
    const int py = y + static_cast<int>(std::sin(i * 1.2566) * size * .55);
    SDL_Rect petal{px - size / 2, py - size / 2, size, size};
    SDL_RenderFillRect(renderer, &petal);
  }
  SDL_SetRenderDrawColor(renderer, edgerunners_theme() ? 24 :
                         berserk_theme() ? 6 : 255,
                         edgerunners_theme() ? 25 :
                         berserk_theme() ? 24 : 224,
                         edgerunners_theme() ? 35 :
                         berserk_theme() ? 63 : 139, 255);
  SDL_Rect center{x - size / 3, y - size / 3, size * 2 / 3, size * 2 / 3};
  SDL_RenderFillRect(renderer, &center);
}

void draw_frontend_background(SDL_Renderer *renderer, SDL_Texture *background) {
  if (background) {
    SDL_SetTextureBlendMode(background, SDL_BLENDMODE_NONE);
    int texture_width = 0;
    int texture_height = 0;
    SDL_QueryTexture(background, nullptr, nullptr, &texture_width,
                     &texture_height);
    double scale = std::max(static_cast<double>(FRONTEND_WIDTH) / texture_width,
                            static_cast<double>(FRONTEND_HEIGHT) / texture_height);
    if (edgerunners_theme())
      scale *= 1.15;
    else if (!berserk_theme())
      scale *= 1.0;
    const int width = static_cast<int>(texture_width * scale);
    const int height = static_cast<int>(texture_height * scale);
    const SDL_Rect destination{(FRONTEND_WIDTH - width) / 2,
                               FRONTEND_HEIGHT - height,
                               width, height};
    SDL_SetRenderDrawColor(renderer, edgerunners_theme() ? 0 :
                           berserk_theme() ? 165 : 35,
                           edgerunners_theme() ? 0 :
                           berserk_theme() ? 22 : 18,
                           edgerunners_theme() ? 0 :
                           berserk_theme() ? 69 : 40, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, background, nullptr, &destination);
    return;
  }

  SDL_SetRenderDrawColor(renderer, edgerunners_theme() ? 0 :
                         berserk_theme() ? 165 : 35,
                         edgerunners_theme() ? 0 :
                         berserk_theme() ? 22 : 18,
                         edgerunners_theme() ? 0 :
                         berserk_theme() ? 69 : 40, 255);
  SDL_RenderClear(renderer);

  SDL_SetRenderDrawColor(renderer, 57, 27, 55, 255);
  SDL_Rect horizon{0, FRONTEND_HEIGHT / 2, FRONTEND_WIDTH,
                   FRONTEND_HEIGHT / 2};
  SDL_RenderFillRect(renderer, &horizon);

  SDL_SetRenderDrawColor(renderer, 83, 38, 69, 255);
  for (int y = 18; y < 250; y += 26) {
    for (int x = (y / 26 % 2) * 18 - 30; x < FRONTEND_WIDTH + 30; x += 72) {
      SDL_Rect blossom{x, y, 42, 18};
      SDL_RenderFillRect(renderer, &blossom);
      SDL_Rect petal{x + 10, y - 8, 18, 34};
      SDL_RenderFillRect(renderer, &petal);
    }
  }

  SDL_SetRenderDrawColor(renderer, 25, 13, 31, 255);
  SDL_RenderDrawLine(renderer, 0, 86, 230, 18);
  SDL_RenderDrawLine(renderer, 0, 145, 190, 65);
  SDL_RenderDrawLine(renderer, 800, 82, 570, 20);
  SDL_RenderDrawLine(renderer, 800, 150, 610, 75);
  SDL_RenderDrawLine(renderer, 0, 270, 170, 190);
  SDL_RenderDrawLine(renderer, 800, 270, 630, 190);

  SDL_SetRenderDrawColor(renderer, 75, 29, 58, 255);
  SDL_Rect gate_top{125, 175, 550, 24};
  SDL_Rect gate_top_cap{105, 163, 590, 12};
  SDL_Rect gate_left{170, 175, 30, 260};
  SDL_Rect gate_right{600, 175, 30, 260};
  SDL_Rect gate_cross{190, 260, 420, 18};
  SDL_RenderFillRect(renderer, &gate_top);
  SDL_RenderFillRect(renderer, &gate_top_cap);
  SDL_RenderFillRect(renderer, &gate_left);
  SDL_RenderFillRect(renderer, &gate_right);
  SDL_RenderFillRect(renderer, &gate_cross);

  SDL_SetRenderDrawColor(renderer, 104, 40, 70, 255);
  SDL_Rect path{300, 430, 200, 170};
  SDL_RenderFillRect(renderer, &path);
  for (int y = 450; y < 600; y += 26) {
    const int inset = (y - 430) / 3;
    SDL_RenderDrawLine(renderer, 300 + inset, y, 500 - inset, y);
  }

  SDL_SetRenderDrawColor(renderer, 104, 45, 69, 255);
  for (int i = 0; i < 9; ++i) {
    const int x = 22 + i * 95;
    const int y = 350 + (i % 3) * 34;
    SDL_Rect cord{x, y, 2, 120};
    SDL_RenderFillRect(renderer, &cord);
    SDL_Rect lantern{x - 13, y + 18, 28, 45};
    SDL_RenderFillRect(renderer, &lantern);
    SDL_SetRenderDrawColor(renderer, 255, 133, 116, 255);
    SDL_Rect light{x - 8, y + 24, 18, 32};
    SDL_RenderFillRect(renderer, &light);
    SDL_SetRenderDrawColor(renderer, 104, 45, 69, 255);
  }

  SDL_SetRenderDrawColor(renderer, 210, 98, 119, 255);
  for (int i = 0; i < 22; ++i) {
    const int x = (i * 83 + 31) % 790;
    const int y = 30 + (i * 47) % 520;
    SDL_Rect petal{x, y, 6 + i % 4, 5 + i % 3};
    SDL_RenderFillRect(renderer, &petal);
  }
}

void draw_easter_eggs_notice(SDL_Renderer *renderer) {
  if (SDL_GetTicks() >= easter_eggs_notice_until) return;
  const SDL_Color color{175, 245, 195, 255};
  const char *message = easter_eggs_notice_kind == 1
      ? "ALL EASTER EGGS ENABLED" : "ALL EASTER EGGS DISABLED";
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(renderer, 28, 76, 48, 235);
  SDL_Rect background{180, 665, 440, 42};
  SDL_RenderFillRect(renderer, &background);
  draw_text_centered(renderer, message, 400, 676, 2, color);
}

void draw_round_rect(SDL_Renderer *renderer, SDL_Rect rect, int radius) {
  radius = std::min(radius, std::min(rect.w, rect.h) / 2);
  SDL_Rect middle{rect.x + radius, rect.y, rect.w - 2 * radius, rect.h};
  SDL_Rect body{rect.x, rect.y + radius, rect.w, rect.h - 2 * radius};
  SDL_RenderFillRect(renderer, &middle);
  SDL_RenderFillRect(renderer, &body);
  for (int y = 0; y < radius; ++y) {
    const int inset = radius - static_cast<int>(
        std::sqrt(radius * radius - (radius - y) * (radius - y)));
    SDL_RenderDrawLine(renderer, rect.x + inset, rect.y + y,
                       rect.x + rect.w - inset - 1, rect.y + y);
    SDL_RenderDrawLine(renderer, rect.x + inset, rect.y + rect.h - y - 1,
                       rect.x + rect.w - inset - 1, rect.y + rect.h - y - 1);
  }
}

void draw_glow_rect(SDL_Renderer *renderer, SDL_Rect rect, int radius) {
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
  for (int spread = 16; spread >= 2; spread -= 2) {
    SDL_SetRenderDrawColor(renderer, edgerunners_theme() ? 234 :
                           berserk_theme() ? 255 : 244,
                           edgerunners_theme() ? 255 :
                           berserk_theme() ? 255 : 133,
                           edgerunners_theme() ? 0 :
                           berserk_theme() ? 255 : 180,
                           static_cast<uint8_t>(2 + (16 - spread) / 2));
    SDL_Rect glow{rect.x - spread, rect.y - spread,
                  rect.w + spread * 2, rect.h + spread * 2};
    draw_round_rect(renderer, glow, radius + spread / 2);
  }
  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

std::string key_name(SDL_Keycode key) {
  if (key >= SDLK_a && key <= SDLK_z)
    return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(key))));
  if (key == SDLK_RETURN) return "ENTER";
  if (key == SDLK_BACKSPACE) return "BACKSPACE";
  if (key == SDLK_UP) return "UP";
  if (key == SDLK_DOWN) return "DOWN";
  if (key == SDLK_LEFT) return "LEFT";
  if (key == SDLK_RIGHT) return "RIGHT";
  if (key == SDLK_TAB) return "TAB";
  if (key == SDLK_SPACE) return "SPACE";
  return "KEY";
}

std::string controller_button_name(SDL_GameControllerButton button) {
  switch (button) {
    case SDL_CONTROLLER_BUTTON_A: return "A";
    case SDL_CONTROLLER_BUTTON_B: return "B";
    case SDL_CONTROLLER_BUTTON_X: return "X";
    case SDL_CONTROLLER_BUTTON_Y: return "Y";
    case SDL_CONTROLLER_BUTTON_BACK: return "BACK";
    case SDL_CONTROLLER_BUTTON_GUIDE: return "GUIDE";
    case SDL_CONTROLLER_BUTTON_START: return "START";
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "L STICK";
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "R STICK";
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "L BUMPER";
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "R BUMPER";
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-PAD UP";
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-PAD DOWN";
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-PAD LEFT";
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-PAD RIGHT";
    default: return "BUTTON";
  }
}

std::string controller_axis_name(SDL_GameControllerAxis axis, bool positive) {
  switch (axis) {
    case SDL_CONTROLLER_AXIS_LEFTX: return positive ? "L STICK RIGHT" : "L STICK LEFT";
    case SDL_CONTROLLER_AXIS_LEFTY: return positive ? "L STICK DOWN" : "L STICK UP";
    case SDL_CONTROLLER_AXIS_RIGHTX: return positive ? "R STICK RIGHT" : "R STICK LEFT";
    case SDL_CONTROLLER_AXIS_RIGHTY: return positive ? "R STICK DOWN" : "R STICK UP";
    case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "LT";
    case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "RT";
    default: return "AXIS";
  }
}

void controls_menu(SDL_Renderer *renderer) {
  mouse_hover_selection = false;
  last_control_first = 0;
  unsigned selected = 0;
  bool waiting = false;
  bool renaming = false;
  std::string profile_edit;
  bool saved = false;
  Uint32 saved_until = 0;
  constexpr unsigned input_device_index = control_keys.size();
  constexpr unsigned gba_turbo_hold_index = input_device_index + 1;
  constexpr unsigned gba_turbo_toggle_index = gba_turbo_hold_index + 1;
  constexpr unsigned option_count = gba_turbo_toggle_index + 1;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      SDL_GameControllerButton controller_button{};
      const bool binding_controller =
          controller_button_event(event, controller_button);
      SDL_GameControllerAxis controller_axis{};
      bool controller_axis_positive = false;
      const bool binding_axis =
          controller_axis_event(event, controller_axis, controller_axis_positive);
      if (waiting && binding_controller) {
        if (selected < input_device_index) {
          gba_controller_buttons[selected] = controller_button;
          gba_controller_axis_bound[selected] = false;
        }
        save_controls();
        waiting = false;
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      if (waiting && binding_axis && selected < input_device_index) {
        gba_controller_axes[selected] = controller_axis;
        gba_controller_axis_positive[selected] = controller_axis_positive;
        gba_controller_axis_bound[selected] = true;
        save_controls();
        waiting = false;
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      translate_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return;
      }
      if (renaming && event.type == SDL_TEXTINPUT) {
        append_profile_text(profile_edit, event.text.text);
        continue;
      }
      if (!waiting && !renaming) {
        if (event.type == SDL_MOUSEWHEEL &&
            mouse_over_control_list(last_control_first, option_count)) {
          last_control_first = std::clamp(
              last_control_first - event.wheel.y, 0,
              std::max(0, static_cast<int>(option_count) - 10));
          mouse_hover_selection = true;
          update_control_mouse_hover(
              selected, last_control_first,
              std::min(10, static_cast<int>(option_count) - last_control_first));
          continue;
        }
        const int first = mouse_hover_selection ? last_control_first :
                          selected > 8 ? static_cast<int>(selected) - 8 : 0;
        last_control_first = first;
        const int visible_rows = std::min(
            10, static_cast<int>(option_count) - first);
        const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
        std::array<SDL_Rect, 10> rows{};
        for (int row = 0; row < visible_rows; ++row)
          rows[row] = SDL_Rect{170, controls_top + row * 34 - 7, 460, 28};
        int hovered_row = 0;
        if (mouse_menu_event(event, hovered_row, rows.data(), visible_rows)) {
          selected = static_cast<unsigned>(first + hovered_row);
          if (event.type == SDL_MOUSEMOTION) continue;
        }
      }
      if (!waiting && !renaming && mouse_footer_back(event)) return;
      if (event.type != SDL_KEYDOWN) continue;
      if (renaming) {
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          renaming = false;
          SDL_StopTextInput();
        } else if (event.key.keysym.sym == SDLK_BACKSPACE) {
          if (!profile_edit.empty()) profile_edit.pop_back();
        } else if (event.key.keysym.sym == SDLK_RETURN) {
          if (rename_profile(ProfileKind::Gba, profile_edit)) {
            saved = true;
            saved_until = SDL_GetTicks() + 1200;
          }
          renaming = false;
          SDL_StopTextInput();
        }
        continue;
      }
      if (!event.key.repeat && event.key.keysym.sym == SDLK_F5) {
        gba_input_mode = gba_input_mode == InputMode::Keyboard
            ? InputMode::Controller : InputMode::Keyboard;
        save_controls();
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      if (!waiting && !event.key.repeat && event.key.keysym.sym == SDLK_c) {
        const auto files = profile_files(ProfileKind::Gba);
        if (!files.empty()) {
          const auto active = active_profile_name(ProfileKind::Gba);
          size_t index = 0;
          for (; index < files.size(); ++index)
            if (profile_name(files[index]) == active) break;
          const Theme theme = active_theme;
          load_profile(ProfileKind::Gba, (index + 1) % files.size());
          load_controls();
          active_theme = theme;
          save_controls();
        }
        continue;
      }
      if (!waiting && !event.key.repeat && event.key.keysym.sym == SDLK_v) {
        save_profile(ProfileKind::Gba);
        continue;
      }
      if (!waiting && !event.key.repeat && event.key.keysym.sym == SDLK_n) {
        create_profile(ProfileKind::Gba);
        continue;
      }
      if (!waiting && !event.key.repeat && event.key.keysym.sym == SDLK_d) {
        const Theme theme = active_theme;
        delete_profile(ProfileKind::Gba);
        load_controls();
        active_theme = theme;
        save_controls();
        continue;
      }
      if (!waiting && !event.key.repeat && event.key.keysym.sym == SDLK_r) {
        renaming = true;
        profile_edit.clear();
        SDL_StartTextInput();
        continue;
      }
      if (waiting) {
        if (event.key.keysym.sym == SDLK_ESCAPE) waiting = false;
        else {
          if (selected == gba_turbo_hold_index) turbo_key = event.key.keysym.sym;
          else if (selected == gba_turbo_toggle_index) {
            waiting = false;
            continue;
          }
          else if (selected < input_device_index) {
            control_keys[selected] = event.key.keysym.sym;
            gba_controller_axis_bound[selected] = false;
          }
          save_controls();
          waiting = false;
          saved = true;
          saved_until = SDL_GetTicks() + 1200;
        }
      } else if (event.key.keysym.sym == SDLK_UP ||
                 event.key.keysym.sym == SDLK_w ||
                 event.key.keysym.sym == SDLK_a) {
        selected = (selected + option_count - 1) % option_count;
        if (selected == gba_turbo_toggle_index)
          selected = gba_turbo_hold_index;
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s ||
                 event.key.keysym.sym == SDLK_d) {
        selected = (selected + 1) % option_count;
        if (selected == gba_turbo_toggle_index)
          selected = 0;
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        if (selected == input_device_index) {
          gba_input_mode = gba_input_mode == InputMode::Keyboard
              ? InputMode::Controller : InputMode::Keyboard;
          save_controls();
        } else if (selected != gba_turbo_toggle_index) waiting = true;
      } else if (event.key.keysym.sym == SDLK_ESCAPE) return;
    }
    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
    SDL_RenderClear(renderer);
    draw_text(renderer, "CONTROLS", 295, 55, 5, {245, 183, 202, 255});
    const int first = mouse_hover_selection ? last_control_first :
                      selected > 8 ? static_cast<int>(selected) - 8 : 0;
    last_control_first = first;
    const int visible_rows = std::min(
        10, static_cast<int>(option_count) - first);
    const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
    for (int row = 0; row < 10 && first + row < static_cast<int>(option_count);
         ++row) {
      const unsigned index = static_cast<unsigned>(first + row);
      const int y = controls_top + row * 34;
      if (menu_selection_visible && index == selected) {
        SDL_SetRenderDrawColor(renderer, 112, 42, 72, 255);
        SDL_Rect highlight{170, y - 7, 460, 28};
        SDL_RenderFillRect(renderer, &highlight);
      }
      const bool input_device = index == input_device_index;
      const bool turbo_hold = index == gba_turbo_hold_index;
      const bool turbo_toggle = index == gba_turbo_toggle_index;
      const char *name = turbo_hold ? "TURBO SPEED HOLD" :
                         input_device ? "INPUT DEVICE" :
                         turbo_toggle ? "TURBO SPEED TOGGLE" :
                         control_names[index];
      draw_text(renderer, name, 190, y, 2,
                menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                  : SDL_Color{220, 166, 187, 255});
      if (input_device) {
        draw_text(renderer, gba_input_mode == InputMode::Controller
                      ? "CONTROLLER" : "KEYBOARD", 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      } else if (turbo_toggle) {
        draw_text(renderer, "SHIFT+" + key_name(turbo_key), 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      } else {
        const std::string binding = waiting && selected == index
            ? "PRESS KEY OR BUTTON"
            : gba_input_mode == InputMode::Controller && !turbo_hold
                ? gba_controller_axis_bound[index]
                    ? controller_axis_name(gba_controller_axes[index],
                                            gba_controller_axis_positive[index])
                    : controller_button_name(gba_controller_buttons[index])
                : key_name(turbo_hold ? turbo_key : control_keys[index]);
        draw_text(renderer, binding, 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      }
    }
    draw_text_centered(renderer, "F1 CRT TOGGLE", 400, 700, 2,
                       {190, 116, 148, 255});
    draw_text_centered(renderer, "F2 FPS   F3 VSYNC   F5 INPUT SWAP",
                       400, 730, 2,
                       {190, 116, 148, 255});
    draw_text_centered(renderer, renaming ? "RENAME PROFILE: " + profile_edit
                                         : "PROFILE: " + active_profile_name(ProfileKind::Gba),
                       400, 630, 2, {190, 116, 148, 255});
    draw_text_centered(renderer,
                       "C CHANGE   V SAVE   N NEW   R RENAME   D DELETE",
                       400, 660, 2, {190, 116, 148, 255});
    if (saved && SDL_GetTicks() < saved_until)
      draw_text_centered(renderer, "SAVED", 400, 590, 2,
                         {175, 245, 195, 255});
    else
      saved = false;
    draw_easter_eggs_notice(renderer);
    draw_easter_eggs_notice(renderer);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

void nds_controls_menu(SDL_Renderer *renderer) {
  mouse_hover_selection = false;
  last_control_first = 0;
  unsigned selected = 0;
  bool waiting = false;
  bool renaming = false;
  std::string profile_edit;
  bool saved = false;
  Uint32 saved_until = 0;
  constexpr unsigned nds_input_device_index = nds_control_keys.size();
  constexpr unsigned nds_turbo_hold_index = nds_input_device_index + 1;
  constexpr unsigned nds_turbo_toggle_index = nds_turbo_hold_index + 1;
  constexpr unsigned option_count = nds_turbo_toggle_index + 1;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      SDL_GameControllerButton controller_button{};
      const bool binding_controller =
          controller_button_event(event, controller_button);
      SDL_GameControllerAxis controller_axis{};
      bool controller_axis_positive = false;
      const bool binding_axis =
          controller_axis_event(event, controller_axis, controller_axis_positive);
      if (waiting && binding_controller) {
        if (selected < nds_input_device_index) {
          nds_controller_buttons[selected] = controller_button;
          nds_controller_axis_bound[selected] = false;
        }
        save_nds_controls();
        waiting = false;
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      if (waiting && binding_axis && selected < nds_input_device_index) {
        nds_controller_axes[selected] = controller_axis;
        nds_controller_axis_positive[selected] = controller_axis_positive;
        nds_controller_axis_bound[selected] = true;
        save_nds_controls();
        waiting = false;
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      translate_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return;
      }
      if (renaming && event.type == SDL_TEXTINPUT) {
        append_profile_text(profile_edit, event.text.text);
        continue;
      }
      if (!waiting && !renaming) {
        if (event.type == SDL_MOUSEWHEEL &&
            mouse_over_control_list(last_control_first, option_count)) {
          last_control_first = std::clamp(
              last_control_first - event.wheel.y, 0,
              std::max(0, static_cast<int>(option_count) - 10));
          mouse_hover_selection = true;
          update_control_mouse_hover(
              selected, last_control_first,
              std::min(10, static_cast<int>(option_count) - last_control_first));
          continue;
        }
        const int first = mouse_hover_selection ? last_control_first :
                          selected > 8 ? static_cast<int>(selected) - 8 : 0;
        last_control_first = first;
        const int visible_rows = std::min(
            10, static_cast<int>(option_count) - first);
        const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
        std::array<SDL_Rect, 10> rows{};
        for (int row = 0; row < visible_rows; ++row)
          rows[row] = SDL_Rect{170, controls_top + row * 34 - 7, 460, 28};
        int hovered_row = 0;
        if (mouse_menu_event(event, hovered_row, rows.data(), visible_rows)) {
          selected = static_cast<unsigned>(first + hovered_row);
          if (event.type == SDL_MOUSEMOTION) continue;
        }
      }
      if (!waiting && !renaming && mouse_footer_back(event)) return;
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (renaming) {
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          renaming = false;
          SDL_StopTextInput();
        } else if (event.key.keysym.sym == SDLK_BACKSPACE) {
          if (!profile_edit.empty()) profile_edit.pop_back();
        } else if (event.key.keysym.sym == SDLK_RETURN) {
          if (rename_profile(ProfileKind::Nds, profile_edit)) {
            saved = true;
            saved_until = SDL_GetTicks() + 1200;
          }
          renaming = false;
          SDL_StopTextInput();
        }
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_r) {
        renaming = true;
        profile_edit.clear();
        SDL_StartTextInput();
        continue;
      }
      if (event.key.keysym.sym == SDLK_F5) {
        nds_input_mode = nds_input_mode == InputMode::Keyboard
            ? InputMode::Controller : InputMode::Keyboard;
        save_nds_controls();
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_c) {
        const auto files = profile_files(ProfileKind::Nds);
        if (!files.empty()) {
          const auto active = active_profile_name(ProfileKind::Nds);
          size_t index = 0;
          for (; index < files.size(); ++index)
            if (profile_name(files[index]) == active) break;
          const Theme theme = active_theme;
          load_profile(ProfileKind::Nds, (index + 1) % files.size());
          load_nds_controls();
          active_theme = theme;
          save_controls();
        }
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_v) {
        save_profile(ProfileKind::Nds);
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_n) {
        create_profile(ProfileKind::Nds);
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_d) {
        const Theme theme = active_theme;
        delete_profile(ProfileKind::Nds);
        load_nds_controls();
        active_theme = theme;
        save_controls();
        continue;
      }
      if (waiting) {
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          waiting = false;
        } else {
          if (selected == nds_turbo_hold_index)
            turbo_key = event.key.keysym.sym;
          else if (selected < nds_input_device_index) {
            nds_control_keys[selected] = event.key.keysym.sym;
            nds_controller_axis_bound[selected] = false;
          }
          else {
            waiting = false;
            continue;
          }
          save_controls();
          save_nds_controls();
          waiting = false;
          saved = true;
          saved_until = SDL_GetTicks() + 1200;
        }
      } else if (event.key.keysym.sym == SDLK_UP ||
                 event.key.keysym.sym == SDLK_w) {
        selected = (selected + option_count - 1) % option_count;
        if (selected == nds_turbo_toggle_index)
          selected = nds_turbo_hold_index;
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s) {
        selected = (selected + 1) % option_count;
        if (selected == nds_turbo_toggle_index)
          selected = 0;
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        if (selected == nds_input_device_index) {
          nds_input_mode = nds_input_mode == InputMode::Keyboard
              ? InputMode::Controller : InputMode::Keyboard;
          save_nds_controls();
        } else if (selected != nds_turbo_toggle_index) waiting = true;
      } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        return;
      }
    }
    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
    SDL_RenderClear(renderer);
    draw_text(renderer, "CONTROLS", 295, 55, 5, {245, 183, 202, 255});
    const int first = mouse_hover_selection ? last_control_first :
                      selected > 8 ? static_cast<int>(selected) - 8 : 0;
    last_control_first = first;
    const int visible_rows = std::min(
        10, static_cast<int>(option_count) - first);
    const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
    for (int row = 0; row < 10 && first + row < static_cast<int>(option_count);
         ++row) {
      const unsigned index = static_cast<unsigned>(first + row);
      const int y = controls_top + row * 34;
      if (menu_selection_visible && index == selected) {
        SDL_SetRenderDrawColor(renderer, 112, 42, 72, 255);
        SDL_Rect highlight{170, y - 7, 460, 28};
        SDL_RenderFillRect(renderer, &highlight);
      }
      const bool input_device = index == nds_input_device_index;
      const bool turbo_hold = index == nds_turbo_hold_index;
      const bool turbo_toggle = index == nds_turbo_toggle_index;
      const char *name = turbo_hold ? "TURBO SPEED HOLD" :
                         input_device ? "INPUT DEVICE" :
                         turbo_toggle ? "TURBO SPEED TOGGLE" :
                         nds_control_names[index];
      draw_text(renderer, name, 190, y, 2,
                menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                  : SDL_Color{220, 166, 187, 255});
      if (input_device) {
        draw_text(renderer, nds_input_mode == InputMode::Controller
                      ? "CONTROLLER" : "KEYBOARD", 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      } else if (turbo_toggle) {
        draw_text(renderer, "SHIFT+" + key_name(turbo_key), 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      } else {
          const std::string binding = waiting && selected == index
              ? "PRESS KEY OR BUTTON"
              : nds_input_mode == InputMode::Controller && !turbo_hold
                  ? nds_controller_axis_bound[index]
                      ? controller_axis_name(nds_controller_axes[index],
                                              nds_controller_axis_positive[index])
                      : controller_button_name(nds_controller_buttons[index])
                  : key_name(turbo_hold ? turbo_key : nds_control_keys[index]);
          draw_text(renderer, binding, 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      }
    }
    draw_text_centered(renderer, "F1 CRT TOGGLE", 400, 700, 2,
                       {190, 116, 148, 255});
    draw_text_centered(renderer, "F2 FPS   F3 VSYNC   F5 INPUT SWAP   F6 LAYOUT",
                       400, 730, 2,
                       {190, 116, 148, 255});
    draw_text_centered(renderer, renaming ? "RENAME PROFILE: " + profile_edit
                                         : "PROFILE: " + active_profile_name(ProfileKind::Nds),
                       400, 630, 2, {190, 116, 148, 255});
    draw_text_centered(renderer,
                       "C CHANGE   V SAVE   N NEW   R RENAME   D DELETE",
                       400, 660, 2, {190, 116, 148, 255});
    if (saved && SDL_GetTicks() < saved_until)
      draw_text_centered(renderer, "SAVED", 400, 590, 2,
                         {175, 245, 195, 255});
    else
      saved = false;
    draw_easter_eggs_notice(renderer);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

void wii_controls_menu(SDL_Renderer *renderer) {
  mouse_hover_selection = false;
  last_control_first = 0;
  unsigned selected = 0;
  constexpr unsigned control_count =
      wii_visible_control_count + 1 + wii_nunchuk_keys.size() +
      wii_tilt_keys.size() + 4;
  constexpr unsigned wii_nunchuk_enabled_index = wii_visible_control_count;
  constexpr unsigned wii_tilt_start_index =
      wii_nunchuk_enabled_index + 1 + wii_nunchuk_keys.size();
  constexpr unsigned wii_shake_index =
      wii_tilt_start_index + wii_tilt_keys.size();
  constexpr unsigned wii_input_device_index =
      wii_shake_index + 1;
  constexpr unsigned wii_turbo_hold_index =
      wii_input_device_index + 1;
  constexpr unsigned wii_turbo_toggle_index = wii_turbo_hold_index + 1;
  bool waiting = false;
  bool renaming = false;
  std::string profile_edit;
  bool saved = false;
  Uint32 saved_until = 0;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (wii_input_mode == InputMode::RealWiimote) {
        translate_controller_event(event);
        if (event.type == SDL_QUIT) {
          quit_requested = true;
          return;
        }
        if (mouse_footer_back(event)) return;
        const SDL_Rect row{170, (FRONTEND_HEIGHT - 34) / 2 - 7, 460, 28};
        int hovered_row = 0;
        if (mouse_menu_event(event, hovered_row, &row, 1)) {
          selected = static_cast<unsigned>(hovered_row);
          if (event.type == SDL_MOUSEMOTION) continue;
        }
        if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
        if (event.key.keysym.sym == SDLK_ESCAPE) return;
        if (event.key.keysym.sym == SDLK_RETURN ||
                   event.key.keysym.sym == SDLK_SPACE) {
          cycle_wii_input_mode();
          selected = 0;
          save_wii_controls();
        } else if (event.key.keysym.sym == SDLK_F5) {
          cycle_wii_input_mode();
          selected = 0;
          save_wii_controls();
        }
        continue;
      }
      SDL_GameControllerButton controller_button{};
      const bool binding_controller =
          controller_button_event(event, controller_button);
      SDL_GameControllerAxis controller_axis{};
      bool controller_axis_positive = false;
      const bool binding_axis =
          controller_axis_event(event, controller_axis, controller_axis_positive);
      if (waiting && binding_controller) {
        if (selected < wii_visible_control_count) {
          wii_controller_buttons[selected] = controller_button;
          wii_controller_axis_bound[selected] = false;
        } else if (selected > wii_nunchuk_enabled_index &&
                   selected < wii_tilt_start_index) {
          wii_nunchuk_controller_buttons[selected -
                                         wii_nunchuk_enabled_index - 1] =
              controller_button;
          wii_nunchuk_controller_axis_bound[selected -
                                             wii_nunchuk_enabled_index - 1] =
              false;
        } else if (selected >= wii_tilt_start_index &&
                   selected < wii_shake_index) {
          const size_t index = selected - wii_tilt_start_index;
          wii_tilt_controller_buttons[index] = controller_button;
          wii_tilt_controller_axis_bound[index] = false;
        } else if (selected == wii_shake_index) {
          wii_shake_controller_button = controller_button;
          wii_shake_controller_axis_bound = false;
        }
        save_wii_controls();
        waiting = false;
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      if (waiting && binding_axis) {
        if (selected < wii_visible_control_count) {
          wii_controller_axes[selected] = controller_axis;
          wii_controller_axis_positive[selected] = controller_axis_positive;
          wii_controller_axis_bound[selected] = true;
        } else if (selected > wii_nunchuk_enabled_index &&
                   selected < wii_tilt_start_index) {
          const size_t index = selected - wii_nunchuk_enabled_index - 1;
          wii_nunchuk_controller_axes[index] = controller_axis;
          wii_nunchuk_controller_axis_positive[index] =
              controller_axis_positive;
          wii_nunchuk_controller_axis_bound[index] = true;
        } else if (selected >= wii_tilt_start_index &&
                   selected < wii_shake_index) {
          const size_t index = selected - wii_tilt_start_index;
          wii_tilt_controller_axes[index] = controller_axis;
          wii_tilt_controller_axis_positive[index] = controller_axis_positive;
          wii_tilt_controller_axis_bound[index] = true;
        } else if (selected == wii_shake_index) {
          wii_shake_controller_axis = controller_axis;
          wii_shake_controller_axis_positive = controller_axis_positive;
          wii_shake_controller_axis_bound = true;
        }
        save_wii_controls();
        waiting = false;
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      translate_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return;
      }
      if (renaming && event.type == SDL_TEXTINPUT) {
        append_profile_text(profile_edit, event.text.text);
        continue;
      }
      if (!waiting && !renaming && wii_input_mode != InputMode::RealWiimote) {
        if (event.type == SDL_MOUSEWHEEL &&
            mouse_over_control_list(last_control_first, control_count)) {
          last_control_first = std::clamp(
              last_control_first - event.wheel.y, 0,
              std::max(0, static_cast<int>(control_count) - 10));
          mouse_hover_selection = true;
          update_control_mouse_hover(
              selected, last_control_first,
              std::min(10, static_cast<int>(control_count) - last_control_first));
          continue;
        }
        const int first = mouse_hover_selection ? last_control_first :
                          selected > 8 ? static_cast<int>(selected) - 8 : 0;
        last_control_first = first;
        const int visible_rows = std::min(
            10, static_cast<int>(control_count) - first);
        const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
        std::array<SDL_Rect, 10> rows{};
        for (int row = 0; row < visible_rows; ++row)
          rows[row] = SDL_Rect{170, controls_top + row * 34 - 7, 460, 28};
        int hovered_row = 0;
        if (mouse_menu_event(event, hovered_row, rows.data(), visible_rows)) {
          selected = static_cast<unsigned>(first + hovered_row);
          if (event.type == SDL_MOUSEMOTION) continue;
        }
      }
      if (!waiting && !renaming && mouse_footer_back(event)) return;
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (renaming) {
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          renaming = false;
          SDL_StopTextInput();
        } else if (event.key.keysym.sym == SDLK_BACKSPACE) {
          if (!profile_edit.empty()) profile_edit.pop_back();
        } else if (event.key.keysym.sym == SDLK_RETURN) {
          if (rename_profile(ProfileKind::Wii, profile_edit)) {
            saved = true;
            saved_until = SDL_GetTicks() + 1200;
          }
          renaming = false;
          SDL_StopTextInput();
        }
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_r) {
        renaming = true;
        profile_edit.clear();
        SDL_StartTextInput();
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_c) {
        const auto files = profile_files(ProfileKind::Wii);
        if (!files.empty()) {
          const auto active = active_profile_name(ProfileKind::Wii);
          size_t index = 0;
          for (; index < files.size(); ++index)
            if (profile_name(files[index]) == active) break;
          const Theme theme = active_theme;
          load_profile(ProfileKind::Wii, (index + 1) % files.size());
          load_wii_controls();
          active_theme = theme;
          save_controls();
        }
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_v) {
        save_profile(ProfileKind::Wii);
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_n) {
        create_profile(ProfileKind::Wii);
        continue;
      }
      if (!waiting && event.key.keysym.sym == SDLK_d) {
        const Theme theme = active_theme;
        delete_profile(ProfileKind::Wii);
        load_wii_controls();
        active_theme = theme;
        save_controls();
        continue;
      }
      if (waiting) {
        if (event.key.keysym.sym == SDLK_ESCAPE) waiting = false;
        else {
          if (selected < wii_visible_control_count) {
            wii_control_keys[selected] = event.key.keysym.sym;
            wii_controller_axis_bound[selected] = false;
          } else if (selected > wii_nunchuk_enabled_index &&
                     selected < wii_tilt_start_index) {
            const size_t index = selected - wii_nunchuk_enabled_index - 1;
            wii_nunchuk_keys[index] = event.key.keysym.sym;
            wii_nunchuk_controller_axis_bound[index] = false;
          } else if (selected >= wii_tilt_start_index &&
                     selected < wii_shake_index) {
            wii_tilt_keys[selected - wii_tilt_start_index] =
                event.key.keysym.sym;
            wii_tilt_controller_axis_bound[selected - wii_tilt_start_index] =
                false;
          } else if (selected == wii_shake_index) {
            wii_shake_key = event.key.keysym.sym;
            wii_shake_controller_axis_bound = false;
          }
          else if (selected == wii_turbo_hold_index)
            turbo_key = event.key.keysym.sym;
          else {
            waiting = false;
            continue;
          }
          save_controls();
          save_wii_controls();
          waiting = false;
          saved = true;
          saved_until = SDL_GetTicks() + 1200;
        }
      } else if (event.key.keysym.sym == SDLK_UP ||
                 event.key.keysym.sym == SDLK_w) {
        selected = (selected + control_count - 1) % control_count;
        if (selected == wii_turbo_toggle_index)
          selected = wii_turbo_hold_index;
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s) {
        selected = (selected + 1) % control_count;
        if (selected == wii_turbo_toggle_index)
          selected = 0;
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        if (selected == wii_nunchuk_enabled_index) {
          wii_nunchuk_enabled = !wii_nunchuk_enabled;
          save_wii_controls();
        } else if (selected == wii_input_device_index) {
          cycle_wii_input_mode();
          if (wii_input_mode == InputMode::RealWiimote) selected = 0;
          save_wii_controls();
        } else if (selected != wii_turbo_toggle_index) waiting = true;
      } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        return;
      }
    }
    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
    SDL_RenderClear(renderer);
    draw_text(renderer, "CONTROLS", 295, 55, 5,
              {245, 183, 202, 255});
    const unsigned visible_control_count =
        wii_input_mode == InputMode::RealWiimote ? 1 : control_count;
    const int first = wii_input_mode == InputMode::RealWiimote
        ? 0 : selected > 8 ? static_cast<int>(selected) - 8 : 0;
    const int visible_rows = std::min(
        10, static_cast<int>(visible_control_count) - first);
    const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
    for (int row = 0; row < 10 &&
                     first + row < static_cast<int>(visible_control_count);
         ++row) {
      const unsigned index = static_cast<unsigned>(first + row);
      const int y = controls_top + row * 34;
      if (menu_selection_visible && index == selected) {
        SDL_SetRenderDrawColor(renderer, 112, 42, 72, 255);
        SDL_Rect highlight{170, y - 7, 460, 28};
        SDL_RenderFillRect(renderer, &highlight);
      }
      const bool real_wiimote = wii_input_mode == InputMode::RealWiimote;
      const bool nunchuk_toggle = !real_wiimote &&
                                  index == wii_nunchuk_enabled_index;
      const bool nunchuk_control = index > wii_nunchuk_enabled_index &&
                                   index < wii_tilt_start_index;
      const bool tilt = index >= wii_tilt_start_index &&
                        index < wii_shake_index;
      const bool shake = index == wii_shake_index;
      const bool input_device = real_wiimote || index == wii_input_device_index;
      const bool turbo_hold = index == wii_turbo_hold_index;
      const bool turbo_toggle = index == wii_turbo_toggle_index;
      const unsigned tilt_index = index - wii_tilt_start_index;
      const unsigned nunchuk_index = index - wii_nunchuk_enabled_index - 1;
      const char *name = input_device ? "INPUT DEVICE" :
                         turbo_hold ? "TURBO SPEED HOLD" :
                         turbo_toggle ? "TURBO SPEED TOGGLE" :
                         nunchuk_toggle ? "NUNCHUK" :
                         shake ? "SHAKE" :
                         nunchuk_control ? wii_nunchuk_names[nunchuk_index] :
                         tilt ? wii_tilt_names[tilt_index]
                              : wii_control_names[index];
      const SDL_Keycode key = nunchuk_toggle ? SDLK_UNKNOWN :
                              turbo_hold ? turbo_key :
                              shake ? wii_shake_key :
                              nunchuk_control ? wii_nunchuk_keys[nunchuk_index] :
                              tilt ? wii_tilt_keys[tilt_index]
                                   : wii_control_keys[index];
      draw_text(renderer, name, 190, y, 2,
                menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                  : SDL_Color{220, 166, 187, 255});
      if (nunchuk_toggle) {
        draw_text(renderer, wii_nunchuk_enabled ? "ENABLED" : "DISABLED",
                  505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      } else if (input_device) {
        draw_text(renderer, wii_input_mode == InputMode::RealWiimote
                      ? "WIIMOTE"
                      : wii_input_mode == InputMode::Controller
                          ? "CONTROLLER" : "KEYBOARD", 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      } else if (turbo_toggle) {
        draw_text(renderer, "SHIFT+" + key_name(turbo_key), 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      } else {
        std::string binding;
        if (waiting && selected == index) {
          binding = "PRESS KEY OR BUTTON";
        } else if (wii_input_mode == InputMode::Controller &&
                   !turbo_hold && !nunchuk_toggle) {
          if (shake)
            binding = wii_shake_controller_axis_bound
                ? controller_axis_name(wii_shake_controller_axis,
                                        wii_shake_controller_axis_positive)
                : controller_button_name(wii_shake_controller_button);
          else if (nunchuk_control)
            binding = wii_nunchuk_controller_axis_bound[nunchuk_index]
                ? controller_axis_name(
                      wii_nunchuk_controller_axes[nunchuk_index],
                      wii_nunchuk_controller_axis_positive[nunchuk_index])
                : controller_button_name(
                      wii_nunchuk_controller_buttons[nunchuk_index]);
          else if (tilt)
            binding = wii_tilt_controller_axis_bound[tilt_index]
                ? controller_axis_name(
                      wii_tilt_controller_axes[tilt_index],
                      wii_tilt_controller_axis_positive[tilt_index])
                : controller_button_name(
                      wii_tilt_controller_buttons[tilt_index]);
          else
            binding = wii_controller_axis_bound[index]
                ? controller_axis_name(wii_controller_axes[index],
                                        wii_controller_axis_positive[index])
                : controller_button_name(wii_controller_buttons[index]);
        } else {
          binding = key_name(key);
        }
        draw_text(renderer, binding, 505, y, 2,
                  menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                    : SDL_Color{190, 116, 148, 255});
      }
    }
    draw_text_centered(renderer, "ESC BACK", 400, 730, 2,
                       {190, 116, 148, 255});
    draw_text_centered(renderer, renaming ? "RENAME PROFILE: " + profile_edit
                                         : "PROFILE: " + active_profile_name(ProfileKind::Wii),
                       400, 630, 2, {190, 116, 148, 255});
    draw_text_centered(renderer,
                       "C CHANGE   V SAVE   N NEW   R RENAME   D DELETE",
                       400, 660, 2, {190, 116, 148, 255});
    if (!waiting && wii_input_mode == InputMode::RealWiimote)
      draw_text_centered(renderer, "PRESS 1+2 IN GAME TO PAIR", 400, 590, 2,
                         {175, 245, 195, 255});
    else if (!waiting && saved && SDL_GetTicks() < saved_until)
      draw_text_centered(renderer, "SAVED", 400, 590, 2,
                         {175, 245, 195, 255});
    else if (!waiting)
      saved = false;
    draw_easter_eggs_notice(renderer);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

void gamecube_controls_menu(SDL_Renderer *renderer) {
  mouse_hover_selection = false;
  last_control_first = 0;
  unsigned selected = 0;
  bool waiting = false;
  bool saved = false;
  Uint32 saved_until = 0;
  constexpr unsigned input_device_index = 12;
  constexpr unsigned turbo_hold_index = 13;
  constexpr unsigned option_count = 15;
  const std::array<const char *, 12> names{
      "A BUTTON", "B BUTTON", "X BUTTON", "Y BUTTON", "START",
      "UP", "DOWN", "LEFT", "RIGHT", "L TRIGGER", "R TRIGGER", "Z TRIGGER"};
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      SDL_GameControllerButton controller_button{};
      const bool binding_controller =
          controller_button_event(event, controller_button);
      SDL_GameControllerAxis controller_axis{};
      bool controller_axis_positive = false;
      const bool binding_axis =
          controller_axis_event(event, controller_axis, controller_axis_positive);
      if (waiting && binding_controller && selected < input_device_index) {
        wii_controller_buttons[selected] = controller_button;
        wii_controller_axis_bound[selected] = false;
        save_wii_controls();
        waiting = false;
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      if (waiting && binding_axis && selected < input_device_index) {
        wii_controller_axes[selected] = controller_axis;
        wii_controller_axis_positive[selected] = controller_axis_positive;
        wii_controller_axis_bound[selected] = true;
        save_wii_controls();
        waiting = false;
        saved = true;
        saved_until = SDL_GetTicks() + 1200;
        continue;
      }
      translate_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return;
      }
      if (!waiting) {
        if (event.type == SDL_MOUSEWHEEL &&
            mouse_over_control_list(last_control_first, option_count)) {
          last_control_first = std::clamp(
              last_control_first - event.wheel.y, 0,
              std::max(0, static_cast<int>(option_count) - 10));
          mouse_hover_selection = true;
          update_control_mouse_hover(
              selected, last_control_first,
              std::min(10, static_cast<int>(option_count) - last_control_first));
          continue;
        }
        const int first = mouse_hover_selection ? last_control_first :
                          selected > 8 ? static_cast<int>(selected) - 8 : 0;
        last_control_first = first;
        const int visible_rows =
            std::min(10, static_cast<int>(option_count) - first);
        const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
        std::array<SDL_Rect, 10> rows{};
        for (int row = 0; row < visible_rows; ++row)
          rows[row] = SDL_Rect{170, controls_top + row * 34 - 7, 460, 28};
        int hovered_row = 0;
        if (mouse_menu_event(event, hovered_row, rows.data(), visible_rows)) {
          selected = static_cast<unsigned>(first + hovered_row);
          if (event.type == SDL_MOUSEMOTION) continue;
        }
      }
      if (!waiting && mouse_footer_back(event)) return;
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (waiting) {
        if (event.key.keysym.sym == SDLK_ESCAPE) {
          waiting = false;
        } else if (selected < input_device_index) {
          wii_control_keys[selected] = event.key.keysym.sym;
          wii_controller_axis_bound[selected] = false;
          save_wii_controls();
          waiting = false;
          saved = true;
          saved_until = SDL_GetTicks() + 1200;
        } else if (selected == turbo_hold_index) {
          turbo_key = event.key.keysym.sym;
          save_controls();
          waiting = false;
          saved = true;
          saved_until = SDL_GetTicks() + 1200;
        }
        continue;
      }
      if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_w)
        selected = (selected + option_count - 1) % option_count;
      else if (event.key.keysym.sym == SDLK_DOWN ||
               event.key.keysym.sym == SDLK_s)
        selected = (selected + 1) % option_count;
      if (selected == 14)
        selected = event.key.keysym.sym == SDLK_UP ||
                           event.key.keysym.sym == SDLK_w
                       ? turbo_hold_index
                       : 0;
      else if (event.key.keysym.sym == SDLK_RETURN ||
               event.key.keysym.sym == SDLK_SPACE) {
        if (selected == input_device_index) {
          wii_input_mode = wii_input_mode == InputMode::Keyboard
              ? InputMode::Controller : InputMode::Keyboard;
          save_wii_controls();
        } else if (selected != turbo_hold_index && selected != 14) {
          waiting = true;
        } else if (selected == turbo_hold_index) {
          waiting = true;
        }
      } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        return;
      }
    }
    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
    SDL_RenderClear(renderer);
    draw_text(renderer, "CONTROLS", 295, 55, 5,
              {245, 183, 202, 255});
    const int first = mouse_hover_selection ? last_control_first :
                      selected > 8 ? static_cast<int>(selected) - 8 : 0;
    last_control_first = first;
    const int visible_rows = std::min(10, static_cast<int>(option_count) - first);
    const int controls_top = (FRONTEND_HEIGHT - visible_rows * 34) / 2;
    for (int row = 0; row < visible_rows; ++row) {
      const unsigned index = static_cast<unsigned>(first + row);
      const int y = controls_top + row * 34;
      if (menu_selection_visible && index == selected) {
        SDL_SetRenderDrawColor(renderer, 112, 42, 72, 255);
        SDL_Rect highlight{170, y - 7, 460, 28};
        SDL_RenderFillRect(renderer, &highlight);
      }
      const char *name = index < names.size() ? names[index] :
                         index == input_device_index ? "INPUT DEVICE" :
                         index == turbo_hold_index ? "TURBO SPEED HOLD" :
                         "TURBO SPEED TOGGLE";
      draw_text(renderer, name, 190, y, 2,
                menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                  : SDL_Color{220, 166, 187, 255});
      std::string binding;
      if (index == input_device_index)
        binding = wii_input_mode == InputMode::Controller ? "CONTROLLER" : "KEYBOARD";
      else if (index == 14)
        binding = "SHIFT + " + key_name(turbo_key);
      else if (waiting && selected == index)
        binding = "PRESS KEY OR BUTTON";
      else if (wii_input_mode == InputMode::Controller && index < input_device_index)
        binding = wii_controller_axis_bound[index]
            ? controller_axis_name(wii_controller_axes[index],
                                    wii_controller_axis_positive[index])
            : controller_button_name(wii_controller_buttons[index]);
      else
        binding = key_name(index == turbo_hold_index ? turbo_key
                                                     : wii_control_keys[index]);
      draw_text(renderer, binding, 505, y, 2,
                menu_selection_visible && index == selected ? SDL_Color{255, 225, 235, 255}
                                  : SDL_Color{190, 116, 148, 255});
    }
    draw_text_centered(renderer, "ESC BACK", 400, 730, 2,
                       {190, 116, 148, 255});
    draw_text_centered(renderer, "PROFILE: " + active_profile_name(ProfileKind::Wii),
                       400, 630, 2, {190, 116, 148, 255});
    draw_text_centered(renderer, "C CHANGE   V SAVE   N NEW   R RENAME   D DELETE",
                       400, 660, 2, {190, 116, 148, 255});
    if (saved && SDL_GetTicks() < saved_until)
      draw_text_centered(renderer, "SAVED", 400, 590, 2,
                         {175, 245, 195, 255});
    else
      saved = false;
    draw_easter_eggs_notice(renderer);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

void wii_controller_ports_menu(SDL_Renderer *renderer, bool gamecube) {
  ScopedFlag dark_page(dark_page_rendering, true);
  unsigned selected = 0;
  constexpr unsigned port_count_option = 4;
  constexpr unsigned option_count = 5;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      translate_frontend_controller_event(event, true);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return;
      }
      {
        constexpr int option_height = 62;
        constexpr int option_gap = 20;
        const int options_height = option_count * option_height +
                                   (option_count - 1) * option_gap;
        const int options_top = (FRONTEND_HEIGHT - options_height) / 2;
        std::array<SDL_Rect, option_count> options{};
        for (unsigned i = 0; i < option_count; ++i)
          options[i] = SDL_Rect{
              240, options_top + static_cast<int>(i) *
                                (option_height + option_gap),
              320, option_height};
        int hovered_option = 0;
        if (mouse_menu_event(event, hovered_option, options.data(),
                             option_count)) {
          selected = static_cast<unsigned>(hovered_option);
          if (event.type == SDL_MOUSEMOTION) continue;
        }
      }
      if (mouse_footer_back(event)) {
        wii_control_port = 1;
        load_wii_controls();
        return;
      }
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (event.key.keysym.sym >= SDLK_1 &&
          event.key.keysym.sym <= SDLK_4) {
        wii_active_port_count =
            static_cast<unsigned>(event.key.keysym.sym - SDLK_0);
        save_wii_port_count();
        continue;
      }
      if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_w) {
        selected = (selected + option_count - 1) % option_count;
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s) {
        selected = (selected + 1) % option_count;
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        if (selected < port_count_option) {
          wii_control_port = selected + 1;
          load_wii_controls();
          if (gamecube) gamecube_controls_menu(renderer);
          else wii_controls_menu(renderer);
          if (quit_requested) return;
          wii_control_port = 1;
          load_wii_controls();
        } else {
          wii_active_port_count = wii_active_port_count % 4 + 1;
          save_wii_port_count();
        }
      } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        wii_control_port = 1;
        load_wii_controls();
        return;
      }
    }

    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
    SDL_RenderClear(renderer);
    draw_text_centered(renderer, "CONTROLS", 400, 55, 5,
                       {245, 183, 202, 255});
    const int option_height = 62;
    const int option_gap = 20;
    const int options_height = option_count * option_height +
                               (option_count - 1) * option_gap;
    const int options_top = (FRONTEND_HEIGHT - options_height) / 2;
    for (unsigned i = 0; i < option_count; ++i) {
      const int y = options_top + static_cast<int>(i) *
                                      (option_height + option_gap);
      SDL_Rect option{240, y, 320, option_height};
      const bool item_selected = menu_selection_visible && i == selected;
      SDL_SetRenderDrawColor(renderer, item_selected ? 117 : 67,
                             item_selected ? 48 : 30,
                             item_selected ? 87 : 48, 255);
      if (item_selected) draw_glow_rect(renderer, option, 10);
      draw_round_rect(renderer, option, 10);
      const std::string label = i < port_count_option
          ? "PORT " + std::to_string(i + 1)
          : "ACTIVE PORTS " + std::to_string(wii_active_port_count);
      draw_text_centered(renderer, label, 400, y + 19, 3,
                         item_selected ? SDL_Color{255, 232, 241, 255}
                                       : SDL_Color{245, 205, 217, 255});
    }
    if (menu_selection_visible) {
      const SDL_Rect selected_box{
          240, options_top + static_cast<int>(selected) *
                       (option_height + option_gap), 320, option_height};
      draw_blossom(renderer, selected_box.x - 22,
                   selected_box.y + selected_box.h / 2, 10);
      draw_blossom(renderer, selected_box.x + selected_box.w + 22,
                   selected_box.y + selected_box.h / 2, 10);
    }
    draw_text_centered(renderer, "UP DOWN SELECT   1-4 PORTS   ESC BACK",
                       400, 730, 2,
                       {190, 116, 148, 255});
    draw_easter_eggs_notice(renderer);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

void frontend_settings_menu(SDL_Renderer *renderer, bool nds, bool wii = false,
                            bool gamecube = false, bool three_ds = false) {
  ScopedFlag dark_page(dark_page_rendering, true);
  unsigned selected = 0;
  const unsigned option_count = three_ds ? 3 : gamecube ? 2 : wii ? 3 : 3;
  if (!three_ds)
    crt_filter = nds ? nds_crt_filter : gba_crt_filter;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      translate_frontend_controller_event(event, true);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_t) {
        toggle_theme();
        continue;
      }
      {
        constexpr int option_height = 62;
        constexpr int option_gap = 33;
        const int options_height = static_cast<int>(option_count) * option_height +
                                   static_cast<int>(option_count - 1) * option_gap;
        const int options_top = (FRONTEND_HEIGHT - options_height) / 2;
        std::array<SDL_Rect, 3> options{};
        for (unsigned i = 0; i < option_count; ++i)
          options[i] = SDL_Rect{
              240, options_top + static_cast<int>(i) *
                                (option_height + option_gap),
              320, option_height};
        int hovered_option = 0;
        if (mouse_menu_event(event, hovered_option, options.data(),
                             option_count)) {
          selected = static_cast<unsigned>(hovered_option);
          if (event.type == SDL_MOUSEMOTION) continue;
        }
      }
      if (mouse_footer_back(event)) return;
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_w) {
        selected = (selected + option_count - 1) % option_count;
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s) {
        selected = (selected + 1) % option_count;
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        if (selected == 0) {
          if (gamecube || wii) wii_controller_ports_menu(renderer, gamecube);
          else if (nds) nds_controls_menu(renderer);
          else controls_menu(renderer);
          if (quit_requested) {
            return;
          }
        } else if (selected == 1) {
          if (three_ds) {
            launch_mii_fix(renderer);
          } else if (wii && !gamecube) {
            wii_sideways = !wii_sideways;
            save_wii_controls();
          } else if (gamecube) {
            wii_resolution = wii_resolution % 4 + 1;
            save_wii_controls();
          } else if (!gamecube) {
            crt_filter = !crt_filter;
            if (nds) nds_crt_filter = crt_filter;
            else gba_crt_filter = crt_filter;
            save_controls();
          }
        } else if (selected == 2 && three_ds) {
          three_ds_resolution = three_ds_resolution % 4 + 1;
          save_three_ds_resolution();
        } else if (selected == 2 && wii) {
          wii_resolution = wii_resolution % 4 + 1;
          save_wii_controls();
        } else if (selected == 2) {
          if (nds) {
            nds_overlay = !nds_overlay;
            save_nds_controls();
          } else {
            gba_overlay = !gba_overlay;
            save_controls();
          }
        }
      } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        return;
      }
    }

    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
    SDL_RenderClear(renderer);
    draw_text_centered(renderer, "SETTINGS", 400, 55, 5,
                       {245, 183, 202, 255});
    const std::string resolution_label =
        "RESOLUTION " + std::to_string(wii_resolution) + "X";
    const std::string three_ds_resolution_label =
        "RESOLUTION " + std::to_string(three_ds_resolution) + "X";
    const char *options[] = {
        "CONTROLS",
        three_ds ? "FIX MII" :
        wii && !gamecube ? (wii_sideways ? "SIDE REMOTE ON"
                            : "SIDE REMOTE OFF")
            : gamecube ? resolution_label.c_str()
            : (crt_filter ? "CRT ON" : "CRT OFF"),
        three_ds ? three_ds_resolution_label.c_str() :
        wii && !gamecube ? resolution_label.c_str()
            : (nds ? (nds_overlay ? "OVERLAY ON" : "OVERLAY OFF")
                   : (gba_overlay ? "OVERLAY ON" : "OVERLAY OFF"))};
    constexpr int option_height = 62;
    constexpr int option_gap = 33;
    const int options_height = static_cast<int>(option_count) * option_height +
                               static_cast<int>(option_count - 1) * option_gap;
    const int options_top = (FRONTEND_HEIGHT - options_height) / 2;
    for (unsigned i = 0; i < option_count; ++i) {
      const int y = options_top + static_cast<int>(i) *
                                      (option_height + option_gap);
      SDL_Rect option{240, y, 320, 62};
      const bool item_selected = menu_selection_visible && i == selected;
      SDL_SetRenderDrawColor(renderer, item_selected ? 117 : 67,
                             item_selected ? 48 : 30,
                             item_selected ? 87 : 48, 255);
      if (item_selected) draw_glow_rect(renderer, option, 10);
      SDL_SetRenderDrawColor(renderer, item_selected ? 117 : 67,
                             item_selected ? 48 : 30,
                             item_selected ? 87 : 48, 255);
      draw_round_rect(renderer, option, 10);
      draw_text_centered(renderer, options[i], 400, y + 19, 3,
                         item_selected ? SDL_Color{255, 232, 241, 255}
                                       : SDL_Color{245, 205, 217, 255});
    }
    if (menu_selection_visible) {
      const SDL_Rect selected_box{
          240, options_top + static_cast<int>(selected) *
                       (option_height + option_gap), 320, option_height};
      draw_blossom(renderer, selected_box.x - 22,
                   selected_box.y + selected_box.h / 2, 10);
      draw_blossom(renderer, selected_box.x + selected_box.w + 22,
                   selected_box.y + selected_box.h / 2, 10);
    }
    draw_text_centered(renderer, "UP DOWN SELECT   ESC BACK", 400, 730, 2,
                       {190, 116, 148, 255});
    draw_easter_eggs_notice(renderer);
    draw_easter_eggs_notice(renderer);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

void draw_emulator_frontend_chrome(SDL_Renderer *renderer, int selected) {
  SDL_SetRenderDrawColor(renderer, 117, 48, 87, 255);
  const SDL_Rect title{205, 70, 390, 62};
  draw_glow_rect(renderer, title, 12);
  draw_round_rect(renderer, title, 12);
  SDL_SetRenderDrawColor(renderer, 255, 232, 241, 255);
  const SDL_Rect title_inner{215, 80, 370, 42};
  draw_round_rect(renderer, title_inner, 8);
  constexpr std::array<SDL_Rect, 3> options{
      SDL_Rect{250, 274, 300, 72}, SDL_Rect{250, 368, 300, 72},
      SDL_Rect{250, 462, 300, 72}};
  for (int i = 0; i < 3; ++i) {
    const bool item_selected = menu_selection_visible && i == selected;
    SDL_SetRenderDrawColor(renderer, item_selected ? 117 : 160, 48,
                           item_selected ? 87 : 120, 255);
    if (item_selected) draw_glow_rect(renderer, options[i], 10);
    draw_round_rect(renderer, options[i], 10);
  }
  if (menu_selection_visible) {
    const SDL_Rect selected_box = options[selected];
    draw_blossom(renderer, selected_box.x - 22,
                 selected_box.y + selected_box.h / 2, 10);
    draw_blossom(renderer, selected_box.x + selected_box.w + 22,
                 selected_box.y + selected_box.h / 2, 10);
  }
}

void draw_emulator_frontend_labels(SDL_Renderer *renderer,
                                   const std::string &title,
                                   const std::string &edition,
                                   const std::string &status) {
  draw_text_centered(renderer, title, 400, 87, 4, {117, 48, 87, 255});
  if (!edition.empty())
    draw_text_centered(renderer, edition, 400, 140, 2,
                       {117, 48, 87, 255});
  draw_text_centered(renderer, "LOAD ROM", 400, 299, 3,
                     {255, 232, 241, 255});
  draw_text_centered(renderer, "SETTINGS", 400, 393, 3,
                     {255, 232, 241, 255});
  draw_text_centered(renderer, "EXIT", 400, 487, 3,
                     {255, 232, 241, 255});
  if (!status.empty())
    draw_text_centered(renderer, status, 400, 745, 2,
                       {245, 145, 175, 255});
  draw_text_centered(renderer, "MADE BY FLOWKIDD", 400, 746, 2,
                     berserk_theme() ? SDL_Color{245, 205, 217, 255}
                                     : SDL_Color{117, 48, 87, 255});
  draw_text(renderer, "BUILD V0.8.1 BETA", 40, 746, 2,
            {180, 110, 145, 255});
  draw_theme_footer(renderer, 746);
}

bool gba_frontend_menu(SDL_Window *window, SDL_Renderer *renderer,
                       std::string &rom_path) {
  int selected = 0;
  bool running = true;
  constexpr std::array<SDL_Rect, 3> menu_options{
      SDL_Rect{250, 274, 300, 72}, SDL_Rect{250, 368, 300, 72},
      SDL_Rect{250, 462, 300, 72}};
  auto load_background = [&]() {
    return load_theme_background(renderer);
  };
  SDL_Texture *background = load_background();
  const auto finish = [&](bool result) {
    if (background) SDL_DestroyTexture(background);
    return result;
  };
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      translate_frontend_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return finish(false);
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 610 && event.button.y >= FRONTEND_HEIGHT - 55) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_background();
        continue;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_t) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_background();
        continue;
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 205 && event.button.x < 595 &&
          event.button.y >= 70 && event.button.y < 132) {
        title_edition = (title_edition + 1) % 3;
      }
      if (mouse_menu_event(event, selected, menu_options.data(),
                           menu_options.size())) {
        if (event.type == SDL_MOUSEMOTION) continue;
      }
      if (event.type != SDL_KEYDOWN) continue;
      if (event.key.keysym.sym == SDLK_UP ||
          event.key.keysym.sym == SDLK_w ||
          event.key.keysym.sym == SDLK_a ||
          event.key.keysym.sym == SDLK_DOWN ||
          event.key.keysym.sym == SDLK_s ||
          event.key.keysym.sym == SDLK_d) {
        const bool down = event.key.keysym.sym == SDLK_DOWN ||
                          event.key.keysym.sym == SDLK_s ||
                          event.key.keysym.sym == SDLK_d;
        selected = (selected + (down ? 1 : 2)) % 3;
      }
      if (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_SPACE) {
        if (selected == 0) {
          if (rom_path.empty()) rom_path = choose_rom(renderer);
          if (quit_requested) return finish(false);
          if (!rom_path.empty()) return finish(true);
        } else if (selected == 1) {
          frontend_settings_menu(renderer, false);
          if (quit_requested) return finish(false);
          SDL_DestroyTexture(background);
          background = load_background();
        } else if (selected == 2) {
          return finish(false);
        }
      }
      if (event.key.keysym.sym == SDLK_ESCAPE) {
        frontend_escape_requested = true;
        return finish(false);
      }
    }
    draw_frontend_background(renderer, background);
    draw_emulator_frontend_chrome(renderer, selected);
    draw_emulator_frontend_labels(
        renderer,
        title_edition == 1 && !all_easter_eggs_enabled ? "DRILBOOR"
        : title_edition == 2 && !all_easter_eggs_enabled ? "BIG FOREHEAD"
        : "FLOWBOYADVANCE",
        title_edition != 0 && !all_easter_eggs_enabled ? "EDITION" : "", "");
    (void)window;
    draw_easter_eggs_notice(renderer);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
  return finish(false);
}

bool nds_frontend_menu(SDL_Window *window, SDL_Renderer *renderer,
                       std::string &rom_path, bool three_ds = false) {
  frontend_escape_requested = false;
  int selected = 0;
  std::string status;
  constexpr std::array<SDL_Rect, 3> menu_options{
      SDL_Rect{250, 274, 300, 72}, SDL_Rect{250, 368, 300, 72},
      SDL_Rect{250, 462, 300, 72}};
  SDL_Texture *background = load_theme_background(renderer);
  const auto finish = [&](bool result) {
    if (background) SDL_DestroyTexture(background);
    return result;
  };
  while (true) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      translate_frontend_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return finish(false);
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 610 && event.button.y >= FRONTEND_HEIGHT - 55) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
        continue;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_t) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
        continue;
      }
      if (!three_ds && event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 205 && event.button.x < 595 &&
          event.button.y >= 70 && event.button.y < 132) {
        if (!nds_cooked_title && !skywalker_active) {
          nds_cooked_title = true;
          ur_cooked_active = true;
        } else if (nds_cooked_title) {
          nds_cooked_title = false;
          ur_cooked_active = false;
          skywalker_active = true;
        } else {
          skywalker_active = false;
        }
        continue;
      }
      if (mouse_menu_event(event, selected, menu_options.data(),
                           menu_options.size())) {
        if (event.type == SDL_MOUSEMOTION) continue;
      }
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_w ||
          event.key.keysym.sym == SDLK_a) {
        selected = (selected + 2) % 3;
        status.clear();
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s ||
                 event.key.keysym.sym == SDLK_d) {
        selected = (selected + 1) % 3;
        status.clear();
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        if (selected == 0) {
          if (rom_path.empty())
            rom_path = choose_rom(renderer, three_ds ? ".3ds" : ".nds");
          if (quit_requested) return finish(false);
          if (!rom_path.empty()) return finish(true);
        }
        else if (selected == 1) {
          frontend_settings_menu(renderer, true, false, false, three_ds);
          if (quit_requested) return finish(false);
          SDL_DestroyTexture(background);
          background = load_theme_background(renderer);
        } else {
          return finish(false);
        }
      } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        frontend_escape_requested = true;
        return finish(false);
      }
    }

    draw_frontend_background(renderer, background);
    draw_emulator_frontend_chrome(renderer, selected);
    draw_emulator_frontend_labels(
        renderer,
        three_ds ? "FLOW3DS"
                 : skywalker_active && !all_easter_eggs_enabled
                       ? "SKYWALKER"
                 : nds_cooked_title && !all_easter_eggs_enabled
                       ? "UR COOKED"
                       : "FLOWDS",
        (nds_cooked_title || skywalker_active) && !three_ds &&
                !all_easter_eggs_enabled
            ? "EDITION"
            : "",
        status);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
  (void)window;
}

bool wii_frontend_menu(SDL_Window *window, SDL_Renderer *renderer,
                       std::string &rom_path, bool gamecube = false) {
  int selected = 0;
  std::string status;
  constexpr std::array<SDL_Rect, 3> menu_options{
      SDL_Rect{250, 274, 300, 72}, SDL_Rect{250, 368, 300, 72},
      SDL_Rect{250, 462, 300, 72}};
  SDL_Texture *background = load_theme_background(renderer);
  const auto finish = [&](bool result) {
    if (background) SDL_DestroyTexture(background);
    return result;
  };
  while (true) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      translate_frontend_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return finish(false);
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 610 && event.button.y >= FRONTEND_HEIGHT - 55) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
        continue;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_t) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
        continue;
      }
      if (mouse_menu_event(event, selected, menu_options.data(),
                           menu_options.size())) {
        if (event.type == SDL_MOUSEMOTION) continue;
      }
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_w ||
          event.key.keysym.sym == SDLK_a) {
        selected = (selected + 2) % 3;
        status.clear();
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s ||
                 event.key.keysym.sym == SDLK_d) {
        selected = (selected + 1) % 3;
        status.clear();
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        if (selected == 0) {
          if (rom_path.empty())
            rom_path = choose_rom(renderer, gamecube ? ".gamecube" : ".wii");
          if (quit_requested) return finish(false);
          if (!rom_path.empty()) {
            gamecube_active = gamecube;
            return finish(true);
          }
        } else if (selected == 1) {
          frontend_settings_menu(renderer, true, true, gamecube);
          if (quit_requested) return finish(false);
          SDL_DestroyTexture(background);
          background = load_theme_background(renderer);
          status.clear();
        } else {
          return finish(false);
        }
      } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        frontend_escape_requested = true;
        return finish(false);
      }
    }
    draw_frontend_background(renderer, background);
    draw_emulator_frontend_chrome(renderer, selected);
    draw_emulator_frontend_labels(renderer, gamecube ? "FLOWCUBE" : "FLOWII",
                                  "", status);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
  (void)window;
}

void patch_notes_menu(SDL_Renderer *renderer) {
  ScopedFlag dark_page(dark_page_rendering, true);
  SDL_Texture *background = load_theme_background(renderer);
  int notes_offset = 0;
  constexpr int notes_line_height = 48;
  constexpr int notes_first_y = 155;
  constexpr int notes_max_offset = 11;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      translate_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        if (background) SDL_DestroyTexture(background);
        return;
      }
      if (event.type == SDL_KEYDOWN &&
          (event.key.keysym.sym == SDLK_ESCAPE ||
           event.key.keysym.sym == SDLK_p)) {
        if (background) SDL_DestroyTexture(background);
        return;
      }
      if (event.type == SDL_MOUSEWHEEL) {
        notes_offset = std::clamp(notes_offset - event.wheel.y, 0,
                                  notes_max_offset);
        continue;
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 200 && event.button.x < 600 &&
          event.button.y >= 700) {
        if (background) SDL_DestroyTexture(background);
        return;
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          (event.key.keysym.sym == SDLK_UP ||
           event.key.keysym.sym == SDLK_w ||
           event.key.keysym.sym == SDLK_DOWN ||
           event.key.keysym.sym == SDLK_s)) {
        const bool down = event.key.keysym.sym == SDLK_DOWN ||
                          event.key.keysym.sym == SDLK_s;
        notes_offset = std::clamp(notes_offset + (down ? 1 : -1), 0,
                                  notes_max_offset);
        continue;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_t) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
      }
    }

    draw_frontend_background(renderer, background);
    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 245);
    const SDL_Rect panel{45, 115, 710, 570};
    draw_round_rect(renderer, panel, 14);
    draw_text_centered(renderer, "PATCH NOTES", 400, 65, 4,
                       {245, 183, 202, 255});
    draw_text(renderer, "P CLOSE   T CHANGE THEME", 250, 730, 2,
              {190, 116, 148, 255});
    const SDL_Color note_color = edgerunners_theme()
        ? SDL_Color{245, 183, 202, 255}
        : SDL_Color{245, 205, 217, 255};
    constexpr const char *notes[] = {
        "                    BETA",
        "V0.8.1: FIXED MOUSE NAVIGATION IN CONTROL SETTINGS",
        "V0.8: ADDED 3DS AND GAMECUBE SUPPORT",
        "      ADDED 2-4 PLAYER SUPPORT",
        "      WII CAN ACTUALLY USE A WIIMOTE NOW",
        "      ADDED A DS OVERLAY + LAYOUTS",
        "      ADDED MOUSE NAVIGATION",
        "                 ALPHA ENDED",
        "V0.7.1: WII ACTUALLY WORKS NOW",
        "V0.7: ADDED CONTROLLER SUPPORT AND PROFILES",
        "V0.6: ADDED THEMES",
        "V0.5: ADDED WORKING DS AND WII SUPPORT",
        "V0.4: ADDED THE FLOWSTATION MULTI-MENU",
        "      ADDED INITIAL DS AND WII SUPPORT",
        "      ADDED A GBA OVERLAY",
        "V0.3: ADDED EASTER EGGS AND CRT FILTERS",
        "V0.2: ADDED A WORKING GAMEBOY ADVANCE EMULATOR",
        "V0.1: ADDED THE GAMEBOY FRONTEND",
        "SECRET HINT: ENABLE ALL EASTER EGGS",
        "UP UP DOWN DOWN LEFT RIGHT LEFT RIGHT",
        "SECRET HINT: DISABLE ALL EASTER EGGS",
        "UP DOWN UP DOWN LEFT LEFT RIGHT RIGHT"};
    const SDL_Rect notes_clip{65, 130, 670, 540};
    SDL_RenderSetClipRect(renderer, &notes_clip);
    for (size_t i = 0; i < std::size(notes); ++i) {
      const int y = notes_first_y + static_cast<int>(i) * notes_line_height -
                    notes_offset * notes_line_height;
      if (std::strcmp(notes[i], "                    BETA") == 0 ||
          std::strcmp(notes[i], "                 ALPHA ENDED") == 0)
        draw_text_centered(renderer,
                           std::strcmp(notes[i], "                    BETA") == 0
                               ? "BETA" : "ALPHA ENDED",
                           FRONTEND_WIDTH / 2, y, 2,
                           note_color);
      else
        draw_text(renderer, notes[i], 85, y, 2, note_color);
    }
    SDL_RenderSetClipRect(renderer, nullptr);

    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

void hotkeys_menu(SDL_Renderer *renderer) {
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      translate_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return;
      }
      if (event.type == SDL_KEYDOWN &&
          (event.key.keysym.sym == SDLK_ESCAPE ||
           event.key.keysym.sym == SDLK_h)) {
        return;
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 200 && event.button.x < 600 &&
          event.button.y >= 700)
        return;
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_t)
        toggle_theme();
    }

    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 245);
    SDL_RenderClear(renderer);
    draw_text_centered(renderer, "MENU CONTROLS", 400, 65, 4,
                       {245, 183, 202, 255});
    const char *keyboard[] = {
        "ARROWS / W A S D", "ENTER / SPACE", "ESC",
        "P", "T", "ESC"};
    const char *controller[] = {
        "D-PAD", "A", "B", "Y", "VIEW / BACK", "START"};
    const char *actions[] = {
        "NAVIGATE", "SELECT", "BACK", "PATCH NOTES",
        "CHANGE THEME", "OPEN / CLOSE MENU"};
    const SDL_Color hotkey_color = edgerunners_theme()
        ? SDL_Color{245, 183, 202, 255}
        : SDL_Color{245, 205, 217, 255};
    for (size_t i = 0; i < std::size(actions); ++i) {
      const int y = 250 + static_cast<int>(i) * 48;
      draw_text(renderer, keyboard[i], 78, y, 2, hotkey_color);
      draw_text(renderer, controller[i], 330, y, 2, hotkey_color);
      draw_text(renderer, actions[i], 535, y, 2, hotkey_color);
    }
    draw_text(renderer, "ESC BACK   T CHANGE THEME", 250, 730, 2,
              {190, 116, 148, 255});
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

int station_escape_menu(SDL_Renderer *renderer) {
  int selected = 0;
  constexpr std::array<SDL_Rect, 3> options{
      SDL_Rect{220, 260, 360, 68}, SDL_Rect{220, 365, 360, 68},
      SDL_Rect{220, 470, 360, 68}};
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_CONTROLLERBUTTONDOWN &&
          event.cbutton.button == SDL_CONTROLLER_BUTTON_START)
        return -1;
      translate_controller_event(event);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return 2;
      }
      if (mouse_menu_event(event, selected, options.data(), options.size())) {
        if (event.type == SDL_MOUSEMOTION) continue;
      }
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_w)
        selected = (selected + 2) % 3;
      else if (event.key.keysym.sym == SDLK_DOWN ||
               event.key.keysym.sym == SDLK_s) {
        selected = (selected + 1) % 3;
      }
      else if (event.key.keysym.sym == SDLK_ESCAPE)
        return -1;
      else if (event.key.keysym.sym == SDLK_RETURN ||
               event.key.keysym.sym == SDLK_SPACE)
        return selected;
    }

    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 245);
    SDL_RenderClear(renderer);
    constexpr const char *options[] = {"MENU CONTROLS", "PATCH NOTES", "QUIT"};
    for (int i = 0; i < 3; ++i) {
      SDL_Rect option{220, 260 + i * 105, 360, 68};
      const bool item_selected = menu_selection_visible && i == selected;
      SDL_SetRenderDrawColor(renderer, item_selected ? 117 : 67,
                             item_selected ? 48 : 30,
                             item_selected ? 87 : 48, 255);
      if (item_selected) draw_glow_rect(renderer, option, 10);
      draw_round_rect(renderer, option, 10);
      draw_text_centered(renderer, options[i], 400, option.y + 23, 3,
                         item_selected ? SDL_Color{255, 232, 241, 255}
                                       : SDL_Color{220, 166, 187, 255});
    }
    draw_easter_eggs_notice(renderer);
    draw_text_centered(renderer, "UP DOWN SELECT   ESC BACK", 400, 730, 2,
                       {190, 116, 148, 255});
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

bool menu(SDL_Window *window, SDL_Renderer *renderer, std::string &rom_path) {
  int selected = 0;
  bool running = true;
  std::string status;
  constexpr std::array<SDL_Rect, 5> system_cards{
      SDL_Rect{250, 180, 300, 72}, SDL_Rect{250, 274, 300, 72},
      SDL_Rect{250, 368, 300, 72}, SDL_Rect{250, 462, 300, 72},
      SDL_Rect{250, 556, 300, 72}};
  station_menu_rendering = true;
  SDL_Texture *background = load_theme_background(renderer);
  const auto finish = [&](bool result) {
    station_menu_rendering = false;
    if (background) SDL_DestroyTexture(background);
    return result;
  };

  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      translate_frontend_controller_event(event, true);
      if (event.type == SDL_QUIT) {
        quit_requested = true;
        return finish(false);
      }
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 570 && event.button.y >= FRONTEND_HEIGHT - 55) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
        continue;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_t) {
        toggle_theme();
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
        status.clear();
        continue;
      }
      if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_p) {
        patch_notes_menu(renderer);
        if (quit_requested) return finish(false);
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
        status.clear();
        continue;
      }
      if (mouse_menu_event(event, selected, system_cards.data(),
                           system_cards.size())) {
        if (event.type == SDL_MOUSEMOTION) continue;
      }
      if (event.type != SDL_KEYDOWN || event.key.repeat) continue;
      if (event.key.keysym.sym == SDLK_UP || event.key.keysym.sym == SDLK_w ||
          event.key.keysym.sym == SDLK_a) {
        selected = (selected + 4) % 5;
        status.clear();
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s ||
                 event.key.keysym.sym == SDLK_d) {
        selected = (selected + 1) % 5;
        status.clear();
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        if (selected == 0) {
          const bool launch = gba_frontend_menu(window, renderer, rom_path);
          if (launch) return finish(true);
          if (quit_requested) return finish(false);
          SDL_DestroyTexture(background);
          background = load_theme_background(renderer);
          status.clear();
        } else if (selected == 1) {
          const bool launch = nds_frontend_menu(window, renderer, rom_path);
          if (launch) return finish(true);
          if (quit_requested) return finish(false);
          SDL_DestroyTexture(background);
          background = load_theme_background(renderer);
          status.clear();
        } else if (selected == 2) {
          const bool launch = nds_frontend_menu(window, renderer, rom_path, true);
          if (launch) return finish(true);
          if (quit_requested) return finish(false);
          SDL_DestroyTexture(background);
          background = load_theme_background(renderer);
          status.clear();
        } else if (selected == 4) {
          const bool launch = wii_frontend_menu(window, renderer, rom_path, true);
          if (launch) return finish(true);
          if (quit_requested) return finish(false);
          SDL_DestroyTexture(background);
          background = load_theme_background(renderer);
          status.clear();
        } else {
          const bool launch = wii_frontend_menu(window, renderer, rom_path);
          if (launch) return finish(true);
          if (quit_requested) return finish(false);
          SDL_DestroyTexture(background);
          background = load_theme_background(renderer);
          status.clear();
        }
      } else if (event.key.keysym.sym == SDLK_ESCAPE) {
        int action = station_escape_menu(renderer);
        if (action == 0) {
          hotkeys_menu(renderer);
        } else if (action == 1) {
          patch_notes_menu(renderer);
          if (!quit_requested)
            action = station_escape_menu(renderer);
        } else if (action == 2 || quit_requested) {
          return finish(false);
        }
        if (action == 2) return finish(false);
        if (quit_requested) return finish(false);
        SDL_DestroyTexture(background);
        background = load_theme_background(renderer);
      }
    }

    draw_frontend_background(renderer, background);
    SDL_SetRenderDrawColor(renderer, 117, 48, 87, 255);
    const SDL_Rect station_title{195, 60, 410, 70};
    draw_glow_rect(renderer, station_title, 12);
    draw_round_rect(renderer, station_title, 12);
    SDL_SetRenderDrawColor(renderer, 255, 232, 241, 255);
    const SDL_Rect station_title_inner{205, 70, 390, 50};
    draw_round_rect(renderer, station_title_inner, 8);
    draw_text_centered(renderer, "FLOWMULATOR", 400, 77, 5,
                       {117, 48, 87, 255});

    constexpr const char *systems[] = {"GBA", "NDS", "3DS", "WII", "GAMECUBE"};
    constexpr int card_y[] = {180, 274, 368, 462, 556};
    for (int i = 0; i < 5; ++i) {
      SDL_Rect card{250, card_y[i], 300, 72};
      const bool item_selected = menu_selection_visible && i == selected;
      if (berserk_theme())
        set_theme_draw_color(renderer, item_selected ? 255 : 6,
                             item_selected ? 255 : 24,
                             item_selected ? 255 : 63, 255);
      else
        SDL_SetRenderDrawColor(renderer, item_selected ? 117 : 67,
                               item_selected ? 48 : 30,
                               item_selected ? 87 : 48, 255);
      if (item_selected) draw_glow_rect(renderer, card, 12);
      if (berserk_theme())
        set_theme_draw_color(renderer, item_selected ? 255 : 6,
                             item_selected ? 255 : 24,
                             item_selected ? 255 : 63, 255);
      else
        SDL_SetRenderDrawColor(renderer, item_selected ? 117 : 67,
                               item_selected ? 48 : 30,
                               item_selected ? 87 : 48, 255);
      draw_round_rect(renderer, card, 12);
      draw_text_centered_mid_scale(
          renderer, systems[i], 400, card_y[i] + 23,
          berserk_theme() ? SDL_Color{221, 34, 80, 255} :
          item_selected ? SDL_Color{255, 232, 241, 255}
                        : SDL_Color{220, 166, 187, 255});
    }
    if (!status.empty())
      draw_text_centered(renderer, status, 400, 690, 2,
                         {245, 145, 175, 255});
    draw_text_centered(renderer, "MADE BY FLOWKIDD", 400, 746, 2,
                       berserk_theme() ? SDL_Color{245, 205, 217, 255}
                                       : SDL_Color{117, 48, 87, 255});
    draw_text(renderer, "BUILD V0.8.1 BETA", 40, 746, 2,
              {180, 110, 145, 255});
    draw_theme_footer(renderer, 746);
    draw_easter_eggs_notice(renderer);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
  return finish(false);
}
}  // namespace

int main(int argc, char **argv) {
  if (argc == 2 && std::string(argv[1]) == "--install-icon")
    return install_application_icon() ? 0 : 1;
  if (!extract_embedded_runtime()) {
    std::cerr << "Flowmulator could not prepare its embedded runtime\n";
    return 1;
  }
  if (!install_dependencies_once()) {
    std::cerr << "Flowmulator could not install its one-time dependencies\n";
    return 1;
  }
  load_controls();
  load_nds_controls();
  load_three_ds_resolution();
  load_wii_port_count();
  load_wii_controls();
  if (profile_files(ProfileKind::Gba).empty()) create_profile(ProfileKind::Gba);
  if (profile_files(ProfileKind::Nds).empty()) create_profile(ProfileKind::Nds);
  if (profile_files(ProfileKind::Wii).empty()) create_profile(ProfileKind::Wii);
  const bool return_to_wii_menu = argc == 2 &&
                                  std::string(argv[1]) == "--wii-menu";
  const bool return_to_gamecube_menu = argc == 2 &&
                                       std::string(argv[1]) == "--gamecube-menu";
  const bool return_to_3ds_menu = argc == 2 &&
                                  std::string(argv[1]) == "--3ds-menu";
  std::string rom_path =
      argc == 2 && !return_to_wii_menu && !return_to_gamecube_menu &&
              !return_to_3ds_menu
          ? argv[1] : "";
  if (argc > 2 || (argc == 2 && !return_to_wii_menu &&
                   !return_to_gamecube_menu && !return_to_3ds_menu &&
                   rom_path.empty())) {
    std::cerr << "Usage: Flowmulator ROM.gba|ROM.nds|ROM.iso\n";
    return 2;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
    std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
    return 1;
  }
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (SDL_IsGameController(i)) {
      game_controller = SDL_GameControllerOpen(i);
      if (game_controller) {
        const char *name = SDL_GameControllerName(game_controller);
        game_controller_name = name ? name : "";
        break;
      }
    }
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_Window *window = SDL_CreateWindow("Flowmulator", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, FRONTEND_WIDTH, FRONTEND_HEIGHT, SDL_WINDOW_SHOWN);
  theme_window = window;
  if (window) SDL_SetWindowTitle(window, theme_window_title());
  if (window) SDL_SetWindowOpacity(window, 1.0f);
  if (window)
    apply_hyprland_window_layout(FRONTEND_WIDTH, FRONTEND_HEIGHT);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (renderer) SDL_RenderSetLogicalSize(renderer, FRONTEND_WIDTH, FRONTEND_HEIGHT);
  bool frontend_result = true;
  if (window && renderer && rom_path.empty()) {
    if (return_to_wii_menu || return_to_gamecube_menu) {
      frontend_result = wii_frontend_menu(window, renderer, rom_path,
                                          return_to_gamecube_menu);
    } else if (return_to_3ds_menu) {
      frontend_result = nds_frontend_menu(window, renderer, rom_path, true);
    } else {
      frontend_result = menu(window, renderer, rom_path);
    }
  }
  if (!window || !renderer) {
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
  }
  if (!frontend_result) {
    if (return_to_wii_menu || return_to_gamecube_menu || return_to_3ds_menu) {
      if (return_to_3ds_menu && frontend_escape_requested)
        quit_requested = false;
      if (!quit_requested) {
        frontend_escape_requested = false;
        rom_path.clear();
        frontend_result = menu(window, renderer, rom_path);
        if (!frontend_result) {
          SDL_DestroyRenderer(renderer);
          SDL_DestroyWindow(window);
          SDL_Quit();
          return 0;
        }
      } else {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
      }
    } else {
      SDL_DestroyRenderer(renderer);
      SDL_DestroyWindow(window);
      SDL_Quit();
      return 0;
    }
  }
  const int launch_display_index = SDL_GetWindowDisplayIndex(window);
  SDL_DisplayMode launch_display_mode{};
  const bool have_launch_display =
      launch_display_index >= 0 &&
      SDL_GetDesktopDisplayMode(launch_display_index, &launch_display_mode) == 0;
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);

  const std::string selected_extension =
      std::filesystem::path(rom_path).extension().string();
  const bool selected_3ds =
      selected_extension == ".3ds" || selected_extension == ".cia" ||
      selected_extension == ".cci" || selected_extension == ".cxi";
#ifndef _WIN32
  if (selected_3ds) {
    const std::filesystem::path azahar =
        runtime_directory() / "azahar.AppImage";
    if (!std::filesystem::exists(azahar)) {
      std::cerr << "Azahar desktop emulator is missing: " << azahar << '\n';
      SDL_Quit();
      return 1;
    }
    const auto azahar_config_root =
        runtime_directory() / "azahar-config";
    if (!sync_azahar_user_data(azahar_config_root)) {
      std::cerr << "Could not prepare Azahar system data\n";
      SDL_Quit();
      return 1;
    }
    if (!sync_azahar_controls(azahar_config_root)) {
      std::cerr << "Could not prepare Azahar controls\n";
      SDL_Quit();
      return 1;
    }
    const std::filesystem::path absolute_rom =
        std::filesystem::absolute(rom_path);
    const pid_t pid = fork();
    if (pid < 0) {
      std::perror("Could not start Azahar");
      SDL_Quit();
      return 1;
    }
    if (pid == 0) {
      setpgid(0, 0);
      setenv("XDG_CONFIG_HOME", azahar_config_root.c_str(), 1);
      setenv("XDG_DATA_HOME", azahar_config_root.c_str(), 1);
      execl(azahar.c_str(), azahar.filename().c_str(),
            "--appimage-extract-and-run", "-f",
            absolute_rom.c_str(),
            static_cast<char *>(nullptr));
      std::perror("Could not start Azahar");
      _exit(127);
    }
    const pid_t window_fix_pid = fork();
    if (window_fix_pid == 0) {
      for (int attempt = 0; attempt < 100; ++attempt) {
        const int found = std::system(
            "hyprctl clients -j | grep -q 'org.azahar_emu.Azahar'");
        if (found == 0) {
          std::system(
              "hyprctl dispatch setfloating active >/dev/null 2>&1");
          apply_emulator_border_size("org.azahar_emu.Azahar");
          const auto azahar_size = scaled_window_size(730, 876);
          const std::string azahar_resize =
              "hyprctl dispatch resizeactive exact " +
              std::to_string(azahar_size.first) + " " +
              std::to_string(azahar_size.second) +
              " >/dev/null 2>&1";
          std::system(
              azahar_resize.c_str());
          std::system(
              "hyprctl dispatch centerwindow >/dev/null 2>&1");
          std::system(
              "hyprctl dispatch "
              "'focuswindow class:^(org.azahar_emu.Azahar)$' >/dev/null "
              "2>&1");
          std::system(
              "hyprctl dispatch "
              "'hl.dsp.window.fullscreen_state({ internal = 0, client = 0 })' "
              ">/dev/null 2>&1");
          usleep(150000);
          std::system(azahar_resize.c_str());
          _exit(0);
        }
        usleep(100000);
      }
      _exit(0);
    }
    int status = 0;
    bool azahar_window_seen = false;
    const bool watch_azahar_window =
        std::getenv("HYPRLAND_INSTANCE_SIGNATURE") != nullptr;
    if (watch_azahar_window) {
      for (;;) {
        const bool window_visible =
            std::system("hyprctl clients -j 2>/dev/null | "
                        "grep -q 'org.azahar_emu.Azahar'") == 0;
        azahar_window_seen = azahar_window_seen || window_visible;
        const pid_t process_state = waitpid(pid, &status, WNOHANG);
        if (process_state == pid ||
            (azahar_window_seen && !window_visible)) {
          if (process_state == 0) {
            kill(-pid, SIGTERM);
            waitpid(pid, &status, WNOHANG);
          }
          break;
        }
        if (process_state < 0 && errno != EINTR) break;
        usleep(50000);
      }
    } else {
      while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
    if (window_fix_pid > 0) {
      if (waitpid(window_fix_pid, nullptr, WNOHANG) == 0) {
        kill(window_fix_pid, SIGTERM);
        waitpid(window_fix_pid, nullptr, 0);
      }
    }
    SDL_Quit();
    const std::filesystem::path flowmulator =
        executable_directory() / "Flowmulator";
    execl(flowmulator.c_str(), flowmulator.filename().c_str(),
          "--3ds-menu",
          static_cast<char *>(nullptr));
    std::perror("Could not return to Flowmulator");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
  }
#endif
  if (selected_extension == ".iso" || selected_extension == ".wbfs" ||
      selected_extension == ".rvz" || selected_extension == ".gcm" ||
      selected_extension == ".gcz" || selected_extension == ".gamecube") {
    wii_active = true;
    if (!have_launch_display) {
      std::cerr << "Could not determine the active display resolution: "
                << SDL_GetError() << '\n';
      SDL_Quit();
      return 1;
    }
    const std::filesystem::path absolute_rom =
        std::filesystem::absolute(rom_path);
    const bool selected_from_gamecube_folder =
        absolute_rom.parent_path().filename() == "GAMECUBE";
    const bool gamecube = gamecube_active || selected_from_gamecube_folder ||
                          selected_extension == ".gcm" ||
                          selected_extension == ".gcz";
    const auto dolphin_size = scaled_window_size(1594, 872);
    const int render_width = dolphin_size.first;
    const int render_height = dolphin_size.second;
    const std::string window_width =
        "Dolphin.Display.RenderWindowWidth=" + std::to_string(render_width);
    const std::string window_height =
        "Dolphin.Display.RenderWindowHeight=" + std::to_string(render_height);
    const std::string internal_resolution =
        "GFX.InternalResolution=" + std::to_string(wii_resolution);
    if (!write_dolphin_window_size(render_width, render_height)) {
      std::cerr << "Could not save Dolphin window size\n";
      SDL_Quit();
      return 1;
    }
    if (!write_dolphin_wii_controls(gamecube)) {
      std::cerr << "Could not save Dolphin controller controls\n";
      SDL_Quit();
      return 1;
    }
    if (!gamecube && !enable_dolphin_wiimote_scanning()) {
      std::cerr << "Could not enable Dolphin Wii Remote continuous scanning\n";
      SDL_Quit();
      return 1;
    }
    int bluetooth_scan_pid = -1;
    if (!gamecube && wii_input_mode == InputMode::RealWiimote)
      bluetooth_scan_pid = prepare_bluetooth_wiimote_scan();
    if (!write_dolphin_wii_resolution(gamecube)) {
      std::cerr << "Could not save Dolphin Wii resolution\n";
      SDL_Quit();
      return 1;
    }
    if (!write_dolphin_wii_hotkeys()) {
      std::cerr << "Could not save Dolphin Wii hotkeys\n";
      SDL_Quit();
      return 1;
    }
    if (!disable_dolphin_osd_messages()) {
      std::cerr << "Could not disable Dolphin on-screen messages\n";
      SDL_Quit();
      return 1;
    }
 #ifdef _WIN32
    std::filesystem::path dolphin = executable_directory() / "Dolphin.exe";
    if (!std::filesystem::exists(dolphin)) dolphin = "Dolphin.exe";
    std::string dolphin_command = "\"" + dolphin.string() + "\" -b -v Vulkan";
    dolphin_command += " -C \"Dolphin.Display.FullscreenDisplayRes=Auto\"";
    dolphin_command += " -C \"Dolphin.Display.Fullscreen=False\"";
    dolphin_command += " -C \"" + window_width + "\"";
    dolphin_command += " -C \"" + window_height + "\"";
    dolphin_command += std::string(" -C \"Dolphin.Display.RenderWindowAutoSize=") +
                       "False\"";
    dolphin_command +=
        " -C \"Dolphin.Core.WiimoteContinuousScanning=True\"";
    dolphin_command += " -C \"Dolphin.Core.Wiimote1=1\"";
    dolphin_command += " -C \"Dolphin.Wiimote1.Source=2\"";
    dolphin_command += " -C \"Interface.ConfirmStop=False\"";
    dolphin_command += " -C \"GFX.BackendMultithreading=False\"";
    dolphin_command += " -C \"" + internal_resolution + "\"";
    dolphin_command += " -e \"" + absolute_rom.string() + "\"";
    const int dolphin_result = std::system(dolphin_command.c_str());
    stop_bluetooth_wiimote_scan(bluetooth_scan_pid);
    if (dolphin_result != 0) {
      std::cerr << "Could not launch Dolphin (exit code "
                << dolphin_result << ")\n";
      SDL_Quit();
      return 1;
    }
    SDL_Quit();
    const std::filesystem::path flowmulator =
        executable_directory() / "Flowmulator.exe";
    std::string return_command = "\"" + flowmulator.string() + "\" " +
                                 (gamecube ? "--gamecube-menu"
                                           : "--wii-menu");
    return std::system(return_command.c_str());
 #else
    const auto launch_dolphin = [&] {
      const pid_t pid = fork();
      if (pid == 0) {
        execl("/usr/bin/dolphin-emu", "dolphin-emu", "-b",
              "-v", "Vulkan",
              "-C", "Dolphin.Display.FullscreenDisplayRes=Auto",
              "-C", "Dolphin.Display.Fullscreen=False",
              "-C", window_width.c_str(),
              "-C", window_height.c_str(),
              "-C", "Dolphin.Display.RenderWindowAutoSize=False",
              "-C", "Dolphin.Core.WiimoteContinuousScanning=True",
              "-C", "Dolphin.Core.Wiimote1=1",
              "-C", "Dolphin.Wiimote1.Source=2",
              "-C", "Interface.ConfirmStop=False",
              "-C", "GFX.BackendMultithreading=False",
              "-C", internal_resolution.c_str(),
              "-e", absolute_rom.c_str(),
              static_cast<char *>(nullptr));
        std::perror("Could not launch Dolphin");
        _exit(127);
      }
      return pid;
    };
    pid_t dolphin_pid = launch_dolphin();
    if (dolphin_pid < 0) {
      stop_bluetooth_wiimote_scan(bluetooth_scan_pid);
      std::perror("Could not start Dolphin");
      SDL_Quit();
      return 1;
    }
    int dolphin_status = 0;
    bool dolphin_window_seen = false;
    const Uint32 launch_started = SDL_GetTicks();
    for (;;) {
      bool window_visible = false;
      FILE *clients = popen("hyprctl clients -j 2>/dev/null", "r");
      if (clients) {
        char buffer[1024];
        while (std::fgets(buffer, sizeof(buffer), clients)) {
          if (std::strstr(buffer, "dolphin-emu") != nullptr) {
            window_visible = true;
            break;
          }
        }
        pclose(clients);
      }
      if (window_visible && !dolphin_window_seen) {
        std::system(
            "hyprctl dispatch setfloating active >/dev/null 2>&1");
        apply_emulator_border_size("dolphin-emu");
        std::system(
            ("hyprctl dispatch resizeactive exact " + window_width + " " +
             window_height + " >/dev/null 2>&1").c_str());
        std::system(
            "hyprctl dispatch centerwindow >/dev/null 2>&1");
        usleep(150000);
        std::system(
            ("hyprctl dispatch resizeactive exact " + window_width + " " +
             window_height + " >/dev/null 2>&1").c_str());
      }
      dolphin_window_seen = dolphin_window_seen || window_visible;
      const bool startup_timeout =
          SDL_GetTicks() - launch_started >= 15000;
      if ((dolphin_window_seen && !window_visible) ||
          (!dolphin_window_seen && startup_timeout))
        break;
      usleep(50000);
    }
    while (waitpid(dolphin_pid, &dolphin_status, WNOHANG) < 0 &&
           errno == EINTR) {
      usleep(1000);
    }
    stop_bluetooth_wiimote_scan(bluetooth_scan_pid);
    SDL_Quit();
    const std::filesystem::path flowmulator =
        executable_directory() / "Flowmulator";
    const char *return_menu = gamecube ? "--gamecube-menu" : "--wii-menu";
    execl(flowmulator.c_str(), flowmulator.filename().c_str(), return_menu,
          static_cast<char *>(nullptr));
    std::perror("Could not return to Flowmulator");
    return WIFEXITED(dolphin_status) ? WEXITSTATUS(dolphin_status) : 1;
 #endif
  }
  bool return_to_rom_selector = false;
  for (;;) {
  Core core;
  const std::string rom_extension = std::filesystem::path(rom_path).extension().string();
  nds_active = rom_extension == ".nds" || rom_extension == ".3ds" ||
               rom_extension == ".cia" || rom_extension == ".cci" ||
               rom_extension == ".cxi";
  three_ds_active = rom_extension == ".3ds" || rom_extension == ".cia" ||
                    rom_extension == ".cci" || rom_extension == ".cxi";
  wii_active = rom_extension == ".iso" || rom_extension == ".wbfs" ||
               rom_extension == ".rvz";
  crt_filter = nds_active ? nds_crt_filter : gba_crt_filter;
  mouse_x = 0;
  mouse_y = 0;
  mouse_delta_x = 0;
  mouse_delta_y = 0;
  mouse_pressed = false;
  logged_3ds_frame = false;
  three_ds_hardware_render = false;
  auto core_path = find_libretro_core(
      three_ds_active
#ifdef _WIN32
          ? "citra_libretro.dll"
          : nds_active ? "desmume_libretro.dll" : "mgba_libretro.dll"
#else
          ? "citra_libretro.so"
          : nds_active ? "desmume_libretro.so" : "mgba_libretro.so"
#endif
  );
  if (three_ds_active && core_path.empty())
    core_path = find_libretro_core(
#ifdef _WIN32
        "azahar_libretro.dll"
#else
        "azahar_libretro.so"
#endif
    );
  core.handle = core_path.empty()
      ? nullptr : open_dynamic_library(core_path);
  if (!core.handle) {
    std::cerr << "Cannot load "
              << (three_ds_active ? "Citra" :
                  nds_active ? "DeSmuME" : wii_active ? "Dolphin" : "mGBA")
              << " core: " << dynamic_library_error() << '\n';
    return 1;
  }
  bool loaded = core.load(core.init, "retro_init") &&
                core.load(core.deinit, "retro_deinit") &&
                core.load(core.get_system_info, "retro_get_system_info") &&
                core.load(core.get_system_av_info, "retro_get_system_av_info") &&
                core.load(core.set_environment, "retro_set_environment") &&
                core.load(core.set_video_refresh, "retro_set_video_refresh") &&
                core.load(core.set_audio_sample, "retro_set_audio_sample") &&
                core.load(core.set_audio_sample_batch, "retro_set_audio_sample_batch") &&
                core.load(core.set_input_poll, "retro_set_input_poll") &&
                core.load(core.set_input_state, "retro_set_input_state") &&
                core.load(core.load_game, "retro_load_game") &&
                core.load(core.unload_game, "retro_unload_game") &&
                core.load(core.run, "retro_run") &&
                core.load(core.get_memory_size, "retro_get_memory_size") &&
                core.load(core.get_memory_data, "retro_get_memory_data");
  if (!loaded) {
    std::cerr << (three_ds_active ? "Citra" :
                  nds_active ? "DeSmuME" : wii_active ? "Dolphin" : "mGBA")
              << " core is missing a required libretro entry point\n";
    close_dynamic_library(core.handle);
    return 1;
  }

  const std::filesystem::path rom_file(rom_path);
  const std::filesystem::path project_root = executable_directory();
  const std::filesystem::path save_directory = project_root / "SAVES";
  std::error_code save_directory_error;
  std::filesystem::create_directories(save_directory, save_directory_error);
  if (save_directory_error) {
    std::cerr << "Could not create save directory: "
              << save_directory_error.message() << '\n';
    core.deinit();
    close_dynamic_library(core.handle);
    return 1;
  }
  core_save_directory = save_directory.string();
  core_system_directory.clear();
  if (wii_active) {
    const std::filesystem::path core_system_root =
        runtime_directory();
    const std::filesystem::path dolphin_system =
        core_system_root / "dolphin-emu";
    const std::filesystem::path dolphin_sys = dolphin_system / "Sys";
    std::error_code system_error;
    std::filesystem::create_directories(dolphin_system, system_error);
    if (system_error) {
      std::cerr << "Could not create Dolphin core system directory: "
                << system_error.message() << '\n';
      core.deinit();
      close_dynamic_library(core.handle);
      return 1;
    }
    if (!std::filesystem::exists(dolphin_sys)) {
      std::filesystem::create_directory_symlink(
          "/usr/share/dolphin-emu/sys", dolphin_sys, system_error);
      if (system_error) {
        std::cerr << "Could not link Dolphin Sys directory: "
                  << system_error.message() << '\n';
        core.deinit();
        close_dynamic_library(core.handle);
        return 1;
      }
    }
  }
  const bool gba_overlay_start = !nds_active && !wii_active && gba_overlay;
  const int emulator_window_width = nds_active ? 640 : 960;
  const int emulator_window_height =
      nds_active ? 960 : wii_active ? 720 : gba_overlay_start ? 540 : 640;
  const Uint32 window_flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN |
                              (wii_vulkan ? SDL_WINDOW_VULKAN
                                          : SDL_WINDOW_OPENGL);
  window = SDL_CreateWindow("Flowmulator", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, emulator_window_width, emulator_window_height,
      window_flags);
  emulator_window = window;
  gl_context = wii_vulkan ? nullptr : (window ? SDL_GL_CreateContext(window) : nullptr);
  if (window) {
    SDL_SetWindowBordered(window, SDL_TRUE);
    SDL_SetWindowResizable(window, SDL_TRUE);
    SDL_SetWindowOpacity(window, 1.0f);
    SDL_SetWindowTitle(window, emulator_window_title());
    SDL_ShowWindow(window);
    SDL_PumpEvents();
    apply_hyprland_window_layout(emulator_window_width,
                                  emulator_window_height);
    SDL_Delay(50);
  }
  if (gl_context) SDL_GL_SetSwapInterval(vsync_enabled ? 1 : 0);
  if (gl_context) {
    glClearColor(0.04f, 0.04f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);
  }
  VulkanHost vulkan_host;
  if (wii_vulkan && window && !vulkan_host.initialize(window)) {
    std::cerr << "Could not initialize the direct Wii Vulkan host\n";
  }
  if (!window || (!wii_vulkan && !gl_context)) {
    if (gl_context) SDL_GL_DeleteContext(gl_context);
    if (window) SDL_DestroyWindow(window);
    close_dynamic_library(core.handle);
    return 1;
  }
  if (active_input_mode() == InputMode::Controller && !game_controller)
      std::cerr << "No SDL game controller detected; controller input unavailable\n";
  core.set_environment(environment);
  core.set_video_refresh(video_refresh);
  core.set_audio_sample(audio_sample);
  core.set_audio_sample_batch(audio_batch);
  core.set_input_poll(input_poll);
  core.set_input_state(input_state);
  if (nds_layout_index < 0)
    nds_layout_index = 0;
  core.init();

  retro_system_info info{};
  core.get_system_info(&info);
  std::cerr << "Loaded " << (info.library_name
                                 ? info.library_name
                                 : (nds_active ? "DeSmuME core"
                                               : wii_active ? "Dolphin core"
                                                            : "GBA core"))
            << " " << (info.library_version ? info.library_version : "") << '\n';

  retro_game_info game{rom_path.c_str(), nullptr, 0, nullptr};
  std::vector<uint8_t> rom;
  if (!wii_active) {
    std::ifstream input(rom_path, std::ios::binary | std::ios::ate);
    if (!input) {
      std::cerr << "Cannot open ROM: " << rom_path << '\n';
      core.deinit();
      close_dynamic_library(core.handle);
      return 1;
    }
    const auto size = input.tellg();
    rom.resize(static_cast<size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char *>(rom.data()), size);
    game.data = rom.data();
    game.size = rom.size();
  } else if (!std::filesystem::is_regular_file(rom_path)) {
    std::cerr << "Cannot open Wii ROM: " << rom_path << '\n';
    core.deinit();
    close_dynamic_library(core.handle);
    return 1;
  }
  game.path = rom_path.c_str();
  std::atomic<bool> load_finished = false;
  retro_bool load_result = false;
  std::thread loader([&] {
    load_result = core.load_game(&game);
    load_finished.store(true, std::memory_order_release);
  });
  while (!load_finished.load(std::memory_order_acquire)) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        SDL_HideWindow(window);
    }
    SDL_Delay(16);
  }
  loader.join();
  if (!load_result) {
    std::cerr << (three_ds_active ? "Citra" :
                  nds_active ? "DeSmuME" : wii_active ? "Dolphin" : "mGBA")
              << " refused to load the ROM\n";
    core.deinit();
    close_dynamic_library(core.handle);
    return 1;
  }
  if (wii_vulkan && (!vulkan_host.activate() || !vulkan_host.ready())) {
    std::cerr << "Dolphin did not negotiate a Vulkan device\n";
    core.unload_game();
    core.deinit();
    vulkan_host.shutdown();
    if (window) SDL_DestroyWindow(window);
    close_dynamic_library(core.handle);
    return 1;
  }
  const unsigned retro_memory_save_ram = 0;
  const size_t save_size = core.get_memory_size(retro_memory_save_ram);
  void *save_data = core.get_memory_data(retro_memory_save_ram);
  std::filesystem::path save_filename = rom_file.filename();
  save_filename.replace_extension(".sav");
  const std::filesystem::path save_path = project_root / "SAVES" / save_filename;
  if (save_data && save_size) {
    std::ifstream save_input(save_path, std::ios::binary);
    if (save_input) {
      save_input.read(static_cast<char *>(save_data),
                      static_cast<std::streamsize>(save_size));
      std::cerr << "Loaded save: " << save_path << '\n';
    }
  }

  retro_system_av_info av{};
  core.get_system_av_info(&av);
  frame_width = av.geometry.base_width;
  frame_height = av.geometry.base_height;
  audio_rate = static_cast<unsigned>(av.timing.sample_rate > 0 ? av.timing.sample_rate : 32768);
  framebuffer.resize(static_cast<size_t>(frame_width) * frame_height);

  SDL_SetWindowFullscreen(window, 0);
  if (window) {
    SDL_SetWindowBordered(window, SDL_TRUE);
    SDL_SetWindowResizable(window, SDL_TRUE);
    SDL_SetWindowOpacity(window, 1.0f);
    int native_window_width = emulator_window_width;
    int native_window_height = emulator_window_height;
    if (nds_active && nds_overlay) {
      native_window_width = 960;
      native_window_height = 540;
    } else if (!wii_active && gba_overlay && !nds_active) {
      // The GBA overlay is a 1920x1080 canvas, so start its floating
      // emulator window at the same 16:9 aspect ratio.
      native_window_width = 960;
      native_window_height = 540;
    } else if (!wii_active && frame_width && frame_height) {
      native_window_height = nds_active ? 960 : 640;
      native_window_width = std::max(
          1, static_cast<int>(native_window_height * frame_width /
                              frame_height));
    }
    SDL_SetWindowSize(window, native_window_width, native_window_height);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED);
  }
  const GLuint crt_program = gl_context ? create_crt_program() : 0;
  GLuint gl_texture = 0;
  GLuint gba_overlay_texture = 0;
  GLuint nds_overlay_texture = 0;
  GLuint nds_top_overlay_texture = 0;
  unsigned texture_width = 0;
  unsigned texture_height = 0;
  if (gl_context) {
    glGenTextures(1, &gl_texture);
    glBindTexture(GL_TEXTURE_2D, gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame_width, frame_height, 0, GL_BGRA,
                 GL_UNSIGNED_BYTE, framebuffer.data());
    texture_width = frame_width;
    texture_height = frame_height;
    if (!nds_active && !wii_active && gba_overlay)
      gba_overlay_texture = load_gba_overlay_texture();
    if (nds_active && nds_overlay)
      nds_overlay_texture = load_nds_overlay_texture();
    if (nds_active && nds_overlay && nds_layout_index != 0)
      nds_top_overlay_texture = load_nds_top_overlay_texture();
    if (drilboor_effect_active() || title_edition == 1)
      load_jackhammer_texture();
    if (skywalker_effect_active())
      load_skywalker_texture();
    if (skywalker_effect_active())
      load_water_splash_texture();
  }
  SDL_AudioSpec audio_spec{};
  SDL_AudioSpec obtained_audio_spec{};
  audio_spec.freq = static_cast<int>(audio_rate);
  audio_spec.format = AUDIO_S16SYS;
  audio_spec.channels = 2;
  audio_spec.samples = 1024;
  audio_device = SDL_OpenAudioDevice(nullptr, 0, &audio_spec,
                                     &obtained_audio_spec, 0);
  if (audio_device) {
    audio_rate = static_cast<unsigned>(obtained_audio_spec.freq);
    load_jackhammer_audio(obtained_audio_spec);
    if (!load_audio_clip(
            _binary__assets_big_forehead_wav_start,
            embedded_size(_binary__assets_big_forehead_wav_start,
                          _binary__assets_big_forehead_wav_end),
            obtained_audio_spec, big_forehead_clip))
      std::cerr << "Could not load Big Forehead audio\n";
    if (!load_skywalker_audio(obtained_audio_spec))
      std::cerr << "Could not load Skywalker splash audio\n";
    if (!load_skywalker_oooooh_audio(obtained_audio_spec))
      std::cerr << "Could not load Skywalker center audio\n";
    big_forehead_position = 0;
    SDL_PauseAudioDevice(audio_device, 0);
  }
  bool running = window &&
                 (wii_vulkan ? vulkan_host.ready()
                             : gl_context && crt_program && gl_texture);
  jackhammer_session_start = static_cast<double>(SDL_GetPerformanceCounter()) /
                             SDL_GetPerformanceFrequency();
  jackhammer_spawn_times.clear();
  const double fps = av.timing.fps > 1.0 ? av.timing.fps : 59.7275;
  const uint64_t ticks_per_frame =
      static_cast<uint64_t>(SDL_GetPerformanceFrequency() / fps);
  uint64_t next_frame = SDL_GetPerformanceCounter();
  uint64_t fps_window_start = next_frame;
  unsigned fps_frames = 0;
  double displayed_fps = 0.0;
  bool escape_pending = false;
  Uint32 escape_pending_since = 0;
  std::vector<CookedPopup> cooked_popups;
  const Uint32 cooked_gameplay_started = SDL_GetTicks();
  Uint32 cooked_next_popup = cooked_gameplay_started + 20000;
  size_t cooked_popup_kind = 0;
  Uint32 skywalker_next_event = cooked_gameplay_started + 10000 +
      std::uniform_int_distribution<Uint32>(0, 15000)(jackhammer_rng);
  CookedPopup cooked_wrong_popup;
  Uint32 cooked_wrong_popup_until = 0;
  const bool cooked_gameplay = ur_cooked_effect_active() &&
                               !wii_active && !three_ds_active;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_KEYDOWN && !event.key.repeat)
        process_easter_egg_sequence(event.key.keysym.sym);
      if (event.type == SDL_QUIT) {
        running = false;
      }
      if (event.type == SDL_WINDOWEVENT &&
          event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
      }
      if (nds_active && event.type == SDL_MOUSEMOTION) {
        mouse_x = event.motion.x;
        mouse_y = event.motion.y;
        mouse_delta_x = std::clamp(mouse_delta_x + event.motion.xrel,
                                   -32768, 32767);
        mouse_delta_y = std::clamp(mouse_delta_y + event.motion.yrel,
                                   -32768, 32767);
      }
      if (nds_active && event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT) {
        mouse_x = event.button.x;
        mouse_y = event.button.y;
        mouse_pressed = true;
      }
      if (nds_active && event.type == SDL_MOUSEBUTTONUP &&
          event.button.button == SDL_BUTTON_LEFT) {
        mouse_x = event.button.x;
        mouse_y = event.button.y;
        mouse_pressed = false;
      }
      if (cooked_gameplay &&
          (event.type == SDL_MOUSEMOTION ||
           (event.type == SDL_MOUSEBUTTONDOWN &&
            event.button.button == SDL_BUTTON_LEFT))) {
        const int pointer_x = event.type == SDL_MOUSEMOTION
            ? event.motion.x : event.button.x;
        const int pointer_y = event.type == SDL_MOUSEMOTION
            ? event.motion.y : event.button.y;
        for (auto &popup : cooked_popups) {
          const int option_y = popup.y + popup.height - 52;
          const int option_width = std::max(
              {cooked_option_width(cooked_options[popup.kind][0]),
               cooked_option_width(cooked_options[popup.kind][1]),
               cooked_option_width(cooked_options[popup.kind][2])});
          const int option_spacing = option_width + 11;
          if (pointer_x < popup.x || pointer_x >= popup.x + popup.width ||
              pointer_y < popup.y || pointer_y >= popup.y + popup.height) {
            if (event.type == SDL_MOUSEMOTION)
              popup.hovered_option = -1;
            continue;
          }
          const bool on_option = pointer_y >= option_y &&
              pointer_y < option_y + 28 &&
              pointer_x >= popup.x + 18 &&
              pointer_x < popup.x + 18 + 3 * option_spacing;
          if (event.type == SDL_MOUSEMOTION) {
            if (on_option) {
              const size_t hovered_option = static_cast<size_t>(
                  (pointer_x - popup.x - 18) / option_spacing);
              if (popup.hovered_option != static_cast<int>(hovered_option)) {
                popup.hovered_option = static_cast<int>(hovered_option);
                std::uniform_int_distribution<int> hover_swap_chance(0, 4);
                if (hover_swap_chance(jackhammer_rng) == 0) {
                  std::shuffle(popup.option_order.begin(),
                               popup.option_order.end(), jackhammer_rng);
                  for (size_t displayed = 0;
                       displayed < popup.option_order.size(); ++displayed) {
                    if (popup.option_order[displayed] ==
                        cooked_close_options[popup.kind])
                      popup.correct_option = displayed;
                  }
                }
              }
              popup.close_x = popup.x + 24 +
                  static_cast<int>((SDL_GetTicks() / 35) %
                                   std::max(1, popup.width - 120));
              popup.close_y = option_y;
            } else
              popup.hovered_option = -1;
          } else {
            if (on_option) {
              const size_t option = static_cast<size_t>(
                  (pointer_x - popup.x - 18) / option_spacing);
              if (option == popup.correct_option) {
                popup.active = false;
              } else {
                cooked_wrong_popup = popup;
                cooked_wrong_popup.active = true;
                cooked_wrong_popup.warning = true;
                cooked_wrong_popup_until = SDL_GetTicks() + 2000;
              }
            } else {
              cooked_wrong_popup = popup;
              cooked_wrong_popup.active = true;
              cooked_wrong_popup.warning = true;
              cooked_wrong_popup_until = SDL_GetTicks() + 2000;
            }
          }
          break;
        }
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_ESCAPE) {
        if (escape_pending) {
          running = false;
          return_to_rom_selector = true;
        } else {
          escape_pending = true;
          escape_pending_since = SDL_GetTicks();
        }
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F1 && !wii_active &&
          !gamecube_active) {
        crt_filter = !crt_filter;
        if (nds_active) nds_crt_filter = crt_filter;
        else gba_crt_filter = crt_filter;
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F11) {
        const Uint32 flags = SDL_GetWindowFlags(window);
        SDL_SetWindowFullscreen(window,
            (flags & SDL_WINDOW_FULLSCREEN) ? 0 : SDL_WINDOW_FULLSCREEN);
        if (!wii_vulkan)
          SDL_GL_SetSwapInterval(vsync_enabled ? 1 : 0);
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F2 && !wii_active &&
          !gamecube_active)
        show_fps = !show_fps;
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F3 && !wii_active &&
          !gamecube_active) {
        vsync_enabled = !vsync_enabled;
        if (!wii_vulkan)
          SDL_GL_SetSwapInterval(vsync_enabled ? 1 : 0);
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F4 && !wii_active &&
          !gamecube_active) {
        if (nds_active) {
          nds_overlay = !nds_overlay;
          save_nds_controls();
        } else {
          gba_overlay = !gba_overlay;
        }
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F5 && !wii_active &&
          !gamecube_active)
        toggle_active_input_mode();
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F6 && nds_active) {
        nds_layout_index =
            (nds_layout_index + 1) % 3;
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == turbo_key) {
        if (event.key.keysym.mod & KMOD_SHIFT) turbo_toggle = !turbo_toggle;
        else turbo_hold = true;
      }
      if (event.type == SDL_KEYUP && event.key.keysym.sym == turbo_key &&
          !(event.key.keysym.mod & KMOD_SHIFT))
        turbo_hold = false;
      if (active_input_mode() == InputMode::Keyboard &&
          (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
          event.key.keysym.sym != turbo_key)
        set_button(event.key.keysym.sym, event.type == SDL_KEYDOWN);
    }
    if (!nds_active && !wii_active && gba_overlay && !gba_overlay_texture)
      gba_overlay_texture = load_gba_overlay_texture();
    else if ((!gba_overlay || nds_active || wii_active) &&
             gba_overlay_texture) {
      glDeleteTextures(1, &gba_overlay_texture);
      gba_overlay_texture = 0;
    }
    if (nds_active && nds_overlay && !nds_overlay_texture)
      nds_overlay_texture = load_nds_overlay_texture();
    else if ((!nds_active || !nds_overlay) && nds_overlay_texture) {
      glDeleteTextures(1, &nds_overlay_texture);
      nds_overlay_texture = 0;
    }
    if (nds_active && nds_overlay && nds_layout_index != 0 &&
        !nds_top_overlay_texture)
      nds_top_overlay_texture = load_nds_top_overlay_texture();
    else if ((!nds_active || !nds_overlay || nds_layout_index == 0) &&
             nds_top_overlay_texture) {
      glDeleteTextures(1, &nds_top_overlay_texture);
      nds_top_overlay_texture = 0;
    }
    if (skywalker_effect_active() && !skywalker_texture)
      load_skywalker_texture();
    if (skywalker_effect_active() && !water_splash_texture)
      load_water_splash_texture();
    if (drilboor_effect_active() && !jackhammer_texture)
      load_jackhammer_texture();
    if (escape_pending &&
        SDL_GetTicks() - escape_pending_since >= 3000)
      escape_pending = false;
    cooked_popups.erase(
        std::remove_if(cooked_popups.begin(), cooked_popups.end(),
                       [](const CookedPopup &popup) { return !popup.active; }),
        cooked_popups.end());
    if (cooked_gameplay && cooked_popups.size() < 12 &&
        SDL_GetTicks() >= cooked_next_popup) {
      int output_width = 0;
      int output_height = 0;
      SDL_GetWindowSize(window, &output_width, &output_height);
      CookedPopup popup;
      popup.kind = cooked_popup_kind++ % std::size(cooked_titles);
      const int option_width = std::max(
          {cooked_option_width(cooked_options[popup.kind][0]),
           cooked_option_width(cooked_options[popup.kind][1]),
           cooked_option_width(cooked_options[popup.kind][2])});
      const int option_spacing = option_width + 11;
      popup.width = std::min(output_width - 40, std::max(
          430, 36 + 3 * option_spacing));
      popup.height = 150;
      std::uniform_int_distribution<int> popup_x(
          20, std::max(20, output_width - popup.width - 20));
      std::uniform_int_distribution<int> popup_y(
          20, std::max(20, output_height - popup.height - 20));
      popup.x = popup_x(jackhammer_rng);
      popup.y = popup_y(jackhammer_rng);
      popup.close_x = popup.x + popup.width - 96;
      popup.close_y = popup.y + popup.height - 52;
      popup.correct_option = cooked_close_options[popup.kind];
      popup.option_order = {0, 1, 2};
      std::uniform_int_distribution<int> troll_chance(0, 9);
      if (troll_chance(jackhammer_rng) == 0) {
        std::shuffle(popup.option_order.begin(), popup.option_order.end(),
                     jackhammer_rng);
        for (size_t displayed = 0; displayed < popup.option_order.size();
             ++displayed) {
          if (popup.option_order[displayed] ==
              cooked_close_options[popup.kind])
            popup.correct_option = displayed;
        }
      }
      popup.active = true;
      cooked_popups.push_back(popup);
      const Uint32 elapsed_ms = SDL_GetTicks() - cooked_gameplay_started;
      constexpr Uint32 initial_popup_interval_ms = 8000;
      constexpr Uint32 minimum_popup_interval_ms = 2500;
      constexpr Uint32 ramp_duration_ms = 20 * 60 * 1000;
      const Uint32 ramp_ms = std::min(elapsed_ms, ramp_duration_ms);
      const Uint32 interval_reduction =
          (initial_popup_interval_ms - minimum_popup_interval_ms) * ramp_ms /
          ramp_duration_ms;
      const Uint32 popup_interval_ms = std::clamp(
          initial_popup_interval_ms - interval_reduction,
          minimum_popup_interval_ms, initial_popup_interval_ms);
      cooked_next_popup = SDL_GetTicks() + popup_interval_ms;
    }
    const bool skywalker_due = SDL_GetTicks() >= skywalker_next_event;
    const double skywalker_animation_now =
        static_cast<double>(SDL_GetPerformanceCounter()) /
        SDL_GetPerformanceFrequency();
    const bool skywalker_splash_pause_active =
        std::any_of(skywalker_flights.begin(), skywalker_flights.end(),
                    [skywalker_animation_now](const SkywalkerFlight &flight) {
                      const double pause_start =
                          flight.slide_duration + flight.pause_duration +
                          flight.splash_duration;
                      const double pause_end =
                          pause_start + flight.splash_hold_duration;
                      const double elapsed =
                          skywalker_animation_now - flight.start_time;
                      return elapsed >= pause_start && elapsed < pause_end;
                    });
    const bool skywalker_overlap_allowed =
        skywalker_flights.empty() ||
        (skywalker_splash_pause_active &&
         std::uniform_int_distribution<int>(0, 99)(jackhammer_rng) < 5);
    if (skywalker_effect_active() && skywalker_texture && skywalker_due &&
        !skywalker_overlap_allowed)
      skywalker_next_event = SDL_GetTicks() + 1000;
    if (skywalker_effect_active() && skywalker_texture && skywalker_due &&
        skywalker_overlap_allowed) {
      int output_width = 0;
      int output_height = 0;
      SDL_GetWindowSize(window, &output_width, &output_height);
      SkywalkerFlight flight;
      const double now_seconds = static_cast<double>(
          SDL_GetPerformanceCounter()) / SDL_GetPerformanceFrequency();
      const float angle = std::uniform_real_distribution<float>(
          0.0f, 6.283185307f)(jackhammer_rng);
      flight.angle = angle;
      flight.start_time = now_seconds;
      flight.size = std::min(430.0f, output_height * 0.58f);
      if (skywalker_speed_bag_index >= skywalker_speed_bag.size()) {
        std::shuffle(skywalker_speed_bag.begin(), skywalker_speed_bag.end(),
                     jackhammer_rng);
        skywalker_speed_bag_index = 0;
      }
      int speed_tier = skywalker_speed_bag[skywalker_speed_bag_index++];
      if (skywalker_next_speed_slow_bias && speed_tier >= 3 &&
          std::uniform_int_distribution<int>(0, 99)(jackhammer_rng) >= 15) {
        speed_tier = std::uniform_int_distribution<int>(0, 2)(jackhammer_rng);
      }
      skywalker_next_speed_slow_bias = false;
      static constexpr double slide_durations[] =
          {2.8, 2.3, 1.9, 1.5, 1.05, 0.67};
      static constexpr double pause_durations[] =
          {3.4, 3.0, 2.6, 2.0, 1.5, 0.5};
      static constexpr double splash_durations[] =
          {3.1, 2.8, 2.5, 2.1, 1.7, 1.35};
      static constexpr float audio_speeds[] =
          {0.52f, 0.7f, 0.92f, 1.12f, 1.38f, 1.72f};
      flight.slide_duration = slide_durations[speed_tier];
      flight.pause_duration = pause_durations[speed_tier];
      flight.splash_duration = splash_durations[speed_tier];
      flight.splash_hold_duration = std::uniform_real_distribution<double>(
          2.0, 6.0)(jackhammer_rng);
      flight.splash_fade_duration = 1.25;
      skywalker_next_speed_slow_bias = flight.splash_hold_duration >= 4.0;
      flight.audio_speed = audio_speeds[speed_tier];
      const float distance = std::max(output_width, output_height) * 0.75f +
                             flight.size;
      flight.start_x = output_width * 0.5f - std::cos(angle) * distance;
      flight.start_y = output_height * 0.5f - std::sin(angle) * distance;
      const size_t splash_count = flight.splash.size() / 6;
      for (size_t i = 0; i < flight.splash.size(); i += 6) {
        const size_t splash_index = i / 6;
        const float sector_angle = 6.283185307f *
            static_cast<float>(splash_index) / splash_count;
        flight.splash[i] = sector_angle +
            std::uniform_real_distribution<float>(
                -0.045f, 0.045f)(jackhammer_rng);
        const float radial_band = static_cast<float>(splash_index % 6) / 5.0f;
        flight.splash[i + 1] = std::clamp(
            0.16f + radial_band * 1.08f +
                std::uniform_real_distribution<float>(
                    -0.12f, 0.12f)(jackhammer_rng),
            0.08f, 1.36f);
        flight.splash[i + 2] = std::uniform_real_distribution<float>(
            90.0f, 300.0f)(jackhammer_rng);
        flight.splash[i + 3] = std::uniform_real_distribution<float>(
            0.0f, 360.0f)(jackhammer_rng);
        flight.splash[i + 4] = std::uniform_real_distribution<float>(
            0.0f, 0.85f)(jackhammer_rng);
        flight.splash[i + 5] = std::uniform_real_distribution<float>(
            0.45f, 1.25f)(jackhammer_rng);
      }
      flight.center_sound_started = true;
      skywalker_voices.push_back({0, flight.audio_speed, 0.55f, true});
      skywalker_flights.push_back(flight);
      skywalker_next_event = SDL_GetTicks() + 10000 +
          std::uniform_int_distribution<Uint32>(0, 15000)(jackhammer_rng);
    }
    if (active_input_mode() == InputMode::Controller)
      update_controller_buttons();
    core.run();
    mouse_delta_x = 0;
    mouse_delta_y = 0;
    if (wii_active) {
      if (wii_vulkan)
        vulkan_host.present();
      else
        SDL_GL_SwapWindow(window);
      continue;
    }
    if (three_ds_active && three_ds_hardware_render) {
      SDL_GL_SwapWindow(window);
      continue;
    }
    int output_width = 0, output_height = 0;
    SDL_GetWindowSize(window, &output_width, &output_height);
    glViewport(0, 0, output_width, output_height);
    glClearColor(0, 0, 0, 1);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(0);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(-1, 1, -1, 1, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glColor3f(0, 0, 0);
    glBegin(GL_TRIANGLES);
    glVertex2f(-1, -1);
    glVertex2f(1, -1);
    glVertex2f(1, 1);
    glVertex2f(-1, -1);
    glVertex2f(1, 1);
    glVertex2f(-1, 1);
    glEnd();
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glUseProgram(crt_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl_texture);
    if (texture_width != frame_width || texture_height != frame_height) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame_width, frame_height, 0,
                   GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
      texture_width = frame_width;
      texture_height = frame_height;
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_width, frame_height,
                    GL_BGRA, GL_UNSIGNED_BYTE, framebuffer.data());
    const float overlay_scale = std::min(
        static_cast<float>(output_width) / 1920.0f,
        static_cast<float>(output_height) / 1080.0f);
    const float overlay_width = 1920.0f * overlay_scale;
    const float overlay_height = 1080.0f * overlay_scale;
    const float overlay_x = (output_width - overlay_width) * 0.5f;
    const float overlay_y = (output_height - overlay_height) * 0.5f;
    // Keep the DS framebuffer's aspect ratio in every window state. Tiling
    // changes the available area, but must not stretch the gameplay.
    const float aspect = wii_active
                         ? 4.0f / 3.0f
                         : static_cast<float>(frame_width) / frame_height;
    float width = static_cast<float>(output_width);
    float height = width / aspect;
    if (height > output_height) {
      height = static_cast<float>(output_height);
      width = height * aspect;
    }
    float screen_x = (output_width - width) * 0.5f;
    float screen_y = (output_height - height) * 0.5f;

    if (gba_overlay_texture) {
      const float cutout_x = overlay_x + 310.0f * overlay_scale;
      const float cutout_y = overlay_y + 105.0f * overlay_scale;
      const float cutout_width = 1300.0f * overlay_scale;
      const float cutout_height = 867.0f * overlay_scale;
      height = cutout_height;
      width = height * aspect;
      screen_x = cutout_x + (cutout_width - width) * 0.5f;
      screen_y = cutout_y;
    }
    const double animation_now =
        static_cast<double>(SDL_GetPerformanceCounter()) /
        SDL_GetPerformanceFrequency();
    const double difficulty_elapsed =
        std::max(0.0, animation_now - jackhammer_session_start);
    constexpr double difficulty_duration = 6.7 * 60.0;
    const size_t difficulty_level = std::min<size_t>(
        14, static_cast<size_t>(difficulty_elapsed * 14.0 / difficulty_duration));
    const float shake_intensity = jackhammer_flights.empty()
        ? 0.0f
        : static_cast<float>((0.03 + 0.97 * difficulty_level / 14.0) *
                             jackhammer_flights.size());
    const float jackhammer_impact =
        (drilboor_effect_active() || title_edition == 1) &&
        !jackhammer_flights.empty()
        ? static_cast<float>(std::sin(animation_now * 6.283185307 * 14.0) *
                             16.0 * shake_intensity)
        : 0.0f;
    const float shake = (drilboor_effect_active() || title_edition == 1)
        ? ((fps_frames & 1u) ? 24.0f : -24.0f) * shake_intensity + jackhammer_impact
        : 0.0f;
    screen_y += shake;
    const auto draw_frame = [&](float x, float y, float draw_width,
                                float draw_height, float source_y,
                                float source_height) {
      glUseProgram(crt_program);
      glBindTexture(GL_TEXTURE_2D, gl_texture);
      glUniform1i(glGetUniformLocation(crt_program, "frame"), 0);
      glUniform2f(glGetUniformLocation(crt_program, "texel"),
                  1.0f / frame_width, 1.0f / frame_height);
      glUniform1f(glGetUniformLocation(crt_program, "enabled"),
                  crt_filter ? 1.0f : 0.0f);
      glUniform4f(glGetUniformLocation(crt_program, "screenRect"),
                  x, output_height - y - draw_height,
                  draw_width, draw_height);
      glUniform4f(glGetUniformLocation(crt_program, "sourceRect"),
                  0.0f, source_y, 1.0f, source_height);
      glBegin(GL_TRIANGLES);
      glTexCoord2f(0, 0); glVertex2f(-1, -1);
      glTexCoord2f(2, 0); glVertex2f(3, -1);
      glTexCoord2f(0, 2); glVertex2f(-1, 3);
      glEnd();
    };
    if (nds_overlay_texture && nds_layout_index == 0) {
      const float scale = std::min(
          static_cast<float>(output_width) / 1920.0f,
          static_cast<float>(output_height) / 1080.0f);
      const float x = (output_width - 1920.0f * scale) * 0.5f;
      const float y = (output_height - 1080.0f * scale) * 0.5f;
      const float top_x = x + 605.0f * scale;
      const float top_y = y + 5.0f * scale;
      const float screen_width = 710.0f * scale;
      const float screen_height = 530.0f * scale;
      draw_frame(top_x, top_y, screen_width, screen_height, 0.0f, 0.5f);
      draw_frame(top_x, y + 545.0f * scale, screen_width, screen_height,
                 0.5f, 0.5f);
      glUseProgram(0);
      glEnable(GL_TEXTURE_2D);
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glBindTexture(GL_TEXTURE_2D, nds_overlay_texture);
      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, output_width, output_height, 0, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      glLoadIdentity();
      glColor4f(1, 1, 1, 1);
      glBegin(GL_QUADS);
      glTexCoord2f(0, 0); glVertex2f(x, y);
      glTexCoord2f(1, 0); glVertex2f(x + 1920.0f * scale, y);
      glTexCoord2f(1, 1); glVertex2f(x + 1920.0f * scale,
                                      y + 1080.0f * scale);
      glTexCoord2f(0, 1); glVertex2f(x, y + 1080.0f * scale);
      glEnd();
      glPopMatrix();
      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
      glMatrixMode(GL_MODELVIEW);
      glDisable(GL_BLEND);
    } else if (nds_top_overlay_texture && nds_layout_index != 0) {
      const float scale = std::min(
          static_cast<float>(output_width) / 1920.0f,
          static_cast<float>(output_height) / 1080.0f);
      const float x = (output_width - 1920.0f * scale) * 0.5f;
      const float y = (output_height - 1080.0f * scale) * 0.5f;
      const float screen_x = x + 327.0f * scale;
      const float screen_y = y + 68.0f * scale;
      const float screen_width = 1257.0f * scale;
      const float screen_height = screen_width * 3.0f / 4.0f;
      draw_frame(screen_x, screen_y, screen_width, screen_height,
                 nds_layout_index == 1 ? 0.0f : 0.5f, 0.5f);
      glUseProgram(0);
      glEnable(GL_TEXTURE_2D);
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glBindTexture(GL_TEXTURE_2D, nds_top_overlay_texture);
      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, output_width, output_height, 0, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      glLoadIdentity();
      glColor4f(1, 1, 1, 1);
      glBegin(GL_QUADS);
      glTexCoord2f(0, 0); glVertex2f(x, y);
      glTexCoord2f(1, 0); glVertex2f(x + 1920.0f * scale, y);
      glTexCoord2f(1, 1); glVertex2f(x + 1920.0f * scale,
                                      y + 1080.0f * scale);
      glTexCoord2f(0, 1); glVertex2f(x, y + 1080.0f * scale);
      glEnd();
      glPopMatrix();
      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
      glMatrixMode(GL_MODELVIEW);
      glDisable(GL_BLEND);
    } else if (nds_active && nds_layout_index != 0) {
      const float single_aspect =
          static_cast<float>(frame_width) /
          std::max(1.0f, static_cast<float>(frame_height) * 0.5f);
      float single_width = static_cast<float>(output_width);
      float single_height = single_width / single_aspect;
      if (single_height > output_height) {
        single_height = static_cast<float>(output_height);
        single_width = single_height * single_aspect;
      }
      const float single_x = (output_width - single_width) * 0.5f;
      const float single_y = (output_height - single_height) * 0.5f;
      draw_frame(single_x, single_y, single_width, single_height,
                 nds_layout_index == 1 ? 0.0f : 0.5f, 0.5f);
    } else {
      draw_frame(screen_x, screen_y, width, height, 0.0f, 1.0f);
    }
    if ((drilboor_effect_active() || title_edition == 1) &&
        jackhammer_texture) {
      const double now_seconds = animation_now;
      const double difficulty_elapsed =
          now_seconds - jackhammer_session_start;
      constexpr double ramp_start = 5.0;
      constexpr double ramp_duration = 6.7 * 60.0 - ramp_start;
      const size_t max_flights = difficulty_elapsed < ramp_start
          ? 0
          : std::min<size_t>(
                15, 1 + static_cast<size_t>(
                    (difficulty_elapsed - ramp_start) * 14.0 /
                    ramp_duration));
      std::uniform_real_distribution<double> spawn_delay(2.0, 6.0);
      while (jackhammer_spawn_times.size() < max_flights) {
        jackhammer_spawn_times.push_back(
            jackhammer_spawn_times.empty()
                ? jackhammer_session_start + ramp_start
                : now_seconds + spawn_delay(jackhammer_rng));
      }
      for (double &spawn_time : jackhammer_spawn_times) {
        if (now_seconds < spawn_time ||
            jackhammer_flights.size() >= max_flights)
          continue;
        const float sprite_w = 180.0f;
        const float sprite_h = 285.0f;
        std::uniform_real_distribution<float> edge_x(
            -sprite_w, static_cast<float>(output_width) + sprite_w);
        std::uniform_real_distribution<float> edge_y(
            -sprite_h, static_cast<float>(output_height) + sprite_h);
        std::uniform_int_distribution<int> edge(0, 3);
        JackhammerFlight flight;
        switch (edge(jackhammer_rng)) {
          case 0:
            flight.start_x = -sprite_w;
            flight.start_y = edge_y(jackhammer_rng);
            flight.end_x = output_width + sprite_w;
            flight.end_y = edge_y(jackhammer_rng);
            break;
          case 1:
            flight.start_x = edge_x(jackhammer_rng);
            flight.start_y = -sprite_h;
            flight.end_x = edge_x(jackhammer_rng);
            flight.end_y = output_height + sprite_h;
            break;
          case 2:
            flight.start_x = output_width + sprite_w;
            flight.start_y = edge_y(jackhammer_rng);
            flight.end_x = -sprite_w;
            flight.end_y = edge_y(jackhammer_rng);
            break;
          default:
            flight.start_x = edge_x(jackhammer_rng);
            flight.start_y = output_height + sprite_h;
            flight.end_x = edge_x(jackhammer_rng);
            flight.end_y = -sprite_h;
            break;
        }
        flight.start_time = now_seconds;
        if (!jackhammer_clip.empty()) {
          flight.voice_index = jackhammer_voice_positions.size();
          jackhammer_voice_positions.push_back(0);
        }
        jackhammer_flights.push_back(flight);
        spawn_time = now_seconds + spawn_delay(jackhammer_rng);
      }
      const float sprite_w = 180.0f;
      const float sprite_h = 285.0f;
      for (auto it = jackhammer_flights.begin(); it != jackhammer_flights.end();) {
        const JackhammerFlight &flight = *it;
        const float progress = static_cast<float>(
            std::min(1.0, (now_seconds - flight.start_time) / 1.8));
        const float x = flight.start_x +
            (flight.end_x - flight.start_x) * progress;
        const float y = flight.start_y +
            (flight.end_y - flight.start_y) * progress;
        const float motor_phase = static_cast<float>(
            (now_seconds - flight.start_time) * 6.283185307 * 14.0);
        const float motor_bob = std::sin(motor_phase) * 8.0f;
        const float motor_tilt = std::sin(motor_phase) * 4.0f;
        const float dx = flight.end_x - flight.start_x;
        const float dy = flight.end_y - flight.start_y;
        const float travel_angle = std::atan2(dy, dx) * 180.0f / 3.14159265f - 90.0f;
        glUseProgram(0);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, jackhammer_texture);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0, output_width, output_height, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glTranslatef(x + sprite_w * 0.5f + motor_bob,
                     y + sprite_h * 0.5f, 0.0f);
        glRotatef(travel_angle + motor_tilt, 0.0f, 0.0f, 1.0f);
        glTranslatef(-sprite_w * 0.5f, -sprite_h * 0.5f, 0.0f);
        glColor4f(1, 1, 1, 1);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex2f(0, 0);
        glTexCoord2f(1, 0); glVertex2f(sprite_w, 0);
        glTexCoord2f(1, 1); glVertex2f(sprite_w, sprite_h);
        glTexCoord2f(0, 1); glVertex2f(0, sprite_h);
        glEnd();
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glDisable(GL_BLEND);
        if (progress >= 1.0f) {
          if (flight.voice_index != SIZE_MAX) {
            jackhammer_voice_positions.erase(
                jackhammer_voice_positions.begin() + flight.voice_index);
            for (JackhammerFlight &other : jackhammer_flights) {
              if (other.voice_index > flight.voice_index)
                --other.voice_index;
            }
          }
          it = jackhammer_flights.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto it = skywalker_flights.begin(); it != skywalker_flights.end();) {
      SkywalkerFlight &flight = *it;
      const double elapsed = animation_now - flight.start_time;
      const double explosion_start =
          flight.slide_duration + flight.pause_duration;
      const double splash_end = explosion_start + flight.splash_duration +
                                flight.splash_hold_duration +
                                flight.splash_fade_duration;
      if (elapsed >= splash_end) {
        it = skywalker_flights.erase(it);
        continue;
      }
      const float slide = static_cast<float>(
          std::clamp(elapsed / flight.slide_duration, 0.0, 1.0));
      const float center_x = output_width * 0.5f;
      const float center_y = output_height * 0.5f;
      const float image_w = flight.size * 473.0f / 1524.0f;
      const float x = flight.start_x +
          (center_x - image_w * 0.5f - flight.start_x) * slide;
      const float y = flight.start_y +
          (center_y - flight.size * 0.5f - flight.start_y) * slide;
      const double splash_elapsed = elapsed - explosion_start;
      const float expansion = static_cast<float>(std::clamp(
          splash_elapsed / flight.splash_duration, 0.0, 1.0));
      const double fade_begin = flight.splash_duration +
                                flight.splash_hold_duration;
      const float fade = splash_elapsed <= fade_begin
          ? 1.0f
          : static_cast<float>(std::clamp(
                1.0 - (splash_elapsed - fade_begin) /
                    flight.splash_fade_duration, 0.0, 1.0));
      const float explosion = expansion;
      if (explosion > 0.0f && !flight.splash_started) {
        flight.splash_started = true;
        skywalker_voices.push_back({0, flight.audio_speed, flight.audio_gain});
      }
      glUseProgram(0);
      glDisable(GL_DEPTH_TEST);
      glEnable(GL_TEXTURE_2D);
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glBindTexture(GL_TEXTURE_2D, skywalker_texture);
      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, output_width, output_height, 0, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      glLoadIdentity();
      glColor4f(1, 1, 1, 1.0f - explosion);
      glBegin(GL_QUADS);
      glTexCoord2f(0, 0); glVertex2f(x, y);
      glTexCoord2f(1, 0); glVertex2f(x + image_w, y);
      glTexCoord2f(1, 1); glVertex2f(x + image_w, y + flight.size);
      glTexCoord2f(0, 1); glVertex2f(x, y + flight.size);
      glEnd();
      glDisable(GL_TEXTURE_2D);
      if (explosion > 0.0f) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, water_splash_texture);
        glColor4f(1, 1, 1, explosion * fade);
        for (size_t i = 0; i < flight.splash.size(); i += 6) {
          const float direction = flight.splash[i];
          const float impact_elapsed = static_cast<float>(
              std::max(0.0, splash_elapsed - flight.splash[i + 4]));
          const float splash_progress = std::clamp(
              impact_elapsed / flight.splash[i + 5], 0.0f, 1.0f);
          const float distance = flight.splash[i + 1] *
                                 std::max(output_width, output_height) *
                                 1.15f * splash_progress;
          const float splash_x = center_x + std::cos(direction) * distance;
          const float splash_y = center_y + std::sin(direction) * distance;
          const float splash_height = flight.splash[i + 2] *
                                      (0.08f + 0.92f * splash_progress);
          const float splash_width = splash_height * 453.0f / 350.0f;
          glPushMatrix();
          glTranslatef(splash_x, splash_y, 0.0f);
          glRotatef(flight.splash[i + 3], 0.0f, 0.0f, 1.0f);
          glTranslatef(-splash_width * 0.5f, -splash_height * 0.5f,
                       0.0f);
          glBegin(GL_QUADS);
          glTexCoord2f(0, 0); glVertex2f(0, 0);
          glTexCoord2f(1, 0); glVertex2f(splash_width, 0);
          glTexCoord2f(1, 1); glVertex2f(splash_width, splash_height);
          glTexCoord2f(0, 1); glVertex2f(0, splash_height);
          glEnd();
          glPopMatrix();
        }
      }
      glPopMatrix();
      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
      glMatrixMode(GL_MODELVIEW);
      glDisable(GL_BLEND);
      ++it;
    }
    if (gba_overlay_texture) {
      glUseProgram(0);
      glEnable(GL_TEXTURE_2D);
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glBindTexture(GL_TEXTURE_2D, gba_overlay_texture);
      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, output_width, output_height, 0, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      glLoadIdentity();
      glColor4f(1, 1, 1, 1);
      glBegin(GL_QUADS);
      glTexCoord2f(0, 0); glVertex2f(overlay_x, overlay_y);
      glTexCoord2f(1, 0); glVertex2f(overlay_x + overlay_width, overlay_y);
      glTexCoord2f(1, 1); glVertex2f(overlay_x + overlay_width,
                                       overlay_y + overlay_height);
      glTexCoord2f(0, 1); glVertex2f(overlay_x, overlay_y + overlay_height);
      glEnd();
      glPopMatrix();
      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
      glMatrixMode(GL_MODELVIEW);
      glDisable(GL_BLEND);
    }
    if (show_fps) {
      char fps_text[32]{};
      std::snprintf(fps_text, sizeof(fps_text), "FPS %u",
                    static_cast<unsigned>(std::lround(displayed_fps)));
      glUseProgram(0);
      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, output_width, output_height, 0, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      glLoadIdentity();
      glDisable(GL_TEXTURE_2D);
      draw_gl_text(fps_text, 16, 16, 3, 1.0f, 0.85f, 0.92f);
      glEnable(GL_TEXTURE_2D);
      glPopMatrix();
      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
      glMatrixMode(GL_MODELVIEW);
    }
    if (!input_swap_notification.empty() &&
        SDL_GetTicks() < input_swap_notification_until) {
      glUseProgram(0);
      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, output_width, output_height, 0, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      glLoadIdentity();
      glDisable(GL_TEXTURE_2D);
      const int text_width =
          static_cast<int>(input_swap_notification.size()) * 18 - 3;
      draw_gl_text(input_swap_notification,
                   std::max(12, (output_width - text_width) / 2), 28, 3,
                   1.0f, 0.85f, 0.92f);
      glEnable(GL_TEXTURE_2D);
      glPopMatrix();
      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
      glMatrixMode(GL_MODELVIEW);
    } else {
      input_swap_notification.clear();
    }
    if (escape_pending) {
      glUseProgram(0);
      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, output_width, output_height, 0, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      glLoadIdentity();
      glDisable(GL_TEXTURE_2D);
      draw_gl_text("PRESS ESC AGAIN FOR ROM MENU", 24,
                   output_height - 36, 3, 1.0f, 0.85f, 0.92f);
      glEnable(GL_TEXTURE_2D);
      glPopMatrix();
      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
      glMatrixMode(GL_MODELVIEW);
    }
    if (cooked_gameplay) {
      glUseProgram(0);
      glMatrixMode(GL_PROJECTION);
      glPushMatrix();
      glLoadIdentity();
      glOrtho(0, output_width, output_height, 0, -1, 1);
      glMatrixMode(GL_MODELVIEW);
      glPushMatrix();
      glLoadIdentity();
      for (const auto &popup : cooked_popups) {
        draw_cooked_popup(popup, output_width, output_height);
      }
      if (SDL_GetTicks() < cooked_wrong_popup_until)
        draw_cooked_popup(cooked_wrong_popup, output_width, output_height,
                          cooked_wrong_popup.warning);
      glPopMatrix();
      glMatrixMode(GL_PROJECTION);
      glPopMatrix();
      glMatrixMode(GL_MODELVIEW);
    }
    ++fps_frames;
    const uint64_t fps_now = SDL_GetPerformanceCounter();
    const double elapsed = static_cast<double>(fps_now - fps_window_start) /
                           SDL_GetPerformanceFrequency();
    if (elapsed >= 0.5) {
      displayed_fps = fps_frames / elapsed;
      fps_frames = 0;
      fps_window_start = fps_now;
    }
    SDL_GL_SwapWindow(window);
      if (!(turbo_toggle || turbo_hold)) {
      next_frame += ticks_per_frame;
      const uint64_t now = SDL_GetPerformanceCounter();
      if (now < next_frame) {
        const uint64_t remaining = next_frame - now;
        const uint32_t milliseconds = static_cast<uint32_t>(
            remaining * 1000 / SDL_GetPerformanceFrequency());
        if (milliseconds) SDL_Delay(milliseconds);
      } else if (now > next_frame + ticks_per_frame * 4) {
        next_frame = now;
      }
    } else {
      next_frame = SDL_GetPerformanceCounter();
      }
    }
  if (audio_device) SDL_CloseAudioDevice(audio_device);
    if (save_data && save_size) {
      std::error_code save_directory_error;
      std::filesystem::create_directories(save_path.parent_path(),
                                          save_directory_error);
      if (save_directory_error)
        std::cerr << "Could not create save directory: "
                  << save_directory_error.message() << '\n';
      std::ofstream save_output(save_path, std::ios::binary | std::ios::trunc);
      if (!save_output) {
        std::cerr << "Could not write save: " << save_path << '\n';
      } else {
        save_output.write(static_cast<const char *>(save_data),
                          static_cast<std::streamsize>(save_size));
        std::cerr << "Saved game: " << save_path << '\n';
      }
    }
    core.unload_game();
    core.deinit();
    if (gl_context) {
      glDeleteTextures(1, &gl_texture);
      if (gba_overlay_texture) glDeleteTextures(1, &gba_overlay_texture);
      if (nds_overlay_texture) glDeleteTextures(1, &nds_overlay_texture);
      if (nds_top_overlay_texture)
        glDeleteTextures(1, &nds_top_overlay_texture);
      if (jackhammer_texture) glDeleteTextures(1, &jackhammer_texture);
      if (skywalker_texture) glDeleteTextures(1, &skywalker_texture);
      if (water_splash_texture) glDeleteTextures(1, &water_splash_texture);
    glDeleteProgram(crt_program);
    SDL_GL_DeleteContext(gl_context);
  } else {
    vulkan_host.shutdown();
  }
  SDL_DestroyWindow(window);
  close_dynamic_library(core.handle);
  if (nds_active) {
    nds_layout_index = 0;
    std::filesystem::path nds_save = save_path;
    nds_save.replace_extension(".dsv");
    std::filesystem::path sav_copy = save_path;
    if (std::filesystem::exists(nds_save)) {
      std::error_code copy_error;
      std::filesystem::copy_file(
          nds_save, sav_copy,
          std::filesystem::copy_options::overwrite_existing, copy_error);
      if (copy_error)
        std::cerr << "Could not export NDS .sav copy: "
                  << copy_error.message() << '\n';
    }
  }
  if (!return_to_rom_selector) {
    SDL_Quit();
    return 0;
  }

  SDL_Window *frontend_window = SDL_CreateWindow(
      "Flowmulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      FRONTEND_WIDTH, FRONTEND_HEIGHT, SDL_WINDOW_SHOWN);
  theme_window = frontend_window;
  if (frontend_window) {
    SDL_SetWindowTitle(frontend_window, theme_window_title());
    SDL_SetWindowOpacity(frontend_window, 1.0f);
  }
  SDL_Renderer *frontend_renderer = frontend_window
      ? SDL_CreateRenderer(frontend_window, -1,
                           SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
      : nullptr;
  if (!frontend_renderer && frontend_window)
    frontend_renderer = SDL_CreateRenderer(frontend_window, -1,
                                           SDL_RENDERER_SOFTWARE);
  if (frontend_renderer)
    SDL_RenderSetLogicalSize(frontend_renderer, FRONTEND_WIDTH,
                             FRONTEND_HEIGHT);
  if (!frontend_window || !frontend_renderer) {
    if (frontend_renderer) SDL_DestroyRenderer(frontend_renderer);
    if (frontend_window) SDL_DestroyWindow(frontend_window);
    SDL_Quit();
    return 1;
  }
  const bool frontend_running = nds_active
      ? nds_frontend_menu(frontend_window, frontend_renderer, rom_path,
                          three_ds_active)
      : gba_frontend_menu(frontend_window, frontend_renderer, rom_path);
  if (!frontend_running && frontend_escape_requested && !quit_requested) {
    frontend_escape_requested = false;
    rom_path.clear();
    const bool main_menu_running =
        menu(frontend_window, frontend_renderer, rom_path);
    if (main_menu_running && !rom_path.empty()) {
      const std::filesystem::path flowmulator = executable_directory() /
                                                "Flowmulator";
      SDL_Quit();
      execl(flowmulator.c_str(), flowmulator.filename().c_str(),
            rom_path.c_str(), static_cast<char *>(nullptr));
      std::perror("Could not launch selected game");
      quit_requested = true;
    } else {
      quit_requested = true;
    }
  }
  SDL_DestroyRenderer(frontend_renderer);
  SDL_DestroyWindow(frontend_window);
  if (!frontend_running && quit_requested) {
    SDL_Quit();
    return 0;
  }
  return_to_rom_selector = false;
  }
}
