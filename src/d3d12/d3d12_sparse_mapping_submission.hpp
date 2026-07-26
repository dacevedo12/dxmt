#pragma once

// API-thread half of UpdateTileMappings / CopyTileMappings: validation, tile
// expansion, immediate application to the resource's mapping table, and
// construction of the queue-work payload the replay thread will consume.
//
// This is everything the two entry points did apart from actually enqueueing.
// None of it reads the command queue -- the only queue-derived inputs are the
// owning device (used for the same-device check) and the queue address, which
// apitrace records as an opaque identity -- so it hoists out of the queue TU
// whole and can be analyzed on its own.

#include "d3d12_queue_work_types.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_tile_mapping.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

/**
 * A validated UpdateTileMappings batch that has to reach the replay thread,
 * together with the perf counters the caller still owes once the payload is
 * enqueued.
 */
struct SparseTileMappingUpdatePlan final {
  SparseUpdateQueueWork work;
  D3D12TileMappingDelta perf_delta;
  bool has_barrier_subresources = false;
};

/**
 * Runs an UpdateTileMappings call up to the point of submission and fills
 * `plan`.
 *
 * Returns false when nothing has to be submitted -- an invalid or dropped
 * call, a fully-backed resource that was updated in place, or an unmap-only
 * batch against a resource with no texture allocation. Every such path has
 * already logged its reason and closed out its perf counters, so the caller
 * only has to return. Returns true when `plan` must be enqueued, after which
 * the caller records `plan.perf_delta`.
 */
[[nodiscard]] bool BuildSparseTileMappingUpdatePlan(
    const void *queue_identity, IMTLD3D12Device *parent_device,
    ID3D12Resource *resource, UINT region_count,
    const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
    const D3D12_TILE_REGION_SIZE *region_sizes, ID3D12Heap *heap,
    UINT range_count, const D3D12_TILE_RANGE_FLAGS *range_flags,
    const UINT *heap_range_offsets, const UINT *range_tile_counts,
    D3D12_TILE_MAPPING_FLAGS flags, SparseTileMappingUpdatePlan &plan);

/**
 * Runs a CopyTileMappings call up to the point of submission and fills `work`.
 *
 * Returns false, after logging any reason, when the copy was dropped or was
 * fully applied in place; true when `work` holds at least one group and must
 * be enqueued.
 */
[[nodiscard]] bool BuildSparseTileMappingCopyWork(
    const void *queue_identity, IMTLD3D12Device *parent_device,
    ID3D12Resource *dst_resource,
    const D3D12_TILED_RESOURCE_COORDINATE *dst_start,
    ID3D12Resource *src_resource,
    const D3D12_TILED_RESOURCE_COORDINATE *src_start,
    const D3D12_TILE_REGION_SIZE *region_size, D3D12_TILE_MAPPING_FLAGS flags,
    SparseCopyQueueWork &work);

} // namespace dxmt::d3d12
