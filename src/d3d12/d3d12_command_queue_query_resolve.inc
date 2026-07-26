// Timestamp sample allocation and query resolve materialization.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // Thin adapter over dxmt::d3d12::AllocateTimestampSample(); only supplies the
  // current submission sequence and the queue-owned allocator cursor. Kept
  // because d3d12_command_queue_replay_state_ops.inc still calls the
  // one-argument form. SyncTimestampSampleAllocator() folded into it.
  uint64_t AllocateTimestampSample(TimestampQuery *query) {
    return dxmt::d3d12::AllocateTimestampSample(
        device_->GetDXMTDevice().queue().CurrentSeqId(),
        current_timestamp_sample_sequence_, current_timestamp_sample_count_,
        query);
  }

  // BufferRangesOverlap() now lives in d3d12_query_resolve_range.hpp;
  // unqualified calls below resolve to dxmt::d3d12.

  // EmitTimestampResolve() now lives in d3d12_replay_query_resolve_ops.hpp; it
  // takes the DXMT queue explicitly, so callers pass
  // device_->GetDXMTDevice().queue().

  bool MaterializeTimestampResolves(CommandChunk *chunk, ReplayState &state,
                                    ID3D12Resource *resource = nullptr,
                                    UINT64 offset = 0, UINT64 size = UINT64_MAX) {
    if (state.pending_timestamp_resolves.empty())
      return false;

    FlushPassBatches(chunk, state);
    auto matches = [&](const PendingTimestampResolve &resolve) {
      if (!resource)
        return true;
      return resolve.dst_buffer.ptr() == resource &&
             BufferRangesOverlap(resolve.dst_offset, resolve.byte_count,
                                 offset, size);
    };

    std::vector<PendingTimestampResolve> remaining;
    remaining.reserve(state.pending_timestamp_resolves.size());
    bool emitted = false;
    for (auto &resolve : state.pending_timestamp_resolves) {
      if (matches(resolve)) {
        EmitTimestampResolve(device_->GetDXMTDevice().queue(), chunk,
                             std::move(resolve));
        emitted = true;
      } else {
        remaining.push_back(std::move(resolve));
      }
    }
    state.pending_timestamp_resolves = std::move(remaining);
    return emitted;
  }

  // ResolveRecordTouchesRange() now lives in d3d12_query_resolve_policy.hpp;
  // unqualified calls below resolve to dxmt::d3d12.

  void ResolveQueryDataToCpuBuffer(const ResolveQueryDataRecord &record,
                                   const QueryResolveSnapshot &snapshot,
                                   const char *context) {
    ResolveQueryDataToCpuBufferStatic(record, snapshot, context,
                                      reinterpret_cast<uintptr_t>(this));
  }

  // CanGpuResolveTimestampSample() now lives in
  // d3d12_query_resolve_policy.hpp; unqualified calls resolve to dxmt::d3d12.

  // Thin adapter over dxmt::d3d12::SliceResolveRecord(); only supplies the
  // per-query result stride, which dxmt::d3d12::QueryResultStride() computes.
  // Kept because d3d12_command_queue_replay_state_ops.inc still calls the
  // three-argument form.
  static ResolveQueryDataRecord
  SliceResolveRecord(const ResolveQueryDataRecord &record, UINT start_index,
                     UINT query_count) {
    return dxmt::d3d12::SliceResolveRecord(record, start_index, query_count,
                                           QueryResultStride(record.type));
  }

  // SliceResolveSnapshot() now lives in d3d12_query_resolve_range.hpp;
  // unqualified calls resolve to dxmt::d3d12.

  // DeferCpuQueryResolveToResource() now lives in
  // d3d12_replay_query_resolve_ops.hpp; unqualified calls resolve to
  // dxmt::d3d12.

  // Thin adapter over dxmt::d3d12::QueueCpuQueryFallback(); only supplies the
  // DXMT queue and this queue's diagnostic identity. Kept because
  // d3d12_command_queue_replay_state_ops.inc still calls the five-argument
  // form.
  void QueueCpuQueryFallback(ReplayState &state,
                             const ResolveQueryDataRecord &record,
                             QueryResolveSnapshot snapshot,
                             UINT64 byte_count, const char *reason) {
    dxmt::d3d12::QueueCpuQueryFallback(
        device_->GetDXMTDevice().queue(), reinterpret_cast<uintptr_t>(this),
        state, record, std::move(snapshot), byte_count, reason);
  }

  bool MaterializeCpuQueryResolves(CommandChunk *&chunk, ReplayState &state,
                                   ID3D12Resource *resource = nullptr,
                                   UINT64 offset = 0,
                                   UINT64 size = UINT64_MAX) {
    if (state.pending_immediate_cpu_query_resolves.empty())
      return false;

    std::vector<PendingCpuQueryResolve> selected;
    std::vector<PendingCpuQueryResolve> remaining;
    selected.reserve(state.pending_immediate_cpu_query_resolves.size());
    remaining.reserve(state.pending_immediate_cpu_query_resolves.size());

    for (auto &resolve : state.pending_immediate_cpu_query_resolves) {
      if (ResolveRecordTouchesRange(resolve.record, resource, offset, size))
        selected.push_back(std::move(resolve));
      else
        remaining.push_back(std::move(resolve));
    }

    state.pending_immediate_cpu_query_resolves = std::move(remaining);
    if (selected.empty())
      return false;

    FlushPassBatches(chunk, state);
    auto &queue = device_->GetDXMTDevice().queue();
    const auto seq = queue.CurrentSeqId();
    const bool query_wait_diag =
        D3D12DiagEnabledEnv("DXMT_DIAG_D3D12_QUERY") ||
        D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
    if (query_wait_diag) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData batch commit before wait"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " count=", selected.size(),
           " seq=", seq,
           " coherent=", queue.CoherentSeqId());
    }
    queue.CommitCurrentChunk();
    if (query_wait_diag) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData batch wait begin"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " count=", selected.size(),
           " seq=", seq,
           " coherent=", queue.CoherentSeqId());
    }
    const auto wait_begin = std::chrono::steady_clock::now();
    queue.WaitCPUFence(seq);
    const auto wait_end = std::chrono::steady_clock::now();
    const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             wait_end - wait_begin)
                             .count();
    if (wait_us > 0)
      g_timestamp_cpu_wait_us.fetch_add(uint64_t(wait_us),
                                        std::memory_order_relaxed);
    dxmt::perf::recordTimestampCpuWait(uint64_t(wait_us));
    dxmt::perf::recordQueryBatchWait(
        1, selected.size(), uint64_t(wait_us));
    if (query_wait_diag) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData batch wait end"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " count=", selected.size(),
           " seq=", seq,
           " coherent=", queue.CoherentSeqId(),
           " waitUs=", wait_us,
           " tsGpuRuns=", g_timestamp_gpu_resolve_runs.load(std::memory_order_relaxed),
           " tsGpuQueries=", g_timestamp_gpu_resolve_queries.load(std::memory_order_relaxed),
           " tsCpuFallbacks=", g_timestamp_cpu_fallbacks.load(std::memory_order_relaxed),
           " tsCpuFallbackQueries=", g_timestamp_cpu_fallback_queries.load(std::memory_order_relaxed),
           " tsCpuWaitUs=", g_timestamp_cpu_wait_us.load(std::memory_order_relaxed));
    }

    for (const auto &resolve : selected)
      ResolveQueryDataToCpuBuffer(resolve.record, resolve.snapshot,
                                  "cpu-batch");

    chunk = queue.CurrentChunk();
    return true;
  }

  void MaterializeTimestampResolvesForAccess(CommandChunk *chunk,
                                             ReplayState &state,
                                             ID3D12Resource *resource,
                                             D3D12_RESOURCE_STATES desired) {
    auto *d3d_resource = GetResource(resource);
    if (!d3d_resource || !d3d_resource->GetBufferAllocation())
      return;

    const bool reads = IsReadOnlyResourceState(desired) ||
                       desired == D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT ||
                       desired == D3D12_RESOURCE_STATE_PREDICATION ||
                       desired == D3D12_RESOURCE_STATE_COPY_SOURCE;
    const bool writes = !IsReadOnlyResourceState(desired) &&
                        desired != D3D12_RESOURCE_STATE_PREDICATION &&
                        desired != D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT &&
                        desired != D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (!reads && !writes)
      return;

    MaterializeTimestampResolves(chunk, state, resource, 0,
                                 d3d_resource->GetResourceDesc().Width);
  }

  void MaterializeTimestampResolvesForCpuRead(CommandChunk *&chunk,
                                              ReplayState &state,
                                              ID3D12Resource *resource,
                                              UINT64 offset, UINT64 size) {
    const bool emitted_gpu =
        MaterializeTimestampResolves(chunk, state, resource, offset, size);
    const bool emitted_cpu =
        MaterializeCpuQueryResolves(chunk, state, resource, offset, size);
    auto *d3d_resource = GetResource(resource);
    bool materialized_deferred_cpu = false;
    if (d3d_resource &&
        d3d_resource->HasPendingCpuQueryResolves(offset, size)) {
      FlushPassBatches(chunk, state);
      auto &queue = device_->GetDXMTDevice().queue();
      queue.CommitCurrentChunk();
      materialized_deferred_cpu =
          d3d_resource->MaterializePendingCpuQueryResolves(offset, size,
                                                           "cpu-read");
      chunk = queue.CurrentChunk();
    }
    if (emitted_gpu && !emitted_cpu && !materialized_deferred_cpu) {
      auto &queue = device_->GetDXMTDevice().queue();
      const auto seq = queue.CurrentSeqId();
      queue.CommitCurrentChunk();
      queue.WaitCPUFence(seq);
      chunk = queue.CurrentChunk();
    }
  }

  void MaterializeDeferredCpuQueryResolvesForGpuAccess(CommandChunk *&chunk,
                                                       ReplayState &state,
                                                       ID3D12Resource *resource,
                                                       UINT64 offset,
                                                       UINT64 size) {
    auto *d3d_resource = GetResource(resource);
    if (!d3d_resource ||
        !d3d_resource->HasPendingCpuQueryResolves(offset, size))
      return;

    FlushPassBatches(chunk, state);
    auto &queue = device_->GetDXMTDevice().queue();
    queue.CommitCurrentChunk();
    d3d_resource->MaterializePendingCpuQueryResolves(offset, size,
                                                     "gpu-access");
    chunk = queue.CurrentChunk();
  }

