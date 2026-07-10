#pragma once

#include "winemetal.h"
#include "d3d9.h"

#include <cstdint>

// D3D9 render-state -> Metal translation for the PSO/DSSO-side and
// encoder-side pipeline state. Lifted out of d3d9_device.cpp (like
// d3d9_sampler_translate.hpp / d3d9_state_defaults.hpp) so the pure
// conversions are host-testable without a Metal device. References: wined3d
// (PRIMARY) dlls/wined3d/context_vk.c blend/compare/cull decode +
// stateblock.c depth-stencil/blend build; DXVK (secondary)
// src/d3d9/d3d9_util.cpp DecodeBlendFactor / DecodeCompareOp /
// DecodeStencilOp / DecodeCullMode.
//
// Invalid-enum default arms follow the references: an out-of-range value
// (uninitialized app state) is garbage-in, but both refs decode it to a
// specific fallback, so we match them rather than inventing a third answer.

namespace dxmt {

// D3D9 clockwise = front; CCW = back. The encoder pins Metal's
// frontFacingWinding to Clockwise, so CW -> cullFront, CCW -> cullBack.
// An out-of-range cull value resolves to NONE (no culling): DXVK's
// DecodeCullMode default arm lands on VK_CULL_MODE_NONE (d3d9_util.cpp),
// which is the only grounded reference answer (wined3d passes the raw
// enum through to GL). Culling the back face instead would silently drop
// geometry for an app that wrote a bad cull mode.
inline WMTCullMode
to_mtl_cull_mode(DWORD d3dCull) {
  switch (d3dCull) {
  case D3DCULL_NONE:
    return WMTCullModeNone;
  case D3DCULL_CW:
    return WMTCullModeFront;
  case D3DCULL_CCW:
    return WMTCullModeBack;
  default:
    return WMTCullModeNone;
  }
}

// D3DFILL_POINT has no Metal equivalent: Metal's triangle fill mode is
// only Fill or Lines, with no per-triangle point rasterization. Both refs
// actually honor it (wined3d GL_POINT, DXVK VK_POLYGON_MODE_POINT in
// d3d9_util.cpp DecodeFillMode), so this is a hard Metal limit, not shared
// behavior; solid fill is the least-wrong fallback for the rare app that
// asks for point fill.
inline WMTTriangleFillMode
to_mtl_fill_mode(DWORD d3dFill) {
  switch (d3dFill) {
  case D3DFILL_WIREFRAME:
    return WMTTriangleFillModeLines;
  case D3DFILL_POINT:
  case D3DFILL_SOLID:
  default:
    return WMTTriangleFillModeFill;
  }
}

// D3DBLEND_* -> WMTBlendFactor. SRCCOLOR2 / INVSRCCOLOR2 are dual-source
// blending: Metal supports them via Source1 factors. The legacy combined
// modes BOTHSRCALPHA / BOTHINVSRCALPHA are only valid in D3DRS_SRCBLEND,
// where fixup_d3d9_blend_pair expands them into a src/dst pair before this
// mapper runs. If one survives to here it was written into D3DRS_DESTBLEND
// (invalid per spec): DXVK's DecodeBlendFactor decodes them as plain
// SRCALPHA / INVSRCALPHA (explicit cases), so match that rather than the
// unknown-value default. An out-of-range factor resolves to ZERO, the
// default arm both refs land on (wined3d context_vk.c FIXME + ZERO, DXVK
// DecodeBlendFactor default -> VK_BLEND_FACTOR_ZERO).
inline WMTBlendFactor
to_mtl_blend_factor(DWORD d3dBlend) {
  switch (d3dBlend) {
  case D3DBLEND_ZERO:
    return WMTBlendFactorZero;
  case D3DBLEND_ONE:
    return WMTBlendFactorOne;
  case D3DBLEND_SRCCOLOR:
    return WMTBlendFactorSourceColor;
  case D3DBLEND_INVSRCCOLOR:
    return WMTBlendFactorOneMinusSourceColor;
  case D3DBLEND_SRCALPHA:
  case D3DBLEND_BOTHSRCALPHA:
    return WMTBlendFactorSourceAlpha;
  case D3DBLEND_INVSRCALPHA:
  case D3DBLEND_BOTHINVSRCALPHA:
    return WMTBlendFactorOneMinusSourceAlpha;
  case D3DBLEND_DESTALPHA:
    return WMTBlendFactorDestinationAlpha;
  case D3DBLEND_INVDESTALPHA:
    return WMTBlendFactorOneMinusDestinationAlpha;
  case D3DBLEND_DESTCOLOR:
    return WMTBlendFactorDestinationColor;
  case D3DBLEND_INVDESTCOLOR:
    return WMTBlendFactorOneMinusDestinationColor;
  case D3DBLEND_SRCALPHASAT:
    return WMTBlendFactorSourceAlphaSaturated;
  case D3DBLEND_BLENDFACTOR:
    return WMTBlendFactorBlendColor;
  case D3DBLEND_INVBLENDFACTOR:
    return WMTBlendFactorOneMinusBlendColor;
  case D3DBLEND_SRCCOLOR2:
    return WMTBlendFactorSource1Color;
  case D3DBLEND_INVSRCCOLOR2:
    return WMTBlendFactorOneMinusSource1Color;
  default:
    return WMTBlendFactorZero;
  }
}

inline WMTBlendOperation
to_mtl_blend_op(DWORD d3dOp) {
  switch (d3dOp) {
  case D3DBLENDOP_ADD:
    return WMTBlendOperationAdd;
  case D3DBLENDOP_SUBTRACT:
    return WMTBlendOperationSubtract;
  case D3DBLENDOP_REVSUBTRACT:
    return WMTBlendOperationReverseSubtract;
  case D3DBLENDOP_MIN:
    return WMTBlendOperationMin;
  case D3DBLENDOP_MAX:
    return WMTBlendOperationMax;
  default:
    return WMTBlendOperationAdd;
  }
}

// D3DCOLORWRITEENABLE_* (RED=1, GREEN=2, BLUE=4, ALPHA=8) -> Metal
// WMTColorWriteMask. The bit values disagree (Metal's enum lists Red=8,
// Green=4, Blue=2, Alpha=1: the reverse byte order), so we remap rather
// than aliasing; a bit-mismatch would silently write the wrong channels.
inline uint8_t
to_mtl_write_mask(DWORD d3dMask) {
  uint8_t out = 0;
  if (d3dMask & D3DCOLORWRITEENABLE_RED)
    out |= WMTColorWriteMaskRed;
  if (d3dMask & D3DCOLORWRITEENABLE_GREEN)
    out |= WMTColorWriteMaskGreen;
  if (d3dMask & D3DCOLORWRITEENABLE_BLUE)
    out |= WMTColorWriteMaskBlue;
  if (d3dMask & D3DCOLORWRITEENABLE_ALPHA)
    out |= WMTColorWriteMaskAlpha;
  return out;
}

// D3DCMP_* -> WMTCompareFunction. Same enum order as Vulkan's VkCompareOp;
// DXVK src/d3d9/d3d9_util.cpp DecodeCompareOp uses the same translation
// against Vulkan, and Metal mirrors Vulkan here. An out-of-range value
// (notably ZFUNC/STENCILFUNC left at 0) decodes to NEVER, both refs'
// default arm (DXVK DecodeCompareOp default -> VK_COMPARE_OP_NEVER;
// wined3d context_vk.c default NEVER + WARN for 0). Defaulting to ALWAYS
// instead would draw everything where the refs draw nothing.
inline WMTCompareFunction
to_mtl_compare_func(DWORD d3dCmp) {
  switch (d3dCmp) {
  case D3DCMP_NEVER:
    return WMTCompareFunctionNever;
  case D3DCMP_LESS:
    return WMTCompareFunctionLess;
  case D3DCMP_EQUAL:
    return WMTCompareFunctionEqual;
  case D3DCMP_LESSEQUAL:
    return WMTCompareFunctionLessEqual;
  case D3DCMP_GREATER:
    return WMTCompareFunctionGreater;
  case D3DCMP_NOTEQUAL:
    return WMTCompareFunctionNotEqual;
  case D3DCMP_GREATEREQUAL:
    return WMTCompareFunctionGreaterEqual;
  case D3DCMP_ALWAYS:
    return WMTCompareFunctionAlways;
  default:
    return WMTCompareFunctionNever;
  }
}

// D3DSTENCILOP_* -> WMTStencilOperation. D3D9 INCRSAT/DECRSAT clamp at
// 0/0xFF; INCR/DECR wrap. Metal mirrors the same split (Clamp vs Wrap
// suffix), so the translation is 1:1; invalid -> Keep matches DXVK's
// DecodeStencilOp default.
inline WMTStencilOperation
to_mtl_stencil_op(DWORD d3dOp) {
  switch (d3dOp) {
  case D3DSTENCILOP_KEEP:
    return WMTStencilOperationKeep;
  case D3DSTENCILOP_ZERO:
    return WMTStencilOperationZero;
  case D3DSTENCILOP_REPLACE:
    return WMTStencilOperationReplace;
  case D3DSTENCILOP_INCRSAT:
    return WMTStencilOperationIncrementClamp;
  case D3DSTENCILOP_DECRSAT:
    return WMTStencilOperationDecrementClamp;
  case D3DSTENCILOP_INVERT:
    return WMTStencilOperationInvert;
  case D3DSTENCILOP_INCR:
    return WMTStencilOperationIncrementWrap;
  case D3DSTENCILOP_DECR:
    return WMTStencilOperationDecrementWrap;
  default:
    return WMTStencilOperationKeep;
  }
}

// Stencil: enable gates fill; TWOSIDEDSTENCILMODE selects front-vs-CCW
// sources. The reference value is encoder-scoped, not descriptor.
// D3DZB_USEW is treated as enabled (Metal has no W buffer, same as both
// refs). dsHasStencil is the aspect of the bound DS: an app can leave
// STENCILENABLE=TRUE (stale from a stencil pass) then bind a depth-only
// surface (D16/D24X8/D32), and a stencil-enabled MTLDepthStencilState
// against a render pass with no stencil attachment is a Metal validation
// failure. Both refs drop the stencil test when the attachment lacks the
// aspect (VK pipelines normalize it away, GL has no stencil bits), so gate
// the stencil ops on dsHasStencil. The write-mediation twin
// (resolved_ds_has_stencil for stencil_write) already models this rule.
inline WMTDepthStencilInfo
depth_stencil_info_from_d3d9_state(const DWORD *renderStates, bool dsAttached, bool dsHasStencil) {
  WMTDepthStencilInfo info{};
  bool z_enabled = dsAttached && renderStates[D3DRS_ZENABLE] != D3DZB_FALSE;
  if (z_enabled) {
    info.depth_compare_function = to_mtl_compare_func(renderStates[D3DRS_ZFUNC]);
    info.depth_write_enabled = renderStates[D3DRS_ZWRITEENABLE] != FALSE;
  } else {
    info.depth_compare_function = WMTCompareFunctionAlways;
    info.depth_write_enabled = false;
  }

  bool stencil_enabled = dsAttached && dsHasStencil && renderStates[D3DRS_STENCILENABLE] != FALSE;
  if (stencil_enabled) {
    uint8_t read_mask = static_cast<uint8_t>(renderStates[D3DRS_STENCILMASK] & 0xFF);
    uint8_t write_mask = static_cast<uint8_t>(renderStates[D3DRS_STENCILWRITEMASK] & 0xFF);

    info.front_stencil.enabled = true;
    info.front_stencil.stencil_compare_function = to_mtl_compare_func(renderStates[D3DRS_STENCILFUNC]);
    info.front_stencil.stencil_fail_op = to_mtl_stencil_op(renderStates[D3DRS_STENCILFAIL]);
    info.front_stencil.depth_fail_op = to_mtl_stencil_op(renderStates[D3DRS_STENCILZFAIL]);
    info.front_stencil.depth_stencil_pass_op = to_mtl_stencil_op(renderStates[D3DRS_STENCILPASS]);
    info.front_stencil.read_mask = read_mask;
    info.front_stencil.write_mask = write_mask;

    info.back_stencil.enabled = true;
    info.back_stencil.read_mask = read_mask;
    info.back_stencil.write_mask = write_mask;
    if (renderStates[D3DRS_TWOSIDEDSTENCILMODE]) {
      info.back_stencil.stencil_compare_function = to_mtl_compare_func(renderStates[D3DRS_CCW_STENCILFUNC]);
      info.back_stencil.stencil_fail_op = to_mtl_stencil_op(renderStates[D3DRS_CCW_STENCILFAIL]);
      info.back_stencil.depth_fail_op = to_mtl_stencil_op(renderStates[D3DRS_CCW_STENCILZFAIL]);
      info.back_stencil.depth_stencil_pass_op = to_mtl_stencil_op(renderStates[D3DRS_CCW_STENCILPASS]);
    } else {
      info.back_stencil.stencil_compare_function = info.front_stencil.stencil_compare_function;
      info.back_stencil.stencil_fail_op = info.front_stencil.stencil_fail_op;
      info.back_stencil.depth_fail_op = info.front_stencil.depth_fail_op;
      info.back_stencil.depth_stencil_pass_op = info.front_stencil.depth_stencil_pass_op;
    }
  }
  return info;
}

// D3D9 blend ops are single-instance; write mask is per-RT
// (COLORWRITEENABLE[0..3]). D3DBLEND_BOTH* on SRCBLEND forces a DESTBLEND
// override (the wined3d/DXVK FixupBlendState pattern). Legacy DX6 pattern,
// only valid on SRCBLEND.
inline void
fixup_d3d9_blend_pair(DWORD &src, DWORD &dst) {
  if (src == D3DBLEND_BOTHSRCALPHA) {
    src = D3DBLEND_SRCALPHA;
    dst = D3DBLEND_INVSRCALPHA;
  } else if (src == D3DBLEND_BOTHINVSRCALPHA) {
    src = D3DBLEND_INVSRCALPHA;
    dst = D3DBLEND_SRCALPHA;
  }
}

inline void
apply_blend_state_to_attachment(
    WMTColorAttachmentBlendInfo &att, const DWORD *renderStates, DWORD writeMaskDw, bool dualSourceActive,
    bool alphaIsOne
) {
  att.blending_enabled = renderStates[D3DRS_ALPHABLENDENABLE] != FALSE;
  att.rgb_blend_operation = to_mtl_blend_op(renderStates[D3DRS_BLENDOP]);
  DWORD src_rgb = renderStates[D3DRS_SRCBLEND];
  DWORD dst_rgb = renderStates[D3DRS_DESTBLEND];
  fixup_d3d9_blend_pair(src_rgb, dst_rgb);
  att.src_rgb_blend_factor = to_mtl_blend_factor(src_rgb);
  att.dst_rgb_blend_factor = to_mtl_blend_factor(dst_rgb);
  if (renderStates[D3DRS_SEPARATEALPHABLENDENABLE]) {
    att.alpha_blend_operation = to_mtl_blend_op(renderStates[D3DRS_BLENDOPALPHA]);
    DWORD src_a = renderStates[D3DRS_SRCBLENDALPHA];
    DWORD dst_a = renderStates[D3DRS_DESTBLENDALPHA];
    fixup_d3d9_blend_pair(src_a, dst_a);
    att.src_alpha_blend_factor = to_mtl_blend_factor(src_a);
    att.dst_alpha_blend_factor = to_mtl_blend_factor(dst_a);
  } else {
    att.alpha_blend_operation = att.rgb_blend_operation;
    att.src_alpha_blend_factor = att.src_rgb_blend_factor;
    att.dst_alpha_blend_factor = att.dst_rgb_blend_factor;
  }
  // A render target with no alpha channel reads its destination alpha as a
  // constant 1.0, so the destination-alpha factors degenerate: DESTALPHA ->
  // ONE and INVDESTALPHA -> ZERO. dxmt backs the X-formats on Metal formats
  // that keep a live alpha byte, which the blend unit would otherwise sample
  // instead of that spec-mandated 1.0, so the normalisation is done here.
  // Applies to the colour and alpha factors alike. Reference: d9vk
  // d3d9_device.cpp (m_alphaSwizzleRTs / NormalizeFactor).
  if (alphaIsOne) {
    auto normalize_dst_alpha = [](WMTBlendFactor f) {
      if (f == WMTBlendFactorDestinationAlpha)
        return WMTBlendFactorOne;
      if (f == WMTBlendFactorOneMinusDestinationAlpha)
        return WMTBlendFactorZero;
      return f;
    };
    att.src_rgb_blend_factor = normalize_dst_alpha(att.src_rgb_blend_factor);
    att.dst_rgb_blend_factor = normalize_dst_alpha(att.dst_rgb_blend_factor);
    att.src_alpha_blend_factor = normalize_dst_alpha(att.src_alpha_blend_factor);
    att.dst_alpha_blend_factor = normalize_dst_alpha(att.dst_alpha_blend_factor);
  }
  // Source1 factors are only legal when the PS variant exports the index(1)
  // output; a Metal PSO with SRC1 factors and no second color index fails
  // pipeline creation. When the dual-source variant isn't active (PS never
  // writes oC1, or blending is off with stale SRC1 factors), fold them to
  // what a declared-but-unwritten oC1 would read: our translator zero-fills
  // the output aggregate, so SRC1COLOR is 0 and 1-SRC1COLOR is 1. DXVK never
  // hits this case; Vulkan keeps the pipeline linkable and DXVK patches the
  // FS to export index 1 whenever SRC1 factors are bound (dxvk_graphics.cpp).
  // Folding the factors instead keeps the PSO legal without forking a variant.
  if (!dualSourceActive) {
    auto normalize_src1 = [](WMTBlendFactor f) {
      if (f == WMTBlendFactorSource1Color)
        return WMTBlendFactorZero;
      if (f == WMTBlendFactorOneMinusSource1Color)
        return WMTBlendFactorOne;
      return f;
    };
    att.src_rgb_blend_factor = normalize_src1(att.src_rgb_blend_factor);
    att.dst_rgb_blend_factor = normalize_src1(att.dst_rgb_blend_factor);
    att.src_alpha_blend_factor = normalize_src1(att.src_alpha_blend_factor);
    att.dst_alpha_blend_factor = normalize_src1(att.dst_alpha_blend_factor);
  }
  att.write_mask = to_mtl_write_mask(writeMaskDw);
}

} // namespace dxmt
