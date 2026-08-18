#pragma once

#include "Metal.hpp"
#include <map>

namespace dxmt {
enum class FormatCapability : int {
  None = 0,
  Atomic = 0x1,
  Filter = 0x2,
  Write = 0x4,
  Color = 0x8,
  Blend = 0x10,
  MSAA = 0x20,
  Sparse = 0x40,
  Resolve = 0x80,
  DepthStencil = 0x100,
  TextureBufferRead = 0x200,
  TextureBufferWrite = 0x400,
  TextureBufferReadWrite = 0x800
};

class FormatCapabilityInspector {
public:
  std::map<WMTPixelFormat, FormatCapability> textureCapabilities{};
  void Inspect(WMT::Device device);
};

WMTPixelFormat Forget_sRGB(WMTPixelFormat format);
WMTPixelFormat Recall_sRGB(WMTPixelFormat format);

inline bool
Is_sRGBVariant(WMTPixelFormat format) {
  return Forget_sRGB(format) != format;
}

// The sRGB view a render target is written through must not carry the X-format
// AlphaIsOne swizzle: a swizzled view is sample-only and drops the render-target
// usage, so the attachment is discarded (an X8R8G8B8 target would render nothing
// under D3DRS_SRGBWRITEENABLE). Only the colour channels need sRGB encoding; the
// ignored X / alpha lane is written linearly, so the alpha-inclusive sRGB variant
// is the correct write format. Returns the input unchanged when it has no sRGB
// pair.
inline WMTPixelFormat
Recall_sRGB_ForRenderTarget(WMTPixelFormat format) {
  WMTPixelFormat base = static_cast<WMTPixelFormat>(format & ~WMTPixelFormatAlphaIsOne);
  WMTPixelFormat srgb = Recall_sRGB(base);
  return srgb != base ? srgb : format;
}

bool IsBlockCompressionFormat(WMTPixelFormat format);

uint32_t DepthStencilPlanarFlags(WMTPixelFormat format);

enum MTL_DXGI_FORMAT_FLAG {
  MTL_DXGI_FORMAT_TYPELESS = 1,
  MTL_DXGI_FORMAT_BC = 2,
  MTL_DXGI_FORMAT_BACKBUFFER = 4,
  MTL_DXGI_FORMAT_DEPTH_PLANER = 16,
  MTL_DXGI_FORMAT_STENCIL_PLANER = 32,
  MTL_DXGI_FORMAT_EMULATED_D24 = 256,
  MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL = 512,
};

struct MTL_DXGI_FORMAT_DESC {
  WMTPixelFormat PixelFormat;
  WMTAttributeFormat AttributeFormat;
  union {
    uint32_t BytesPerTexel;
    uint32_t BlockSize;
  };
  uint16_t PlanarCount;
  uint16_t Flag;
};

int32_t MTLQueryDXGIFormat(WMT::Device device, uint32_t format, MTL_DXGI_FORMAT_DESC &description);

uint32_t MTLGetTexelSize(WMTPixelFormat format);

WMTPixelFormat MTLGetUnsignedIntegerFormat(WMTPixelFormat format);

bool IsUnorm8RenderTargetFormat(WMTPixelFormat format);

bool IsIntegerFormat(WMTPixelFormat format);

void SanitizeRTVClearColor(WMTPixelFormat format, WMTClearColor &color);

} // namespace dxmt