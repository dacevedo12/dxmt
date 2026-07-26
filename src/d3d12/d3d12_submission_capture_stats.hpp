#pragma once

// Per-submit capture-side counters carried from ExecuteCommandLists to the
// submission worker.
//
// Extracted from class CommandQueueImpl
// (d3d12_command_queue_submission.inc). The struct is filled while the calling
// thread captures descriptor spans / frozen native ranges, then folded into the
// worker's replay frame statistics when the Execute operation drains.

#include <cstdint>

namespace dxmt {
struct FrameStatistics;
}

namespace dxmt::d3d12 {

struct SubmissionCaptureStatistics final {
  uint64_t generic_descriptor_span_lookups = 0;
  uint64_t generic_descriptor_span_unique = 0;
  uint64_t generic_descriptor_span_reuses = 0;
  uint64_t frozen_native_direct_packets = 0;
  uint64_t frozen_range_lookups = 0;
  uint64_t frozen_range_unique = 0;
  uint64_t frozen_range_reuses = 0;
  uint64_t frozen_root_word_lookups = 0;
  uint64_t frozen_root_word_reuses = 0;
};

// Adds every capture counter into the matching frame_* accumulator of `stats`.
void AccumulateSubmissionCaptureStatistics(
    dxmt::FrameStatistics &stats, const SubmissionCaptureStatistics &capture);

} // namespace dxmt::d3d12
