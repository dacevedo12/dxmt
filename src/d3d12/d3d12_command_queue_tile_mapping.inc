// Sparse tile mapping and tile copy entry points.
// Included inside class CommandQueueImpl; not a standalone translation unit.
//
// All of the validation, tile expansion and payload construction lives in
// d3d12_sparse_mapping_submission.cpp. What is left here is the part that
// genuinely needs the queue: turning the built payload into a PendingOperation
// and ordering it against the rest of the submission stream.
public:
  void UpdateTileMappingsImpl(ID3D12Resource *resource, UINT region_count,
                              const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
                                const D3D12_TILE_REGION_SIZE *region_sizes,
                                ID3D12Heap *heap, UINT range_count,
                                const D3D12_TILE_RANGE_FLAGS *range_flags,
                              const UINT *heap_range_offsets,
                              const UINT *range_tile_counts,
                              D3D12_TILE_MAPPING_FLAGS flags) {
    SparseTileMappingUpdatePlan plan;
    if (!BuildSparseTileMappingUpdatePlan(
            this, device_.ptr(), resource, region_count,
            region_start_coordinates, region_sizes, heap, range_count,
            range_flags, heap_range_offsets, range_tile_counts, flags, plan))
      return;
    PendingOperation op(QueueWorkSubmission{std::move(plan.work)});
    EnqueuePendingOperation(std::move(op), nullptr);
    dxmt::perf::recordTileMapping(
        plan.perf_delta.standard_ops, plan.perf_delta.packed_ops,
        plan.perf_delta.map_ops, plan.perf_delta.unmap_ops, 0, 0,
        plan.has_barrier_subresources ? 1 : 0);
  }

  void CopyTileMappingsImpl(ID3D12Resource *dst_resource,
                            const D3D12_TILED_RESOURCE_COORDINATE *dst_start,
                            ID3D12Resource *src_resource,
                            const D3D12_TILED_RESOURCE_COORDINATE *src_start,
                            const D3D12_TILE_REGION_SIZE *region_size,
                            D3D12_TILE_MAPPING_FLAGS flags) {
    SparseCopyQueueWork work;
    if (!BuildSparseTileMappingCopyWork(this, device_.ptr(), dst_resource,
                                        dst_start, src_resource, src_start,
                                        region_size, flags, work))
      return;
    PendingOperation pending(QueueWorkSubmission{std::move(work)});
    EnqueuePendingOperation(std::move(pending), nullptr);
  }

