#include "d3d12_replay_query_resolve_ops.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {

void
EmitTimestampResolve(::dxmt::CommandQueue &queue, CommandChunk *chunk,
                     PendingTimestampResolve resolve) {
  auto *heap = dynamic_cast<QueryHeap *>(resolve.heap.ptr());
  auto *dst = GetResource(resolve.dst_buffer.ptr());
  if (!heap || !dst || !dst->GetBufferAllocation() || !resolve.query_count)
    return;

  if (resolve.samples.size() < resolve.query_count)
    return;

  std::vector<Rc<TimestampQuery>> retained_queries;
  retained_queries.reserve(resolve.query_count);
  for (const auto &sample : resolve.samples)
    if (sample.query)
      retained_queries.push_back(sample.query);

  Rc<BufferAllocation> allocation = dst->GetBufferAllocation();
  WMT::Reference<WMT::Buffer> dst_buffer(allocation->buffer());
  const uint64_t dst_buffer_offset = dst->GetHeapOffset() + resolve.dst_offset;
  dst->AddPendingTimestampResolve(resolve.dst_offset, resolve.byte_count,
                                  queue.CurrentSeqId());
  for (UINT i = 0; i < resolve.query_count;) {
    const auto &first_sample = resolve.samples[i];
    const uint64_t start_sample = first_sample.index;
    if (start_sample == ~0ull)
      return;
    UINT run_count = 1;
    while (i + run_count < resolve.query_count &&
           resolve.samples[i + run_count].index == start_sample + run_count
#if DXMT_DX12_METAL4
           && resolve.samples[i + run_count].heap == first_sample.heap
           && bool(resolve.samples[i + run_count].heap) == bool(first_sample.heap)
           && resolve.samples[i + run_count].heap_entry_size ==
                  first_sample.heap_entry_size
#endif
    )
      run_count++;

    const uint64_t run_dst_offset =
        dst_buffer_offset + uint64_t(i) * sizeof(uint64_t);
    const uint64_t run_dst_length =
        uint64_t(run_count) * sizeof(uint64_t);
#if DXMT_DX12_METAL4
    if (first_sample.heap) {
      WMT::Reference<WMT::CounterHeap> src_heap(first_sample.heap);
      chunk->emitcc([src_heap = std::move(src_heap), start_sample, run_count,
                     dst_buffer, run_dst_offset,
                     run_dst_length](ArgumentEncodingContext &enc) mutable {
        enc.resolveTimestamp(std::move(src_heap), start_sample, run_count,
                             dst_buffer, run_dst_offset, run_dst_length);
      });
    } else {
      chunk->emitcc([start_sample, run_count, dst_buffer, run_dst_offset,
                     run_dst_length](ArgumentEncodingContext &enc) {
        enc.resolveTimestamp(start_sample, run_count, dst_buffer,
                             run_dst_offset, run_dst_length);
      });
    }
#else
    chunk->emitcc([start_sample, run_count, dst_buffer, run_dst_offset,
                   run_dst_length](ArgumentEncodingContext &enc) {
      enc.resolveTimestamp(start_sample, run_count, dst_buffer,
                           run_dst_offset, run_dst_length);
    });
#endif
    g_timestamp_gpu_resolve_runs.fetch_add(1, std::memory_order_relaxed);
    g_timestamp_gpu_resolve_queries.fetch_add(run_count,
                                              std::memory_order_relaxed);
    dxmt::perf::recordTimestampGpuResolve(run_count);
    i += run_count;
  }
  if (!retained_queries.empty())
    chunk->retainTimestampQueries(std::move(retained_queries));
}

bool
DeferCpuQueryResolveToResource(::dxmt::CommandQueue &queue, uintptr_t queue_id,
                               const ResolveQueryDataRecord &record,
                               QueryResolveSnapshot snapshot,
                               UINT64 byte_count) {
  if (!D3D12QueryCpuFallbackDeferEnabled())
    return false;

  auto *dst = GetResource(record.dst_buffer.ptr());
  if (!dst || !dst->CanDeferCpuQueryResolve())
    return false;

  const auto seq = queue.CurrentSeqId();
  const bool timestamp = record.type == D3D12_QUERY_TYPE_TIMESTAMP;
  auto target = std::make_unique<DeferredCpuQueryResolveTarget>(
      record.command_list_identity, record.heap, record.type,
      record.start_index, record.query_count, record.dst_buffer_offset,
      queue_id, std::move(snapshot));
  if (!dst->AddPendingCpuQueryResolve(
          record.dst_buffer_offset, byte_count, seq, std::move(target)))
    return false;
  if (timestamp) {
    g_timestamp_cpu_deferred_fallbacks.fetch_add(
        1, std::memory_order_relaxed);
    g_timestamp_cpu_deferred_queries.fetch_add(
        record.query_count, std::memory_order_relaxed);
    dxmt::perf::recordTimestampCpuDeferred(record.query_count);
  }
  return true;
}

void
QueueCpuQueryFallback(::dxmt::CommandQueue &queue, uintptr_t queue_id,
                      ReplayState &state, const ResolveQueryDataRecord &record,
                      QueryResolveSnapshot snapshot, UINT64 byte_count,
                      const char *reason) {
  if (record.type == D3D12_QUERY_TYPE_TIMESTAMP) {
    g_timestamp_cpu_fallbacks.fetch_add(1, std::memory_order_relaxed);
    g_timestamp_cpu_fallback_queries.fetch_add(record.query_count,
                                              std::memory_order_relaxed);
    dxmt::perf::recordTimestampCpuFallback(record.query_count);
  }

  if (DeferCpuQueryResolveToResource(queue, queue_id, record, snapshot,
                                     byte_count)) {
    static std::atomic<uint32_t> defer_log_count = 0;
    if (D3D12DiagShouldLog(defer_log_count, D3D12QueryFallbackStatsEnabled())) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData CPU fallback deferred"
           " queue=", queue_id,
           " reason=", reason ? reason : "",
           " start=", record.start_index,
           " count=", record.query_count,
           " bytes=", byte_count,
           " tsCpuDeferredFallbacks=",
           g_timestamp_cpu_deferred_fallbacks.load(std::memory_order_relaxed),
           " tsCpuDeferredQueries=",
           g_timestamp_cpu_deferred_queries.load(std::memory_order_relaxed));
    }
    return;
  }

  if (record.type == D3D12_QUERY_TYPE_TIMESTAMP) {
    g_timestamp_cpu_immediate_fallbacks.fetch_add(1,
                                                 std::memory_order_relaxed);
    g_timestamp_cpu_unsafe_fallbacks.fetch_add(1, std::memory_order_relaxed);
    dxmt::perf::recordTimestampCpuImmediate(true);
  }
  state.pending_immediate_cpu_query_resolves.push_back(
      PendingCpuQueryResolve{record, std::move(snapshot)});
  static std::atomic<uint32_t> immediate_log_count = 0;
  if (D3D12DiagShouldLog(immediate_log_count, D3D12QueryFallbackStatsEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData CPU fallback immediate"
         " queue=", queue_id,
         " reason=", reason ? reason : "",
         " start=", record.start_index,
         " count=", record.query_count,
         " bytes=", byte_count,
         " tsCpuImmediateFallbacks=",
         g_timestamp_cpu_immediate_fallbacks.load(std::memory_order_relaxed),
         " tsCpuUnsafeFallbacks=",
         g_timestamp_cpu_unsafe_fallbacks.load(std::memory_order_relaxed));
  }
}

} // namespace dxmt::d3d12
