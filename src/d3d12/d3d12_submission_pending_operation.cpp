#include "d3d12_submission_pending_operation.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_submission_drain_diag.hpp"
#include "d3d12_submission_wait_state.hpp"

#include <algorithm>
#include <utility>

namespace dxmt::d3d12 {

PendingOperationType
PendingOperation::type() const noexcept {
  switch (payload.index()) {
  case 0:
    return PendingOperationType::Execute;
  case 1:
    return PendingOperationType::QueueWork;
  case 2:
    return PendingOperationType::Signal;
  case 3:
    return PendingOperationType::Wait;
  case 4:
    return PendingOperationType::Stop;
  default:
    std::terminate();
  }
}

UINT64
PendingOperation::value() const noexcept {
  if (const auto *signal = std::get_if<FenceSignalSubmission>(&payload))
    return signal->value;
  if (const auto *wait = std::get_if<FenceWaitSubmission>(&payload))
    return wait->value;
  return 0;
}

const char *
PendingOperationTypeName(PendingOperationType type) {
  switch (type) {
  case PendingOperationType::Execute:
    return "execute";
  case PendingOperationType::QueueWork:
    return "queue-work";
  case PendingOperationType::Signal:
    return "signal";
  case PendingOperationType::Wait:
    return "wait";
  case PendingOperationType::Stop:
    return "stop";
  }
  return "unknown";
}

std::deque<PendingOperation>
CancelPendingOperationsFromBlockingWait(
    dxmt::mutex &queue_mutex, std::deque<PendingOperation> &pending) {
  std::deque<PendingOperation> cancelled;
  auto first_wait =
      std::find_if(pending.begin(), pending.end(),
                   [](const PendingOperation &operation) {
                     if (!operation.is<FenceWaitSubmission>())
                       return false;
                     const auto &wait = operation.get<FenceWaitSubmission>();
                     return QueuedFenceWaitBlocksShutdown(
                         wait.fence.get(), wait.value,
                         wait.completion && wait.completion->completed());
                   });
  while (first_wait != pending.end()) {
    cancelled.push_back(std::move(*first_wait));
    first_wait = pending.erase(first_wait);
  }
  return cancelled;
}

void
CompleteCancelledPendingOperations(
    uintptr_t queue_identity, D3D12_COMMAND_LIST_TYPE queue_type,
    std::deque<PendingOperation> &cancelled) {
  for (auto &operation : cancelled) {
    D3D12SubmissionLifecycleLog(
        queue_identity, queue_type, "operation-cancel.enter",
        PendingOperationTypeName(operation.type()),
        operation.lifecycle_pair_id, operation.queue_lifecycle_id,
        operation.frame_id, 0, 0, operation.value(), 0);
    if (auto *execute = std::get_if<ExecuteSubmission>(&operation.payload)) {
      for (auto &use : execute->allocator_uses) {
        if (use.state)
          use.state->Complete(use.serial);
      }
    }
    D3D12SubmissionLifecycleLog(
        queue_identity, queue_type, "operation-cancel.leave",
        PendingOperationTypeName(operation.type()),
        operation.lifecycle_pair_id, operation.queue_lifecycle_id,
        operation.frame_id, 0, 0, operation.value(), 0);
  }
}

void
TakeCoalescedFenceSignals(dxmt::mutex &queue_mutex,
                          std::deque<PendingOperation> &pending,
                          std::vector<PendingOperation> &signals) {
  while (!pending.empty() && pending.front().is<FenceSignalSubmission>()) {
    signals.push_back(std::move(pending.front()));
    pending.pop_front();
  }
}

bool
SubmissionDrainCanStart(dxmt::mutex &queue_mutex,
                        const std::deque<PendingOperation> &pending,
                        bool worker_active, bool service_stopped,
                        bool waiting_for_wait) {
  if (worker_active || pending.empty() || service_stopped)
    return false;
  if (waiting_for_wait) {
    const auto &front = pending.front();
    if (!front.is<FenceWaitSubmission>())
      return false;
    const auto &wait = front.get<FenceWaitSubmission>();
    if (!wait.completion || !wait.completion->completed())
      return false;
  }
  return true;
}

void
SignalFenceBatchImmediately(uintptr_t queue_identity,
                            D3D12_COMMAND_LIST_TYPE queue_type,
                            std::vector<PendingOperation> &signals,
                            std::atomic<UINT64> &signal_count,
                            std::atomic<UINT64> &last_signal_value) {
  for (auto &signal : signals) {
    auto &payload = signal.get<FenceSignalSubmission>();
    auto *fence = payload.fence.get();
    if (!fence)
      continue;

    LogFenceSignalImmediate(queue_identity, queue_type,
                            reinterpret_cast<uintptr_t>(fence), payload.value,
                            signals.size());
    fence->SignalFromQueue(payload.value);
    signal_count.fetch_add(1, std::memory_order_relaxed);
    last_signal_value.store(payload.value, std::memory_order_relaxed);
  }
}

} // namespace dxmt::d3d12
