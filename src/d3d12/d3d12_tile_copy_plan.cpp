#include "d3d12_tile_copy_plan.hpp"

#include "d3d12_copy_footprint.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "d3d12_tile_mapping.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <utility>

namespace dxmt::d3d12 {

CopyTilesDirection
ResolveCopyTilesDirection(D3D12_TILE_COPY_FLAGS flags) {
  constexpr UINT kDirectionFlags =
      static_cast<UINT>(D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) |
      static_cast<UINT>(D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
  constexpr UINT kAllowedFlags =
      kDirectionFlags |
      static_cast<UINT>(D3D12_TILE_COPY_FLAG_NO_HAZARD);
  if (static_cast<UINT>(flags) & ~kAllowedFlags) {
    WARN("D3D12CommandQueue: TODO CopyTiles unsupported flags flags=", flags);
    return {};
  }
  const bool buffer_to_texture =
      flags & D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE;
  const bool texture_to_buffer =
      flags & D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER;
  if (buffer_to_texture == texture_to_buffer) {
    WARN("D3D12CommandQueue: TODO CopyTiles requires exactly one direction flag"
         " flags=", flags);
    return {};
  }
  return {true, buffer_to_texture};
}

bool
PlanBufferTileCopies(IMTLD3D12Device *device, const CopyTilesRecord &record,
                     Resource &tiled, Resource &linear,
                     std::vector<BufferTileCopy> &copies) {
  const auto *tiling = tiled.GetTiling();
  if (!tiling || record.start.Subresource >= tiling->subresources.size()) {
    WARN("D3D12CommandQueue: CopyTiles invalid reserved buffer tiling"
         " subresource=", record.start.Subresource);
    return false;
  }

  constexpr UINT64 tile_bytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  UINT linear_tile = 0;
  const bool valid = ForEachTileInRegion(
      *tiling, record.start, &record.size,
      [&](UINT subresource, UINT x, UINT y, UINT z) {
        if (subresource || y || z)
          return false;
        UINT64 tiled_offset = UINT64(x) * tile_bytes;
        Rc<Buffer> tiled_buffer = tiled.GetBuffer();
        ResourceTileMapping mapping = {};
        if (tiled.GetTileMapping(subresource, x, y, z, mapping) &&
            mapping.heap && mapping.heap_tile >= 0) {
          auto *heap_object = dynamic_cast<d3d12::Heap *>(mapping.heap.ptr());
          if (!heap_object || heap_object->GetParentDevice() != device ||
              UINT64(mapping.heap_tile) * tile_bytes >=
                  heap_object->GetHeapDesc().SizeInBytes)
            return false;
          tiled_buffer = heap_object->GetBuffer();
          tiled_offset = UINT64(mapping.heap_tile) * tile_bytes;
        }
        const auto linear_size = linear.GetResourceDesc().Width;
        // BufferStartOffsetInBytes is an unvalidated UINT64. Anchoring it
        // inside the buffer before the per-tile sum is what keeps that
        // addition from wrapping; the `linear_offset >= linear_size` test
        // below cannot catch a wrapped offset because it has already
        // become small. This lives inside the visitor so that a region
        // holding no tiles at all stays a plain success, exactly as before:
        // there is no access to bound.
        if (record.buffer_offset >= linear_size)
          return false;
        const UINT64 linear_offset =
            record.buffer_offset + UINT64(linear_tile++) * tile_bytes;
        const auto tiled_size = tiled_buffer ? tiled_buffer->length() : 0;
        if (tiled_offset >= tiled_size || linear_offset >= linear_size)
          return false;
        const UINT64 copy_size =
            std::min(tile_bytes, tiled_size - tiled_offset);
        if (copy_size > linear_size - linear_offset)
          return false;

        copies.push_back({std::move(tiled_buffer), tiled_offset, linear_offset,
                          copy_size});
        return true;
      });
  if (!valid) {
    WARN("D3D12CommandQueue: CopyTiles invalid reserved buffer region"
         " x=", record.start.X,
         " y=", record.start.Y,
         " z=", record.start.Z,
         " tiles=", record.size.NumTiles,
         " bufferOffset=", record.buffer_offset);
    return false;
  }
  return true;
}

bool
PlanTextureTileCopies(Resource &tiled, const ResourceTiling &tiling,
                      const CopyTilesRecord &record, UINT64 buffer_heap_offset,
                      const MTL_DXGI_FORMAT_DESC &format,
                      std::vector<TextureTileCopy> &ops) {
  const UINT64 tile_bytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;
  UINT tile_index = 0;
  const WMTSize subresource_size =
      GetSubresourceSize(tiled, record.start.Subresource, nullptr);
  // BufferStartOffsetInBytes and the tile count are application input, so the
  // linear side needs the same bound the reserved-buffer path applies: nothing
  // below may hand the blit encoder an offset or a length that runs past the
  // linear buffer. A missing buffer degrades to a zero-length bound rather
  // than an early return, so a region holding no tiles keeps returning an
  // empty plan instead of a failure.
  const UINT64 linear_size =
      record.buffer ? record.buffer->GetDesc().Width : 0;
  const bool ok = ForEachTileInRegion(
      tiling, record.start, &record.size,
      [&](UINT subresource, UINT x, UINT y, UINT z) {
        const auto &tile_subresource = tiling.subresources[subresource];
        if (tile_subresource.plane != 0 ||
            IsPackedTileSubresource(tile_subresource)) {
          WARN("D3D12CommandQueue: TODO CopyTiles encountered unsupported packed/planar tile"
               " subresource=", subresource,
               " plane=", tile_subresource.plane,
               " packed=", IsPackedTileSubresource(tile_subresource));
          return false;
        }
        // Same ordering as PlanBufferTileCopies above, and for the same
        // reason: anchoring the base offset inside the buffer first is what
        // keeps the per-tile sum from wrapping the 64-bit addition. Kept
        // inside the visitor so a tile-less region stays a plain success.
        if (record.buffer_offset >= linear_size)
          return false;
        const UINT64 linear_offset =
            record.buffer_offset + UINT64(tile_index++) * tile_bytes;
        const UINT64 buffer_offset = buffer_heap_offset + linear_offset;
        const UINT level = tile_subresource.mip_level;
        const UINT slice = tile_subresource.array_slice;
        const WMTSize current_subresource_size =
            subresource == record.start.Subresource
                ? subresource_size
                : GetSubresourceSize(tiled, subresource, nullptr);
        const WMTOrigin origin = TileCopyOrigin(tiling.tile_shape, x, y, z);
        const WMTSize size =
            TileCopySize(tiling.tile_shape, current_subresource_size, origin);
        if (!size.width || !size.height || !size.depth)
          return false;
        const auto row_layout = ComputeTileCopyRowLayout(format, size);
        // The blit reads/writes at most one image pitch per depth slice, and a
        // tile never spans more than D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES of
        // the linear layout.
        const UINT64 copy_size = std::min<UINT64>(
            tile_bytes, UINT64(row_layout.image_pitch) * size.depth);
        if (linear_offset >= linear_size ||
            copy_size > linear_size - linear_offset)
          return false;
        ops.push_back({buffer_offset, level, slice, origin, size,
                       row_layout.row_pitch, row_layout.image_pitch});
        return true;
      });
  if (!ok) {
    WARN("D3D12CommandQueue: TODO CopyTiles invalid region"
         " tiled=", record.tiled_resource.ptr(),
         " subresource=", record.start.Subresource,
         " x=", record.start.X,
         " y=", record.start.Y,
         " z=", record.start.Z,
         " tiles=", record.size.NumTiles,
         " bufferOffset=", record.buffer_offset,
         " bufferSize=", linear_size);
    return false;
  }
  return true;
}

} // namespace dxmt::d3d12
