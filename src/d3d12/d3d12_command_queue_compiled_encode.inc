// Compiled binding recipes and compiled graphics/compute encoding.
// Included inside class CommandQueueImpl; not a standalone translation unit.
//
// Only the instance shims are left here: each extracted entry point takes the
// queue-owned state it needs explicitly, so the shims below are the only places
// that still reach through `device_` or the stage-plan caches.
private:
  // BindRootConstantsSnapshot lives in d3d12_snapshot_root_constants.hpp; its
  // last in-class caller left with ApplyGraphicsBindingSnapshot.

  // FindCompiledRootTable / RecordCompiledDescriptorBackendStats live in
  // d3d12_compiled_binding_tables.hpp.

  // BuildCompiledNativeBindingRecipe now lives in
  // d3d12_compiled_native_recipe.hpp.

  // EncodeCompiledNativeBindingRecipe / EncodeNativeArgumentTables live in
  // d3d12_compiled_binding_encode.hpp.

  // EncodeCompiledVertexBindingRecipe lives in
  // d3d12_vertex_buffer_encode.hpp.

  // The queue-owned state the extracted binding encode still needs: the Metal
  // device, the argument-buffer ring owner and the two stage-plan memos. Every
  // extracted entry point below takes one of these instead of the queue itself,
  // so the analyzer never has to expand CommandQueueImpl to follow a draw.
  dxmt::d3d12::SubmissionBindingContext BindingContext() {
    return {device_->GetMTLDevice(), device_->GetDXMTDevice().queue(),
            bindless_stage_plan_cache_, native_stage_plan_cache_};
  }

  // EncodeCompiledGraphicsBindings and EncodeCompiledComputeBindings now live
  // in d3d12_compiled_direct_binding_encode.hpp; the Metal device and the
  // argument-buffer ring owner are the only things they ever needed from the
  // command queue. Only the graphics one still has an in-class caller
  // (d3d12_command_queue_draw_encode.inc).
  void EncodeCompiledGraphicsBindings(
      ArgumentEncodingContext &enc,
      const CompiledDirectGraphicsBindingPayload &payload,
      PipelineState &pipeline, uint64_t &argbuf_offset,
      const CompiledBindingDelta *binding_delta = nullptr,
      BindlessMirrorDrawDiag *draw_diag = nullptr,
      CompiledCommandTestTelemetry *test_telemetry = nullptr) {
    dxmt::d3d12::EncodeCompiledGraphicsBindings(
        device_->GetMTLDevice(), device_->GetDXMTDevice().queue(), enc, payload,
        pipeline, argbuf_offset, binding_delta, draw_diag, test_telemetry);
  }

  // FillCompiledBindingState / BuildCompiledDirectGraphicsBindingFingerprint /
  // BuildCompiledEncoderBindingIdentity / ResolveRenderEncoderBindingDelta /
  // ResolveComputeEncoderBindingDelta live in
  // d3d12_compiled_binding_encode.hpp.
  // ResolveCompiledDirtyMask lives in d3d12_compiled_binding_tables.hpp.
  // AddCompiledDirectEncoderAccess lives in
  // d3d12_compiled_direct_access.hpp.

  // AddCompiledDescriptorEncoderAccess and the body of
  // AddCompiledSnapshotEncoderAccesses now live in
  // d3d12_compiled_snapshot_access.hpp; the Metal device handle is the only
  // thing they ever needed from the command queue.
  void AddCompiledSnapshotEncoderAccesses(
      CompiledDirectAccessList &list,
      const GraphicsBindingSnapshot *snapshot) {
    dxmt::d3d12::AddCompiledSnapshotEncoderAccesses(device_->GetMTLDevice(),
                                                    list, snapshot);
  }

  // CompiledDirectAccessListRequiresReverseBoundary /
  // PublishCompiledDirectAccessListForEncode / RetainFrozenNativeStoreForGpu
  // live in d3d12_compiled_direct_access.hpp.
  // RetainDirectBufferForGpu lives in d3d12_compiled_binding_tables.hpp.

  // The body lives in d3d12_replay_dispatch_encode.hpp.
  void EncodeReplayDispatchPacket(ArgumentEncodingContext &enc,
                                  ReplayDispatchPacket &packet,
                                  uint64_t &argbuf_offset) {
    dxmt::d3d12::EncodeReplayDispatchPacket(BindingContext(), enc, packet,
                                            argbuf_offset);
  }

  static void EncodeReplayDispatchCompiled(
      CommandQueueImpl &queue, ArgumentEncodingContext &enc,
      ReplayDispatchPacket &payload,
      uint64_t &argbuf_offset) {
    queue.EncodeReplayDispatchPacket(enc, payload, argbuf_offset);
  }

  // The body lives in d3d12_snapshot_binding_apply.hpp; a captured snapshot is
  // self-contained, so a SubmissionBindingContext is all it needs.
  void ApplyGraphicsBindingSnapshot(ArgumentEncodingContext &enc,
                                    const GraphicsBindingSnapshot &snapshot,
                                    PipelineState &pipeline,
                                    bool use_geometry,
                                    bool use_tessellation,
                                    uint64_t &argbuf_offset,
                                    BindlessMirrorDrawDiag *draw_diag = nullptr) {
    dxmt::d3d12::ApplyGraphicsBindingSnapshot(
        BindingContext(), enc, snapshot, pipeline, use_geometry,
        use_tessellation, argbuf_offset, draw_diag);
  }

  // EncodeVertexBuffers lives in d3d12_vertex_buffer_encode.hpp.

  // EncodeGraphicsBindings and EncodeComputeBindings now live in
  // d3d12_replay_binding_encode.hpp. Their last callers were the indirect
  // replays, which now build a SubmissionBindingContext and call the free
  // functions from d3d12_indirect_command_encode.cpp, so no shim is left.
