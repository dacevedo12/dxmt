#pragma once

// Attribution of replayed work to the per-frame statistics block: which bucket
// a queued packet belongs to, and what an assembled graphics pass plan
// contributed.
//
// These were RecordGraphicsPacketClassification() /
// RecordComputePacketClassification() in d3d12_command_queue_pass_queue.inc and
// the statistics fold inside FlushGraphicsPassBatch() in
// d3d12_command_queue_pass_batching.inc. The only thing any of them needed
// `this` for was reaching the statistics block through device_, so the
// derivations move here and the queue keeps wrappers that supply it. The gate
// is a separate predicate because that lookup must stay lazy: it walks
// device_ -> DXMT device -> queue and must not run when perf counters are off.

#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "dxmt_perf_stats.hpp"

namespace dxmt::d3d12 {

/** True when packet classification should be recorded at all: perf counters
 *  must be enabled and the record being replayed must not already have been
 *  attributed a compiled-path fallback reason. */
[[nodiscard]] bool ReplayPacketClassificationEnabled(const ReplayState &state);

/** Counts one graphics packet as compiled, or as a fallback with the reason
 *  implied by the command kind and the geometry/tessellation lowering flags.
 *  Barrier commands are not packets and must be filtered out by the caller. */
void RecordReplayGraphicsPacketClassification(FrameStatistics *stats,
                                              ReplayGraphicsCommandKind kind,
                                              bool compiled_candidate,
                                              bool use_geometry,
                                              bool use_tessellation);

/** Compute counterpart: the only fallback reason a compute packet can carry is
 *  the legacy binding path. */
void RecordReplayComputePacketClassification(FrameStatistics *stats,
                                             bool compiled_candidate);

/** Folds a finished graphics pass plan, and the time it took to build, into
 *  the frame counters. */
void RecordReplayGraphicsPassPlanStats(FrameStatistics *stats,
                                       const ReplayGraphicsPassPlan &plan,
                                       dxmt::clock::duration build_time);

} // namespace dxmt::d3d12
