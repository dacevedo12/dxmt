#include "d3d12_replay_query_records.hpp"

#include "d3d12_indirect_topology.hpp"
#include "d3d12_query_resolve_policy.hpp"
#include "d3d12_query_resolve_range.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_replay_pass_batch_ops.hpp"
#include "d3d12_replay_pass_flush_ops.hpp"
#include "d3d12_replay_query_resolve_ops.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_context.hpp"
#include "log/log.hpp"

#include <atomic>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {

namespace {

// Rewinds the per-submission sample counter once the DXMT queue has moved on to
// a newer submission; sample indices are only unique within one submission.
void
SyncTimestampSampleAllocator(uint64_t current_sequence,
                             uint64_t &sample_sequence,
                             uint64_t &sample_count) {
  if (sample_sequence == current_sequence)
    return;

  sample_sequence = current_sequence;
  sample_count = 0;
}

} // namespace

uint64_t
AllocateTimestampSample(uint64_t current_sequence, uint64_t &sample_sequence,
                        uint64_t &sample_count, TimestampQuery *query) {
  SyncTimestampSampleAllocator(current_sequence, sample_sequence, sample_count);
#if DXMT_DX12_METAL4
  if (query->resolveHeap()) {
    query->setSampleSequence(sample_sequence);
    return query->sampleIndex();
  }
#endif
  const auto sample_index = sample_count++;
  query->setSampleLocation(sample_sequence, sample_index);
  return sample_index;
}

void
ReplayBeginQuery(CommandChunk *chunk, const BeginQueryRecord &record) {
  auto *heap = dynamic_cast<QueryHeap *>(record.heap.ptr());
  if (!heap) {
    WARN("D3D12CommandQueue: BeginQuery skipped for foreign query heap");
    return;
  }
  if (record.type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||
      record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 ||
      record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1 ||
      record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2 ||
      record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
    heap->BeginStatistics(record.type, record.index);
    return;
  }
  auto query = heap->BeginVisibility(record.type, record.index);
  if (!query)
    return;
  chunk->emitcc([query = std::move(query)](
                    ArgumentEncodingContext &enc) mutable {
    enc.beginVisibilityResultQuery(std::move(query));
  });
}

void
QueueReplayTimestampMarker(CommandChunk *chunk, ReplayState &state,
                           const EndQueryRecord &record,
                           Rc<TimestampQuery> query) {
  if (D3D12DeferredTimestampMarkersEnabled() &&
      (HasPendingGraphicsPass(state) || HasPendingComputePass(state))) {
    state.pending_timestamp_markers.push_back(PendingTimestampMarker{
        record.heap, std::move(query), record.type, record.index});
    return;
  }
  state.pending_timestamp_markers.push_back(PendingTimestampMarker{
      record.heap, std::move(query), record.type, record.index});
  EmitTimestampMarkers(chunk, state);
}

void
ReplayEndNonTimestampQuery(CommandChunk *chunk, QueryHeap &heap,
                           const EndQueryRecord &record) {
  if (record.type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||
      record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 ||
      record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1 ||
      record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2 ||
      record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
    heap.EndStatistics(record.type, record.index);
    return;
  }
  auto query = heap.EndVisibility(record.type, record.index);
  if (!query)
    return;
  chunk->emitcc([query = std::move(query)](
                    ArgumentEncodingContext &enc) mutable {
    enc.endVisibilityResultQuery(std::move(query));
  });
}

void
ReplayResolveQueryData(::dxmt::CommandQueue &queue, uintptr_t queue_id,
                       D3D12_COMMAND_LIST_TYPE queue_type, ReplayState &state,
                       const ResolveQueryDataRecord &record) {
  auto *heap = dynamic_cast<QueryHeap *>(record.heap.ptr());
  if (!heap) {
    WARN("D3D12CommandQueue: ResolveQueryData skipped for foreign query heap");
    return;
  }

  static std::atomic<uint32_t> query_log_count = 0;
  const bool query_diag_enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_D3D12_QUERY") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
  if (D3D12DiagShouldLog(query_log_count, query_diag_enabled)) {
    const auto &desc = heap->GetDesc();
    WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData enter"
         " queue=", queue_id,
         " queueType=", queue_type,
         " heap=", reinterpret_cast<uintptr_t>(heap),
         " heapType=", desc.Type,
         " heapCount=", desc.Count,
         " queryType=", record.type,
         " start=", record.start_index,
         " count=", record.query_count,
         " dst=", reinterpret_cast<uintptr_t>(record.dst_buffer.ptr()),
         " dstOffset=", record.dst_buffer_offset);
  }

  QueryResolveSnapshot snapshot;
  std::vector<uint8_t> sizing_data;
  if (!heap->CaptureResolve(record.type, record.start_index,
                            record.query_count, snapshot) ||
      !ResolveQuerySnapshot(snapshot, sizing_data)) {
    if (D3D12DiagShouldLog(query_log_count, query_diag_enabled)) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData sizing failed"
           " queue=", queue_id,
           " queryType=", record.type,
           " start=", record.start_index,
           " count=", record.query_count);
    }
    return;
  }
  if (D3D12DiagShouldLog(query_log_count, query_diag_enabled)) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData sizing ok"
         " queue=", queue_id,
         " bytes=", sizing_data.size());
  }
  auto *dst = GetResource(record.dst_buffer.ptr());
  if (!ValidateBufferRange(dst, record.dst_buffer_offset, sizing_data.size(),
                           "query resolve"))
    return;

  if (D3D12TimestampGpuResolveEnabled() &&
      record.type == D3D12_QUERY_TYPE_TIMESTAMP &&
      dst->GetBufferAllocation() &&
      sizing_data.size() == UINT64(record.query_count) * sizeof(uint64_t)) {
    std::vector<PendingTimestampResolve::Sample> samples;
    samples.reserve(record.query_count);
    std::vector<bool> can_gpu_resolve;
    can_gpu_resolve.reserve(record.query_count);
    const auto current_sequence = queue.CurrentSeqId();
    for (UINT i = 0; i < record.query_count; i++) {
      const auto &query = snapshot.entries[i].timestamp;
      PendingTimestampResolve::Sample sample = {};
      uint64_t sequence = ~0ull;
      if (query) {
        sample.query = query;
        sample.index = query->sampleIndex();
        sequence = query->sampleSequence();
#if DXMT_DX12_METAL4
        sample.heap = query->resolveHeap();
        sample.heap_entry_size = query->resolveHeapEntrySize();
#endif
      }
      can_gpu_resolve.push_back(
          CanGpuResolveTimestampSample(sample, sequence, current_sequence));
      samples.push_back(std::move(sample));
    }

    bool emitted_any_run = false;
    for (UINT i = 0; i < record.query_count;) {
      const bool gpu_run = can_gpu_resolve[i];
      UINT run_count = 1;
      while (i + run_count < record.query_count &&
             can_gpu_resolve[i + run_count] == gpu_run)
        run_count++;

      const UINT run_start = record.start_index + i;
      auto run_record = SliceResolveRecord(record, run_start, run_count,
                                           QueryResultStride(record.type));
      const UINT64 run_bytes = UINT64(run_count) * sizeof(uint64_t);
      if (gpu_run) {
        std::vector<PendingTimestampResolve::Sample> run_samples;
        run_samples.reserve(run_count);
        for (UINT j = 0; j < run_count; j++)
          run_samples.push_back(samples[i + j]);
        state.pending_timestamp_resolves.push_back(PendingTimestampResolve{
            record.command_list_identity, record.heap, record.dst_buffer,
            std::move(run_samples), run_start, run_count,
            run_record.dst_buffer_offset, run_bytes});
        emitted_any_run = true;
      } else {
        auto run_snapshot = SliceResolveSnapshot(snapshot, i, run_count);
        QueueCpuQueryFallback(queue, queue_id, state, run_record,
                              std::move(run_snapshot), run_bytes,
                              "timestamp-missing-gpu-resolve-source");
      }
      i += run_count;
    }

    if (emitted_any_run &&
        D3D12DiagShouldLog(query_log_count, query_diag_enabled)) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData split timestamp"
           " queue=", queue_id,
           " start=", record.start_index,
           " count=", record.query_count,
           " bytes=", sizing_data.size(),
           " tsGpuRuns=", g_timestamp_gpu_resolve_runs.load(std::memory_order_relaxed),
           " tsCpuFallbacks=",
           g_timestamp_cpu_fallbacks.load(std::memory_order_relaxed),
           " tsCpuDeferredFallbacks=",
           g_timestamp_cpu_deferred_fallbacks.load(std::memory_order_relaxed),
           " tsCpuImmediateFallbacks=",
           g_timestamp_cpu_immediate_fallbacks.load(std::memory_order_relaxed));
    }
    return;
  }

  QueueCpuQueryFallback(queue, queue_id, state, record, std::move(snapshot),
                        sizing_data.size(), "non-gpu-query");
  if (D3D12DiagShouldLog(query_log_count, query_diag_enabled)) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData deferred CPU fallback"
         " queue=", queue_id,
         " start=", record.start_index,
         " count=", record.query_count,
         " bytes=", sizing_data.size());
  }
}

} // namespace dxmt::d3d12
