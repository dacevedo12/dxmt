// Per-record replay dispatch, resource access tracking, and barriers.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  template <typename T>
  void ReplayRecord(CommandChunk *chunk, ReplayState &state, const T &record) {
      if constexpr (std::is_same_v<T, CopyBufferRegionRecord>) {
        FlushReplayRenderComputeTimestampBatches(chunk, state);
        RecordReplayResourceAccess(chunk, state, record.dst.ptr(),
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   "CopyBufferRegion dst");
        RecordReplayResourceAccess(chunk, state, record.src.ptr(),
                                   D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   "CopyBufferRegion src");
        ReplayCopyBufferRegion(chunk, state, record);
      } else if constexpr (std::is_same_v<T, CopyTextureRegionRecord>) {
        FlushReplayRenderComputeTimestampBatches(chunk, state);
        RecordReplayResourceAccess(
            chunk, state, record.dst.resource.ptr(), D3D12_RESOURCE_STATE_COPY_DEST,
            "CopyTextureRegion dst",
            record.dst.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
                ? record.dst.subresource_index
                : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        RecordReplayResourceAccess(
            chunk, state, record.src.resource.ptr(), D3D12_RESOURCE_STATE_COPY_SOURCE,
            "CopyTextureRegion src",
            record.src.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
                ? record.src.subresource_index
                : D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
        ReplayCopyTextureRegion(chunk, state, record);
      } else if constexpr (std::is_same_v<T, CopyResourceRecord>) {
        FlushReplayRenderComputeTimestampBatches(chunk, state);
        RecordReplayResourceAccess(chunk, state, record.dst.ptr(),
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   "CopyResource dst");
        RecordReplayResourceAccess(chunk, state, record.src.ptr(),
                                   D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   "CopyResource src");
        ReplayCopyResource(chunk, state, record);
      } else if constexpr (std::is_same_v<T, CopyTilesRecord>) {
        FlushReplayRenderComputeTimestampBatches(chunk, state);
        const bool buffer_to_texture =
            record.flags & D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE;
        RecordReplayResourceAccess(
            chunk, state, record.tiled_resource.ptr(),
            buffer_to_texture ? D3D12_RESOURCE_STATE_COPY_DEST
                              : D3D12_RESOURCE_STATE_COPY_SOURCE,
            "CopyTiles tiled resource", record.start.Subresource);
        RecordReplayResourceAccess(
            chunk, state, record.buffer.ptr(),
            buffer_to_texture ? D3D12_RESOURCE_STATE_COPY_SOURCE
                              : D3D12_RESOURCE_STATE_COPY_DEST,
            "CopyTiles buffer");
        ReplayCopyTiles(chunk, state, record);
      } else if constexpr (std::is_same_v<T, ResolveSubresourceRecord>) {
        FlushPassBatches(chunk, state);
        RecordReplayResourceAccess(chunk, state, record.dst.ptr(),
                                   D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                   "ResolveSubresource dst",
                                   record.dst_subresource);
        RecordReplayResourceAccess(chunk, state, record.src.ptr(),
                                   D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
                                   "ResolveSubresource src",
                                   record.src_subresource);
        ReplayResolveSubresource(device_->GetMTLDevice(), chunk, record);
      } else if constexpr (std::is_same_v<T, ClearRenderTargetRecord>) {
        FlushPassBatches(chunk, state);
        if (auto *resource = GetResource(record.descriptor.resource.ptr())) {
          RecordDescriptorResourceAccessRanges(
              chunk, state, *resource, D3D12_RESOURCE_STATE_RENDER_TARGET,
              record.descriptor, "ClearRenderTargetView",
              GetRenderTargetSubresourceRanges);
        }
        ReplayClearRenderTarget(device_->GetMTLDevice(), chunk, record);
      } else if constexpr (std::is_same_v<T, ClearDepthStencilRecord>) {
        FlushPassBatches(chunk, state);
        if (auto *resource = GetResource(record.descriptor.resource.ptr())) {
          RecordDescriptorResourceAccessRanges(
              chunk, state, *resource, D3D12_RESOURCE_STATE_DEPTH_WRITE,
              record.descriptor, "ClearDepthStencilView",
              GetDepthStencilSubresourceRanges);
        }
        ReplayClearDepthStencil(device_->GetMTLDevice(), chunk, record);
    } else if constexpr (std::is_same_v<T, ClearUnorderedAccessRecord>) {
        FlushPassBatches(chunk, state);
        if (auto *resource = GetResource(record.resource.ptr())) {
          RecordDescriptorResourceAccessRanges(
              chunk, state, *resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
              record.descriptor, "ClearUnorderedAccessView",
              GetUnorderedAccessSubresourceRanges);
        }
        ReplayClearUnorderedAccess(device_->GetMTLDevice(), chunk, record);
    } else if constexpr (std::is_same_v<T, DiscardResourceRecord>) {
      FlushPassBatches(chunk, state);
      TouchReplayResource(state, record.resource.ptr());
      ReplayDiscardResource(chunk, record);
    } else if constexpr (std::is_same_v<T, ClearStateRecord>) {
      FlushPassBatches(chunk, state);
      ResetReplayApiState(state, record.pipeline_state.ptr());
    } else if constexpr (std::is_same_v<T, PipelineStateRecord>) {
      state.pipeline_state = record.pipeline_state;
      BumpGraphicsBindingGeneration(
          state, GraphicsBindingGenerationBumpSource::PipelineState);
    } else if constexpr (std::is_same_v<T, PrimitiveTopologyRecord>) {
      state.topology = record.topology;
    } else if constexpr (std::is_same_v<T, ViewportRecord>) {
      state.viewports = record.viewports;
    } else if constexpr (std::is_same_v<T, ScissorRecord>) {
      state.scissors = record.rects;
    } else if constexpr (std::is_same_v<T, BlendFactorRecord>) {
      state.blend_factor = record.blend_factor;
    } else if constexpr (std::is_same_v<T, StencilRefRecord>) {
      state.stencil_ref = record.stencil_ref;
    } else if constexpr (std::is_same_v<T, RenderTargetsRecord>) {
      state.render_targets = record.render_targets;
      state.depth_stencil = record.depth_stencil;
    } else if constexpr (std::is_same_v<T, DescriptorHeapsRecord>) {
      // Queued draw/dispatch packets capture their binding state up front, so
      // heap changes do not need to close an otherwise mergeable pass.
      state.cbv_srv_uav_heap = nullptr;
      state.sampler_heap = nullptr;
      state.graphics_tables = {};
      state.compute_tables = {};
      for (const auto &heap : record.heaps) {
        auto *descriptor_heap = dynamic_cast<DescriptorHeap *>(heap.ptr());
        if (!descriptor_heap)
          continue;
        const auto &desc = descriptor_heap->GetDescriptorHeapDesc();
        if (!(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
          continue;
        if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
          state.cbv_srv_uav_heap = heap;
        else if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
          state.sampler_heap = heap;
      }
      BumpGraphicsBindingGeneration(
          state, GraphicsBindingGenerationBumpSource::DescriptorHeaps);
    } else if constexpr (std::is_same_v<T, VertexBuffersRecord>) {
      for (UINT i = 0; i < record.view_count &&
                       record.start_slot + i < state.vertex_buffers.size();
           i++) {
        const auto slot = record.start_slot + i;
        if (i < record.views.size()) {
          state.vertex_buffers[slot] = record.views[i];
          state.resolved_vertex_buffers[slot] =
              ResolveReplayVertexBuffer(state, record.views[i]);
        } else {
          state.vertex_buffers[record.start_slot + i].reset();
          state.resolved_vertex_buffers[slot] = {};
        }
      }
      BumpGraphicsBindingGeneration(
          state, GraphicsBindingGenerationBumpSource::VertexBuffers);
    } else if constexpr (std::is_same_v<T, IndexBufferRecord>) {
      state.index_buffer = record.view;
      state.resolved_index_buffer =
          record.view ? ResolveReplayIndexBuffer(state, *record.view)
                      : ResolvedReplayIndexBuffer{};
    } else if constexpr (std::is_same_v<T, ResourceBarrierRecord>) {
      // This is the REAL app-barrier replay entry (ReplayResourceBarrier is a
      // separate path). An app-declared barrier changes resource state, so it
      // must invalidate the descAccess cache — otherwise a later same-binding
      // draw could wrongly reuse a stale "frozen state" hazard result.
      state.access_epoch++;
      // Stage-2 probe: count app-DECLARED barrier transitions here (vs the
      // auto-inferred mismatchBarriers) to gauge passthrough-completeness.
      if (ReplayPerfEnabled())
        perDrawSubTimers().appBarrierTransitions += record.barriers.size();
      auto batch = BuildResourceBarrierBatch(state, record, desc_.Type);
      if (batch.entries.empty() && !batch.needs_separator)
        return;
      QueueOrDeferResourceAccessBarrierBatch(chunk, state, std::move(batch));
    } else if constexpr (std::is_same_v<T, RootSignatureRecord>) {
      // Root signatures only affect binding translation for subsequently queued
      // packets; already queued packets keep their cloned state.
      auto *root = GetRootSignature(record.root_signature.ptr());
      if (record.compute) {
        state.compute_root_signature = record.root_signature;
        state.compute_root_signature_impl = root;
        state.compute_tables = {};
        state.compute_root_constants = {};
        state.compute_cbv_roots = {};
        state.compute_srv_roots = {};
        state.compute_uav_roots = {};
      } else {
        state.graphics_root_signature = record.root_signature;
        state.graphics_root_signature_impl = root;
        state.graphics_tables = {};
        state.graphics_root_constants = {};
        state.graphics_cbv_roots = {};
        state.graphics_srv_roots = {};
        state.graphics_uav_roots = {};
        BumpGraphicsBindingGeneration(
            state, GraphicsBindingGenerationBumpSource::RootSignature);
      }
    } else if constexpr (std::is_same_v<T, RootDescriptorTableRecord>) {
      auto &tables = record.compute ? state.compute_tables
                                    : state.graphics_tables;
      if (record.root_parameter_index < tables.size())
        tables[record.root_parameter_index] = record.base_descriptor;
      if (!record.compute)
        BumpGraphicsBindingGeneration(
            state, GraphicsBindingGenerationBumpSource::RootDescriptorTable);
    } else if constexpr (std::is_same_v<T, RootDescriptorRecord>) {
      StoreRootDescriptor(state, record);
      if (!record.compute)
        BumpGraphicsBindingGeneration(
            state, GraphicsBindingGenerationBumpSource::RootDescriptor);
    } else if constexpr (std::is_same_v<T, RootConstantsRecord>) {
      StoreRootConstants(state, record);
      if (!record.compute)
        BumpGraphicsBindingGeneration(
            state, GraphicsBindingGenerationBumpSource::RootConstants);
    } else if constexpr (std::is_same_v<T, BeginQueryRecord>) {
      FlushPassBatches(chunk, state);
      ReplayBeginQuery(chunk, state, record);
    } else if constexpr (std::is_same_v<T, EndQueryRecord>) {
      FlushPassBatches(chunk, state);
      ReplayEndQuery(chunk, state, record);
      } else if constexpr (std::is_same_v<T, ResolveQueryDataRecord>) {
        FlushPassBatches(chunk, state);
        RecordReplayResourceAccess(chunk, state, record.dst_buffer.ptr(),
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   "ResolveQueryData dst",
                                   D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, 0,
                                   false, false);
        ReplayResolveQueryData(chunk, state, record);
      } else if constexpr (std::is_same_v<T, PredicationRecord>) {
        FlushPassBatches(chunk, state);
        RecordReplayResourceAccess(chunk, state, record.buffer.ptr(),
                                   D3D12_RESOURCE_STATE_PREDICATION,
                                   "SetPredication");
        ReplaySetPredication(state, record);
      } else if constexpr (std::is_same_v<T, WriteBufferImmediateRecord>) {
        FlushPassBatches(chunk, state);
        for (const auto &operation : record.operations) {
          RecordReplayResourceAccess(chunk, state, operation.resource.ptr(),
                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                     "WriteBufferImmediate");
        }
        ReplayWriteBufferImmediate(device_->GetMTLDevice(), chunk, record);
      } else if constexpr (std::is_same_v<T, ExecuteIndirectRecord>) {
        RecordReplayResourceAccess(chunk, state, record.arg_buffer.ptr(),
                                   D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                   "ExecuteIndirect arguments");
        RecordReplayResourceAccess(chunk, state, record.count_buffer.ptr(),
                                   D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                                   "ExecuteIndirect count");
        ReplayExecuteIndirect(chunk, state, record);
      } else if constexpr (std::is_same_v<T, TemporalUpscaleRecord>) {
        FlushPassBatches(chunk, state);
        RecordReplayResourceAccess(chunk, state, record.color.ptr(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   "TemporalUpscale color");
        RecordReplayResourceAccess(chunk, state, record.depth.ptr(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   "TemporalUpscale depth");
        RecordReplayResourceAccess(chunk, state, record.motion_vector.ptr(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   "TemporalUpscale motion");
        RecordReplayResourceAccess(chunk, state, record.exposure_texture.ptr(),
                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                   "TemporalUpscale exposure");
        RecordReplayResourceAccess(chunk, state, record.output.ptr(),
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                   "TemporalUpscale output");
        ReplayTemporalUpscale(device_->GetMTLDevice(), temporal_scaler_cache_,
                              chunk, record);
    } else if constexpr (std::is_same_v<T, DrawInstancedRecord>) {
      ReplayDrawInstanced(chunk, state, record);
    } else if constexpr (std::is_same_v<T, DrawIndexedInstancedRecord>) {
      ReplayDrawIndexedInstanced(chunk, state, record);
    } else if constexpr (std::is_same_v<T, DispatchRecord>) {
      FlushGraphicsBeforeCompute(chunk, state);
      ReplayDispatch(chunk, state, record);
    }
  }

  void ReplayCopyBufferRegion(CommandChunk *chunk, ReplayState &state,
                              const CopyBufferRegionRecord &record) {
    auto *dst = GetResource(record.dst.ptr());
    auto *src = GetResource(record.src.ptr());
    if (!dst || !src || !dst->GetBuffer() || !src->GetBuffer())
      return;

    Rc<Buffer> dst_buffer = dst->GetBuffer();
    Rc<Buffer> src_buffer = src->GetBuffer();
    const UINT64 src_offset = src->GetHeapOffset() + record.src_offset;
    const UINT64 dst_offset = dst->GetHeapOffset() + record.dst_offset;
    QueueBlitCommand(
        chunk, state, {record.src.ptr()}, {record.dst.ptr()},
        [dst_buffer, src_buffer, src_offset, dst_offset,
         byte_count = record.byte_count](ArgumentEncodingContext &enc) {
      auto [src_allocation, src_sub_offset] =
          enc.access(src_buffer, src_offset, byte_count, ResourceAccess::Read);
      auto [dst_allocation, dst_sub_offset] =
          enc.access(dst_buffer, dst_offset, byte_count, ResourceAccess::Write);
      auto &copy =
          enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = src_allocation->buffer();
      copy.src_offset = src_sub_offset + src_offset;
      copy.dst = dst_allocation->buffer();
      copy.dst_offset = dst_sub_offset + dst_offset;
      copy.copy_length = byte_count;
    });
  }

  // BuildResourceBarrierBatch() and ReplayResourceBarrier() now live in
  // d3d12_replay_barrier_record.hpp; both take the queue type explicitly, so
  // callers pass desc_.Type.

  // ReplayWriteBufferImmediate() now lives in
  // d3d12_replay_buffer_immediate.hpp and ReplayTemporalUpscale() in
  // d3d12_replay_temporal_upscale.hpp; both take the Metal device explicitly
  // (and the temporal upscale also the queue-owned scaler cache), so callers
  // pass device_->GetMTLDevice() and temporal_scaler_cache_.

  // GetReplayResourceStates(), TouchReplayResource(),
  // ResetReplayResourceStatesForAliasing() and
  // WarnReplayResourceAccessMismatch() now live in
  // d3d12_replay_resource_access_state.hpp; unqualified calls below resolve to
  // dxmt::d3d12. WarnReplayResourceAccessMismatch() takes the queue type
  // explicitly, so callers pass desc_.Type.

  // ShouldEmitReadAfterWriteMismatchBarrier(),
  // ShouldEmitWriteAfterReadMismatchBarrier() and
  // ShouldEmitWriteAfterWriteMismatchBarrier() now live in
  // d3d12_replay_mismatch_barrier_policy.hpp; unqualified calls below resolve
  // to dxmt::d3d12.

  void RecordReplayResourceAccess(CommandChunk *chunk, ReplayState &state,
                                  ID3D12Resource *d3d_resource,
                                  D3D12_RESOURCE_STATES desired,
                                  const char *context,
                                  UINT first_subresource =
                                      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  UINT subresource_count = 0,
                                  bool materialize_query_resolves = true,
                                  bool materialize_timestamp_resolves = true) {
    const bool rb_su = ReplayPerfEnabled();
    ScopeAccum rb_su_guard{rb_su, rb_su ? clock::now() : clock::time_point{},
                           &perDrawSubTimers().stateUpd};
    if (rb_su) perDrawSubTimers().resAccessCalls++;
    // If this texture is cached as uniformly read-steady (all subresources in
    // one read-only state S with no write-sync debt) and the desired read-only
    // access is a subset of S, the full path is a no-op for every subresource:
    // the existing state already covers desired and no mismatch barrier is due.
    if (ReplayResourceAccessIsSteadyReadNoop(state, d3d_resource, desired)) {
      if (rb_su) perDrawSubTimers().resAccessSteadyNoop++;
      return;
    }
    auto *resource = GetResource(d3d_resource);
    if (!resource)
      return;
    if (materialize_query_resolves && resource->GetBufferAllocation())
      MaterializeCpuQueryResolves(chunk, state, d3d_resource, 0,
                                  resource->GetResourceDesc().Width);
    if (materialize_query_resolves && resource->GetBufferAllocation())
      MaterializeDeferredCpuQueryResolvesForGpuAccess(
          chunk, state, d3d_resource, 0, resource->GetResourceDesc().Width);
    if (materialize_timestamp_resolves)
      MaterializeTimestampResolvesForAccess(chunk, state, d3d_resource, desired);
    WarnUnsupportedResourceState(desired, context);

    auto &states = GetReplayResourceStates(state, *resource);
    TouchReplayResource(state, resource->GetD3D12Resource());
    if (desired == D3D12_RESOURCE_STATE_COMMON)
      return;

    const bool all_subresources =
        first_subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!all_subresources && first_subresource >= states.size()) {
      WARN("D3D12CommandQueue: resource access subresource out of range context=",
           context, " subresource=", first_subresource,
           " count=", uint32_t(states.size()));
      return;
    }

    const UINT first = all_subresources ? 0 : first_subresource;
    const UINT count = all_subresources
                           ? UINT(states.size())
                           : std::min<UINT>(subresource_count ? subresource_count : 1,
                                            UINT(states.size()) - first);
    ResourceAccessBarrierBatch mismatch_resource_barriers;
    const bool has_mismatch_resource_barriers =
        ApplyReplayResourceAccessTransitions(states, *resource, desired, first,
                                             count, context, desc_.Type,
                                             mismatch_resource_barriers);
    if (has_mismatch_resource_barriers) {
      // A real read/write hazard barrier is a resource-state sync point — bump
      // the descAccess-cache epoch so a later same-binding draw re-records the
      // hazard instead of wrongly reusing a stale "frozen state" result.
      state.access_epoch++;
      // Stage-2 probe: a sync point DXMT auto-inferred (not app-declared).
      if (rb_su)
        perDrawSubTimers().mismatchBarriers++;
      // Some reflected uses can observe a read/write hazard before DXMT's
      // replay state sees the app's later transition. Keep the D3D12 state
      // unchanged, but make Metal visibility safe for the actual access.
      QueueOrDeferResourceAccessBarrierBatch(
          chunk, state, std::move(mismatch_resource_barriers));
    }
    UpdateReplaySteadyReadState(state, d3d_resource, *resource, states);
  }

  void RecordReplayResourceAccessRanges(
      CommandChunk *chunk, ReplayState &state, ID3D12Resource *d3d_resource,
      D3D12_RESOURCE_STATES desired, const char *context,
      const std::vector<SubresourceRange> &ranges) {
    if (ranges.empty()) {
      RecordReplayResourceAccess(chunk, state, d3d_resource, desired, context);
      return;
    }
    for (const auto &range : ranges) {
      RecordReplayResourceAccess(chunk, state, d3d_resource, desired, context,
                                 range.first, range.count);
    }
  }

  void RecordDescriptorResourceAccessRanges(
      CommandChunk *chunk, ReplayState &state, Resource &resource, D3D12_RESOURCE_STATES desired,
      const DescriptorRecord &descriptor, const char *context,
      bool (*get_ranges)(Resource &, const DescriptorRecord &,
                         std::vector<SubresourceRange> &)) {
    // PERF: reuse a thread-local buffer (clear keeps capacity) instead of a
    // fresh heap allocation per SRV/UAV descriptor. No reentrancy: the consumer
    // (RecordReplayResourceAccessRanges) does not call back into this function.
    thread_local std::vector<SubresourceRange> ranges;
    ranges.clear();
    if (!get_ranges(resource, descriptor, ranges)) {
      WarnDescriptorSubresourceRangeUnsupported(resource, descriptor, context);
      return;
    }
    if (ranges.empty()) {
      WarnDescriptorSubresourceRangeEmpty(resource, descriptor, context);
      return;
    }
    RecordReplayResourceAccessRanges(chunk, state, resource.GetD3D12Resource(), desired,
                                     context, ranges);
  }

  // Thin adapter over dxmt::d3d12::DecayTouchedResourceStates(); only supplies
  // the queue-owned backend state map and this queue's list type. Kept because
  // d3d12_command_queue_submission.inc still calls the one-argument form.
  void DecayTouchedResourceStates(
      const std::vector<Com<ID3D12Resource>> &touched_resources) {
    dxmt::d3d12::DecayTouchedResourceStates(
        touched_resources, device_queue_state_->BackendResourceStates(),
        desc_.Type);
  }

  // Thin adapter over dxmt::d3d12::ReplayTransitionBarrier(); only supplies
  // this queue's list type. Kept because d3d12_command_queue_execute.inc still
  // calls the three-argument form.
  void ReplayTransitionBarrier(ReplayState &state,
                               const StoredResourceBarrier &barrier,
                               ResourceAccessBarrierBatch &batch) {
    dxmt::d3d12::ReplayTransitionBarrier(state, barrier, batch, desc_.Type);
  }

  // ReplayUavBarrier(), ReplayAliasingBarrier(), AddResourceAccessBarrier()
  // and QueueResourceAccessBarrierBatch() now live in
  // d3d12_replay_barrier_record.hpp; MergeResourceAccessBarrierBatch() lives in
  // d3d12_replay_barrier_encode.hpp. Unqualified calls resolve to dxmt::d3d12.

