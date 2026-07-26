#pragma once

// Per-resource replay state bookkeeping: lazily (re)initialized subresource
// state vectors, the touched-resource ledger, aliasing resets, the access
// mismatch diagnostic, and the end-of-submission COMMON decay.
//
// These helpers used to be private members of CommandQueueImpl
// (d3d12_command_queue_replay_records.inc). They only read and write the
// ReplayState value type plus namespace-level resource helpers, so hoisting
// them to dxmt::d3d12 lets them be compiled and analyzed independently. The
// two pieces of queue identity they used to reach for through `this` —
// desc_.Type and device_queue_state_->BackendResourceStates() — are now
// explicit parameters.

#include "d3d12_device_queue_state.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_resource.hpp"

#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Replay state is keyed by the D3D12 resource pointer and reinitialized when
// the underlying allocation identity changes.
[[nodiscard]] std::vector<ReplaySubresourceState> &
GetReplayResourceStates(ReplayState &state, Resource &resource);

// Records `resource` in the per-list touched ledger (first-seen order).
void TouchReplayResource(ReplayState &state, ID3D12Resource *resource);

// Rewinds every subresource of `resource` to its initial state after an
// aliasing barrier.
void ResetReplayResourceStatesForAliasing(ReplayState &state,
                                          Resource &resource);

// Rate-limited diagnostic for a resource used in a state the replay tracker
// does not expect.
void WarnReplayResourceAccessMismatch(const Resource &resource,
                                      UINT subresource,
                                      D3D12_RESOURCE_STATES current,
                                      D3D12_RESOURCE_STATES desired,
                                      const char *context,
                                      D3D12_COMMAND_LIST_TYPE queue_type);

// Applies D3D12 state decay to every backend-tracked subresource of the
// resources touched by a drained submission.
void DecayTouchedResourceStates(
    const std::vector<Com<ID3D12Resource>> &touched_resources,
    ReplayResourceStateMap &backend_resource_states,
    D3D12_COMMAND_LIST_TYPE queue_type);

} // namespace dxmt::d3d12
