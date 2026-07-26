#pragma once

// The per-subresource half of replay resource-access tracking: the steady-read
// short circuit, the state transition / mismatch-barrier state machine, and the
// steady-read cache refresh that follows it.
//
// Carved out of CommandQueueImpl::RecordReplayResourceAccess
// (d3d12_command_queue_replay_records.inc). What stays on the queue side is
// everything that has to reach for a CommandChunk — materializing pending query
// resolves before the access, and handing the produced barrier batch to
// QueueOrDeferResourceAccessBarrierBatch. What lives here only reads and writes
// the ReplayState value type plus namespace-level resource helpers, with
// desc_.Type passed in explicitly.

#include "d3d12_device_queue_state.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_resource_barrier_batch.hpp"

#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

/**
 * True when the steady-read cache proves this access is a no-op for every
 * subresource: the cached uniform state is read-only and already covers
 * `desired`, so neither a state update nor a mismatch barrier is due. Always
 * false for writes and for D3D12_RESOURCE_STATE_COMMON.
 */
[[nodiscard]] bool
ReplayResourceAccessIsSteadyReadNoop(const ReplayState &state,
                                     ID3D12Resource *d3d_resource,
                                     D3D12_RESOURCE_STATES desired);

/**
 * Moves subresources [first, first + count) of `resource` to `desired` and
 * appends to `barriers` the Metal-side sync a missing or mismatched app barrier
 * still requires. Returns true when anything was appended.
 *
 * A subresource whose D3D12 state cannot legally reach `desired` keeps its last
 * explicit or implicitly promoted state (so one bad transition does not poison
 * the tracker) and is reported through WarnReplayResourceAccessMismatch.
 */
[[nodiscard]] bool ApplyReplayResourceAccessTransitions(
    std::vector<ReplaySubresourceState> &states, Resource &resource,
    D3D12_RESOURCE_STATES desired, UINT first, UINT count, const char *context,
    D3D12_COMMAND_LIST_TYPE queue_type, ResourceAccessBarrierBatch &barriers);

/**
 * Refreshes the steady-read cache entry for `d3d_resource` after a state
 * update. Only a texture whose subresources are all in one read-only state,
 * with no write-sync debt, pending split barrier or implicit promotion, may be
 * short circuited by a later identical read; anything else drops its entry.
 */
void UpdateReplaySteadyReadState(
    ReplayState &state, ID3D12Resource *d3d_resource, Resource &resource,
    const std::vector<ReplaySubresourceState> &states);

} // namespace dxmt::d3d12
