#pragma once

#include <stddef.h>
#include <stdint.h>

typedef bool retro_bool;
typedef retro_bool (*retro_environment_t)(unsigned, void *);
typedef void (*retro_video_refresh_t)(const void *, unsigned, unsigned, size_t);
typedef void (*retro_audio_sample_t)(int16_t, int16_t);
typedef size_t (*retro_audio_sample_batch_t)(const int16_t *, size_t);
typedef void (*retro_input_poll_t)(void);
typedef int16_t (*retro_input_state_t)(unsigned, unsigned, unsigned, unsigned);
typedef void (*retro_hw_context_reset_t)(void);
typedef uintptr_t (*retro_hw_get_current_framebuffer_t)(void);
typedef void *(*retro_hw_get_proc_address_t)(const char *);

struct retro_game_info {
  const char *path;
  const void *data;
  size_t size;
  const char *meta;
};


struct retro_system_info {
  const char *library_name;
  const char *library_version;
  const char *valid_extensions;
  retro_bool need_fullpath;
  retro_bool block_extract;
};

struct retro_game_geometry {
  unsigned base_width;
  unsigned base_height;
  unsigned max_width;
  unsigned max_height;
  float aspect_ratio;
};

struct retro_system_timing {
  double fps;
  double sample_rate;
};

struct retro_system_av_info {
  retro_game_geometry geometry;
  retro_system_timing timing;
};

enum {
  RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY = 9,
  RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10,
  RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME = 18,
  RETRO_ENVIRONMENT_GET_LOG_INTERFACE = 27,
  RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY = 31,
  RETRO_ENVIRONMENT_GET_VARIABLE = 15,
  RETRO_ENVIRONMENT_SET_VARIABLES = 16,
  RETRO_ENVIRONMENT_SET_GEOMETRY = 37,
  RETRO_ENVIRONMENT_SET_HW_RENDER = 14,
  RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE = 41 | 0x10000,
  RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE = 43 | 0x10000,
  /* Older libretro headers assigned this experimental command to 42. */
  RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_OLD = 42 | 0x10000,
  RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT = 73 | 0x10000,
  RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER = 56,
};

#define RETRO_HW_FRAME_BUFFER_VALID ((void *)(uintptr_t)-1)

enum retro_hw_context_type {
  RETRO_HW_CONTEXT_NONE = 0,
  RETRO_HW_CONTEXT_OPENGL = 1,
  RETRO_HW_CONTEXT_OPENGL_CORE = 3,
  RETRO_HW_CONTEXT_VULKAN = 6,
};

enum retro_hw_render_interface_type {
  RETRO_HW_RENDER_INTERFACE_VULKAN = 0,
};

struct retro_hw_render_interface {
  retro_hw_render_interface_type interface_type;
  unsigned interface_version;
};

enum retro_hw_render_context_negotiation_interface_type {
  RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN = 0,
};

struct retro_hw_render_context_negotiation_interface {
  retro_hw_render_context_negotiation_interface_type interface_type;
  unsigned interface_version;
};

struct retro_hw_render_callback {
  retro_hw_context_type context_type;
  retro_hw_context_reset_t context_reset;
  retro_hw_get_current_framebuffer_t get_current_framebuffer;
  retro_hw_get_proc_address_t get_proc_address;
  bool depth;
  bool stencil;
  bool bottom_left_origin;
  unsigned version_major;
  unsigned version_minor;
  bool cache_context;
  retro_hw_context_reset_t context_destroy;
  bool debug_context;
};

enum {
  RETRO_PIXEL_FORMAT_0RGB1555 = 0,
  RETRO_PIXEL_FORMAT_XRGB8888 = 1,
  RETRO_PIXEL_FORMAT_RGB565 = 2,
};

enum {
  RETRO_DEVICE_JOYPAD = 1,
  RETRO_DEVICE_ID_JOYPAD_B = 0,
  RETRO_DEVICE_ID_JOYPAD_Y = 1,
  RETRO_DEVICE_ID_JOYPAD_SELECT = 2,
  RETRO_DEVICE_ID_JOYPAD_START = 3,
  RETRO_DEVICE_ID_JOYPAD_UP = 4,
  RETRO_DEVICE_ID_JOYPAD_DOWN = 5,
  RETRO_DEVICE_ID_JOYPAD_LEFT = 6,
  RETRO_DEVICE_ID_JOYPAD_RIGHT = 7,
  RETRO_DEVICE_ID_JOYPAD_A = 8,
  RETRO_DEVICE_ID_JOYPAD_X = 9,
  RETRO_DEVICE_ID_JOYPAD_L = 10,
  RETRO_DEVICE_ID_JOYPAD_R = 11,
};

enum {
  RETRO_DEVICE_MOUSE = 2,
  RETRO_DEVICE_ID_MOUSE_X = 0,
  RETRO_DEVICE_ID_MOUSE_Y = 1,
  RETRO_DEVICE_ID_MOUSE_LEFT = 2,
};

enum {
  RETRO_DEVICE_POINTER = 6,
  RETRO_DEVICE_ID_POINTER_X = 0,
  RETRO_DEVICE_ID_POINTER_Y = 1,
  RETRO_DEVICE_ID_POINTER_PRESSED = 2,
};

struct retro_variable {
  const char *key;
  const char *value;
};
