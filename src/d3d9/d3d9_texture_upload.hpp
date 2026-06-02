#pragma once

#include <cstddef>
#include <cstdint>

namespace dxmt {

struct TextureUploadLayout {
  uint32_t bytes_per_image; // bytes per destination depth slice
  size_t total_bytes;       // staging size for every slice
};

// Staging byte layout for a (sub-region) texture upload. A volume (3D)
// texture stages every depth slice, so the total scales with depth;
// ignoring it leaves all but the first slice unwritten. Compressed
// formats round the row count up to whole 4x4 block rows (Metal requires
// sourceBytesPerImage >= bytesPerRow * rows). depth 0 is treated as 1.
inline TextureUploadLayout
texture_upload_layout(uint32_t src_pitch, uint32_t height, uint32_t depth, bool compressed) {
  uint32_t row_count = compressed ? (height + 3u) / 4u : height;
  uint32_t bytes_per_image = src_pitch * row_count;
  uint32_t slices = depth ? depth : 1u;
  return {bytes_per_image, static_cast<size_t>(bytes_per_image) * slices};
}

} // namespace dxmt
