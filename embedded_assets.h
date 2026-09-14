#pragma once

#include <cstddef>
#include <cstdint>

extern const uint8_t _binary__assets_frontend_background_bmp_start[];
extern const uint8_t _binary__assets_frontend_background_bmp_end[];
extern const uint8_t _binary__assets_edgerunners_background_bmp_start[];
extern const uint8_t _binary__assets_edgerunners_background_bmp_end[];
extern const uint8_t _binary__assets_berserk_background_bmp_start[];
extern const uint8_t _binary__assets_berserk_background_bmp_end[];
extern const uint8_t _binary__assets_gba_overlay_rgba_start[];
extern const uint8_t _binary__assets_gba_overlay_rgba_end[];
extern const uint8_t _binary__assets_nds_overlay_rgba_start[];
extern const uint8_t _binary__assets_nds_overlay_rgba_end[];
extern const uint8_t _binary__assets_nds_top_overlay_rgba_start[];
extern const uint8_t _binary__assets_nds_top_overlay_rgba_end[];
extern const uint8_t _binary__assets_jackhammer_rgba_start[];
extern const uint8_t _binary__assets_jackhammer_rgba_end[];
extern const uint8_t _binary__assets_jackhammer_wav_start[];
extern const uint8_t _binary__assets_jackhammer_wav_end[];
extern const uint8_t _binary__assets_big_forehead_wav_start[];
extern const uint8_t _binary__assets_big_forehead_wav_end[];
extern const uint8_t _binary__assets_skywalker_rgba_start[];
extern const uint8_t _binary__assets_skywalker_rgba_end[];
extern const uint8_t _binary__assets_water_splash_rgba_start[];
extern const uint8_t _binary__assets_water_splash_rgba_end[];
extern const uint8_t _binary__assets_skywalker_splash_wav_start[];
extern const uint8_t _binary__assets_skywalker_splash_wav_end[];
extern const uint8_t _binary__assets_skywalker_oooooh_wav_start[];
extern const uint8_t _binary__assets_skywalker_oooooh_wav_end[];
extern const uint8_t _binary_Helper_sh_start[];
extern const uint8_t _binary_Helper_sh_end[];
extern const uint8_t _binary__assets_flowmulator_svg_start[];
extern const uint8_t _binary__assets_flowmulator_svg_end[];
extern const uint8_t _binary__runtime_azahar_AppImage_start[];
extern const uint8_t _binary__runtime_azahar_AppImage_end[];
extern const uint8_t _binary__runtime_azahar_libretro_so_start[];
extern const uint8_t _binary__runtime_azahar_libretro_so_end[];
extern const uint8_t _binary__runtime_mgba_libretro_so_start[];
extern const uint8_t _binary__runtime_mgba_libretro_so_end[];
extern const uint8_t _binary__runtime_desmume_libretro_so_start[];
extern const uint8_t _binary__runtime_desmume_libretro_so_end[];
extern const uint8_t _binary__runtime_MiiFix_3dsx_start[];
extern const uint8_t _binary__runtime_MiiFix_3dsx_end[];

inline std::size_t embedded_size(const uint8_t *start, const uint8_t *end) {
  return static_cast<std::size_t>(end - start);
}