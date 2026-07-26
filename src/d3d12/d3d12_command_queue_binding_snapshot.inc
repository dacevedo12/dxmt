// Graphics binding snapshot capture and bindless table freezing.
// Included inside class CommandQueueImpl; not a standalone translation unit.
//
// Capture itself is gone from the class: it lives in
// d3d12_graphics_binding_capture.hpp together with the descriptor-table walk,
// and the bindless freeze in d3d12_frozen_bindless_stage_tables.hpp. What is
// left here is the pass-batch cache around capture, which is queue state, plus
// the instance shims that supply the Metal device and the stage-plan memos.
private:
  // CaptureDescriptorTableBindings and CaptureGraphicsBindingSnapshot now live
  // in d3d12_graphics_binding_capture.hpp.

  // CaptureGraphicsRootDescriptorAddress and CaptureGraphicsRootConstantsValues
  // now live in d3d12_root_binding_capture.hpp, together with the shared
  // BuildRootBufferDescriptor used below by ApplyRootBufferDescriptor.

  // CaptureGraphicsRootDescriptor, CaptureGraphicsRootConstants and
  // CaptureGraphicsVertexBuffers now live in d3d12_live_binding_capture.hpp.

  // RecordGraphicsBindingSnapshotRequestPerf and
  // RecordGraphicsBindingSnapshotCapturePerf now live in
  // d3d12_snapshot_capture_perf.hpp.

  std::shared_ptr<GraphicsBindingSnapshot> GetOrCaptureGraphicsBindingSnapshot(
      CommandChunk *chunk, ReplayState &state, PipelineState &pipeline,
      const ReplayRenderPassAttachments &attachments,
      dxmt::DescriptorContentRevision descriptor_content_revision,
      std::shared_ptr<GraphicsBindingSnapshot> prepared_snapshot = {}) {
    RecordGraphicsBindingSnapshotRequestPerf(state,
                                             descriptor_content_revision);
    if (!D3D12ReplayGraphicsBatchingEnabled())
    {
      if (ReplayPerfEnabled())
        perDrawSubTimers().snapshotPassthrough++;
      GraphicsBindingSnapshotCaptureStats capture_stats = {};
      auto snapshot = std::make_shared<GraphicsBindingSnapshot>(
          CaptureGraphicsBindingSnapshot(state, pipeline, &capture_stats));
      snapshot->descriptor_content_revision = descriptor_content_revision;
      RecordGraphicsBindingSnapshotCapturePerf(capture_stats);
      return snapshot;
    }

    bool batch_reset = false;
    if (HasPendingComputePass(state) || HasPendingBlitBatch(state)) {
      FlushPassBatches(chunk, state);
      batch_reset = true;
    }

    auto &batch = state.graphics_pass_batch;
    if (batch.active &&
        !ReplayRenderPassAttachmentsMatch(batch.attachments, attachments)) {
      FlushPassBatches(chunk, state);
      batch_reset = true;
    }

    auto &active_batch = state.graphics_pass_batch;
    if (!active_batch.active) {
      active_batch.active = true;
      active_batch.attachments = attachments;
      active_batch.argument_buffer_size = 0;
    }

    if (prepared_snapshot) {
      if (batch_reset) {
        const auto key = HashGraphicsBindingSnapshotState(
            state, pipeline, descriptor_content_revision);
        active_batch.captured_binding_snapshots.emplace(
            key, prepared_snapshot);
      }
      if (ReplayPerfEnabled())
        perDrawSubTimers().snapshotCacheHits++;
      return prepared_snapshot;
    }

    if (auto snapshot = FindCachedGraphicsBindingSnapshot(
            state, pipeline, descriptor_content_revision)) {
      if (ReplayPerfEnabled())
        perDrawSubTimers().snapshotCacheHits++;
      return snapshot;
    }

    if (ReplayPerfEnabled())
      perDrawSubTimers().snapshotCacheMisses++;
    GraphicsBindingSnapshotCaptureStats capture_stats = {};
    auto snapshot = std::make_shared<GraphicsBindingSnapshot>(
        CaptureGraphicsBindingSnapshot(state, pipeline, &capture_stats));
    snapshot->descriptor_content_revision = descriptor_content_revision;
    const auto key = HashGraphicsBindingSnapshotState(
        state, pipeline, descriptor_content_revision);
    active_batch.captured_binding_snapshots.emplace(key, snapshot);
    RecordGraphicsBindingSnapshotCapturePerf(capture_stats);
    return snapshot;
  }

  // The capture body lives in d3d12_graphics_binding_capture.hpp; the Metal
  // device and the two stage-plan memos are the only things it ever needed from
  // the command queue.
  GraphicsBindingSnapshot CaptureGraphicsBindingSnapshot(
      const ReplayState &state, PipelineState &pipeline,
      GraphicsBindingSnapshotCaptureStats *capture_stats = nullptr) {
    return dxmt::d3d12::CaptureGraphicsBindingSnapshot(
        device_->GetMTLDevice(), bindless_stage_plan_cache_,
        native_stage_plan_cache_, state, pipeline, capture_stats);
  }

  // The body lives in d3d12_root_buffer_bind.hpp; the Metal device handle is
  // the only thing it ever needed from the command queue.
  template <typename State>
  void ApplyRootBufferDescriptor(ArgumentEncodingContext &enc,
                                 const State &state,
                                 const PipelineState &pipeline, bool compute,
                                 UINT root_index,
                                 const RootSignatureParameter &parameter,
                                 DescriptorRecordType type) {
    dxmt::d3d12::ApplyRootBufferDescriptor(device_->GetMTLDevice(), enc, state,
                                           pipeline, compute, root_index,
                                           parameter, type);
  }

  // PipelineUsesGeometry, ShaderResourceViewIsMultisampledTexture and
  // SetPixelShaderMsaaSrvDemoteBit now live in d3d12_shader_stage_query.hpp /
  // d3d12_descriptor_record_query.hpp.

  // Both PixelShaderSingleSampleMsaaSRVDemoteMask overloads now live at
  // namespace scope: the live-state one in d3d12_live_msaa_demote_query.hpp,
  // the snapshot-only one in d3d12_snapshot_binding_query.hpp. Neither is a
  // member any more, so unqualified calls inside this class see both.

  // CurrentPixelShaderMsaaSrvDemoteMasks, EncodeShaderBindingsForStageBindless
  // and EncodeShaderBindingsForStageBindlessSnapshot now live in
  // d3d12_bindless_stage_encode.hpp; BuildBindlessRootOffsets, which the live
  // one drives, in d3d12_bindless_root_offsets.hpp.

  // NativeShaderUsesBufferSrv now lives in d3d12_shader_stage_query.hpp.

  // ---------------------------------------------------------------------------
  // Bindless stage tables — D3DMetal materialize-early / bind-late model
  //
  // Capture freezes CPU-side root_offsets + compact tex/sampler windows from the
  // submitted descriptor recipe. Encode only allocates GPU argument-buffer
  // slices, copies the frozen bytes, retains them for the current command
  // buffer, and binds slots 28-30. Encode must not re-walk recipes or rebuild
  // compact windows from live heap state.
  // ---------------------------------------------------------------------------

  // FrozenBindlessTablesForStage and MutableFrozenBindlessTablesForStage now
  // live in d3d12_snapshot_binding_query.hpp.

  // The upload body lives in d3d12_argument_upload.hpp; the queue instance is
  // the only thing it ever needed from the command queue.
  AllocatedArgumentBufferSlice
  UploadArgumentBufferBytes(ArgumentEncodingContext &enc, const void *bytes,
                            size_t size, size_t alignment = 64) {
    return dxmt::d3d12::UploadArgumentBufferBytes(
        device_->GetDXMTDevice().queue(), enc, bytes, size, alignment);
  }

  // FreezeBindlessStageTables and FreezeAllBindlessStageTables now live in
  // d3d12_frozen_bindless_stage_tables.hpp; the Metal device and the bindless
  // stage-plan memo are the only things they ever needed from the command
  // queue. Both remaining callers (capture in d3d12_graphics_binding_capture.cpp
  // and the compiled path in d3d12_command_queue_pass_queue.inc) call the free
  // function directly, so no shim is left.

  // UploadFrozenBindlessStageTables lives in d3d12_argument_upload.hpp and
  // BuildSnapshotBindlessBufferTableBindings in d3d12_snapshot_buffer_table.hpp;
  // both lost their last in-class caller with
  // EncodeShaderBindingsForStageBindlessSnapshot.
