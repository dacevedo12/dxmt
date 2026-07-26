#pragma once

// One unit of work sitting in a logical queue's submission FIFO, and the
// ordering rules the queue applies to that FIFO.
//
// Extracted from class CommandQueueImpl
// (d3d12_command_queue_submission.inc). The payload variant used to be nested
// in the queue class, which made every operation on the FIFO unnameable outside
// it -- the shutdown cancellation, the signal coalescing and the drain
// admission gate were all pinned into the queue translation unit by nothing
// more than that.
//
// The FIFO itself stays a queue member: these functions take it, together with
// the mutex that guards it, as parameters. The mutex parameter carries no data;
// it is the capability the analyser checks, so a caller that forgot the lock is
// a compile error rather than a race.

#include "d3d12_command_allocator.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_queue_work_types.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_submission_capture_stats.hpp"
#include "d3d12_submission_timeline.hpp"
#include "d3d12_submission_wake_channel.hpp"
#include "thread.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct ExecuteSubmission final {
  std::vector<std::shared_ptr<const SubmittedCompiledCommandListPlan>>
      submitted_command_lists;
  std::vector<CompiledCommandDescriptorSnapshots> compiled_descriptor_snapshots;
  std::vector<SubmittedCommandAllocatorUse> allocator_uses;
  SubmissionCaptureStatistics capture_statistics = {};
};

struct QueueWorkSubmission final {
  QueueWorkPayload work;
};

struct FenceSignalSubmission final {
  FencePrivateReference fence;
  UINT64 value = 0;
};

struct FenceWaitSubmission final {
  FencePrivateReference fence;
  UINT64 value = 0;
  bool callback_armed = false;
  std::shared_ptr<QueueWaitState> completion;
};

struct StopSubmission final {};

using PendingOperationPayload =
    std::variant<ExecuteSubmission, QueueWorkSubmission, FenceSignalSubmission,
                 FenceWaitSubmission, StopSubmission>;

struct PendingOperation final {
  PendingOperationPayload payload;
  uint64_t frame_id = kUnresolvedSubmissionFrameId;
  uint64_t lifecycle_pair_id = 0;
  uint64_t queue_lifecycle_id = 0;

  template <typename Payload>
  explicit PendingOperation(
      Payload &&value, uint64_t frame = kUnresolvedSubmissionFrameId) noexcept
      : payload(std::forward<Payload>(value)), frame_id(frame) {}

  PendingOperation(const PendingOperation &) = delete;
  PendingOperation &operator=(const PendingOperation &) = delete;
  PendingOperation(PendingOperation &&) noexcept = default;
  PendingOperation &operator=(PendingOperation &&) noexcept = default;
  ~PendingOperation() noexcept = default;

  [[nodiscard]] PendingOperationType type() const noexcept;

  [[nodiscard]] UINT64 value() const noexcept;

  template <typename Payload>
  [[nodiscard]] bool is() const noexcept {
    return std::holds_alternative<Payload>(payload);
  }

  template <typename Payload>
  [[nodiscard]] Payload &get() noexcept {
    auto *value = std::get_if<Payload>(&payload);
    if (!value)
      std::terminate();
    return *value;
  }

  template <typename Payload>
  [[nodiscard]] const Payload &get() const noexcept {
    const auto *value = std::get_if<Payload>(&payload);
    if (!value)
      std::terminate();
    return *value;
  }
};

static_assert(std::is_nothrow_move_constructible_v<PendingOperation>);
static_assert(!std::is_copy_constructible_v<PendingOperation>);

/** Stable short name of an operation kind, as it appears in lifecycle traces. */
[[nodiscard]] const char *PendingOperationTypeName(PendingOperationType type);

/**
 * Removes, from the first queue wait that cannot retire during destruction
 * onwards, every remaining operation and returns them in queue order.
 *
 * Operations before that wait may still be drained; the wait and everything
 * ordered behind it cannot make progress while the queue is being destroyed, so
 * the caller cancels that suffix instead and puts Stop at the now-reachable
 * tail.
 */
[[nodiscard]] std::deque<PendingOperation>
CancelPendingOperationsFromBlockingWait(
    dxmt::mutex &queue_mutex,
    std::deque<PendingOperation> &pending) DXMT_REQUIRES(queue_mutex);

/**
 * Releases what each cancelled operation held, in queue order, and traces the
 * cancellation. Only an Execute submission owns something the rest of the
 * system waits on: its allocator uses have to be completed here or the command
 * allocators behind them never become reusable.
 */
void CompleteCancelledPendingOperations(
    uintptr_t queue_identity, D3D12_COMMAND_LIST_TYPE queue_type,
    std::deque<PendingOperation> &cancelled);

/**
 * Moves the run of fence signals at the head of the FIFO onto `signals`, in
 * queue order. Signals immediately behind a submitted batch ride the same chunk
 * commit instead of each forcing one of their own.
 */
void TakeCoalescedFenceSignals(
    dxmt::mutex &queue_mutex, std::deque<PendingOperation> &pending,
    std::vector<PendingOperation> &signals) DXMT_REQUIRES(queue_mutex);

/**
 * Whether the submission service may start a drain pass on this queue.
 *
 * A worker that parked on an unsatisfied wait must find that same wait, now
 * completed, still at the head: anything else means the wait it parked on is
 * not what it would dequeue, and re-entering the drain loop would spin.
 */
[[nodiscard]] bool
SubmissionDrainCanStart(dxmt::mutex &queue_mutex,
                        const std::deque<PendingOperation> &pending,
                        bool worker_active, bool service_stopped,
                        bool waiting_for_wait) DXMT_REQUIRES(queue_mutex);

/**
 * Signals every fence in `signals` directly on the CPU and folds the batch into
 * the queue's signal counters. Only valid for a queue that has neither
 * submitted a batch nor waited, where there is no chunk to attach the signal
 * to; entries with no fence are skipped.
 */
void SignalFenceBatchImmediately(uintptr_t queue_identity,
                                 D3D12_COMMAND_LIST_TYPE queue_type,
                                 std::vector<PendingOperation> &signals,
                                 std::atomic<UINT64> &signal_count,
                                 std::atomic<UINT64> &last_signal_value);

} // namespace dxmt::d3d12
