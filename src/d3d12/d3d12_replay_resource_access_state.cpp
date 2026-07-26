#include "d3d12_replay_resource_access_state.hpp"

#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_resource_state_semantics.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace dxmt::d3d12 {

namespace {

// Matches the historical WarnReplayResourceAccessMismatch log budget.
constexpr uint32_t kResourceAccessMismatchLogLimit = 128;

} // namespace

std::vector<ReplaySubresourceState> &
GetReplayResourceStates(ReplayState &state, Resource &resource) {
  auto &entry = (*state.resource_states)[resource.GetD3D12Resource()];
  const auto &desc = resource.GetResourceDesc();
  const UINT subresource_count = GetSubresourceCount(resource);
  if (entry.subresources.size() != subresource_count ||
      std::memcmp(&entry.desc, &desc, sizeof(desc)) != 0 ||
      entry.initial_state != resource.GetInitialState() ||
      entry.heap_offset != resource.GetHeapOffset()) {
    ReplaySubresourceState initial = {};
    initial.state = resource.GetInitialState();
    entry.desc = desc;
    entry.initial_state = resource.GetInitialState();
    entry.heap_offset = resource.GetHeapOffset();
    entry.subresources.assign(subresource_count, initial);
    // P2: re-init means the entry's identity changed under this pointer; drop
    // any steady-read cache so a skipped access can't trust a stale state.
    if (!state.steady_read_states.empty())
      state.steady_read_states.erase(resource.GetD3D12Resource());
  }
  return entry.subresources;
}

void
TouchReplayResource(ReplayState &state, ID3D12Resource *resource) {
  if (!state.touched_resources || !resource)
    return;
  // PERF: O(1) membership via the companion set; the vector keeps first-seen
  // order (consumed by DecayTouchedResourceStates). Falls back to linear scan
  // if the set isn't wired (defensive; the set is always set at list start).
  if (state.touched_resources_set) {
    if (!state.touched_resources_set->insert(resource).second)
      return;
    state.touched_resources->push_back(resource);
    return;
  }
  if (std::find_if(state.touched_resources->begin(),
                   state.touched_resources->end(),
                   [resource](const Com<ID3D12Resource> &entry) {
                     return entry.ptr() == resource;
                   }) != state.touched_resources->end())
    return;
  state.touched_resources->push_back(resource);
}

void
ResetReplayResourceStatesForAliasing(ReplayState &state, Resource &resource) {
  auto &states = GetReplayResourceStates(state, resource);
  const auto initial_state = resource.GetInitialState();
  for (auto &entry : states) {
    entry.state = initial_state;
    entry.pending_before = initial_state;
    entry.pending_after = initial_state;
    entry.mismatch_barrier_synced_state = initial_state;
    entry.implicitly_promoted = false;
    entry.has_pending_split = false;
  }
  // P2: aliasing resets state -> drop any steady-read cache entry.
  if (!state.steady_read_states.empty())
    state.steady_read_states.erase(resource.GetD3D12Resource());
}

void
WarnReplayResourceAccessMismatch(const Resource &resource, UINT subresource,
                                 D3D12_RESOURCE_STATES current,
                                 D3D12_RESOURCE_STATES desired,
                                 const char *context,
                                 D3D12_COMMAND_LIST_TYPE queue_type) {
  static std::atomic<uint32_t> log_count = 0;
  if (log_count.fetch_add(1, std::memory_order_relaxed) >=
      kResourceAccessMismatchLogLimit)
    return;
  const auto &desc = resource.GetResourceDesc();
  WARN("D3D12CommandQueue: resource access state mismatch context=",
       context, " subresource=", subresource,
       " resource=", const_cast<Resource &>(resource).GetD3D12Resource(),
       " mip=", GetMipLevel(resource, subresource),
       " slice=", GetArraySlice(resource, subresource),
       " plane=", GetSubresourcePlane(resource, subresource),
       " current=", uint32_t(current), " desired=", uint32_t(desired),
       " queueType=", uint32_t(queue_type),
       " dimension=", uint32_t(desc.Dimension),
       " size=", uint64_t(desc.Width), "x", uint32_t(desc.Height), "x",
       uint32_t(desc.DepthOrArraySize),
       " format=", uint32_t(desc.Format),
       " mipLevels=", uint32_t(GetResourceMipLevelCount(resource)),
       " flags=", uint32_t(desc.Flags),
       " initial=", uint32_t(resource.GetInitialState()));
}

void
DecayTouchedResourceStates(
    const std::vector<Com<ID3D12Resource>> &touched_resources,
    ReplayResourceStateMap &backend_resource_states,
    D3D12_COMMAND_LIST_TYPE queue_type) {
  for (const auto &resource_com : touched_resources) {
    auto *resource = GetResource(resource_com.ptr());
    if (!resource)
      continue;

    auto &resource_states = backend_resource_states;
    auto it = resource_states.find(resource->GetD3D12Resource());
    if (it == resource_states.end())
      continue;

    for (auto &subresource_state : it->second.subresources) {
      if (!IsDecayEligibleResourceState(
              *resource, queue_type, subresource_state.state,
              subresource_state.implicitly_promoted))
        continue;
      subresource_state.state = D3D12_RESOURCE_STATE_COMMON;
      subresource_state.mismatch_barrier_synced_state =
          D3D12_RESOURCE_STATE_COMMON;
      subresource_state.implicitly_promoted = false;
    }
  }
}

} // namespace dxmt::d3d12
