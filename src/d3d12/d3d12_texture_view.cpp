#include "d3d12_texture_view.hpp"

#include "dxmt_format.hpp"
#include "log/log.hpp"

namespace dxmt::d3d12 {

WMTPixelFormat ResolveTextureViewFormat(WMT::Device device,
                                        Resource &resource,
                                        DXGI_FORMAT format, UINT plane,
                                        const char *context) {
  auto *texture = resource.GetTexture(plane);
  if (!texture)
    return WMTPixelFormatInvalid;
  if (format == DXGI_FORMAT_UNKNOWN)
    return texture->pixelFormat();
  if (DepthStencilPlanarFlags(texture->pixelFormat())) {
    switch (format) {
    case DXGI_FORMAT_R16_UNORM:
      if (texture->pixelFormat() == WMTPixelFormatDepth16Unorm)
        return texture->pixelFormat();
      break;
    case DXGI_FORMAT_R32_FLOAT:
      if (texture->pixelFormat() == WMTPixelFormatDepth32Float)
        return texture->pixelFormat();
      break;
    default:
      break;
    }
  }

  MTL_DXGI_FORMAT_DESC format_desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, format, format_desc)) ||
      format_desc.PixelFormat == WMTPixelFormatInvalid) {
    WARN(context, ": unsupported texture view format ", uint32_t(format));
    return WMTPixelFormatInvalid;
  }
  return format_desc.PixelFormat;
}

WMTPixelFormat ResolveRenderTargetTextureViewFormat(
    WMT::Device device, Resource &resource, DXGI_FORMAT format) {
  auto resolved = ResolveTextureViewFormat(device, resource, format, 0,
                                           "D3D12CommandQueue");
  if (resolved == WMTPixelFormatInvalid)
    return resolved;

  if (DepthStencilPlanarFlags(resolved)) {
    WARN("D3D12CommandQueue: unsupported RTV texture view format ",
         uint32_t(format));
    return WMTPixelFormatInvalid;
  }

  return resolved;
}

WMTPixelFormat ResolveDepthStencilViewFormat(WMT::Device device,
                                             Resource &resource,
                                             DXGI_FORMAT format) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return WMTPixelFormatInvalid;
  if (format == DXGI_FORMAT_UNKNOWN)
    return texture->pixelFormat();

  MTL_DXGI_FORMAT_DESC format_desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, format, format_desc)) ||
      !DepthStencilPlanarFlags(format_desc.PixelFormat)) {
    WARN("D3D12CommandQueue: unsupported DSV texture view format ",
         uint32_t(format));
    return WMTPixelFormatInvalid;
  }
  return format_desc.PixelFormat;
}

TextureViewKey CreateRenderTargetView(WMT::Device device, Resource &resource,
                                      const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = 1;
  view.firstArraySlice = 0;
  view.arraySize = texture->arrayLength();
  view.intendedUsage = WMTTextureUsageRenderTarget;

  if (descriptor.has_desc) {
    const auto &rtv = descriptor.desc.rtv;
    view.format =
        ResolveRenderTargetTextureViewFormat(device, resource, rtv.Format);
    if (view.format == WMTPixelFormatInvalid)
      return {};

    switch (rtv.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE1D:
      view.type = WMTTextureType2D;
      view.firstMiplevel = rtv.Texture1D.MipSlice;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = rtv.Texture1DArray.MipSlice;
      view.firstArraySlice = rtv.Texture1DArray.FirstArraySlice;
      view.arraySize = rtv.Texture1DArray.ArraySize;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2D:
      view.firstMiplevel = rtv.Texture2D.MipSlice;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = rtv.Texture2DArray.MipSlice;
      view.firstArraySlice = rtv.Texture2DArray.FirstArraySlice;
      view.arraySize = rtv.Texture2DArray.ArraySize;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMS:
      view.type = WMTTextureType2DMultisample;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
      view.type = WMTTextureType2DMultisampleArray;
      view.firstArraySlice = rtv.Texture2DMSArray.FirstArraySlice;
      view.arraySize = rtv.Texture2DMSArray.ArraySize;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE3D:
      view.type = WMTTextureType3D;
      view.firstMiplevel = rtv.Texture3D.MipSlice;
      view.firstArraySlice = 0;
      view.arraySize = 1;
      break;
    default:
      break;
    }
  }

  auto key = texture->createView(view);
  D3D12DiagLogTextureView("RTV", resource, descriptor, view, key);
  return key;
}

TextureViewKey CreateDepthStencilView(WMT::Device device, Resource &resource,
                                      const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = 1;
  view.firstArraySlice = 0;
  view.arraySize = texture->arrayLength();
  view.intendedUsage = WMTTextureUsageRenderTarget;

  if (descriptor.has_desc) {
    const auto &dsv = descriptor.desc.dsv;
    view.format = ResolveDepthStencilViewFormat(device, resource, dsv.Format);
    if (view.format == WMTPixelFormatInvalid)
      return {};

    switch (dsv.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE1D:
      view.type = WMTTextureType2D;
      view.firstMiplevel = dsv.Texture1D.MipSlice;
      view.arraySize = 1;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = dsv.Texture1DArray.MipSlice;
      view.firstArraySlice = dsv.Texture1DArray.FirstArraySlice;
      view.arraySize = dsv.Texture1DArray.ArraySize;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2D:
      view.firstMiplevel = dsv.Texture2D.MipSlice;
      view.arraySize = 1;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = dsv.Texture2DArray.MipSlice;
      view.firstArraySlice = dsv.Texture2DArray.FirstArraySlice;
      view.arraySize = dsv.Texture2DArray.ArraySize;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMS:
      view.type = WMTTextureType2DMultisample;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
      view.type = WMTTextureType2DMultisampleArray;
      view.firstArraySlice = dsv.Texture2DMSArray.FirstArraySlice;
      view.arraySize = dsv.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
    }
  }

  auto key = texture->createView(view);
  if (!key)
    D3D12DiagLogDSVReplayDescriptor("createView returned empty key", resource,
                                    descriptor, view, key);
  D3D12DiagLogTextureView("DSV", resource, descriptor, view, key);
  return key;
}

} // namespace dxmt::d3d12
