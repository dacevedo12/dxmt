#pragma once

// Everything a compiled graphics packet needs resolved before it can be
// emitted: the Metal pipeline variant, the per-encoder attachment identity,
// the per-packet binding plan, and the Close-time descriptor diagnostics.
//
// These used to be private members of class CommandQueueImpl
// (d3d12_command_queue_execute.inc). They never touched the queue instance for
// anything but three things, which are now explicit parameters: the Metal
// device the attachment textures and encoder accesses are created against, and
// the queue-owned native stage-plan cache the dense descriptor diagnostic
// memoizes its root-base plans in. Everything else they read lives in the
// CompiledReplayContext bundle of caller locals.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_indirect_topology.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_compiled_execute_types.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "d3d12_stage_plan_cache.hpp"

#include <memory>
#include <optional>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Picks the Metal graphics PSO variant (MSAA demote masks, topology, index
// format) for a compiled packet. Returns None once plan.metal_pso is usable.
[[nodiscard]] CompiledCommandFallbackReason
ResolveCompiledGraphicsMetalPipeline(
    const CompiledGraphicsPacket &packet,
    const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
    DirectIndirectOperation indirect_operation,
    CompiledGraphicsPacketPlan &plan);

// Close-time target and native descriptor diagnostics for a graphics packet.
// `native_stage_plan_cache` is only consulted when the dense correctness
// diagnostic is enabled.
void DiagnoseCompiledGraphicsPacket(
    CompiledReplayContext &ctx,
    SubmissionStagePlanCache<NativeRootBaseStagePlan> &native_stage_plan_cache,
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
    const std::vector<CompiledCommandRootDescriptorTable>
        &submitted_root_tables,
    const ExecuteIndirectRecord *indirect,
    const CompiledGraphicsPacketPlan &plan);

// Builds the per-packet binding state, direct-access list, native binding
// recipe and descriptor fingerprints shared by the three emit paths.
void BuildCompiledGraphicsPacketBindingPlan(
    CompiledReplayContext &ctx, WMT::Device device,
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
    const std::vector<CompiledCommandRootDescriptorTable>
        &submitted_root_tables,
    CompiledGraphicsPacketPlan &plan);

// Materializes the immutable Close-time attachment identity once per active
// graphics encoder rather than once per draw packet. Leaves
// `active_encoder_attachments` untouched when the segment names no render
// state at all.
void MaterializeActiveEncoderAttachments(
    CompiledReplayContext &ctx, WMT::Device device,
    const CompiledCommandSegment &segment,
    std::optional<ReplayRenderPassAttachments> &active_encoder_attachments);

} // namespace dxmt::d3d12
