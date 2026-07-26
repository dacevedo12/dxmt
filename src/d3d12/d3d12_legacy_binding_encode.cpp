#include "d3d12_legacy_binding_encode.hpp"

#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_sampler.hpp"
#include "d3d12_shader_binding.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"

#include <optional>
#include <utility>

namespace dxmt::d3d12 {

bool
ResolveDescriptorBufferOffset(ID3D12Resource *d3d_resource, UINT64 &offset) {
  auto *resource = GetResource(d3d_resource);
  if (!resource || !resource->GetBuffer())
    return false;
  const auto address = resource->GetGpuVirtualAddress();
  if (!address)
    return false;
  Resource *resolved = nullptr;
  offset = ResolveBufferGpuAddress(address, resolved);
  return resolved == resource;
}

bool
MakeConstantBufferBindingFromDescriptor(const DescriptorRecord &descriptor,
                                        ConstantBufferBinding &binding) {
  if (descriptor.type != DescriptorRecordType::ConstantBufferView)
    return false;
  binding = {};
  // CreateConstantBufferView(nullptr, ...) is an explicitly populated null
  // descriptor. Preserve it as a valid empty snapshot so compiled replay
  // does not fall back to live encoder state left by an earlier draw.
  if (!descriptor.has_desc)
    return true;
  if (descriptor.desc.cbv.BufferLocation &
      (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
    return false;
  if (descriptor.desc.cbv.SizeInBytes &
      (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
    return false;

  Resource *resource = nullptr;
  const auto offset =
      ResolveBufferGpuAddress(descriptor.desc.cbv.BufferLocation, resource);
  const bool null_cbv =
      !descriptor.desc.cbv.BufferLocation && !descriptor.desc.cbv.SizeInBytes;
  if (!null_cbv && (!resource || !resource->GetBuffer()))
    return false;

  if (!null_cbv) {
    binding.buffer = Rc<Buffer>(resource->GetBuffer());
    binding.offset = resource->GetHeapOffset() + offset;
  }
  return true;
}

bool
MakeShaderResourceBindingFromDescriptor(
    WMT::Device device, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT *argument, ResourceViewBinding &binding) {
  if (descriptor.type != DescriptorRecordType::ShaderResourceView)
    return false;
  auto *resource = GetResource(descriptor.resource.ptr());
  if (!resource)
    return false;

  binding = {};
  if (resource->GetBuffer()) {
    UINT64 offset = 0;
    if (!ResolveDescriptorBufferOffset(descriptor.resource.ptr(), offset))
      offset = 0;
    UINT64 byte_size = resource->GetResourceDesc().Width;
    const bool needs_texture_buffer_view =
        argument && (argument->Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE);
    BufferSlice slice = DefaultBufferSlice(*resource);
    uint64_t view_id = 0;
    if (descriptor.has_desc) {
      const auto &srv = descriptor.desc.srv;
      if (srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
        return false;
      const UINT64 first_element = srv.Buffer.FirstElement;
      if (srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
        offset += first_element * sizeof(uint32_t);
        byte_size = UINT64(srv.Buffer.NumElements) * sizeof(uint32_t);
        if (needs_texture_buffer_view) {
          auto view = CreateBufferView(device, *resource, DXGI_FORMAT_R32_UINT,
                                       offset, byte_size,
                                       WMTTextureUsageShaderRead);
          if (!view)
            return false;
          view_id = view->key;
          slice = TextureBufferSlice(*resource, offset, byte_size,
                                     sizeof(uint32_t));
          slice.firstElement += view->firstElementBias;
        } else {
          slice = StructuredBufferSlice(*resource, offset, byte_size,
                                        sizeof(uint32_t));
        }
      } else if (srv.Format != DXGI_FORMAT_UNKNOWN) {
        MTL_DXGI_FORMAT_DESC format = {};
        if (FAILED(MTLQueryDXGIFormat(device, srv.Format, format)))
          return false;
        offset += first_element * format.BytesPerTexel;
        byte_size = UINT64(srv.Buffer.NumElements) * format.BytesPerTexel;
        if (needs_texture_buffer_view) {
          auto view = CreateBufferView(device, *resource, srv.Format, offset,
                                       byte_size, WMTTextureUsageShaderRead);
          if (!view)
            return false;
          view_id = view->key;
          slice = TextureBufferSlice(*resource, offset, byte_size,
                                     format.BytesPerTexel);
          slice.firstElement += view->firstElementBias;
        } else {
          slice = StructuredBufferSlice(*resource, offset, byte_size,
                                        format.BytesPerTexel);
        }
      } else if (srv.Buffer.StructureByteStride) {
        offset += first_element * srv.Buffer.StructureByteStride;
        byte_size =
            UINT64(srv.Buffer.NumElements) * srv.Buffer.StructureByteStride;
        if (needs_texture_buffer_view) {
          const auto view_format =
              UintBufferViewFormatForStride(srv.Buffer.StructureByteStride);
          if (view_format == DXGI_FORMAT_UNKNOWN)
            return false;
          auto view = CreateBufferView(device, *resource, view_format, offset,
                                       byte_size, WMTTextureUsageShaderRead);
          if (!view)
            return false;
          view_id = view->key;
          slice = TextureBufferSlice(
              *resource, offset, byte_size, srv.Buffer.StructureByteStride);
          slice.firstElement += view->firstElementBias;
        } else {
          slice = StructuredBufferSlice(*resource, offset, byte_size,
                                        srv.Buffer.StructureByteStride);
        }
      } else {
        offset += first_element;
        byte_size = srv.Buffer.NumElements;
        if (needs_texture_buffer_view) {
          auto view = CreateBufferView(device, *resource, DXGI_FORMAT_R32_UINT,
                                       offset, byte_size,
                                       WMTTextureUsageShaderRead);
          if (!view)
            return false;
          view_id = view->key;
          slice = TextureBufferSlice(*resource, offset, byte_size,
                                     sizeof(uint32_t));
          slice.firstElement += view->firstElementBias;
        } else {
          slice = DefaultBufferSlice(*resource, offset, byte_size);
        }
      }
    } else if (needs_texture_buffer_view) {
      auto view = CreateBufferView(device, *resource, DXGI_FORMAT_R32_UINT,
                                   offset, byte_size,
                                   WMTTextureUsageShaderRead);
      if (!view)
        return false;
      view_id = view->key;
      slice = TextureBufferSlice(*resource, offset, byte_size,
                                 sizeof(uint32_t));
      slice.firstElement += view->firstElementBias;
    }
    if (!ValidateLegacyBufferSliceRange("SRV", offset, byte_size))
      return false;
    binding.viewId = view_id;
    binding.buffer = Rc<Buffer>(resource->GetBuffer());
    binding.slice = slice;
    return true;
  }

  if (resource->GetTexture()) {
    if (descriptor.has_desc &&
        descriptor.desc.srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER)
      return false;
    if (resource->IsReservedTexture() &&
        !resource->EnsureTextureAllocation("MakeShaderResourceBinding"))
      return false;
    const auto view =
        CreateShaderResourceTextureView(device, *resource, descriptor);
    if (!view || !view.texture.ptr())
      return false;
    binding.texture = std::move(view.texture);
    binding.viewId = view.view;
    return true;
  }

  return false;
}

bool
MakeUnorderedAccessBindingFromDescriptor(
    WMT::Device device, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT *argument,
    UnorderedAccessViewBinding &binding) {
  if (descriptor.type != DescriptorRecordType::UnorderedAccessView)
    return false;
  auto *resource = GetResource(descriptor.resource.ptr());
  if (!resource)
    return false;

  binding = {};
  if (auto *counter_resource = GetResource(descriptor.counter_resource.ptr())) {
    if (counter_resource->GetBuffer())
      binding.counter = Rc<Buffer>(counter_resource->GetBuffer());
  }

  if (resource->GetBuffer()) {
    UINT64 offset = 0;
    if (!ResolveDescriptorBufferOffset(descriptor.resource.ptr(), offset))
      offset = 0;
    UINT64 byte_size = resource->GetResourceDesc().Width;
    const bool needs_texture_buffer_view =
        argument && (argument->Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE);
    BufferSlice slice = DefaultBufferSlice(*resource);
    uint64_t view_id = 0;
    if (descriptor.has_desc) {
      const auto &uav = descriptor.desc.uav;
      if (uav.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
        return false;
      const UINT64 first_element = uav.Buffer.FirstElement;
      if (uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
        offset += first_element * sizeof(uint32_t);
        byte_size = UINT64(uav.Buffer.NumElements) * sizeof(uint32_t);
        if (needs_texture_buffer_view) {
          auto view = CreateBufferView(
              device, *resource, DXGI_FORMAT_R32_UINT, offset, byte_size,
              WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite);
          if (!view)
            return false;
          view_id = view->key;
          slice = TextureBufferSlice(*resource, offset, byte_size,
                                     sizeof(uint32_t));
          slice.firstElement += view->firstElementBias;
        } else {
          slice = StructuredBufferSlice(*resource, offset, byte_size,
                                        sizeof(uint32_t));
        }
      } else if (uav.Format != DXGI_FORMAT_UNKNOWN) {
        MTL_DXGI_FORMAT_DESC format = {};
        if (FAILED(MTLQueryDXGIFormat(device, uav.Format, format)))
          return false;
        offset += first_element * format.BytesPerTexel;
        byte_size = UINT64(uav.Buffer.NumElements) * format.BytesPerTexel;
        if (needs_texture_buffer_view) {
          auto view = CreateBufferView(
              device, *resource, uav.Format, offset, byte_size,
              WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite);
          if (!view)
            return false;
          view_id = view->key;
          slice = TextureBufferSlice(*resource, offset, byte_size,
                                     format.BytesPerTexel);
          slice.firstElement += view->firstElementBias;
        } else {
          slice = StructuredBufferSlice(*resource, offset, byte_size,
                                        format.BytesPerTexel);
        }
      } else if (uav.Buffer.StructureByteStride) {
        offset += first_element * uav.Buffer.StructureByteStride;
        byte_size =
            UINT64(uav.Buffer.NumElements) * uav.Buffer.StructureByteStride;
        if (needs_texture_buffer_view) {
          const auto view_format =
              UintBufferViewFormatForStride(uav.Buffer.StructureByteStride);
          if (view_format == DXGI_FORMAT_UNKNOWN)
            return false;
          auto view = CreateBufferView(
              device, *resource, view_format, offset, byte_size,
              WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite);
          if (!view)
            return false;
          view_id = view->key;
          slice = TextureBufferSlice(
              *resource, offset, byte_size, uav.Buffer.StructureByteStride);
          slice.firstElement += view->firstElementBias;
        } else {
          slice = StructuredBufferSlice(*resource, offset, byte_size,
                                        uav.Buffer.StructureByteStride);
        }
      } else {
        offset += first_element;
        byte_size = uav.Buffer.NumElements;
        if (needs_texture_buffer_view) {
          auto view = CreateBufferView(
              device, *resource, DXGI_FORMAT_R32_UINT, offset, byte_size,
              WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite);
          if (!view)
            return false;
          view_id = view->key;
          slice = TextureBufferSlice(*resource, offset, byte_size,
                                     sizeof(uint32_t));
          slice.firstElement += view->firstElementBias;
        } else {
          slice = DefaultBufferSlice(*resource, offset, byte_size);
        }
      }
    } else if (needs_texture_buffer_view) {
      auto view = CreateBufferView(
          device, *resource, DXGI_FORMAT_R32_UINT, offset, byte_size,
          WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite);
      if (!view)
        return false;
      view_id = view->key;
      slice = TextureBufferSlice(*resource, offset, byte_size,
                                 sizeof(uint32_t));
      slice.firstElement += view->firstElementBias;
    }
    if (!ValidateLegacyBufferSliceRange("UAV", offset, byte_size))
      return false;
    binding.viewId = view_id;
    binding.buffer = Rc<Buffer>(resource->GetBuffer());
    binding.slice = slice;
    return true;
  }

  if (resource->GetTexture()) {
    if (descriptor.has_desc &&
        descriptor.desc.uav.ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
      return false;
    if (resource->IsReservedTexture() &&
        !resource->EnsureTextureAllocation("MakeUnorderedAccessBinding"))
      return false;
    const auto view =
        CreateUnorderedAccessTextureView(device, *resource, descriptor);
    if (!view || !view.texture.ptr())
      return false;
    binding.texture = std::move(view.texture);
    binding.viewId = view.view;
    return true;
  }

  return false;
}

void
ClearShaderResourceBinding(ArgumentEncodingContext &enc, PipelineStage stage,
                           UINT slot) {
  if (slot >= kSRVBindings)
    return;

  if (stage == PipelineStage::Compute)
    enc.bindBuffer<PipelineStage::Compute>(slot, {}, 0, {});
  else if (stage == PipelineStage::Pixel)
    enc.bindBuffer<PipelineStage::Pixel>(slot, {}, 0, {});
  else if (stage == PipelineStage::Geometry)
    enc.bindBuffer<PipelineStage::Geometry>(slot, {}, 0, {});
  else if (stage == PipelineStage::Hull)
    enc.bindBuffer<PipelineStage::Hull>(slot, {}, 0, {});
  else if (stage == PipelineStage::Domain)
    enc.bindBuffer<PipelineStage::Domain>(slot, {}, 0, {});
  else
    enc.bindBuffer<PipelineStage::Vertex>(slot, {}, 0, {});
}

void
ClearUnorderedAccessBinding(ArgumentEncodingContext &enc, PipelineStage stage,
                            UINT slot) {
  if (slot >= kUAVBindings)
    return;

  if (stage == PipelineStage::Compute)
    enc.bindOutputBuffer<PipelineStage::Compute>(slot, {}, 0, {}, {});
  else if (stage == PipelineStage::Pixel)
    enc.bindOutputBuffer<PipelineStage::Pixel>(slot, {}, 0, {}, {});
}

void
BindConstantBufferDescriptor(ArgumentEncodingContext &enc, PipelineStage stage,
                             UINT slot, const DescriptorRecord &descriptor) {
  if (descriptor.type != DescriptorRecordType::ConstantBufferView ||
      !descriptor.has_desc)
    return;
  if (slot >= kLegacyConstantBufferSlotCount) {
    WARN("D3D12CommandQueue: CBV slot b", slot, " is unsupported");
    return;
  }
  if (descriptor.desc.cbv.BufferLocation &
      (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)) {
    WARN("D3D12CommandQueue: root/table CBV BufferLocation is not 256-byte aligned");
    return;
  }
  if (descriptor.desc.cbv.SizeInBytes &
      (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)) {
    WARN("D3D12CommandQueue: root/table CBV SizeInBytes is not 256-byte aligned");
    return;
  }

  Resource *resource = nullptr;
  const auto offset =
      ResolveBufferGpuAddress(descriptor.desc.cbv.BufferLocation, resource);
  const bool null_cbv =
      !descriptor.desc.cbv.BufferLocation && !descriptor.desc.cbv.SizeInBytes;
  if (!null_cbv && (!resource || !resource->GetBuffer()))
    return;
  const auto buffer_offset = null_cbv ? 0 : resource->GetHeapOffset() + offset;

  auto buffer = null_cbv ? Rc<Buffer>() : Rc<Buffer>(resource->GetBuffer());
  switch (stage) {
  case PipelineStage::Compute:
    enc.bindConstantBuffer<PipelineStage::Compute>(slot, buffer_offset,
                                                   std::move(buffer));
    break;
  case PipelineStage::Pixel:
    enc.bindConstantBuffer<PipelineStage::Pixel>(slot, buffer_offset,
                                                 std::move(buffer));
    break;
  case PipelineStage::Geometry:
    enc.bindConstantBuffer<PipelineStage::Geometry>(slot, buffer_offset,
                                                    std::move(buffer));
    break;
  case PipelineStage::Hull:
    enc.bindConstantBuffer<PipelineStage::Hull>(slot, buffer_offset,
                                                std::move(buffer));
    break;
  case PipelineStage::Domain:
    enc.bindConstantBuffer<PipelineStage::Domain>(slot, buffer_offset,
                                                  std::move(buffer));
    break;
  default:
    enc.bindConstantBuffer<PipelineStage::Vertex>(slot, buffer_offset,
                                                  std::move(buffer));
    break;
  }
}

void
BindShaderResourceDescriptor(WMT::Device device, ArgumentEncodingContext &enc,
                             PipelineStage stage, UINT slot,
                             const DescriptorRecord &descriptor,
                             const DXMT12_MTL4_SHADER_ARGUMENT *argument) {
  if (descriptor.type != DescriptorRecordType::ShaderResourceView) {
    ClearShaderResourceBinding(enc, stage, slot);
    return;
  }
  if (slot >= kSRVBindings) {
    WARN("D3D12CommandQueue: SRV slot t", slot, " is unsupported for ",
         PipelineStageName(stage), " stage");
    return;
  }

  ResourceViewBinding binding = {};
  if (!MakeShaderResourceBindingFromDescriptor(device, descriptor, argument,
                                               binding)) {
    ClearShaderResourceBinding(enc, stage, slot);
    return;
  }

  if (binding.buffer.ptr()) {
    if (stage == PipelineStage::Compute)
      enc.bindBuffer<PipelineStage::Compute>(
          slot, std::move(binding.buffer), binding.viewId, binding.slice);
    else if (stage == PipelineStage::Pixel)
      enc.bindBuffer<PipelineStage::Pixel>(
          slot, std::move(binding.buffer), binding.viewId, binding.slice);
    else if (stage == PipelineStage::Geometry)
      enc.bindBuffer<PipelineStage::Geometry>(
          slot, std::move(binding.buffer), binding.viewId, binding.slice);
    else if (stage == PipelineStage::Hull)
      enc.bindBuffer<PipelineStage::Hull>(
          slot, std::move(binding.buffer), binding.viewId, binding.slice);
    else if (stage == PipelineStage::Domain)
      enc.bindBuffer<PipelineStage::Domain>(
          slot, std::move(binding.buffer), binding.viewId, binding.slice);
    else
      enc.bindBuffer<PipelineStage::Vertex>(
          slot, std::move(binding.buffer), binding.viewId, binding.slice);
    return;
  }

  if (binding.texture.ptr()) {
    if (stage == PipelineStage::Compute)
      enc.bindTexture<PipelineStage::Compute>(
          slot, std::move(binding.texture), binding.viewId);
    else if (stage == PipelineStage::Pixel)
      enc.bindTexture<PipelineStage::Pixel>(
          slot, std::move(binding.texture), binding.viewId);
    else if (stage == PipelineStage::Geometry)
      enc.bindTexture<PipelineStage::Geometry>(
          slot, std::move(binding.texture), binding.viewId);
    else if (stage == PipelineStage::Hull)
      enc.bindTexture<PipelineStage::Hull>(
          slot, std::move(binding.texture), binding.viewId);
    else if (stage == PipelineStage::Domain)
      enc.bindTexture<PipelineStage::Domain>(
          slot, std::move(binding.texture), binding.viewId);
    else
      enc.bindTexture<PipelineStage::Vertex>(
          slot, std::move(binding.texture), binding.viewId);
  }
}

void
BindUnorderedAccessDescriptor(WMT::Device device, ArgumentEncodingContext &enc,
                              PipelineStage stage, UINT slot,
                              const DescriptorRecord &descriptor,
                              const DXMT12_MTL4_SHADER_ARGUMENT *argument) {
  if (descriptor.type != DescriptorRecordType::UnorderedAccessView) {
    ClearUnorderedAccessBinding(enc, stage, slot);
    return;
  }
  if (slot >= kUAVBindings) {
    WARN("D3D12CommandQueue: UAV slot u", slot, " is unsupported for ",
         PipelineStageName(stage), " stage");
    return;
  }

  UnorderedAccessViewBinding binding = {};
  if (!MakeUnorderedAccessBindingFromDescriptor(device, descriptor, argument,
                                                binding)) {
    ClearUnorderedAccessBinding(enc, stage, slot);
    return;
  }

  if (binding.buffer.ptr()) {
    if (stage == PipelineStage::Compute)
      enc.bindOutputBuffer<PipelineStage::Compute>(
          slot, std::move(binding.buffer), binding.viewId,
          std::move(binding.counter), binding.slice);
    else if (stage == PipelineStage::Pixel)
      enc.bindOutputBuffer<PipelineStage::Pixel>(
          slot, std::move(binding.buffer), binding.viewId,
          std::move(binding.counter), binding.slice);
    else
      WARN("D3D12CommandQueue: UAV binding for ", PipelineStageName(stage),
           " stage is unsupported");
    return;
  }

  if (binding.texture.ptr()) {
    if (stage == PipelineStage::Compute)
      enc.bindOutputTexture<PipelineStage::Compute>(
          slot, std::move(binding.texture), binding.viewId);
    else if (stage == PipelineStage::Pixel)
      enc.bindOutputTexture<PipelineStage::Pixel>(
          slot, std::move(binding.texture), binding.viewId);
    else
      WARN("D3D12CommandQueue: UAV binding for ", PipelineStageName(stage),
           " stage is unsupported");
  }
}

void
BindSamplerDescriptor(WMT::Device device, ArgumentEncodingContext &enc,
                      PipelineStage stage, UINT slot,
                      const DescriptorRecord &descriptor) {
  if (descriptor.type != DescriptorRecordType::Sampler ||
      !descriptor.has_desc)
    return;
  if (slot >= kLegacySamplerSlotCount) {
    WARN("D3D12CommandQueue: sampler slot s", slot, " is unsupported for ",
         PipelineStageName(stage), " stage");
    return;
  }

  auto sampler = CreateD3D12Sampler(device, descriptor.desc.sampler);
  if (!sampler)
    return;

  if (stage == PipelineStage::Compute)
    enc.bindSampler<PipelineStage::Compute>(slot, std::move(sampler));
  else if (stage == PipelineStage::Pixel)
    enc.bindSampler<PipelineStage::Pixel>(slot, std::move(sampler));
  else if (stage == PipelineStage::Geometry)
    enc.bindSampler<PipelineStage::Geometry>(slot, std::move(sampler));
  else if (stage == PipelineStage::Hull)
    enc.bindSampler<PipelineStage::Hull>(slot, std::move(sampler));
  else if (stage == PipelineStage::Domain)
    enc.bindSampler<PipelineStage::Domain>(slot, std::move(sampler));
  else
    enc.bindSampler<PipelineStage::Vertex>(slot, std::move(sampler));
}

void
BindDescriptor(WMT::Device device, ArgumentEncodingContext &enc,
               PipelineStage stage, D3D12_DESCRIPTOR_RANGE_TYPE range_type,
               UINT slot, const DescriptorRecord &descriptor,
               const DXMT12_MTL4_SHADER_ARGUMENT *argument) {
  switch (range_type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
    BindConstantBufferDescriptor(enc, stage, slot, descriptor);
    break;
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    BindShaderResourceDescriptor(device, enc, stage, slot, descriptor,
                                 argument);
    break;
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
    BindUnorderedAccessDescriptor(device, enc, stage, slot, descriptor,
                                  argument);
    break;
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    BindSamplerDescriptor(device, enc, stage, slot, descriptor);
    break;
  }
}

void
ClearDescriptorBinding(ArgumentEncodingContext &enc, PipelineStage stage,
                       D3D12_DESCRIPTOR_RANGE_TYPE range_type, UINT slot) {
  switch (range_type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    ClearShaderResourceBinding(enc, stage, slot);
    break;
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
    ClearUnorderedAccessBinding(enc, stage, slot);
    break;
  default:
    break;
  }
}

void
ApplyStaticSamplers(WMT::Device device, ArgumentEncodingContext &enc,
                    const PipelineState &pipeline, const RootSignature &root,
                    bool compute) {
  for (const auto &sampler_desc : root.GetStaticSamplers()) {
    ForEachVisibleStage(
        sampler_desc.ShaderVisibility, compute, [&](PipelineStage stage) {
          auto slot = ResolveShaderBindingSlot(
              pipeline, stage, SM50BindingType::Sampler,
              sampler_desc.ShaderRegister, sampler_desc.RegisterSpace);
          if (!slot)
            return;

          auto sampler = CreateD3D12StaticSampler(device, sampler_desc);
          if (!sampler)
            return;
          if (stage == PipelineStage::Compute)
            enc.bindSampler<PipelineStage::Compute>(*slot,
                                                     std::move(sampler));
          else if (stage == PipelineStage::Pixel)
            enc.bindSampler<PipelineStage::Pixel>(*slot,
                                                   std::move(sampler));
          else if (stage == PipelineStage::Geometry)
            enc.bindSampler<PipelineStage::Geometry>(*slot,
                                                     std::move(sampler));
          else if (stage == PipelineStage::Hull)
            enc.bindSampler<PipelineStage::Hull>(*slot,
                                                 std::move(sampler));
          else if (stage == PipelineStage::Domain)
            enc.bindSampler<PipelineStage::Domain>(*slot,
                                                   std::move(sampler));
          else
            enc.bindSampler<PipelineStage::Vertex>(*slot,
                                                    std::move(sampler));
        });
  }
}

} // namespace dxmt::d3d12
