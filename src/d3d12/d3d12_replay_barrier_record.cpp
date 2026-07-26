#include "d3d12_replay_barrier_record.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_replay_barrier_encode.hpp"
#include "d3d12_replay_perf_timers.hpp"
#include "d3d12_replay_resource_access_state.hpp"
#include "d3d12_resource_state_semantics.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"

#include <cstdint>
#include <utility>

namespace dxmt::d3d12 {

void
AddResourceAccessBarrier(ResourceAccessBarrierBatch &batch, Resource &resource,
                         UINT first_subresource, UINT subresource_count,
                         int access, bool requires_cross_submit_wait) {
  auto add_or_merge_entry = [&batch](ResourceAccessBarrierEntry entry) {
    if (batch.entry_index.empty()) {
      if (batch.entries.size() < kResourceAccessBarrierLinearEntryLimit) {
        for (auto &existing : batch.entries) {
          if (!ResourceAccessBarrierEntriesMatch(existing, entry))
            continue;
          MergeResourceAccessBarrierEntry(existing, std::move(entry));
          return;
        }
      } else {
        RebuildResourceAccessBarrierIndex(batch);
      }
    }

    if (!batch.entry_index.empty()) {
      auto key = MakeResourceAccessBarrierKey(entry);
      auto found = batch.entry_index.find(key);
      if (found != batch.entry_index.end()) {
        MergeResourceAccessBarrierEntry(batch.entries[found->second],
                                        std::move(entry));
        return;
      }
      batch.entry_index.emplace(key, batch.entries.size());
      if (auto *resource = entry.d3d_resource.ptr())
        batch.resource_entry_keys[resource].push_back(key);
    }

    batch.entries.push_back(std::move(entry));
  };

  if (resource.GetBuffer()) {
    Rc<Buffer> buffer = resource.GetBuffer();
    const UINT64 length = resource.GetResourceDesc().Width;
    add_or_merge_entry({Com<ID3D12Resource>(resource.GetD3D12Resource()),
                        std::move(buffer), {}, length, 0, 0, access,
                        requires_cross_submit_wait});
    return;
  }

  if (resource.GetTextureAllocation()) {
    bool added = false;
    for (UINT i = 0; i < subresource_count; i++) {
      const UINT subresource = first_subresource + i;
      const UINT plane = GetSubresourcePlane(resource, subresource);
      if (!resource.GetTextureAllocation(plane))
        continue;
      Rc<Texture> texture = Rc<Texture>(resource.GetTexture(plane));
      if (!texture)
        continue;
      const UINT level = GetMipLevel(resource, subresource);
      const UINT slice = GetArraySlice(resource, subresource);
      add_or_merge_entry({Com<ID3D12Resource>(resource.GetD3D12Resource()),
                          {}, std::move(texture), 0, level, slice, access,
                          requires_cross_submit_wait});
      added = true;
    }
    if (subresource_count && !added)
      batch.needs_separator = true;
    return;
  }

  batch.needs_separator = true;
}

void
ReplayTransitionBarrier(ReplayState &state,
                        const StoredResourceBarrier &barrier,
                        ResourceAccessBarrierBatch &batch,
                        D3D12_COMMAND_LIST_TYPE queue_type) {
  auto *resource = GetResource(barrier.resource.ptr());
  if (!resource) {
    WARN("D3D12CommandQueue: transition barrier skipped for foreign resource");
    batch.needs_separator = true;
    return;
  }

  const auto &transition = barrier.barrier.Transition;
  WarnUnsupportedResourceState(transition.StateBefore, "transition before");
  WarnUnsupportedResourceState(transition.StateAfter, "transition after");

  auto &states = GetReplayResourceStates(state, *resource);
  TouchReplayResource(state, resource->GetD3D12Resource());
  // P2: a transition mutates state -> drop the steady-read cache entry so the
  // next access re-evaluates via the full path (and may repopulate).
  if (!state.steady_read_states.empty())
    state.steady_read_states.erase(resource->GetD3D12Resource());
  const UINT subresource_count = states.size();
  const bool all_subresources =
      transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  if (!all_subresources && transition.Subresource >= subresource_count) {
    WARN("D3D12CommandQueue: transition barrier subresource out of range ",
         transition.Subresource, " count=", subresource_count);
    batch.needs_separator = true;
    return;
  }

  const UINT first = all_subresources ? 0 : transition.Subresource;
  const UINT count = all_subresources ? subresource_count : 1;
  const bool begin_only =
      barrier.barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
  const bool end_only =
      barrier.barrier.Flags & D3D12_RESOURCE_BARRIER_FLAG_END_ONLY;
  for (UINT i = 0; i < count; i++) {
    const UINT subresource = first + i;
    auto &current = states[subresource];
    if (begin_only) {
      if (current.has_pending_split) {
        const auto &desc = resource->GetResourceDesc();
        WARN("D3D12CommandQueue: split transition BEGIN overwrites pending barrier"
             " subresource=", subresource,
             " pendingBefore=", uint32_t(current.pending_before),
             " pendingAfter=", uint32_t(current.pending_after),
             " before=", uint32_t(transition.StateBefore),
             " after=", uint32_t(transition.StateAfter),
             " queueType=", uint32_t(queue_type),
             " dimension=", uint32_t(desc.Dimension),
             " flags=", uint32_t(desc.Flags));
      }
      if (current.state != transition.StateBefore &&
          IsImplicitPromotionCompatibleResource(*resource, current.state,
                                                transition.StateBefore)) {
        current.state = transition.StateBefore;
        current.mismatch_barrier_synced_state =
            D3D12_RESOURCE_STATE_COMMON;
        current.implicitly_promoted = true;
      }
      if (!IsTransitionBeforeStateCompatible(queue_type, current.state,
                                             transition.StateBefore)) {
        const auto &desc = resource->GetResourceDesc();
        WARN("D3D12CommandQueue: split transition BEGIN state mismatch"
             " subresource=", subresource,
             " expected=", uint32_t(current.state),
             " before=", uint32_t(transition.StateBefore),
             " after=", uint32_t(transition.StateAfter),
             " queueType=", uint32_t(queue_type),
             " dimension=", uint32_t(desc.Dimension),
             " flags=", uint32_t(desc.Flags),
             " initial=", uint32_t(resource->GetInitialState()));
      }
      current.pending_before = transition.StateBefore;
      current.pending_after = transition.StateAfter;
      current.has_pending_split = true;
      continue;
    }

    if (end_only) {
      if (!current.has_pending_split ||
          current.pending_before != transition.StateBefore ||
          current.pending_after != transition.StateAfter) {
        const auto &desc = resource->GetResourceDesc();
        WARN("D3D12CommandQueue: split transition END mismatch"
             " subresource=", subresource,
             " hasPending=", current.has_pending_split,
             " pendingBefore=", uint32_t(current.pending_before),
             " pendingAfter=", uint32_t(current.pending_after),
             " before=", uint32_t(transition.StateBefore),
             " after=", uint32_t(transition.StateAfter),
             " queueType=", uint32_t(queue_type),
             " dimension=", uint32_t(desc.Dimension),
             " flags=", uint32_t(desc.Flags),
             " initial=", uint32_t(resource->GetInitialState()));
      }
      current.has_pending_split = false;
      current.state = transition.StateAfter;
      current.mismatch_barrier_synced_state =
          D3D12_RESOURCE_STATE_COMMON;
      current.implicitly_promoted = false;
      continue;
    }

    if (current.has_pending_split) {
      const auto &desc = resource->GetResourceDesc();
      WARN("D3D12CommandQueue: transition barrier clears unmatched split barrier"
           " subresource=", subresource,
           " pendingBefore=", uint32_t(current.pending_before),
           " pendingAfter=", uint32_t(current.pending_after),
           " before=", uint32_t(transition.StateBefore),
           " after=", uint32_t(transition.StateAfter),
           " queueType=", uint32_t(queue_type),
           " dimension=", uint32_t(desc.Dimension),
           " flags=", uint32_t(desc.Flags));
      current.has_pending_split = false;
    }
    if (current.state != transition.StateBefore &&
        IsImplicitPromotionCompatibleResource(*resource, current.state,
                                              transition.StateBefore)) {
      current.state = transition.StateBefore;
      current.mismatch_barrier_synced_state =
          D3D12_RESOURCE_STATE_COMMON;
      current.implicitly_promoted = true;
    }
    if (!IsTransitionBeforeStateCompatible(queue_type, current.state,
                                           transition.StateBefore)) {
      const auto &desc = resource->GetResourceDesc();
      WARN("D3D12CommandQueue: transition barrier state mismatch subresource=",
           subresource, " expected=", uint32_t(current.state),
           " before=", uint32_t(transition.StateBefore),
           " after=", uint32_t(transition.StateAfter),
           " queueType=", uint32_t(queue_type),
           " dimension=", uint32_t(desc.Dimension),
           " flags=", uint32_t(desc.Flags),
           " initial=", uint32_t(resource->GetInitialState()));
    }
    current.state = transition.StateAfter;
    current.mismatch_barrier_synced_state = D3D12_RESOURCE_STATE_COMMON;
    current.implicitly_promoted = false;
  }

  const int before_access = ResourceAccessForState(transition.StateBefore);
  const int after_access = ResourceAccessForState(transition.StateAfter);
  int access = before_access | after_access;
  if (!access)
    access = ResourceAccess::All;
  AddResourceAccessBarrier(batch, *resource, first, count, access);
}

void
ReplayUavBarrier(const StoredResourceBarrier &barrier,
                 ResourceAccessBarrierBatch &batch) {
  if (barrier.resource) {
    auto *resource = GetResource(barrier.resource.ptr());
    if (!resource) {
      WARN("D3D12CommandQueue: UAV barrier skipped for foreign resource");
      batch.needs_separator = true;
      return;
    }
    AddResourceAccessBarrier(batch, *resource, 0,
                             GetSubresourceCount(*resource),
                             ResourceAccess::All, true);
    return;
  }

  batch.needs_separator = true;
}

void
ReplayAliasingBarrier(ReplayState &state, const StoredResourceBarrier &barrier,
                      ResourceAccessBarrierBatch &batch) {
  if (auto *before = GetResource(barrier.resource_before.ptr())) {
    AddResourceAccessBarrier(batch, *before, 0,
                             GetSubresourceCount(*before),
                             ResourceAccess::All);
  } else if (barrier.resource_before) {
    WARN("D3D12CommandQueue: aliasing barrier has foreign before resource");
  }

  if (auto *after = GetResource(barrier.resource_after.ptr())) {
    ActivateBufferGpuVirtualAddress(after);
    ResetReplayResourceStatesForAliasing(state, *after);
    TouchReplayResource(state, after->GetD3D12Resource());
    AddResourceAccessBarrier(batch, *after, 0, GetSubresourceCount(*after),
                             ResourceAccess::All);
  } else if (barrier.resource_after) {
    WARN("D3D12CommandQueue: aliasing barrier has foreign after resource");
  }

  batch.needs_separator = true;
}

ResourceAccessBarrierBatch
BuildResourceBarrierBatch(ReplayState &state,
                          const ResourceBarrierRecord &record,
                          D3D12_COMMAND_LIST_TYPE queue_type) {
  ResourceAccessBarrierBatch batch;
  if (record.barriers.empty())
    return batch;

  for (const auto &barrier : record.barriers) {
    switch (barrier.barrier.Type) {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
      ReplayTransitionBarrier(state, barrier, batch, queue_type);
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
      ReplayUavBarrier(barrier, batch);
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
      ReplayAliasingBarrier(state, barrier, batch);
      break;
    default:
      WARN("D3D12CommandQueue: unsupported resource barrier type ",
           barrier.barrier.Type);
      batch.needs_separator = true;
      break;
    }
  }

  return batch;
}

void
QueueResourceAccessBarrierBatch(ReplayState &state,
                                ResourceAccessBarrierBatch batch) {
  if (batch.entries.empty() && !batch.needs_separator)
    return;

  auto &pending = state.pending_resource_barriers;
  if (!pending.entries.empty() || pending.needs_separator) {
    if (auto *stats = dxmt::perf::currentFrameStatistics())
      stats->resource_barrier_batches_merged++;
  }
  MergeResourceAccessBarrierBatch(pending, std::move(batch));
}

void
ReplayResourceBarrier(CommandChunk *chunk, ReplayState &state,
                      const ResourceBarrierRecord &record,
                      D3D12_COMMAND_LIST_TYPE queue_type) {
  auto batch = BuildResourceBarrierBatch(state, record, queue_type);
  // Explicit barrier changes resource state — invalidate the descAccess cache.
  state.access_epoch++;
  // Stage-2 probe: count app-DECLARED barrier transitions (vs auto-inferred
  // mismatchBarriers) to gauge whether FH4's barriers are passthrough-complete.
  if (ReplayPerfEnabled())
    perDrawSubTimers().appBarrierTransitions += record.barriers.size();
  QueueResourceAccessBarrierBatch(state, std::move(batch));
}

} // namespace dxmt::d3d12
