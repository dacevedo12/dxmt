#include "d3d12_replay_perf_frame.hpp"

namespace dxmt::d3d12 {

dxmt::FrameStatistics *
BeginReplayPerfFrame(ReplayPerfFrameAccumulator &accumulator,
                     uint64_t frame_id, uintptr_t queue_identity,
                     D3D12_COMMAND_LIST_TYPE queue_type) {
  if (!dxmt::perf::enabled())
    return nullptr;
  if (accumulator.frame_id != frame_id) {
    FlushReplayPerfFrame(accumulator, queue_identity, queue_type);
    accumulator.stats.reset();
    accumulator.frame_id = frame_id;
  }
  accumulator.execute_batches++;
  return &accumulator.stats;
}

void
FlushReplayPerfFrame(ReplayPerfFrameAccumulator &accumulator,
                     uintptr_t queue_identity,
                     D3D12_COMMAND_LIST_TYPE queue_type) {
  if (accumulator.frame_id == ReplayPerfFrameAccumulator::kNoReplayPerfFrame ||
      !accumulator.execute_batches)
    return;
  dxmt::perf::recordReplayWorkerFrame(accumulator.frame_id, queue_identity,
                                      queue_type, accumulator.execute_batches,
                                      accumulator.stats);
  accumulator.stats.reset();
  accumulator.frame_id = ReplayPerfFrameAccumulator::kNoReplayPerfFrame;
  accumulator.execute_batches = 0;
}

} // namespace dxmt::d3d12
