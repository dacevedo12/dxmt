#pragma once

// Live binding of a root CBV/SRV/UAV descriptor into the argument encoder.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). It stays a template because it
// runs over live replay state, compiled packet state and frozen snapshot
// identities alike; the only piece of the command queue it ever used was the
// Metal device handle, which is now passed in.

#include "Metal.hpp"
#include "d3d12_binding_debug_log.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_legacy_binding_encode.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_root_binding_capture.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_shader_binding.hpp"
#include "dxmt_context.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Binds the root descriptor at `root_index` for every stage the parameter is
// visible to. No-op when the root index is out of range, nothing is bound
// there, or the address does not resolve to a buffer.
template <typename State>
void ApplyRootBufferDescriptor(WMT::Device device, ArgumentEncodingContext &enc,
                               const State &state,
                               const PipelineState &pipeline, bool compute,
                               UINT root_index,
                               const RootSignatureParameter &parameter,
                               DescriptorRecordType type) {
  if (root_index >= ReplayState::kMaxRootParameters)
    return;
  const ReplayRootDescriptorSlot *slot_ptr = nullptr;
  if constexpr (requires { state.compute_cbv_roots; }) {
    if (type == DescriptorRecordType::ConstantBufferView)
      slot_ptr = compute ? &state.compute_cbv_roots[root_index]
                         : &state.graphics_cbv_roots[root_index];
    else if (type == DescriptorRecordType::ShaderResourceView)
      slot_ptr = compute ? &state.compute_srv_roots[root_index]
                         : &state.graphics_srv_roots[root_index];
    else
      slot_ptr = compute ? &state.compute_uav_roots[root_index]
                         : &state.graphics_uav_roots[root_index];
  } else {
    (void)compute;
    if (type == DescriptorRecordType::ConstantBufferView)
      slot_ptr = &state.graphics_cbv_roots[root_index];
    else if (type == DescriptorRecordType::ShaderResourceView)
      slot_ptr = &state.graphics_srv_roots[root_index];
    else
      slot_ptr = &state.graphics_uav_roots[root_index];
  }
  const auto &slot = *slot_ptr;
  if (!slot.valid)
    return;

  RootBufferDescriptorBuild build;
  if (!BuildRootBufferDescriptor(slot.address, type, build))
    return;
  auto *resource = build.resource;
  const auto offset = build.offset;
  const auto &descriptor = build.descriptor;

  const auto range_type = RootDescriptorRangeType(type);
  ForEachVisibleStage(parameter.visibility, compute, [&](PipelineStage stage) {
    const auto *argument = ResolveShaderBindingArgument(
        pipeline, stage, BindingTypeForRange(range_type),
        parameter.descriptor.ShaderRegister,
        parameter.descriptor.RegisterSpace);
    if (!argument)
      return;
    DebugLogRootBinding(RootDescriptorDebugKind(type), pipeline, compute, stage,
                        root_index, argument->SM50BindingSlot,
                        parameter.descriptor.ShaderRegister,
                        parameter.descriptor.RegisterSpace,
                        resource->GetResourceDesc().Width - offset,
                        slot.address, &descriptor, argument);
    BindDescriptor(device, enc, stage, range_type, argument->SM50BindingSlot,
                   descriptor, argument);
  });
}

} // namespace dxmt::d3d12
