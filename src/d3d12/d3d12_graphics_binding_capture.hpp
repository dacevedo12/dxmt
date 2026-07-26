#pragma once

// Capture of one draw's live graphics binding state into a submission-owned
// GraphicsBindingSnapshot.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). Capture reads live ReplayState
// and writes a self-contained snapshot; the only queue-owned things it reached
// for were the Metal device (to materialize frozen sampler payloads) and the
// two memoized stage-plan caches, which are parameters now. The caching /
// batching shell around it (GetOrCaptureGraphicsBindingSnapshot) stays in the
// queue because it owns the pass batches.

#include "Metal.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_stage_plan_cache.hpp"

namespace dxmt::d3d12 {

/** Appends one snapshot entry per descriptor the recipe selects, resolving each
 *  through the heaps bound in `state`. Table handles and heap lookups are
 *  memoized per root index because a recipe walks the same table many times.
 *  Entries whose root table is unbound are skipped entirely, so they never
 *  reach the content fingerprint. */
void CaptureDescriptorTableBindings(WMT::Device device,
                                    GraphicsBindingSnapshot &snapshot,
                                    const ReplayState &state,
                                    const PipelineState &pipeline,
                                    const DescriptorTableBindingRecipe &recipe,
                                    bool compute);

/** Captures everything the deferred graphics replay of this draw will need:
 *  heap and root-signature identity, descriptor tables, root descriptors, root
 *  constants, vertex buffers, and — for the bindless mirror ABI — the frozen
 *  compact stage tables. `capture_stats`, when given, accumulates the per-draw
 *  capture counters the perf reporter consumes. */
[[nodiscard]] GraphicsBindingSnapshot CaptureGraphicsBindingSnapshot(
    WMT::Device device,
    SubmissionStagePlanCache<BindlessMirrorStagePlan> &bindless_plan_cache,
    SubmissionStagePlanCache<NativeRootBaseStagePlan> &native_plan_cache,
    const ReplayState &state, PipelineState &pipeline,
    GraphicsBindingSnapshotCaptureStats *capture_stats = nullptr);

} // namespace dxmt::d3d12
