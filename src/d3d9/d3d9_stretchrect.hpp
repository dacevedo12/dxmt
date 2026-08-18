#pragma once

#include "winemetal.h"
#include "d3d9.h"
#include "d3d9_format.hpp"

// StretchRect validation + kind selection, lifted out of
// MTLD3D9Device::StretchRect (the same move as d3d9_sampler_translate.hpp) so
// the pure decision is host-testable without a Metal device. References: wined3d
// (PRIMARY) dlls/d3d9/device.c d3d9_device_StretchRect + dlls/wined3d/device.c
// wined3d_device_context_blt + dlls/wined3d/texture.c texture2d_blt +
// dlls/wined3d/resource.c wined3d_resource_check_box_dimensions; DXVK
// (secondary) src/d3d9/d3d9_device.cpp StretchRect + src/d3d9/d3d9_util.h
// AreFormatsSimilar / IsBlitRegionInvalid.

namespace dxmt {

// DXVK d3d9_util.h AreFormatsSimilar: a same-storage copy is legal only for
// identical formats or the four one-way alpha -> "X" (ignored-alpha) pairs. The
// reverse X -> A is deliberately excluded because an X source has no defined
// alpha; copying it into an A destination would propagate whatever the storage
// alpha happens to be, so both refs force alpha = 1 through the swizzled /
// converting blit instead.
inline bool
d3d9_formats_similar(D3DFORMAT src, D3DFORMAT dst) {
  return src == dst || (src == D3DFMT_A8B8G8R8 && dst == D3DFMT_X8B8G8R8) ||
         (src == D3DFMT_A8R8G8B8 && dst == D3DFMT_X8R8G8B8) || (src == D3DFMT_A1R5G5B5 && dst == D3DFMT_X1R5G5B5) ||
         (src == D3DFMT_A4R4G4B4 && dst == D3DFMT_X4R4G4B4);
}

// D3DFormatSamplerSwizzle returns the {Zero,Zero,Zero,Zero} "no override"
// sentinel for a format sampled through the identity channel map; any other
// value is a real fixup (L8 -> LLL1, X-alpha -> 1, A4R4G4B4 permute, 2-channel
// .b = 1, depth RRRR...). A converting stretch FROM such a format has to sample
// through that swizzle to read the shape D3D9 promises.
inline bool
d3d9_format_needs_sampler_swizzle(D3DFORMAT format) {
  WMTTextureSwizzleChannels sw = D3DFormatSamplerSwizzle(format);
  return !(
      sw.r == WMTTextureSwizzleZero && sw.g == WMTTextureSwizzleZero && sw.b == WMTTextureSwizzleZero &&
      sw.a == WMTTextureSwizzleZero
  );
}

// wined3d resource.c wined3d_resource_check_box_dimensions block-mask rule for
// block-compressed formats: a sub-rect origin must sit on a block boundary, and
// its right / bottom edge must be block-aligned OR touch the surface edge (the
// final block row / column may be partial). Non-compressed formats always pass.
inline bool
d3d9_rect_block_aligned(
    D3DFORMAT format, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t surf_w, uint32_t surf_h
) {
  if (!IsCompressedFormat(format))
    return true;
  const uint32_t wmask = D3DFormatBlockWidth(format) - 1u;
  const uint32_t hmask = D3DFormatBlockHeight(format) - 1u;
  if ((x0 & wmask) || (y0 & hmask))
    return false;
  if ((x1 & wmask) && x1 != surf_w)
    return false;
  if ((y1 & hmask) && y1 != surf_h)
    return false;
  return true;
}

enum class StretchRectKind : uint8_t {
  Copy = 0,               // MTLBlit same-format sub-rect copy (BC honoured natively)
  Stretch = 1,            // render-pass sample + scale + channel-fixup
  Resolve = 2,            // 1:1 fragment-average MSAA resolve
  ResolveThenStretch = 3, // scaled / fixup-source resolve: resolve to a transient, then Stretch
  DepthResolve = 4,       // point (sample 0) resolve of a multisampled depth surface
};

// Everything the kind decision needs about one participant, reduced to plain
// values so the helper never touches a live surface or Metal object. The Metal
// sample count (1 = single-sampled) is what MTLBlitCommandEncoder's equal-count
// rule and the resolve trigger key on, not the raw D3DMULTISAMPLE_TYPE.
struct StretchRectSurface {
  WMTPixelFormat metal_format;
  D3DFORMAT d3d_format;
  D3DPOOL pool;
  DWORD usage;
  uint32_t width;
  uint32_t height;
  uint32_t sample_count;
  bool is_texture_subresource;
};

struct StretchRectPlan {
  HRESULT hr; // D3D_OK or D3DERR_INVALIDCALL
  StretchRectKind kind;
  // Effective (NULL-resolved, validated) sub-rects, origin + extent.
  uint32_t src_x, src_y, src_w, src_h;
  uint32_t dst_x, dst_y, dst_w, dst_h;
};

inline StretchRectPlan
plan_stretch_rect(
    const StretchRectSurface &src, const StretchRectSurface &dst, const RECT *src_rect, const RECT *dst_rect,
    D3DTEXTUREFILTERTYPE filter, bool in_scene, bool from_readback = false
) {
  StretchRectPlan plan{};
  auto fail = [&plan]() -> StretchRectPlan {
    plan.hr = D3DERR_INVALIDCALL;
    return plan;
  };

  // Filter: only NONE / POINT / LINEAR are legal for StretchRect (the IDL
  // rejects ANISOTROPIC and the *QUAD filters); wined3d texture.c and DXVK both
  // INVALIDCALL anything else.
  if (filter != D3DTEXF_NONE && filter != D3DTEXF_LINEAR && filter != D3DTEXF_POINT)
    return fail();

  // Both participants must be GPU-only (DEFAULT). A CPU-access pool is
  // UpdateSurface / GetRenderTargetData territory; wined3d device.c INVALIDCALLs
  // a CPU-access source or destination, DXVK gates both pools == DEFAULT.
  if (src.pool != D3DPOOL_DEFAULT || dst.pool != D3DPOOL_DEFAULT)
    return fail();

  // A texture sub-resource destination is legal only when its texture carries a
  // RENDERTARGET / DEPTHSTENCIL bind; a plain-texture level is INVALIDCALL
  // (wined3d bind check, DXVK dstIsSurface arm). A standalone surface has no
  // parent texture and is always a valid destination. (DXVK additionally
  // permits an exact whole-surface copy into a plain-texture level on a D3D9Ex
  // device, a native-verified relaxation wined3d does not implement; dxmt sits
  // with wined3d, the primary reference. Follow DXVK only if a title surfaces.)
  if (dst.is_texture_subresource && !(dst.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)))
    return fail();

  // A plain offscreen destination (a standalone surface with no RT / DS bind)
  // takes a copy only from another plain surface. A texture-subresource or a
  // render-target source into a plain surface is INVALIDCALL on native; wined3d
  // and DXVK are lenient here (todo_wine). The lone internal exception is
  // GetRenderTargetData's DEFAULT-pool copy of a render target into a plain
  // offscreen surface, which the device forwards with from_readback set.
  const bool dst_plain = !dst.is_texture_subresource && !(dst.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
  const bool src_plain = !src.is_texture_subresource && !(src.usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL));
  if (dst_plain && !src_plain && !from_readback)
    return fail();

  const bool src_is_ds = IsDepthStencilFormat(src.d3d_format);
  const bool dst_is_ds = IsDepthStencilFormat(dst.d3d_format);
  // Depth-ness must match pairwise; DS <-> colour is spec-illegal (wined3d
  // texture.c aspect check, DXVK srcIsDS != dstIsDS).
  if (src_is_ds != dst_is_ds)
    return fail();

  // Resolve the rects. NULL means the full surface extent. Reject inverted,
  // empty, and out-of-bounds boxes on either side (wined3d
  // check_box_dimensions -> WINEDDERR_INVALIDRECT -> INVALIDCALL, DXVK
  // IsBlitRegionInvalid).
  uint32_t sx0 = 0, sy0 = 0, sw = src.width, sh = src.height;
  uint32_t dx0 = 0, dy0 = 0, dw = dst.width, dh = dst.height;
  if (src_rect) {
    if (src_rect->left < 0 || src_rect->top < 0 || src_rect->right <= src_rect->left ||
        src_rect->bottom <= src_rect->top || (uint32_t)src_rect->right > src.width ||
        (uint32_t)src_rect->bottom > src.height)
      return fail();
    sx0 = src_rect->left;
    sy0 = src_rect->top;
    sw = src_rect->right - src_rect->left;
    sh = src_rect->bottom - src_rect->top;
  }
  if (dst_rect) {
    if (dst_rect->left < 0 || dst_rect->top < 0 || dst_rect->right <= dst_rect->left ||
        dst_rect->bottom <= dst_rect->top || (uint32_t)dst_rect->right > dst.width ||
        (uint32_t)dst_rect->bottom > dst.height)
      return fail();
    dx0 = dst_rect->left;
    dy0 = dst_rect->top;
    dw = dst_rect->right - dst_rect->left;
    dh = dst_rect->bottom - dst_rect->top;
  }
  plan.src_x = sx0;
  plan.src_y = sy0;
  plan.src_w = sw;
  plan.src_h = sh;
  plan.dst_x = dx0;
  plan.dst_y = dy0;
  plan.dst_w = dw;
  plan.dst_h = dh;

  const bool src_ms = src.sample_count > 1;
  const bool dst_ms = dst.sample_count > 1;

  // Depth-stencil: both surfaces DS, same lowered Metal format, equal extent,
  // full rects on both sides, outside an active scene. This covers the
  // INTZ-style depth snapshot between frames. A format-converting or partial DS
  // blit needs a depth-write PSO (deferred); a multisampled depth surface on
  // either side needs a true depth resolve or broadcast (deferred, the device
  // logs it). A mixed sample-count DS pair is rejected rather than left to the
  // copy below, which Metal rejects for unequal sample counts. wined3d services
  // the multisampled destination through a per-subresource resolved location,
  // which dxmt has no equivalent of. wined3d device.c DS block + DEPTH_BLIT,
  // DXVK both-DS + full-src-rect + not-in-scene.
  if (src_is_ds) {
    if (in_scene)
      return fail();
    if (src.metal_format != dst.metal_format)
      return fail();
    if (src.width != dst.width || src.height != dst.height)
      return fail();
    if (sx0 || sy0 || sw != src.width || sh != src.height || dx0 || dy0 || dw != dst.width || dh != dst.height)
      return fail();
    // A multisampled depth source into a single-sample depth destination is a
    // point resolve (sample 0, which D3DTEXF_POINT selects); the device drives a
    // depth-write pass that picks that sample. A multisampled DESTINATION still
    // has no path (wined3d's per-subresource resolved location has no dxmt
    // equivalent), so it stays rejected.
    if (src_ms && !dst_ms) {
      plan.kind = StretchRectKind::DepthResolve;
      plan.hr = D3D_OK;
      return plan;
    }
    if (src_ms || dst_ms)
      return fail();
    plan.kind = StretchRectKind::Copy;
    plan.hr = D3D_OK;
    return plan;
  }

  const bool extent_mismatch = (sw != dw) || (sh != dh);

  // MSAA source -> MSAA destination. A same-format, same-extent pair at equal
  // sample count is a straight blit copy (Metal copies matched sample counts
  // sample-for-sample), preserving per-sample data. A scale or format convert
  // has no direct MSAA -> MSAA Metal primitive (the blit encoder can neither
  // scale nor touch a multisampled image), so resolve the source to a
  // single-sample transient and broadcast-stretch it into the multisampled
  // destination (ResolveThenStretch: the resolve leg is the same MSAA -> single
  // path used below, the stretch leg renders into the MSAA dst). This is the
  // fall-off-the-fast-path route DXVK takes (d3d9_device.cpp, equal-count
  // MSAA -> MSAA drops the copy fast path to the draw blit). A count mismatch
  // stays rejected (no title needs it, native's handling unverified); compressed
  // formats can never be multisampled.
  if (src_ms && dst_ms) {
    if (src.sample_count != dst.sample_count)
      return fail();
    if (!extent_mismatch && src.metal_format == dst.metal_format &&
        d3d9_formats_similar(src.d3d_format, dst.d3d_format)) {
      plan.kind = StretchRectKind::Copy;
      plan.hr = D3D_OK;
      return plan;
    }
    plan.kind = StretchRectKind::ResolveThenStretch;
    plan.hr = D3D_OK;
    return plan;
  }

  // MSAA source -> single-sample destination: a resolve. A same-extent resolve
  // from an identity-swizzle source averages the samples directly (Resolve). A
  // scaled resolve, or a resolve from a fixup-needing source, resolves to a
  // transient at the source extent and then Stretches so the scale and the
  // channel fixup are both honoured; a bare 1:1 Resolve would crop instead of
  // scale and would skip the source swizzle. Neither BC (uncompressible
  // as MSAA on the source) nor a BC destination is reachable: reject a BC dst
  // that would need the renderable resolve/stretch path.
  if (src_ms) {
    if (IsCompressedFormat(dst.d3d_format))
      return fail();
    if (extent_mismatch || d3d9_format_needs_sampler_swizzle(src.d3d_format))
      plan.kind = StretchRectKind::ResolveThenStretch;
    else
      plan.kind = StretchRectKind::Resolve;
    plan.hr = D3D_OK;
    return plan;
  }

  // Single-sample source -> MSAA destination. Metal cannot blit-copy
  // into a multisampled texture; it needs a render pass at the destination
  // sample count that broadcasts the sampled source into every sample. The
  // StretchBlit path does exactly that, its PSO + render pass keyed on the dst
  // sample count. A compressed source can't feed the sampler-scale, and a
  // compressed texture can never be a multisampled target anyway. References:
  // wined3d texture.c (draw blitter into an MSAA dst), DXVK d3d9_device.cpp
  // (fbBlit -> blitImageView).
  if (dst_ms) {
    if (IsCompressedFormat(src.d3d_format))
      return fail();
    plan.kind = StretchRectKind::Stretch;
    plan.hr = D3D_OK;
    return plan;
  }

  // A genuine stretch (a dimension change) drives the sampler-scale render pass,
  // which can only target a render target: native rejects StretchRect scaling
  // into a non-RT destination (an offscreen-plain or a plain-texture surface).
  // Both references are lenient here (wined3d is todo_wine; DXVK deliberately
  // permits an offscreen-plain destination, "works fine in practice"), so this
  // sits with the strict native expectation the test pins, the same side the
  // plain-destination gates above take. A same-size copy or format convert stays
  // legal into any single-sampled destination.
  if (extent_mismatch && !(dst.usage & D3DUSAGE_RENDERTARGET))
    return fail();

  // Both single-sampled. A raw copy is legal only for a same-Metal-format,
  // same-extent, alpha-safe pair (identical D3DFORMAT, or the alpha -> X rows);
  // everything else (scale, convert, or the X -> A alpha fix) takes the
  // render-pass Stretch.
  const bool copy_ok =
      !extent_mismatch && src.metal_format == dst.metal_format && d3d9_formats_similar(src.d3d_format, dst.d3d_format);
  if (copy_ok) {
    // A compressed same-format copy must be block-aligned on both sides (edge
    // exempt), or MTLBlitCommandEncoder rejects the unaligned origin.
    if (!d3d9_rect_block_aligned(src.d3d_format, sx0, sy0, sx0 + sw, sy0 + sh, src.width, src.height) ||
        !d3d9_rect_block_aligned(dst.d3d_format, dx0, dy0, dx0 + dw, dy0 + dh, dst.width, dst.height))
      return fail();
    plan.kind = StretchRectKind::Copy;
    plan.hr = D3D_OK;
    return plan;
  }

  // The Stretch path samples + renders, so it cannot source or target a
  // compressed texture (Apple Silicon can't render to BC, and the sampler-scale
  // needs a renderable destination); wined3d and DXVK both reject compressed
  // here. Only the Copy path above handles BC.
  if (IsCompressedFormat(src.d3d_format) || IsCompressedFormat(dst.d3d_format))
    return fail();
  plan.kind = StretchRectKind::Stretch;
  plan.hr = D3D_OK;
  return plan;
}

} // namespace dxmt
