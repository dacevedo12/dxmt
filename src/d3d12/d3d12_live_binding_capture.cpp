#include "d3d12_live_binding_capture.hpp"

#include "d3d12_binding_fingerprint.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_root_binding_capture.hpp"

#include <span>
#include <utility>

namespace dxmt::d3d12 {

namespace {

// Vertex input slots addressable by the 32-bit snapshot slot mask.
constexpr UINT kVertexSlotMaskBits = 32;

} // namespace

void CaptureGraphicsRootDescriptor(GraphicsBindingSnapshot &snapshot,
                                   const ReplayState &state,
                                   const PipelineState &pipeline,
                                   UINT root_index,
                                   const RootSignatureParameter &parameter,
                                   DescriptorRecordType type, bool compute) {
  const auto &map =
      type == DescriptorRecordType::ConstantBufferView
          ? (compute ? state.compute_cbv_roots : state.graphics_cbv_roots)
          : type == DescriptorRecordType::ShaderResourceView
                ? (compute ? state.compute_srv_roots : state.graphics_srv_roots)
                : (compute ? state.compute_uav_roots
                           : state.graphics_uav_roots);
  if (root_index >= ReplayState::kMaxRootParameters)
    return;
  const auto &slot = map[root_index];
  if (!slot.valid)
    return;
  CaptureGraphicsRootDescriptorAddress(snapshot, pipeline, root_index,
                                       parameter, type, slot.address, compute);
}

void CaptureGraphicsRootConstants(GraphicsBindingSnapshot &snapshot,
                                  const ReplayState &state,
                                  const PipelineState &pipeline,
                                  UINT root_index,
                                  const RootSignatureParameter &parameter,
                                  bool compute) {
  if (root_index >= ReplayState::kMaxRootParameters)
    return;
  const auto &slot = compute ? state.compute_root_constants[root_index]
                             : state.graphics_root_constants[root_index];
  CaptureGraphicsRootConstantsValues(
      snapshot, pipeline, root_index, parameter,
      slot.valid ? std::span<const UINT>(slot.values) : std::span<const UINT>{},
      0, compute);
}

void CaptureGraphicsVertexBuffers(
    GraphicsBindingSnapshot &snapshot, const ReplayState &state,
    const PipelineGraphicsState *graphics_state) {
  if (!graphics_state)
    return;

  uint32_t slot_mask = 0;
  for (const auto &element : graphics_state->input_elements) {
    if (element.InputSlot < kVertexSlotMaskBits)
      slot_mask |= 1u << element.InputSlot;
  }
  if (!slot_mask)
    return;

  snapshot.vertex_slot_mask = slot_mask;
  HashGraphicsBindingValue(snapshot.content_fingerprint,
                           snapshot.vertex_slot_mask);
  const auto max_slot = kVertexSlotMaskBits - __builtin_clz(slot_mask);
  for (UINT slot = 0; slot < max_slot; slot++) {
    if (!(slot_mask & (1u << slot)))
      continue;
    const auto &vertex_buffer = state.vertex_buffers[slot];
    if (!vertex_buffer)
      continue;
    const auto &view = *vertex_buffer;
    UINT64 resource_offset = 0;
    auto *resource = LookupBufferResourceByGpuVirtualAddress(
        view.BufferLocation, &resource_offset);
    if (!resource || !resource->GetBuffer())
      continue;

    GraphicsVertexBufferBindingSnapshot binding = {
        slot, view.StrideInBytes, resource->GetHeapOffset() + resource_offset,
        Rc<Buffer>(resource->GetBuffer())};
    HashGraphicsBindingValue(snapshot.content_fingerprint, binding.slot);
    HashGraphicsBindingPointer(snapshot.content_fingerprint,
                               binding.buffer.ptr());
    HashGraphicsBindingValue(snapshot.content_fingerprint, binding.offset);
    HashGraphicsBindingValue(snapshot.content_fingerprint, binding.stride);
    snapshot.vertex_buffers.push_back(std::move(binding));
  }
}

} // namespace dxmt::d3d12
