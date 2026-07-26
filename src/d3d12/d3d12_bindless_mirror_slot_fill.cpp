#include "d3d12_bindless_mirror_slot_fill.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_sampler.hpp"
#include "log/log.hpp"

#include <optional>
#include <utility>

namespace dxmt::d3d12 {

void
VerifyBindlessMirrorSamplerSlot(DescriptorHeapMirror *mirror, UINT slot,
                                const Sampler *sampler, uint64_t dummy_handle,
                                PipelineStage stage,
                                const DXMT12_MTL4_SHADER_ARGUMENT *arg) {
  if (!BindlessMirrorVerifyEnabled())
    return;
  uint64_t expected[kMirrorSamplerQwords] = {};
  if (sampler)
    EncodeMirrorSamplerSlot(expected, *sampler);
  else
    EncodeMirrorSamplerSlotNull(expected, dummy_handle);
  const auto payload = mirror ? mirror->samplerSlotPayload(slot)
                              : std::nullopt;
  if (!payload)
    return;
  const uint64_t got[kMirrorSamplerQwords] = {
      payload->handle, payload->cube_handle, payload->lod_bias};
  for (uint32_t i = 0; i < kMirrorSamplerQwords; i++) {
    if (got[i] == expected[i])
      continue;
    if (!BindlessMirrorVerifyShouldLog())
      return;
    ERR("DXMT bindless-mirror VERIFY mismatch (sampler)"
        " draw=", DiagCurrentReplayRecordSequence(),
        " drawSerial=", DiagCurrentReplayRecordSerial(),
        " stage=", PipelineStageName(stage),
        " arg=", arg ? arg->StructurePtrOffset : UINT32_MAX,
        " slot=", slot,
        " field=qword", i,
        " expected=0x", std::hex, expected[i],
        " actual=0x", got[i], std::dec);
  }
}

void
VerifyBindlessMirrorTextureSlot(ArgumentEncodingContext &enc,
                                DescriptorHeapMirror *mirror, UINT slot,
                                const Rc<Texture> &texture,
                                TextureViewKey view) {
  (void)enc;
  (void)mirror;
  (void)slot;
  (void)texture;
  (void)view;
}

void
VerifyBindlessMirrorSamplerDescriptor(
    WMT::Device device, ArgumentEncodingContext &enc,
    const DescriptorRecord &record, PipelineStage stage,
    const DXMT12_MTL4_SHADER_ARGUMENT *arg) {
  if (!BindlessMirrorVerifyEnabled())
    return;
  if (record.type != DescriptorRecordType::Sampler || !record.has_desc) {
    VerifyBindlessMirrorSamplerSlot(record.mirror, record.heap_index, nullptr,
                                    enc.dummySamplerHandle(), stage, arg);
    return;
  }
  auto sampler = CreateD3D12Sampler(device, record.desc.sampler);
  VerifyBindlessMirrorSamplerSlot(record.mirror, record.heap_index,
                                  sampler.ptr(), enc.dummySamplerHandle(),
                                  stage, arg);
}

BindlessMirrorFillKind
FillBindlessMirrorSlot(WMT::Device device, ArgumentEncodingContext &enc,
                       DescriptorHeapMirror *mirror,
                       const DescriptorRecord &record, PipelineStage stage,
                       const DXMT12_MTL4_SHADER_ARGUMENT *arg) {
  if (!mirror)
    return BindlessMirrorFillKind::None;
  const UINT slot = record.heap_index;
  const auto pending_version = mirror->slotPendingVersion(slot);
  if (pending_version && record.slot_version != *pending_version)
    return BindlessMirrorFillKind::None;
  if (mirror->isSamplerHeap()) {
    if (record.type != DescriptorRecordType::Sampler || !record.has_desc) {
      bool filled = false;
      if (pending_version) {
        auto mirror_lock = mirror->AcquireLock();
        filled = mirror->FillSamplerSlot(
                     slot, nullptr, enc.dummySamplerHandle(),
                     *pending_version) &&
                 ReplaceDescriptorMirrorResidencyTargetForEncode(
                     enc, *mirror, slot, *pending_version, {});
      }
      if (filled) {
        VerifyBindlessMirrorSamplerSlot(mirror, slot, nullptr,
                                        enc.dummySamplerHandle(), stage, arg);
        return BindlessMirrorFillKind::Sampler;
      }
      VerifyBindlessMirrorSamplerSlot(mirror, slot, nullptr,
                                      enc.dummySamplerHandle(), stage, arg);
      return BindlessMirrorFillKind::None;
    }
    if (!pending_version && !BindlessMirrorVerifyEnabled())
      return BindlessMirrorFillKind::None;
    auto sampler = record.materialized_sampler;
    if (!sampler)
      sampler = CreateD3D12Sampler(device, record.desc.sampler);
    if (!sampler)
      return BindlessMirrorFillKind::None;
    if (pending_version) {
      auto mirror_lock = mirror->AcquireLock();
      if (!mirror->FillSamplerSlot(slot, sampler.ptr(),
                                   enc.dummySamplerHandle(),
                                   *pending_version))
        return BindlessMirrorFillKind::None;
      DescriptorResidencyTarget target = {};
      target.sampler = sampler;
      if (!ReplaceDescriptorMirrorResidencyTargetForEncode(
              enc, *mirror, slot, *pending_version,
              std::move(target)))
        return BindlessMirrorFillKind::None;
      VerifyBindlessMirrorSamplerSlot(mirror, slot, sampler.ptr(),
                                      enc.dummySamplerHandle(), stage, arg);
      return BindlessMirrorFillKind::Sampler;
    }
    VerifyBindlessMirrorSamplerSlot(mirror, slot, sampler.ptr(),
                                    enc.dummySamplerHandle(), stage, arg);
    return BindlessMirrorFillKind::None;
  }

  // A fill token ties the DescriptorRecord snapshot to one app-thread write.
  // Conditional mirror writes below refuse to publish if a newer descriptor
  // generation wins while resource/view materialization is in progress.
  if (!pending_version)
    return BindlessMirrorFillKind::None;
  const dxmt::DescriptorSlotVersion expected_version = *pending_version;
  auto clear_current = [&] {
    auto mirror_lock = mirror->AcquireLock();
    if (!mirror->ClearTextureSlot(slot, expected_version) ||
        !ReplaceDescriptorMirrorResidencyTargetForEncode(
            enc, *mirror, slot, expected_version, {}))
      return BindlessMirrorFillKind::None;
    return BindlessMirrorFillKind::Null;
  };

  // CBV/SRV/UAV mirror: ordinary buffer descriptors are covered by the per-draw
  // buf_table, but DXBC typed/structured `dcl_resource dim=buffer` lowers to a
  // Metal texture-buffer argument. Those descriptors must publish their texture
  // view handle here just like texture SRVs/UAVs.
  Resource *resource = nullptr;
  if (record.type == DescriptorRecordType::ShaderResourceView ||
      record.type == DescriptorRecordType::UnorderedAccessView)
    resource = GetResource(record.resource.ptr());
  if (!resource)
    return clear_current();

  if (record.type == DescriptorRecordType::ShaderResourceView) {
    if (resource->GetBuffer()) {
      if (!arg || !(arg->Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE))
        return clear_current();
      auto binding = CreateShaderResourceTextureBufferBinding(
          device, *resource, record, WMTTextureUsageShaderRead);
      if (!binding)
        return clear_current();
      auto buffer = Rc<Buffer>(resource->GetBuffer());
      return FillBindlessTextureBufferMirrorSlot(
                 enc, *mirror, slot, stage, buffer, binding->first,
                 binding->second, ResourceAccess::Read,
                 expected_version)
                 ? BindlessMirrorFillKind::TextureBuffer
                 : BindlessMirrorFillKind::None;
    }
    if (!resource->GetTexture())
      return clear_current();
    const auto view =
        CreateShaderResourceTextureView(device, *resource, record);
    if (!view || !view.texture.ptr())
      return clear_current();
    auto *allocation = view.texture->current();
    if (!allocation)
      return clear_current();
    const uint64_t gpu_resource_id = view.texture->view(view.view, allocation).gpuResourceID;
    const uint32_t array_length = view.texture->arrayLength(view.view);
    auto mirror_lock = mirror->AcquireLock();
    if (!mirror->FillTextureSlot(slot, gpu_resource_id, array_length, 0.0f,
                                 expected_version))
      return BindlessMirrorFillKind::None;
    DescriptorResidencyTarget target = {};
    target.mirror_allocation = allocation->texture();
    if (!ReplaceDescriptorMirrorResidencyTargetForEncode(
            enc, *mirror, slot, expected_version, std::move(target)))
      return BindlessMirrorFillKind::None;
    VerifyBindlessMirrorTextureSlot(enc, mirror, slot, view.texture, view.view);
    return BindlessMirrorFillKind::Texture;
  } else if (record.type == DescriptorRecordType::UnorderedAccessView) {
    if (resource->GetBuffer()) {
      if (!arg || !(arg->Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE))
        return clear_current();
      bool read = (arg->Flags >> 10) & 1;
      bool write = (arg->Flags >> 10) & 2;
      if (!read && !write) {
        read = true;
        write = true;
      }
      auto binding = CreateUnorderedAccessTextureBufferBinding(
          device, *resource, record,
          WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite);
      if (!binding)
        return clear_current();
      auto buffer = Rc<Buffer>(resource->GetBuffer());
      return FillBindlessTextureBufferMirrorSlot(
                 enc, *mirror, slot, stage, buffer, binding->first,
                 binding->second,
                 (read ? ResourceAccess::Read : 0) |
                     (write ? ResourceAccess::Write : 0) |
                     ResourceAccess::UAV,
                 expected_version)
                 ? BindlessMirrorFillKind::TextureBuffer
                 : BindlessMirrorFillKind::None;
    }
    if (record.has_desc &&
        record.desc.uav.ViewDimension == D3D12_UAV_DIMENSION_BUFFER)
      return clear_current();
    if (!resource->GetTexture())
      return clear_current();
    if (resource->IsReservedTexture() &&
        !resource->EnsureTextureAllocation("FillBindlessMirrorSlot"))
      return clear_current();
    const auto view =
        CreateUnorderedAccessTextureView(device, *resource, record);
    if (!view || !view.texture.ptr())
      return clear_current();
    auto *allocation = view.texture->current();
    if (!allocation)
      return clear_current();
    const uint64_t gpu_resource_id = view.texture->view(view.view, allocation).gpuResourceID;
    const uint32_t array_length = view.texture->arrayLength(view.view);
    auto mirror_lock = mirror->AcquireLock();
    if (!mirror->FillTextureSlot(slot, gpu_resource_id, array_length, 0.0f,
                                 expected_version))
      return BindlessMirrorFillKind::None;
    DescriptorResidencyTarget target = {};
    target.mirror_allocation = allocation->texture();
    if (!ReplaceDescriptorMirrorResidencyTargetForEncode(
            enc, *mirror, slot, expected_version, std::move(target)))
      return BindlessMirrorFillKind::None;
    VerifyBindlessMirrorTextureSlot(enc, mirror, slot, view.texture, view.view);
    return BindlessMirrorFillKind::Texture;
  }
  return BindlessMirrorFillKind::None;
}

BindlessMirrorFillKind
MaybeFillBindlessMirrorSlot(WMT::Device device, ArgumentEncodingContext &enc,
                            D3D12_DESCRIPTOR_RANGE_TYPE range_type,
                            const DescriptorRecord &record, PipelineStage stage,
                            const DXMT12_MTL4_SHADER_ARGUMENT *arg) {
  if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
    return FillBindlessMirrorSlot(device, enc, record.mirror, record, stage,
                                  arg);
  }

  if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV ||
      range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
    return FillBindlessMirrorSlot(device, enc, record.mirror, record, stage,
                                  arg);
  }
  return BindlessMirrorFillKind::None;
}

} // namespace dxmt::d3d12
