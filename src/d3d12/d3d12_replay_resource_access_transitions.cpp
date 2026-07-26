#include "d3d12_replay_resource_access_transitions.hpp"

#include "d3d12_replay_barrier_record.hpp"
#include "d3d12_replay_mismatch_barrier_policy.hpp"
#include "d3d12_replay_resource_access_state.hpp"
#include "d3d12_resource_state_semantics.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

bool
ReplayResourceAccessIsSteadyReadNoop(const ReplayState &state,
                                     ID3D12Resource *d3d_resource,
                                     D3D12_RESOURCE_STATES desired) {
  if (desired == D3D12_RESOURCE_STATE_COMMON ||
      !IsReadOnlyResourceState(desired))
    return false;
  auto sit = state.steady_read_states.find(d3d_resource);
  return sit != state.steady_read_states.end() &&
         IsReadOnlyResourceState(sit->second) &&
         (uint32_t(sit->second) & uint32_t(desired)) == uint32_t(desired);
}

bool
ApplyReplayResourceAccessTransitions(
    std::vector<ReplaySubresourceState> &states, Resource &resource,
    D3D12_RESOURCE_STATES desired, UINT first, UINT count, const char *context,
    D3D12_COMMAND_LIST_TYPE queue_type, ResourceAccessBarrierBatch &barriers) {
  bool has_mismatch_resource_barriers = false;
  auto add_mismatch_barrier = [&](Resource &resource, UINT subresource,
                                  D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    int access = ResourceAccessForState(before) | ResourceAccessForState(after);
    if (!access)
      access = ResourceAccess::All;
    AddResourceAccessBarrier(barriers, resource, subresource, 1, access);
    has_mismatch_resource_barriers = true;
  };
  for (UINT i = 0; i < count; i++) {
    const UINT subresource = first + i;
    auto &current = states[subresource];
    if (current.state == desired) {
      // A previous inferred write may have used this resource through a
      // different write state while the app-visible D3D state stayed put.
      // Synchronize that write before returning to the explicit state.
      if (StateHasWriteAccess(desired) &&
          StateHasWriteAccess(current.mismatch_barrier_synced_state) &&
          current.mismatch_barrier_synced_state != desired) {
        add_mismatch_barrier(resource, subresource,
                             current.mismatch_barrier_synced_state, desired);
        current.mismatch_barrier_synced_state = desired;
        continue;
      }
      if (IsReadOnlyResourceState(desired) &&
          StateHasWriteAccess(current.mismatch_barrier_synced_state)) {
        add_mismatch_barrier(resource, subresource,
                             current.mismatch_barrier_synced_state, desired);
        current.mismatch_barrier_synced_state = desired;
        continue;
      }
      if (StateHasWriteAccess(desired))
        current.mismatch_barrier_synced_state =
            D3D12_RESOURCE_STATE_COMMON;
      continue;
    }

    if (current.state == D3D12_RESOURCE_STATE_COMMON &&
        IsImplicitPromotionCompatibleState(resource, desired)) {
      current.state = desired;
      current.mismatch_barrier_synced_state = D3D12_RESOURCE_STATE_COMMON;
      current.implicitly_promoted = true;
      continue;
    }

    if (IsReadOnlyResourceState(current.state) &&
        IsReadOnlyResourceState(desired)) {
      current.state =
          D3D12_RESOURCE_STATES(uint32_t(current.state) | uint32_t(desired));
      current.mismatch_barrier_synced_state = D3D12_RESOURCE_STATE_COMMON;
      continue;
    }

    if (ShouldEmitReadAfterWriteMismatchBarrier(current.state, desired)) {
      if (current.mismatch_barrier_synced_state == current.state)
        continue;
      add_mismatch_barrier(resource, subresource, current.state, desired);
      current.mismatch_barrier_synced_state = current.state;
      continue;
    }

    if (ShouldEmitWriteAfterReadMismatchBarrier(current.state, desired)) {
      if (current.mismatch_barrier_synced_state == desired)
        continue;
      add_mismatch_barrier(resource, subresource, current.state, desired);
      current.mismatch_barrier_synced_state = desired;
      continue;
    }

    if (ShouldEmitWriteAfterWriteMismatchBarrier(current.state, desired)) {
      if (current.mismatch_barrier_synced_state == desired)
        continue;
      const auto before =
          StateHasWriteAccess(current.mismatch_barrier_synced_state)
              ? current.mismatch_barrier_synced_state
              : current.state;
      add_mismatch_barrier(resource, subresource, before, desired);
      current.mismatch_barrier_synced_state = desired;
      continue;
    }

    WarnReplayResourceAccessMismatch(resource, subresource, current.state,
                                     desired, context, queue_type);
    // Resource use does not transition D3D12 state. Keep the last explicit
    // or implicitly promoted state so a missing or mismatched barrier does
    // not poison subsequent state tracking.
  }
  return has_mismatch_resource_barriers;
}

void
UpdateReplaySteadyReadState(
    ReplayState &state, ID3D12Resource *d3d_resource, Resource &resource,
    const std::vector<ReplaySubresourceState> &states) {
  // Only textures are eligible. Buffers can carry pending timestamp/CPU query
  // resolves, which must not be skipped by a replay-state shortcut.
  bool eligible = !resource.GetBufferAllocation() && !states.empty() &&
                  IsReadOnlyResourceState(states[0].state) &&
                  states[0].mismatch_barrier_synced_state ==
                      D3D12_RESOURCE_STATE_COMMON &&
                  !states[0].has_pending_split &&
                  !states[0].implicitly_promoted;
  const D3D12_RESOURCE_STATES uniform =
      eligible ? states[0].state : D3D12_RESOURCE_STATE_COMMON;
  for (size_t s = 1; eligible && s < states.size(); s++) {
    const auto &ss = states[s];
    if (ss.state != uniform ||
        ss.mismatch_barrier_synced_state != D3D12_RESOURCE_STATE_COMMON ||
        ss.has_pending_split || ss.implicitly_promoted)
      eligible = false;
  }
  if (eligible)
    state.steady_read_states[d3d_resource] = uniform;
  else
    state.steady_read_states.erase(d3d_resource);
}

} // namespace dxmt::d3d12
