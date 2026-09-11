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
  RETRO_ENVIRONMENT_SET_GEOMETRY = 37,
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
