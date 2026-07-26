#include "d3d12_submitted_descriptor_capture_telemetry.hpp"

#include <unordered_set>

namespace dxmt::d3d12 {

SubmittedDescriptorCaptureCounters SampleSubmittedDescriptorCaptureCounters(
    const SubmittedDescriptorRecordStore &records,
    const SubmittedNativeDescriptorSpanStore &spans) {
  SubmittedDescriptorCaptureCounters counters;
  counters.records = records.records.size();
  counters.record_reuses = records.reuse_count;
  counters.span_lookups = spans.lookup_count;
  counters.spans = spans.spans.size();
  counters.span_reuses = spans.reuse_count;
  counters.frozen_direct_packets =
      spans.frozen_native ? spans.frozen_native->direct_packet_count : 0;
  counters.frozen_range_lookups =
      spans.frozen_native ? spans.frozen_native->range_lookups : 0;
  counters.frozen_ranges =
      spans.frozen_native ? spans.frozen_native->range_bases.size() : 0;
  counters.frozen_range_reuses =
      spans.frozen_native ? spans.frozen_native->range_reuses : 0;
  counters.frozen_root_word_lookups =
      spans.frozen_native ? spans.frozen_native->root_word_lookups : 0;
  counters.frozen_root_word_reuses =
      spans.frozen_native ? spans.frozen_native->root_word_reuses : 0;
  return counters;
}

void RecordSubmittedDescriptorSnapshotTelemetry(
    CompiledCommandTestTelemetry &telemetry,
    const GraphicsBindingSnapshot &snapshot) {
  telemetry.submitted_descriptor_snapshots.fetch_add(
      1, std::memory_order_relaxed);
  telemetry.submitted_descriptor_entries.fetch_add(
      static_cast<UINT>(SnapshotBindingEntryCount(snapshot)),
      std::memory_order_relaxed);
}

void RecordSubmittedDescriptorCaptureTelemetry(
    CompiledCommandTestTelemetry &telemetry,
    const CompiledCommandDescriptorSnapshots &snapshots,
    const SubmittedDescriptorRecordStore &records,
    const SubmittedNativeDescriptorSpanStore &spans,
    const SubmittedDescriptorCaptureCounters &before) {
  std::unordered_set<const GraphicsBindingSnapshot *> unique_snapshots;
  for (const auto &snapshot : snapshots.graphics)
    if (snapshot)
      unique_snapshots.insert(snapshot.get());
  for (const auto &snapshot : snapshots.compute)
    if (snapshot)
      unique_snapshots.insert(snapshot.get());
  telemetry.submitted_unique_descriptor_snapshots.fetch_add(
      static_cast<UINT>(unique_snapshots.size()), std::memory_order_relaxed);
  telemetry.submitted_unique_descriptor_records.fetch_add(
      static_cast<UINT>(records.records.size() - before.records),
      std::memory_order_relaxed);
  telemetry.submitted_descriptor_record_reuses.fetch_add(
      records.reuse_count - before.record_reuses, std::memory_order_relaxed);
  telemetry.generic_descriptor_span_lookups.fetch_add(
      spans.lookup_count - before.span_lookups, std::memory_order_relaxed);
  telemetry.generic_descriptor_span_unique.fetch_add(
      static_cast<UINT>(spans.spans.size() - before.spans),
      std::memory_order_relaxed);
  telemetry.generic_descriptor_span_reuses.fetch_add(
      spans.reuse_count - before.span_reuses, std::memory_order_relaxed);
  if (spans.frozen_native) {
    const auto &frozen = *spans.frozen_native;
    telemetry.frozen_native_direct_packets.fetch_add(
        frozen.direct_packet_count - before.frozen_direct_packets,
        std::memory_order_relaxed);
    telemetry.frozen_range_lookups.fetch_add(
        frozen.range_lookups - before.frozen_range_lookups,
        std::memory_order_relaxed);
    telemetry.frozen_range_unique.fetch_add(
        static_cast<UINT>(frozen.range_bases.size() - before.frozen_ranges),
        std::memory_order_relaxed);
    telemetry.frozen_range_reuses.fetch_add(
        frozen.range_reuses - before.frozen_range_reuses,
        std::memory_order_relaxed);
    telemetry.frozen_root_word_lookups.fetch_add(
        frozen.root_word_lookups - before.frozen_root_word_lookups,
        std::memory_order_relaxed);
    telemetry.frozen_root_word_reuses.fetch_add(
        frozen.root_word_reuses - before.frozen_root_word_reuses,
        std::memory_order_relaxed);
  }
}

} // namespace dxmt::d3d12
