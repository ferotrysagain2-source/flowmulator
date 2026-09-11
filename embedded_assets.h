#pragma once

#include <cstddef>
#include <cstdint>

extern const uint8_t _binary__assets_frontend_background_bmp_start[];
extern const uint8_t _binary__assets_frontend_background_bmp_end[];
extern const uint8_t _binary__assets_jackhammer_rgba_start[];
extern const uint8_t _binary__assets_jackhammer_rgba_end[];
extern const uint8_t _binary__assets_jackhammer_wav_start[];
extern const uint8_t _binary__assets_jackhammer_wav_end[];
extern const uint8_t _binary__assets_big_forehead_wav_start[];
extern const uint8_t _binary__assets_big_forehead_wav_end[];
extern const uint8_t _binary_install_dependencies_sh_start[];
extern const uint8_t _binary_install_dependencies_sh_end[];

inline std::size_t embedded_size(const uint8_t *start, const uint8_t *end) {
  return static_cast<std::size_t>(end - start);
}