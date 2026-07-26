#pragma once

// Interpreted application of a root signature's non-table root parameters (and
// the descriptor tables alongside them) into the argument encoder.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_descriptor_binding.inc /
// d3d12_command_queue_native_binding.inc). They stay templates because they run
// over live replay state and frozen snapshot identities alike; the only pieces
// of the command queue they ever used were the Metal device handle and the DXMT
// queue that owns the argument-buffer ring, both of which are now passed in.

#include "Metal.hpp"
#include "d3d12_binding_debug_log.hpp"
#include "d3d12_legacy_binding_encode.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_root_buffer_bind.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_shader_binding.hpp"
#include "d3d12_table_recipe_apply.hpp"
#include "d3d12_table_recipe_lookup.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Materializes the 32-bit root constants bound at `root_index` into a fresh
// argument-buffer slice and binds it as a constant buffer to every stage the
// parameter is visible to.
//
// The slice is sized by the larger of the declared Num32BitValues and what was
// actually recorded, because a command list may set more words than the root
// signature declares; the recorded bytes are then clamped to the allocation.
template <typename State>
void BindRootConstants(::dxmt::CommandQueue &queue,
                       ArgumentEncodingContext &enc, const State &state,
                       const PipelineState &pipeline, bool compute,
                       UINT root_index,
                       const RootSignatureParameter &parameter) {
  if (root_index >= ReplayState::kMaxRootParameters)
    return;
  const auto &slot = [&]() -> const ReplayRootConstantsSlot & {
    if constexpr (requires { state.compute_root_constants; })
      return compute ? state.compute_root_constants[root_index]
                     : state.graphics_root_constants[root_index];
    else
      return state.graphics_root_constants[root_index];
  }();

  const auto declared_count = parameter.constants.Num32BitValues;
  const auto actual_count =
      std::max<uint32_t>(declared_count,
                         slot.valid ? uint32_t(slot.values.size()) : 0u);
  if (!actual_count)
    return;

  const auto byte_length = uint64_t(actual_count) * sizeof(UINT);
  auto constants = queue.AllocateArgumentBuffer(
      enc.currentSeqId(), byte_length,
      D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
  if (!constants.valid() || !constants.fill_zero())
    return;
  if (slot.valid && !slot.values.empty()) {
    const auto nbytes = std::min<uint64_t>(
        uint64_t(slot.values.size()) * sizeof(UINT), constants.length);
    if (!constants.write(0, slot.values.data(), size_t(nbytes)))
      return;
  }
  constants.flush_if_needed();
  const auto gpu_address = constants.gpu_address + constants.offset;

  ForEachVisibleStage(parameter.visibility, compute, [&](PipelineStage stage) {
    auto slot = ResolveShaderBindingSlot(
        pipeline, stage, SM50BindingType::ConstantBuffer,
        parameter.constants.ShaderRegister,
        parameter.constants.RegisterSpace);
    if (!slot)
      return;
    DebugLogRootBinding("root-constants", pipeline, compute, stage,
                        root_index, *slot,
                        parameter.constants.ShaderRegister,
                        parameter.constants.RegisterSpace,
                        byte_length,
                        0);
    if (*slot >= 14) {
      WARN("D3D12CommandQueue: root constants target unsupported CBV slot b",
           *slot);
      return;
    }
    switch (stage) {
    case PipelineStage::Compute:
      enc.bindConstantBufferDirect<PipelineStage::Compute>(
          *slot, constants.gpu_buffer, gpu_address, byte_length);
      break;
    case PipelineStage::Pixel:
      enc.bindConstantBufferDirect<PipelineStage::Pixel>(
          *slot, constants.gpu_buffer, gpu_address, byte_length);
      break;
    case PipelineStage::Geometry:
      enc.bindConstantBufferDirect<PipelineStage::Geometry>(
          *slot, constants.gpu_buffer, gpu_address, byte_length);
      break;
    case PipelineStage::Hull:
      enc.bindConstantBufferDirect<PipelineStage::Hull>(
          *slot, constants.gpu_buffer, gpu_address, byte_length);
      break;
    case PipelineStage::Domain:
      enc.bindConstantBufferDirect<PipelineStage::Domain>(
          *slot, constants.gpu_buffer, gpu_address, byte_length);
      break;
    case PipelineStage::Vertex:
    default:
      enc.bindConstantBufferDirect<PipelineStage::Vertex>(
          *slot, constants.gpu_buffer, gpu_address, byte_length);
      break;
    }
  });
}

// Binds the static samplers, the descriptor tables (through the memoized
// binding recipe) and every root constant / root CBV / SRV / UAV declared by
// the root signature `state` currently has bound. No-op without a root
// signature.
template <typename State>
void ApplyRootDescriptorTables(WMT::Device device, ::dxmt::CommandQueue &queue,
                               ArgumentEncodingContext &enc, const State &state,
                               const PipelineState &pipeline, bool compute) {
  auto *root = [&]() -> RootSignature * {
    if constexpr (requires { state.compute_root_signature_impl; })
      return compute ? state.compute_root_signature_impl
                     : state.graphics_root_signature_impl;
    else
      return state.graphics_root_signature_impl;
  }();
  if (!root)
    return;

  ApplyStaticSamplers(device, enc, pipeline, *root, compute);
  ApplyDescriptorTableBindingRecipe(
      device, enc, state, pipeline, compute,
      GetDescriptorTableBindingRecipe(pipeline, *root, compute));

  const auto parameters = root->GetParameters();
  for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
    const auto &parameter = parameters[root_index];
    if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      continue;
    } else if (parameter.parameter_type ==
               D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
      BindRootConstants(queue, enc, state, pipeline, compute, root_index,
                        parameter);
    } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
      ApplyRootBufferDescriptor(device, enc, state, pipeline, compute,
                                root_index, parameter,
                                DescriptorRecordType::ConstantBufferView);
    } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
      ApplyRootBufferDescriptor(device, enc, state, pipeline, compute,
                                root_index, parameter,
                                DescriptorRecordType::ShaderResourceView);
    } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
      ApplyRootBufferDescriptor(device, enc, state, pipeline, compute,
                                root_index, parameter,
                                DescriptorRecordType::UnorderedAccessView);
    }
  }
}

} // namespace dxmt::d3d12
