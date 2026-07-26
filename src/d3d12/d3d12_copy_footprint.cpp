#include "d3d12_copy_footprint.hpp"

#include <algorithm>
#include <climits>

namespace dxmt::d3d12 {

namespace {

UINT64
AlignUp(UINT64 value, UINT64 alignment) {
  return alignment ? ((value + alignment - 1) / alignment) * alignment : value;
}

bool
FormatIsBlockCompressed(const MTL_DXGI_FORMAT_DESC &format) {
  return (format.Flag & MTL_DXGI_FORMAT_BC) != 0;
}

UINT
BlitBlockWidth(const MTL_DXGI_FORMAT_DESC &format) {
  return FormatIsBlockCompressed(format) ? kBlockCompressedTexelsPerBlockAxis
                                         : 1u;
}

UINT
BlitBlockHeight(const MTL_DXGI_FORMAT_DESC &format) {
  return FormatIsBlockCompressed(format) ? kBlockCompressedTexelsPerBlockAxis
                                         : 1u;
}

UINT
BlitElementSize(const MTL_DXGI_FORMAT_DESC &format) {
  return FormatIsBlockCompressed(format) ? format.BlockSize
                                         : format.BytesPerTexel;
}

} // namespace

std::optional<BlockReinterpretCopyLayout>
ComputeBlockReinterpretCopyLayout(const WMTSize &size) {
  const UINT64 row_bytes = UINT64(size.width) * kBlockReinterpretBytesPerTexel;
  const UINT64 row_pitch =
      AlignUp(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
  const UINT64 image_pitch = row_pitch * UINT64(size.height);
  const UINT64 staging_length = image_pitch * UINT64(size.depth);
  const UINT64 dst_width =
      UINT64(size.width) * kBlockReinterpretTexelsPerBlockAxis;
  const UINT64 dst_height =
      UINT64(size.height) * kBlockReinterpretTexelsPerBlockAxis;
  if (!size.width || !size.height || !size.depth || row_pitch > UINT_MAX ||
      image_pitch > UINT_MAX || staging_length > SIZE_MAX ||
      dst_width > UINT_MAX || dst_height > UINT_MAX)
    return std::nullopt;

  BlockReinterpretCopyLayout layout = {};
  layout.row_pitch = UINT(row_pitch);
  layout.image_pitch = UINT(image_pitch);
  layout.staging_length = staging_length;
  layout.dst_size = {UINT(dst_width), UINT(dst_height), size.depth};
  return layout;
}

std::optional<PlacedFootprintRowLayout>
ComputePlacedFootprintRowLayout(const D3D12_SUBRESOURCE_FOOTPRINT &footprint,
                                bool format_known,
                                const MTL_DXGI_FORMAT_DESC &format_desc) {
  const UINT block_height = format_known && FormatIsBlockCompressed(format_desc)
                                ? kBlockCompressedTexelsPerBlockAxis
                                : 1u;
  const UINT64 row_count = std::max<UINT64>(
      1, (UINT64(footprint.Height) + block_height - 1) / block_height);
  const UINT64 image_pitch = UINT64(footprint.RowPitch) * row_count;
  if (row_count > UINT_MAX || image_pitch > UINT_MAX)
    return std::nullopt;

  PlacedFootprintRowLayout layout = {};
  layout.block_height = block_height;
  layout.row_count = UINT(row_count);
  layout.image_pitch = UINT(image_pitch);
  return layout;
}

WMTOrigin
TileCopyOrigin(const D3D12_TILE_SHAPE &tile_shape, UINT x, UINT y, UINT z) {
  // The tile coordinate and the tile shape are both UINT while WMTOrigin is
  // 64-bit, so widen before multiplying exactly like the footprint helpers
  // above: a 32-bit product would wrap at 2^32 texels and only then be
  // widened. The reserved-texture tiling that feeds this today keeps the
  // product inside a texture dimension, but the reserved-buffer tiling in
  // d3d12_resource.cpp already pairs a 65536-texel tile shape with a tile
  // count spanning the whole UINT range, so the narrow form is one caller
  // away from wrapping.
  return WMTOrigin{UINT64(x) * tile_shape.WidthInTexels,
                   UINT64(y) * tile_shape.HeightInTexels,
                   UINT64(z) * tile_shape.DepthInTexels};
}

WMTSize
TileCopySize(const D3D12_TILE_SHAPE &tile_shape,
             const WMTSize &subresource_size, const WMTOrigin &origin) {
  return WMTSize{
      std::min<UINT64>(tile_shape.WidthInTexels,
                       subresource_size.width > origin.x
                           ? subresource_size.width - origin.x
                           : 0),
      std::min<UINT64>(tile_shape.HeightInTexels,
                       subresource_size.height > origin.y
                           ? subresource_size.height - origin.y
                           : 0),
      std::min<UINT64>(tile_shape.DepthInTexels,
                       subresource_size.depth > origin.z
                           ? subresource_size.depth - origin.z
                           : 0)};
}

TileCopyRowLayout
ComputeTileCopyRowLayout(const MTL_DXGI_FORMAT_DESC &format,
                         const WMTSize &size) {
  const UINT block_width = BlitBlockWidth(format);
  const UINT block_height = BlitBlockHeight(format);
  const UINT element_size = BlitElementSize(format);
  TileCopyRowLayout layout = {};
  layout.row_pitch = static_cast<UINT>(
      AlignUp(((size.width + block_width - 1) / block_width) * element_size,
              D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
  layout.row_count =
      std::max<UINT>(1, (size.height + block_height - 1) / block_height);
  layout.image_pitch = layout.row_pitch * layout.row_count;
  return layout;
}

} // namespace dxmt::d3d12
