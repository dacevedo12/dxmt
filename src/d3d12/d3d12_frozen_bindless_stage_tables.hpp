#pragma once

// Capture-time freeze of the compact bindless mirror tables (slots 28-30) that
// a submitted GraphicsBindingSnapshot will bind at encode time.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). It resolves everything out of the
// already-captured snapshot rather than live replay state, so the only pieces
// of the command queue it ever needed were the Metal device (to materialize
// static samplers and texture window payloads) and the queue-owned memo for
// bindless stage plans; both are parameters now.

#include "Metal.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_stage_plan_cache.hpp"

namespace dxmt::d3d12 {

/** Freezes `want_stage`'s compact root_offsets plus texture/sampler windows out
 *  of the snapshot's captured descriptors. Texture payloads are resolved from
 *  submitted DescriptorRecord values only: buffer-SRV textures that would need
 *  encode-time residency stay zero, because packBindlessStage still owns buffer
 *  addresses in slot 27.
 *
 *  Every attempt that gets this far marks the stage valid, including a stage
 *  with no shader, no root signature or no mirror arguments at all, so encode
 *  never mistakes an empty stage for a materialize failure. */
void FreezeBindlessStageTables(
    WMT::Device device,
    SubmissionStagePlanCache<BindlessMirrorStagePlan> &plan_cache,
    GraphicsBindingSnapshot &snapshot, const PipelineState &pipeline,
    PipelineStage want_stage, bool compute);

/** Freezes every stage the pipeline kind can bind: Compute alone, or the
 *  Vertex/Pixel pair. A non-bindless snapshot marks all three stages valid
 *  without touching any plan. */
void FreezeAllBindlessStageTables(
    WMT::Device device,
    SubmissionStagePlanCache<BindlessMirrorStagePlan> &plan_cache,
    GraphicsBindingSnapshot &snapshot, const PipelineState &pipeline,
    bool compute);

} // namespace dxmt::d3d12
