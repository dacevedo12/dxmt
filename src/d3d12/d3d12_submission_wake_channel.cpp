#include "d3d12_submission_wake_channel.hpp"

#include "d3d12_queue_diagnostics_env.hpp"

namespace dxmt::d3d12 {

void
SubmissionWakeState::WakeOne() noexcept {
  condition.notify_one();
  if (auto target = service.lock())
    target->Wake();
}

void
SubmissionWakeState::WakeAll() noexcept {
  condition.notify_all();
  if (auto target = service.lock())
    target->Wake();
}

void
QueueWaitState::CompleteFenceWait() noexcept {
  D3D12SubmissionLifecycleLog(queue_identity_, queue_type_,
                              "fence-wait-target.enter", "wait",
                              lifecycle_pair_id_, queue_lifecycle_id_,
                              frame_id_, 0, 0, value_, 0);
  completed_.store(true, std::memory_order_release);
  wake_state_->WakeOne();
  D3D12SubmissionLifecycleLog(queue_identity_, queue_type_,
                              "fence-wait-target.leave", "wait",
                              lifecycle_pair_id_, queue_lifecycle_id_,
                              frame_id_, 0, 0, value_, 1);
}

} // namespace dxmt::d3d12
