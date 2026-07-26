#pragma once

// Translation of D3D12 resource barrier records into replay state updates and
// a ResourceAccessBarrierBatch.
//
// These helpers used to be private members of CommandQueueImpl
// (d3d12_command_queue_replay_records.inc). They only touch the ReplayState
// value type, the barrier batch value types and namespace-level resource
// helpers. The single piece of queue identity they used to reach for through
// `this` — desc_.Type, used for the transition-state compatibility diagnostic
// — is now an explicit parameter.

#include "d3d12_command_list.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_resource_barrier_batch.hpp"
#include "dxmt_command_queue.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Appends (or merges into) one buffer / texture-subresource barrier entry.
void AddResourceAccessBarrier(ResourceAccessBarrierBatch &batch,
                              Resource &resource, UINT first_subresource,
                              UINT subresource_count, int access,
                              bool requires_cross_submit_wait = false);

// Applies one app-declared transition barrier (including split BEGIN/END) to
// the replay state and records the matching access barrier.
void ReplayTransitionBarrier(ReplayState &state,
                             const StoredResourceBarrier &barrier,
                             ResourceAccessBarrierBatch &batch,
                             D3D12_COMMAND_LIST_TYPE queue_type);

// Applies one UAV barrier.
void ReplayUavBarrier(const StoredResourceBarrier &barrier,
                      ResourceAccessBarrierBatch &batch);

// Applies one aliasing barrier, resetting the after-resource replay state.
void ReplayAliasingBarrier(ReplayState &state,
                           const StoredResourceBarrier &barrier,
                           ResourceAccessBarrierBatch &batch);

// Folds a whole ResourceBarrier() record into a single batch.
[[nodiscard]] ResourceAccessBarrierBatch
BuildResourceBarrierBatch(ReplayState &state,
                          const ResourceBarrierRecord &record,
                          D3D12_COMMAND_LIST_TYPE queue_type);

// Merges a batch into the queue-level pending barrier batch.
void QueueResourceAccessBarrierBatch(ReplayState &state,
                                     ResourceAccessBarrierBatch batch);

// Non-batching barrier replay entry point.
void ReplayResourceBarrier(CommandChunk *chunk, ReplayState &state,
                           const ResourceBarrierRecord &record,
                           D3D12_COMMAND_LIST_TYPE queue_type);

} // namespace dxmt::d3d12
