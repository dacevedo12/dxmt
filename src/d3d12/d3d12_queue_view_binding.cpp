#include "d3d12_queue_view_binding.hpp"

#include "d3d12_descriptor_diagnostics.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "d3d12_texture_swizzle.hpp"
#include "d3d12_texture_view.hpp"
#include "dxmt_format.hpp"
#include "dxmt_legacy_buffer_slice.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>

namespace dxmt::d3d12 {

namespace {

// Texture-buffer view diagnostics are noisy on broken content; cap them.
constexpr uint32_t kTextureBufferViewDiagLimit = 32;

bool
ShouldLogTextureBufferViewDiag() {
  static std::atomic<uint32_t> count = 0;
  return count.fetch_add(1, std::memory_order_relaxed) <
         kTextureBufferViewDiagLimit;
}

void
WarnTextureBufferViewUnavailable(const char *binding, DXGI_FORMAT format,
                                 UINT stride, UINT64 offset,
                                 UINT64 byte_size) {
  if (!ShouldLogTextureBufferViewDiag())
    return;
  WARN("D3D12CommandQueue: ", binding,
       " buffer view reflected as texture but Metal texture-buffer view is unavailable"
       " format=", uint32_t(format),
       " stride=", stride,
       " offset=", offset,
       " byteSize=", byte_size,
       "; leaving binding null");
}

void
WarnTextureBufferViewInvalidRange(const char *binding, DXGI_FORMAT format,
                                  UINT stride, UINT64 offset,
                                  UINT64 byte_size, UINT64 heap_offset,
                                  UINT64 backing_length,
                                  const char *reason) {
  if (!ShouldLogTextureBufferViewDiag())
    return;
  WARN("D3D12CommandQueue: ", binding,
       " buffer view reflected as texture but range is invalid"
       " format=", uint32_t(format),
       " stride=", stride,
       " offset=", offset,
       " byteSize=", byte_size,
       " heapOffset=", heap_offset,
       " backingLength=", backing_length,
       " reason=", reason,
       "; leaving binding null");
}

template <PipelineStage stage>
bool FillBindlessTextureBufferMirrorSlotForStage(
    ArgumentEncodingContext &enc, DescriptorHeapMirror &mirror, UINT slot,
    const Rc<Buffer> &buffer, const BufferViewBinding &binding,
    const BufferSlice &slice, int access_flags,
    dxmt::DescriptorSlotVersion expected_version) {
  auto [view, suballocation_offset] =
      enc.access<stage>(buffer, binding.key, access_flags);
  auto mirror_lock = mirror.AcquireLock();
  if (!mirror.FillTextureSlotPayload(
          slot, view.gpu_resource_id,
          (uint64_t(slice.elementCount) << 32) |
              uint64_t(slice.firstElement + suballocation_offset),
          expected_version))
    return false;
  DescriptorResidencyTarget target = {};
  auto texture_allocation = view.texture ? WMT::Resource{view.texture.handle}
                                         : WMT::Resource{};
  if (texture_allocation)
    target.mirror_allocation = texture_allocation;
  return ReplaceDescriptorMirrorResidencyTargetForEncode(
      enc, mirror, slot, expected_version, std::move(target));
}

} // namespace

DescriptorRecordType
ExpectedDescriptorTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
  switch (range_type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
    return DescriptorRecordType::ConstantBufferView;
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    return DescriptorRecordType::ShaderResourceView;
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
    return DescriptorRecordType::UnorderedAccessView;
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    return DescriptorRecordType::Sampler;
  default:
    return DescriptorRecordType::Empty;
  }
}

D3D12_DESCRIPTOR_HEAP_TYPE
DescriptorHeapTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
  return range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
             ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
             : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
}

BufferSlice
DefaultBufferSlice(Resource &resource, UINT64 offset,
                   UINT64 requested_size) {
  const auto width = resource.GetResourceDesc().Width;
  const auto remaining = width > offset ? width - offset : 0;
  const auto size = requested_size ? std::min<UINT64>(requested_size, remaining)
                                  : remaining;
  return {
      .byteOffset = UINT32(offset),
      .byteLength = UINT32(std::min<UINT64>(size, UINT32_MAX)),
      .firstElement = UINT32(offset),
      .elementCount = UINT32(std::min<UINT64>(size, UINT32_MAX)),
  };
}

bool
ValidateLegacyBufferSliceRange(const char *binding_type, UINT64 byte_offset,
                               UINT64 byte_length) {
  if (LegacyBufferSliceRepresentable(byte_offset, byte_length))
    return true;
  WARN("D3D12CommandQueue: ", binding_type,
       " buffer range exceeds the legacy shader ABI; binding null instead"
       " offset=", byte_offset, " length=", byte_length);
  return false;
}

BufferSlice
StructuredBufferSlice(Resource &resource, UINT64 offset,
                      UINT64 byte_size, UINT stride) {
  auto slice = DefaultBufferSlice(resource, offset, byte_size);
  if (stride) {
    slice.firstElement = UINT32(offset / stride);
    slice.elementCount = UINT32(slice.byteLength / stride);
  }
  return slice;
}

BufferSlice
TextureBufferSlice(Resource &resource, UINT64 offset, UINT64 byte_size,
                   UINT stride) {
  auto slice = StructuredBufferSlice(resource, offset, byte_size, stride);
  slice.firstElement = 0;
  return slice;
}

UINT64
ResolveBufferGpuAddress(D3D12_GPU_VIRTUAL_ADDRESS address,
                        Resource *&resource) {
  UINT64 offset = 0;
  resource = LookupBufferResourceByGpuVirtualAddress(address, &offset);
  return offset;
}

std::optional<BufferViewBinding>
CreateBufferView(WMT::Device device, Resource &resource, DXGI_FORMAT format,
                 UINT64 offset, UINT64 byte_size, WMTTextureUsage usage) {
  auto *buffer = resource.GetBuffer();
  if (!buffer)
    return std::nullopt;

  MTL_DXGI_FORMAT_DESC format_desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, format, format_desc)) ||
      format_desc.PixelFormat == WMTPixelFormatInvalid ||
      !format_desc.BytesPerTexel)
    return std::nullopt;

  const UINT64 heap_offset = resource.GetHeapOffset();
  const UINT64 backing_offset = heap_offset + offset;
  if (backing_offset < heap_offset) {
    WarnTextureBufferViewInvalidRange("resource", format,
                                      format_desc.BytesPerTexel, offset,
                                      byte_size, heap_offset, buffer->length(),
                                      "offset-overflow");
    return std::nullopt;
  }
  if (backing_offset > buffer->length()) {
    WarnTextureBufferViewInvalidRange("resource", format,
                                      format_desc.BytesPerTexel, offset,
                                      byte_size, heap_offset, buffer->length(),
                                      "offset-out-of-range");
    return std::nullopt;
  }

  const UINT64 max_byte_size = buffer->length() - backing_offset;
  const UINT64 clamped_byte_size = std::min<UINT64>(byte_size, max_byte_size);
  if (!clamped_byte_size) {
    WarnTextureBufferViewInvalidRange("resource", format,
                                      format_desc.BytesPerTexel, offset,
                                      byte_size, heap_offset, buffer->length(),
                                      "empty-range");
    return std::nullopt;
  }
  if ((backing_offset % format_desc.BytesPerTexel) ||
      (clamped_byte_size % format_desc.BytesPerTexel)) {
    WarnTextureBufferViewInvalidRange("resource", format,
                                      format_desc.BytesPerTexel, offset,
                                      byte_size, heap_offset, buffer->length(),
                                      "texel-unaligned");
    return std::nullopt;
  }
  const UINT64 texture_alignment = std::max<UINT64>(
      format_desc.BytesPerTexel,
      device.minimumLinearTextureAlignmentForPixelFormat(
          format_desc.PixelFormat));
  const UINT64 aligned_backing_offset =
      backing_offset - (backing_offset % texture_alignment);
  const UINT64 first_element_bias_bytes =
      backing_offset - aligned_backing_offset;
  const UINT64 view_byte_size =
      first_element_bias_bytes + clamped_byte_size;
  if (first_element_bias_bytes % format_desc.BytesPerTexel) {
    WarnTextureBufferViewInvalidRange("resource", format,
                                      format_desc.BytesPerTexel, offset,
                                      byte_size, heap_offset, buffer->length(),
                                      "alignment-bias-unaligned");
    return std::nullopt;
  }
  if (aligned_backing_offset > UINT32_MAX || view_byte_size > UINT32_MAX) {
    WarnTextureBufferViewInvalidRange("resource", format,
                                      format_desc.BytesPerTexel, offset,
                                      byte_size, heap_offset, buffer->length(),
                                      "range-too-large");
    return std::nullopt;
  }

  BufferViewDescriptor view = {};
  view.format = format_desc.PixelFormat;
  view.usage = usage;
  view.type = WMTTextureTypeTextureBuffer;
  view.byteOffset = UINT32(aligned_backing_offset);
  view.byteLength = UINT32(view_byte_size);

  BufferViewBinding binding = {};
  binding.key = buffer->createView(view);
  binding.firstElementBias =
      UINT(first_element_bias_bytes / format_desc.BytesPerTexel);
  return binding;
}

DXGI_FORMAT
UintBufferViewFormatForStride(UINT stride) {
  switch (stride) {
  case 4:
    return DXGI_FORMAT_R32_UINT;
  case 8:
    return DXGI_FORMAT_R32G32_UINT;
  case 16:
    return DXGI_FORMAT_R32G32B32A32_UINT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

std::optional<std::pair<BufferViewBinding, BufferSlice>>
CreateShaderResourceTextureBufferBinding(WMT::Device device, Resource &resource,
                                         const DescriptorRecord &descriptor,
                                         WMTTextureUsage usage) {
  UINT64 offset = 0;
  UINT64 byte_size = resource.GetResourceDesc().Width;

  if (descriptor.has_desc) {
    const auto &srv = descriptor.desc.srv;
    if (srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER) {
      WARN("D3D12CommandQueue: buffer SRV has unsupported view dimension ",
           uint32_t(srv.ViewDimension));
      return std::nullopt;
    }

    const UINT64 first_element = srv.Buffer.FirstElement;
    if (srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
      offset += first_element * sizeof(uint32_t);
      byte_size = UINT64(srv.Buffer.NumElements) * sizeof(uint32_t);
      auto view = CreateBufferView(device, resource, DXGI_FORMAT_R32_UINT,
                                   offset, byte_size, usage);
      if (!view) {
        WarnTextureBufferViewUnavailable("SRV", DXGI_FORMAT_R32_UINT,
                                         sizeof(uint32_t), offset, byte_size);
        return std::nullopt;
      }
      auto slice = TextureBufferSlice(resource, offset, byte_size,
                                      sizeof(uint32_t));
      slice.firstElement += view->firstElementBias;
      return std::make_pair(*view, slice);
    }

    if (srv.Format != DXGI_FORMAT_UNKNOWN) {
      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device, srv.Format, format))) {
        WARN("D3D12CommandQueue: typed buffer SRV uses unsupported format ",
             uint32_t(srv.Format));
        return std::nullopt;
      }
      offset += first_element * format.BytesPerTexel;
      byte_size = UINT64(srv.Buffer.NumElements) * format.BytesPerTexel;
      auto view = CreateBufferView(device, resource, srv.Format, offset,
                                   byte_size, usage);
      if (!view) {
        WarnTextureBufferViewUnavailable("SRV", srv.Format,
                                         format.BytesPerTexel, offset,
                                         byte_size);
        return std::nullopt;
      }
      auto slice = TextureBufferSlice(resource, offset, byte_size,
                                      format.BytesPerTexel);
      slice.firstElement += view->firstElementBias;
      return std::make_pair(*view, slice);
    }

    if (srv.Buffer.StructureByteStride) {
      offset += first_element * srv.Buffer.StructureByteStride;
      byte_size = UINT64(srv.Buffer.NumElements) *
                  srv.Buffer.StructureByteStride;
      const auto view_format =
          UintBufferViewFormatForStride(srv.Buffer.StructureByteStride);
      if (view_format == DXGI_FORMAT_UNKNOWN) {
        WarnTextureBufferViewUnavailable(
            "SRV", view_format, srv.Buffer.StructureByteStride, offset,
            byte_size);
        return std::nullopt;
      }
      auto view = CreateBufferView(device, resource, view_format, offset,
                                   byte_size, usage);
      if (!view) {
        WarnTextureBufferViewUnavailable(
            "SRV", view_format, srv.Buffer.StructureByteStride, offset,
            byte_size);
        return std::nullopt;
      }
      auto slice = TextureBufferSlice(resource, offset, byte_size,
                                      srv.Buffer.StructureByteStride);
      slice.firstElement += view->firstElementBias;
      return std::make_pair(*view, slice);
    }

    offset += first_element;
    byte_size = srv.Buffer.NumElements;
  }

  auto view = CreateBufferView(device, resource, DXGI_FORMAT_R32_UINT, offset,
                               byte_size, usage);
  if (!view) {
    WarnTextureBufferViewUnavailable("SRV", DXGI_FORMAT_R32_UINT,
                                     sizeof(uint32_t), offset, byte_size);
    return std::nullopt;
  }
  auto slice = TextureBufferSlice(resource, offset, byte_size,
                                  sizeof(uint32_t));
  slice.firstElement += view->firstElementBias;
  return std::make_pair(*view, slice);
}

std::optional<std::pair<BufferViewBinding, BufferSlice>>
CreateUnorderedAccessTextureBufferBinding(WMT::Device device, Resource &resource,
                                          const DescriptorRecord &descriptor,
                                          WMTTextureUsage usage) {
  UINT64 offset = 0;
  UINT64 byte_size = resource.GetResourceDesc().Width;

  if (descriptor.has_desc) {
    const auto &uav = descriptor.desc.uav;
    if (uav.ViewDimension != D3D12_UAV_DIMENSION_BUFFER) {
      WARN("D3D12CommandQueue: buffer UAV has unsupported view dimension ",
           uint32_t(uav.ViewDimension));
      return std::nullopt;
    }

    const UINT64 first_element = uav.Buffer.FirstElement;
    if (uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
      offset += first_element * sizeof(uint32_t);
      byte_size = UINT64(uav.Buffer.NumElements) * sizeof(uint32_t);
      auto view = CreateBufferView(device, resource, DXGI_FORMAT_R32_UINT,
                                   offset, byte_size, usage);
      if (!view) {
        WarnTextureBufferViewUnavailable("UAV", DXGI_FORMAT_R32_UINT,
                                         sizeof(uint32_t), offset, byte_size);
        return std::nullopt;
      }
      auto slice = TextureBufferSlice(resource, offset, byte_size,
                                      sizeof(uint32_t));
      slice.firstElement += view->firstElementBias;
      return std::make_pair(*view, slice);
    }

    if (uav.Format != DXGI_FORMAT_UNKNOWN) {
      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device, uav.Format, format))) {
        WARN("D3D12CommandQueue: typed buffer UAV uses unsupported format ",
             uint32_t(uav.Format));
        return std::nullopt;
      }
      offset += first_element * format.BytesPerTexel;
      byte_size = UINT64(uav.Buffer.NumElements) * format.BytesPerTexel;
      auto view = CreateBufferView(device, resource, uav.Format, offset,
                                   byte_size, usage);
      if (!view) {
        WarnTextureBufferViewUnavailable("UAV", uav.Format,
                                         format.BytesPerTexel, offset,
                                         byte_size);
        return std::nullopt;
      }
      auto slice = TextureBufferSlice(resource, offset, byte_size,
                                      format.BytesPerTexel);
      slice.firstElement += view->firstElementBias;
      return std::make_pair(*view, slice);
    }

    if (uav.Buffer.StructureByteStride) {
      offset += first_element * uav.Buffer.StructureByteStride;
      byte_size = UINT64(uav.Buffer.NumElements) *
                  uav.Buffer.StructureByteStride;
      const auto view_format =
          UintBufferViewFormatForStride(uav.Buffer.StructureByteStride);
      if (view_format == DXGI_FORMAT_UNKNOWN) {
        WarnTextureBufferViewUnavailable(
            "UAV", view_format, uav.Buffer.StructureByteStride, offset,
            byte_size);
        return std::nullopt;
      }
      auto view = CreateBufferView(device, resource, view_format, offset,
                                   byte_size, usage);
      if (!view) {
        WarnTextureBufferViewUnavailable(
            "UAV", view_format, uav.Buffer.StructureByteStride, offset,
            byte_size);
        return std::nullopt;
      }
      auto slice = TextureBufferSlice(resource, offset, byte_size,
                                      uav.Buffer.StructureByteStride);
      slice.firstElement += view->firstElementBias;
      return std::make_pair(*view, slice);
    }

    offset += first_element;
    byte_size = uav.Buffer.NumElements;
  }

  auto view = CreateBufferView(device, resource, DXGI_FORMAT_R32_UINT, offset,
                               byte_size, usage);
  if (!view) {
    WarnTextureBufferViewUnavailable("UAV", DXGI_FORMAT_R32_UINT,
                                     sizeof(uint32_t), offset, byte_size);
    return std::nullopt;
  }
  auto slice = TextureBufferSlice(resource, offset, byte_size,
                                  sizeof(uint32_t));
  slice.firstElement += view->firstElementBias;
  return std::make_pair(*view, slice);
}

bool ReplaceDescriptorMirrorResidencyTargetForEncode(
    ArgumentEncodingContext &enc, DescriptorHeapMirror &mirror, UINT slot,
    dxmt::DescriptorSlotVersion expected_version,
    DescriptorResidencyTarget target) {
  // The caller holds mirror.AcquireLock() from payload publication through
  // this lifetime publication, so readers can never observe a mixed pair.
  // Residency belongs to allocation owners; the slot only retains the payload
  // until encoded GPU work finishes.
  DescriptorResidencyTransition transition = {};
  if (!mirror.ReplaceMirrorResidencyTargetIfCurrent(
          slot, expected_version, std::move(target), &transition)) {
    return false;
  }

  if (transition.previous.sampler)
    enc.queue().RetainGpuOwner(
        enc.currentSeqId(), std::move(transition.previous.sampler));
  return true;
}

bool
FillBindlessTextureBufferMirrorSlot(
    ArgumentEncodingContext &enc, DescriptorHeapMirror &mirror, UINT slot,
    PipelineStage stage, const Rc<Buffer> &buffer,
    const BufferViewBinding &binding, const BufferSlice &slice,
    int access_flags, dxmt::DescriptorSlotVersion expected_version) {
  switch (stage) {
  case PipelineStage::Compute:
    return FillBindlessTextureBufferMirrorSlotForStage<PipelineStage::Compute>(
        enc, mirror, slot, buffer, binding, slice, access_flags,
        expected_version);
  case PipelineStage::Pixel:
    return FillBindlessTextureBufferMirrorSlotForStage<PipelineStage::Pixel>(
        enc, mirror, slot, buffer, binding, slice, access_flags,
        expected_version);
  case PipelineStage::Geometry:
    return FillBindlessTextureBufferMirrorSlotForStage<PipelineStage::Geometry>(
        enc, mirror, slot, buffer, binding, slice, access_flags,
        expected_version);
  case PipelineStage::Hull:
    return FillBindlessTextureBufferMirrorSlotForStage<PipelineStage::Hull>(
        enc, mirror, slot, buffer, binding, slice, access_flags,
        expected_version);
  case PipelineStage::Domain:
    return FillBindlessTextureBufferMirrorSlotForStage<PipelineStage::Domain>(
        enc, mirror, slot, buffer, binding, slice, access_flags,
        expected_version);
  case PipelineStage::Vertex:
  default:
    return FillBindlessTextureBufferMirrorSlotForStage<PipelineStage::Vertex>(
        enc, mirror, slot, buffer, binding, slice, access_flags,
        expected_version);
  }
}

bool
IsDepthStencilResourceFormat(DXGI_FORMAT format) {
  return GetDXGIFormatTraits(format).flags & DXGI_FORMAT_TRAIT_DEPTH_STENCIL;
}

TextureViewKey
CreateDepthStencilPlaneReadView(dxmt::Texture *texture, UINT plane, UINT level,
                                UINT slice) {
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  switch (texture->textureType()) {
  case WMTTextureType2D:
  case WMTTextureType2DArray:
  case WMTTextureTypeCube:
  case WMTTextureTypeCubeArray:
    view.type = WMTTextureType2D;
    break;
  default:
    return {};
  }
  view.format = plane ? WMTPixelFormatX32G8X32 : texture->pixelFormat();
  view.firstMiplevel = level;
  view.miplevelCount = 1;
  view.firstArraySlice = slice;
  view.arraySize = 1;
  view.intendedUsage = WMTTextureUsageShaderRead;
  return texture->createView(view);
}

bool
ValidateTextureViewRange(const char *context, TextureViewDescriptor &view,
                         const Resource &resource) {
  const auto *texture = resource.GetTexture();
  if (!texture)
    return false;

  if (view.firstMiplevel >= texture->miplevelCount() ||
      view.miplevelCount == 0 ||
      view.miplevelCount > texture->miplevelCount() - view.firstMiplevel) {
    WARN("D3D12CommandQueue: ", context,
         " mip range exceeds texture levels first=", view.firstMiplevel,
         " count=", view.miplevelCount,
         " levels=", texture->miplevelCount());
    return false;
  }

  if (resource.GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    view.firstArraySlice = 0;
    view.arraySize = 1;
    return true;
  }

  if (view.firstArraySlice >= texture->arrayLength() ||
      view.arraySize == 0 ||
      view.arraySize > texture->arrayLength() - view.firstArraySlice) {
    WARN("D3D12CommandQueue: ", context,
         " array range exceeds texture array first=", view.firstArraySlice,
         " count=", view.arraySize, " array_length=", texture->arrayLength());
    return false;
  }
  return true;
}

TextureViewBinding
CreateShaderResourceTextureView(WMT::Device device, Resource &resource,
                                const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = texture->miplevelCount();
  view.firstArraySlice = 0;
  view.arraySize = texture->arrayLength();
  view.intendedUsage = WMTTextureUsageShaderRead;
  view.swizzle =
      ShaderResourceViewSwizzle(view.format,
                                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);

  if (!descriptor.has_desc) {
    auto key = texture->createView(view);
    D3D12DiagLogTextureView("SRV", resource, descriptor, view, key);
    return {Rc<Texture>(texture), key};
  }

  const auto &srv = descriptor.desc.srv;
  UINT plane = 0;
  switch (srv.ViewDimension) {
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    plane = srv.Texture2D.PlaneSlice;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    plane = srv.Texture2DArray.PlaneSlice;
    break;
  default:
    break;
  }
  if (GetPlaneCount(resource) > 1 && srv.Format != DXGI_FORMAT_UNKNOWN &&
      !IsDXGIFormatPlaneCompatible(resource.GetResourceDesc().Format,
                                   srv.Format, plane)) {
    WARN("D3D12CommandQueue: unsupported SRV plane format resource_format=",
         uint32_t(resource.GetResourceDesc().Format),
         " view_format=", uint32_t(srv.Format),
         " plane=", uint32_t(plane));
    return {};
  }

  auto *plane_texture = resource.GetTexture(plane);
  if (!plane_texture)
    return {};
  auto plane_texture_ref = Rc<Texture>(plane_texture);
  view.format = ResolveTextureViewFormat(device, resource, srv.Format, plane,
                                         "D3D12CommandQueue");
  if (view.format == WMTPixelFormatInvalid)
    return {};
  view.swizzle =
      ShaderResourceViewSwizzle(view.format, srv.Shader4ComponentMapping);

  switch (srv.ViewDimension) {
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    view.type = WMTTextureType2D;
    view.firstMiplevel = srv.Texture1D.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture1D.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.arraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    view.type = WMTTextureType2DArray;
    view.firstMiplevel = srv.Texture1DArray.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture1DArray.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.firstArraySlice = srv.Texture1DArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(srv.Texture1DArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    view.type = WMTTextureType2D;
    view.firstMiplevel = srv.Texture2D.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture2D.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.firstArraySlice = 0;
    view.arraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    view.type = WMTTextureType2DArray;
    view.firstMiplevel = srv.Texture2DArray.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture2DArray.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.firstArraySlice = srv.Texture2DArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(srv.Texture2DArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DMS:
    view.type = WMTTextureType2DMultisample;
    view.miplevelCount = 1;
    view.arraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
    view.type = WMTTextureType2DMultisampleArray;
    view.miplevelCount = 1;
    view.firstArraySlice = srv.Texture2DMSArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(srv.Texture2DMSArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    view.type = WMTTextureType3D;
    view.firstMiplevel = srv.Texture3D.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture3D.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.arraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    view.type = WMTTextureTypeCube;
    view.firstMiplevel = srv.TextureCube.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.TextureCube.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.arraySize = std::min<UINT>(6, texture->arrayLength());
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    view.type = WMTTextureTypeCubeArray;
    view.firstMiplevel = srv.TextureCubeArray.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.TextureCubeArray.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.firstArraySlice = srv.TextureCubeArray.First2DArrayFace;
    view.arraySize = NormalizeViewCount(srv.TextureCubeArray.NumCubes * 6,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  default:
    WARN("D3D12CommandQueue: unsupported SRV texture dimension ",
         uint32_t(srv.ViewDimension));
    return {};
  }

  if (!ValidateTextureViewRange("SRV texture view", view, resource))
    return {};
  auto key = plane_texture_ref->createView(view);
  D3D12DiagLogTextureView("SRV", resource, descriptor, view, key);
  return {std::move(plane_texture_ref), key};
}

TextureViewBinding
CreateUnorderedAccessTextureView(WMT::Device device, Resource &resource,
                                 const DescriptorRecord &descriptor,
                                 bool allow_3d_slice_subrange) {
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
  view.intendedUsage =
      WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;

  if (!descriptor.has_desc) {
    auto key = texture->createView(view);
    D3D12DiagLogTextureView("UAV", resource, descriptor, view, key);
    return {Rc<Texture>(texture), key};
  }

  const auto &uav = descriptor.desc.uav;
  UINT plane = 0;
  switch (uav.ViewDimension) {
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    plane = uav.Texture2D.PlaneSlice;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    plane = uav.Texture2DArray.PlaneSlice;
    break;
  default:
    break;
  }
  if (GetPlaneCount(resource) > 1 && uav.Format != DXGI_FORMAT_UNKNOWN &&
      !IsDXGIFormatPlaneCompatible(resource.GetResourceDesc().Format,
                                   uav.Format, plane)) {
    WARN("D3D12CommandQueue: unsupported UAV plane format resource_format=",
         uint32_t(resource.GetResourceDesc().Format),
         " view_format=", uint32_t(uav.Format),
         " plane=", uint32_t(plane));
    return {};
  }

  auto *plane_texture = resource.GetTexture(plane);
  if (!plane_texture)
    return {};
  auto plane_texture_ref = Rc<Texture>(plane_texture);
  view.format = ResolveTextureViewFormat(device, resource, uav.Format, plane,
                                         "D3D12CommandQueue");
  if (view.format == WMTPixelFormatInvalid)
    return {};

  switch (uav.ViewDimension) {
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    view.type = WMTTextureType2D;
    view.firstMiplevel = uav.Texture1D.MipSlice;
    view.arraySize = 1;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    view.type = WMTTextureType2DArray;
    view.firstMiplevel = uav.Texture1DArray.MipSlice;
    view.firstArraySlice = uav.Texture1DArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(uav.Texture1DArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    view.type = WMTTextureType2D;
    view.firstMiplevel = uav.Texture2D.MipSlice;
    view.arraySize = 1;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    view.type = WMTTextureType2DArray;
    view.firstMiplevel = uav.Texture2DArray.MipSlice;
    view.firstArraySlice = uav.Texture2DArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(uav.Texture2DArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    view.type = WMTTextureType3D;
    view.firstMiplevel = uav.Texture3D.MipSlice;
    view.arraySize = 1;
    if (view.firstMiplevel >= texture->miplevelCount()) {
      WARN("D3D12CommandQueue: invalid 3D texture UAV mip slice ",
           view.firstMiplevel);
      return {};
    }
    {
      const UINT mip_depth = GetMipDepth(resource, view.firstMiplevel);
      const UINT first_w = uav.Texture3D.FirstWSlice;
      const UINT w_size = uav.Texture3D.WSize == UINT_MAX
                              ? (first_w < mip_depth ? mip_depth - first_w : 0)
                              : uav.Texture3D.WSize;
      if (first_w >= mip_depth || w_size == 0 || w_size > mip_depth - first_w) {
        WARN("D3D12CommandQueue: invalid 3D texture UAV W slice range first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return {};
      }
      if (!allow_3d_slice_subrange &&
          (first_w != 0 || w_size != mip_depth)) {
        // TODO(d3d12): lower 3D texture UAV depth-slice subviews once the
        // DXMT texture view layer can represent a W-slice range for 3D images.
        WARN("D3D12CommandQueue: unsupported 3D texture UAV W slice subrange first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return {};
      }
    }
    break;
  default:
    WARN("D3D12CommandQueue: unsupported UAV texture dimension ",
         uint32_t(uav.ViewDimension));
    return {};
  }

  if (!ValidateTextureViewRange("UAV texture view", view, resource))
    return {};
  auto key = plane_texture_ref->createView(view);
  D3D12DiagLogTextureView("UAV", resource, descriptor, view, key);
  return {std::move(plane_texture_ref), key};
}

} // namespace dxmt::d3d12
