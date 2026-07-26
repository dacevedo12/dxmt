#include "d3d12_tile_mapping.hpp"

#include "log/log.hpp"
#include "util_env.hpp"
#include <algorithm>
#include <atomic>

namespace dxmt::d3d12 {
namespace {

struct D3D12TileMappingCounters {
  std::atomic<uint64_t> calls = {0};
  std::atomic<uint64_t> standard_ops = {0};
  std::atomic<uint64_t> packed_ops = {0};
  std::atomic<uint64_t> map_ops = {0};
  std::atomic<uint64_t> unmap_ops = {0};
  std::atomic<uint64_t> invalid = {0};
  std::atomic<uint64_t> metal_failures = {0};
  std::atomic<uint64_t> barriers = {0};
};

D3D12TileMappingCounters &TileMappingStats() noexcept {
  static D3D12TileMappingCounters counters;
  return counters;
}

bool TileMappingEnabled() {
  static const bool enabled = [] {
    const auto value = env::getEnvVar("DXMT_D3D12_TILE_MAPPING");
    return value != "0" && value != "false" && value != "no" &&
           value != "off";
  }();
  return enabled;
}

bool TileMappingStatsEnabled() {
  static const bool enabled = [] {
    const auto enabled_env = [](const char *name) {
      const auto value = env::getEnvVar(name);
      return value == "1" || value == "true" || value == "yes" ||
             value == "trace";
    };
    return enabled_env("DXMT_D3D12_TILE_MAPPING_STATS") ||
           enabled_env("DXMT_DIAG_D3D12_TILE_MAPPING");
  }();
  return enabled;
}

const char *TileRangeFlagName(D3D12_TILE_RANGE_FLAGS flags) noexcept {
  switch (flags) {
  case D3D12_TILE_RANGE_FLAG_NONE:
    return "NONE";
  case D3D12_TILE_RANGE_FLAG_NULL:
    return "NULL";
  case D3D12_TILE_RANGE_FLAG_SKIP:
    return "SKIP";
  case D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE:
    return "REUSE_SINGLE_TILE";
  default:
    return "UNKNOWN";
  }
}

bool ResolveSparseTileCoordinate(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &coordinate,
    SparseTileCoordinate &resolved) {
  if (coordinate.Subresource >= tiling.subresources.size())
    return false;

  const auto &subresource = tiling.subresources[coordinate.Subresource];
  if (IsPackedTileSubresource(subresource)) {
    if (!TileMappingEnabled() || coordinate.X || coordinate.Y ||
        coordinate.Z || !tiling.packed_mip_info.NumPackedMips ||
        subresource.packed_tile_index >= tiling.total_tile_count)
      return false;
    resolved = {
        .subresource = coordinate.Subresource,
        .tile_index = subresource.packed_tile_index,
        .packed = true,
    };
    return true;
  }

  if (coordinate.X >= subresource.width_in_tiles ||
      coordinate.Y >= subresource.height_in_tiles ||
      coordinate.Z >= subresource.depth_in_tiles)
    return false;

  resolved = {
      .subresource = coordinate.Subresource,
      .x = coordinate.X,
      .y = coordinate.Y,
      .z = coordinate.Z,
      .tile_index =
          subresource.start_tile_index +
          (coordinate.Z * subresource.height_in_tiles + coordinate.Y) *
              subresource.width_in_tiles +
          coordinate.X,
  };
  return resolved.tile_index < tiling.total_tile_count;
}

bool IsValidTileCoordinate(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &coordinate) noexcept {
  if (coordinate.Subresource >= tiling.subresources.size())
    return false;
  const auto &subresource = tiling.subresources[coordinate.Subresource];
  return !IsPackedTileSubresource(subresource) &&
         coordinate.X < subresource.width_in_tiles &&
         coordinate.Y < subresource.height_in_tiles &&
         coordinate.Z < subresource.depth_in_tiles;
}

bool AdvanceTileCoordinate(
    const ResourceTiling &tiling,
    D3D12_TILED_RESOURCE_COORDINATE &coordinate) noexcept {
  if (coordinate.Subresource >= tiling.subresources.size())
    return false;
  const auto &subresource = tiling.subresources[coordinate.Subresource];
  if (IsPackedTileSubresource(subresource))
    return false;
  if (++coordinate.X < subresource.width_in_tiles)
    return true;
  coordinate.X = 0;
  if (++coordinate.Y < subresource.height_in_tiles)
    return true;
  coordinate.Y = 0;
  if (++coordinate.Z < subresource.depth_in_tiles)
    return true;
  coordinate.Z = 0;
  ++coordinate.Subresource;
  while (coordinate.Subresource < tiling.subresources.size()) {
    const auto &next = tiling.subresources[coordinate.Subresource];
    if (!IsPackedTileSubresource(next) ||
        next.mip_level == tiling.packed_mip_info.NumStandardMips)
      return true;
    ++coordinate.Subresource;
  }
  return false;
}

bool AdvanceLogicalTileCoordinate(
    const ResourceTiling &tiling,
    D3D12_TILED_RESOURCE_COORDINATE &coordinate) {
  SparseTileCoordinate resolved = {};
  if (!ResolveSparseTileCoordinate(tiling, coordinate, resolved))
    return false;
  const auto &subresource = tiling.subresources[coordinate.Subresource];
  if (!resolved.packed)
    return AdvanceTileCoordinate(tiling, coordinate);

  const UINT packed_tile_index = subresource.packed_tile_index;
  do {
    ++coordinate.Subresource;
  } while (
      coordinate.Subresource < tiling.subresources.size() &&
      IsPackedTileSubresource(tiling.subresources[coordinate.Subresource]) &&
      tiling.subresources[coordinate.Subresource].packed_tile_index ==
          packed_tile_index);
  coordinate.X = 0;
  coordinate.Y = 0;
  coordinate.Z = 0;
  return coordinate.Subresource < tiling.subresources.size();
}

bool ConsumeTileRange(TileRangeCursor &cursor, const TileRangeView &ranges,
                      D3D12_TILE_RANGE_FLAGS &flags, UINT &heap_tile) {
  while (cursor.index < ranges.count()) {
    flags = ranges.flags(cursor.index);
    const UINT count = ranges.tileCount(cursor.index);
    const UINT base = ranges.heapOffset(
        cursor.index, cursor.total_consumed - cursor.consumed);
    if (cursor.consumed < count) {
      heap_tile = (flags & D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE)
                      ? base
                      : base + cursor.consumed;
      ++cursor.consumed;
      ++cursor.total_consumed;
      if (cursor.consumed == count) {
        ++cursor.index;
        cursor.consumed = 0;
      }
      return true;
    }
    ++cursor.index;
    cursor.consumed = 0;
  }
  return false;
}

bool AppendSparseTileMappingOp(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &coordinate,
    D3D12_TILE_RANGE_FLAGS range_flags, UINT heap_tile,
    std::vector<WMTSparseTextureMappingOperation> &operations) {
  SparseTileCoordinate resolved = {};
  if (!ResolveSparseTileCoordinate(tiling, coordinate, resolved))
    return false;
  const auto &subresource = tiling.subresources[resolved.subresource];
  operations.push_back({
      .mode = (range_flags & D3D12_TILE_RANGE_FLAG_NULL)
                  ? WMTSparseTextureMappingModeUnmap
                  : WMTSparseTextureMappingModeMap,
      .level = resolved.packed ? tiling.packed_mip_info.NumStandardMips
                               : subresource.mip_level,
      .slice = subresource.array_slice,
      .x = resolved.packed ? 0u : resolved.x,
      .y = resolved.packed ? 0u : resolved.y,
      .z = resolved.packed ? 0u : resolved.z,
      .width = 1,
      .height = 1,
      .depth = 1,
      .heap_offset =
          UINT64(heap_tile) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,
  });
  return true;
}

} // namespace

bool ShouldLogTileMappingDiag() noexcept {
  static std::atomic<uint32_t> count = 0;
  return count.fetch_add(1, std::memory_order_relaxed) < 16;
}

bool IsPackedTileSubresource(
    const SubresourceTiling &subresource) noexcept {
  return subresource.start_tile_index == D3D12_PACKED_TILE ||
         !subresource.width_in_tiles || !subresource.height_in_tiles ||
         !subresource.depth_in_tiles;
}

bool AppendSparseTileMappingRegion(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &start,
    const D3D12_TILE_REGION_SIZE *size, TileRangeCursor &cursor,
    const TileRangeView &ranges,
    std::vector<WMTSparseTextureMappingOperation> &operations) {
  SparseTileCoordinate resolved = {};
  if (!ResolveSparseTileCoordinate(tiling, start, resolved))
    return false;

  const auto append_one =
      [&tiling, &cursor, &ranges,
       &operations](const D3D12_TILED_RESOURCE_COORDINATE &coordinate) {
        D3D12_TILE_RANGE_FLAGS flags = D3D12_TILE_RANGE_FLAG_NONE;
        UINT heap_tile = 0;
        if (!ConsumeTileRange(cursor, ranges, flags, heap_tile))
          return false;
        if (flags == D3D12_TILE_RANGE_FLAG_SKIP)
          return true;
        constexpr UINT supported_flags =
            D3D12_TILE_RANGE_FLAG_NULL | D3D12_TILE_RANGE_FLAG_SKIP |
            D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
        if ((flags & ~supported_flags) ||
            ((flags & D3D12_TILE_RANGE_FLAG_SKIP) &&
             (flags & ~D3D12_TILE_RANGE_FLAG_SKIP)) ||
            ((flags & D3D12_TILE_RANGE_FLAG_NULL) &&
             (flags & D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE))) {
          if (ShouldLogTileMappingDiag())
            WARN("D3D12CommandQueue: TODO unsupported tile range flag"
                 " flag=", flags, " name=", TileRangeFlagName(flags));
          return false;
        }
        return AppendSparseTileMappingOp(tiling, coordinate, flags, heap_tile,
                                         operations);
      };

  if (!size)
    return append_one(start);

  if (size->UseBox) {
    const auto &subresource = tiling.subresources[start.Subresource];
    if (IsPackedTileSubresource(subresource) ||
        start.X > subresource.width_in_tiles ||
        size->Width > subresource.width_in_tiles - start.X ||
        start.Y > subresource.height_in_tiles ||
        size->Height > subresource.height_in_tiles - start.Y ||
        start.Z > subresource.depth_in_tiles ||
        size->Depth > subresource.depth_in_tiles - start.Z)
      return false;
    for (UINT z = 0; z < size->Depth; ++z)
      for (UINT y = 0; y < size->Height; ++y)
        for (UINT x = 0; x < size->Width; ++x) {
          auto coordinate = start;
          coordinate.X += x;
          coordinate.Y += y;
          coordinate.Z += z;
          if (!append_one(coordinate))
            return false;
        }
    return true;
  }

  auto coordinate = start;
  for (UINT i = 0; i < size->NumTiles; ++i) {
    if (!ResolveSparseTileCoordinate(tiling, coordinate, resolved) ||
        !append_one(coordinate))
      return false;
    if (i + 1 < size->NumTiles &&
        !AdvanceLogicalTileCoordinate(tiling, coordinate))
      return false;
  }
  return true;
}

bool TileRangeCursorConsumedAll(
    const TileRangeCursor &cursor, const TileRangeView &ranges) noexcept {
  if (cursor.index >= ranges.count())
    return true;
  if (!ranges.hasTileCounts())
    return cursor.consumed == 0 && cursor.index == ranges.count();
  if (cursor.consumed)
    return false;
  for (UINT i = cursor.index; i < ranges.count(); ++i)
    if (ranges.tileCount(i))
      return false;
  return true;
}

bool CollectLogicalTilesInRegion(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &start,
    const D3D12_TILE_REGION_SIZE *size,
    std::vector<SparseTileCoordinate> &coordinates) {
  SparseTileCoordinate resolved = {};
  if (!ResolveSparseTileCoordinate(tiling, start, resolved))
    return false;

  if (!size) {
    coordinates.push_back(resolved);
    return true;
  }

  if (size->UseBox) {
    const auto &subresource = tiling.subresources[start.Subresource];
    if (IsPackedTileSubresource(subresource) ||
        start.X > subresource.width_in_tiles ||
        size->Width > subresource.width_in_tiles - start.X ||
        start.Y > subresource.height_in_tiles ||
        size->Height > subresource.height_in_tiles - start.Y ||
        start.Z > subresource.depth_in_tiles ||
        size->Depth > subresource.depth_in_tiles - start.Z)
      return false;
    for (UINT z = 0; z < size->Depth; ++z)
      for (UINT y = 0; y < size->Height; ++y)
        for (UINT x = 0; x < size->Width; ++x) {
          auto coordinate = start;
          coordinate.X += x;
          coordinate.Y += y;
          coordinate.Z += z;
          if (!ResolveSparseTileCoordinate(tiling, coordinate, resolved))
            return false;
          coordinates.push_back(resolved);
        }
    return true;
  }

  auto coordinate = start;
  for (UINT i = 0; i < size->NumTiles; ++i) {
    if (!ResolveSparseTileCoordinate(tiling, coordinate, resolved))
      return false;
    coordinates.push_back(resolved);
    if (i + 1 < size->NumTiles &&
        !AdvanceLogicalTileCoordinate(tiling, coordinate))
      return false;
  }
  return true;
}

bool CollectTilesInRegion(
    const ResourceTiling &tiling,
    const D3D12_TILED_RESOURCE_COORDINATE &start,
    const D3D12_TILE_REGION_SIZE *size,
    std::vector<D3D12_TILED_RESOURCE_COORDINATE> &coordinates) {
  if (!IsValidTileCoordinate(tiling, start))
    return false;
  if (!size) {
    coordinates.push_back(start);
    return true;
  }

  if (size->UseBox) {
    const auto &subresource = tiling.subresources[start.Subresource];
    if (start.X > subresource.width_in_tiles ||
        size->Width > subresource.width_in_tiles - start.X ||
        start.Y > subresource.height_in_tiles ||
        size->Height > subresource.height_in_tiles - start.Y ||
        start.Z > subresource.depth_in_tiles ||
        size->Depth > subresource.depth_in_tiles - start.Z)
      return false;
    for (UINT z = 0; z < size->Depth; ++z)
      for (UINT y = 0; y < size->Height; ++y)
        for (UINT x = 0; x < size->Width; ++x) {
          auto coordinate = start;
          coordinate.X += x;
          coordinate.Y += y;
          coordinate.Z += z;
          coordinates.push_back(coordinate);
        }
    return true;
  }

  auto coordinate = start;
  for (UINT i = 0; i < size->NumTiles; ++i) {
    if (!IsValidTileCoordinate(tiling, coordinate))
      return false;
    coordinates.push_back(coordinate);
    if (i + 1 < size->NumTiles &&
        !AdvanceTileCoordinate(tiling, coordinate))
      return false;
  }
  return true;
}

void ApplySparseTileMappingOpsToResource(
    Resource &resource, const ResourceTiling &tiling, ID3D12Heap *heap,
    std::span<const WMTSparseTextureMappingOperation> operations) {
  for (const auto &operation : operations) {
    for (UINT subresource = 0; subresource < tiling.subresources.size();
         ++subresource) {
      const auto &tile = tiling.subresources[subresource];
      if (tile.mip_level != operation.level ||
          tile.array_slice != operation.slice)
        continue;
      if (IsPackedTileSubresource(tile)) {
        if (operation.x || operation.y || operation.z ||
            !resource.UpdateTileMappingByIndex(
                tile.packed_tile_index, heap,
                operation.mode == WMTSparseTextureMappingModeMap,
                operation.heap_offset /
                    D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES))
          continue;
        break;
      }
      if (operation.x >= tile.width_in_tiles ||
          operation.y >= tile.height_in_tiles ||
          operation.z >= tile.depth_in_tiles)
        continue;
      resource.UpdateTileMapping(
          subresource, operation.x, operation.y, operation.z, heap,
          operation.mode == WMTSparseTextureMappingModeMap,
          operation.heap_offset / D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES);
      break;
    }
  }
}

bool GetSparseTileMapping(Resource &resource,
                          const SparseTileCoordinate &coordinate,
                          ResourceTileMapping &mapping) {
  if (coordinate.packed)
    return resource.GetTileMappingByIndex(coordinate.tile_index, mapping);
  return resource.GetTileMapping(coordinate.subresource, coordinate.x,
                                 coordinate.y, coordinate.z, mapping);
}

void RecordTileMappingCall() noexcept {
  TileMappingStats().calls.fetch_add(1, std::memory_order_relaxed);
}

void RecordTileMappingInvalid() noexcept {
  TileMappingStats().invalid.fetch_add(1, std::memory_order_relaxed);
}

void RecordTileMappingMetalFailure() noexcept {
  TileMappingStats().metal_failures.fetch_add(1, std::memory_order_relaxed);
}

void RecordTileMappingBarrier() noexcept {
  TileMappingStats().barriers.fetch_add(1, std::memory_order_relaxed);
}

D3D12TileMappingDelta RecordTileMappingOps(
    const ResourceTiling &tiling,
    std::span<const WMTSparseTextureMappingOperation> operations) noexcept {
  D3D12TileMappingDelta delta;
  auto &stats = TileMappingStats();
  for (const auto &operation : operations) {
    bool packed = false;
    for (const auto &subresource : tiling.subresources) {
      if (subresource.mip_level == operation.level &&
          subresource.array_slice == operation.slice) {
        packed = IsPackedTileSubresource(subresource);
        break;
      }
    }
    if (packed) {
      stats.packed_ops.fetch_add(1, std::memory_order_relaxed);
      ++delta.packed_ops;
    } else {
      stats.standard_ops.fetch_add(1, std::memory_order_relaxed);
      ++delta.standard_ops;
    }
    if (operation.mode == WMTSparseTextureMappingModeMap) {
      stats.map_ops.fetch_add(1, std::memory_order_relaxed);
      ++delta.map_ops;
    } else {
      stats.unmap_ops.fetch_add(1, std::memory_order_relaxed);
      ++delta.unmap_ops;
    }
  }
  return delta;
}

void LogTileMappingStats(const char *context) {
  if (!TileMappingStatsEnabled())
    return;
  static std::atomic<uint32_t> log_count = 0;
  if (log_count.fetch_add(1, std::memory_order_relaxed) >= 128)
    return;
  auto &stats = TileMappingStats();
  WARN_FILE_ONLY("D3D12 tile mapping stats"
                 " context=", context ? context : "",
                 " calls=", stats.calls.load(std::memory_order_relaxed),
                 " standardOps=",
                 stats.standard_ops.load(std::memory_order_relaxed),
                 " packedOps=",
                 stats.packed_ops.load(std::memory_order_relaxed),
                 " mapOps=", stats.map_ops.load(std::memory_order_relaxed),
                 " unmapOps=", stats.unmap_ops.load(std::memory_order_relaxed),
                 " invalid=", stats.invalid.load(std::memory_order_relaxed),
                 " metalFailures=",
                 stats.metal_failures.load(std::memory_order_relaxed),
                 " barriers=", stats.barriers.load(std::memory_order_relaxed));
}

std::vector<SparseTileBarrierSubresource>
CollectSparseTileBarrierSubresources(
    std::span<const WMTSparseTextureMappingOperation> operations) {
  std::vector<SparseTileBarrierSubresource> subresources;
  subresources.reserve(operations.size());
  for (const auto &operation : operations) {
    const SparseTileBarrierSubresource entry = {
        operation.level, operation.slice};
    const auto exists = std::find_if(
        subresources.begin(), subresources.end(),
        [entry](const SparseTileBarrierSubresource &existing) {
          return existing.level == entry.level &&
                 existing.slice == entry.slice;
        });
    if (exists == subresources.end())
      subresources.push_back(entry);
  }
  return subresources;
}

} // namespace dxmt::d3d12
