// Queue work replay, retirement visitors, and retirement records.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  bool ReplayQueueWork(CommandChunk *, std::monostate &) noexcept {
    return false;
  }

  // The sparse and swap-chain-resize handlers hoisted into
  // d3d12_replay_queue_work.cpp; only the queue state the resize handler reads
  // has to be threaded back in. These wrappers keep one ReplayQueueWork
  // overload set for QueueWorkReplayVisitor to resolve against.
  bool ReplayQueueWork(CommandChunk *chunk, SparseUpdateQueueWork &work) {
    return ReplaySparseUpdateQueueWork(chunk, work);
  }

  bool ReplayQueueWork(CommandChunk *chunk, SparseCopyQueueWork &work) {
    return ReplaySparseCopyQueueWork(chunk, work);
  }

  bool ReplayQueueWork(CommandChunk *chunk, D3D12PresentSubmission &work) {
    auto *resource = GetResource(work.resource.ptr());
    const auto present_view =
        resource ? resource->GetPresentSourceView() : TextureViewKey{};
    static std::atomic<uint32_t> present_view_log_count = 0;
    if (D3D12DiagShouldLog(present_view_log_count,
                           D3D12DiagSwapChainEnabled())) {
      INFO("D3D12 diagnostic: queue-ordered present view",
           " resource=", uint64_t(work.resource.ptr()),
           " api_thread_view=", uint64_t(work.api_thread_view),
           " replay_ordered_view=", uint64_t(present_view),
           " changed_after_present_call=",
           uint64_t(work.api_thread_view) != uint64_t(present_view));
    }
    EnqueueRetirement(
        chunk, PresentRetirementWork{
                   .state = work.state,
                   .signals = std::move(work.signals),
               });
    chunk->emitcc([
        backbuffer = std::move(work.backbuffer), present_view,
        presenter = std::move(work.presenter),
        present_after = work.present_after,
        apitrace_frame_index = work.apitrace_frame_index,
        sync_interval = work.sync_interval, flags = work.flags,
        state = std::move(work.state)](
                       ArgumentEncodingContext &context) mutable {
      context.present(backbuffer, present_view, presenter, present_after,
                      state->metadata, apitrace_frame_index, sync_interval,
                      flags);
    });
    return true;
  }

  bool ReplayQueueWork(CommandChunk *,
                       SwapChainResizeQueueWork &work) {
    return ReplaySwapChainResizeQueueWork(
        work, pending_queue_resource_barriers_, *device_queue_state_);
  }

  class QueueWorkReplayVisitor final {
  public:
    QueueWorkReplayVisitor(CommandQueueImpl &queue,
                           CommandChunk *chunk) noexcept
        : queue_(queue), chunk_(chunk) {}

    template <typename Work>
    bool operator()(Work &work) const {
      return queue_.ReplayQueueWork(chunk_, work);
    }

  private:
    CommandQueueImpl &queue_;
    CommandChunk *chunk_;
  };

  // Every retirement payload type now lives at namespace scope, so all of the
  // handlers hoisted into d3d12_retire_submission_work.cpp and
  // d3d12_retire_diag_work.cpp. These wrappers exist only to keep a single
  // `Retire()` overload set for RetirementVisitor to resolve against.
  static void Retire(AllocatorRetirementWork &work,
                     GpuCompletionStatus status) noexcept {
    RetireAllocatorWork(work, status);
  }

  static void Retire(FenceRetirementWork &work,
                     GpuCompletionStatus status) noexcept {
    RetireFenceWork(work, status);
  }

  static void Retire(PresentRetirementWork &work,
                     GpuCompletionStatus status) noexcept {
    RetirePresentWork(work, status);
  }

  static void Retire(DrawVisibilityRetirementWork &work,
                     GpuCompletionStatus status) noexcept {
    RetireDrawVisibilityWork(work, status);
  }

  static void Retire(IndexReadbackRetirementWork &work,
                     GpuCompletionStatus status) noexcept {
    RetireIndexReadbackWork(work, status);
  }

  static void Retire(VertexReadbackRetirementWork &work,
                     GpuCompletionStatus status) noexcept {
    RetireVertexReadbackWork(work, status);
  }

  static void Retire(ConstantBufferReadbackRetirementWork &work,
                     GpuCompletionStatus status) noexcept {
    RetireConstantBufferReadbackWork(work, status);
  }

  class RetirementVisitor final {
  public:
    explicit RetirementVisitor(GpuCompletionStatus status) noexcept
        : status_(status) {}

    template <typename Work>
    void operator()(Work &work) const noexcept {
      CommandQueueImpl::Retire(work, status_);
    }

  private:
    GpuCompletionStatus status_;
  };

  class RetirementRecord final : public GpuCompletionTarget {
  public:
    explicit RetirementRecord(RetirementPayload payload) noexcept
        : payload_(std::move(payload)) {}
    RetirementRecord(const RetirementRecord &) = delete;
    RetirementRecord &operator=(const RetirementRecord &) = delete;
    ~RetirementRecord() noexcept override = default;

    void CompleteGpuWork(GpuCompletionStatus status) noexcept override {
      if (!dxmt::invokeNoexcept("D3D12 typed GPU retirement",
                                [this, status]() {
                                  std::visit(RetirementVisitor(status),
                                             payload_);
                                }))
        std::terminate();
    }

  private:
    RetirementPayload payload_;
  };

  void EnqueueRetirement(CommandChunk *chunk, RetirementPayload payload) {
    chunk->addCompletionTarget(
        std::make_shared<RetirementRecord>(std::move(payload)));
  }

