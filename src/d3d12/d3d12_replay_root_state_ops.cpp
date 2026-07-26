#include "d3d12_replay_root_state_ops.hpp"

#include "d3d12_command_list.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_replay_state_ops.hpp"

namespace dxmt::d3d12 {

void StoreRootDescriptor(ReplayState &state,
                         const RootDescriptorRecord &record) {
  if (record.root_parameter_index >= ReplayState::kMaxRootParameters)
    return;
  ReplayRootDescriptorSlot *slot = nullptr;
  switch (record.parameter_type) {
  case D3D12_ROOT_PARAMETER_TYPE_CBV:
    slot = record.compute
               ? &state.compute_cbv_roots[record.root_parameter_index]
               : &state.graphics_cbv_roots[record.root_parameter_index];
    break;
  case D3D12_ROOT_PARAMETER_TYPE_UAV:
    slot = record.compute
               ? &state.compute_uav_roots[record.root_parameter_index]
               : &state.graphics_uav_roots[record.root_parameter_index];
    break;
  case D3D12_ROOT_PARAMETER_TYPE_SRV:
  default:
    slot = record.compute
               ? &state.compute_srv_roots[record.root_parameter_index]
               : &state.graphics_srv_roots[record.root_parameter_index];
    break;
  }
  slot->valid = true;
  slot->address = record.address;
}

void StoreRootConstants(ReplayState &state,
                        const RootConstantsRecord &record) {
  if (record.root_parameter_index >= ReplayState::kMaxRootParameters)
    return;
  auto &slot = record.compute
                   ? state.compute_root_constants[record.root_parameter_index]
                   : state.graphics_root_constants[record.root_parameter_index];
  slot.valid = true;
  ApplyRootConstants(slot.values, record.dst_offset, record.values);
}

bool CurrentPipelineIsCompute(const ReplayState &state) {
  auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
  return pipeline && pipeline->GetType() == PipelineStateType::Compute;
}

void ReplaySetPredication(ReplayState &state,
                          const PredicationRecord &record) {
  state.predication_buffer = record.buffer;
  state.predication_buffer_offset = record.buffer_offset;
  state.predication_operation = record.operation;
}

} // namespace dxmt::d3d12
