#define GL_GLEXT_PROTOTYPES
#include "libretro.h"
#include "embedded_assets.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <GL/gl.h>
#include <dlfcn.h>

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
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

namespace {
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
  void *handle = nullptr;
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
    function = reinterpret_cast<T>(dlsym(handle, name));
    return function != nullptr;
  }
};

std::vector<uint32_t> framebuffer(240 * 160);
std::vector<uint32_t> crt_framebuffer(240 * 160);
unsigned frame_width = 240, frame_height = 160;
unsigned buttons = 0;
unsigned pixel_format = RETRO_PIXEL_FORMAT_XRGB8888;
SDL_AudioDeviceID audio_device = 0;
bool crt_filter = false;
int title_edition = 0;
unsigned audio_rate = 32768;
std::vector<int16_t> jackhammer_clip;
std::vector<size_t> jackhammer_voice_positions;
std::vector<int16_t> big_forehead_clip;
size_t big_forehead_position = 0;
double forehead_mid_average = 0.0;
double forehead_side_average = 0.0;
GLuint jackhammer_texture = 0;
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
std::filesystem::path executable_directory();

bool install_dependencies_once() {
  const char *home = std::getenv("HOME");
  if (!home) return false;
  const std::filesystem::path marker =
      std::filesystem::path(home) / ".config" / "flowmulator" /
      "dependencies-installed";
  if (std::filesystem::exists(marker)) return true;
  void *installed_core = dlopen("/usr/lib/libretro/mgba_libretro.so",
                                 RTLD_NOW | RTLD_LOCAL);
  if (installed_core) {
    dlclose(installed_core);
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
      _binary_install_dependencies_sh_end -
      _binary_install_dependencies_sh_start);
  size_t written = 0;
  while (written < script_size) {
    const ssize_t count = write(fd, _binary_install_dependencies_sh_start + written,
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
  const std::string command = "bash " + std::string(temp_name);
  const int result = std::system(command.c_str());
  unlink(temp_name);
  if (result != 0) return false;
  std::error_code error;
  std::filesystem::create_directories(marker.parent_path(), error);
  if (error) return false;
  std::ofstream marker_file(marker);
  return static_cast<bool>(marker_file);
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
constexpr unsigned turbo_hold_index = 10;
constexpr unsigned turbo_toggle_index = 11;

std::filesystem::path controls_path() {
  const char *home = std::getenv("HOME");
  if (!home) return {};
  return std::filesystem::path(home) / ".config" / "flowmulator" / "controls.cfg";
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
}

bool environment(unsigned command, void *data) {
  switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      pixel_format = *static_cast<unsigned *>(data);
      return pixel_format == RETRO_PIXEL_FORMAT_XRGB8888 ||
             pixel_format == RETRO_PIXEL_FORMAT_RGB565 ||
             pixel_format == RETRO_PIXEL_FORMAT_0RGB1555;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: {
      const auto *geometry = static_cast<const retro_game_geometry *>(data);
      frame_width = geometry->base_width;
      frame_height = geometry->base_height;
      framebuffer.resize(static_cast<size_t>(frame_width) * frame_height);
      return true;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *static_cast<const char **>(data) = "/usr/share/libretro";
      return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
      return false;
    default:
      return false;
  }
}

void video_refresh(const void *data, unsigned width, unsigned height, size_t pitch) {
  if (!data || !width || !height) return;
  frame_width = width;
  frame_height = height;
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
        uniform float enabled;
        varying vec2 uv;
        void main() {
          vec2 sampleUv = vec2(
              (gl_FragCoord.x - screenRect.x) / screenRect.z,
              1.0 - (gl_FragCoord.y - screenRect.y) / screenRect.w);
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

size_t audio_batch(const int16_t *data, size_t frames) {
  if (audio_device && data) {
    const Uint32 queued = SDL_GetQueuedAudioSize(audio_device);
    const Uint32 max_queued = audio_rate * 2 * sizeof(int16_t) / 5;
    if (queued > max_queued) SDL_ClearQueuedAudio(audio_device);
    std::vector<int16_t> mixed(data, data + frames * 2);
    for (size_t i = 0; i < frames; ++i) {
      int left = mixed[i * 2];
      int right = mixed[i * 2 + 1];
      if (title_edition == 2) {
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
      if (title_edition == 1 && !jackhammer_clip.empty() &&
          !jackhammer_voice_positions.empty()) {
        for (size_t &position : jackhammer_voice_positions) {
          if (position + 1 < jackhammer_clip.size()) {
            left += jackhammer_clip[position];
            right += jackhammer_clip[position + 1];
            position += 2;
          }
        }
      }
      if (title_edition == 2 && big_forehead_clip.size() >= 2) {
        left += static_cast<int>(big_forehead_clip[big_forehead_position] * 0.75);
        right += static_cast<int>(
            big_forehead_clip[big_forehead_position + 1] * 0.75);
        big_forehead_position =
            (big_forehead_position + 2) % big_forehead_clip.size();
      }
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
void audio_sample(int16_t left, int16_t right) {
  const int16_t sample[2] = {left, right};
  if (audio_device) SDL_QueueAudio(audio_device, sample, sizeof(sample));
}
void input_poll() {}
int16_t input_state(unsigned, unsigned, unsigned, unsigned id) {
  return (buttons & (1u << id)) ? 1 : 0;
}

void set_button(SDL_Keycode key, bool pressed) {
  unsigned id = 32;
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
  if (id < 32) {
    if (pressed) buttons |= 1u << id;
    else buttons &= ~(1u << id);
  }
}

std::filesystem::path executable_directory() {
  char buffer[4096]{};
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (length <= 0) return std::filesystem::current_path();
  buffer[length] = '\0';
  return std::filesystem::path(buffer).parent_path();
}

void draw_round_rect(SDL_Renderer *renderer, SDL_Rect rect, int radius);
void draw_text(SDL_Renderer *renderer, const std::string &text, int x, int y,
               int scale, SDL_Color color);
void draw_text_centered(SDL_Renderer *renderer, const std::string &text,
                        int center_x, int y, int scale, SDL_Color color);

std::string choose_rom(SDL_Renderer *renderer) {
  const std::filesystem::path rom_directory = executable_directory() / "ROMS";
  std::vector<std::filesystem::path> roms;
  std::error_code error;
  if (std::filesystem::exists(rom_directory, error)) {
    for (const auto &entry : std::filesystem::directory_iterator(rom_directory, error)) {
      if (entry.is_regular_file() && entry.path().extension() == ".gba")
        roms.push_back(entry.path());
    }
  }
  std::sort(roms.begin(), roms.end());
  if (!roms.empty()) {
    size_t selected = 0;
    for (;;) {
      SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
      SDL_RenderClear(renderer);
      draw_text_centered(renderer, "SELECT ROM", 400, 46, 4, {245, 183, 202, 255});
      draw_text_centered(renderer, "ROMS FOLDER", 400, 82, 2, {190, 116, 148, 255});
      const size_t first = selected > 6 ? selected - 6 : 0;
      const size_t last = std::min(roms.size(), first + 8);
      for (size_t i = first; i < last; ++i) {
        const int row_top = 126 + static_cast<int>(i - first) * 48;
        const int text_y = row_top + 12;
        SDL_Rect row{95, row_top, 610, 38};
        SDL_SetRenderDrawColor(renderer, i == selected ? 112 : 67,
                               i == selected ? 42 : 30,
                               i == selected ? 72 : 48, 255);
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
        draw_text(renderer, label, 115, text_y, 2, {245, 205, 217, 255});
      }
      draw_text(renderer, "UP DOWN SELECT   ESC BACK", 250, 555, 2,
                {190, 116, 148, 255});
      SDL_RenderPresent(renderer);

      SDL_Event event;
      while (SDL_WaitEvent(&event)) {
        if (event.type == SDL_QUIT) return {};
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
        else
          continue;
        break;
      }
    }
  }
  const std::string directory = (rom_directory.string() + "/");
  const std::string command =
      "zenity --file-selection --filename='" + directory +
      "' --title='Choose a Game Boy Advance ROM' "
      "--file-filter='GBA ROMs | *.gba' --file-filter='All files | *' 2>/dev/null";
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

void draw_blossom(SDL_Renderer *renderer, int x, int y, int size) {
  SDL_SetRenderDrawColor(renderer, 248, 174, 202, 255);
  for (int i = 0; i < 5; ++i) {
    const int px = x + static_cast<int>(std::cos(i * 1.2566) * size * .55);
    const int py = y + static_cast<int>(std::sin(i * 1.2566) * size * .55);
    SDL_Rect petal{px - size / 2, py - size / 2, size, size};
    SDL_RenderFillRect(renderer, &petal);
  }
  SDL_SetRenderDrawColor(renderer, 255, 224, 139, 255);
  SDL_Rect center{x - size / 3, y - size / 3, size * 2 / 3, size * 2 / 3};
  SDL_RenderFillRect(renderer, &center);
}

void draw_frontend_background(SDL_Renderer *renderer, SDL_Texture *background) {
  if (background) {
    SDL_SetTextureBlendMode(background, SDL_BLENDMODE_NONE);
    const SDL_Rect destination{0, 0, 800, 600};
    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, background, nullptr, &destination);
    return;
  }
  SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
  SDL_RenderClear(renderer);

  SDL_SetRenderDrawColor(renderer, 57, 27, 55, 255);
  SDL_Rect horizon{0, 300, 800, 300};
  SDL_RenderFillRect(renderer, &horizon);

  SDL_SetRenderDrawColor(renderer, 83, 38, 69, 255);
  for (int y = 18; y < 250; y += 26) {
    for (int x = (y / 26 % 2) * 18 - 30; x < 830; x += 72) {
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
    SDL_SetRenderDrawColor(renderer, 244, 133, 180,
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
  return "KEY";
}

void controls_menu(SDL_Renderer *renderer) {
  unsigned selected = 0;
  bool waiting = false;
  constexpr unsigned option_count = 12;
  for (;;) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) return;
      if (event.type != SDL_KEYDOWN) continue;
      if (waiting) {
        if (event.key.keysym.sym == SDLK_ESCAPE) waiting = false;
        else {
          if (selected == turbo_hold_index) turbo_key = event.key.keysym.sym;
          else if (selected == turbo_toggle_index) {
            waiting = false;
            continue;
          }
          else control_keys[selected] = event.key.keysym.sym;
          save_controls();
          waiting = false;
        }
      } else if (event.key.keysym.sym == SDLK_UP ||
                 event.key.keysym.sym == SDLK_w ||
                 event.key.keysym.sym == SDLK_a) {
        selected = (selected + option_count - 1) % option_count;
      } else if (event.key.keysym.sym == SDLK_DOWN ||
                 event.key.keysym.sym == SDLK_s ||
                 event.key.keysym.sym == SDLK_d) {
        selected = (selected + 1) % option_count;
      } else if (event.key.keysym.sym == SDLK_RETURN ||
                 event.key.keysym.sym == SDLK_SPACE) {
        waiting = true;
      } else if (event.key.keysym.sym == SDLK_ESCAPE) return;
    }
    SDL_SetRenderDrawColor(renderer, 35, 18, 40, 255);
    SDL_RenderClear(renderer);
    draw_text(renderer, "CONTROLS", 295, 55, 5, {245, 183, 202, 255});
    for (unsigned i = 0; i < control_keys.size(); ++i) {
      const int y = 125 + static_cast<int>(i) * 34;
      if (i == selected) {
        SDL_SetRenderDrawColor(renderer, 112, 42, 72, 255);
        SDL_Rect highlight{170, y - 7, 460, 32};
        SDL_RenderFillRect(renderer, &highlight);
      }
      draw_text(renderer, control_names[i], 190, y, 2,
                i == selected ? SDL_Color{255, 225, 235, 255}
                              : SDL_Color{220, 166, 187, 255});
      draw_text(renderer, key_name(control_keys[i]), 505, y, 2,
                i == selected ? SDL_Color{255, 225, 235, 255}
                              : SDL_Color{190, 116, 148, 255});
    }
    const int turbo_y = 125 + static_cast<int>(control_keys.size()) * 34;
    if (selected == turbo_hold_index) {
      SDL_SetRenderDrawColor(renderer, 112, 42, 72, 255);
      SDL_Rect highlight{170, turbo_y - 7, 460, 32};
      SDL_RenderFillRect(renderer, &highlight);
    }
    draw_text(renderer, "TURBO SPEED HOLD", 190, turbo_y, 2,
              selected == 10 ? SDL_Color{255, 225, 235, 255}
                             : SDL_Color{220, 166, 187, 255});
    draw_text(renderer, key_name(turbo_key), 505, turbo_y, 2,
              selected == turbo_hold_index ? SDL_Color{255, 225, 235, 255}
                                            : SDL_Color{190, 116, 148, 255});
    const int turbo_toggle_y = turbo_y + 34;
    if (selected == turbo_toggle_index) {
      SDL_SetRenderDrawColor(renderer, 112, 42, 72, 255);
      SDL_Rect highlight{170, turbo_toggle_y - 7, 460, 32};
      SDL_RenderFillRect(renderer, &highlight);
    }
    draw_text(renderer, "TURBO SPEED TOGGLE", 190, turbo_toggle_y, 2,
              selected == turbo_toggle_index ? SDL_Color{255, 225, 235, 255}
                                              : SDL_Color{220, 166, 187, 255});
    draw_text(renderer, "SHIFT +", 505, turbo_toggle_y, 2,
              selected == turbo_toggle_index ? SDL_Color{255, 225, 235, 255}
                                              : SDL_Color{190, 116, 148, 255});
    draw_text(renderer, key_name(turbo_key), 601, turbo_toggle_y, 2,
              selected == turbo_toggle_index ? SDL_Color{255, 225, 235, 255}
                                              : SDL_Color{190, 116, 148, 255});
    if (waiting)
      draw_text(renderer, "PRESS A KEY", 300, 530, 2, {245, 145, 175, 255});
    draw_text_centered(renderer, "F1 CRT TOGGLE", 400, 532, 2,
                       {190, 116, 148, 255});
    draw_text_centered(renderer, "F2 FPS OVERLAY   F3 VSYNC", 400, 556, 2,
                       {190, 116, 148, 255});
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
}

bool menu(SDL_Window *window, SDL_Renderer *renderer, std::string &rom_path) {
  int selected = 0;
  bool settings = false;
  bool running = true;
  SDL_RWops *background_rw = SDL_RWFromConstMem(
      _binary__assets_frontend_background_bmp_start,
      static_cast<int>(embedded_size(_binary__assets_frontend_background_bmp_start,
                                     _binary__assets_frontend_background_bmp_end)));
  SDL_Surface *background_surface = background_rw
      ? SDL_LoadBMP_RW(background_rw, SDL_TRUE) : nullptr;
  SDL_Texture *background = background_surface
      ? SDL_CreateTextureFromSurface(renderer, background_surface)
      : nullptr;
  if (background_surface) SDL_FreeSurface(background_surface);
  const auto finish = [&](bool result) {
    if (background) SDL_DestroyTexture(background);
    return result;
  };
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) return finish(false);
      if (event.type == SDL_MOUSEBUTTONDOWN &&
          event.button.button == SDL_BUTTON_LEFT &&
          event.button.x >= 250 && event.button.x < 550 &&
          event.button.y >= 70 && event.button.y < 132) {
        title_edition = (title_edition + 1) % 3;
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
          if (!rom_path.empty()) return finish(true);
        } else if (selected == 1) {
          crt_filter = !crt_filter;
          save_controls();
        } else if (selected == 2) {
          controls_menu(renderer);
        }
      }
      if (event.key.keysym.sym == SDLK_ESCAPE) return finish(false);
    }
    draw_frontend_background(renderer, background);
    SDL_SetRenderDrawColor(renderer, 117, 48, 87, 255);
    SDL_Rect title{250, 70, 300, 62}; draw_glow_rect(renderer, title, 12);
    draw_round_rect(renderer, title, 12);
    SDL_SetRenderDrawColor(renderer, 255, 232, 241, 255);
    SDL_Rect title_inner{260, 80, 280, 42}; draw_round_rect(renderer, title_inner, 8);
    SDL_SetRenderDrawColor(renderer, selected == 0 ? 117 : 160, 48, selected == 0 ? 87 : 120, 255);
    SDL_Rect load{280, 220, 240, 60}; draw_glow_rect(renderer, load, 10); draw_round_rect(renderer, load, 10);
    SDL_SetRenderDrawColor(renderer, selected == 1 ? 117 : 160, 48, selected == 1 ? 87 : 120, 255);
    SDL_Rect filter{280, 310, 240, 60}; draw_glow_rect(renderer, filter, 10); draw_round_rect(renderer, filter, 10);
    SDL_SetRenderDrawColor(renderer, selected == 2 ? 117 : 160, 48, selected == 2 ? 87 : 120, 255);
    SDL_Rect controls{280, 400, 240, 60}; draw_glow_rect(renderer, controls, 10); draw_round_rect(renderer, controls, 10);
    const SDL_Rect selected_box = selected == 0 ? load : selected == 1 ? filter :
                                  controls;
    draw_blossom(renderer, selected_box.x - 22, selected_box.y + selected_box.h / 2, 10);
    draw_text_centered(renderer,
                       title_edition == 1 ? "DRILBOOR"
                       : title_edition == 2 ? "BIG FOREHEAD" : "FLOWMULATOR",
                       400, 87, 4, {117, 48, 87, 255});
    if (title_edition != 0)
      draw_text_centered(renderer, "EDITION", 400, 140, 2,
                         {117, 48, 87, 255});
    draw_text_centered(renderer, "LOAD ROM", 400, 238, 3, {255, 232, 241, 255});
    draw_text_centered(renderer, crt_filter ? "CRT ON" : "CRT OFF",
                       400, 330, 3, {255, 232, 241, 255});
    draw_text_centered(renderer, "CONTROLS", 400, 420, 3, {255, 232, 241, 255});
    if (settings) {
      draw_text(renderer, "Z A   X B   ENTER START", 265, 520, 2, {117, 48, 87, 255});
      draw_text(renderer, "ARROWS D PAD   A L   S R", 265, 545, 2, {117, 48, 87, 255});
    }
    SDL_SetRenderDrawColor(renderer, 117, 48, 87, 255);
    SDL_RenderDrawLine(renderer, 300, 500, 500, 500);
    draw_text_centered(renderer, "MADE BY FLOWKIDD", 400, 565, 2,
                       {117, 48, 87, 255});
    draw_text(renderer, "BUILD V0.3", 40, 565, 2,
              {180, 110, 145, 255});
    if (settings) {
      SDL_SetRenderDrawColor(renderer, 117, 48, 87, 255);
      SDL_Rect marker{535, 415, 12, 30}; SDL_RenderFillRect(renderer, &marker);
    }
    (void)window;
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
  }
  return finish(false);
}
}  // namespace

int main(int argc, char **argv) {
  if (!install_dependencies_once()) {
    std::cerr << "Flowmulator could not install its one-time dependencies\n";
    return 1;
  }
  load_controls();
  std::string rom_path = argc == 2 ? argv[1] : "";
  if (argc > 2) {
    std::cerr << "Usage: Flowmulator ROM.gba\n";
    return 2;
  }

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
    std::cerr << "SDL initialization failed: " << SDL_GetError() << '\n';
    return 1;
  }
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
  SDL_Window *window = SDL_CreateWindow("Flowmulator", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_SHOWN);
  if (window) SDL_SetWindowOpacity(window, 1.0f);
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
      SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  if (!window || !renderer || !menu(window, renderer, rom_path)) {
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
  }
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);

  bool return_to_rom_selector = false;
  for (;;) {
  Core core;
  core.handle = dlopen("/usr/lib/libretro/mgba_libretro.so", RTLD_NOW | RTLD_LOCAL);
  if (!core.handle) {
    std::cerr << "Cannot load mGBA core: " << dlerror() << '\n';
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
    std::cerr << "mGBA core is missing a required libretro entry point\n";
    dlclose(core.handle);
    return 1;
  }

  core.set_environment(environment);
  core.set_video_refresh(video_refresh);
  core.set_audio_sample(audio_sample);
  core.set_audio_sample_batch(audio_batch);
  core.set_input_poll(input_poll);
  core.set_input_state(input_state);
  core.init();

  retro_system_info info{};
  core.get_system_info(&info);
  std::cerr << "Loaded " << (info.library_name ? info.library_name : "GBA core")
            << " " << (info.library_version ? info.library_version : "") << '\n';

  retro_game_info game{rom_path.c_str(), nullptr, 0, nullptr};
  std::ifstream input(rom_path, std::ios::binary | std::ios::ate);
  if (!input) {
    std::cerr << "Cannot open ROM: " << rom_path << '\n';
    core.deinit();
    dlclose(core.handle);
    return 1;
  }
  const auto size = input.tellg();
  std::vector<uint8_t> rom(static_cast<size_t>(size));
  input.seekg(0);
  input.read(reinterpret_cast<char *>(rom.data()), size);
  game.data = rom.data();
  game.size = rom.size();
  game.path = rom_path.c_str();
  if (!core.load_game(&game)) {
    std::cerr << "mGBA refused to load the ROM\n";
    core.deinit();
    dlclose(core.handle);
    return 1;
  }
  const unsigned retro_memory_save_ram = 0;
  const size_t save_size = core.get_memory_size(retro_memory_save_ram);
  void *save_data = core.get_memory_data(retro_memory_save_ram);
  const std::filesystem::path rom_file(rom_path);
  const std::filesystem::path project_root = rom_file.parent_path().parent_path();
  std::filesystem::path save_filename = rom_file.filename();
  save_filename.replace_extension(".sav");
  const std::filesystem::path save_path =
      project_root / "SAVES" / save_filename;
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

  window = SDL_CreateWindow("Flowmulator", SDL_WINDOWPOS_CENTERED,
      SDL_WINDOWPOS_CENTERED, 960, 640,
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
  SDL_SetWindowFullscreen(window, 0);
  SDL_GLContext gl_context = window ? SDL_GL_CreateContext(window) : nullptr;
  if (window) {
    SDL_SetWindowBordered(window, SDL_TRUE);
    SDL_SetWindowResizable(window, SDL_TRUE);
    SDL_SetWindowOpacity(window, 1.0f);
    SDL_SetWindowSize(window, 960, 640);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED,
                          SDL_WINDOWPOS_CENTERED);
  }
  if (gl_context) SDL_GL_SetSwapInterval(vsync_enabled ? 1 : 0);
  const GLuint crt_program = gl_context ? create_crt_program() : 0;
  GLuint gl_texture = 0;
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
    if (title_edition == 1) load_jackhammer_texture();
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
    load_audio_clip(_binary__assets_big_forehead_wav_start,
                    embedded_size(_binary__assets_big_forehead_wav_start,
                                  _binary__assets_big_forehead_wav_end),
                    obtained_audio_spec, big_forehead_clip);
    SDL_PauseAudioDevice(audio_device, 0);
  }
  bool running = window && gl_context && crt_program && gl_texture;
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
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT)
        running = false;
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_ESCAPE) {
        if (escape_pending) {
          running = false;
          return_to_rom_selector = true;
        } else {
          escape_pending = true;
        }
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F1) {
        crt_filter = !crt_filter;
        save_controls();
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F11) {
        const Uint32 flags = SDL_GetWindowFlags(window);
        SDL_SetWindowFullscreen(window,
            (flags & SDL_WINDOW_FULLSCREEN) ? 0 : SDL_WINDOW_FULLSCREEN);
        SDL_GL_SetSwapInterval(vsync_enabled ? 1 : 0);
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F2)
        show_fps = !show_fps;
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == SDLK_F3) {
        vsync_enabled = !vsync_enabled;
        SDL_GL_SetSwapInterval(vsync_enabled ? 1 : 0);
      }
      if (event.type == SDL_KEYDOWN && !event.key.repeat &&
          event.key.keysym.sym == turbo_key) {
        if (event.key.keysym.mod & KMOD_SHIFT) turbo_toggle = !turbo_toggle;
        else turbo_hold = true;
      }
      if (event.type == SDL_KEYUP && event.key.keysym.sym == turbo_key &&
          !(event.key.keysym.mod & KMOD_SHIFT))
        turbo_hold = false;
      if ((event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) &&
          event.key.keysym.sym != turbo_key)
        set_button(event.key.keysym.sym, event.type == SDL_KEYDOWN);
    }
    core.run();
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
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_width, frame_height,
                    GL_BGRA, GL_UNSIGNED_BYTE, framebuffer.data());
    const float aspect = 3.0f / 2.0f;
    float width = static_cast<float>(output_width);
    float height = width / aspect;
    if (height > output_height) {
      height = static_cast<float>(output_height);
      width = height * aspect;
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
    const float jackhammer_impact = title_edition == 1 && !jackhammer_flights.empty()
        ? static_cast<float>(std::sin(animation_now * 6.283185307 * 14.0) *
                             16.0 * shake_intensity)
        : 0.0f;
    glUniform1i(glGetUniformLocation(crt_program, "frame"), 0);
    glUniform2f(glGetUniformLocation(crt_program, "texel"),
                1.0f / frame_width, 1.0f / frame_height);
    const float shake = title_edition == 1
        ? ((fps_frames & 1u) ? 24.0f : -24.0f) * shake_intensity + jackhammer_impact
        : 0.0f;
    glUniform4f(glGetUniformLocation(crt_program, "screenRect"),
                (output_width - width) * 0.5f,
                (output_height - height) * 0.5f + shake,
                width, height);
    glUniform1f(glGetUniformLocation(crt_program, "enabled"), crt_filter ? 1.0f : 0.0f);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(2, 0); glVertex2f(3, -1);
    glTexCoord2f(0, 2); glVertex2f(-1, 3);
    glEnd();
    if (title_edition == 1 && jackhammer_texture) {
      const double now_seconds = animation_now;
      const double difficulty_elapsed =
          now_seconds - jackhammer_session_start;
      constexpr double ramp_start = 30.0;
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
                ? jackhammer_session_start + 30.0
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
  glDeleteTextures(1, &gl_texture);
  if (jackhammer_texture) glDeleteTextures(1, &jackhammer_texture);
  glDeleteProgram(crt_program);
  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
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
  dlclose(core.handle);
  if (!return_to_rom_selector) {
    SDL_Quit();
    return 0;
  }

  SDL_Window *selector_window = SDL_CreateWindow(
      "Flowmulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
      800, 600, SDL_WINDOW_SHOWN);
  if (selector_window) SDL_SetWindowOpacity(selector_window, 1.0f);
  SDL_Renderer *selector_renderer = selector_window
      ? SDL_CreateRenderer(selector_window, -1,
                           SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)
      : nullptr;
  if (!selector_renderer && selector_window)
    selector_renderer = SDL_CreateRenderer(selector_window, -1,
                                           SDL_RENDERER_SOFTWARE);
  if (!selector_window || !selector_renderer) {
    if (selector_renderer) SDL_DestroyRenderer(selector_renderer);
    if (selector_window) SDL_DestroyWindow(selector_window);
    SDL_Quit();
    return 1;
  }
  const std::string next_rom = choose_rom(selector_renderer);
  if (!next_rom.empty()) {
    rom_path = next_rom;
    SDL_DestroyRenderer(selector_renderer);
    SDL_DestroyWindow(selector_window);
    return_to_rom_selector = false;
    continue;
  }
  rom_path.clear();
  const bool menu_running =
      menu(selector_window, selector_renderer, rom_path);
  SDL_DestroyRenderer(selector_renderer);
  SDL_DestroyWindow(selector_window);
  if (!menu_running) {
    SDL_Quit();
    return 0;
  }
  return_to_rom_selector = false;
  }
}
