// Replay pass batching, flushing, and blit hazard tracking.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // ResolveReplayVertexBuffer(), ResolveReplayIndexBuffer(),
  // ReplayStateRecordScope and GraphicsBindingGenerationBumpSource now live
  // in d3d12_replay_pass_batch_ops.hpp; unqualified uses below resolve to
  // dxmt::d3d12.

  // ReplayRenderPassAttachmentsMatch(), TextureSubresourceOverlapsView() and
  // ResourceBarrierTouchesRenderPassAttachments() now live in
  // d3d12_replay_pass_compatibility.hpp; unqualified calls below resolve to
  // dxmt::d3d12.

  // ReplayGraphicsPassBatchHasRealWork(),
  // ReplayComputePassBatchHasRealWork(),
  // CanDeferResourceBarriersIntoGraphicsBatch(),
  // CanDeferResourceBarriersIntoComputeBatch(), HasPendingGraphicsPass(),
  // HasPendingComputePass(), HasPendingBlitBatch(), HasPendingReplayWork(),
  // BuildReplayGraphicsPassPlan(), ResolveSubmissionEncoderBoundary() and
  // TakePendingResourceBarrierBatch() now live in
  // d3d12_replay_pass_batch_ops.hpp; unqualified calls below resolve to
  // dxmt::d3d12.

  // EncodeResourceAccessBarrierEntriesForStage(),
  // EncodeResourceAccessBarrierEntries(),
  // EncodeRenderResourceAccessBarrierEntries(),
  // RenderResourceAccessBarrierEntriesRequireReverseBoundary(),
  // EncodeInlineRenderResourceBarrierEntries() and
  // RecordSeparatorOnlyBarrier() now live in d3d12_replay_barrier_encode.hpp;
  // unqualified calls below resolve to dxmt::d3d12.

  void FlushComputePassBatch(CommandChunk *chunk, ReplayState &state) {
    auto &batch = state.compute_pass_batch;
    if (!HasPendingComputePass(state))
      return;
    const bool _rb = ReplayPerfEnabled();
    if (_rb) perDrawSubTimers().flushComputeCalls++;
    ScopeAccum _rbacc{_rb, _rb ? clock::now() : clock::time_point{},
                      &perDrawSubTimers().flushComputeUs};

    auto barriers = TakePendingResourceBarrierBatch(state);
    RecordSeparatorOnlyBarrier(barriers);
    ReplayCommandStorage<ReplayComputePassCommand> command_storage{
        std::move(batch.encoder_arena), std::move(batch.commands)};

    chunk->emitcc([this, command_storage = std::move(command_storage),
                   barrier_entries = std::move(barriers.entries),
                   argument_buffer_size = batch.argument_buffer_size](
                      ArgumentEncodingContext &enc) mutable {
      enc.startComputePass(argument_buffer_size);
      EncodeResourceAccessBarrierEntries(enc, barrier_entries);
      if (!barrier_entries.empty())
        enc.resolveComputePassBarrier();
      for (auto &command : command_storage.commands) {
        if (command.d3d_sequence != 0)
          dxmt::apitrace::set_current_d3d_sequence(command.d3d_sequence);
        uint64_t argbuf_offset = command.argument_buffer_offset;
        enc.setArgumentBufferWriteRange(command.argument_buffer_offset,
                                        command.argument_buffer_size);
        command.encoder->Encode(*this, enc, argbuf_offset);
        const bool cursor_underflow =
            argbuf_offset < command.argument_buffer_offset;
        const bool cursor_overflow =
            !cursor_underflow &&
            argbuf_offset - command.argument_buffer_offset >
                command.argument_buffer_size;
        if (enc.argumentBufferOverflowed() || cursor_underflow ||
            cursor_overflow) {
          WARN("D3D12CommandQueue: compute argument-buffer reservation overflow"
               " reserved=", command.argument_buffer_size,
               " begin=", command.argument_buffer_offset,
               " end=", argbuf_offset);
        }
      }
      enc.endPass();
    });

    batch = {};
  }

  void FlushGraphicsPassBatch(CommandChunk *chunk, ReplayState &state) {
    auto &batch = state.graphics_pass_batch;
    if (!HasPendingGraphicsPass(state))
      return;
    const bool _rb = ReplayPerfEnabled();
    if (_rb) perDrawSubTimers().flushGraphicsCalls++;
    ScopeAccum _rbacc{_rb, _rb ? clock::now() : clock::time_point{},
                      &perDrawSubTimers().flushGraphicsUs};

    auto barriers = TakePendingResourceBarrierBatch(state);
    RecordSeparatorOnlyBarrier(barriers);

    {
      ScopeAccum _rbplan{_rb, _rb ? clock::now() : clock::time_point{},
                         &perDrawSubTimers().buildPlanUs};
      batch.plan = BuildReplayGraphicsPassPlan(batch.commands,
                                               batch.argument_buffer_size);
      // The statistics block is only reachable through device_, so the lookup
      // stays here; the fold itself lives in
      // d3d12_replay_frame_stats_attribution.hpp.
      if (auto *stats = dxmt::perf::enabled()
                            ? &device_->GetDXMTDevice()
                                   .queue()
                                   .CurrentFrameStatistics()
                            : nullptr)
        RecordReplayGraphicsPassPlanStats(stats, batch.plan,
                                          clock::now() - _rbplan.t0);
    }

    auto pass_recipe = BuildCompiledRenderPassRecipe(
        std::move(batch.attachments), batch.use_geometry,
        batch.use_tessellation);
    ReplayCommandStorage<ReplayGraphicsPassCommand> command_storage{
        std::move(batch.encoder_arena), std::move(batch.commands)};
    chunk->emitcc([this, pass_recipe = std::move(pass_recipe),
                   command_storage = std::move(command_storage),
                   plan = batch.plan,
                   barrier_entries = std::move(barriers.entries),
                   argument_buffer_size = batch.argument_buffer_size](
                      ArgumentEncodingContext &enc) mutable {
      if (!BeginCompiledRenderPass(enc, pass_recipe,
                                   argument_buffer_size)) {
        return;
      }
      EncodeRenderResourceAccessBarrierEntries(enc, barrier_entries);
      for (auto &command : command_storage.commands) {
        if (command.d3d_sequence != 0) {
          dxmt::apitrace::set_current_d3d_sequence(command.d3d_sequence);
        }
        if (ReplayGraphicsCommandKindIsRealWork(command.kind))
          enc.bumpVisibilityResultOffset();
        uint64_t argbuf_offset = command.argument_buffer_offset;
        enc.setArgumentBufferWriteRange(command.argument_buffer_offset,
                                        command.argument_buffer_size);
        if (command.bindless_compiled_candidate) {
          const bool compiled_encode_perf = dxmt::perf::enabled();
          const auto compiled_encode_begin =
              compiled_encode_perf ? clock::now() : clock::time_point{};
          command.encoder->Encode(*this, enc, argbuf_offset);
          if (compiled_encode_perf) {
            dxmt::perf::recordCompiledDrawEncodeTime(
                &enc.currentFrameStatistics(),
                clock::now() - compiled_encode_begin);
          }
        } else {
          command.encoder->Encode(*this, enc, argbuf_offset);
        }
        const bool cursor_underflow =
            argbuf_offset < command.argument_buffer_offset;
        const auto command_used =
            cursor_underflow
                ? UINT64_MAX
                : argbuf_offset - command.argument_buffer_offset;
        if (enc.argumentBufferOverflowed() || cursor_underflow ||
            command_used > command.argument_buffer_size) {
          WARN("D3D12CommandQueue: graphics pass argument-buffer reservation overflow"
               " commandIndex=", command.command_index,
               " reserved=", command.argument_buffer_size,
               " used=", command_used,
               " passCommands=", plan.command_count);
        }
      }
      enc.endPass();
    });

    batch = {};
  }

  // EmitTimestampMarkers() and FlushBlitBatch() now live in
  // d3d12_replay_pass_flush_ops.hpp; unqualified calls below resolve to
  // dxmt::d3d12. FlushComputePassBatch() and FlushGraphicsPassBatch() stay here
  // because their emitcc closures capture `this` for
  // ReplayPassEncodeCommand::Encode(CommandQueueImpl &, ...), and the graphics
  // one also reads device_.

  // TakeMatchingPendingResourceBarriers() now lives in
  // d3d12_replay_pass_batch_ops.hpp; unqualified calls below resolve to
  // dxmt::d3d12.

  void FlushReplayRenderComputeTimestampBatches(CommandChunk *chunk,
                                                ReplayState &state) {
    FlushComputePassBatch(chunk, state);
    FlushGraphicsPassBatch(chunk, state);
    EmitTimestampMarkers(chunk, state);
  }

  void FlushReplayWorkBeforeResourceBarrier(CommandChunk *chunk,
                                            ReplayState &state) {
    if (HasPendingReplayWork(state))
      FlushPassBatches(chunk, state);
  }

  void FlushPassBatches(CommandChunk *chunk, ReplayState &state) {
    StallScope _ss(StallDiagEnabled(), &stallProbe().emitUs);
    ResolveSubmissionEncoderBoundary(
        state, CompiledEncoderKind::None, false);
    const bool has_encoder_work =
        HasPendingBlitBatch(state) || HasPendingComputePass(state) ||
        HasPendingGraphicsPass(state);
    if (ReplayPerfEnabled()) {
      perDrawSubTimers().flushPassBatchesCalls++;
      if (!has_encoder_work)
        perDrawSubTimers().flushPassBatchesEmpty++;
    }
    FlushBlitBatch(chunk, state);
    FlushReplayRenderComputeTimestampBatches(chunk, state);
  }

  void FlushGraphicsBeforeCompute(CommandChunk *chunk, ReplayState &state) {
    if (HasPendingGraphicsPass(state) || HasPendingBlitBatch(state))
      FlushPassBatches(chunk, state);
  }

  // ReplayBlitBatchHasHazard() and ReplayBlitBatchTrackAccess() now live in
  // d3d12_replay_blit_hazard.hpp; unqualified calls below resolve to
  // dxmt::d3d12.

  template <typename Fn>
  void QueueBlitCommand(CommandChunk *chunk, ReplayState &state,
                        std::initializer_list<ID3D12Resource *> reads,
                        std::initializer_list<ID3D12Resource *> writes,
                        Fn &&fn) {
    const bool perf_enabled = ReplayPerfEnabled();
    if (perf_enabled)
      perDrawSubTimers().queueBlitCalls++;
    if (HasPendingComputePass(state) || HasPendingGraphicsPass(state) ||
        !state.pending_timestamp_markers.empty())
      FlushReplayRenderComputeTimestampBatches(
          chunk, state);
    const auto hazard_begin =
        perf_enabled ? clock::now() : clock::time_point{};
    const bool has_hazard =
        ReplayBlitBatchHasHazard(state.blit_batch, reads, writes);
    if (perf_enabled) {
      perDrawSubTimers().queueBlitHazardUs +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - hazard_begin)
              .count();
      if (has_hazard)
        perDrawSubTimers().queueBlitHazardFlushes++;
    }
    if (has_hazard)
      FlushBlitBatch(chunk, state);

    const auto track_begin =
        perf_enabled ? clock::now() : clock::time_point{};
    ReplayBlitBatchTrackAccess(state.blit_batch, reads, writes);
    if (perf_enabled) {
      perDrawSubTimers().queueBlitTrackUs +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - track_begin)
              .count();
    }
    const auto append_begin =
        perf_enabled ? clock::now() : clock::time_point{};
    using Encode = std::decay_t<Fn>;
    ReplayBlitPassCommand command;
    command.encoder = AllocateReplayEncoder<
        ReplayBlitEncodeCommand, ReplayOwnedBlitEncodeCommand<Encode>>(
        state.blit_batch.encoder_arena, std::forward<Fn>(fn));
    command.d3d_sequence = dxmt::apitrace::current_d3d_sequence();
    state.blit_batch.commands.push_back(std::move(command));
    if (perf_enabled) {
      perDrawSubTimers().queueBlitAppendUs +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - append_begin)
              .count();
    }
  }

