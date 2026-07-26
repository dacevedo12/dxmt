#include "d3d12_selected_descriptor_diag.hpp"

#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_resource.hpp"

#include <optional>

namespace dxmt::d3d12 {

void
AddSelectedDescriptorReason(SelectedDescriptorConsistency &diag, uint32_t flag,
                            const char *reason) {
  diag.flags |= flag;
  if (!diag.reasons.empty())
    diag.reasons += ',';
  diag.reasons += reason;
}

SelectedDescriptorConsistency
DiagnoseSelectedDescriptor(const DescriptorRecord &descriptor,
                           const DXMT12_MTL4_SHADER_ARGUMENT *argument) {
  SelectedDescriptorConsistency diag = {};
  constexpr uint32_t kMissingMirror = 1u << 0;
  constexpr uint32_t kSlotOutOfRange = 1u << 1;
  constexpr uint32_t kArgumentTypeMismatch = 1u << 2;
  constexpr uint32_t kRecordVersionMismatch = 1u << 3;
  constexpr uint32_t kSlotStillPending = 1u << 4;
  constexpr uint32_t kFilledVersionMismatch = 1u << 5;
  constexpr uint32_t kBackendKindMismatch = 1u << 6;
  constexpr uint32_t kMissingBackendPayload = 1u << 7;
  constexpr uint32_t kInvalidNativeBuffer = 1u << 8;
  constexpr uint32_t kNullPayloadNotCleared = 1u << 9;
  constexpr uint32_t kShaderShapeMismatch = 1u << 10;

  auto descriptor_matches_argument = [&] {
    if (!argument)
      return true;
    switch (argument->Type) {
    case SM50BindingType::ConstantBuffer:
      return descriptor.type == DescriptorRecordType::ConstantBufferView;
    case SM50BindingType::SRV:
      return descriptor.type == DescriptorRecordType::ShaderResourceView ||
             descriptor.type == DescriptorRecordType::Empty;
    case SM50BindingType::UAV:
      return descriptor.type == DescriptorRecordType::UnorderedAccessView ||
             descriptor.type == DescriptorRecordType::Empty;
    case SM50BindingType::Sampler:
      return descriptor.type == DescriptorRecordType::Sampler ||
             descriptor.type == DescriptorRecordType::Empty;
    default:
      return true;
    }
  };
  if (!descriptor_matches_argument())
    AddSelectedDescriptorReason(diag, kArgumentTypeMismatch,
                                "shader-argument-type-mismatch");

  auto *resource = GetResource(descriptor.resource.ptr());
  diag.legal_null =
      descriptor.type == DescriptorRecordType::Empty ||
      ((descriptor.type == DescriptorRecordType::ShaderResourceView ||
        descriptor.type == DescriptorRecordType::UnorderedAccessView) &&
       !resource) ||
      (descriptor.type == DescriptorRecordType::ConstantBufferView &&
       (!descriptor.has_desc ||
        (!descriptor.desc.cbv.BufferLocation &&
         !descriptor.desc.cbv.SizeInBytes))) ||
      (descriptor.type == DescriptorRecordType::Sampler &&
       !descriptor.has_desc);

  if (!diag.legal_null) {
    if (descriptor.type == DescriptorRecordType::Sampler) {
      diag.expected_kind = DescriptorBackendSlotKind::Sampler;
    } else if (descriptor.type == DescriptorRecordType::ConstantBufferView ||
               (resource && resource->GetBuffer())) {
      diag.expected_kind = DescriptorBackendSlotKind::Buffer;
    } else if (resource && resource->GetTexture()) {
      diag.expected_kind = DescriptorBackendSlotKind::Texture;
    }
  }

  if (argument && !diag.legal_null &&
      (descriptor.type == DescriptorRecordType::ShaderResourceView ||
       descriptor.type == DescriptorRecordType::UnorderedAccessView)) {
    const bool shader_expects_texture =
        argument->Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE;
    const bool resource_is_texture = resource && resource->GetTexture();
    const bool resource_is_buffer = resource && resource->GetBuffer();
    if ((!shader_expects_texture && resource_is_texture) ||
        (!resource_is_texture && !resource_is_buffer)) {
      AddSelectedDescriptorReason(diag, kShaderShapeMismatch,
                                  "shader-resource-shape-mismatch");
    }
  }

  auto *mirror = descriptor.mirror;
  if (!mirror) {
    if (descriptor.shader_visible)
      AddSelectedDescriptorReason(diag, kMissingMirror,
                                  "shader-visible-mirror-missing");
    return diag;
  }
  if (descriptor.heap_index >= mirror->numDescriptors()) {
    AddSelectedDescriptorReason(diag, kSlotOutOfRange,
                                "mirror-slot-out-of-range");
    return diag;
  }

  const auto slot = descriptor.heap_index;
  diag.stale_version = mirror->slotStaleVersion(slot);
  diag.filled_version = mirror->slotFilledVersion(slot);
  diag.needs_fill = mirror->SlotNeedsFill(slot);
  if (diag.stale_version != descriptor.slot_version)
    AddSelectedDescriptorReason(diag, kRecordVersionMismatch,
                                "record-stale-version-mismatch");
  if (diag.needs_fill)
    AddSelectedDescriptorReason(diag, kSlotStillPending,
                                "mirror-fill-still-pending-at-bind");
  if (!diag.needs_fill && diag.filled_version != descriptor.slot_version)
    AddSelectedDescriptorReason(diag, kFilledVersionMismatch,
                                "record-filled-version-mismatch");

  const auto meta = mirror->slotMeta(slot);
  if (meta)
    diag.actual_kind = meta->kind;
  if (!meta || diag.actual_kind != diag.expected_kind)
    AddSelectedDescriptorReason(diag, kBackendKindMismatch,
                                "descriptor-backend-kind-mismatch");

  const auto table = mirror->descriptorTableEntry(slot);
  if (table)
    diag.table = *table;
  const auto native = mirror->bufferDescriptorRecord(slot);
  if (native)
    diag.native = *native;
  const auto texture =
      mirror->textureSlotPayload(slot, descriptor.slot_version);
  if (texture)
    diag.texture = *texture;

  if (diag.legal_null && !mirror->isSamplerHeap()) {
    if (diag.table.gpu_va || diag.table.texture_view_id ||
        diag.table.metadata || diag.native.resource_index ||
        diag.native.flags || diag.native.byte_offset ||
        diag.native.byte_size || diag.texture.handle ||
        diag.texture.metadata) {
      AddSelectedDescriptorReason(diag, kNullPayloadNotCleared,
                                  "legal-null-retains-backend-payload");
    }
  } else if (diag.expected_kind == DescriptorBackendSlotKind::Texture) {
    if (!diag.table.texture_view_id || !diag.texture.handle)
      AddSelectedDescriptorReason(diag, kMissingBackendPayload,
                                  "texture-backend-payload-missing");
  } else if (diag.expected_kind == DescriptorBackendSlotKind::Buffer) {
    const auto backend = diag.native.resource_index
                             ? mirror->backendResourceRecord(
                                   diag.native.resource_index)
                             : std::nullopt;
    diag.native_diag_flags =
        DiagnoseNativeBufferDescriptor(diag.native, backend);
    if (diag.native_diag_flags)
      AddSelectedDescriptorReason(diag, kInvalidNativeBuffer,
                                  "native-buffer-record-invalid");

    const bool shader_expects_texture =
        argument && (argument->Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE);
    if (shader_expects_texture &&
        (!diag.table.texture_view_id || !diag.texture.handle)) {
      AddSelectedDescriptorReason(diag, kMissingBackendPayload,
                                  "texture-buffer-payload-missing");
    }
  }
  return diag;
}

} // namespace dxmt::d3d12
