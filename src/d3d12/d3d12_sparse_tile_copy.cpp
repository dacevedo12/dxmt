#include "d3d12_sparse_tile_copy.hpp"

#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <limits>

namespace dxmt::d3d12 {

bool SparseTilingCompatibleForCopy(const ResourceTiling &dst,
                                   const ResourceTiling &src) noexcept {
  return dst.tile_shape.WidthInTexels == src.tile_shape.WidthInTexels &&
         dst.tile_shape.HeightInTexels == src.tile_shape.HeightInTexels &&
         dst.tile_shape.DepthInTexels == src.tile_shape.DepthInTexels &&
         dst.packed_mip_info.NumStandardMips ==
             src.packed_mip_info.NumStandardMips;
}

WMTSparseTextureMappingOperation
MakeSparseTileCopyOperation(const ResourceTiling &dst_tiling,
                            const SparseTileCoordinate &coordinate,
                            const ResourceTileMapping &mapping) noexcept {
  const auto &subresource = dst_tiling.subresources[coordinate.subresource];
  WMTSparseTextureMappingOperation op = {};
  op.mode = mapping.heap && mapping.heap_tile >= 0
                ? WMTSparseTextureMappingModeMap
                : WMTSparseTextureMappingModeUnmap;
  op.level = coordinate.packed ? dst_tiling.packed_mip_info.NumStandardMips
                               : subresource.mip_level;
  op.slice = subresource.array_slice;
  op.x = coordinate.packed ? 0 : coordinate.x;
  op.y = coordinate.packed ? 0 : coordinate.y;
  op.z = coordinate.packed ? 0 : coordinate.z;
  op.width = 1;
  op.height = 1;
  op.depth = 1;
  op.heap_offset = mapping.heap_tile >= 0
                       ? UINT64(mapping.heap_tile) *
                             D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES
                       : 0;
  return op;
}

bool AnySparseTileMappingOpMaps(
    std::span<const WMTSparseTextureMappingOperation> operations) noexcept {
  bool has_map = false;
  for (const auto &op : operations) {
    has_map |= op.mode == WMTSparseTextureMappingModeMap;
  }
  return has_map;
}

UINT64 SparseHeapTileCapacity(UINT64 heap_size_in_bytes) noexcept {
  return heap_size_in_bytes / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
}

std::optional<UINT64> FindSparseHeapTileOutOfRange(
    std::span<const WMTSparseTextureMappingOperation> operations,
    UINT64 heap_tile_count) noexcept {
  for (const auto &op : operations) {
    if (op.mode != WMTSparseTextureMappingModeMap)
      continue;
    const UINT64 tile =
        op.heap_offset / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
    if (tile >= heap_tile_count)
      return tile;
  }
  return std::nullopt;
}

uint32_t ClampSparseCountToUint32(uint64_t value) noexcept {
  return static_cast<uint32_t>(
      std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
}

bool ResolveSparseUpdatePlacementHeap(
    Resource &resource_object, Heap *heap_object, ID3D12Resource *resource,
    ID3D12Heap *heap, std::span<const WMTSparseTextureMappingOperation> ops,
    const D3D12TileMappingDelta &perf_delta, WMT::Heap &placement_heap) {
  if (!resource_object.EnsureTextureAllocation("UpdateTileMappings")) {
    if (ShouldLogTileMappingDiag()) {
      WARN("D3D12CommandQueue: TODO UpdateTileMappings failed to materialize reserved texture"
           " resource=", resource,
           " ops=", ops.size());
    }
    dxmt::perf::recordTileMapping(
        perf_delta.standard_ops, perf_delta.packed_ops,
        perf_delta.map_ops, perf_delta.unmap_ops, 0, 1, 0);
    return false;
  }
  if (!heap_object) {
    if (ShouldLogTileMappingDiag())
      WARN("D3D12CommandQueue: TODO UpdateTileMappings map requires D3D12 heap"
           " resource=", resource,
           " heap=", heap,
           " ops=", ops.size());
    dxmt::perf::recordTileMapping(
        perf_delta.standard_ops, perf_delta.packed_ops,
        perf_delta.map_ops, perf_delta.unmap_ops, 0, 1, 0);
    return false;
  }
  const auto &heap_desc = heap_object->GetHeapDesc();
  const UINT64 heap_tile_count = SparseHeapTileCapacity(heap_desc.SizeInBytes);
  if (const auto tile = FindSparseHeapTileOutOfRange(ops, heap_tile_count)) {
    WARN("D3D12CommandQueue: TODO UpdateTileMappings heap tile out of range"
         " resource=", resource,
         " heap=", heap,
         " tile=", *tile,
         " heapTiles=", heap_tile_count);
    dxmt::perf::recordTileMapping(
        perf_delta.standard_ops, perf_delta.packed_ops,
        perf_delta.map_ops, perf_delta.unmap_ops, 0, 1, 0);
    return false;
  }
  placement_heap = heap_object->GetPlacementHeap();
  if (!placement_heap) {
    if (ShouldLogTileMappingDiag())
      WARN("D3D12CommandQueue: TODO UpdateTileMappings failed to get placement heap"
           " resource=", resource,
           " heap=", heap,
           " ops=", ops.size());
    dxmt::perf::recordTileMapping(
        perf_delta.standard_ops, perf_delta.packed_ops,
        perf_delta.map_ops, perf_delta.unmap_ops, 0, 1, 0);
    return false;
  }
  return true;
}

bool PrepareSparseTileCopyRegions(
    ID3D12Resource *dst_resource,
    const D3D12_TILED_RESOURCE_COORDINATE &dst_start,
    ID3D12Resource *src_resource,
    const D3D12_TILED_RESOURCE_COORDINATE &src_start,
    const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags,
    IMTLD3D12Device *parent_device, Resource *&dst, Resource *&src,
    const ResourceTiling *&dst_tiling,
    std::vector<SparseTileCoordinate> &src_coordinates,
    std::vector<SparseTileCoordinate> &dst_coordinates) {
  if (flags && ShouldLogTileMappingDiag()) {
    WARN("D3D12CommandQueue: TODO CopyTileMappings flags are ignored"
         " flags=", flags,
         " dst=", dst_resource,
         " src=", src_resource);
  }

  dst = dynamic_cast<d3d12::Resource *>(dst_resource);
  src = dynamic_cast<d3d12::Resource *>(src_resource);
  dst_tiling = dst ? dst->GetTiling() : nullptr;
  const auto *src_tiling = src ? src->GetTiling() : nullptr;
  if (!dst || !src || dst->GetParentDevice() != parent_device ||
      src->GetParentDevice() != parent_device ||
      !dst->IsReserved() || !src->IsReserved() ||
      !dst_tiling || !src_tiling) {
    WARN("D3D12CommandQueue: CopyTileMappings requires local reserved resources"
         " dst=", dst_resource,
         " src=", src_resource,
         " flags=", flags);
    return false;
  }
  if (dst_start.Subresource >= dst_tiling->subresources.size() ||
      src_start.Subresource >= src_tiling->subresources.size()) {
    WARN("D3D12CommandQueue: CopyTileMappings subresource out of range"
         " dstSubresource=", dst_start.Subresource,
         " srcSubresource=", src_start.Subresource);
    return false;
  }
  if (!SparseTilingCompatibleForCopy(*dst_tiling, *src_tiling)) {
    WARN("D3D12CommandQueue: TODO CopyTileMappings incompatible tiling"
         " dst=", dst_resource,
         " src=", src_resource,
         " dstTile=", dst_tiling->tile_shape.WidthInTexels, "x",
                       dst_tiling->tile_shape.HeightInTexels, "x",
                       dst_tiling->tile_shape.DepthInTexels,
         " srcTile=", src_tiling->tile_shape.WidthInTexels, "x",
                       src_tiling->tile_shape.HeightInTexels, "x",
                       src_tiling->tile_shape.DepthInTexels,
         " dstStandardMips=", uint32_t(dst_tiling->packed_mip_info.NumStandardMips),
         " srcStandardMips=", uint32_t(src_tiling->packed_mip_info.NumStandardMips));
    return false;
  }

  const bool src_ok = CollectLogicalTilesInRegion(*src_tiling, src_start,
                                                  region_size, src_coordinates);
  const bool dst_ok = CollectLogicalTilesInRegion(*dst_tiling, dst_start,
                                                  region_size, dst_coordinates);
  if (!src_ok || !dst_ok ||
      src_coordinates.size() != dst_coordinates.size()) {
    WARN("D3D12CommandQueue: TODO CopyTileMappings invalid or incompatible region"
         " dst=", dst_resource,
         " src=", src_resource,
         " dstSubresource=", dst_start.Subresource,
         " srcSubresource=", src_start.Subresource,
         " useBox=", region_size ? region_size->UseBox : 0,
         " numTiles=", region_size ? region_size->NumTiles : 1);
    return false;
  }
  return true;
}

bool ResolveSparseCopyPlacementHeaps(
    const std::unordered_map<ID3D12Heap *,
                             std::vector<WMTSparseTextureMappingOperation>>
        &map_ops,
    ID3D12Resource *dst_resource,
    std::unordered_map<ID3D12Heap *, WMT::Heap> &placement_heaps) {
  for (const auto &entry : map_ops) {
    auto *heap_object = dynamic_cast<d3d12::Heap *>(entry.first);
    WMT::Heap placement_heap = {};
    if (!heap_object ||
        !(placement_heap = heap_object->GetPlacementHeap())) {
      WARN("D3D12CommandQueue: TODO CopyTileMappings failed to get placement heap"
           " heap=", entry.first,
           " dst=", dst_resource,
           " ops=", entry.second.size());
      return false;
    }
    placement_heaps.emplace(entry.first, placement_heap);
  }
  return true;
}

} // namespace dxmt::d3d12
