#include "d3d12_retire_submission_work.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "log/log.hpp"

#include <atomic>

namespace dxmt::d3d12 {

void
RetireAllocatorWork(AllocatorRetirementWork &work,
                    GpuCompletionStatus) noexcept {
  D3D12SubmissionLifecycleLog(
      work.queue_identity, work.queue_type, "retirement.enter", "execute",
      work.lifecycle_pair_id, work.queue_lifecycle_id, work.frame,
      work.chunk, 0, work.chunk, 0);
  for (auto &use : work.uses) {
    if (use.state)
      use.state->Complete(use.serial);
  }
  D3D12SubmissionLifecycleLog(
      work.queue_identity, work.queue_type, "retirement.leave", "execute",
      work.lifecycle_pair_id, work.queue_lifecycle_id, work.frame,
      work.chunk, 0, work.chunk, 1);
}

void
RetireFenceWork(FenceRetirementWork &work, GpuCompletionStatus) noexcept {
  auto *fence = work.fence.get();
  if (!fence)
    return;
  D3D12SubmissionLifecycleLog(
      work.queue_identity, work.queue_type, "retirement.enter", "signal",
      work.lifecycle_pair_id, work.queue_lifecycle_id, work.frame,
      work.chunk, 0, work.value, work.chunk_event);
  static std::atomic<uint32_t> complete_log_count = 0;
  if (D3D12DiagShouldLog(
          complete_log_count,
          D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE"))) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: FenceSignalRetireChunk"
         " queue=", work.queue_identity,
         " queueType=", work.queue_type,
         " fence=", reinterpret_cast<uintptr_t>(fence),
         " value=", work.value,
         " dxmtChunk=", work.chunk,
         " chunkEvent=", work.chunk_event,
         " frame=", work.frame);
  }
  fence->SetCompletedValue(work.value);
  D3D12SubmissionLifecycleLog(
      work.queue_identity, work.queue_type, "retirement.leave", "signal",
      work.lifecycle_pair_id, work.queue_lifecycle_id, work.frame,
      work.chunk, 0, work.value, work.chunk_event);
}

void
RetirePresentWork(PresentRetirementWork &work, GpuCompletionStatus) noexcept {
  if (work.state)
    work.state->complete();
  if (work.signals)
    work.signals->Signal();
}

} // namespace dxmt::d3d12
