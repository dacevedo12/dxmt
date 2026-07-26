#pragma once

// Per-frame performance ledger of a logical queue's replay work.
//
// Extracted from class CommandQueueImpl
// (d3d12_command_queue_submission.inc), where it was three loose members that
// only ever changed together. Replay runs on the submission worker, not on the
// Present thread, so the ledger is worker-owned and one aggregate is flushed
// per frame; grouping the three into one value makes that ownership the
// property of a type instead of a convention.

#include "dxmt_perf_stats.hpp"

#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

/**
 * Statistics accumulated for one replayed frame plus the frame they belong to.
 * `frame_id == kNoReplayPerfFrame` means nothing is being accumulated.
 */
struct ReplayPerfFrameAccumulator final {
  static constexpr uint64_t kNoReplayPerfFrame = ~0ull;

  dxmt::FrameStatistics stats;
  uint64_t frame_id = kNoReplayPerfFrame;
  uint64_t execute_batches = 0;
};

/**
 * Opens `frame_id` on the accumulator, flushing whatever frame was open before,
 * and returns the statistics block the caller should bind for this batch.
 * Returns null when performance collection is off, which is also the signal to
 * the caller that no accumulation happens at all.
 */
[[nodiscard]] dxmt::FrameStatistics *
BeginReplayPerfFrame(ReplayPerfFrameAccumulator &accumulator,
                     uint64_t frame_id, uintptr_t queue_identity,
                     D3D12_COMMAND_LIST_TYPE queue_type);

/**
 * Publishes the open frame, if any, and resets the accumulator. A frame that
 * saw no execute batch is dropped rather than reported as an empty one.
 */
void FlushReplayPerfFrame(ReplayPerfFrameAccumulator &accumulator,
                          uintptr_t queue_identity,
                          D3D12_COMMAND_LIST_TYPE queue_type);

} // namespace dxmt::d3d12
