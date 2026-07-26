#include "d3d12_compiled_binding_encode.hpp"

#include "d3d12_binding_fingerprint.hpp"
#include "d3d12_compiled_binding_tables.hpp"

#include <atomic>
#include <cstring>

namespace dxmt::d3d12 {

namespace {

// GraphicsBindingSnapshotEntry::debug_kind marks root-parameter-backed
// descriptors with this prefix; table-backed entries carry the range type name.
constexpr const char kRootBindingDebugKindPrefix[] = "root-";
constexpr size_t kRootBindingDebugKindPrefixLength =
    sizeof(kRootBindingDebugKindPrefix) - 1;

[[nodiscard]] bool
IsRootBindingDebugKind(const char *debug_kind) {
  return debug_kind && std::strncmp(debug_kind, kRootBindingDebugKindPrefix,
                                    kRootBindingDebugKindPrefixLength) == 0;
}

} // namespace

void
EncodeCompiledNativeBindingRecipe(
    ArgumentEncodingContext &enc, const CompiledNativeBindingRecipe &recipe,
    uint32_t dirty_fields, uint64_t root_table_dirty_mask,
    uint64_t root_constant_dirty_mask, uint64_t root_descriptor_dirty_mask,
    CompiledCommandTestTelemetry *test_telemetry) {
  for (const auto &op : recipe.ops) {
    const auto matching_fields = op.required_dirty_fields & dirty_fields;
    const uint32_t non_root_dirty =
        matching_fields & ~CompiledBindingDirtyRootTables;
    const bool root_dirty =
        (matching_fields & CompiledBindingDirtyRootTables) &&
        (!op.root_table_mask || root_table_dirty_mask == UINT64_MAX ||
         (op.root_table_mask & root_table_dirty_mask));
    const bool constant_dirty =
        (matching_fields & CompiledBindingDirtyRootConstants) &&
        (!op.root_constant_mask || root_constant_dirty_mask == UINT64_MAX ||
         (op.root_constant_mask & root_constant_dirty_mask));
    const bool descriptor_dirty =
        (matching_fields & CompiledBindingDirtyRootDescriptors) &&
        (!op.root_descriptor_mask || root_descriptor_dirty_mask == UINT64_MAX ||
         (op.root_descriptor_mask & root_descriptor_dirty_mask));
    const uint32_t other_dirty =
        non_root_dirty & ~(CompiledBindingDirtyRootConstants |
                           CompiledBindingDirtyRootDescriptors);
    if (!other_dirty && !root_dirty && !constant_dirty && !descriptor_dirty) {
      if (test_telemetry)
        test_telemetry->encoder_native_binding_ops_skipped.fetch_add(
            1, std::memory_order_relaxed);
      continue;
    }
    if (test_telemetry)
      test_telemetry->encoder_native_binding_ops.fetch_add(
          1, std::memory_order_relaxed);
    switch (op.kind) {
    case CompiledNativeBindingOpKind::ArgumentBuffer:
      enc.bindNativeArgumentBuffer(op.buffer, op.offset, op.index, op.compute,
                                   op.render_stages);
      break;
    case CompiledNativeBindingOpKind::NullConstantBuffer:
      enc.bindNativeNullConstantBuffer(op.compute, op.render_stages);
      break;
    case CompiledNativeBindingOpKind::NullBuffer:
      enc.bindNativeNullBuffer(op.compute, op.render_stages);
      break;
    }
  }
}

void
FillCompiledBindingState(
    CompiledPacketBindingState &state,
    const std::vector<CompiledCommandRootConstants> &constants,
    const std::vector<CompiledCommandRootDescriptor> &descriptors,
    const GraphicsBindingSnapshot *snapshot) {
  for (const auto &entry : constants) {
    if (entry.root_parameter_index >= state.root_constants.size())
      continue;
    auto &slot = state.root_constants[entry.root_parameter_index];
    slot.valid = true;
    slot.dst_offset = entry.dst_offset;
    slot.values = entry.values;
  }
  for (const auto &entry : descriptors) {
    if (entry.root_parameter_index >= state.cbv_roots.size())
      continue;
    auto *slot = entry.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV
                     ? &state.cbv_roots[entry.root_parameter_index]
                     : entry.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV
                           ? &state.srv_roots[entry.root_parameter_index]
                           : &state.uav_roots[entry.root_parameter_index];
    slot->valid = true;
    slot->address = entry.address;
  }
  if (!snapshot)
    return;
  if (snapshot->frozen_native && snapshot->frozen_native->ready) {
    for (UINT root_index = 0;
         root_index < snapshot->frozen_root_constants.size(); ++root_index) {
      const auto &frozen = snapshot->frozen_root_constants[root_index];
      if (!frozen.valid)
        continue;
      auto &slot = state.root_constants[root_index];
      slot.frozen_buffer = snapshot->frozen_native->root_base_buffer;
      slot.frozen_offset = frozen.offset;
      slot.frozen_length = frozen.length;
      slot.frozen_gpu_address =
          snapshot->frozen_native->root_base_gpu_address + frozen.offset;
    }
  }
  for (const auto &entry : snapshot->entries) {
    if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor ||
        !entry.has_descriptor || entry.root_index >= state.cbv_roots.size() ||
        !IsRootBindingDebugKind(entry.debug_kind))
      continue;
    auto *slot = entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                     ? &state.cbv_roots[entry.root_index]
                     : entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                           ? &state.srv_roots[entry.root_index]
                           : &state.uav_roots[entry.root_index];
    if (!slot->frozen_descriptor)
      slot->frozen_descriptor = SnapshotDescriptor(*snapshot, entry);
  }
}

uint64_t
BuildCompiledDirectGraphicsBindingFingerprint(
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const PipelineState *pipeline, const RootSignature *root,
    const CompiledVertexBindingRecipe *vertex_binding_recipe,
    const GraphicsBindingSnapshot *bindless_snapshot) {
  uint64_t fingerprint = kGraphicsBindingFingerprintOffset;
  if (packet.binding_program) {
    HashGraphicsBindingPointer(fingerprint, packet.binding_program.get());
  } else {
    HashGraphicsBindingPointer(fingerprint, pipeline);
    HashGraphicsBindingPointer(fingerprint, root);
    HashGraphicsBindingPointer(fingerprint,
                               packet.descriptor_heaps.cbv_srv_uav.ptr());
    HashGraphicsBindingPointer(fingerprint,
                               packet.descriptor_heaps.sampler.ptr());
    HashGraphicsBindingPointer(fingerprint, packet.root_constants.identity());
    HashGraphicsBindingPointer(fingerprint, packet.root_descriptors.identity());
    HashGraphicsBindingPointer(fingerprint, vertex_binding_recipe);
    if (!vertex_binding_recipe) {
      HashGraphicsBindingPointer(
          fingerprint, packet.input_assembler.vertex_buffers.identity());
      HashGraphicsBindingValue(
          fingerprint, packet.input_assembler.vertex_buffer_dirty_mask);
    }
  }
  HashGraphicsBindingPointer(fingerprint, prepared.root_tables.get());
  if (bindless_snapshot)
    HashGraphicsBindingValue(fingerprint,
                             bindless_snapshot->content_fingerprint);
  return fingerprint;
}

CompiledBindingDelta
ResolveRenderEncoderBindingDelta(
    const CompiledEncoderBindingIdentity &identity,
    const CompiledBindingDelta &compiled,
    dxmt::DescriptorContentRevision descriptor_content_revision,
    const dxmt::RenderBindingStateCache &cache) {
  CompiledBindingDelta delta = {};
  const bool layout_changed =
      !cache.valid || cache.binding_program != identity.program;
  delta.full_bind = layout_changed;
  if (layout_changed)
    delta.dirty_fields = UINT32_MAX;
  if (!layout_changed && cache.resource_heap != identity.resource_heap)
    delta.dirty_fields |= CompiledBindingDirtyResourceHeap;
  if (!layout_changed && cache.sampler_heap != identity.sampler_heap)
    delta.dirty_fields |= CompiledBindingDirtySamplerHeap;
  const bool descriptor_content_changed =
      cache.descriptor_content_revision != descriptor_content_revision;
  if (!layout_changed && (cache.root_tables != identity.root_tables ||
                          descriptor_content_changed)) {
    delta.dirty_fields |= CompiledBindingDirtyRootTables;
    delta.root_table_dirty_mask =
        descriptor_content_changed
            ? UINT64_MAX
            : ResolveCompiledDirtyMask(cache.root_tables,
                                       compiled.base_root_tables_identity,
                                       compiled.root_table_dirty_mask);
  }
  if (!layout_changed && cache.root_constants != identity.root_constants) {
    delta.dirty_fields |= CompiledBindingDirtyRootConstants;
    delta.root_constant_dirty_mask = ResolveCompiledDirtyMask(
        cache.root_constants, compiled.base_root_constants_identity,
        compiled.root_constant_dirty_mask);
  }
  if (!layout_changed && cache.root_descriptors != identity.root_descriptors) {
    delta.dirty_fields |= CompiledBindingDirtyRootDescriptors;
    delta.root_descriptor_dirty_mask = ResolveCompiledDirtyMask(
        cache.root_descriptors, compiled.base_root_descriptors_identity,
        compiled.root_descriptor_dirty_mask);
  }
  if (!layout_changed && cache.vertex_bindings != identity.vertex_bindings) {
    delta.dirty_fields |= CompiledBindingDirtyVertexBuffers;
    delta.vertex_buffer_dirty_mask = ResolveCompiledDirtyMask(
        cache.vertex_bindings, compiled.base_vertex_bindings_identity,
        compiled.vertex_buffer_dirty_mask);
  }
  if (layout_changed) {
    delta.root_table_dirty_mask = UINT64_MAX;
    delta.root_constant_dirty_mask = UINT64_MAX;
    delta.root_descriptor_dirty_mask = UINT64_MAX;
    delta.vertex_buffer_dirty_mask = UINT32_MAX;
  }
  return delta;
}

CompiledBindingDelta
ResolveComputeEncoderBindingDelta(
    const CompiledDirectComputeBindingPayload &payload,
    const dxmt::ComputeBindingStateCache &cache) {
  CompiledBindingDelta delta = {};
  const auto &identity = payload.binding_identity;
  const auto &compiled = payload.packet.binding_delta;
  const bool layout_changed =
      !cache.binding_valid || cache.binding_program != identity.program;
  delta.full_bind = layout_changed;
  if (layout_changed)
    delta.dirty_fields = UINT32_MAX;
  if (!layout_changed && cache.resource_heap != identity.resource_heap)
    delta.dirty_fields |= CompiledBindingDirtyResourceHeap;
  if (!layout_changed && cache.sampler_heap != identity.sampler_heap)
    delta.dirty_fields |= CompiledBindingDirtySamplerHeap;
  const bool descriptor_content_changed =
      cache.descriptor_content_revision != payload.descriptor_content_revision;
  if (!layout_changed && (cache.root_tables != identity.root_tables ||
                          descriptor_content_changed)) {
    delta.dirty_fields |= CompiledBindingDirtyRootTables;
    delta.root_table_dirty_mask =
        descriptor_content_changed
            ? UINT64_MAX
            : ResolveCompiledDirtyMask(cache.root_tables,
                                       compiled.base_root_tables_identity,
                                       compiled.root_table_dirty_mask);
  }
  if (!layout_changed && cache.root_constants != identity.root_constants) {
    delta.dirty_fields |= CompiledBindingDirtyRootConstants;
    delta.root_constant_dirty_mask = ResolveCompiledDirtyMask(
        cache.root_constants, compiled.base_root_constants_identity,
        compiled.root_constant_dirty_mask);
  }
  if (!layout_changed && cache.root_descriptors != identity.root_descriptors) {
    delta.dirty_fields |= CompiledBindingDirtyRootDescriptors;
    delta.root_descriptor_dirty_mask = ResolveCompiledDirtyMask(
        cache.root_descriptors, compiled.base_root_descriptors_identity,
        compiled.root_descriptor_dirty_mask);
  }
  if (layout_changed) {
    delta.root_table_dirty_mask = UINT64_MAX;
    delta.root_constant_dirty_mask = UINT64_MAX;
    delta.root_descriptor_dirty_mask = UINT64_MAX;
  }
  return delta;
}

} // namespace dxmt::d3d12
