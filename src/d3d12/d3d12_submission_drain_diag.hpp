#pragma once

// DXMT_DIAG_COMMAND_QUEUE trace points of the submission worker drain loop and
// the fence signal encoder.
//
// Extracted from class CommandQueueImpl
// (d3d12_command_queue_submission.inc). Each entry point owns the throttle
// counter that used to be a function-local static at the call site; a
// function-local static in a class member function is already a single object
// shared by every queue, so moving it here keeps one throttle sequence per
// trace point. The `drain replay list` begin/end pair deliberately shares one
// counter, exactly as the single static declared next to it did.
//
// Values that the call site used to compute inside the WARN_FILE_ONLY argument
// list are now ordinary arguments, so they are read whether or not the trace
// fires. Every one of them is a relaxed atomic load, a container size or an
// already-resolved variant alternative, so this is a cost, not an observable
// difference.

#include "d3d12_fence.hpp"

#include <cstddef>
#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

[[nodiscard]] const char *FenceGpuWaitStatusName(FenceGpuWaitStatus status);

// A queued wait could not be turned into a GPU wait and falls back to the CPU
// completion callback.
void LogQueueWaitFallbackCpu(uintptr_t queue,
                             D3D12_COMMAND_LIST_TYPE queue_type,
                             uintptr_t fence, UINT64 value,
                             FenceGpuWaitStatus status, bool callback_armed,
                             size_t pending_operations);

// A queued wait resolved against a producer queue's signal and was encoded as
// a GPU wait event.
void LogQueueWaitResolvedGpu(uintptr_t queue,
                             D3D12_COMMAND_LIST_TYPE queue_type,
                             uintptr_t fence, UINT64 value,
                             const FenceGpuSignal &signal,
                             uint64_t wait_chunk, uint64_t wait_chunk_event,
                             uint64_t wait_frame);

void LogDrainExecuteBegin(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                          size_t command_lists, uint64_t submitted_batches);

void LogDrainReplayListBegin(uintptr_t queue,
                             D3D12_COMMAND_LIST_TYPE queue_type,
                             UINT list_index, UINT record_count,
                             size_t compiled_segments,
                             size_t compiled_graphics_packets,
                             size_t compiled_compute_packets,
                             uint64_t submitted_batches);

void LogDrainReplayListEnd(uintptr_t queue,
                           D3D12_COMMAND_LIST_TYPE queue_type,
                           UINT list_index, UINT record_count,
                           size_t touched_resources,
                           uint64_t submitted_batches);

void LogDrainDecayBegin(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                        size_t touched_resources, uint64_t submitted_batches);

void LogDrainDecayEnd(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                      size_t touched_resources, uint64_t submitted_batches);

void LogDrainCommitBegin(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                         uint64_t submitted_batches, size_t coalesced_signals);

void LogDrainCommitEnd(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                       uint64_t submitted_batches);

void LogDrainExecuteSubmitted(uintptr_t queue,
                              D3D12_COMMAND_LIST_TYPE queue_type,
                              uint64_t submitted_batches);

void LogDrainSignalBatch(uintptr_t queue, D3D12_COMMAND_LIST_TYPE queue_type,
                         size_t count, uintptr_t first_fence,
                         UINT64 first_value, uintptr_t last_fence,
                         UINT64 last_value);

void LogDrainWaitSatisfied(uintptr_t queue,
                           D3D12_COMMAND_LIST_TYPE queue_type,
                           uintptr_t fence, UINT64 value);

// A signal on a queue that has neither submitted a batch nor waited: the fence
// is signalled directly instead of being encoded into a chunk.
void LogFenceSignalImmediate(uintptr_t queue,
                             D3D12_COMMAND_LIST_TYPE queue_type,
                             uintptr_t fence, UINT64 value, size_t batch_size);

// Everything the encoded-signal path traces: the signal itself and the chunk
// it was bound to. Two independently throttled trace points, in this order.
struct FenceSignalEncodeTrace {
  uintptr_t queue = 0;
  D3D12_COMMAND_LIST_TYPE queue_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  uintptr_t fence = 0;
  UINT64 value = 0;
  const char *mode = "";
  uint64_t chunk = 0;
  uint64_t chunk_slot = 0;
  uint64_t chunk_event = 0;
  uint64_t frame = 0;
  uint64_t submitted_batches = 0;
  bool has_waited = false;
};

void LogFenceSignalEncode(const FenceSignalEncodeTrace &trace);

} // namespace dxmt::d3d12
