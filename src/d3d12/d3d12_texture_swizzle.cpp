#include "d3d12_texture_swizzle.hpp"

#include "log/log.hpp"

namespace dxmt::d3d12 {
namespace {

WMTTextureSwizzleChannels DefaultTextureViewSwizzle() {
  return {
      WMTTextureSwizzleRed,
      WMTTextureSwizzleGreen,
      WMTTextureSwizzleBlue,
      WMTTextureSwizzleAlpha,
  };
}

WMTTextureSwizzleChannels
BaseShaderReadSwizzleForFormat(WMTPixelFormat format) {
  switch (format) {
  case WMTPixelFormatA8Unorm:
    return {
        WMTTextureSwizzleZero,
        WMTTextureSwizzleZero,
        WMTTextureSwizzleZero,
        WMTTextureSwizzleRed,
    };
  case WMTPixelFormatR8Unorm:
  case WMTPixelFormatR8Unorm_sRGB:
  case WMTPixelFormatR8Snorm:
  case WMTPixelFormatR8Uint:
  case WMTPixelFormatR8Sint:
  case WMTPixelFormatR16Unorm:
  case WMTPixelFormatR16Snorm:
  case WMTPixelFormatR16Uint:
  case WMTPixelFormatR16Sint:
  case WMTPixelFormatR16Float:
  case WMTPixelFormatR32Uint:
  case WMTPixelFormatR32Sint:
  case WMTPixelFormatR32Float:
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC4_RSnorm:
  case WMTPixelFormatEAC_R11Unorm:
  case WMTPixelFormatEAC_R11Snorm:
  case WMTPixelFormatDepth16Unorm:
  case WMTPixelFormatDepth32Float:
    return {
        WMTTextureSwizzleRed,
        WMTTextureSwizzleZero,
        WMTTextureSwizzleZero,
        WMTTextureSwizzleOne,
    };
  case WMTPixelFormatRG8Unorm:
  case WMTPixelFormatRG8Unorm_sRGB:
  case WMTPixelFormatRG8Snorm:
  case WMTPixelFormatRG8Uint:
  case WMTPixelFormatRG8Sint:
  case WMTPixelFormatRG16Unorm:
  case WMTPixelFormatRG16Snorm:
  case WMTPixelFormatRG16Uint:
  case WMTPixelFormatRG16Sint:
  case WMTPixelFormatRG16Float:
  case WMTPixelFormatRG32Uint:
  case WMTPixelFormatRG32Sint:
  case WMTPixelFormatRG32Float:
  case WMTPixelFormatBC5_RGUnorm:
  case WMTPixelFormatBC5_RGSnorm:
  case WMTPixelFormatEAC_RG11Unorm:
  case WMTPixelFormatEAC_RG11Snorm:
    return {
        WMTTextureSwizzleRed,
        WMTTextureSwizzleGreen,
        WMTTextureSwizzleZero,
        WMTTextureSwizzleOne,
    };
  case WMTPixelFormatRG11B10Float:
  case WMTPixelFormatRGB9E5Float:
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatB5G6R5Unorm:
  case WMTPixelFormatPVRTC_RGB_2BPP:
  case WMTPixelFormatPVRTC_RGB_2BPP_sRGB:
  case WMTPixelFormatPVRTC_RGB_4BPP:
  case WMTPixelFormatPVRTC_RGB_4BPP_sRGB:
  case WMTPixelFormatBGRX8Unorm:
  case WMTPixelFormatBGRX8Unorm_sRGB:
    return {
        WMTTextureSwizzleRed,
        WMTTextureSwizzleGreen,
        WMTTextureSwizzleBlue,
        WMTTextureSwizzleOne,
    };
  default:
    return DefaultTextureViewSwizzle();
  }
}

WMTTextureSwizzle
TextureSwizzleFromD3D12Component(UINT component_mapping,
                                 UINT component_index) {
  switch (D3D12_DECODE_SHADER_4_COMPONENT_MAPPING(component_index,
                                                  component_mapping)) {
  case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_0:
    return WMTTextureSwizzleRed;
  case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1:
    return WMTTextureSwizzleGreen;
  case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_2:
    return WMTTextureSwizzleBlue;
  case D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_3:
    return WMTTextureSwizzleAlpha;
  case D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0:
    return WMTTextureSwizzleZero;
  case D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1:
    return WMTTextureSwizzleOne;
  default:
    WARN("D3D12CommandQueue: invalid shader component mapping ",
         component_mapping);
    return WMTTextureSwizzleZero;
  }
}

WMTTextureSwizzle
ComposeTextureSwizzleComponent(const WMTTextureSwizzleChannels &base,
                               WMTTextureSwizzle component) {
  switch (component) {
  case WMTTextureSwizzleRed:
    return base.r;
  case WMTTextureSwizzleGreen:
    return base.g;
  case WMTTextureSwizzleBlue:
    return base.b;
  case WMTTextureSwizzleAlpha:
    return base.a;
  case WMTTextureSwizzleZero:
  case WMTTextureSwizzleOne:
    return component;
  default:
    return WMTTextureSwizzleZero;
  }
}

} // namespace

WMTTextureSwizzleChannels
ShaderResourceViewSwizzle(WMTPixelFormat format, UINT component_mapping) {
  const auto base = BaseShaderReadSwizzleForFormat(format);
  return {
      ComposeTextureSwizzleComponent(
          base, TextureSwizzleFromD3D12Component(component_mapping, 0)),
      ComposeTextureSwizzleComponent(
          base, TextureSwizzleFromD3D12Component(component_mapping, 1)),
      ComposeTextureSwizzleComponent(
          base, TextureSwizzleFromD3D12Component(component_mapping, 2)),
      ComposeTextureSwizzleComponent(
          base, TextureSwizzleFromD3D12Component(component_mapping, 3)),
  };
}

} // namespace dxmt::d3d12
