#pragma once

// Replay of the query records a command list carries: timestamp sample index
// allocation, BeginQuery, and the ResolveQueryData GPU/CPU split.
//
// These used to be private members of CommandQueueImpl
// (d3d12_command_queue_query_resolve.inc and
// d3d12_command_queue_replay_state_ops.inc). Every piece of queue state they
// reached for through `this` is an explicit parameter here: the sample-allocator
// cursor, the DXMT submission queue behind device_->GetDXMTDevice().queue(),
// the queue's diagnostic identity reinterpret_cast<uintptr_t>(this) and
// desc_.Type. The queue is passed by reference (rather than a precomputed
// sequence id) so CurrentSeqId() is still sampled at exactly the original point.
//
// The pass-batch flush that used to open ReplayBeginQuery stays on the queue
// side: FlushPassBatches() is still a CommandQueueImpl member.

#include "d3d12_command_list.hpp"
#include "d3d12_query.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_occlusion_query.hpp"

#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

/**
 * Assigns `query` its sample location within the current submission and returns
 * the sample index.
 *
 * `sample_sequence` / `sample_count` are the queue-owned allocator cursor; the
 * counter rewinds whenever `current_sequence` names a newer submission. With a
 * Metal 4 counter heap the query already owns its slot, so the cursor only
 * records which submission the sample belongs to.
 */
uint64_t AllocateTimestampSample(uint64_t current_sequence,
                                 uint64_t &sample_sequence,
                                 uint64_t &sample_count,
                                 ::dxmt::TimestampQuery *query);

/**
 * Replays a BeginQuery record. Statistics queries are accumulated on the heap;
 * visibility queries open a Metal visibility-result region on `chunk`. A record
 * naming a query heap from another runtime is dropped with a warning.
 *
 * Callers must have flushed the pending pass batches first.
 */
void ReplayBeginQuery(CommandChunk *chunk, const BeginQueryRecord &record);

/**
 * Parks a finished timestamp marker on `state`. The marker is emitted right
 * away unless deferral is enabled and a graphics or compute pass is still open,
 * in which case it waits for the next flush so it does not split that pass.
 */
void QueueReplayTimestampMarker(CommandChunk *chunk, ReplayState &state,
                                const EndQueryRecord &record,
                                Rc<::dxmt::TimestampQuery> query);

/**
 * Replays the non-timestamp half of an EndQuery record: statistics queries are
 * accumulated on the heap, visibility queries close their Metal region on
 * `chunk`. Timestamp queries stay on the queue side — they need the queue's
 * sample allocator and a second pass-batch flush.
 *
 * Callers must have flushed the pending pass batches first.
 */
void ReplayEndNonTimestampQuery(CommandChunk *chunk, QueryHeap &heap,
                                const EndQueryRecord &record);

/**
 * Replays a ResolveQueryData record.
 *
 * Timestamp resolves are split into maximal runs of samples that can and cannot
 * be resolved on the GPU: the former are parked in
 * `state.pending_timestamp_resolves` for a later materialization point, the
 * latter go through the CPU fallback. Every other query type takes the CPU
 * fallback whole. A record whose snapshot cannot be sized, or whose destination
 * range does not fit, is dropped.
 */
void ReplayResolveQueryData(::dxmt::CommandQueue &queue, uintptr_t queue_id,
                            D3D12_COMMAND_LIST_TYPE queue_type,
                            ReplayState &state,
                            const ResolveQueryDataRecord &record);

} // namespace dxmt::d3d12
