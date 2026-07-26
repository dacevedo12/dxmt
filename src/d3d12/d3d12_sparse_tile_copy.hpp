#pragma once

#include "d3d12_heap.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_tile_mapping.hpp"
#include "winemetal.h"

#include <cstdint>
#include <d3d12.h>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace dxmt::d3d12 {

// CopyTileMappings can only replay a source mapping into the destination when
// both resources agree on the tile geometry and on how many mips stay
// standard (i.e. individually mappable) rather than packed.
[[nodiscard]] bool
SparseTilingCompatibleForCopy(const ResourceTiling &dst,
                              const ResourceTiling &src) noexcept;

// Builds the single-tile mapping operation that reproduces `mapping` at the
// destination tile `coordinate`. Packed tiles collapse to the first standard
// mip level at origin, which is how the packed mip tail is addressed.
[[nodiscard]] WMTSparseTextureMappingOperation
MakeSparseTileCopyOperation(const ResourceTiling &dst_tiling,
                            const SparseTileCoordinate &coordinate,
                            const ResourceTileMapping &mapping) noexcept;

// True when at least one operation actually binds heap memory, meaning the
// destination texture has to be materialized before the batch is applied.
[[nodiscard]] bool AnySparseTileMappingOpMaps(
    std::span<const WMTSparseTextureMappingOperation> operations) noexcept;

// Number of whole D3D12 tiles a heap of `heap_size_in_bytes` can back.
[[nodiscard]] UINT64 SparseHeapTileCapacity(UINT64 heap_size_in_bytes) noexcept;

// Heap tile index of the first map operation that falls outside a heap holding
// `heap_tile_count` tiles, or std::nullopt when every operation fits.
[[nodiscard]] std::optional<UINT64> FindSparseHeapTileOutOfRange(
    std::span<const WMTSparseTextureMappingOperation> operations,
    UINT64 heap_tile_count) noexcept;

// Saturating narrowing used by the sparse queue-work counters.
[[nodiscard]] uint32_t ClampSparseCountToUint32(uint64_t value) noexcept;

// Prepares an UpdateTileMappings batch that binds heap memory: materializes
// the reserved texture and resolves the Metal placement heap the tiles have to
// be placed into, writing it to `placement_heap`.
//
// Returns false when the batch cannot be applied. The reason has already been
// logged and the tile-mapping perf counters have already been closed out with
// `perf_delta` plus one failure, so the caller only has to bail out.
[[nodiscard]] bool ResolveSparseUpdatePlacementHeap(
    Resource &resource_object, Heap *heap_object, ID3D12Resource *resource,
    ID3D12Heap *heap, std::span<const WMTSparseTextureMappingOperation> ops,
    const D3D12TileMappingDelta &perf_delta, WMT::Heap &placement_heap);

// Resolves and validates the reserved-resource pair a CopyTileMappings call
// names against `parent_device`, then expands the copied region into the
// matching source and destination logical tile lists.
//
// Returns false, after logging the reason, when the call has to be dropped.
// On success `dst`, `src` and `dst_tiling` are non-null and the two coordinate
// lists have the same length.
[[nodiscard]] bool PrepareSparseTileCopyRegions(
    ID3D12Resource *dst_resource,
    const D3D12_TILED_RESOURCE_COORDINATE &dst_start,
    ID3D12Resource *src_resource,
    const D3D12_TILED_RESOURCE_COORDINATE &src_start,
    const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags,
    IMTLD3D12Device *parent_device, Resource *&dst, Resource *&src,
    const ResourceTiling *&dst_tiling,
    std::vector<SparseTileCoordinate> &src_coordinates,
    std::vector<SparseTileCoordinate> &dst_coordinates);

// Resolves the Metal placement heap of every D3D12 heap a CopyTileMappings
// batch maps from. Returns false, after logging, when one of them has none,
// which drops the whole copy.
[[nodiscard]] bool ResolveSparseCopyPlacementHeaps(
    const std::unordered_map<ID3D12Heap *,
                             std::vector<WMTSparseTextureMappingOperation>>
        &map_ops,
    ID3D12Resource *dst_resource,
    std::unordered_map<ID3D12Heap *, WMT::Heap> &placement_heaps);

} // namespace dxmt::d3d12
