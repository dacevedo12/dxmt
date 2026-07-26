#include "d3d12_replay_clear_encode.hpp"

#include "d3d12_descriptor_diagnostics.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "d3d12_texture_view.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {

void
ReplayClearRenderTarget(WMT::Device device, CommandChunk *chunk,
                        const ClearRenderTargetRecord &record) {
  auto *resource = GetResource(record.descriptor.resource.ptr());
  if (!resource)
    return;
  if (resource->IsReservedTexture() &&
      !resource->EnsureTextureAllocation("ClearRenderTarget"))
    return;
  if (!resource->GetTexture() || !resource->GetTextureAllocation())
    return;

  Rc<Texture> texture = resource->GetTexture();
  auto view = CreateRenderTargetView(device, *resource, record.descriptor);
  TrackPresentSourceRenderTargetView(*resource, view);
  const UINT array_length =
      GetRenderTargetArrayLength(*resource, record.descriptor);
  const UINT depth_plane = GetRenderTargetDepthPlane(record.descriptor);
  WMTClearColor color = {record.color[0], record.color[1], record.color[2],
                         record.color[3]};
  if (record.rects.empty()) {
    chunk->emitcc([texture = std::move(texture), view, array_length,
                   depth_plane, color](ArgumentEncodingContext &enc) mutable {
      enc.clearColor(std::move(texture), view, array_length, color, false,
                     depth_plane);
    });
    return;
  }

  chunk->emitcc([texture = std::move(texture), view, array_length,
                 depth_plane, color = record.color,
                 rects = record.rects](ArgumentEncodingContext &enc) mutable {
    enc.clear_rt_cmd.begin(std::move(texture), view, 0, 0, depth_plane,
                           array_length);
    for (const auto &rect : rects) {
      if (rect.right <= rect.left || rect.bottom <= rect.top)
        continue;
      enc.clear_rt_cmd.clear(rect.left, rect.top, rect.right - rect.left,
                             rect.bottom - rect.top, color);
    }
    enc.clear_rt_cmd.end();
  });
}

void
ReplayClearDepthStencil(WMT::Device device, CommandChunk *chunk,
                        const ClearDepthStencilRecord &record) {
  auto *resource = GetResource(record.descriptor.resource.ptr());
  if (!resource)
    return;
  if (resource->IsReservedTexture() &&
      !resource->EnsureTextureAllocation("ClearDepthStencil"))
    return;
  if (!resource->GetTexture() || !resource->GetTextureAllocation())
    return;

  Rc<Texture> texture = resource->GetTexture();
  auto view = CreateDepthStencilView(device, *resource, record.descriptor);
  if (!view)
    D3D12DiagLogDSVReplayDescriptor("ReplayClearDepthStencil empty view",
                                    *resource, record.descriptor,
                                    TextureViewDescriptor{}, view);
  const UINT array_length = GetDepthStencilArrayLength(*resource, record.descriptor);
  unsigned flags = 0;
  if (record.flags & D3D12_CLEAR_FLAG_DEPTH)
    flags |= 1;
  if (record.flags & D3D12_CLEAR_FLAG_STENCIL)
    flags |= 2;
  if (record.rects.empty()) {
    chunk->emitcc([texture = std::move(texture), view, array_length, flags,
                   depth = record.depth,
                   stencil = record.stencil](ArgumentEncodingContext &enc) mutable {
      enc.clearDepthStencil(std::move(texture), view, array_length, flags,
                            depth, stencil);
    });
    return;
  }

  chunk->emitcc(
      [texture = std::move(texture), view, flags, depth = record.depth,
       stencil = record.stencil,
       rects = record.rects](ArgumentEncodingContext &enc) mutable {
        enc.clear_rt_cmd.begin(std::move(texture), view, flags, stencil);
        const std::array<float, 4> value = {depth, 0.0f, 0.0f, 0.0f};
        for (const auto &rect : rects) {
          if (rect.right <= rect.left || rect.bottom <= rect.top)
            continue;
          enc.clear_rt_cmd.clear(rect.left, rect.top, rect.right - rect.left,
                                 rect.bottom - rect.top, value);
        }
        enc.clear_rt_cmd.end();
      });
}

void
ReplayClearUnorderedAccess(WMT::Device device, CommandChunk *chunk,
                           const ClearUnorderedAccessRecord &record) {
  auto *resource = GetResource(record.resource.ptr());
  if (!resource) {
    WARN("D3D12CommandQueue: ClearUnorderedAccessView skipped for foreign resource");
    return;
  }

  if (resource->GetBuffer()) {
    UINT64 offset = 0;
    UINT64 byte_size = resource->GetResourceDesc().Width;
    uint64_t view_id = 0;
    UINT view_first_element_bias = 0;
    UINT view_element_count = 0;
    bool has_buffer_view = false;
    bool raw_buffer = false;

    if (record.descriptor.has_desc &&
        record.descriptor.desc.uav.ViewDimension ==
            D3D12_UAV_DIMENSION_BUFFER) {
      const auto &uav = record.descriptor.desc.uav;
      const UINT64 first_element = uav.Buffer.FirstElement;
      if (uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
        raw_buffer = true;
        offset += first_element * sizeof(uint32_t);
        byte_size = UINT64(uav.Buffer.NumElements) * sizeof(uint32_t);
      } else if (uav.Format != DXGI_FORMAT_UNKNOWN) {
        MTL_DXGI_FORMAT_DESC format = {};
        if (SUCCEEDED(MTLQueryDXGIFormat(device, uav.Format, format))) {
          offset += first_element * format.BytesPerTexel;
          byte_size = UINT64(uav.Buffer.NumElements) *
                      format.BytesPerTexel;
          if (format.BytesPerTexel == sizeof(uint32_t)) {
            raw_buffer = true;
          } else {
            auto view = CreateBufferView(
                device, *resource, uav.Format, offset,
                byte_size, WMTTextureUsageShaderRead |
                               WMTTextureUsageShaderWrite);
            if (view) {
              view_id = view->key;
              view_first_element_bias = view->firstElementBias;
              view_element_count = UINT(std::min<UINT64>(
                  byte_size / format.BytesPerTexel, UINT_MAX));
              has_buffer_view = true;
            }
          }
        }
      } else if (uav.Buffer.StructureByteStride) {
        offset += first_element * uav.Buffer.StructureByteStride;
        byte_size = UINT64(uav.Buffer.NumElements) *
                    uav.Buffer.StructureByteStride;
        raw_buffer = !(uav.Buffer.StructureByteStride % sizeof(uint32_t));
      }
    } else {
      raw_buffer = true;
    }

    if (!has_buffer_view && !raw_buffer) {
      // TODO(d3d12): support formatted/structured UAV buffer clears when a
      // Metal texture-buffer view cannot be created for the requested UAV.
      WARN("D3D12CommandQueue: ClearUnorderedAccessView buffer view is unsupported");
      return;
    }

    Rc<Buffer> buffer = resource->GetBuffer();
    const UINT element_count = has_buffer_view
                                   ? view_element_count
                                   : UINT(std::min<UINT64>(
                                         byte_size / sizeof(uint32_t),
                                         UINT_MAX));
    if (!element_count)
      return;
    chunk->emitcc([buffer = std::move(buffer), view_id,
                   view_first_element_bias, raw_buffer,
                   integer = record.integer,
                   uint_values = record.uint_values,
                   float_values = record.float_values,
                   byte_offset = offset,
                   byte_size, element_count](ArgumentEncodingContext &enc) mutable {
      if (raw_buffer) {
        enc.startComputePass(0);
        auto [allocation, suballocation_offset] =
            enc.access(buffer, byte_offset, byte_size, ResourceAccess::Write);
        if (integer)
          enc.emulated_cmd.ClearBufferUint(allocation->buffer(),
                                           suballocation_offset + byte_offset,
                                           element_count, uint_values);
        else
          enc.emulated_cmd.ClearBufferFloat(allocation->buffer(),
                                             suballocation_offset + byte_offset,
                                             element_count, float_values);
        enc.endPass();
      } else {
        if (integer)
          enc.clear_res_cmd.begin(uint_values, Rc<Buffer>(buffer), view_id);
        else
          enc.clear_res_cmd.begin(float_values, Rc<Buffer>(buffer), view_id);
        enc.clear_res_cmd.clear(view_first_element_bias, 0, element_count, 1);
      }
      enc.clear_res_cmd.end();
    });
    return;
  }

  if (resource->GetTexture()) {
    if (resource->IsReservedTexture() &&
        !resource->EnsureTextureAllocation("ClearUnorderedAccessTexture"))
      return;
    auto view = CreateUnorderedAccessTextureView(device, *resource,
                                                 record.descriptor, true);
    if (!view)
      return;
    auto *texture = view.texture.ptr();
    const auto key = view.view;
    const auto type = texture->textureType(key);
    if (type != WMTTextureType2D && type != WMTTextureType2DArray &&
        type != WMTTextureType3D) {
      WARN("D3D12CommandQueue: ClearUnorderedAccessView texture type is unsupported");
      return;
    }
    std::vector<D3D12_RECT> rects = record.rects;
    if (rects.empty()) {
      rects.push_back(D3D12_RECT{0, 0, static_cast<LONG>(texture->width(key)),
                                 static_cast<LONG>(texture->height(key))});
    }
    UINT first_depth_slice = 0;
    UINT depth_slice_count = 1;
    if (type == WMTTextureType3D) {
      depth_slice_count =
          std::max(texture->depth() >> key.mip_start, 1u);
      if (record.descriptor.has_desc &&
          record.descriptor.desc.uav.ViewDimension ==
              D3D12_UAV_DIMENSION_TEXTURE3D) {
        const auto &texture3d = record.descriptor.desc.uav.Texture3D;
        first_depth_slice = texture3d.FirstWSlice;
        depth_slice_count = NormalizeViewCount(
            texture3d.WSize, first_depth_slice, depth_slice_count);
      }
    }
    Rc<Texture> rc_texture = std::move(view.texture);
    chunk->emitcc([texture = std::move(rc_texture), view,
                   integer = record.integer,
                   uint_values = record.uint_values,
                   float_values = record.float_values,
                   rects = std::move(rects), type, first_depth_slice,
                   depth_slice_count](ArgumentEncodingContext &enc) mutable {
      if (integer)
        enc.clear_res_cmd.begin(uint_values, Rc<Texture>(texture), view.view);
      else
        enc.clear_res_cmd.begin(float_values, Rc<Texture>(texture), view.view);
      for (const auto &rect : rects) {
        const auto left = uint32_t(std::max<LONG>(0, rect.left));
        const auto top = uint32_t(std::max<LONG>(0, rect.top));
        const auto width =
            uint32_t(std::max<LONG>(0, rect.right - rect.left));
        const auto height =
            uint32_t(std::max<LONG>(0, rect.bottom - rect.top));
        if (width && height) {
          if (type == WMTTextureType3D)
            enc.clear_res_cmd.clear3D(left, top, first_depth_slice, width,
                                      height, depth_slice_count);
          else
            enc.clear_res_cmd.clear(left, top, width, height);
        }
      }
      enc.clear_res_cmd.end();
    });
    return;
  }

  WARN("D3D12CommandQueue: ClearUnorderedAccessView resource has no backing allocation");
}

void
ReplayDiscardResource(CommandChunk *chunk,
                      const DiscardResourceRecord &record) {
  auto *resource = GetResource(record.resource.ptr());
  if (!resource) {
    WARN("D3D12CommandQueue: DiscardResource skipped for foreign resource");
    return;
  }

  (void)chunk;
}

} // namespace dxmt::d3d12
