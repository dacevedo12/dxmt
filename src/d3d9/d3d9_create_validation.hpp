/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "winemetal.h"
#include "d3d9.h"
#include "d3d9_format.hpp"

// The Metal-independent create-time rejection matrix shared by the three
// texture entry points (CreateTexture / CreateCubeTexture /
// CreateVolumeTexture), lifted out of MTLD3D9Device so the pure pool x usage x
// format-class x dims/levels decision is host-testable without a Metal device,
// the same move plan_stretch_rect makes in d3d9_stretchrect.hpp. References:
// wined3d (PRIMARY)
// dlls/d3d9/texture.c d3d9_texture_2d/cube/3d_init + dlls/wined3d/texture.c
// wined3d_texture_init; DXVK (secondary) src/d3d9/d3d9_common_texture.cpp
// NormalizeTextureProperties.
//
// What stays in the device (needs Metal, a live surface, or carries a
// diagnostic already pinned by another host test): the shared-handle policy,
// the MSAA sample-count probe (surfaces only), the format-lowering rejects
// (unsupported-format and RENDERTARGET-capability, pinned by
// test_format_matrix / test_color_rt_format and logged for game bring-up), the
// allocation itself, and the buffer-backed linear-pitch alignment.

namespace dxmt {

enum class D3D9TextureCreateKind : uint8_t {
  Texture2D,
  Cube,
  Volume,
};

struct D3D9TextureCreateInfo {
  D3D9TextureCreateKind kind;
  D3DFORMAT format;
  D3DPOOL pool;
  DWORD usage;
  uint32_t width;
  uint32_t height; // == width for a cube (EdgeLength); mip 0 extent
  uint32_t depth;  // volume slice count; 1 for a 2D texture or cube
  uint32_t levels; // 0 == request the full chain
  bool is_ex;      // device was created through IDirect3D9Ex
};

// Full mip chain length for the given extents: wined3d_log2i(max) + 1. The 2D
// and cube paths pass depth = 1 so only width/height count.
inline uint32_t
d3d9_full_mip_levels(uint32_t width, uint32_t height, uint32_t depth) {
  uint32_t m = width;
  if (height > m)
    m = height;
  if (depth > m)
    m = depth;
  uint32_t levels = 1;
  while (m > 1) {
    m >>= 1;
    ++levels;
  }
  return levels;
}

// Returns D3D_OK when every Metal-independent create gate passes, else
// D3DERR_INVALIDCALL. All rejects share one HRESULT, so the order among them
// does not affect the result.
inline HRESULT
validate_texture_create(const D3D9TextureCreateInfo &c) {
  const bool is_volume = c.kind == D3D9TextureCreateKind::Volume;
  const bool is_cube = c.kind == D3D9TextureCreateKind::Cube;

  // Dimensions. 2D/cube mirror GetDeviceCaps's MaxTextureWidth/Height (16384);
  // volumes mirror MaxVolumeExtent (2048). A zero extent is always invalid.
  // The caps gate the creates rather than letting the allocator fail an
  // over-cap request as OUTOFVIDEOMEMORY.
  if (c.width == 0 || c.height == 0 || (is_volume && c.depth == 0))
    return D3DERR_INVALIDCALL;
  const uint32_t max_extent = is_volume ? 2048u : 16384u;
  if (c.width > max_extent || c.height > max_extent || (is_volume && c.depth > max_extent))
    return D3DERR_INVALIDCALL;

  // Block-compressed (DXTn) mip 0 must align to the 4x4 block on width and
  // height (native drivers reject an unaligned compressed create); volume
  // depth is unconstrained (block depth is 1).
  if (IsCompressedFormat(c.format) && ((c.width & 3u) || (c.height & 3u)))
    return D3DERR_INVALIDCALL;

  // Pool enum validity (DEFAULT..SCRATCH). D3DPOOL_MANAGED is invalid on a
  // D3D9Ex device (the distinct MANAGED_EX value 6 is handled elsewhere).
  if (c.pool > D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;
  if (c.pool == D3DPOOL_MANAGED && c.is_ex)
    return D3DERR_INVALIDCALL;

  // D3DUSAGE_WRITEONLY is a vertex/index-buffer flag; invalid on a texture.
  if (c.usage & D3DUSAGE_WRITEONLY)
    return D3DERR_INVALIDCALL;

  if (is_volume) {
    // Volumes can never be render targets or depth-stencil surfaces, and have
    // no sampler-time auto-mip path (AUTOGENMIPMAP is 2D/cube only).
    if (c.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
      return D3DERR_INVALIDCALL;
    if (c.usage & D3DUSAGE_AUTOGENMIPMAP)
      return D3DERR_INVALIDCALL;
    // DYNAMIC is incompatible with MANAGED (sysmem master with its own upload
    // model) and, for volumes specifically, with SCRATCH: DXVK scopes the
    // SCRATCH+DYNAMIC reject to volumes, and the wine d3d9 test asserts the
    // volume case, while 2D and cube permit the combo (below).
    if ((c.usage & D3DUSAGE_DYNAMIC) && (c.pool == D3DPOOL_MANAGED || c.pool == D3DPOOL_SCRATCH))
      return D3DERR_INVALIDCALL;
  } else {
    // RT and DS are mutually exclusive on a texture and must live in DEFAULT:
    // the GPU-only side has no managed mirror to push back from the CPU.
    if ((c.usage & D3DUSAGE_RENDERTARGET) && (c.usage & D3DUSAGE_DEPTHSTENCIL))
      return D3DERR_INVALIDCALL;
    if ((c.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && c.pool != D3DPOOL_DEFAULT)
      return D3DERR_INVALIDCALL;
    // DYNAMIC is incompatible with MANAGED and with an RT/DS bind. SCRATCH +
    // DYNAMIC stays legal for 2D/cube (only volumes reject it, above).
    if ((c.usage & D3DUSAGE_DYNAMIC) && c.pool == D3DPOOL_MANAGED)
      return D3DERR_INVALIDCALL;
    if ((c.usage & D3DUSAGE_DYNAMIC) && (c.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
      return D3DERR_INVALIDCALL;
    // AUTOGENMIPMAP forbids SYSTEMMEM and restricts Levels to 0/1 (the app
    // sees a single level; the runtime owns the generated chain).
    if (c.usage & D3DUSAGE_AUTOGENMIPMAP) {
      if (c.pool == D3DPOOL_SYSTEMMEM)
        return D3DERR_INVALIDCALL;
      if (c.levels != 0 && c.levels != 1)
        return D3DERR_INVALIDCALL;
    }
  }

  // Depth-stencil format placement. A depth format on a cube
  // is rejected outright (any pool, any usage): no native driver backs a depth
  // cube, allowing it trips a broken code path in Gothic 3 (DXVK
  // d3d9_common_texture.cpp), and CheckDeviceFormat already denies CUBETEXTURE
  // + DEPTHSTENCIL, so accepting would split the probe from the create. A
  // depth format on a 2D texture is creatable only in DEFAULT (INTZ-style
  // shadow maps): MANAGED / SYSTEMMEM / SCRATCH would need a CPU-visible
  // (Shared) depth texture, which Metal forbids, so the create is rejected up
  // front rather than dying at the allocator (nil texture -> OUTOFVIDEOMEMORY
  // at best, a validation abort at worst). Native + DXVK reject both; the
  // offscreen-plain depth surface keeps its own exemption in the device, and
  // volumes reject every depth format through IsVolumeTextureFormat below.
  if (is_cube && IsDepthStencilFormat(c.format))
    return D3DERR_INVALIDCALL;
  if (!is_volume && !is_cube && IsDepthStencilFormat(c.format) && c.pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;

  // Volume format class. The formats Metal cannot realize as a 3D texture
  // (block-compressed, 3Dc, packed YUV) are creatable only as a CPU-only
  // SCRATCH blob; every other format must be a mapped color 3D format
  // (IsVolumeTextureFormat also excludes depth, matching the probe). The 2D
  // and cube unsupported-format / RT-capability rejects stay in the device
  // where they carry a diagnostic and are pinned by test_format_matrix /
  // test_color_rt_format.
  if (is_volume) {
    if (IsCompressedFormat(c.format) || Is3DcFormat(c.format) || IsScratchableUnsupportedFormat(c.format)) {
      if (c.pool != D3DPOOL_SCRATCH)
        return D3DERR_INVALIDCALL;
    } else if (!IsVolumeTextureFormat(c.format)) {
      return D3DERR_INVALIDCALL;
    }
  }

  // Levels. 0 means the full chain. A count beyond the chain is rejected
  //: Metal caps the mip count at the pyramid, so wined3d's
  // degenerate 1x1 tail levels are unrealizable and dxmt fails closed with
  // INVALIDCALL. No reference pins native's exact HRESULT here (DXVK clamps),
  // so this is oracle-debt; revisit if a native probe lands (clamp is the
  // only other option Metal can honour).
  if (c.levels != 0) {
    const uint32_t max_levels = d3d9_full_mip_levels(c.width, c.height, is_volume ? c.depth : 1u);
    if (c.levels > max_levels)
      return D3DERR_INVALIDCALL;
  }

  return D3D_OK;
}

// The D3D9Ex Create* Usage gate. Only the three RESTRICT_* flags are accepted;
// passing D3DUSAGE_RENDERTARGET / D3DUSAGE_DEPTHSTENCIL explicitly is
// INVALIDCALL on Windows (the Ex Create method implies the resource type), and
// either shared-resource flag requires a non-null pSharedHandle. Native + DXVK
// (d3d9_device.cpp: "Yes, it actually fails when explicitly passing
// D3DUSAGE_RENDERTARGET"); wined3d is the looser one. Kept a pure predicate so
// it can be exercised without a device.
inline HRESULT
validateCreateExUsage(DWORD Usage, HANDLE *pSharedHandle) {
  constexpr DWORD valid_ex_usage_mask =
      D3DUSAGE_RESTRICTED_CONTENT | D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER;
  if (Usage & ~valid_ex_usage_mask)
    return D3DERR_INVALIDCALL;
  if ((Usage & (D3DUSAGE_RESTRICT_SHARED_RESOURCE | D3DUSAGE_RESTRICT_SHARED_RESOURCE_DRIVER)) != 0 &&
      pSharedHandle == nullptr)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}

} // namespace dxmt
