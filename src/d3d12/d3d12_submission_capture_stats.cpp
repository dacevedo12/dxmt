#include "d3d12_submission_capture_stats.hpp"

#include "dxmt_statistics.hpp"

namespace dxmt::d3d12 {

void
AccumulateSubmissionCaptureStatistics(
    dxmt::FrameStatistics &stats, const SubmissionCaptureStatistics &capture) {
  stats.frame_generic_descriptor_span_lookups +=
      capture.generic_descriptor_span_lookups;
  stats.frame_generic_descriptor_span_unique +=
      capture.generic_descriptor_span_unique;
  stats.frame_generic_descriptor_span_reuses +=
      capture.generic_descriptor_span_reuses;
  stats.frame_frozen_native_direct_packets +=
      capture.frozen_native_direct_packets;
  stats.frame_frozen_range_lookups += capture.frozen_range_lookups;
  stats.frame_frozen_range_unique += capture.frozen_range_unique;
  stats.frame_frozen_range_reuses += capture.frozen_range_reuses;
  stats.frame_frozen_root_word_lookups += capture.frozen_root_word_lookups;
  stats.frame_frozen_root_word_reuses += capture.frozen_root_word_reuses;
}

} // namespace dxmt::d3d12
