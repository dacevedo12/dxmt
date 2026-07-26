#pragma once

#include "dxmt_format.hpp"
#include "winemetal.h"

#include <cstdint>
#include <d3d12.h>
#include <optional>

namespace dxmt::d3d12 {

// One R32G32B32A32_UINT texel carries exactly one 16-byte block-compressed
// block, and that block covers a 4x4 texel footprint once decompressed.
inline constexpr uint64_t kBlockReinterpretBytesPerTexel = 16;
inline constexpr uint64_t kBlockReinterpretTexelsPerBlockAxis = 4;

// Block-compressed formats are laid out in 4x4 texel blocks.
inline constexpr UINT kBlockCompressedTexelsPerBlockAxis = 4;

// Staging layout for a raw block copy from an uncompressed R32G32B32A32_UINT
// source to a 16-byte block-compressed destination.
struct BlockReinterpretCopyLayout {
  UINT row_pitch = 0;
  UINT image_pitch = 0;
  UINT64 staging_length = 0;
  // Destination extent expressed in texels rather than blocks.
  WMTSize dst_size = {};
};

// Returns std::nullopt when the copy extent is empty or the derived layout
// does not fit the Metal blit encoder's 32-bit pitch fields.
[[nodiscard]] std::optional<BlockReinterpretCopyLayout>
ComputeBlockReinterpretCopyLayout(const WMTSize &size);

// Row geometry of a D3D12 placed subresource footprint.
struct PlacedFootprintRowLayout {
  UINT block_height = 1;
  UINT row_count = 1;
  UINT image_pitch = 0;
};

// Returns std::nullopt when the application-supplied footprint does not fit
// the Metal blit encoder's 32-bit pitch fields. RowPitch and Height come
// straight from D3D12_PLACED_SUBRESOURCE_FOOTPRINT, so the row count and the
// image pitch are derived in 64-bit and range-checked exactly like
// ComputeBlockReinterpretCopyLayout above.
[[nodiscard]] std::optional<PlacedFootprintRowLayout>
ComputePlacedFootprintRowLayout(const D3D12_SUBRESOURCE_FOOTPRINT &footprint,
                                bool format_known,
                                const MTL_DXGI_FORMAT_DESC &format_desc);

// Texel origin of the tile at tile coordinate (x, y, z).
[[nodiscard]] WMTOrigin TileCopyOrigin(const D3D12_TILE_SHAPE &tile_shape,
                                       UINT x, UINT y, UINT z);

// Extent of a single tile clamped to the subresource it belongs to. Any zero
// component means the tile lies fully outside the subresource.
[[nodiscard]] WMTSize TileCopySize(const D3D12_TILE_SHAPE &tile_shape,
                                   const WMTSize &subresource_size,
                                   const WMTOrigin &origin);

// Linear-buffer row geometry used when copying a single tile.
struct TileCopyRowLayout {
  UINT row_pitch = 0;
  UINT row_count = 1;
  UINT image_pitch = 0;
};

[[nodiscard]] TileCopyRowLayout
ComputeTileCopyRowLayout(const MTL_DXGI_FORMAT_DESC &format,
                         const WMTSize &size);

} // namespace dxmt::d3d12
