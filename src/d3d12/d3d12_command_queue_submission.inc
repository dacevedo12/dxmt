// Submission queue plumbing, pending operations, and fence signalling.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // SubmissionCaptureStatistics lives in d3d12_submission_capture_stats.hpp.
  //
  // PendingOperation, its payload alternatives, PendingOperationTypeName and
  // the ordering rules of the pending-operation FIFO live in
  // d3d12_submission_pending_operation.hpp.
  //
  // FenceGpuWaitStatusName and the DXMT_DIAG_COMMAND_QUEUE drain trace points
  // live in d3d12_submission_drain_diag.hpp.

  void EnqueuePendingOperation(PendingOperation &&operation,
                               dxmt::FrameStatistics *perf_stats) {
    dxmt::perf::ScopedCodeTimer code_timer(
        dxmt::PerfCodePath::QueueEnqueueControl);
    auto t0 = clock::now();
    operation.lifecycle_pair_id = dxmt::lifecycle::nextPairSequence();
    operation.queue_lifecycle_id =
        lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
    {
      dxmt::perf::ScopedCodeTimer phase_timer(
          dxmt::PerfCodePath::QueueEnqueueFrameTag);
      if (operation.frame_id == kUnresolvedSubmissionFrameId)
        operation.frame_id = PreviousFrameSeq(
            device_->GetDXMTDevice().queue().CurrentFrameSeq());
    }
    const auto lifecycle_pair_id = operation.lifecycle_pair_id;
    const auto queue_lifecycle_id = operation.queue_lifecycle_id;
    const auto operation_type = operation.type();
    const auto operation_frame = operation.frame_id;
    const auto wait_sequence = operation.value();
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type, "enqueue.enter",
        PendingOperationTypeName(operation_type), lifecycle_pair_id,
        queue_lifecycle_id, operation_frame, 0, 0, wait_sequence,
        submitted_batches_.load(std::memory_order_relaxed));
    {
      dxmt::perf::ScopedCodeTimer phase_timer(
          dxmt::PerfCodePath::QueueEnqueueLock);
      std::lock_guard lock(mutex_);
      if (operation.is<FenceWaitSubmission>())
        has_waited_.store(true, std::memory_order_relaxed);
      pending_operations_.push_back(std::move(operation));
    }
    dxmt::perf::recordExecuteTime(
        perf_stats, dxmt::perf::ExecuteTimeBucket::Enqueue,
        clock::now() - t0);
    {
      dxmt::perf::ScopedCodeTimer phase_timer(
          dxmt::PerfCodePath::QueueEnqueueNotify);
      submission_wake_state_->WakeOne();
    }
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type, "enqueue.leave",
        PendingOperationTypeName(operation_type), lifecycle_pair_id,
        queue_lifecycle_id, operation_frame, 0, 0, wait_sequence,
        submitted_batches_.load(std::memory_order_relaxed));
  }

  size_t PendingOperationCountForDiag() {
    std::lock_guard lock(mutex_);
    return pending_operations_.size();
  }

  // The replay performance ledger is a ReplayPerfFrameAccumulator member; see
  // d3d12_replay_perf_frame.hpp.

  void AssertSubmissionStateLocked() const noexcept
      DXMT_ASSERT_CAPABILITY(mutex_) { mutex_.assert_held(); }

  void FlushPendingOperations() {
    if (device_queue_state_->SubmissionService()->IsWorkerThread())
      return;

    const auto pair_id = dxmt::lifecycle::nextPairSequence();
    const auto queue_pair_id =
        lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type,
        "caller-drain-wait.enter", "flush", pair_id, queue_pair_id, ~0ull, 0,
        0, submitted_batches_.load(std::memory_order_relaxed), 0);
    std::unique_lock lock(mutex_);
    AssertSubmissionStateLocked();
    // The predicate is deliberately unchanged and still has no stop condition:
    // this is a drain, and returning early would let ResizeBuffers -- the only
    // caller -- proceed past work that has not landed.  What is new is that we
    // surface the two shapes in which it can never be satisfied.
    //
    // The worker parks (clears submission_worker_active_ while the queue is
    // still non-empty) whenever the head is a fence wait that is not satisfied
    // yet, and the only thing that can move it again is CompleteFenceWait.  A
    // wait that is never signalled -- an application bug, or two queues waiting
    // on each other -- therefore blocks here forever.  On a real device that
    // situation stalls the GPU while ResizeBuffers still returns; here it
    // freezes the thread that pumps window messages, with nothing in the log.
    //
    // Waking up to report costs nothing on the success path: the wait still
    // only exits when the predicate holds.
    constexpr auto kStallReportInterval = std::chrono::seconds(2);
    unsigned stall_intervals = 0;
    size_t last_pending = 0;
    unsigned unchanged_intervals = 0;
    while (!submission_wake_state_->condition.wait_for(
        lock, kStallReportInterval, [this]() {
          AssertSubmissionStateLocked();
          return pending_operations_.empty() && !submission_worker_active_;
        })) {
      AssertSubmissionStateLocked();
      stall_intervals++;
      const auto pending = pending_operations_.size();
      unchanged_intervals = pending == last_pending ? unchanged_intervals + 1 : 0;
      last_pending = pending;
      const auto waited_seconds =
          stall_intervals * kStallReportInterval.count();
      // These two counters exist for exactly this moment and are read nowhere
      // else; they name the fence pair and value the worker parked on.
      const auto dependency_pair =
          submission_worker_dependency_pair_.load(std::memory_order_relaxed);
      const auto dependency_value =
          submission_worker_dependency_value_.load(std::memory_order_relaxed);
      if (submission_worker_waiting_for_wait_ && !submission_worker_active_) {
        ERR("D3D12CommandQueue: swap chain resize has been blocked for ",
            waited_seconds,
            "s because the submission worker is parked on a fence wait that "
            "has not been signalled; queue=",
            reinterpret_cast<uintptr_t>(this), " type=", desc_.Type,
            " pending=", pending, " fencePair=", dependency_pair,
            " awaitedValue=", dependency_value);
      } else if (unchanged_intervals >= 1) {
        // Not parked, yet nothing has drained across two consecutive reports.
        // The submission worker is shared by every queue on the device, so this
        // is what a stalled GPU looks like from here: the sequencer is pinned
        // in chunk back-pressure and never returns to clear the queue.
        ERR("D3D12CommandQueue: swap chain resize has been blocked for ",
            waited_seconds,
            "s with no drain progress; queue=",
            reinterpret_cast<uintptr_t>(this), " type=", desc_.Type,
            " pending=", pending, " workerActive=", submission_worker_active_,
            " fencePair=", dependency_pair,
            " awaitedValue=", dependency_value);
      }
    }
    AssertSubmissionStateLocked();
    lock.unlock();
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type,
        "caller-drain-wait.leave", "flush", pair_id, queue_pair_id, ~0ull, 0,
        0, submitted_batches_.load(std::memory_order_relaxed), 0);
  }

  void StopSubmissionService() {
    if (device_queue_state_->SubmissionService()->IsWorkerThread()) {
      ERR("D3D12CommandQueue: queue destruction attempted on the device "
          "submission service");
      std::terminate();
    }

    std::deque<PendingOperation> cancelled;
    {
      std::lock_guard lock(mutex_);
      if (!submission_worker_stopping_) {
        cancelled = CancelPendingOperationsFromBlockingWait(
            mutex_, pending_operations_);
        PendingOperation stop(StopSubmission{});
        stop.lifecycle_pair_id = dxmt::lifecycle::nextPairSequence();
        stop.queue_lifecycle_id =
            lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
        pending_operations_.push_back(std::move(stop));
        submission_worker_stopping_ = true;
        ClearSubmissionWaitDependency(mutex_,
                                      submission_worker_waiting_for_wait_,
                                      submission_worker_dependency_pair_,
                                      submission_worker_dependency_value_);
      }
    }
    CompleteCancelledPendingOperations(reinterpret_cast<uintptr_t>(this),
                                       desc_.Type, cancelled);
    submission_wake_state_->WakeAll();

    const auto pair_id = dxmt::lifecycle::nextPairSequence();
    const auto queue_pair_id =
        lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type,
        "worker-join-wait.enter", "stop", pair_id, queue_pair_id, ~0ull, 0,
        0, 0, 0);
    {
      std::unique_lock lock(mutex_);
      AssertSubmissionStateLocked();
      submission_wake_state_->condition.wait(lock, [this]() {
        AssertSubmissionStateLocked();
        return submission_service_stopped_ && !submission_worker_active_;
      });
      AssertSubmissionStateLocked();
    }
    if (auto endpoint = submission_endpoint_.lock())
      device_queue_state_->SubmissionService()->UnregisterQueue(*endpoint);
    submission_endpoint_.reset();
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type,
        "worker-join-wait.leave", "stop", pair_id, queue_pair_id, ~0ull, 0,
        0, 0, 0);
  }

  [[nodiscard]] bool ProcessReadySubmission() noexcept {
    {
      std::lock_guard lock(mutex_);
      if (!SubmissionDrainCanStart(mutex_, pending_operations_,
                                   submission_worker_active_,
                                   submission_service_stopped_,
                                   submission_worker_waiting_for_wait_))
        return false;
      submission_worker_active_ = true;
    }

    const auto drain_pair_id = dxmt::lifecycle::nextPairSequence();
    const auto drain_queue_pair_id =
        lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type, "worker-drain.enter",
        "device-sequencer", drain_pair_id, drain_queue_pair_id, ~0ull, 0, 0,
        submitted_batches_.load(std::memory_order_relaxed), 0);

    const bool replay_ok = dxmt::invokeNoexcept(
        "D3D12 logical queue submission",
        [this]() { DrainPendingOperations(); });

    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type, "worker-drain.leave",
        "device-sequencer", drain_pair_id, drain_queue_pair_id, ~0ull, 0, 0,
        submitted_batches_.load(std::memory_order_relaxed),
        replay_ok ? 1 : 0);
    {
      std::lock_guard lock(mutex_);
      submission_worker_active_ = false;
      submission_service_stopped_ =
          submission_worker_stopping_ && pending_operations_.empty();
    }
    submission_wake_state_->condition.notify_all();
    if (!replay_ok)
      std::terminate();
    return true;
  }

  void DrainPendingOperations() {
    for (;;) {
      PendingOperation operation(StopSubmission{});
      dxmt::FrameStatistics *operation_perf_stats = nullptr;
      bool has_operation = false;
      Fence *wait_fence = nullptr;
      UINT64 wait_value = 0;
      std::shared_ptr<QueueWaitState> wait_completion;
      bool arm_wait_callback = false;
      bool resolved_gpu_wait = false;
      FenceGpuSignal gpu_wait_signal = {};
      FenceGpuWaitStatus gpu_wait_status = FenceGpuWaitStatus::Unknown;
      uint64_t wait_lifecycle_pair_id = 0;
      uint64_t wait_queue_lifecycle_id = 0;
      uint64_t wait_frame_id = kUnresolvedSubmissionFrameId;

      auto lock_begin = clock::now();
      {
        std::lock_guard lock(mutex_);
        if (pending_operations_.empty()) {
          return;
        }

        auto &front = pending_operations_.front();
        wait_lifecycle_pair_id = front.lifecycle_pair_id;
        wait_queue_lifecycle_id = front.queue_lifecycle_id;
        wait_frame_id = front.frame_id;
        auto *front_wait = std::get_if<FenceWaitSubmission>(&front.payload);
        const bool callback_completed =
            front_wait && front_wait->completion &&
            front_wait->completion->completed();
        if (front_wait &&
            !QueuedFenceWaitIsSatisfied(front_wait->fence.get(),
                                        front_wait->value,
                                        callback_completed)) {
          gpu_wait_status =
              front_wait->fence.get()->TryResolveGpuWait(
                  front_wait->value, gpu_wait_signal);
          if (gpu_wait_status == FenceGpuWaitStatus::Resolved) {
            ClearSubmissionWaitDependency(
                mutex_, submission_worker_waiting_for_wait_,
                submission_worker_dependency_pair_,
                submission_worker_dependency_value_);
            operation = std::move(front);
            pending_operations_.pop_front();
            has_operation = true;
            resolved_gpu_wait = gpu_wait_signal.event;
          } else {
            LogQueueWaitFallbackCpu(
                reinterpret_cast<uintptr_t>(this), desc_.Type,
                reinterpret_cast<uintptr_t>(front_wait->fence.get()),
                front_wait->value, gpu_wait_status,
                front_wait->callback_armed, pending_operations_.size());
            if (!front_wait->callback_armed) {
              front_wait->callback_armed = true;
              front_wait->completion =
                  std::make_shared<QueueWaitState>(
                      submission_wake_state_,
                      reinterpret_cast<uintptr_t>(this), desc_.Type,
                      front.lifecycle_pair_id, front.queue_lifecycle_id,
                      front.frame_id, front_wait->value);
              wait_completion = front_wait->completion;
              wait_fence = front_wait->fence.get();
              // StopSubmissionWorker may erase the queued Wait as soon as we
              // drop mutex_. Keep the fence alive until callback registration
              // finishes; the registration stores only a weak wait target.
              wait_fence->AddRefPrivate();
              wait_value = front_wait->value;
              arm_wait_callback = true;
            }
            SetSubmissionWaitDependency(
                mutex_, submission_worker_waiting_for_wait_,
                submission_worker_dependency_pair_,
                submission_worker_dependency_value_, front.lifecycle_pair_id,
                front_wait->value);
          }
        } else {
          ClearSubmissionWaitDependency(mutex_,
                                        submission_worker_waiting_for_wait_,
                                        submission_worker_dependency_pair_,
                                        submission_worker_dependency_value_);
          operation = std::move(front);
          pending_operations_.pop_front();
          has_operation = true;
        }
      }
      const auto lock_end = clock::now();
      const auto dequeue_lock_interval = lock_end - lock_begin;

      if (arm_wait_callback) {
        const auto arm_begin = clock::now();
        const auto queue_identity = reinterpret_cast<uintptr_t>(this);
        const auto queue_type = desc_.Type;
        D3D12SubmissionLifecycleLog(
            queue_identity, queue_type, "fence-wait-register.enter", "wait",
            wait_lifecycle_pair_id, wait_queue_lifecycle_id, wait_frame_id, 0,
            0, wait_value,
            submitted_batches_.load(std::memory_order_relaxed));
        wait_fence->RegisterQueueWait(
            wait_value,
            std::weak_ptr<FenceWaitTarget>(wait_completion));
        wait_fence->ReleasePrivate();
        D3D12SubmissionLifecycleLog(
            queue_identity, queue_type, "fence-wait-register.leave", "wait",
            wait_lifecycle_pair_id, wait_queue_lifecycle_id, wait_frame_id, 0,
            0, wait_value, 0);
        RecordExecuteDrainTime(
            operation_perf_stats, dxmt::perf::ExecuteTimeBucket::WaitArm,
            clock::now() - arm_begin);
        return;
      }

      if (!has_operation)
        return;

      const auto operation_type = operation.type();
      const auto operation_name = PendingOperationTypeName(operation_type);
      const auto operation_lifecycle_pair_id = operation.lifecycle_pair_id;
      const auto operation_queue_lifecycle_id = operation.queue_lifecycle_id;
      const auto operation_frame_id = operation.frame_id;
      const auto operation_value = operation.value();
      const auto operation_chunk =
          device_->GetDXMTDevice().queue().CurrentSeqId();
      D3D12SubmissionLifecycleLog(
          reinterpret_cast<uintptr_t>(this), desc_.Type, "operation.enter",
          operation_name, operation.lifecycle_pair_id,
          operation.queue_lifecycle_id, operation.frame_id, operation_chunk,
          0, operation.value(),
          resolved_gpu_wait ? gpu_wait_signal.dxmt_chunk
                            : submitted_batches_.load(
                                  std::memory_order_relaxed));

      if (resolved_gpu_wait) {
        has_waited_.store(true, std::memory_order_relaxed);
        auto &queue = device_->GetDXMTDevice().queue();
        auto *chunk = queue.CurrentChunk();
        const auto wait_chunk_id = queue.CurrentSeqId();
        const auto wait_chunk_event_id =
            NextChunkEventSeqId(queue.GetCurrentEventSeqId());
        const auto wait_frame = operation.frame_id;
        const auto waited_value = operation.value();
        chunk->emitcc([event = std::move(gpu_wait_signal.event),
                       waited_value](ArgumentEncodingContext &enc) mutable {
          enc.waitEvent(std::move(event), waited_value);
        });
        LogQueueWaitResolvedGpu(
            reinterpret_cast<uintptr_t>(this), desc_.Type,
            reinterpret_cast<uintptr_t>(
                operation.get<FenceWaitSubmission>().fence.get()),
            operation.value(), gpu_wait_signal, wait_chunk_id,
            wait_chunk_event_id, wait_frame);
        D3D12SubmissionLifecycleLog(
            reinterpret_cast<uintptr_t>(this), desc_.Type, "operation.leave",
            operation_name, operation.lifecycle_pair_id,
            operation.queue_lifecycle_id, operation.frame_id, wait_chunk_id,
            0, operation.value(), gpu_wait_signal.dxmt_chunk);
        continue;
      }

      switch (operation_type) {
      case PendingOperationType::Stop:
        FlushReplayPerfFrame(replay_perf_frame_,
                             reinterpret_cast<uintptr_t>(this), desc_.Type);
        D3D12SubmissionLifecycleLog(
            reinterpret_cast<uintptr_t>(this), desc_.Type, "operation.leave",
            operation_name, operation.lifecycle_pair_id,
            operation.queue_lifecycle_id, operation.frame_id,
            operation_chunk, 0, operation.value(), 0);
        return;
      case PendingOperationType::Execute: {
        auto &execute = operation.get<ExecuteSubmission>();
        operation_perf_stats = BeginReplayPerfFrame(
            replay_perf_frame_, operation.frame_id,
            reinterpret_cast<uintptr_t>(this), desc_.Type);
        dxmt::perf::ScopedFrameStatisticsBinding perf_stats_binding(
            operation_perf_stats);
        if (operation_perf_stats)
          AccumulateSubmissionCaptureStatistics(*operation_perf_stats,
                                                execute.capture_statistics);
        RecordExecuteDrainTime(
            operation_perf_stats,
            dxmt::perf::ExecuteTimeBucket::DrainLock,
            dequeue_lock_interval);
        LogDrainExecuteBegin(
            reinterpret_cast<uintptr_t>(this), desc_.Type,
            execute.submitted_command_lists.size(),
            submitted_batches_.load(std::memory_order_relaxed));
        // Immutable binding plans are keyed by monotonic root-signature and
        // pipeline identities. They survive submit batches and are invalidated
        // individually by identity replacement or bounded eviction.
        std::vector<Com<ID3D12Resource>> touched_resources;
        std::unordered_set<ID3D12Resource *> touched_resources_set;
        ResourceAccessBarrierBatch queue_pending_barriers =
            std::move(pending_queue_resource_barriers_);
        ReplayState submission_replay_state = {};
        submission_replay_state.pending_resource_barriers =
            std::move(queue_pending_barriers);
        queue_pending_barriers = {};
        submission_replay_state.resource_states =
            &device_queue_state_->BackendResourceStates();
        submission_replay_state.queue_type = desc_.Type;
        submission_replay_state.touched_resources = &touched_resources;
        submission_replay_state.touched_resources_set =
            &touched_resources_set;
        const size_t last_compiled_list_index =
            LastCompiledSubmittedListIndex(execute.submitted_command_lists);
        UINT command_list_index = 0;
        for (const auto &submitted_plan : execute.submitted_command_lists) {
          const SubmittedCompiledCommandListPlan *submitted =
              submitted_plan.get();
          const CompiledCommandList *compiled =
              SubmittedCompiledGeneration(submitted);
          const auto compiled_shape = DescribeCompiledCommandList(compiled);
          const auto record_count = compiled_shape.record_count;
          const CompiledCommandDescriptorSnapshots *descriptor_snapshots =
              SubmittedDescriptorSnapshotsAt(
                  execute.compiled_descriptor_snapshots, command_list_index);
          LogDrainReplayListBegin(
              reinterpret_cast<uintptr_t>(this), desc_.Type,
              command_list_index, record_count, compiled_shape.segments,
              compiled_shape.graphics_packets,
              compiled_shape.compute_packets,
              submitted_batches_.load(std::memory_order_relaxed));
          const auto replay_begin = clock::now();
          ExecuteCompiledCommandList(submitted, descriptor_snapshots,
                                     submission_replay_state,
                                     command_list_index ==
                                         last_compiled_list_index,
                                     queue_pending_barriers);
          RecordExecuteDrainTime(
              operation_perf_stats, dxmt::perf::ExecuteTimeBucket::Replay,
              clock::now() - replay_begin);
          LogDrainReplayListEnd(
              reinterpret_cast<uintptr_t>(this), desc_.Type,
              command_list_index, record_count, touched_resources.size(),
              submitted_batches_.load(std::memory_order_relaxed));
          command_list_index++;
        }
        if (last_compiled_list_index == SIZE_MAX)
          queue_pending_barriers = TakePendingResourceBarrierBatch(
              submission_replay_state);
        pending_queue_resource_barriers_ =
            std::move(queue_pending_barriers);
        LogDrainDecayBegin(
            reinterpret_cast<uintptr_t>(this), desc_.Type,
            touched_resources.size(),
            submitted_batches_.load(std::memory_order_relaxed));
        const auto decay_begin = clock::now();
        DecayTouchedResourceStates(touched_resources);
        RecordExecuteDrainTime(
            operation_perf_stats, dxmt::perf::ExecuteTimeBucket::Decay,
            clock::now() - decay_begin);
        LogDrainDecayEnd(
            reinterpret_cast<uintptr_t>(this), desc_.Type,
            touched_resources.size(),
            submitted_batches_.load(std::memory_order_relaxed));
        auto &dxmt_queue = device_->GetDXMTDevice().queue();
        auto *chunk = dxmt_queue.CurrentChunk();
        const auto completion_chunk = dxmt_queue.CurrentSeqId();
        const auto queue_identity = reinterpret_cast<uintptr_t>(this);
        const auto queue_type = desc_.Type;
        const auto completion_pair_id = operation.lifecycle_pair_id;
        const auto completion_queue_pair_id = operation.queue_lifecycle_id;
        const auto completion_frame = operation.frame_id;
        D3D12SubmissionLifecycleLog(
            queue_identity, queue_type, "completion-link.enter", "execute",
            completion_pair_id, completion_queue_pair_id, completion_frame,
            completion_chunk, 0, completion_chunk, 0);
        EnqueueRetirement(
            chunk,
            AllocatorRetirementWork{
                .uses = std::move(execute.allocator_uses),
                .queue_identity = queue_identity,
                .queue_type = queue_type,
                .lifecycle_pair_id = completion_pair_id,
                .queue_lifecycle_id = completion_queue_pair_id,
                .frame = completion_frame,
                .chunk = completion_chunk,
            });
        D3D12SubmissionLifecycleLog(
            queue_identity, queue_type, "completion-link.leave", "execute",
            completion_pair_id, completion_queue_pair_id, completion_frame,
            completion_chunk, 0, completion_chunk, 0);

        std::vector<PendingOperation> coalesced_signals;
        {
          lock_begin = clock::now();
          std::lock_guard lock(mutex_);
          TakeCoalescedFenceSignals(mutex_, pending_operations_,
                                    coalesced_signals);
          RecordExecuteDrainTime(
              operation_perf_stats, dxmt::perf::ExecuteTimeBucket::DrainLock,
              clock::now() - lock_begin);
        }
        const auto signal_begin = clock::now();
        for (auto &signal : coalesced_signals) {
          auto &payload = signal.get<FenceSignalSubmission>();
          EncodeFenceSignal(payload.fence.get(), payload.value, "coalesced",
                            operation.frame_id, signal.lifecycle_pair_id,
                            signal.queue_lifecycle_id);
        }
        RecordExecuteDrainTime(
            operation_perf_stats, dxmt::perf::ExecuteTimeBucket::Signal,
            clock::now() - signal_begin);

        LogDrainCommitBegin(
            reinterpret_cast<uintptr_t>(this), desc_.Type,
            submitted_batches_.load(std::memory_order_relaxed),
            coalesced_signals.size());
        const auto commit_begin = clock::now();
        device_->GetDXMTDevice().queue().CommitCurrentChunkForFrame(
            operation.frame_id);
        RecordExecuteDrainTime(
            operation_perf_stats, dxmt::perf::ExecuteTimeBucket::Commit,
            clock::now() - commit_begin);
        LogDrainCommitEnd(
            reinterpret_cast<uintptr_t>(this), desc_.Type,
            submitted_batches_.load(std::memory_order_relaxed));
        const auto submitted_batches =
            submitted_batches_.fetch_add(1, std::memory_order_relaxed) + 1;
        LogDrainExecuteSubmitted(reinterpret_cast<uintptr_t>(this),
                                 desc_.Type, submitted_batches);
        break;
      }
      case PendingOperationType::QueueWork: {
        auto &queue_work = operation.get<QueueWorkSubmission>();
        auto *chunk = device_->GetDXMTDevice().queue().CurrentChunk();
        const bool has_gpu_work = std::visit(
            QueueWorkReplayVisitor(*this, chunk), queue_work.work);

        std::vector<PendingOperation> coalesced_signals;
        {
          lock_begin = clock::now();
          std::lock_guard lock(mutex_);
          if (has_gpu_work)
            TakeCoalescedFenceSignals(mutex_, pending_operations_,
                                      coalesced_signals);
          RecordExecuteDrainTime(
              operation_perf_stats, dxmt::perf::ExecuteTimeBucket::DrainLock,
              clock::now() - lock_begin);
        }

        if (has_gpu_work) {
          const auto signal_begin = clock::now();
          for (auto &signal : coalesced_signals) {
            auto &payload = signal.get<FenceSignalSubmission>();
            EncodeFenceSignal(payload.fence.get(), payload.value, "coalesced",
                              operation.frame_id, signal.lifecycle_pair_id,
                              signal.queue_lifecycle_id);
          }
          RecordExecuteDrainTime(
              operation_perf_stats, dxmt::perf::ExecuteTimeBucket::Signal,
              clock::now() - signal_begin);

          const auto commit_begin = clock::now();
          device_->GetDXMTDevice().queue().CommitCurrentChunkForFrame(
              operation.frame_id);
          RecordExecuteDrainTime(
              operation_perf_stats, dxmt::perf::ExecuteTimeBucket::Commit,
              clock::now() - commit_begin);
          submitted_batches_.fetch_add(1, std::memory_order_relaxed);
        }
        break;
      }
      case PendingOperationType::Signal: {
        std::vector<PendingOperation> signals;
        signals.push_back(std::move(operation));
        {
          lock_begin = clock::now();
          std::lock_guard lock(mutex_);
          TakeCoalescedFenceSignals(mutex_, pending_operations_, signals);
          RecordExecuteDrainTime(
              operation_perf_stats,
              dxmt::perf::ExecuteTimeBucket::DrainLock,
              clock::now() - lock_begin);
        }
        {
          const auto &first_signal =
              signals.front().get<FenceSignalSubmission>();
          const auto &last_signal =
              signals.back().get<FenceSignalSubmission>();
          LogDrainSignalBatch(
              reinterpret_cast<uintptr_t>(this), desc_.Type, signals.size(),
              reinterpret_cast<uintptr_t>(first_signal.fence.get()),
              first_signal.value,
              reinterpret_cast<uintptr_t>(last_signal.fence.get()),
              last_signal.value);
        }
        const auto signal_begin = clock::now();
        SubmitFenceSignals(std::move(signals));
        RecordExecuteDrainTime(
            operation_perf_stats, dxmt::perf::ExecuteTimeBucket::Signal,
            clock::now() - signal_begin);
        break;
      }
      case PendingOperationType::Wait:
        {
          const auto &wait = operation.get<FenceWaitSubmission>();
          LogDrainWaitSatisfied(
              reinterpret_cast<uintptr_t>(this), desc_.Type,
              reinterpret_cast<uintptr_t>(wait.fence.get()), wait.value);
        }
        break;
      }
      D3D12SubmissionLifecycleLog(
          reinterpret_cast<uintptr_t>(this), desc_.Type, "operation.leave",
          operation_name, operation_lifecycle_pair_id,
          operation_queue_lifecycle_id, operation_frame_id,
          device_->GetDXMTDevice().queue().CurrentSeqId(), 0,
          operation_value,
          submitted_batches_.load(std::memory_order_relaxed));
    }
  }

  void SubmitFenceSignals(std::vector<PendingOperation> signals) {
    if (signals.empty())
      return;

    if (!submitted_batches_.load(std::memory_order_relaxed) &&
        !has_waited_.load(std::memory_order_relaxed)) {
      SignalFenceBatchImmediately(reinterpret_cast<uintptr_t>(this),
                                  desc_.Type, signals, signal_count_,
                                  last_signal_value_);
      return;
    }

    for (auto &signal : signals) {
      auto &payload = signal.get<FenceSignalSubmission>();
      if (!payload.fence.get())
        continue;

      EncodeFenceSignal(payload.fence.get(), payload.value,
          signals.size() > 1 ? "batched" : "encoded", signal.frame_id,
          signal.lifecycle_pair_id, signal.queue_lifecycle_id);
    }
    auto &queue = device_->GetDXMTDevice().queue();
    queue.CommitCurrentChunkForFrame(
        signals.front().frame_id == kUnresolvedSubmissionFrameId
            ? PreviousFrameSeq(queue.CurrentFrameSeq())
            : signals.front().frame_id);
  }

  void EncodeFenceSignal(Fence *state, UINT64 value, const char *mode,
                         uint64_t frame_id, uint64_t lifecycle_pair_id,
                         uint64_t queue_lifecycle_id) {
    auto event = state->GetSharedEvent();
    auto &queue = device_->GetDXMTDevice().queue();
    auto chunk = queue.CurrentChunk();
    const auto chunk_id = queue.CurrentSeqId();
    const auto chunk_slot = CommandChunkSlot(chunk_id);
    const auto chunk_event_id =
        NextChunkEventSeqId(queue.GetCurrentEventSeqId());
    const auto chunk_frame = frame_id;
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type,
        "signal-completion-link.enter", "signal", lifecycle_pair_id,
        queue_lifecycle_id, chunk_frame, chunk_id, 0, value, chunk_event_id);
    LogFenceSignalEncode({
        .queue = reinterpret_cast<uintptr_t>(this),
        .queue_type = desc_.Type,
        .fence = reinterpret_cast<uintptr_t>(state),
        .value = value,
        .mode = mode,
        .chunk = chunk_id,
        .chunk_slot = chunk_slot,
        .chunk_event = chunk_event_id,
        .frame = chunk_frame,
        .submitted_batches =
            submitted_batches_.load(std::memory_order_relaxed),
        .has_waited = has_waited_.load(std::memory_order_relaxed),
    });
    state->RegisterQueueSignal({
        event,
        value,
        reinterpret_cast<uintptr_t>(this),
        desc_.Type,
        chunk_id,
        chunk_event_id,
        chunk_frame,
    });
    chunk->emitcc([event = std::move(event), value](ArgumentEncodingContext &enc) mutable {
      enc.signalEvent(std::move(event), value);
    });
    EnqueueRetirement(
        chunk,
        FenceRetirementWork{
            .fence = FencePrivateReference(state),
            .value = value,
            .queue_identity = reinterpret_cast<uintptr_t>(this),
            .queue_type = desc_.Type,
            .chunk = chunk_id,
            .chunk_event = chunk_event_id,
            .frame = chunk_frame,
            .lifecycle_pair_id = lifecycle_pair_id,
            .queue_lifecycle_id = queue_lifecycle_id,
        });
    D3D12SubmissionLifecycleLog(
        reinterpret_cast<uintptr_t>(this), desc_.Type,
        "signal-completion-link.leave", "signal", lifecycle_pair_id,
        queue_lifecycle_id, chunk_frame, chunk_id, 0, value, chunk_event_id);
    signal_count_.fetch_add(1, std::memory_order_relaxed);
    last_signal_value_.store(value, std::memory_order_relaxed);
  }

