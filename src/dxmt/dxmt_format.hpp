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
  uint32_t Flag;
};

enum class DXGIFormatClass : uint32_t {
  Native,
  Emulated,
  Mask,
  Unsupported,
};

enum DXGIFormatTraitFlags : uint32_t {
  DXGI_FORMAT_TRAIT_NONE = 0,
  DXGI_FORMAT_TRAIT_MULTIPLANE = 1u << 0,
  DXGI_FORMAT_TRAIT_VIDEO = 1u << 1,
  DXGI_FORMAT_TRAIT_DEPTH_STENCIL = 1u << 2,
};

struct DXGIFormatPlaneTraits {
  uint32_t backingFormat = 0;
  uint32_t viewFormat = 0;
  uint32_t footprintFormat = 0;
  uint32_t elementSize = 0;
  uint32_t subsampleXLog2 = 0;
  uint32_t subsampleYLog2 = 0;
};

struct DXGIFormatTraits {
  uint32_t format = 0;
  DXGIFormatClass classification = DXGIFormatClass::Unsupported;
  uint32_t planeCount = 0;
  uint32_t flags = DXGI_FORMAT_TRAIT_NONE;
  DXGIFormatPlaneTraits planes[3] = {};
};

const DXGIFormatTraits &GetDXGIFormatTraits(uint32_t format);

bool IsDXGIFormatSupportedByTraits(uint32_t format);

bool IsDXGIFormatPlaneCompatible(uint32_t allocation_format, uint32_t view_or_copy_format,
                                 uint32_t plane);

int32_t MTLQueryDXGIFormat(WMT::Device device, uint32_t format, MTL_DXGI_FORMAT_DESC &description);

uint32_t MTLGetTexelSize(WMTPixelFormat format);

WMTPixelFormat MTLGetUnsignedIntegerFormat(WMTPixelFormat format);

bool IsUnorm8RenderTargetFormat(WMTPixelFormat format);

bool IsIntegerFormat(WMTPixelFormat format);

} // namespace dxmt
