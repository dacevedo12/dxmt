#include "d3d12_replay_resolve_encode.hpp"

#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_resolve_region.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"

#include <cstdint>
#include <utility>

namespace dxmt::d3d12 {

TextureViewKey
CreateResolveView(Resource &resource, UINT subresource, WMTPixelFormat format,
                  WMTTextureUsage intended_usage) {
  auto *texture = resource.GetTexture(GetSubresourcePlane(resource, subresource));
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = format;
  view.type = texture->textureType();
  view.firstMiplevel = GetMipLevel(resource, subresource);
  view.miplevelCount = 1;
  view.firstArraySlice = GetArraySlice(resource, subresource);
  view.arraySize = 1;
  view.intendedUsage = intended_usage;
  return texture->createView(view);
}

void
ReplayResolveSubresource(WMT::Device device, CommandChunk *chunk,
                         const ResolveSubresourceRecord &record) {
  auto *dst = GetResource(record.dst.ptr());
  auto *src = GetResource(record.src.ptr());
  if (!dst || !src)
    return;
  if (dst->IsReservedTexture())
    dst->EnsureTextureAllocation("ResolveSubresource dst");
  if (src->IsReservedTexture())
    src->EnsureTextureAllocation("ResolveSubresource src");
  if (!dst->GetTexture() || !src->GetTexture())
    return;
  dst->SetPresentSourceView({});

  const auto &dst_desc = dst->GetResourceDesc();
  const auto &src_desc = src->GetResourceDesc();
  if (src_desc.SampleDesc.Count <= 1 || dst_desc.SampleDesc.Count != 1) {
    WARN("D3D12CommandQueue: ResolveSubresource supports MSAA color source to single-sample destination only");
    return;
  }
  if (record.dst_subresource >= GetSubresourceCount(*dst) ||
      record.src_subresource >= GetSubresourceCount(*src)) {
    WARN("D3D12CommandQueue: ResolveSubresource subresource out of range");
    return;
  }

  WMTPixelFormat format = src->GetTexture()->pixelFormat();
  if (record.format != DXGI_FORMAT_UNKNOWN) {
    MTL_DXGI_FORMAT_DESC format_desc = {};
    if (FAILED(MTLQueryDXGIFormat(device, record.format, format_desc)) ||
        format_desc.PixelFormat == WMTPixelFormatInvalid) {
      WARN("D3D12CommandQueue: ResolveSubresource unsupported format ",
           uint32_t(record.format));
      return;
    }
    format = format_desc.PixelFormat;
  }

  if (DepthStencilPlanarFlags(format) || IsIntegerFormat(format)) {
    WARN("D3D12CommandQueue: ResolveSubresource supports non-integer color formats only");
    return;
  }
  if (src->GetTexture()->pixelFormat() != format ||
      dst->GetTexture()->pixelFormat() != format) {
    WARN("D3D12CommandQueue: ResolveSubresource currently supports same-format color resolves only");
    return;
  }

  auto mode = ConvertResolveMode(record.mode);
  if (!mode) {
    WARN("D3D12CommandQueue: ResolveSubresource unsupported resolve mode ",
         uint32_t(record.mode));
    return;
  }

  const UINT src_mip = GetMipLevel(*src, record.src_subresource);
  const UINT dst_mip = GetMipLevel(*dst, record.dst_subresource);
  const uint64_t src_width = ResolveMipLevelWidth(src_desc, src_mip);
  const uint64_t src_height = ResolveMipLevelHeight(src_desc, src_mip);
  const uint64_t dst_width = ResolveMipLevelWidth(dst_desc, dst_mip);
  const uint64_t dst_height = ResolveMipLevelHeight(dst_desc, dst_mip);

  WMTScissorRect src_rect = {};
  WMTOrigin dst_origin = {};
  WMTSize resolve_size = {};
  if (!NormalizeResolveRegion(record, src_width, src_height, dst_width,
                              dst_height, src_rect, dst_origin,
                              resolve_size))
    return;

  auto src_view = CreateResolveView(*src, record.src_subresource, format,
                                    WMTTextureUsageRenderTarget);
  auto dst_view = CreateResolveView(*dst, record.dst_subresource, format,
                                    WMTTextureUsageRenderTarget);
  Rc<Texture> src_texture = src->GetTexture();
  Rc<Texture> dst_texture = dst->GetTexture();
  const bool fast_path =
      *mode == ResolveTextureMode::Average &&
      IsFullResolveRegion(record, src_width, src_height, dst_width,
                          dst_height);
  chunk->emitcc([src_texture = std::move(src_texture),
                 dst_texture = std::move(dst_texture), src_view, dst_view,
                 mode = *mode, src_rect, dst_origin, resolve_size,
                 fast_path](ArgumentEncodingContext &enc) mutable {
    if (fast_path) {
      enc.resolveTexture(std::move(src_texture), src_view,
                         std::move(dst_texture), dst_view);
    } else {
      enc.resolve_texture_cmd.resolve(std::move(src_texture), src_view,
                                      std::move(dst_texture), dst_view,
                                      mode, src_rect, dst_origin,
                                      resolve_size);
    }
  });
}

} // namespace dxmt::d3d12
