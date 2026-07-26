#pragma once

// CopyTiles() planning: direction-flag validation plus the per-tile copy lists
// for reserved buffers and reserved textures.
//
// Split out of CommandQueueImpl (d3d12_command_queue_copy_clear.inc). Tile
// coordinates, tile counts and heap tile indices all come straight from the
// application, so the range checks below are the only thing between a bad
// CopyTiles() and an out-of-range Metal blit. Keeping them in their own
// translation unit is what lets the static analyzer solve them.

#include "d3d12_command_list.hpp"
#include "d3d12_device.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_format.hpp"

#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Exactly one direction flag has to be set; NO_HAZARD is the only other flag
// the replay understands.
struct CopyTilesDirection {
  bool valid = false;
  bool buffer_to_tiled = false;
};

/** Rejects unsupported and ambiguous flag combinations, logging the reason. */
[[nodiscard]] CopyTilesDirection
ResolveCopyTilesDirection(D3D12_TILE_COPY_FLAGS flags);

// One tile-sized buffer-to-buffer copy against a reserved buffer resource.
struct BufferTileCopy {
  Rc<Buffer> tiled_buffer;
  UINT64 tiled_offset = 0;
  UINT64 linear_offset = 0;
  UINT64 byte_count = 0;
};

/** Walks the requested tile region of a reserved *buffer* and emits one clamped
 *  copy per tile. `device` is the queue's device: tiles backed by a heap that
 *  belongs to another device are rejected rather than copied. Returns false
 *  (having logged) when any tile falls outside its backing allocation, in which
 *  case `copies` must be discarded. */
[[nodiscard]] bool PlanBufferTileCopies(IMTLD3D12Device *device,
                                        const CopyTilesRecord &record,
                                        Resource &tiled, Resource &linear,
                                        std::vector<BufferTileCopy> &copies);

// One tile-sized buffer<->texture copy against a reserved texture resource.
struct TextureTileCopy {
  UINT64 buffer_offset = 0;
  UINT level = 0;
  UINT slice = 0;
  WMTOrigin origin = {};
  WMTSize size = {};
  UINT row_pitch = 0;
  UINT image_pitch = 0;
};

/** Walks the requested tile region of a reserved *texture* and emits one copy
 *  per tile, clamped to the subresource extent. Returns false (having logged)
 *  when the region is invalid, contains a packed/planar tile, or would step
 *  outside `record.buffer` on the linear side, in which case `ops` must be
 *  discarded. An empty `ops` on success means the region degraded to nothing
 *  and no blit should be encoded. */
[[nodiscard]] bool PlanTextureTileCopies(Resource &tiled,
                                         const ResourceTiling &tiling,
                                         const CopyTilesRecord &record,
                                         UINT64 buffer_heap_offset,
                                         const MTL_DXGI_FORMAT_DESC &format,
                                         std::vector<TextureTileCopy> &ops);

} // namespace dxmt::d3d12
