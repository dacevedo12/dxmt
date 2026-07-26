#pragma once

#include "dxmt_scaler.hpp"
#include "dxmt_texture.hpp"
#include <cstdint>

namespace dxmt::d3d12 {

struct CachedTemporalScaler final {
  WMTPixelFormat color_pixel_format = WMTPixelFormatInvalid;
  WMTPixelFormat output_pixel_format = WMTPixelFormatInvalid;
  WMTPixelFormat depth_pixel_format = WMTPixelFormatInvalid;
  WMTPixelFormat motion_texture_pixel_format = WMTPixelFormatInvalid;
  bool auto_exposure = false;
  bool motion_vector_in_display_res = false;
  uint32_t input_width = 0;
  uint32_t input_height = 0;
  uint32_t motion_width = 0;
  uint32_t motion_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  Rc<TemporalScaler> scaler;
  Rc<Texture> mv_downscaled;
};

[[nodiscard]] WMTPixelFormat
TemporalUpscaleMotionVectorSourceFormat(WMTPixelFormat format) noexcept;

[[nodiscard]] WMTPixelFormat TemporalUpscaleMotionTextureFormat(
    WMTPixelFormat source_format,
    bool motion_vector_in_display_resolution) noexcept;

} // namespace dxmt::d3d12
