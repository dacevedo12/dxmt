// Packet classification, pass queueing, and descriptor snapshot capture.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // ReplayPacketClassificationEnabled(),
  // RecordReplayGraphicsPacketClassification() and
  // RecordReplayComputePacketClassification() now live in
  // d3d12_replay_frame_stats_attribution.hpp; unqualified calls resolve to
  // dxmt::d3d12. These two wrappers stay because the frame statistics block is
  // only reachable through device_, and the gate has to keep that walk lazy.
  void RecordGraphicsPacketClassification(
      const ReplayState &state, ReplayGraphicsCommandKind kind,
      bool compiled_candidate, bool use_geometry, bool use_tessellation) {
    if (!ReplayPacketClassificationEnabled(state) ||
        kind == ReplayGraphicsCommandKind::Barrier)
      return;

    RecordReplayGraphicsPacketClassification(
        &device_->GetDXMTDevice().queue().CurrentFrameStatistics(), kind,
        compiled_candidate, use_geometry, use_tessellation);
  }

  void RecordComputePacketClassification(const ReplayState &state,
                                         bool compiled_candidate) {
    if (!ReplayPacketClassificationEnabled(state))
      return;

    RecordReplayComputePacketClassification(
        &device_->GetDXMTDevice().queue().CurrentFrameStatistics(),
        compiled_candidate);
  }

  template <typename Fn>
  void QueueGraphicsPassCommand(CommandChunk *chunk, ReplayState &state,
                                ReplayRenderPassAttachments attachments,
                                uint64_t argument_buffer_size, Fn &&fn,
                                bool use_geometry = false,
                                bool use_tessellation = false,
                                ReplayGraphicsCommandKind kind =
                                    ReplayGraphicsCommandKind::Draw,
                                bool bindless_compiled_candidate = false) {
    RecordGraphicsPacketClassification(
        state, kind, bindless_compiled_candidate, use_geometry,
        use_tessellation);
    ResolveSubmissionEncoderBoundary(
        state, CompiledEncoderKind::Graphics,
        ReplayGraphicsPassContinuesSubmissionBoundary(state, attachments));
    if (HasPendingComputePass(state) || HasPendingBlitBatch(state))
      FlushPassBatches(chunk, state);

    auto &batch = state.graphics_pass_batch;
    if (ReplayGraphicsBatchRejectsAttachments(batch, attachments))
      FlushPassBatches(chunk, state);

    auto &active_batch = state.graphics_pass_batch;
    if (!active_batch.active) {
      active_batch.active = true;
      active_batch.attachments = std::move(attachments);
      active_batch.argument_buffer_size = 0;
    }

    const auto slot = ReserveReplayGraphicsBatchSlot(
        active_batch, argument_buffer_size, kind, use_geometry,
        use_tessellation);
    ReplayGraphicsPassCommand command = {};
    using Encode = std::decay_t<Fn>;
    command.encoder = AllocateReplayEncoder<
        ReplayPassEncodeCommand, ReplayOwnedEncodeCommand<Encode>>(
        active_batch.encoder_arena, std::forward<Fn>(fn));
    command.d3d_sequence = dxmt::apitrace::current_d3d_sequence();
    FillReplayGraphicsPassCommandSlot(command, slot, argument_buffer_size, kind,
                                      use_geometry, use_tessellation,
                                      bindless_compiled_candidate);
    active_batch.commands.push_back(std::move(command));
  }

  template <typename Payload, typename Encode>
  void QueueCompiledGraphicsPassCommand(
      CommandChunk *chunk, ReplayState &state,
      const ReplayRenderPassAttachments &attachments,
      uint64_t argument_buffer_size,
      Payload &&payload, Encode encode,
      bool use_geometry, bool use_tessellation,
      ReplayGraphicsCommandKind kind) {
    RecordGraphicsPacketClassification(state, kind, true, use_geometry,
                                       use_tessellation);
    ResolveSubmissionEncoderBoundary(
        state, CompiledEncoderKind::Graphics,
        ReplayGraphicsPassContinuesSubmissionBoundary(state, attachments));
    if (HasPendingComputePass(state) || HasPendingBlitBatch(state))
      FlushPassBatches(chunk, state);

    auto &batch = state.graphics_pass_batch;
    if (ReplayGraphicsBatchRejectsAttachments(batch, attachments))
      FlushPassBatches(chunk, state);

    auto &active_batch = state.graphics_pass_batch;
    if (!active_batch.active) {
      active_batch.active = true;
      active_batch.attachments = attachments;
      active_batch.argument_buffer_size = 0;
    }

    const auto slot = ReserveReplayGraphicsBatchSlot(
        active_batch, argument_buffer_size, kind, use_geometry,
        use_tessellation);

    ReplayGraphicsPassCommand command = {};
    using OwnedPayload = std::decay_t<Payload>;
    command.encoder = AllocateReplayEncoder<
        ReplayPassEncodeCommand,
        ReplayCompiledEncodeCommand<OwnedPayload, Encode>>(
        active_batch.encoder_arena, std::forward<Payload>(payload), encode);
    command.d3d_sequence = dxmt::apitrace::current_d3d_sequence();
    FillReplayGraphicsPassCommandSlot(command, slot, argument_buffer_size, kind,
                                      use_geometry, use_tessellation, true);
    active_batch.commands.push_back(std::move(command));
  }

  // QueueGraphicsResourceBarrierCommand() and
  // QueueComputeResourceBarrierCommand() now live in
  // d3d12_replay_inline_pass_commands.hpp; unqualified calls below resolve to
  // dxmt::d3d12. Their encode closures only capture the moved barrier entry
  // vector, so neither the functions nor the closures ever named `this`.

  void QueueOrDeferResourceAccessBarrierBatch(CommandChunk *chunk,
                                              ReplayState &state,
                                              ResourceAccessBarrierBatch batch) {
    if (batch.entries.empty() && !batch.needs_separator)
      return;
    if (CanDeferResourceBarriersIntoGraphicsBatch(state, batch)) {
      QueueGraphicsResourceBarrierCommand(chunk, state, std::move(batch));
      return;
    }
    if (CanDeferResourceBarriersIntoComputeBatch(state, batch)) {
      QueueComputeResourceBarrierCommand(chunk, state, std::move(batch));
      return;
    }
    FlushReplayWorkBeforeResourceBarrier(chunk, state);
    QueueResourceAccessBarrierBatch(state, std::move(batch));
  }

  // D3D12ReplayComputeBatchingEnabled() now lives in
  // d3d12_replay_batching_policy.hpp; unqualified calls resolve to dxmt::d3d12.

  // EmitSingleComputePass() and EmitSingleGraphicsPass() now live in
  // d3d12_replay_inline_pass_commands.hpp; unqualified calls below resolve to
  // dxmt::d3d12. Both emitcc closures capture only the functions' own
  // parameters. The graphics one used to need a `this` capture for
  // BeginRenderPass(); that became a free function in
  // d3d12_render_pass_attachments.hpp, so the last tie is gone.

  template <typename Fn>
  void QueueComputePassCommand(CommandChunk *chunk, ReplayState &state,
                               uint64_t argument_buffer_size, Fn &&fn,
                               bool compiled_candidate = false) {
    RecordComputePacketClassification(state, compiled_candidate);
    ResolveSubmissionEncoderBoundary(
        state, CompiledEncoderKind::Compute,
        ReplayComputePassContinuesSubmissionBoundary(state));
    if (!D3D12ReplayComputeBatchingEnabled()) {
      FlushPassBatches(chunk, state);
      EmitSingleComputePass(chunk, argument_buffer_size,
                            dxmt::apitrace::current_d3d_sequence(),
                            std::forward<Fn>(fn));
      return;
    }

    FlushGraphicsBeforeCompute(chunk, state);

    auto &batch = state.compute_pass_batch;
    if (!batch.active) {
      batch.active = true;
      batch.argument_buffer_size = 0;
    }

    const uint64_t argument_buffer_offset = batch.argument_buffer_size;
    batch.argument_buffer_size += argument_buffer_size;
    using Encode = std::decay_t<Fn>;
    ReplayComputePassCommand command = {};
    command.encoder = AllocateReplayEncoder<
        ReplayPassEncodeCommand, ReplayOwnedEncodeCommand<Encode>>(
        batch.encoder_arena, std::forward<Fn>(fn));
    command.d3d_sequence = dxmt::apitrace::current_d3d_sequence();
    command.argument_buffer_offset = argument_buffer_offset;
    command.argument_buffer_size = argument_buffer_size;
    command.kind = ReplayComputeCommandKind::Dispatch;
    command.compiled_candidate = compiled_candidate;
    batch.commands.push_back(std::move(command));
  }

  template <typename Payload, typename Encode>
  void QueueCompiledComputePassCommand(
      CommandChunk *chunk, ReplayState &state, uint64_t argument_buffer_size,
      Payload &&payload, Encode encode) {
    RecordComputePacketClassification(state, true);
    ResolveSubmissionEncoderBoundary(
        state, CompiledEncoderKind::Compute,
        ReplayComputePassContinuesSubmissionBoundary(state));
    if (!D3D12ReplayComputeBatchingEnabled()) {
      FlushPassBatches(chunk, state);
      const auto d3d_sequence = dxmt::apitrace::current_d3d_sequence();
      using OwnedPayload = std::decay_t<Payload>;
      auto command = std::make_unique<
          ReplayCompiledEncodeCommand<OwnedPayload, Encode>>(
          std::forward<Payload>(payload), encode);
      chunk->emitcc([this, command = std::move(command), argument_buffer_size,
                     d3d_sequence](ArgumentEncodingContext &enc) mutable {
        enc.startComputePass(argument_buffer_size);
        uint64_t argbuf_offset = 0;
        enc.setArgumentBufferWriteRange(0, argument_buffer_size);
        if (d3d_sequence)
          dxmt::apitrace::set_current_d3d_sequence(d3d_sequence);
        command->Encode(*this, enc, argbuf_offset);
        enc.endPass();
      });
      return;
    }

    FlushGraphicsBeforeCompute(chunk, state);
    auto &batch = state.compute_pass_batch;
    if (!batch.active) {
      batch.active = true;
      batch.argument_buffer_size = 0;
    }
    ReplayComputePassCommand command = {};
    using OwnedPayload = std::decay_t<Payload>;
    command.encoder = AllocateReplayEncoder<
        ReplayPassEncodeCommand,
        ReplayCompiledEncodeCommand<OwnedPayload, Encode>>(
        batch.encoder_arena, std::forward<Payload>(payload), encode);
    command.d3d_sequence = dxmt::apitrace::current_d3d_sequence();
    command.argument_buffer_offset = batch.argument_buffer_size;
    command.argument_buffer_size = argument_buffer_size;
    command.kind = ReplayComputeCommandKind::Dispatch;
    command.compiled_candidate = true;
    batch.argument_buffer_size += argument_buffer_size;
    batch.commands.push_back(std::move(command));
  }

  // D3D12ReplayGraphicsBatchingEnabled() now lives in
  // d3d12_replay_batching_policy.hpp; unqualified calls resolve to dxmt::d3d12.

  // PerDrawSubTimers and perDrawSubTimers() now live in
  // d3d12_replay_perf_timers.hpp; unqualified uses below resolve to
  // dxmt::d3d12.
  //
  // RecordReplayRecordPerfBucket<T>() now lives in its own
  // d3d12_replay_perf_record_buckets.hpp; unqualified calls resolve to
  // dxmt::d3d12. It needs the whole CommandRecord payload type set to pick a
  // bucket, so it is deliberately kept out of the dependency-free
  // d3d12_replay_perf_timers.hpp ledger header.

  // BumpGraphicsBindingGeneration() now lives in
  // d3d12_replay_pass_batch_ops.hpp; unqualified calls resolve to dxmt::d3d12.

  // StallProbe, stallProbe(), StallDiagEnabled(), StallScope and ScopeAccum now
  // live in d3d12_replay_stall_probe.hpp; unqualified uses below resolve to
  // dxmt::d3d12. stallProbe() still returns the one program-wide thread_local
  // probe, now defined out-of-line in d3d12_replay_stall_probe.cpp.

  // HashGraphicsBindingFull(), HashGraphicsBindingSnapshotState(),
  // GraphicsBindingSnapshotMatches(), FindCachedGraphicsBindingSnapshot() and
  // RecordReplayBindingSignaturePerf() now live in
  // d3d12_replay_binding_signature.hpp; unqualified calls resolve to
  // dxmt::d3d12.

  // HashCompiledDescriptorBindingIdentity /
  // CompiledDescriptorBindingIdentityMatches /
  // CompiledDescriptorSnapshotStillCurrent /
  // FinalizeCompiledDescriptorSnapshot /
  // CaptureCompiledDescriptorJournalCursor /
  // RecordCompiledDescriptorJournalSlot / RecordCompiledDescriptorJournalSpan
  // live in d3d12_descriptor_snapshot_journal.hpp.

  // CaptureCompiledDescriptorTableBindings(),
  // CaptureCompiledDescriptorBindingSnapshot(),
  // GetOrCaptureCompiledDescriptorBindingSnapshot() and
  // CaptureSubmittedDescriptorSnapshots() now live in
  // d3d12_submitted_descriptor_capture.hpp. Only the first three were ever
  // reachable from the fourth, so all but the submission entry point are now
  // internal to that translation unit. The Metal device and the two
  // stage-plan memos they reached for arrive in a SubmissionBindingContext.

  // IsGraphicsPassRunRecord(), IsGraphicsStateSetterRecord(),
  // IsDeferrableCompiledStateRecord(), FallbackRecordNeedsBindingState()
  // and IntraPassParallelEnabled() now live in
  // d3d12_replay_batching_policy.hpp; unqualified calls resolve to
  // dxmt::d3d12.
