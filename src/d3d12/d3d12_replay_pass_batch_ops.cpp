#include "d3d12_replay_pass_batch_ops.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_replay_pass_compatibility.hpp"
#include "d3d12_replay_perf_timers.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_perf_stats.hpp"
#include "dxmt_statistics.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <utility>

namespace dxmt::d3d12 {

void BumpGraphicsBindingGeneration(
    ReplayState &state, GraphicsBindingGenerationBumpSource source) {
  state.graphics_binding_generation++;
  if (!state.graphics_binding_generation)
    state.graphics_binding_generation = 1;
  if (!ReplayPerfEnabled())
    return;
  auto &timers = perDrawSubTimers();
  timers.bindingGenBumps++;
  switch (source) {
  case GraphicsBindingGenerationBumpSource::PipelineState:
    timers.bindingGenPipeline++;
    break;
  case GraphicsBindingGenerationBumpSource::DescriptorHeaps:
    timers.bindingGenDescriptorHeaps++;
    break;
  case GraphicsBindingGenerationBumpSource::VertexBuffers:
    timers.bindingGenVertexBuffers++;
    break;
  case GraphicsBindingGenerationBumpSource::RootSignature:
    timers.bindingGenRootSignature++;
    break;
  case GraphicsBindingGenerationBumpSource::RootDescriptorTable:
    timers.bindingGenRootDescriptorTable++;
    break;
  case GraphicsBindingGenerationBumpSource::RootDescriptor:
    timers.bindingGenRootDescriptor++;
    break;
  case GraphicsBindingGenerationBumpSource::RootConstants:
    timers.bindingGenRootConstants++;
    break;
  case GraphicsBindingGenerationBumpSource::IndirectVertexBuffer:
    timers.bindingGenIndirectVertexBuffer++;
    break;
  case GraphicsBindingGenerationBumpSource::IndirectRootConstants:
    timers.bindingGenIndirectRootConstants++;
    break;
  case GraphicsBindingGenerationBumpSource::IndirectRootDescriptor:
    timers.bindingGenIndirectRootDescriptor++;
    break;
  }
}

ResolvedReplayVertexBuffer
ResolveReplayVertexBuffer(ReplayState &state,
                          const D3D12_VERTEX_BUFFER_VIEW &view) {
  auto cache_it = state.resolved_vertex_buffer_cache.find(view.BufferLocation);
  if (cache_it != state.resolved_vertex_buffer_cache.end())
    return cache_it->second;

  ResolvedReplayVertexBuffer resolved = {};
  resolved.address = view.BufferLocation;
  UINT64 resource_offset = 0;
  auto *resource =
      LookupBufferResourceByGpuVirtualAddress(view.BufferLocation,
                                              &resource_offset);
  if (!resource || !resource->GetBuffer())
    return resolved;
  resolved.valid = true;
  resolved.binding_offset = resource->GetHeapOffset() + resource_offset;
  resolved.d3d_resource = resource->GetD3D12Resource();
  resolved.resource = resource;
  resolved.buffer = resource->GetBuffer();
  state.resolved_vertex_buffer_cache[view.BufferLocation] = resolved;
  return resolved;
}

ResolvedReplayIndexBuffer
ResolveReplayIndexBuffer(ReplayState &state,
                         const D3D12_INDEX_BUFFER_VIEW &view) {
  auto cache_it = state.resolved_index_buffer_cache.find(view.BufferLocation);
  if (cache_it != state.resolved_index_buffer_cache.end())
    return cache_it->second;

  ResolvedReplayIndexBuffer resolved = {};
  resolved.address = view.BufferLocation;
  UINT64 resource_offset = 0;
  auto *resource =
      LookupBufferResourceByGpuVirtualAddress(view.BufferLocation,
                                              &resource_offset);
  if (!resource || !resource->GetBufferAllocation())
    return resolved;
  resolved.valid = true;
  resolved.binding_offset = resource->GetHeapOffset() + resource_offset;
  resolved.d3d_resource = resource->GetD3D12Resource();
  resolved.resource = resource;
  resolved.allocation = resource->GetBufferAllocation();
  state.resolved_index_buffer_cache[view.BufferLocation] = resolved;
  return resolved;
}

bool ReplayGraphicsPassBatchHasRealWork(
    const ReplayGraphicsPassBatch &batch) {
  if (!batch.active)
    return false;
  for (const auto &command : batch.commands) {
    if (ReplayGraphicsCommandKindIsRealWork(command.kind))
      return true;
  }
  return false;
}

bool ReplayComputePassBatchHasRealWork(
    const ReplayComputePassBatch &batch) {
  if (!batch.active)
    return false;
  for (const auto &command : batch.commands) {
    if (command.kind != ReplayComputeCommandKind::Barrier)
      return true;
  }
  return false;
}

bool CanDeferResourceBarriersIntoGraphicsBatch(
    const ReplayState &state, const ResourceAccessBarrierBatch &batch) {
  if (!(state.graphics_pass_batch.active &&
        ReplayGraphicsPassBatchHasRealWork(state.graphics_pass_batch)) ||
      (state.compute_pass_batch.active &&
       !state.compute_pass_batch.commands.empty()) ||
      !state.blit_batch.commands.empty() ||
      !state.pending_timestamp_markers.empty())
    return false;
  if (!state.pending_resource_barriers.entries.empty() ||
      state.pending_resource_barriers.needs_separator)
    return false;
  if (batch.needs_separator)
    return false;
  const auto &graphics = state.graphics_pass_batch;
  return !ResourceBarrierTouchesRenderPassAttachments(batch,
                                                     graphics.attachments);
}

bool CanDeferResourceBarriersIntoComputeBatch(
    const ReplayState &state, const ResourceAccessBarrierBatch &batch) {
  if (!(state.compute_pass_batch.active &&
        ReplayComputePassBatchHasRealWork(state.compute_pass_batch)) ||
      (state.graphics_pass_batch.active &&
       !state.graphics_pass_batch.commands.empty()) ||
      !state.blit_batch.commands.empty() ||
      !state.pending_timestamp_markers.empty())
    return false;
  if (!state.pending_resource_barriers.entries.empty() ||
      state.pending_resource_barriers.needs_separator)
    return false;
  return !batch.needs_separator;
}

bool HasPendingGraphicsPass(const ReplayState &state) {
  return state.graphics_pass_batch.active &&
         !state.graphics_pass_batch.commands.empty();
}

bool HasPendingComputePass(const ReplayState &state) {
  return state.compute_pass_batch.active &&
         !state.compute_pass_batch.commands.empty();
}

bool HasPendingBlitBatch(const ReplayState &state) {
  return !state.blit_batch.commands.empty();
}

ReplayGraphicsPassPlan BuildReplayGraphicsPassPlan(
    const std::vector<ReplayGraphicsPassCommand> &commands,
    uint64_t argument_buffer_size) {
  ReplayGraphicsPassPlan plan = {};
  plan.command_count = static_cast<uint32_t>(commands.size());
  plan.argument_buffer_size = argument_buffer_size;
  for (const auto &command : commands) {
    if (ReplayGraphicsCommandKindIsIndexed(command.kind))
      plan.indexed_count++;
    if (ReplayGraphicsCommandKindIsIndirect(command.kind))
      plan.indirect_count++;
    if (command.kind == ReplayGraphicsCommandKind::Barrier) {
      plan.compiled_barrier_count++;
    } else if (ReplayGraphicsCommandKindIsIndirect(command.kind)) {
      plan.compiled_indirect_count++;
      plan.compiled_legacy_count++;
    } else if (command.use_geometry || command.use_tessellation) {
      plan.compiled_gs_ts_count++;
      plan.compiled_legacy_count++;
    } else if (command.bindless_compiled_candidate) {
      plan.compiled_candidate_count++;
    } else {
      plan.compiled_legacy_count++;
    }
    if (command.parallel_candidate) {
      plan.parallel_candidate_count++;
      plan.current_parallel_candidate_run++;
      plan.largest_parallel_candidate_run =
          std::max(plan.largest_parallel_candidate_run,
                   plan.current_parallel_candidate_run);
    } else {
      plan.current_parallel_candidate_run = 0;
      plan.all_parallel_candidates = false;
    }
  }
  return plan;
}

bool HasPendingReplayWork(const ReplayState &state) {
  return HasPendingBlitBatch(state) || HasPendingComputePass(state) ||
         HasPendingGraphicsPass(state) ||
         !state.pending_timestamp_markers.empty();
}

void ResolveSubmissionEncoderBoundary(ReplayState &state,
                                      CompiledEncoderKind incoming_kind,
                                      bool compatible) {
  auto *telemetry = state.submission_boundary_telemetry;
  if (state.submission_boundary_kind == CompiledEncoderKind::None)
    return;
  if (compatible &&
      state.submission_boundary_kind == incoming_kind) {
    if (telemetry) {
      auto &counter =
          incoming_kind == CompiledEncoderKind::Graphics
              ? telemetry
                    ->submission_graphics_encoder_boundary_merges
              : telemetry
                    ->submission_compute_encoder_boundary_merges;
      counter.fetch_add(1, std::memory_order_relaxed);
    }
    if (auto *stats = dxmt::perf::currentFrameStatistics()) {
      if (incoming_kind == CompiledEncoderKind::Graphics)
        stats->frame_submission_graphics_encoder_boundary_merges++;
      else
        stats->frame_submission_compute_encoder_boundary_merges++;
    }
  } else {
    if (telemetry) {
      telemetry->submission_encoder_boundary_flushes.fetch_add(
          1, std::memory_order_relaxed);
    }
    if (auto *stats = dxmt::perf::currentFrameStatistics())
      stats->frame_submission_encoder_boundary_flushes++;
  }
  state.submission_boundary_telemetry = nullptr;
  state.submission_boundary_kind = CompiledEncoderKind::None;
}

ResourceAccessBarrierBatch
TakePendingResourceBarrierBatch(ReplayState &state) {
  auto batch = std::move(state.pending_resource_barriers);
  state.pending_resource_barriers = {};
  return batch;
}

ResourceAccessBarrierBatch TakeMatchingPendingResourceBarriers(
    ReplayState &state, const std::unordered_set<ID3D12Resource *> &reads,
    const std::unordered_set<ID3D12Resource *> &writes) {
  const bool perf_enabled = ReplayPerfEnabled();
  const auto take_begin =
      perf_enabled ? clock::now() : clock::time_point{};
  auto &timers = perDrawSubTimers();
  if (perf_enabled) {
    const auto pending = state.pending_resource_barriers.entries.size();
    timers.blitBarrierPendingEntries += pending;
    timers.blitBarrierPendingEntriesMax =
        std::max<uint64_t>(timers.blitBarrierPendingEntriesMax, pending);
  }
  auto &pending = state.pending_resource_barriers;
  if (pending.needs_separator)
    return TakePendingResourceBarrierBatch(state);

  ResourceAccessBarrierBatch matched;

  const bool use_resource_index = !pending.entry_index.empty();
  if (!use_resource_index) {
    ResourceAccessBarrierBatch remaining;
    remaining.entries.reserve(pending.entries.size());
    for (auto &entry : pending.entries) {
      auto *resource = entry.d3d_resource.ptr();
      if (resource && (reads.count(resource) || writes.count(resource)))
        matched.entries.push_back(std::move(entry));
      else
        remaining.entries.push_back(std::move(entry));
    }
    pending.entries = std::move(remaining.entries);
  } else {
    auto take_resource = [&](ID3D12Resource *resource) {
      auto resource_it = pending.resource_entry_keys.find(resource);
      if (resource_it == pending.resource_entry_keys.end())
        return;

      auto keys = std::move(resource_it->second);
      pending.resource_entry_keys.erase(resource_it);
      matched.entries.reserve(matched.entries.size() + keys.size());
      for (const auto &key : keys) {
        auto entry_it = pending.entry_index.find(key);
        if (entry_it == pending.entry_index.end())
          continue;

        const size_t index = entry_it->second;
        pending.entry_index.erase(entry_it);
        matched.entries.push_back(std::move(pending.entries[index]));

        const size_t last_index = pending.entries.size() - 1;
        if (index != last_index) {
          pending.entries[index] = std::move(pending.entries[last_index]);
          auto moved_key =
              MakeResourceAccessBarrierKey(pending.entries[index]);
          auto moved_it = pending.entry_index.find(moved_key);
          if (moved_it != pending.entry_index.end())
            moved_it->second = index;
        }
        pending.entries.pop_back();
      }
    };

    for (auto *resource : reads)
      take_resource(resource);
    for (auto *resource : writes)
      take_resource(resource);
  }
  const auto scan_end = perf_enabled ? clock::now() : clock::time_point{};
  const auto matched_rebuild_end =
      perf_enabled ? clock::now() : clock::time_point{};
  const auto remaining_rebuild_end =
      perf_enabled ? clock::now() : clock::time_point{};
  if (perf_enabled) {
    const auto assign_end = clock::now();
    const auto us = [](auto duration) -> uint64_t {
      return std::chrono::duration_cast<std::chrono::microseconds>(duration)
          .count();
    };
    timers.blitBarrierTakeUs += us(assign_end - take_begin);
    if (use_resource_index)
      timers.blitBarrierLookupUs += us(scan_end - take_begin);
    else
      timers.blitBarrierScanUs += us(scan_end - take_begin);
    timers.blitBarrierRebuildMatchedUs +=
        us(matched_rebuild_end - scan_end);
    timers.blitBarrierRebuildRemainingUs +=
        us(remaining_rebuild_end - matched_rebuild_end);
    timers.blitBarrierAssignUs += us(assign_end - remaining_rebuild_end);
    timers.blitBarrierMatchedEntries += matched.entries.size();
    timers.blitBarrierRemainingEntries +=
        pending.entries.size();
  }
  return matched;
}

} // namespace dxmt::d3d12
