#pragma once

// Test-telemetry accounting for one Execute submission's descriptor capture.
//
// The record and span stores may be shared across submissions, so every figure
// reported here is a delta against a sample taken before the capture started.
// This was open-coded at the head and tail of
// CaptureSubmittedDescriptorSnapshots() in
// d3d12_command_queue_pass_queue.inc; it only reads the promoted store types
// and writes the caller-supplied telemetry block, never the queue.

#include "d3d12_command_list.hpp"
#include "d3d12_replay_binding_types.hpp"

#include <cstddef>
#include <cstdint>

namespace dxmt::d3d12 {

/** Sample of the shared capture stores taken before a submission runs. */
struct SubmittedDescriptorCaptureCounters {
  size_t records = 0;
  uint32_t record_reuses = 0;
  uint64_t span_lookups = 0;
  size_t spans = 0;
  uint64_t span_reuses = 0;
  uint64_t frozen_direct_packets = 0;
  uint64_t frozen_range_lookups = 0;
  size_t frozen_ranges = 0;
  uint64_t frozen_range_reuses = 0;
  uint64_t frozen_root_word_lookups = 0;
  uint64_t frozen_root_word_reuses = 0;
};

/** Takes the "before" sample. The frozen-native figures read as zero when the
 *  span store has no frozen table yet, which matches a store that gains one
 *  during this submission. */
[[nodiscard]] SubmittedDescriptorCaptureCounters
SampleSubmittedDescriptorCaptureCounters(
    const SubmittedDescriptorRecordStore &records,
    const SubmittedNativeDescriptorSpanStore &spans);

/** Counts one captured packet snapshot and its binding entries. Called per
 *  packet, so snapshots shared by several packets are counted several times;
 *  the unique count comes from
 *  RecordSubmittedDescriptorCaptureTelemetry(). */
void RecordSubmittedDescriptorSnapshotTelemetry(
    CompiledCommandTestTelemetry &telemetry,
    const GraphicsBindingSnapshot &snapshot);

/** Reports what this submission added to the shared stores, relative to
 *  `before`. */
void RecordSubmittedDescriptorCaptureTelemetry(
    CompiledCommandTestTelemetry &telemetry,
    const CompiledCommandDescriptorSnapshots &snapshots,
    const SubmittedDescriptorRecordStore &records,
    const SubmittedNativeDescriptorSpanStore &spans,
    const SubmittedDescriptorCaptureCounters &before);

} // namespace dxmt::d3d12
