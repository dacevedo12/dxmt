#include "d3d12_submission_wait_state.hpp"

#include "d3d12_fence.hpp"

namespace dxmt::d3d12 {

bool
QueuedFenceWaitIsSatisfied(const Fence *fence, UINT64 value,
                           bool completion_observed) {
  return completion_observed || fence->GetCompletedValue() >= value;
}

bool
QueuedFenceWaitBlocksShutdown(const Fence *fence, UINT64 value,
                              bool completion_observed) {
  if (QueuedFenceWaitIsSatisfied(fence, value, completion_observed))
    return false;
  FenceGpuSignal signal = {};
  return fence->TryResolveGpuWait(value, signal) !=
         FenceGpuWaitStatus::Resolved;
}

void
ClearSubmissionWaitDependency(
    dxmt::mutex &queue_mutex, bool &waiting_for_wait,
    std::atomic<uint64_t> &dependency_pair,
    std::atomic<uint64_t> &dependency_value) noexcept {
  waiting_for_wait = false;
  dependency_pair.store(0, std::memory_order_relaxed);
  dependency_value.store(0, std::memory_order_relaxed);
}

void
SetSubmissionWaitDependency(dxmt::mutex &queue_mutex, bool &waiting_for_wait,
                            std::atomic<uint64_t> &dependency_pair,
                            std::atomic<uint64_t> &dependency_value,
                            uint64_t lifecycle_pair_id,
                            UINT64 value) noexcept {
  waiting_for_wait = true;
  dependency_pair.store(lifecycle_pair_id, std::memory_order_relaxed);
  dependency_value.store(value, std::memory_order_relaxed);
}

} // namespace dxmt::d3d12
