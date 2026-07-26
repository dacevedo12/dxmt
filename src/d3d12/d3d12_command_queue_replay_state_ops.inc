// Root descriptor/constant state, predication, and query replay.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // ResetReplayApiState() now lives in d3d12_replay_api_state_reset.hpp;
  // unqualified calls below resolve to dxmt::d3d12. Its body only wrote fields
  // of the ReplayState it was handed, and its one queue-side dependency,
  // BumpGraphicsBindingGeneration(), is now the namespace-level free function
  // in d3d12_replay_pass_batch_ops.hpp.

  bool ReadBufferBytesForCpuReplay(CommandChunk *&chunk, ReplayState &state,
                                   ID3D12Resource *resource, UINT64 offset,
                                   void *dst, UINT64 size,
                                   const char *context) {
    auto *d3d12_resource = GetResource(resource);
    if (!d3d12_resource || !d3d12_resource->GetBufferAllocation() || !dst)
      return false;
    if (offset > d3d12_resource->GetResourceDesc().Width ||
        size > d3d12_resource->GetResourceDesc().Width - offset) {
      WARN("D3D12CommandQueue: ", context, " read exceeds buffer bounds");
      return false;
    }

    auto allocation = Rc<BufferAllocation>(
        d3d12_resource->GetBufferAllocation());
    if (auto *mapped = allocation->mappedMemory(0)) {
      std::memcpy(dst,
                  static_cast<const char *>(mapped) +
                      d3d12_resource->GetHeapOffset() + offset,
                  size);
      return true;
    }

    WMTBufferInfo info = {};
    info.length = size;
    info.options = WMTResourceStorageModeShared |
                   WMTResourceHazardTrackingModeUntracked;
    info.memory.set(nullptr);
#ifdef __i386__
    info.memory.set(wsi::aligned_malloc(size, DXMT_PAGE_SIZE));
#endif
    auto staging = device_->GetMTLDevice().newBuffer(info);
    auto *staging_mapped = info.memory.get_accessible_or_null();
    if (!staging || !staging_mapped) {
#ifdef __i386__
      wsi::aligned_free(info.memory.get_accessible_or_null());
#endif
      WARN("D3D12CommandQueue: ", context,
           " failed to allocate a CPU readback buffer");
      return false;
    }

    FlushPassBatches(chunk, state);
    const UINT64 source_offset =
        d3d12_resource->GetHeapOffset() + offset;
    chunk->emitcc(
        [allocation = std::move(allocation),
         staging = WMT::Reference<WMT::Buffer>(staging), source_offset,
         size](ArgumentEncodingContext &enc) {
          RetainDirectBufferForGpu(enc, staging);
          enc.retainAllocation(allocation.ptr());
          enc.startBlitPass();
          auto &copy =
              enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          copy.type = WMTBlitCommandCopyFromBufferToBuffer;
          copy.src = allocation->buffer();
          copy.src_offset = source_offset;
          copy.dst = staging;
          copy.dst_offset = 0;
          copy.copy_length = size;
          enc.endPass();
        });

    auto &queue = device_->GetDXMTDevice().queue();
    const auto sequence = queue.CurrentSeqId();
    queue.CommitCurrentChunk();
    queue.WaitCPUFence(sequence);
    chunk = queue.CurrentChunk();
    std::memcpy(dst, staging_mapped, size);
#ifdef __i386__
    wsi::aligned_free(staging_mapped);
#endif
    return true;
  }

  bool PredicationAllows(CommandChunk *&chunk, ReplayState &state) {
    if (!state.predication_buffer)
      return true;

    MaterializeTimestampResolvesForCpuRead(
        chunk, state, state.predication_buffer.ptr(),
        state.predication_buffer_offset, sizeof(uint64_t));
    uint64_t value = 0;
    if (!ReadBufferBytesForCpuReplay(
            chunk, state, state.predication_buffer.ptr(),
            state.predication_buffer_offset, &value, sizeof(value),
            "predication")) {
      WARN("D3D12CommandQueue: predication buffer is unavailable; command will be skipped");
      return false;
    }

    return PredicationValuePasses(state.predication_operation, value);
  }

  // Thin adapter over dxmt::d3d12::ReplayBeginQuery(); only supplies the pass
  // flush, which stays here because FlushPassBatches() is still a
  // CommandQueueImpl member (d3d12_command_queue_pass_batching.inc).
  void ReplayBeginQuery(CommandChunk *chunk, ReplayState &state,
                        const BeginQueryRecord &record) {
    FlushPassBatches(chunk, state);
    dxmt::d3d12::ReplayBeginQuery(chunk, record);
  }

  // Only the timestamp branch stays here: it needs the queue's sample allocator
  // and a second FlushPassBatches(). QueueReplayTimestampMarker() and
  // ReplayEndNonTimestampQuery() live in d3d12_replay_query_records.hpp.
  void ReplayEndQuery(CommandChunk *chunk, ReplayState &state,
                      const EndQueryRecord &record) {
    FlushPassBatches(chunk, state);
    auto *heap = dynamic_cast<QueryHeap *>(record.heap.ptr());
    if (!heap) {
      WARN("D3D12CommandQueue: EndQuery skipped for foreign query heap");
      return;
    }
    if (record.type == D3D12_QUERY_TYPE_TIMESTAMP) {
      auto query = heap->EndTimestamp(record.type, record.index);
      if (!query)
        return;

      if (!D3D12DeferredTimestampMarkersEnabled())
        FlushPassBatches(chunk, state);

      AllocateTimestampSample(query.ptr());
      QueueReplayTimestampMarker(chunk, state, record, std::move(query));
      return;
    }
    dxmt::d3d12::ReplayEndNonTimestampQuery(chunk, *heap, record);
  }

  // Thin adapter over dxmt::d3d12::ReplayResolveQueryData(); only supplies the
  // DXMT queue, this queue's diagnostic identity and its list type. The chunk
  // parameter is unused, but kept so d3d12_command_queue_replay_records.inc
  // still dispatches every record handler uniformly.
  void ReplayResolveQueryData(CommandChunk *chunk, ReplayState &state,
                              const ResolveQueryDataRecord &record) {
    dxmt::d3d12::ReplayResolveQueryData(
        device_->GetDXMTDevice().queue(), reinterpret_cast<uintptr_t>(this),
        desc_.Type, state, record);
  }

