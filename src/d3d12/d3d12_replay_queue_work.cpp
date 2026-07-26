#include "d3d12_replay_queue_work.hpp"

#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_replay_barrier_encode.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_tile_mapping.hpp"
#include "dxmt_context.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace dxmt::d3d12 {

bool
ReplaySparseUpdateQueueWork(CommandChunk *chunk, SparseUpdateQueueWork &work) {
  auto &group = work.group;
  chunk->beginSparseMappingDiagnostic(
      work.resource_identity, uint64_t(group.texture.handle),
      uint64_t(group.placement_heap.handle), work.gpu_resource_id,
      work.mapping_generation, work.operation_count, work.map_count,
      work.unmap_count, group.barrier_subresources.empty() ? 0 : 1);
  chunk->emitcc([
      texture = group.texture,
      placement_heap = group.placement_heap,
      operations = std::move(group.operations),
      resource = std::move(group.resource),
      heap = std::move(group.heap), has_map = work.has_map,
      chunk](ArgumentEncodingContext &enc) mutable {
    if (!enc.queue().UpdateSparseTextureMappings(
            texture, placement_heap, operations.data(), operations.size())) {
      RecordTileMappingMetalFailure();
      WARN("D3D12CommandQueue: TODO Metal4 UpdateTileMappings failed"
           " resource=", resource.ptr(),
           " heap=", heap.ptr(),
           " ops=", operations.size(),
           " hasMap=", has_map);
      auto *resource_object =
          dynamic_cast<d3d12::Resource *>(resource.ptr());
      chunk->completeSparseMappingDiagnostic(
          resource_object ? resource_object->GetTileMappingGeneration() : 0,
          false);
      return;
    }
    if (auto *resource_object =
            dynamic_cast<d3d12::Resource *>(resource.ptr())) {
      chunk->completeSparseMappingDiagnostic(
          resource_object->GetTileMappingGeneration(), true);
    } else {
      chunk->completeSparseMappingDiagnostic(0, false);
    }
  });
  EmitSparseTextureMappingBarrier(
      chunk, std::move(group.barrier_texture),
      std::move(group.barrier_subresources));
  return true;
}

bool
ReplaySparseCopyQueueWork(CommandChunk *chunk, SparseCopyQueueWork &work) {
  for (auto &group : work.groups) {
    chunk->emitcc([
        texture = group.texture,
        placement_heap = group.placement_heap,
        resource = std::move(group.resource),
        heap = std::move(group.heap),
        operations = std::move(group.operations)](
                           ArgumentEncodingContext &enc) mutable {
      if (!enc.queue().UpdateSparseTextureMappings(
              texture, placement_heap, operations.data(),
              operations.size())) {
        WARN("D3D12CommandQueue: TODO Metal4 CopyTileMappings failed"
             " dst=", resource.ptr(),
             " heap=", heap.ptr(),
             " ops=", operations.size());
      }
    });
    EmitSparseTextureMappingBarrier(
        chunk, std::move(group.barrier_texture),
        std::move(group.barrier_subresources));
  }
  return true;
}

bool
ReplaySwapChainResizeQueueWork(
    SwapChainResizeQueueWork &work,
    ResourceAccessBarrierBatch &pending_queue_resource_barriers,
    D3D12DeviceQueueState &device_queue_state) {
  bool ready = true;
  std::unordered_set<ID3D12Resource *> backbuffer_set;
  backbuffer_set.reserve(work.backbuffers.size());
  for (const auto &backbuffer : work.backbuffers) {
    if (backbuffer)
      backbuffer_set.insert(backbuffer.ptr());
  }

  ResourceAccessBarrierBatch detached;
  ResourceAccessBarrierBatch remaining;
  remaining.needs_separator =
      pending_queue_resource_barriers.needs_separator;
  for (auto &entry : pending_queue_resource_barriers.entries) {
    if (backbuffer_set.count(entry.d3d_resource.ptr()))
      detached.entries.push_back(std::move(entry));
    else
      remaining.entries.push_back(std::move(entry));
  }
  pending_queue_resource_barriers = std::move(remaining);
  if (pending_queue_resource_barriers.entries.size() >=
      kResourceAccessBarrierLinearEntryLimit) {
    RebuildResourceAccessBarrierIndex(pending_queue_resource_barriers);
  }

  for (const auto &backbuffer : work.backbuffers) {
    if (!backbuffer)
      continue;
    const ULONG detached_ref_count = static_cast<ULONG>(std::count_if(
        detached.entries.begin(), detached.entries.end(),
        [&](const ResourceAccessBarrierEntry &entry) {
          return entry.d3d_resource.ptr() == backbuffer.ptr();
        }));
    const ULONG ref_count = backbuffer->AddRef();
    backbuffer->Release();
    if (ref_count > 2 + detached_ref_count) {
      MergeResourceAccessBarrierBatch(pending_queue_resource_barriers,
                                      std::move(detached));
      ready = false;
      break;
    }
  }

  if (ready) {
    for (auto *backbuffer : backbuffer_set)
      device_queue_state.EraseResource(backbuffer);
  }
  work.result->ready.store(ready, std::memory_order_release);
  return false;
}

} // namespace dxmt::d3d12
