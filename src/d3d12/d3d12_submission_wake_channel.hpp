#pragma once

// The channel a completed fence uses to wake a logical queue's submission
// worker: the condition variable the queue shares with the device submission
// service, and the FenceWaitTarget a queued wait installs on a fence.
//
// Extracted from class CommandQueueImpl (d3d12_command_queue.cpp). Neither type
// reads queue state -- the wait target carries by value the identity it traces
// with -- so nesting them in the class only forced every user of a queued wait
// into the same translation unit. FenceWaitSubmission is that user, and it is
// what the rest of the submission path is threaded through.

#include "d3d12_fence.hpp"
#include "d3d12_submission_service.hpp"
#include "thread.hpp"

#include <atomic>
#include <cstdint>
#include <memory>

#include <d3d12.h>

namespace dxmt::d3d12 {

/**
 * Wake side of the submission handshake. Notifying the condition variable is
 * not sufficient on its own: the worker may be parked in the device submission
 * service rather than on this queue, so both are poked, in that order.
 */
struct SubmissionWakeState {
  dxmt::condition_variable condition;
  std::weak_ptr<DeviceSubmissionService> service;

  explicit SubmissionWakeState(
      const std::shared_ptr<DeviceSubmissionService> &submission_service)
      : service(submission_service) {}

  void WakeOne() noexcept;

  void WakeAll() noexcept;
};

/**
 * Completion target a queued fence wait installs when the wait cannot be
 * turned into a GPU wait. `completed()` is the acquire load that both the drain
 * loop and the destruction path test the wait against; it pairs with the
 * release store in CompleteFenceWait.
 */
class QueueWaitState final : public FenceWaitTarget {
public:
  QueueWaitState(std::shared_ptr<SubmissionWakeState> wake_state,
                 uintptr_t queue_identity,
                 D3D12_COMMAND_LIST_TYPE queue_type,
                 uint64_t lifecycle_pair_id, uint64_t queue_lifecycle_id,
                 uint64_t frame_id, UINT64 value) noexcept
      : wake_state_(std::move(wake_state)), queue_identity_(queue_identity),
        queue_type_(queue_type), lifecycle_pair_id_(lifecycle_pair_id),
        queue_lifecycle_id_(queue_lifecycle_id), frame_id_(frame_id),
        value_(value) {}

  void CompleteFenceWait() noexcept override;

  [[nodiscard]] bool completed() const noexcept {
    return completed_.load(std::memory_order_acquire);
  }

private:
  std::shared_ptr<SubmissionWakeState> wake_state_;
  const uintptr_t queue_identity_;
  const D3D12_COMMAND_LIST_TYPE queue_type_;
  const uint64_t lifecycle_pair_id_;
  const uint64_t queue_lifecycle_id_;
  const uint64_t frame_id_;
  const UINT64 value_;
  std::atomic_bool completed_{false};
};

} // namespace dxmt::d3d12
