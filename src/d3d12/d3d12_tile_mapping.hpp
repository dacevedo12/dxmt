#pragma once

#include "d3d12_resource.hpp"
#include <span>
#include <vector>

namespace dxmt::d3d12 {

struct TileRangeCursor {
  UINT index = 0;
  UINT consumed = 0;
  UINT total_consumed = 0;
};

class TileRangeView final {
public:
  static TileRangeView
  FromAbi(UINT count, const D3D12_TILE_RANGE_FLAGS *flags,
          const UINT *heap_offsets, const UINT *tile_counts) noexcept {
    return TileRangeView(
        count,
        flags ? std::span<const D3D12_TILE_RANGE_FLAGS>(flags, count)
              : std::span<const D3D12_TILE_RANGE_FLAGS>(),
        heap_offsets ? std::span<const UINT>(heap_offsets, count)
                     : std::span<const UINT>(),
        tile_counts ? std::span<const UINT>(tile_counts, count)
                    : std::span<const UINT>());
  }

  [[nodiscard]] UINT count() const noexcept {
    return count_;
  }

  [[nodiscard]] D3D12_TILE_RANGE_FLAGS flags(UINT index) const noexcept {
    return flags_.empty() ? D3D12_TILE_RANGE_FLAG_NONE : flags_[index];
  }

  [[nodiscard]] UINT heapOffset(UINT index, UINT implicit_offset) const noexcept {
    return heap_offsets_.empty() ? implicit_offset : heap_offsets_[index];
  }

  [[nodiscard]] UINT tileCount(UINT index) const noexcept {
    return tile_counts_.empty() ? 1 : tile_counts_[index];
  }

  [[nodiscard]] bool hasTileCounts() const noexcept {
    return !tile_counts_.empty();
  }

private:
  TileRangeView(UINT count, std::span<const D3D12_TILE_RANGE_FLAGS> flags,
                std::span<const UINT> heap_offsets,
                std::span<const UINT> tile_counts) noexcept
      : count_(count), flags_(flags), heap_offsets_(heap_offsets),
        tile_counts_(tile_counts) {}

  UINT count_ = 0;
  std::span<const D3D12_TILE_RANGE_FLAGS> flags_;
  std::span<const UINT> heap_offsets_;
  std::span<const UINT> tile_counts_;
};

struct SparseTileCoordinate {
  UINT subresource = 0;
  UINT x = 0;
  UINT y = 0;
  UINT z = 0;
  UINT tile_index = 0;
  bool packed = false;
};

struct SparseTileBarrierSubresource {
  UINT level = 0;
  UINT slice = 0;
};

struct D3D12TileMappingDelta {
  uint64_t standard_ops = 0;
  uint64_t packed_ops = 0;
  uint64_t map_ops = 0;
  uint64_t unmap_ops = 0;
};

[[nodiscard]] bool ShouldLogTileMappingDiag() noexcept;
[[nodiscard]] bool
IsPackedTileSubresource(const SubresourceTiling &subresource) noexcept;

[[nodiscard]] bool AppendSparseTileMappingRegion(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &start,
    const D3D12_TILE_REGION_SIZE *size, TileRangeCursor &cursor,
    const TileRangeView &ranges,
    std::vector<WMTSparseTextureMappingOperation> &operations);

[[nodiscard]] bool
TileRangeCursorConsumedAll(const TileRangeCursor &cursor,
                           const TileRangeView &ranges) noexcept;

[[nodiscard]] bool CollectLogicalTilesInRegion(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &start,
    const D3D12_TILE_REGION_SIZE *size,
    std::vector<SparseTileCoordinate> &coordinates);

[[nodiscard]] bool CollectTilesInRegion(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &start,
    const D3D12_TILE_REGION_SIZE *size,
    std::vector<D3D12_TILED_RESOURCE_COORDINATE> &coordinates);

template <typename Visitor>
[[nodiscard]] bool ForEachTileInRegion(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &start,
    const D3D12_TILE_REGION_SIZE *size, Visitor &&visitor) {
  std::vector<D3D12_TILED_RESOURCE_COORDINATE> coordinates;
  if (!CollectTilesInRegion(tiling, start, size, coordinates))
    return false;
  for (const auto &coordinate : coordinates)
    if (!visitor(coordinate.Subresource, coordinate.X, coordinate.Y,
                 coordinate.Z))
      return false;
  return true;
}

void ApplySparseTileMappingOpsToResource(
    Resource &resource, const ResourceTiling &tiling, ID3D12Heap *heap,
    std::span<const WMTSparseTextureMappingOperation> operations);

[[nodiscard]] bool
GetSparseTileMapping(Resource &resource, const SparseTileCoordinate &coordinate,
                     ResourceTileMapping &mapping);

void RecordTileMappingCall() noexcept;
void RecordTileMappingInvalid() noexcept;
void RecordTileMappingMetalFailure() noexcept;
void RecordTileMappingBarrier() noexcept;

[[nodiscard]] D3D12TileMappingDelta RecordTileMappingOps(
    const ResourceTiling &tiling,
    std::span<const WMTSparseTextureMappingOperation> operations) noexcept;

void LogTileMappingStats(const char *context);

[[nodiscard]] std::vector<SparseTileBarrierSubresource>
CollectSparseTileBarrierSubresources(
    std::span<const WMTSparseTextureMappingOperation> operations);

} // namespace dxmt::d3d12
