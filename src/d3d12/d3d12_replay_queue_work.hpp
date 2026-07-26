#pragma once

// Replay-thread handlers for the QueueWorkPayload alternatives that do not
// need the command queue instance.
//
// They used to be CommandQueueImpl::ReplayQueueWork overloads purely because
// their payload types were nested in that class. The sparse handlers touch
// nothing but the chunk; the swap-chain resize handler needs exactly two
// pieces of queue state, which are passed in, so none of them pull the queue
// into the analysis. CommandQueueImpl keeps thin overloads forwarding here so
// QueueWorkReplayVisitor still resolves against one overload set.

#include "d3d12_device_queue_state.hpp"
#include "d3d12_queue_work_types.hpp"
#include "d3d12_resource_barrier_batch.hpp"
#include "dxmt_command_queue.hpp"

namespace dxmt::d3d12 {

// Emits the Metal sparse-mapping update plus its post-update resource-state
// barrier, bracketed by the chunk's sparse-mapping diagnostic. Always returns
// true: the chunk carries work either way.
bool ReplaySparseUpdateQueueWork(CommandChunk *chunk,
                                 SparseUpdateQueueWork &work);

// Same, per heap group, for a CopyTileMappings batch. Copies carry no
// diagnostic record because they replay an already-validated mapping.
bool ReplaySparseCopyQueueWork(CommandChunk *chunk, SparseCopyQueueWork &work);

// Answers "are the old backbuffers releasable?" at the point in the replay
// order where the resize was requested. Barrier entries naming a backbuffer
// are detached first so they do not count as live references; if any
// backbuffer is still referenced elsewhere they are folded back and the resize
// is refused. Publishes the verdict on `work.result` and always returns false,
// since no GPU work is emitted.
bool ReplaySwapChainResizeQueueWork(
    SwapChainResizeQueueWork &work,
    ResourceAccessBarrierBatch &pending_queue_resource_barriers,
    D3D12DeviceQueueState &device_queue_state);

} // namespace dxmt::d3d12
