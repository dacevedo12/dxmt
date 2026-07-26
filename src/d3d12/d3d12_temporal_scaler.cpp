#include "d3d12_temporal_scaler.hpp"

namespace dxmt::d3d12 {

WMTPixelFormat
TemporalUpscaleMotionVectorSourceFormat(WMTPixelFormat format) noexcept {
  switch (format) {
  case WMTPixelFormatRG16Uint:
  case WMTPixelFormatRG16Float:
  case WMTPixelFormatRG16Sint:
  case WMTPixelFormatRG16Snorm:
  case WMTPixelFormatRG16Unorm:
    return WMTPixelFormatRG16Float;
  case WMTPixelFormatRG32Uint:
  case WMTPixelFormatRG32Float:
  case WMTPixelFormatRG32Sint:
    return WMTPixelFormatRG32Float;
  case WMTPixelFormatRGBA16Sint:
  case WMTPixelFormatRGBA16Snorm:
  case WMTPixelFormatRGBA16Uint:
  case WMTPixelFormatRGBA16Unorm:
  case WMTPixelFormatRGBA16Float:
    return WMTPixelFormatRGBA16Float;
  default:
    return WMTPixelFormatInvalid;
  }
}

WMTPixelFormat TemporalUpscaleMotionTextureFormat(
    WMTPixelFormat source_format,
    bool motion_vector_in_display_resolution) noexcept {
  return motion_vector_in_display_resolution ? WMTPixelFormatRG32Float
                                             : source_format;
}

} // namespace dxmt::d3d12
