#pragma once

// GPU timestamp-resolve emission and the CPU query-resolve fallback / deferral
// policy.
//
// These helpers used to be private members of CommandQueueImpl
// (d3d12_command_queue_query_resolve.inc). They only touch the pending-resolve
// value types, the command chunk and the DXMT submission queue. The two pieces
// of queue identity they used to reach for through `this` — the DXMT queue
// behind device_->GetDXMTDevice().queue() and the queue's diagnostic identity
// reinterpret_cast<uintptr_t>(this) — are now explicit parameters. The queue is
// passed by reference (rather than a precomputed sequence id) so CurrentSeqId()
// is still sampled at exactly the original point.

#include "d3d12_command_list.hpp"
#include "d3d12_query.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_replay_state_types.hpp"
#include "dxmt_command_queue.hpp"

#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Encodes one pending timestamp resolve as GPU counter copies into the
// destination buffer.
void EmitTimestampResolve(::dxmt::CommandQueue &queue, CommandChunk *chunk,
                          PendingTimestampResolve resolve);

// Parks a CPU query resolve on the destination resource so it materializes
// lazily instead of stalling now. Returns false when deferral is unavailable.
[[nodiscard]] bool
DeferCpuQueryResolveToResource(::dxmt::CommandQueue &queue, uintptr_t queue_id,
                               const ResolveQueryDataRecord &record,
                               QueryResolveSnapshot snapshot,
                               UINT64 byte_count);

// Routes a query resolve that cannot be done on the GPU to either the deferred
// or the immediate CPU fallback path.
void QueueCpuQueryFallback(::dxmt::CommandQueue &queue, uintptr_t queue_id,
                           ReplayState &state,
                           const ResolveQueryDataRecord &record,
                           QueryResolveSnapshot snapshot, UINT64 byte_count,
                           const char *reason);

} // namespace dxmt::d3d12
