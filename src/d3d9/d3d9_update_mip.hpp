#pragma once

#include <cstdint>

// Source mip-level offset for UpdateTexture (SYSTEMMEM master -> DEFAULT
// mirror), the ONE derivation shared by the 2D / cube / volume branches of
// MTLD3D9Device::UpdateTexture. When the source chain has a larger level-0
// base than the destination, the destination maps onto the source's mip tail;
// the offset is how many source levels to skip to reach one whose extent
// matches the destination base.
//
// Derived from the level-0 BASE SIZE ratio, NOT from the level-count
// difference: wined3d (PRIMARY) dlls/wined3d/device.c
// wined3d_device_update_texture halves the larger source extent until it no
// longer exceeds the destination extent, counting the shifts. This top-aligns
// two chains of unequal depth that share a base size, covering the mip-stream
// pattern (more sysmem levels than GPU levels) and an AUTOGENMIPMAP
// destination (app level count 1 over a full internal chain), both of which a
// level-count difference wrongly rejected. DXVK (secondary) src/d3d9
// d3d9_device.cpp reaches the same offset, but only when the level-0 extents
// differ.
//
// For two full chains of equal base the loop never runs and the offset is 0,
// byte-identical to the level-count derivation, so matching-chain uploads (the
// common case) are unchanged. size = max(width, height, depth) per side; the
// 2D and cube callers pass depth 1 (a cube is square, so width == height).

namespace dxmt {

inline uint32_t
update_mip_offset(
    uint32_t src_width, uint32_t src_height, uint32_t src_depth, uint32_t dst_width, uint32_t dst_height,
    uint32_t dst_depth
) {
  uint32_t src_size = src_width > src_height ? src_width : src_height;
  if (src_depth > src_size)
    src_size = src_depth;
  uint32_t dst_size = dst_width > dst_height ? dst_width : dst_height;
  if (dst_depth > dst_size)
    dst_size = dst_depth;
  uint32_t offset = 0;
  while (src_size > dst_size) {
    src_size >>= 1;
    ++offset;
  }
  return offset;
}

} // namespace dxmt
