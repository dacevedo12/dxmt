#include "d3d12_texture_copy_plan.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>

namespace dxmt::d3d12 {

CopyResourcePlan
PlanCopyResource(Resource &dst, Resource &src) {
  CopyResourcePlan plan = {};
  if (dst.GetBufferAllocation() && src.GetBufferAllocation()) {
    plan.kind = CopyResourceKind::Buffer;
    plan.byte_count =
        std::min(dst.GetResourceDesc().Width, src.GetResourceDesc().Width);
    return plan;
  }

  if (dst.IsReservedTexture())
    dst.EnsureTextureAllocation("CopyResource dst");
  if (src.IsReservedTexture())
    src.EnsureTextureAllocation("CopyResource src");
  if (dst.GetTextureAllocation() && src.GetTextureAllocation()) {
    const UINT dst_subresources = GetSubresourceCount(dst);
    const UINT src_subresources = GetSubresourceCount(src);
    plan.kind = CopyResourceKind::Texture;
    plan.subresource_count = std::min(dst_subresources, src_subresources);
  }
  return plan;
}

TextureRegionCopyPlan
PlanTextureRegionCopy(WMT::Device device, Resource &dst, Resource &src,
                      const CopyTextureRegionRecord &record,
                      UINT dst_subresource, UINT src_subresource) {
  dst.SetPresentSourceView({});
  const auto size =
      GetSubresourceSize(src, src_subresource,
                         record.src_box ? &*record.src_box : nullptr);
  const auto src_origin = record.src_box
                              ? WMTOrigin{record.src_box->left,
                                          record.src_box->top,
                                          record.src_box->front}
                              : WMTOrigin{0, 0, 0};
  const auto dst_origin = WMTOrigin{record.dst_x, record.dst_y, record.dst_z};
  if (!size.width || !size.height || !size.depth) {
    WARN("D3D12CommandQueue: texture copy source box is empty or outside the source subresource src_subresource=",
         uint32_t(src_subresource), " size=", uint32_t(size.width), "x",
         uint32_t(size.height), "x", uint32_t(size.depth));
    return {};
  }
  const UINT src_plane = GetSubresourcePlane(src, src_subresource);
  const UINT dst_plane = GetSubresourcePlane(dst, dst_subresource);
  const UINT dst_slice = GetArraySlice(dst, dst_subresource);
  const UINT dst_level = GetMipLevel(dst, dst_subresource);
  const UINT src_slice = GetArraySlice(src, src_subresource);
  const UINT src_level = GetMipLevel(src, src_subresource);
  if (src_plane != dst_plane) {
    WARN("D3D12CommandQueue: texture copy between different planes is not supported yet src_plane=",
         src_plane, " dst_plane=", dst_plane);
    return {};
  }
  Rc<Texture> dst_texture = Rc<Texture>(dst.GetTexture(dst_plane));
  Rc<Texture> src_texture = Rc<Texture>(src.GetTexture(src_plane));
  if (!dst_texture || !src_texture)
    return {};
  if (!ValidateTextureSubresourceAccess("texture copy dst", dst,
                                        dst_texture.ptr(), dst_subresource,
                                        dst_plane, dst_level, dst_slice) ||
      !ValidateTextureSubresourceAccess("texture copy src", src,
                                        src_texture.ptr(), src_subresource,
                                        src_plane, src_level, src_slice))
    return {};
  if (src_texture->pixelFormat() != dst_texture->pixelFormat()) {
    MTL_DXGI_FORMAT_DESC src_format_desc = {};
    MTL_DXGI_FORMAT_DESC dst_format_desc = {};
    const auto src_format = src.GetResourceDesc().Format;
    const auto dst_format = dst.GetResourceDesc().Format;
    const bool src_format_known =
        SUCCEEDED(MTLQueryDXGIFormat(device, src_format, src_format_desc));
    const bool dst_format_known =
        SUCCEEDED(MTLQueryDXGIFormat(device, dst_format, dst_format_desc));
    const bool src_is_uncompressed_block_payload =
        src_format == DXGI_FORMAT_R32G32B32A32_UINT;
    const bool dst_is_16_byte_block_compressed =
        dst_format_known && (dst_format_desc.Flag & MTL_DXGI_FORMAT_BC) &&
        dst_format_desc.BlockSize == kBlockReinterpretBytesPerTexel;

    // D3D12 permits a raw block copy from one R32G32B32A32_UINT texel to
    // one 16-byte BC block. Metal texture-to-texture blits require matching
    // pixel formats, so preserve the bits through an aligned temporary
    // buffer and expand the destination extent from blocks to texels.
    if (src_format_known && src_is_uncompressed_block_payload &&
        dst_is_16_byte_block_compressed && src_plane == 0 && dst_plane == 0 &&
        src_format_desc.BytesPerTexel == kBlockReinterpretBytesPerTexel) {
      const auto layout = ComputeBlockReinterpretCopyLayout(size);
      if (!layout) {
        WARN("D3D12CommandQueue: invalid block reinterpret texture copy size");
        return {};
      }

      TextureRegionCopyPlan plan = {};
      plan.kind = TextureRegionCopyKind::BlockReinterpret;
      plan.dst_texture = std::move(dst_texture);
      plan.src_texture = std::move(src_texture);
      plan.dst_slice = dst_slice;
      plan.dst_level = dst_level;
      plan.src_slice = src_slice;
      plan.src_level = src_level;
      plan.src_origin = src_origin;
      plan.dst_origin = dst_origin;
      plan.size = size;
      plan.block_layout = *layout;
      return plan;
    }

    WARN("D3D12CommandQueue: plane texture copy format mismatch src_format=",
         uint32_t(src_texture->pixelFormat()),
         " dst_format=", uint32_t(dst_texture->pixelFormat()),
         " plane=", uint32_t(src_plane));
    return {};
  }
  if (D3D12DiagTextureCopyEnabled()) {
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12DiagShouldLog(log_count, true)) {
      INFO("D3D12 diagnostic: texture copy record",
           " dst_resource=", uint64_t(dst.GetD3D12Resource()),
           " src_resource=", uint64_t(src.GetD3D12Resource()),
           " dst_texture=", dst_texture && dst_texture->current()
                              ? uint64_t(dst_texture->current()->texture())
                              : 0,
           " src_texture=", src_texture && src_texture->current()
                              ? uint64_t(src_texture->current()->texture())
                              : 0,
           " plane=", uint32_t(src_plane),
           " dst_subresource=", uint32_t(dst_subresource),
           " src_subresource=", uint32_t(src_subresource),
           " dst_level=", uint32_t(dst_level),
           " dst_slice=", uint32_t(dst_slice),
           " src_level=", uint32_t(src_level),
           " src_slice=", uint32_t(src_slice),
           " dst_origin=", uint32_t(dst_origin.x), ",",
           uint32_t(dst_origin.y), ",", uint32_t(dst_origin.z),
           " src_origin=", uint32_t(src_origin.x), ",",
           uint32_t(src_origin.y), ",", uint32_t(src_origin.z),
           " size=", uint32_t(size.width), "x", uint32_t(size.height),
           "x", uint32_t(size.depth),
           " dst_resource_size=", uint64_t(dst.GetResourceDesc().Width),
           "x", uint32_t(dst.GetResourceDesc().Height),
           " src_resource_size=", uint64_t(src.GetResourceDesc().Width),
           "x", uint32_t(src.GetResourceDesc().Height),
           " dst_format=", uint32_t(dst.GetResourceDesc().Format),
           " src_format=", uint32_t(src.GetResourceDesc().Format));
    }
  }

  TextureRegionCopyPlan plan = {};
  plan.kind = TextureRegionCopyKind::Direct;
  plan.dst_texture = std::move(dst_texture);
  plan.src_texture = std::move(src_texture);
  plan.dst_slice = dst_slice;
  plan.dst_level = dst_level;
  plan.src_slice = src_slice;
  plan.src_level = src_level;
  plan.src_origin = src_origin;
  plan.dst_origin = dst_origin;
  plan.size = size;
  return plan;
}

BufferTextureCopyPlan
PlanBufferTextureCopy(WMT::Device device, const CopyTextureRegionRecord &record,
                      Resource &dst, Resource &src) {
  const bool dst_is_buffer = dst.GetBufferAllocation() != nullptr;
  const bool src_is_buffer = src.GetBufferAllocation() != nullptr;
  if (dst_is_buffer == src_is_buffer)
    return {};

  auto &buffer_resource = dst_is_buffer ? dst : src;
  auto &texture_resource = dst_is_buffer ? src : dst;
  if (texture_resource.IsReservedTexture() &&
      !texture_resource.EnsureTextureAllocation("BufferTextureCopy"))
    return {};
  if (!dst_is_buffer)
    texture_resource.SetPresentSourceView({});
  Rc<Buffer> buffer = buffer_resource.GetBuffer();
  if (!buffer)
    return {};

  const auto &buffer_location = dst_is_buffer ? record.dst : record.src;
  const auto &texture_location = dst_is_buffer ? record.src : record.dst;
  if (buffer_location.type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
    return {};
  const UINT subresource =
      texture_location.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
          ? texture_location.subresource_index
          : 0;
  const UINT slice = GetArraySlice(texture_resource, subresource);
  const UINT level = GetMipLevel(texture_resource, subresource);
  const UINT plane = GetSubresourcePlane(texture_resource, subresource);
  Rc<Texture> texture = Rc<Texture>(texture_resource.GetTexture(plane));
  if (!texture)
    return {};
  if (!ValidateTextureSubresourceAccess("buffer texture copy",
                                        texture_resource, texture.ptr(),
                                        subresource, plane, level, slice))
    return {};
  const auto size =
      GetSubresourceSize(texture_resource, subresource,
                         record.src_box ? &*record.src_box : nullptr);
  const auto origin = record.src_box
                          ? WMTOrigin{record.src_box->left,
                                      record.src_box->top,
                                      record.src_box->front}
                          : WMTOrigin{dst_is_buffer ? 0u : record.dst_x,
                                      dst_is_buffer ? 0u : record.dst_y,
                                      dst_is_buffer ? 0u : record.dst_z};
  if (!size.width || !size.height || !size.depth) {
    WARN("D3D12CommandQueue: buffer texture copy source box is empty or outside the subresource subresource=",
         uint32_t(subresource), " size=", uint32_t(size.width), "x",
         uint32_t(size.height), "x", uint32_t(size.depth));
    return {};
  }
  const auto footprint = buffer_location.placed_footprint.Footprint;
  const UINT64 buffer_offset =
      buffer_resource.GetHeapOffset() + buffer_location.placed_footprint.Offset;
  const UINT row_pitch = footprint.RowPitch;
  if (GetPlaneCount(texture_resource) > 1 &&
      !IsDXGIFormatPlaneCompatible(texture_resource.GetResourceDesc().Format,
                                   footprint.Format, plane)) {
    WARN("D3D12CommandQueue: buffer texture copy format is not plane compatible resource_format=",
         uint32_t(texture_resource.GetResourceDesc().Format),
         " footprint_format=", uint32_t(footprint.Format),
         " plane=", uint32_t(plane));
    return {};
  }
  MTL_DXGI_FORMAT_DESC footprint_format_desc = {};
  const bool footprint_format_known = SUCCEEDED(
      MTLQueryDXGIFormat(device, footprint.Format, footprint_format_desc));
  const auto footprint_row_layout = ComputePlacedFootprintRowLayout(
      footprint, footprint_format_known, footprint_format_desc);
  if (!footprint_row_layout) {
    WARN("D3D12CommandQueue: buffer texture copy footprint does not fit a 32-bit image pitch row_pitch=",
         uint32_t(footprint.RowPitch), " height=", uint32_t(footprint.Height),
         " footprint_format=", uint32_t(footprint.Format));
    return {};
  }
  const UINT footprint_block_height = footprint_row_layout->block_height;
  const UINT footprint_row_count = footprint_row_layout->row_count;
  const UINT image_pitch = footprint_row_layout->image_pitch;
  const DXGI_FORMAT footprint_format = footprint.Format;
  const DXGI_FORMAT resource_format = texture_resource.GetResourceDesc().Format;
  const bool emulated_d24 =
      resource_format == DXGI_FORMAT_D24_UNORM_S8_UINT ||
      resource_format == DXGI_FORMAT_R24G8_TYPELESS;
  const uint32_t resource_width =
      uint32_t(texture_resource.GetResourceDesc().Width);
  const uint32_t resource_height = texture_resource.GetResourceDesc().Height;
  const uint32_t resource_depth =
      texture_resource.GetResourceDesc().DepthOrArraySize;
  const uint32_t texture_format = uint32_t(texture->pixelFormat());
  const uint32_t texture_type = uint32_t(texture->textureType());
  const uint32_t texture_width = texture->width();
  const uint32_t texture_height = texture->height();
  const uint32_t texture_depth = texture->depth();
  const uint32_t texture_array = texture->arrayLength();
  const uint32_t texture_mips = texture->miplevelCount();
  const uint32_t texture_samples = texture->sampleCount();

  if (D3D12DiagTextureCopyEnabled()) {
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12DiagShouldLog(log_count, true)) {
      INFO("D3D12 diagnostic: buffer texture copy record",
           " direction=", dst_is_buffer ? "texture_to_buffer" : "buffer_to_texture",
           " dst_type=", D3D12TextureCopyTypeName(record.dst.type),
           " src_type=", D3D12TextureCopyTypeName(record.src.type),
           " subresource=", uint32_t(subresource),
           " level=", uint32_t(level),
           " slice=", uint32_t(slice),
           " dst_xyz=", uint32_t(record.dst_x), ",", uint32_t(record.dst_y), ",", uint32_t(record.dst_z),
           " origin=", uint32_t(origin.x), ",", uint32_t(origin.y), ",", uint32_t(origin.z),
           " size=", uint32_t(size.width), "x", uint32_t(size.height), "x", uint32_t(size.depth),
           " buffer_heap_offset=", uint64_t(buffer_resource.GetHeapOffset()),
           " footprint_offset=", uint64_t(buffer_location.placed_footprint.Offset),
           " buffer_offset=", uint64_t(buffer_offset),
           " row_pitch=", uint32_t(row_pitch),
           " image_pitch=", uint32_t(image_pitch),
           " row_count=", uint32_t(footprint_row_count),
           " block_height=", uint32_t(footprint_block_height),
           " footprint_format=", uint32_t(footprint_format),
           " footprint_size=", uint32_t(footprint.Width), "x", uint32_t(footprint.Height), "x", uint32_t(footprint.Depth),
           " resource_format=", uint32_t(resource_format),
           " resource_size=", resource_width, "x", resource_height, "x", resource_depth,
           " texture_format=", texture_format,
           " texture_type=", texture_type,
           " texture_size=", texture_width, "x", texture_height, "x", texture_depth,
           " texture_array=", texture_array,
           " texture_mips=", texture_mips,
           " texture_samples=", texture_samples);
    }
  }

  BufferTextureCopyPlan plan = {};
  plan.dst_is_buffer = dst_is_buffer;
  plan.buffer_offset = buffer_offset;
  plan.row_pitch = row_pitch;
  plan.image_pitch = image_pitch;
  plan.size = size;
  plan.origin = origin;
  plan.slice = slice;
  plan.level = level;
  plan.plane = plane;
  plan.emulated_d24 = emulated_d24;
  plan.footprint_format = footprint_format;
  plan.resource_format = resource_format;
  plan.texture_format = texture_format;
  plan.footprint_row_count = footprint_row_count;
  plan.footprint_block_height = footprint_block_height;

  if (IsDepthStencilResourceFormat(texture_resource.GetResourceDesc().Format) &&
      DepthStencilPlanarFlags(texture->pixelFormat()) > 1) {
    if (size.depth != 1 || plane > 1) {
      WARN("D3D12CommandQueue: depth/stencil buffer texture copy has unsupported plane/depth plane=",
           uint32_t(plane), " depth=", uint32_t(size.depth));
      return {};
    }

    if (dst_is_buffer) {
      plan.read_view =
          CreateDepthStencilPlaneReadView(texture.ptr(), plane, level, slice);
      if (!plan.read_view)
        return {};
    }

    plan.kind = BufferTextureCopyKind::DepthStencilPlane;
    plan.buffer = std::move(buffer);
    plan.texture = std::move(texture);
    return plan;
  }

  plan.kind = BufferTextureCopyKind::Blit;
  plan.buffer = std::move(buffer);
  plan.texture = std::move(texture);
  return plan;
}

} // namespace dxmt::d3d12
