#include "d3d12_sparse_mapping_submission.hpp"

#include "d3d12_heap.hpp"
#include "d3d12_sparse_tile_copy.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"

#include <unordered_map>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {

namespace {

// One heap's worth of a CopyTileMappings batch, packaged for replay. The
// apitrace record is emitted here so that each group is traced in the same
// order it is appended to the submission.
SparseMappingGroup
MakeSparseMappingGroup(const void *queue_identity, ID3D12Resource *resource,
                       Resource &state, WMT::Texture texture, ID3D12Heap *heap,
                       WMT::Heap placement_heap,
                       std::vector<WMTSparseTextureMappingOperation> ops) {
  dxmt::apitrace::record_sparse_texture_mapping_ops(
      queue_identity, resource, heap, "CopyTileMappings", ops.data(),
      ops.size());
  auto barrier_subresources = CollectSparseTileBarrierSubresources(ops);
  return {
      .texture = texture,
      .placement_heap = placement_heap,
      .operations = std::move(ops),
      .resource = Com<ID3D12Resource>(resource),
      .heap = Com<ID3D12Heap>(heap),
      .barrier_texture = Rc<Texture>(state.GetTexture()),
      .barrier_subresources = std::move(barrier_subresources),
  };
}

} // namespace

bool
BuildSparseTileMappingUpdatePlan(
    const void *queue_identity, IMTLD3D12Device *parent_device,
    ID3D12Resource *resource, UINT region_count,
    const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
    const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap,
    UINT range_count, const D3D12_TILE_RANGE_FLAGS *range_flags,
    const UINT *heap_range_offsets, const UINT *range_tile_counts,
    D3D12_TILE_MAPPING_FLAGS flags, SparseTileMappingUpdatePlan &plan) {
  dxmt::apitrace::record_update_tile_mappings(
      queue_identity, resource, region_count, region_start_coordinates,
      region_sizes, heap, range_count, range_flags, heap_range_offsets,
      range_tile_counts, flags);
  RecordTileMappingCall();
  if (!resource || !region_count || !region_start_coordinates) {
    return false;
  }
  if (flags && ShouldLogTileMappingDiag()) {
    WARN("D3D12CommandQueue: TODO UpdateTileMappings flags are ignored"
         " flags=", flags,
         " resource=", resource,
         " regions=", region_count,
         " ranges=", range_count);
  }

  auto *resource_object = dynamic_cast<d3d12::Resource *>(resource);
  auto *heap_object = heap ? dynamic_cast<d3d12::Heap *>(heap) : nullptr;
  const auto *tiling = resource_object ? resource_object->GetTiling() : nullptr;
  if (!resource_object ||
      resource_object->GetParentDevice() != parent_device ||
      !resource_object->IsReserved() || !tiling ||
      (heap && (!heap_object ||
                heap_object->GetParentDevice() != parent_device))) {
    if (ShouldLogTileMappingDiag()) {
      WARN("D3D12CommandQueue: UpdateTileMappings requires local reserved resource and heap"
           " resource=", resource,
           " heap=", heap,
           " regions=", region_count,
           " ranges=", range_count,
           " flags=", flags);
    }
    return false;
  }

  std::vector<WMTSparseTextureMappingOperation> ops;
  TileRangeCursor cursor = {};
  const auto ranges = TileRangeView::FromAbi(
      range_count, range_flags, heap_range_offsets, range_tile_counts);
  for (UINT i = 0; i < region_count; ++i) {
    const D3D12_TILE_REGION_SIZE *size =
        region_sizes ? &region_sizes[i] : nullptr;
    if (!AppendSparseTileMappingRegion(
            *tiling, region_start_coordinates[i], size, cursor,
            ranges, ops)) {
      RecordTileMappingInvalid();
      dxmt::perf::recordTileMapping(0, 0, 0, 0, 1, 0, 0);
      if (ShouldLogTileMappingDiag()) {
        const auto &coord = region_start_coordinates[i];
        WARN("D3D12CommandQueue: TODO UpdateTileMappings unsupported or invalid region"
             " resource=", resource,
             " regionIndex=", i,
             " subresource=", coord.Subresource,
             " x=", coord.X,
             " y=", coord.Y,
             " z=", coord.Z,
             " useBox=", size ? size->UseBox : 0,
             " numTiles=", size ? size->NumTiles : 1,
             " width=", size ? size->Width : 1,
             " height=", size ? size->Height : 1,
             " depth=", size ? size->Depth : 1,
             " ranges=", range_count);
      }
      return false;
    }
  }
  if (!TileRangeCursorConsumedAll(cursor, ranges) &&
      ShouldLogTileMappingDiag()) {
    WARN("D3D12CommandQueue: UpdateTileMappings tile range count mismatch"
         " resource=", resource,
         " consumed=", cursor.total_consumed,
         " rangeIndex=", cursor.index,
         " rangeConsumed=", cursor.consumed,
         " ranges=", range_count);
  }
  if (ops.empty()) {
    return false;
  }
  auto perf_delta = RecordTileMappingOps(*tiling, ops);
  LogTileMappingStats("UpdateTileMappings");

  if (!resource_object->UsesPlacementSparse()) {
    dxmt::apitrace::record_sparse_texture_mapping_ops(
        queue_identity, resource, heap,
        "UpdateTileMappings fully-backed fallback", ops.data(), ops.size());
    ApplySparseTileMappingOpsToResource(*resource_object, *tiling, heap,
                                        ops);
    dxmt::perf::recordTileMapping(
        perf_delta.standard_ops, perf_delta.packed_ops,
        perf_delta.map_ops, perf_delta.unmap_ops, 0, 0, 0);
    return false;
  }

  const bool has_map = AnySparseTileMappingOpMaps(ops);

  WMT::Heap placement_heap = {};
  if (has_map &&
      !ResolveSparseUpdatePlacementHeap(*resource_object, heap_object,
                                        resource, heap, ops, perf_delta,
                                        placement_heap))
    return false;

  ApplySparseTileMappingOpsToResource(*resource_object, *tiling, heap, ops);
  if (!has_map && !resource_object->GetTextureAllocation()) {
    dxmt::perf::recordTileMapping(
        perf_delta.standard_ops, perf_delta.packed_ops,
        perf_delta.map_ops, perf_delta.unmap_ops, 0, 0, 0);
    return false;
  }

  auto texture = resource_object->GetTextureAllocation()->texture();
  const uint64_t sparse_resource_identity =
      resource_object->GetDescriptorIdentity();
  const uint64_t sparse_gpu_resource_id =
      resource_object->GetTextureAllocation()->gpuResourceID;
  const uint64_t sparse_mapping_generation =
      resource_object->GetTileMappingGeneration();
  const uint32_t sparse_operation_count =
      ClampSparseCountToUint32(ops.size());
  const uint32_t sparse_map_count =
      ClampSparseCountToUint32(perf_delta.map_ops);
  const uint32_t sparse_unmap_count =
      ClampSparseCountToUint32(perf_delta.unmap_ops);
  auto barrier_subresources = CollectSparseTileBarrierSubresources(ops);
  const size_t barrier_subresource_count = barrier_subresources.size();
  dxmt::apitrace::record_sparse_texture_mapping_ops(
      queue_identity, resource, heap, "UpdateTileMappings", ops.data(),
      ops.size());
  Com<ID3D12Resource> resource_ref = resource;
  Com<ID3D12Heap> heap_ref = heap;
  Rc<Texture> barrier_texture = Rc<Texture>(resource_object->GetTexture());
  plan.work = SparseUpdateQueueWork{
      .group =
          {
              .texture = texture,
              .placement_heap = placement_heap,
              .operations = std::move(ops),
              .resource = std::move(resource_ref),
              .heap = std::move(heap_ref),
              .barrier_texture = std::move(barrier_texture),
              .barrier_subresources = std::move(barrier_subresources),
          },
      .has_map = has_map,
      .resource_identity = sparse_resource_identity,
      .gpu_resource_id = sparse_gpu_resource_id,
      .mapping_generation = sparse_mapping_generation,
      .operation_count = sparse_operation_count,
      .map_count = sparse_map_count,
      .unmap_count = sparse_unmap_count,
  };
  plan.perf_delta = perf_delta;
  plan.has_barrier_subresources = barrier_subresource_count != 0;
  return true;
}

bool
BuildSparseTileMappingCopyWork(
    const void *queue_identity, IMTLD3D12Device *parent_device,
    ID3D12Resource *dst_resource,
    const D3D12_TILED_RESOURCE_COORDINATE *dst_start,
    ID3D12Resource *src_resource,
    const D3D12_TILED_RESOURCE_COORDINATE *src_start,
    const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags,
    SparseCopyQueueWork &work) {
  dxmt::apitrace::record_copy_tile_mappings(
      queue_identity, dst_resource, dst_start, src_resource, src_start,
      region_size, flags);
  if (!dst_resource || !src_resource || !dst_start || !src_start) {
    return false;
  }
  Resource *dst = nullptr;
  Resource *src = nullptr;
  const ResourceTiling *dst_tiling = nullptr;
  std::vector<SparseTileCoordinate> src_coordinates;
  std::vector<SparseTileCoordinate> dst_coordinates;
  if (!PrepareSparseTileCopyRegions(dst_resource, *dst_start, src_resource,
                                    *src_start, region_size, flags,
                                    parent_device, dst, src, dst_tiling,
                                    src_coordinates, dst_coordinates))
    return false;
  std::unordered_map<ID3D12Heap *,
                     std::vector<WMTSparseTextureMappingOperation>> map_ops;
  std::vector<WMTSparseTextureMappingOperation> unmap_ops;
  for (size_t i = 0; i < src_coordinates.size(); ++i) {
    ResourceTileMapping mapping = {};
    if (!GetSparseTileMapping(*src, src_coordinates[i], mapping))
      return false;
    const auto op = MakeSparseTileCopyOperation(*dst_tiling,
                                                dst_coordinates[i], mapping);
    if (op.mode == WMTSparseTextureMappingModeMap)
      map_ops[mapping.heap.ptr()].push_back(op);
    else
      unmap_ops.push_back(op);
  }

  if (!dst->UsesPlacementSparse()) {
    if (!unmap_ops.empty())
      ApplySparseTileMappingOpsToResource(*dst, *dst_tiling, nullptr,
                                          unmap_ops);
    for (const auto &entry : map_ops)
      ApplySparseTileMappingOpsToResource(*dst, *dst_tiling, entry.first,
                                          entry.second);
    return false;
  }

  if (!map_ops.empty() && !dst->EnsureTextureAllocation("CopyTileMappings")) {
    WARN("D3D12CommandQueue: TODO CopyTileMappings failed to materialize destination"
         " dst=", dst_resource,
         " src=", src_resource);
    return false;
  }
  std::unordered_map<ID3D12Heap *, WMT::Heap> placement_heaps;
  if (!ResolveSparseCopyPlacementHeaps(map_ops, dst_resource,
                                       placement_heaps))
    return false;
  if (!unmap_ops.empty())
    ApplySparseTileMappingOpsToResource(*dst, *dst_tiling, nullptr,
                                        unmap_ops);
  for (const auto &entry : map_ops)
    ApplySparseTileMappingOpsToResource(*dst, *dst_tiling, entry.first,
                                        entry.second);
  if (map_ops.empty() && !dst->GetTextureAllocation())
    return false;

  auto texture = dst->GetTextureAllocation()->texture();
  std::vector<SparseMappingGroup> sparse_groups;
  sparse_groups.reserve(map_ops.size() + (unmap_ops.empty() ? 0 : 1));

  if (!unmap_ops.empty())
    sparse_groups.push_back(MakeSparseMappingGroup(
        queue_identity, dst_resource, *dst, texture, nullptr, {},
        std::move(unmap_ops)));
  for (auto &entry : map_ops)
    sparse_groups.push_back(MakeSparseMappingGroup(
        queue_identity, dst_resource, *dst, texture, entry.first,
        placement_heaps.at(entry.first), std::move(entry.second)));
  if (sparse_groups.empty())
    return false;
  work.groups = std::move(sparse_groups);
  return true;
}

} // namespace dxmt::d3d12
