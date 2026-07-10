#pragma once

#include <cstdint>

// Pure block-rect validation and lock-offset math for surface / volume / cube
// image locks, factored out of the LockRect / LockBox / AddDirty* paths so the
// host unit tier can exercise it without a Metal device (the d3d9_buffer_map.hpp
// shape for the buffer side). References: wined3d resource.c
// (wined3d_resource_check_box_dimensions + resource_offset_map_pointer), DXVK
// d3d9_device.cpp (CalcImageLockOffset).

namespace dxmt {

// Ports wined3d_resource_check_box_dimensions (wined3d resource.c): a mapped or
// dirty box is valid when it is non-inverted, in bounds against the level
// extent, and - for a block-addressed format (block_w/block_h > 1) - block-
// aligned on its left/top edges with right/bottom either block-aligned or
// exactly at the extent. block_w/block_h are 1 for uncompressed formats; block
// depth is always 1 (D3D9 has no depth-compressed formats). Coordinates are
// unsigned: a negative RECT edge cast to uint32_t wraps large and trips the
// bounds test, the way wined3d stores box edges into UINT fields via
// wined3d_box_set.
inline bool
image_box_dimensions_valid(
    uint32_t left, uint32_t top, uint32_t right, uint32_t bottom, uint32_t front, uint32_t back, uint32_t width,
    uint32_t height, uint32_t depth, uint32_t block_w, uint32_t block_h
) {
  if (left >= right || right > width || top >= bottom || bottom > height || front >= back || back > depth)
    return false;
  // Power-of-two block sizes (wined3d makes the same assumption); mask = size-1.
  if (block_w > 1 || block_h > 1) {
    const uint32_t wmask = block_w - 1u;
    const uint32_t hmask = block_h - 1u;
    if ((left & wmask) || (top & hmask) || ((right & wmask) && right != width) || ((bottom & hmask) && bottom != height))
      return false;
  }
  return true;
}

// Byte offset of a locked / uploaded box's first byte into a level mirror: the
// wined3d resource_offset_map_pointer / DXVK CalcImageLockOffset math shared by
// LockRect (the pointer handed to the app) and UnlockRect (the upload source).
// Row stride is the surface's stored (4-byte-aligned) pitch; the per-column step
// derives from the tight row_pitch divided by the block-column count, so a
// sub-DWORD-width row gets the right bytes-per-column. Compressed formats step
// in whole 4x4 blocks: the truncating divide floors x/y to the block that
// contains the requested corner, so a misaligned corner lands on its block
// origin rather than the level origin. 3Dc passes compressed=false to keep its
// linear one-byte fiction (its block validation is handled separately).
inline uint32_t
image_lock_block_offset(
    uint32_t x, uint32_t y, uint32_t surface_pitch, uint32_t row_pitch, uint32_t width, bool compressed
) {
  const uint32_t block_w = compressed ? 4u : 1u;
  const uint32_t block_h = compressed ? 4u : 1u;
  const uint32_t cols_per_row = compressed ? (width + 3u) / 4u : width;
  const uint32_t col_bytes = cols_per_row > 0u ? row_pitch / cols_per_row : 0u;
  return (y / block_h) * surface_pitch + (x / block_w) * col_bytes;
}

} // namespace dxmt
