#include "d3d12_submission_drain_diag.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "log/log.hpp"

#include <atomic>

namespace dxmt::d3d12 {

namespace {

[[nodiscard]] bool CommandQueueDiagEnabled() {
  return D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
}

std::atomic<uint32_t> g_wait_fallback_log_count = 0;
std::atomic<uint32_t> g_wait_resolved_log_count = 0;
std::atomic<uint32_t> g_drain_execute_log_count = 0;
// One counter for the begin/end pair, matching the single function-local
// static that used to guard both trace points.
std::atomic<uint32_t> g_drain_replay_list_log_count = 0;
std::atomic<uint32_t> g_drain_decay_begin_log_count = 0;
std::atomic<uint32_t> g_drain_decay_end_log_count = 0;
std::atomic<uint32_t> g_drain_commit_begin_log_count = 0;
std::atomic<uint32_t> g_drain_commit_end_log_count = 0;
std::atomic<uint32_t> g_drain_execute_submitted_log_count = 0;
std::atomic<uint32_t> g_drain_signal_batch_log_count = 0;
std::atomic<uint32_t> g_drain_wait_satisfied_log_count = 0;
std::atomic<uint32_t> g_fence_signal_immediate_log_count = 0;
std::atomic<uint32_t> g_fence_signal_encode_log_count = 0;
std::atomic<uint32_t> g_fence_signal_bind_log_count = 0;

} // namespace

const char *
FenceGpuWaitStatusName(FenceGpuWaitStatus status) {
  switch (status) {
  case FenceGpuWaitStatus::Resolved:
    return "resolved";
  case FenceGpuWaitStatus::External:
    return "external";
  case FenceGpuWaitStatus::Shared:
    return "shared";
  case FenceGpuWaitStatus::CpuSignal:
    return "cpu_signal";
  case FenceGpuWaitStatus::Unknown:
    return "unknown";
  case FenceGpuWaitStatus::Rewind:
    return "rewind";
  }
  return "unknown";
}

void
LogQueueWaitFallbackCpu(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                        uintptr_t fence, UINT64 value,
                        FenceGpuWaitStatus status, bool callback_armed,
                        size_t pending_operations) {
  if (D3D12DiagShouldLog(g_wait_fallback_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: WaitFallbackCpu"
         " queue=", queue,
         " queueType=", queue_type,
         " fence=", fence,
         " value=", value,
         " reason=", FenceGpuWaitStatusName(status),
         " callbackArmed=", callback_armed,
         " pendingOps=", pending_operations);
  }
}

void
LogQueueWaitResolvedGpu(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                        uintptr_t fence, UINT64 value,
                        const FenceGpuSignal &signal, uint64_t wait_chunk,
                        uint64_t wait_chunk_event, uint64_t wait_frame) {
  if (D3D12DiagShouldLog(g_wait_resolved_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: WaitResolvedGpu"
         " queue=", queue,
         " queueType=", queue_type,
         " fence=", fence,
         " value=", value,
         " producerQueue=", signal.queue,
         " producerQueueType=", signal.queue_type,
         " producerDxmtChunk=", signal.dxmt_chunk,
         " producerChunkEvent=", signal.chunk_event,
         " producerFrame=", signal.frame,
         " waitDxmtChunk=", wait_chunk,
         " waitChunkEvent=", wait_chunk_event,
         " waitFrame=", wait_frame);
  }
}

void
LogDrainExecuteBegin(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                     size_t command_lists, uint64_t submitted_batches) {
  if (D3D12DiagShouldLog(g_drain_execute_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain execute"
         " queue=", queue,
         " queueType=", queue_type,
         " commandLists=", command_lists,
         " submittedBatchesBefore=", submitted_batches);
  }
}

void
LogDrainReplayListBegin(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                        UINT list_index, UINT record_count,
                        size_t compiled_segments,
                        size_t compiled_graphics_packets,
                        size_t compiled_compute_packets,
                        uint64_t submitted_batches) {
  if (D3D12DiagShouldLog(g_drain_replay_list_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain replay list begin"
         " queue=", queue,
         " queueType=", queue_type,
         " listIndex=", list_index,
         " records=", record_count,
         " compiledSegments=", compiled_segments,
         " compiledGraphicsPackets=", compiled_graphics_packets,
         " compiledComputePackets=", compiled_compute_packets,
         " submittedBatchesBefore=", submitted_batches);
  }
}

void
LogDrainReplayListEnd(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                      UINT list_index, UINT record_count,
                      size_t touched_resources, uint64_t submitted_batches) {
  if (D3D12DiagShouldLog(g_drain_replay_list_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain replay list end"
         " queue=", queue,
         " queueType=", queue_type,
         " listIndex=", list_index,
         " records=", record_count,
         " touchedResources=", touched_resources,
         " submittedBatchesBefore=", submitted_batches);
  }
}

void
LogDrainDecayBegin(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                   size_t touched_resources, uint64_t submitted_batches) {
  if (D3D12DiagShouldLog(g_drain_decay_begin_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain decay begin"
         " queue=", queue,
         " queueType=", queue_type,
         " touchedResources=", touched_resources,
         " submittedBatchesBefore=", submitted_batches);
  }
}

void
LogDrainDecayEnd(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                 size_t touched_resources, uint64_t submitted_batches) {
  if (D3D12DiagShouldLog(g_drain_decay_end_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain decay end"
         " queue=", queue,
         " queueType=", queue_type,
         " touchedResources=", touched_resources,
         " submittedBatchesBefore=", submitted_batches);
  }
}

void
LogDrainCommitBegin(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                    uint64_t submitted_batches, size_t coalesced_signals) {
  if (D3D12DiagShouldLog(g_drain_commit_begin_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain commit begin"
         " queue=", queue,
         " queueType=", queue_type,
         " submittedBatchesBefore=", submitted_batches,
         " coalescedSignals=", coalesced_signals);
  }
}

void
LogDrainCommitEnd(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                  uint64_t submitted_batches) {
  if (D3D12DiagShouldLog(g_drain_commit_end_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain commit end"
         " queue=", queue,
         " queueType=", queue_type,
         " submittedBatchesBefore=", submitted_batches);
  }
}

void
LogDrainExecuteSubmitted(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                         uint64_t submitted_batches) {
  if (D3D12DiagShouldLog(g_drain_execute_submitted_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain execute submitted"
         " queue=", queue,
         " queueType=", queue_type,
         " submittedBatches=", submitted_batches);
  }
}

void
LogDrainSignalBatch(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                    size_t count, uintptr_t first_fence, UINT64 first_value,
                    uintptr_t last_fence, UINT64 last_value) {
  if (D3D12DiagShouldLog(g_drain_signal_batch_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain signal batch"
         " queue=", queue,
         " queueType=", queue_type,
         " count=", count,
         " firstFence=", first_fence,
         " firstValue=", first_value,
         " lastFence=", last_fence,
         " lastValue=", last_value);
  }
}

void
LogDrainWaitSatisfied(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                      uintptr_t fence, UINT64 value) {
  if (D3D12DiagShouldLog(g_drain_wait_satisfied_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: drain wait satisfied"
         " queue=", queue,
         " queueType=", queue_type,
         " fence=", fence,
         " value=", value);
  }
}

void
LogFenceSignalImmediate(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                        uintptr_t fence, UINT64 value, size_t batch_size) {
  if (D3D12DiagShouldLog(g_fence_signal_immediate_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: SubmitFenceSignal immediate"
         " queue=", queue,
         " queueType=", queue_type,
         " fence=", fence,
         " value=", value,
         " batchSize=", batch_size);
  }
}

void
LogFenceSignalEncode(const FenceSignalEncodeTrace &trace) {
  if (D3D12DiagShouldLog(g_fence_signal_encode_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: SubmitFenceSignal ", trace.mode,
         " queue=", trace.queue,
         " queueType=", trace.queue_type,
         " fence=", trace.fence,
         " value=", trace.value,
         " submittedBatches=", trace.submitted_batches,
         " hasWaited=", trace.has_waited);
  }
  if (D3D12DiagShouldLog(g_fence_signal_bind_log_count,
                         CommandQueueDiagEnabled())) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: FenceSignalBindChunk"
         " queue=", trace.queue,
         " queueType=", trace.queue_type,
         " fence=", trace.fence,
         " value=", trace.value,
         " mode=", trace.mode,
         " dxmtChunk=", trace.chunk,
         " chunkSlot=", trace.chunk_slot,
         " chunkEvent=", trace.chunk_event,
         " frame=", trace.frame,
         " submittedBatches=", trace.submitted_batches,
         " hasWaited=", trace.has_waited);
  }
}

} // namespace dxmt::d3d12
