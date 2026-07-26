#pragma once

// Close-time compiled packet resolution and validation.
//
// These used to be private members of class CommandQueueImpl
// (d3d12_command_queue_execute.inc). None of them reads a CommandQueueImpl
// instance member or names `this`: they only inspect the compiled command
// value types and call free helpers (GetPipelineState(),
// GetDirectIndirectOperation(), ShouldInjectQueueReplayFault()). Hoisting them
// into dxmt::d3d12 lets them be compiled and analyzed independently of the
// ~20k-line queue translation unit.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_command_list.hpp"
#include "d3d12_indirect_topology.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_root_signature.hpp"

#include "airconv_dx12_metal4.h"

#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// A TEXTURE3D render target has no compiled render-pass encoding.
[[nodiscard]] bool CompiledPacketTargetsTexture3DRenderTarget(
    const CompiledGraphicsPacket &packet);

// Resolves the draw / draw-indexed command signature of an indirect node.
[[nodiscard]] CompiledCommandFallbackReason
ResolveCompiledIndirectDrawSignature(
    const ExecuteIndirectRecord &indirect,
    DirectIndirectOperation &indirect_operation,
    const D3D12_COMMAND_SIGNATURE_DESC *&indirect_desc,
    const D3D12_INDIRECT_ARGUMENT_DESC *&indirect_argument);

// Test-only queue-time fault injection shared by the graphics and compute
// native packet paths. Returns None when no fault was injected.
[[nodiscard]] CompiledCommandFallbackReason InjectCompiledNativePacketFaults(
    bool native_packet,
    const std::vector<CompiledCommandRootDescriptorTable>
        &submitted_root_tables,
    const char *pipeline_failure_message,
    const char *descriptor_failure_message);

// Resolves the DXMT pipeline / root signature of a compiled packet and
// rejects ABI mismatches. Returns None once both are usable.
template <typename PacketT>
[[nodiscard]] CompiledCommandFallbackReason ResolveCompiledPacketPipeline(
    const PacketT &packet, bool native_packet, PipelineState *&pipeline,
    RootSignature *&root) {
  pipeline = packet.pipeline.metadata.pipeline;
  if (!pipeline)
    pipeline = GetPipelineState(packet.pipeline.pipeline_state.ptr());
  root = packet.pipeline.root_signature_impl;
  if (!pipeline)
    return CompiledCommandFallbackReason::MissingPipelineState;
  if (!root)
    return native_packet
               ? CompiledCommandFallbackReason::NativeUnsupportedRootSignature
               : CompiledCommandFallbackReason::MissingRootSignature;
  if (pipeline->GetShaderAbiVersion() !=
      packet.pipeline.metadata.shader_abi_version)
    return native_packet
               ? CompiledCommandFallbackReason::NativeShaderAbiMismatch
               : CompiledCommandFallbackReason::NonBindlessPipelineState;
  if (pipeline->GetShaderAbiVersion() !=
          DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE &&
      !pipeline->UsesBindlessMirror())
    return native_packet
               ? CompiledCommandFallbackReason::NativeShaderAbiMismatch
               : CompiledCommandFallbackReason::NonBindlessPipelineState;
  return CompiledCommandFallbackReason::None;
}

// Structural validation of the Close-time encoder program against the
// submission-time plan. A false result forces whole-list compatibility replay.
[[nodiscard]] bool IsCompiledCommandListPlanValid(
    const CompiledCommandList *compiled,
    const SubmittedCompiledCommandListPlan *submitted,
    const std::vector<CommandRecord> &records);

// True when an indirect packet's own compiled state already carries this
// deferred setter, so replaying it before the packet would be redundant.
[[nodiscard]] bool IndirectPacketCapturesStateRecord(
    const CommandRecord &record, bool compute);

} // namespace dxmt::d3d12
