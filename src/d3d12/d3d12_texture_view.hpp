#pragma once

#include "d3d12_descriptor_diagnostics.hpp"

namespace dxmt::d3d12 {

[[nodiscard]] WMTPixelFormat
ResolveTextureViewFormat(WMT::Device device, Resource &resource,
                         DXGI_FORMAT format, UINT plane,
                         const char *context);

[[nodiscard]] WMTPixelFormat
ResolveRenderTargetTextureViewFormat(WMT::Device device, Resource &resource,
                                     DXGI_FORMAT format);

[[nodiscard]] WMTPixelFormat
ResolveDepthStencilViewFormat(WMT::Device device, Resource &resource,
                              DXGI_FORMAT format);

[[nodiscard]] TextureViewKey
CreateRenderTargetView(WMT::Device device, Resource &resource,
                       const DescriptorRecord &descriptor);

[[nodiscard]] TextureViewKey
CreateDepthStencilView(WMT::Device device, Resource &resource,
                       const DescriptorRecord &descriptor);

} // namespace dxmt::d3d12
