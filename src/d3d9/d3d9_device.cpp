#include "d3d9_device.hpp"

#include "airconv_public.h"
#include "d3d9_buffer.hpp"
#include "d3d9_cube_texture.hpp"
#include "d3d9_volume_texture.hpp"
#include "d3d9_format.hpp"
#include "d3d9_fvf.hpp"
#include "d3d9_interface.hpp"
#include "d3d9_query.hpp"
#include "d3d9_shader.hpp"
#include "d3d9_state_defaults.hpp"
#include "d3d9_texture_upload.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_state_block.hpp"
#include "d3d9_swapchain.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_validation.hpp"
#include "d3d9_vertex_declaration.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "dxso_header.hpp"
#include "log/log.hpp"
#include "wsi_platform.hpp"
#include "wsi_window.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include "com/com_pointer.hpp"

namespace dxmt {

// Size parity between MTLD3D9Device's calling-thread shadow and the
// encode-thread D9EncodingState. The state struct lives in its own
// header to keep the include surface small; these asserts catch any
// drift if either side's array bounds change.
static_assert(D9ES_MAX_TEXTURE_UNITS == D3D9_MAX_TEXTURE_UNITS, "");
static_assert(D9ES_MAX_VERTEX_STREAMS == D3D9_MAX_VERTEX_STREAMS, "");
static_assert(D9ES_MAX_VS_CONST_F == D3D9_MAX_VS_CONST_F, "");
static_assert(D9ES_MAX_VS_CONST_I == D3D9_MAX_VS_CONST_I, "");
static_assert(D9ES_MAX_VS_CONST_B == D3D9_MAX_VS_CONST_B, "");
static_assert(D9ES_MAX_PS_CONST_F == D3D9_MAX_PS_CONST_F, "");
static_assert(D9ES_MAX_PS_CONST_I == D3D9_MAX_PS_CONST_I, "");
static_assert(D9ES_MAX_PS_CONST_B == D3D9_MAX_PS_CONST_B, "");

namespace {

// D3DDECLTYPE → MTLAttributeFormat (dxbc_signature.cpp mirrors the table).
// D3DCOLOR legacy 0xAARRGGBB layout matches Metal's UChar4Normalized_BGRA.
// UDEC3 / DEC3N packed formats need custom unpack; not covered.
inline uint32_t
to_mtl_attr_format(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return 28; // Float
  case D3DDECLTYPE_FLOAT2:
    return 29; // Float2
  case D3DDECLTYPE_FLOAT3:
    return 30; // Float3
  case D3DDECLTYPE_FLOAT4:
    return 31; // Float4
  case D3DDECLTYPE_D3DCOLOR:
    return 42; // UChar4Normalized_BGRA
  case D3DDECLTYPE_UBYTE4:
    return 3; // UChar4
  case D3DDECLTYPE_SHORT2:
    return 16; // Short2
  case D3DDECLTYPE_SHORT4:
    return 18; // Short4
  case D3DDECLTYPE_UBYTE4N:
    return 9; // UChar4Normalized
  case D3DDECLTYPE_SHORT2N:
    return 22; // Short2Normalized
  case D3DDECLTYPE_SHORT4N:
    return 24; // Short4Normalized
  case D3DDECLTYPE_USHORT2N:
    return 19; // UShort2Normalized
  case D3DDECLTYPE_USHORT4N:
    return 21; // UShort4Normalized
  case D3DDECLTYPE_FLOAT16_2:
    return 25; // Half2
  case D3DDECLTYPE_FLOAT16_4:
    return 27; // Half4
  case D3DDECLTYPE_DEC3N:
    // 10-10-10-2 signed normalized: Metal's Int1010102Normalized is
    // exact: 10-bit signed integer x/y/z normalized to [-1, 1] + 2-bit
    // signed w. Game engines pack tangent-space vectors here.
    return 40; // Int1010102Normalized
  case D3DDECLTYPE_UDEC3:
    // 10-10-10-2 unsigned UNnormalized per D3D9 spec. Metal has no
    // unnormalized 10-bit attribute format, so apps that wrote
    // x in [0,1023] read x in [0,1]. DXVK keeps the raw values via
    // Vulkan's USCALED format; Metal has no equivalent. Accepted gap.
    return 41; // UInt1010102Normalized
  default:
    return 0; // Invalid
  }
}

// D3DPRIMITIVETYPE → Metal primitive class. The Metal type enum is at
// winemetal.h; the D3D9 enum is contiguous from 1
// (D3DPT_POINTLIST). D3D9 fans have no Metal equivalent; the entry
// points emulate them with an index-buffer rewrite that reaches the
// Resolve/EmitDrawBatch path as TRIANGLELIST, so the encoder never
// sees a fan.
inline WMTPrimitiveType
to_mtl_prim_type(D3DPRIMITIVETYPE pt) {
  switch (pt) {
  case D3DPT_POINTLIST:
    return WMTPrimitiveTypePoint;
  case D3DPT_LINELIST:
    return WMTPrimitiveTypeLine;
  case D3DPT_LINESTRIP:
    return WMTPrimitiveTypeLineStrip;
  case D3DPT_TRIANGLELIST:
    return WMTPrimitiveTypeTriangle;
  case D3DPT_TRIANGLESTRIP:
    return WMTPrimitiveTypeTriangleStrip;
  default:
    return WMTPrimitiveTypeTriangle; // unreachable
  }
}

// PrimitiveCount → vertex count (MGL/wined3d use the same arithmetic).
// MaxPrimitiveCount is advertised via D3DCAPS9 only; neither wined3d
// (device.c d3d9_device_DrawPrimitive) nor DXVK (d3d9_device.cpp
// D3D9DeviceEx::DrawPrimitive) rejects a draw whose PrimitiveCount
// exceeds the cap, so no max-count gate lives here.
//
// TRIANGLEFAN matches TRIANGLESTRIP's vertex-count formula (count + 2);
// the entry points fold the fan into a list before the encoder so the
// per-prim arithmetic still uses the original D3D9 vertex count.
inline UINT
prim_to_vertex_count(D3DPRIMITIVETYPE pt, UINT count) {
  switch (pt) {
  case D3DPT_POINTLIST:
    return count;
  case D3DPT_LINELIST:
    return count * 2;
  case D3DPT_LINESTRIP:
    return count + 1;
  case D3DPT_TRIANGLELIST:
    return count * 3;
  case D3DPT_TRIANGLESTRIP:
    return count + 2;
  case D3DPT_TRIANGLEFAN:
    return count + 2;
  default:
    return 0;
  }
}

// D3D9 fan with N verts yields N-2 triangles: (0, k+1, k+2).
// Metal has no fan; synthesise index list as TRIANGLELIST.
// src may be null (generate 0..N-1) or u16/u32 array; src_idx_size ∈ {0,2,4}.
inline void
fill_fan_to_list_indices(uint32_t *dst, UINT prim_count, const void *src, uint32_t src_idx_size) {
  if (src == nullptr) {
    for (UINT k = 0; k < prim_count; ++k) {
      dst[k * 3 + 0] = 0;
      dst[k * 3 + 1] = k + 1;
      dst[k * 3 + 2] = k + 2;
    }
  } else if (src_idx_size == 2) {
    auto *s = static_cast<const uint16_t *>(src);
    for (UINT k = 0; k < prim_count; ++k) {
      dst[k * 3 + 0] = s[0];
      dst[k * 3 + 1] = s[k + 1];
      dst[k * 3 + 2] = s[k + 2];
    }
  } else {
    auto *s = static_cast<const uint32_t *>(src);
    for (UINT k = 0; k < prim_count; ++k) {
      dst[k * 3 + 0] = s[0];
      dst[k * 3 + 1] = s[k + 1];
      dst[k * 3 + 2] = s[k + 2];
    }
  }
}

// D3DTADDRESS_* → WMTSamplerAddressMode. wined3d state.c
// sampler_texaddress shares this table; MGL's GL→Metal address-mode
// lowering matches Metal one-to-one (clamp/repeat/mirror have direct
// equivalents). MIRRORONCE = clamp-after-one-mirror = Metal's
// MirrorClampToEdge.
inline WMTSamplerAddressMode
to_mtl_address_mode(DWORD d3dMode) {
  switch (d3dMode) {
  case D3DTADDRESS_WRAP:
    return WMTSamplerAddressModeRepeat;
  case D3DTADDRESS_MIRROR:
    return WMTSamplerAddressModeMirrorRepeat;
  case D3DTADDRESS_CLAMP:
    return WMTSamplerAddressModeClampToEdge;
  case D3DTADDRESS_BORDER:
    return WMTSamplerAddressModeClampToBorderColor;
  case D3DTADDRESS_MIRRORONCE:
    return WMTSamplerAddressModeMirrorClampToEdge;
  default:
    return WMTSamplerAddressModeClampToEdge;
  }
}

// D3DTEXF_* min/mag → Metal min/mag filter. Anisotropic in D3D9 picks
// linear sampling underneath, with the anisotropy level coming from
// D3DSAMP_MAXANISOTROPY: same shape as DXVK d3d9_state.cpp DecodeFilter.
inline WMTSamplerMinMagFilter
to_mtl_minmag_filter(DWORD d3dFilter) {
  switch (d3dFilter) {
  case D3DTEXF_POINT:
    return WMTSamplerMinMagFilterNearest;
  case D3DTEXF_LINEAR:
  case D3DTEXF_ANISOTROPIC:
    return WMTSamplerMinMagFilterLinear;
  default:
    return WMTSamplerMinMagFilterNearest;
  }
}

// D3DTEXF_* mip → Metal mip filter. NONE collapses the chain to level 0
// (Metal's NotMipmapped), POINT and LINEAR map straight across.
inline WMTSamplerMipFilter
to_mtl_mip_filter(DWORD d3dFilter) {
  switch (d3dFilter) {
  case D3DTEXF_NONE:
    return WMTSamplerMipFilterNotMipmapped;
  case D3DTEXF_POINT:
    return WMTSamplerMipFilterNearest;
  case D3DTEXF_LINEAR:
    return WMTSamplerMipFilterLinear;
  default:
    return WMTSamplerMipFilterNotMipmapped;
  }
}

// D3D9 clockwise = front; CCW = back. Spec default: D3DCULL_CCW = cull back.
// Pin Metal's frontFacingWinding to Clockwise; map D3DCULL_* to cullMode.
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
    return WMTCullModeBack; // D3D9 default
  }
}

// D3DFILL_POINT has no Metal equivalent (Metal only exposes Fill /
// Lines for triangle fill); apps that set it land back on solid fill.
// DXVK does the same; Vulkan also lacks a per-tri POINT fill on the
// graphics pipeline.
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

// D3DBLEND_* → WMTBlendFactor. BOTHSRCALPHA / BOTHINVSRCALPHA are
// legacy combined-blend modes that overwrite the dest factor; they're
// expanded by fixup_d3d9_blend_pair before reaching this mapper and
// thus shouldn't appear here. SRCCOLOR2 / INVSRCCOLOR2 are dual-source
// blending: Metal supports them via Source1 factors.
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
    return WMTBlendFactorSourceAlpha;
  case D3DBLEND_INVSRCALPHA:
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
    return WMTBlendFactorOne;
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

// D3DCOLORWRITEENABLE_* (RED=1, GREEN=2, BLUE=4, ALPHA=8) → Metal
// WMTColorWriteMask. The bit values disagree (Metal's enum lists
// Red=8, Green=4, Blue=2, Alpha=1: the reverse byte order), so we
// remap rather than aliasing; bit-mismatch silently writes wrong
// channels to the RT.
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

// D3DCMP_* → WMTCompareFunction. Same enum order as Vulkan's
// VkCompareOp; DXVK src/d3d9/d3d9_state.cpp DecodeCompareOp uses the
// same translation against Vulkan, and Metal mirrors Vulkan here.
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
    return WMTCompareFunctionAlways;
  }
}

// D3DSTENCILOP_* → WMTStencilOperation. D3D9 INCRSAT/DECRSAT clamp at
// 0/0xFF; INCR/DECR wrap. Metal mirrors the same split (Clamp vs Wrap
// suffix), so the translation is 1:1.
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

// D3D9 has no view objects, so each surface op synthesizes its Metal view
// inline: keep the multisample bit derived from the texture in one place
// (d3d11/d3d12 do the equivalent in their own view helpers).
inline WMTTextureType
surface_view_type(const dxmt::Texture *texture) {
  return texture->sampleCount() > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
}

// Stencil: enable gates fill; TWOSIDEDSTENCILMODE selects front-vs-CCW sources.
// Reference value is encoder-scoped, not descriptor. D3DZB_USEW treated as enabled.
inline WMTDepthStencilInfo
depth_stencil_info_from_d3d9_state(const DWORD *renderStates, bool dsAttached) {
  WMTDepthStencilInfo info{};
  bool z_enabled = dsAttached && renderStates[D3DRS_ZENABLE] != D3DZB_FALSE;
  if (z_enabled) {
    info.depth_compare_function = to_mtl_compare_func(renderStates[D3DRS_ZFUNC]);
    info.depth_write_enabled = renderStates[D3DRS_ZWRITEENABLE] != FALSE;
  } else {
    info.depth_compare_function = WMTCompareFunctionAlways;
    info.depth_write_enabled = false;
  }

  bool stencil_enabled = dsAttached && renderStates[D3DRS_STENCILENABLE] != FALSE;
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

// D3D9 blend ops are single-instance; write mask is per-RT (COLORWRITEENABLE[0..3]).
// D3DBLEND_BOTH* on SRCBLEND forces DESTBLEND override (wined3d/DXVK pattern).
// Legacy DX6 pattern; not valid on actual DESTBLEND writes.
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
    WMTColorAttachmentBlendInfo &att, const DWORD *renderStates, DWORD writeMaskDw, bool rtAlphaReadsOne,
    bool dualSourceActive
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
  // Alpha-less D3D formats land on Metal formats with a live alpha
  // byte, so destination-alpha factors would read stale writes instead
  // of the 1.0 the format defines. DXVK normalises these factors per
  // attachment; wined3d gets it free from GL's RGBX storage.
  if (rtAlphaReadsOne) {
    auto normalize = [](WMTBlendFactor f) {
      if (f == WMTBlendFactorDestinationAlpha)
        return WMTBlendFactorOne;
      if (f == WMTBlendFactorOneMinusDestinationAlpha)
        return WMTBlendFactorZero;
      return f;
    };
    att.src_rgb_blend_factor = normalize(att.src_rgb_blend_factor);
    att.dst_rgb_blend_factor = normalize(att.dst_rgb_blend_factor);
    att.src_alpha_blend_factor = normalize(att.src_alpha_blend_factor);
    att.dst_alpha_blend_factor = normalize(att.dst_alpha_blend_factor);
  }
  // Source1 factors are only legal when the PS variant exports the
  // index(1) output; a Metal PSO with SRC1 factors and no second color
  // index fails pipeline creation. When the dual-source variant isn't
  // active (PS never writes oC1, or blending is off with stale SRC1
  // factors), fold them to what a declared-but-unwritten oC1 would
  // read: our translator zero-fills the output aggregate, so SRC1COLOR
  // is 0 and 1-SRC1COLOR is 1. DXVK never hits this case; Vulkan keeps
  // the pipeline linkable and DXVK patches the FS to export index 1
  // whenever SRC1 factors are bound (dxvk_graphics.cpp). Folding the
  // factors instead keeps the PSO legal without forking a variant.
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

// Per-RT D3DRS_COLORWRITEENABLE row. Order matches m_renderTargets[]
// (RT 0..3); index 0 reuses the original D3DRS_COLORWRITEENABLE slot
// per D3D9 spec.
constexpr D3DRENDERSTATETYPE kColorWriteEnableRS[D3D_MAX_SIMULTANEOUS_RENDERTARGETS] = {
    D3DRS_COLORWRITEENABLE,
    D3DRS_COLORWRITEENABLE1,
    D3DRS_COLORWRITEENABLE2,
    D3DRS_COLORWRITEENABLE3,
};

// Translate one D3D9 sampler-stage state row into a Metal
// WMTSamplerInfo. The D3D9 stage is m_samplerStates[slot]; the row is
// indexed by D3DSAMP_* (1..13). LOD-bias and SRGB sampling don't have
// direct Metal-sampler equivalents and are handled at sample-site time
// (TexLdb / format-aware texture creation), not in the sampler object.
inline WMTSamplerInfo
sampler_info_from_d3d9_state(const DWORD *state, bool shadow_compare = false) {
  WMTSamplerInfo info{};
  info.s_address_mode = to_mtl_address_mode(state[D3DSAMP_ADDRESSU]);
  info.t_address_mode = to_mtl_address_mode(state[D3DSAMP_ADDRESSV]);
  info.r_address_mode = to_mtl_address_mode(state[D3DSAMP_ADDRESSW]);
  info.mag_filter = to_mtl_minmag_filter(state[D3DSAMP_MAGFILTER]);
  info.min_filter = to_mtl_minmag_filter(state[D3DSAMP_MINFILTER]);
  info.mip_filter = to_mtl_mip_filter(state[D3DSAMP_MIPFILTER]);
  // Border-color quantization: Metal exposes only three border colors
  // (transparent black / opaque black / opaque white). Apps that
  // depend on a specific tinted border get the closest match; we
  // pick by the alpha and luma of the D3DCOLOR.
  DWORD bc = state[D3DSAMP_BORDERCOLOR];
  uint8_t a = (bc >> 24) & 0xFF;
  uint8_t r = (bc >> 16) & 0xFF;
  uint8_t g = (bc >> 8) & 0xFF;
  uint8_t b = (bc) & 0xFF;
  if (a < 0x80)
    info.border_color = WMTSamplerBorderColorTransparentBlack;
  else if ((uint32_t)r + g + b > 0x80 * 3)
    info.border_color = WMTSamplerBorderColorOpaqueWhite;
  else
    info.border_color = WMTSamplerBorderColorOpaqueBlack;
  // Hardware-PCF shadow stages get a LessEqual compare (D3D9 HW shadow
  // maps are fixed LessEqual; there is no D3DSAMP state to pick the
  // func; binding a depth texture implies it, matching wined3d's
  // GL_LEQUAL). Non-shadow stages stay Always (the sample op ignores it).
  // The sampler cache key folds compare_function, so a shadow and a
  // non-shadow sampler over the same D3DSAMP state get distinct entries.
  info.compare_function = shadow_compare ? WMTCompareFunctionLessEqual : WMTCompareFunctionAlways;
  // D3DSAMP_MAXMIPLEVEL: largest mip the sampler is allowed to use
  // (0 = full detail, N = skip first N levels). Metal's lod_min_clamp
  // has the same direction. wined3d state.c sampler_lod_min_max wires
  // it 1:1, and DXVK folds D3DSAMP_MAXMIPLEVEL into its sampler key as
  // the min-LOD clamp. Without this the sampler always reaches the
  // largest mip even when the app asked for a lower-detail view, which
  // silently overrides D3DXCreateTextureFromFile's "skip top N"
  // behaviour.
  info.lod_min_clamp = static_cast<float>(state[D3DSAMP_MAXMIPLEVEL]);
  info.lod_max_clamp = FLT_MAX;
  uint32_t aniso = state[D3DSAMP_MAXANISOTROPY];
  if (aniso < 1)
    aniso = 1;
  if (aniso > 16)
    aniso = 16;
  info.max_anisotroy = aniso;
  info.normalized_coords = true;
  info.lod_average = false;
  info.support_argument_buffers = false;
  return info;
}

// D3D9 pixel centers at integers; Metal at (i+0.5, j+0.5). Shift viewport +0.5.
// No Y-flip needed (Metal NDC y-axis matches D3D9).
// Clamp MinZ/MaxZ to [0,1]; re-enforce strict ordering with 1/65536 nudge.
inline WMTViewport
wmt_viewport_from_d3d9(const D3DVIEWPORT9 &vp) {
  WMTViewport out;
  out.originX = static_cast<double>(vp.X) + 0.5;
  out.originY = static_cast<double>(vp.Y) + 0.5;
  out.width = static_cast<double>(vp.Width);
  out.height = static_cast<double>(vp.Height);
  out.znear = std::clamp(static_cast<double>(vp.MinZ), 0.0, 1.0);
  out.zfar = std::clamp(static_cast<double>(vp.MaxZ), 0.0, 1.0);
  if (out.zfar <= out.znear)
    out.zfar = std::min(1.0, out.znear + 1.0 / 65536.0);
  return out;
}

// D3D9 has scissor rect + enable flag; Metal lacks "off" mode.
// When disabled, set scissor to viewport bounds. Always intersect rect vs viewport.
inline WMTScissorRect
wmt_scissor_from_d3d9(const RECT &sr, const D3DVIEWPORT9 &vp, bool scissor_enabled) {
  const int64_t vp_l = static_cast<int64_t>(vp.X);
  const int64_t vp_t = static_cast<int64_t>(vp.Y);
  const int64_t vp_r = vp_l + static_cast<int64_t>(vp.Width);
  const int64_t vp_b = vp_t + static_cast<int64_t>(vp.Height);
  int64_t l = vp_l, t = vp_t, r = vp_r, b = vp_b;
  if (scissor_enabled) {
    l = std::max<int64_t>(vp_l, sr.left);
    t = std::max<int64_t>(vp_t, sr.top);
    r = std::min<int64_t>(vp_r, sr.right);
    b = std::min<int64_t>(vp_b, sr.bottom);
    if (r < l)
      r = l;
    if (b < t)
      b = t;
  }
  WMTScissorRect out;
  out.x = static_cast<uint64_t>(l);
  out.y = static_cast<uint64_t>(t);
  out.width = static_cast<uint64_t>(r - l);
  out.height = static_cast<uint64_t>(b - t);
  return out;
}

// IEC 61966-2-1 piecewise linear→sRGB encode for clear-color stamping
// when an RT attachment uses an sRGB-format pixel-format view. Metal's
// MTLRenderPassAttachmentDescriptor.clearColor writes raw storage and
// bypasses the view's encode (observed on Apple GPUs), so the
// app's linear D3DCOLOR would land on disk as if it were already sRGB-
// encoded; visible as a tinted band wherever Clear meets a draw.
// Pre-encode here so cleared pixels match drawn pixels in storage.
inline float
encode_srgb_channel(float c) {
  // Apps occasionally pass slightly-negative or HDR-ish floats through
  // Clear; pow(negative, 1/2.4) returns NaN. wined3d / DXVK clamp first.
  c = std::clamp(c, 0.0f, 1.0f);
  if (c <= 0.0031308f)
    return 12.92f * c;
  return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// D3DMULTISAMPLE_TYPE (+ quality) → Metal sampleCount + accept/reject
// HRESULT. Keeps CreateRenderTarget / CreateDepthStencilSurface in
// lock-step with MTLD3D9Interface::CheckDeviceMultiSampleType: the same
// supportsTextureSampleCount probe runs at probe time and at create
// time, so apps that see the probe accept 8x on M2+/M3 GPUs don't then
// get NOTAVAILABLE from the create call. D3DMULTISAMPLE_NONMASKABLE
// reads the quality level as a sample-count selector (1 << quality),
// the same mapping DXVK uses in GetSampleCount (d3d9_util.cpp), rather
// than rejecting it. Returns (sample_count=1, INVALIDCALL) on
// out-of-enum, (1, NOTAVAILABLE) on in-enum-but-unsupported,
// (N, S_OK) on success.
inline std::pair<uint32_t, HRESULT>
multisample_type_to_metal_sample_count(D3DMULTISAMPLE_TYPE ms, DWORD quality, WMT::Device device) {
  auto check = [&](uint32_t n) -> std::pair<uint32_t, HRESULT> {
    if (n == 1)
      return {1u, D3D_OK};
    return device.supportsTextureSampleCount(static_cast<uint8_t>(n))
               ? std::make_pair(n, static_cast<HRESULT>(S_OK))
               : std::make_pair(1u, static_cast<HRESULT>(D3DERR_NOTAVAILABLE));
  };
  switch (ms) {
  case D3DMULTISAMPLE_NONE:
    return {1u, D3D_OK};
  case D3DMULTISAMPLE_2_SAMPLES:
  case D3DMULTISAMPLE_4_SAMPLES:
  case D3DMULTISAMPLE_8_SAMPLES:
  case D3DMULTISAMPLE_16_SAMPLES:
    return check(static_cast<uint32_t>(ms));
  // NONMASKABLE picks the sample count from the quality level: quality N
  // selects 1 << N samples. Out-of-range quality (count > 16) is the
  // app's error, so reject with INVALIDCALL like DXVK's GetSampleCount.
  case D3DMULTISAMPLE_NONMASKABLE: {
    if (quality > 4)
      return {1u, static_cast<HRESULT>(D3DERR_INVALIDCALL)};
    return check(1u << quality);
  }
  case D3DMULTISAMPLE_3_SAMPLES:
  case D3DMULTISAMPLE_5_SAMPLES:
  case D3DMULTISAMPLE_6_SAMPLES:
  case D3DMULTISAMPLE_7_SAMPLES:
  case D3DMULTISAMPLE_9_SAMPLES:
  case D3DMULTISAMPLE_10_SAMPLES:
  case D3DMULTISAMPLE_11_SAMPLES:
  case D3DMULTISAMPLE_12_SAMPLES:
  case D3DMULTISAMPLE_13_SAMPLES:
  case D3DMULTISAMPLE_14_SAMPLES:
  case D3DMULTISAMPLE_15_SAMPLES:
    return {1u, static_cast<HRESULT>(D3DERR_NOTAVAILABLE)};
  default:
    return {1u, static_cast<HRESULT>(D3DERR_INVALIDCALL)};
  }
}

} // namespace

// Async PSO compile: mirrors d3d11 MTLCompiledGraphicsPipelineImpl.
// Calls newRenderPipelineState off-thread; signals ready bit when done.
// Device-lifetime cache holds completed PSOs; Com<> retains in-flight ones.
class D3D9PsoCompileTask {
public:
  D3D9PsoCompileTask(
      WMT::Device device, Com<MTLD3D9VertexShader, false> vs, Com<MTLD3D9PixelShader, false> ps,
      const WMTRenderPipelineInfo &info
  ) :
      m_device(device),
      m_vs(std::move(vs)),
      m_ps(std::move(ps)),
      m_info(info) {}

  // Worker entry. Synchronously calls newRenderPipelineState; on failure
  // m_state stays null and m_error captures the NSError description for
  // post-mortem (the negative-cache lookup below picks this up).
  D3D9PsoCompileTask *
  RunTask() {
    WMT::Reference<WMT::Error> err;
    m_state = m_device.newRenderPipelineState(m_info, err);
    if (!m_state)
      m_error = err ? err.description().getUTF8String() : std::string("(no NSError)");
    return this; // signals "task complete" to the scheduler
  }

  // task_trait hooks. atomic_bool.notify_all wakes Wait() callers.
  bool
  GetDone() const noexcept {
    return m_ready.load(std::memory_order_acquire);
  }
  void
  SetDone(bool s) noexcept {
    m_ready.store(s, std::memory_order_release);
    m_ready.notify_all();
  }

  // Block the calling thread until the worker has finished. First-draw
  // hits this; subsequent draws against the same key see ready=true and
  // return immediately.
  void
  Wait() const noexcept {
    while (!m_ready.load(std::memory_order_acquire))
      m_ready.wait(false, std::memory_order_acquire);
  }

  WMT::RenderPipelineState
  state() const noexcept {
    return m_state ? WMT::RenderPipelineState{m_state.handle} : WMT::RenderPipelineState{};
  }
  const std::string &
  error() const noexcept {
    return m_error;
  }
  const WMTRenderPipelineInfo &
  info() const noexcept {
    return m_info;
  }

private:
  WMT::Device m_device;
  Com<MTLD3D9VertexShader, false> m_vs;
  Com<MTLD3D9PixelShader, false> m_ps;
  WMTRenderPipelineInfo m_info;
  WMT::Reference<WMT::RenderPipelineState> m_state;
  std::string m_error;
  mutable std::atomic<bool> m_ready{false};
};

// task_trait specialisation methods. Out-of-class so the bodies live in
// a single TU; visible to the device ctor below where m_psoScheduler is
// constructed (which instantiates task_scheduler<D3D9PsoCompileTask*>::
// worker_func, which calls these). Same shape as
// d3d11_pipeline_cache.cpp.
D3D9PsoCompileTask *
task_trait<D3D9PsoCompileTask *>::run_task(D3D9PsoCompileTask *task) {
  return task->RunTask();
}
bool
task_trait<D3D9PsoCompileTask *>::get_done(D3D9PsoCompileTask *task) {
  return task->GetDone();
}
void
task_trait<D3D9PsoCompileTask *>::set_done(D3D9PsoCompileTask *task) {
  task->SetDone(true);
}

MTLD3D9Device::MTLD3D9Device(
    MTLD3D9Interface *parent, bool isEx, UINT adapter, D3DDEVTYPE deviceType, HWND focusWindow, DWORD behaviorFlags,
    const D3DPRESENT_PARAMETERS &validatedParams, WMT::Reference<WMT::Device> &&metalDevice
) :
    m_parent(parent),
    m_isEx(isEx),
    m_metalDevice(std::move(metalDevice)),
    m_dxmtQueue(std::make_unique<dxmt::CommandQueue>(m_metalDevice)),
    m_internalCmdLib(m_metalDevice),
    m_clearQuad(m_metalDevice, m_internalCmdLib),
    m_constRing(
        {m_metalDevice, static_cast<WMTResourceOptions>(
                            WMTResourceCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                            WMTResourceStorageModeShared
                        )}
    ),
    m_uploadRing(
        {m_metalDevice, static_cast<WMTResourceOptions>(
                            WMTResourceCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                            WMTResourceStorageModeShared
                        )}
    ) {}


void
MTLD3D9Device::resetStateToDefaults(bool enableAutoDepthStencil) {
  // Sampler-state, texture-stage-state and render-state power-on
  // defaults. The tables live in d3d9_state_defaults.cpp as free
  // functions so they can be checked host-native against the reference
  // (wined3d / the Wine d3d9 conformance suite) without a device.
  init_default_sampler_states(m_samplerStates, D3D9_MAX_TEXTURE_UNITS);
  init_default_texture_stage_states(m_textureStageStates, 8);
  initDefaultRenderStates(enableAutoDepthStencil);
  // Transform state defaults: identity matrices everywhere.
  for (uint32_t i = 0; i < kMaxTransforms; ++i) {
    D3DMATRIX m = {};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = m.m[3][3] = 1.0f;
    m_transforms[i] = m;
  }
  // SetStreamSourceFreq defaults to 1 per stream. Push SetRef(null)
  // ops alongside the calling-thread shadow clears so m_encodeSideRefs
  // stays in lockstep with the post-Reset zero-state; without these
  // the encode-side mirror would carry stale refs to surfaces /
  // textures / buffers that the app is about to release through Reset.
  for (uint32_t i = 0; i < D3D9_MAX_VERTEX_STREAMS; ++i) {
    m_streamFreq[i] = 1;
    m_streamOffsets[i] = 0;
    m_streamStrides[i] = 0;
    if (m_vertexBuffers[i].ptr())
      QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::VertexBuffer0 + i), nullptr);
    m_vertexBuffers[i] = nullptr;
  }
  if (m_indexBuffer.ptr())
    QueueRefOp(PendingRefOp::IndexBuffer, nullptr);
  m_indexBuffer = nullptr;
  if (m_vertexDeclaration.ptr())
    QueueRefOp(PendingRefOp::VertexDeclaration, nullptr);
  m_vertexDeclaration = nullptr;
  if (m_vertexShader.ptr())
    QueueRefOp(PendingRefOp::VertexShader, nullptr);
  m_vertexShader = nullptr;
  if (m_pixelShader.ptr())
    QueueRefOp(PendingRefOp::PixelShader, nullptr);
  m_pixelShader = nullptr;
  m_fvf = 0;
  // Bound textures: drop all D3D9_MAX_TEXTURE_UNITS slots (PS + VS).
  for (uint32_t i = 0; i < D3D9_MAX_TEXTURE_UNITS; ++i) {
    if (m_textures[i].ptr())
      QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::Texture0 + i), nullptr);
    m_textures[i] = nullptr;
  }
  // FFP material defaults: wined3d stateblock.c default_material.
  m_material.Diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
  m_material.Ambient = {0.0f, 0.0f, 0.0f, 0.0f};
  m_material.Specular = {0.0f, 0.0f, 0.0f, 0.0f};
  m_material.Emissive = {0.0f, 0.0f, 0.0f, 0.0f};
  m_material.Power = 0.0f;
  // Lights + clip planes: empty.
  m_lights.clear();
  m_lightEnables.clear();
  std::memset(m_clipPlanes, 0, sizeof(m_clipPlanes));
  // VS/PS constants: zero per spec.
  std::memset(m_vsConstantsF, 0, sizeof(m_vsConstantsF));
  std::memset(m_vsConstantsI, 0, sizeof(m_vsConstantsI));
  std::memset(m_vsConstantsB, 0, sizeof(m_vsConstantsB));
  std::memset(m_psConstantsF, 0, sizeof(m_psConstantsF));
  std::memset(m_psConstantsI, 0, sizeof(m_psConstantsI));
  std::memset(m_psConstantsB, 0, sizeof(m_psConstantsB));
  // Reset the const-F coverage trackers so the post-Reset upload
  // clamp starts at minimum again. Sticky-monotonic across the device
  // *between* Resets only.
  m_vsConstFMax = 0;
  m_psConstFMax = 0;
  // POD state is now per-draw via BatchedDraw::pod_snapshot; mark
  // every axis dirty so the next QueueBatchedDraw takes a fresh
  // snapshot off the just-reset shadows.
  m_encShadowDirty = dxmt::D9ES_DIRTY_ALL;
  // REF state lives on the encode-side m_encodeSideRefs mirror. The
  // SetRef(null) ops queued above bump m_encodeSideRefsGen as the
  // encode-thread walker applies them, invalidating stale cluster
  // caches; the walker is that gen's sole writer, so there is no
  // inline bump here.
}

void
MTLD3D9Device::initDefaultRenderStates(bool enableAutoDepthStencil) {
  // D3D9 spec defaults from DXVK reference (same D3DRS_* indexing).
  // Float states stored as IEEE-754 bit pattern; apps Set/Get as DWORD.
  init_default_render_states(m_renderStates, enableAutoDepthStencil);
}

bool
MTLD3D9Device::acquireBufferBacking(
    size_t size, WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned
) {
  for (auto it = m_bufferBackingPool.begin(); it != m_bufferBackingPool.end(); ++it) {
    if (it->capacity == size) {
      out_buffer = std::move(it->buffer);
      out_owned = it->owned_backing;
      out_host = it->owned_backing;
      out_gpu = it->gpu_address;
      m_bufferBackingPool.erase(it);
      return true;
    }
  }
  return false;
}

void
MTLD3D9Device::releaseBufferBacking(
    WMT::Reference<WMT::Buffer> &&buffer, void *owned, uint64_t gpu_address, size_t capacity
) {
  if (m_bufferBackingPool.size() >= kMaxBufferBackingPoolSize) {
    // Pool full: drop on the floor. The moved-in WMT::Reference
    // releases the Metal buffer when it goes out of scope below; we
    // free the wsi backing here.
    buffer = WMT::Reference<WMT::Buffer>{};
    if (owned)
      wsi::aligned_free(owned);
    return;
  }
  BufferBackingPoolEntry entry;
  entry.buffer = std::move(buffer);
  entry.owned_backing = owned;
  entry.gpu_address = gpu_address;
  entry.capacity = capacity;
  m_bufferBackingPool.push_back(std::move(entry));
}

HRESULT
MTLD3D9Device::acquireOrAllocateBufferBacking(
    UINT length, WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned
) {
  if (acquireBufferBacking(length, out_buffer, out_gpu, out_host, out_owned))
    return D3D_OK;
  out_owned = wsi::aligned_malloc(length, DXMT_PAGE_SIZE);
  if (!out_owned)
    return D3DERR_OUTOFVIDEOMEMORY;
  // Pre-fault the placement now. A registered host buffer's first
  // touch pays a steep page-fault cost under 32-bit x86 translation;
  // faulting the whole block up front moves that cost off the draw
  // path that would otherwise hit it lazily.
  std::memset(out_owned, 0, length);
  // A fresh WMTBufferInfo per newBuffer call; reusing one aliases the
  // prior buffer's storage.
  WMTBufferInfo info{};
  info.length = length;
  info.options = WMTResourceStorageModeShared;
  info.memory.set(out_owned);
  out_buffer = m_metalDevice.newBuffer(info);
  if (out_buffer == nullptr) {
    wsi::aligned_free(out_owned);
    out_owned = nullptr;
    return D3DERR_OUTOFVIDEOMEMORY;
  }
  out_gpu = info.gpu_address;
  out_host = out_owned;
  return D3D_OK;
}

MTLD3D9Device::~MTLD3D9Device() {}


// Out-of-line accessor: the inline form would dereference an
// incomplete dxmt::CommandQueue type at every TU that includes
// d3d9_device.hpp. Moving the body here keeps the header's include
// surface light.
WMT::CommandQueue
MTLD3D9Device::commandQueue() const {
  return m_dxmtQueue->commandQueue;
}

dxmt::CommandQueue &
MTLD3D9Device::dxmtQueue() const {
  return *m_dxmtQueue;
}

// Sampler cache lookup. Builds the prefix key from the input info,
// reuses on hit; on miss invokes the dxmt::Sampler factory shared
// with d3d11 (src/dxmt/dxmt_sampler.cpp) and inserts. Insertion
// is unconditional even on factory failure so a repeatedly bad
// descriptor doesn't burn a Metal round-trip every draw.
Rc<Sampler>
MTLD3D9Device::getOrCreateSampler(const WMTSamplerInfo &info) {
  SamplerKey key = samplerKeyFromInfo(info);
  if (auto it = m_samplerCache.find(key); it != m_samplerCache.end())
    return it->second;
  auto sampler = Sampler::createSampler(
      m_metalDevice, info,
      /*lod_bias=*/0.0f
  );
  auto [ins, _] = m_samplerCache.emplace(key, std::move(sampler));
  return ins->second;
}

obj_handle_t
MTLD3D9Device::dummyFragmentTexture2D() {
  // Lazy 1×1 BGRA8 placeholder for fragment sampler slots a PS samples
  // with no app texture bound; see the header comment. Built via the
  // dxmt::Texture path (same shape as CreateOffscreenPlainSurface's NULL
  // RT) so the allocation is owned + kept alive by m_dummyFragTexAlloc.
  // Contents are left uninitialised: a correctly-bound app never samples
  // this, and the transient mis-bound case (post-Reset) just needs a
  // valid resource at the index, not specific texels.
  if (m_dummyFragTexHandle != 0)
    return m_dummyFragTexHandle;
  WMTTextureInfo info{};
  info.pixel_format = WMTPixelFormatBGRA8Unorm;
  info.width = 1;
  info.height = 1;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsageShaderRead;
  info.options = WMTResourceStorageModePrivate;
  Rc<dxmt::Texture> tex = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = tex->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return 0; // creation failed; resolve binds null (no worse than before)
  m_dummyFragTexHandle = allocation->texture().handle;
  tex->rename(std::move(allocation));
  m_dummyFragTexAlloc = std::move(tex);
  return m_dummyFragTexHandle;
}

// DSSO cache lookup. Mirrors the sampler-cache shape above and
// d3d11's StateObjectCache<D3D11_DEPTH_STENCIL_DESC, ...>. WMTDepthStencilInfo
// is the natural key: fully-specified 32-byte POD with no Metal-side
// out-fields to mask. On miss, defer the m_metalDevice.newDepthStencilState
// round-trip; on hit, reuse the WMT::Reference held in the cache.
WMT::DepthStencilState
MTLD3D9Device::getOrCreateDSSO(const WMTDepthStencilInfo &info) {
  DepthStencilKey key{info};
  if (auto it = m_dssoCache.find(key); it != m_dssoCache.end())
    return WMT::DepthStencilState{it->second.handle};
  auto dsso = m_metalDevice.newDepthStencilState(info);
  auto [ins, _] = m_dssoCache.emplace(key, std::move(dsso));
  return WMT::DepthStencilState{ins->second.handle};
}

void
MTLD3D9Device::flushOpenWork() {
  // Drain any pending Clear (D3D9 Clear is eager; apps that issue
  // Clear and then immediately Present / GetRenderTargetData / blit
  // expect the targeted attachments to be wiped) and any deferred
  // mip-gen sweep onto the current chunk. Both calls post chunk
  // lambdas via chunk->emitcc on CurrentChunk(); the chunk's
  // EncodingThread replays them in emit order. No sync cmdbuf is
  // built here: all sync paths now route through chunks.
  drainPendingClear();
  flushDeferredBlitWork();
}

void
MTLD3D9Device::drainPendingClear() {
  // No-op if there's nothing to drain.
  if (!m_pendingClear.color_valid && !m_pendingClear.depth_valid && !m_pendingClear.stencil_valid)
    return;
  // Route through ArgumentEncodingContext clear methods (not empty render pass).
  // dxmt_context coalescer folds Clear→Render into next encoder's loadAction.
  // Fires only when no queued draws AND clear must reach GPU before next sync.
  MTLD3D9Surface *ds = m_depthStencilSurface.ptr();

  Rc<dxmt::Texture> ds_tex = ds ? ds->dxmtTexture() : nullptr;
  TextureViewKey ds_view = 0;
  bool ds_has_stencil = false;
  if (ds_tex) {
    ds_has_stencil = HasStencilAspect(ds->desc().Format);
    ds_view = ds_tex->createView({
        .format = ds->metalPixelFormat(),
        .type = surface_view_type(ds_tex.ptr()),
        .firstMiplevel = static_cast<uint16_t>(ds->mipLevel()),
        .miplevelCount = 1,
        .firstArraySlice = static_cast<uint16_t>(ds->arraySlice()),
        .arraySize = 1,
    });
  }

  PendingClear pc = m_pendingClear;
  m_pendingClear = {};

  bool have_any_color = false;
  if (pc.color_valid)
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      MTLD3D9Surface *rt = m_renderTargets[i].ptr();
      if (rt && !IsNullFormat(rt->desc().Format) && rt->dxmtTexture()) {
        have_any_color = true;
        break;
      }
    }
  const bool have_ds_clear = ds_tex && (pc.depth_valid || (pc.stencil_valid && ds_has_stencil));
  if (!have_any_color && !have_ds_clear)
    return; // (D3D9's "no-RT Clear" no-op)

  auto *chunk = m_dxmtQueue->CurrentChunk();

  // D3D9 Clear(D3DCLEAR_TARGET) clears every bound render target, not just
  // slot 0 (DXVK loops m_state.renderTargets). A deferred renderer binding an
  // MRT G-buffer otherwise keeps stale content in the unflagged slots. Emit
  // one Clear encoder per bound colour target; each folds into the next
  // matching Render's loadAction.
  if (pc.color_valid) {
    const bool srgb_write_pass = m_renderStates[D3DRS_SRGBWRITEENABLE] != 0;
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      MTLD3D9Surface *rt = m_renderTargets[i].ptr();
      if (!rt || IsNullFormat(rt->desc().Format))
        continue;
      Rc<dxmt::Texture> rt_tex = rt->dxmtTexture();
      if (!rt_tex)
        continue;
      auto base_fmt = rt->metalPixelFormat();
      auto effective_fmt = base_fmt;
      bool srgb_swapped = false;
      if (srgb_write_pass) {
        auto srgb_fmt = Recall_sRGB(base_fmt);
        if (srgb_fmt != base_fmt) {
          effective_fmt = srgb_fmt;
          srgb_swapped = true;
        }
      }
      TextureViewKey rt_view = rt_tex->createView({
          .format = effective_fmt,
          .type = surface_view_type(rt_tex.ptr()),
          .firstMiplevel = static_cast<uint16_t>(rt->mipLevel()),
          .miplevelCount = 1,
          .firstArraySlice = static_cast<uint16_t>(rt->arraySlice()),
          .arraySize = 1,
      });
      WMTClearColor clear_color{
          srgb_swapped ? encode_srgb_channel(pc.color[0]) : pc.color[0],
          srgb_swapped ? encode_srgb_channel(pc.color[1]) : pc.color[1],
          srgb_swapped ? encode_srgb_channel(pc.color[2]) : pc.color[2],
          pc.color[3],
      };
      chunk->emitcc([rt_tex_cap = rt_tex, rt_view, clear_color](ArgumentEncodingContext &ctx) mutable {
        ctx.clearColor(std::move(rt_tex_cap), rt_view, 1, clear_color);
      });
    }
  }
  if (have_ds_clear) {
    unsigned flag = (pc.depth_valid ? 1u : 0u) | ((pc.stencil_valid && ds_has_stencil) ? 2u : 0u);
    float clear_depth = pc.depth_valid ? pc.depth : 0.0f;
    uint8_t clear_stencil = pc.stencil_valid ? pc.stencil : 0u;
    chunk->emitcc([ds_tex_cap = ds_tex, ds_view, flag, clear_depth,
                   clear_stencil](ArgumentEncodingContext &ctx) mutable {
      ctx.clearDepthStencil(std::move(ds_tex_cap), ds_view, 1, flag, clear_depth, clear_stencil);
    });
  }
}

void
MTLD3D9Device::emitClippedClear(
    const std::vector<WMTScissorRect> &regions, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil
) {
  auto *chunk = m_dxmtQueue->CurrentChunk();

  // The region list is in render-target space; an attachment can be
  // smaller than the viewport, so narrow per attachment and drop
  // empties (DXVK d3d9_device.cpp ClearImageView clamps against the
  // image extent the same way).
  auto clamp_regions = [&regions](uint32_t width, uint32_t height) {
    std::vector<WMTScissorRect> clamped;
    clamped.reserve(regions.size());
    for (const auto &r : regions) {
      if (r.x >= width || r.y >= height)
        continue;
      clamped.push_back({
          r.x,
          r.y,
          std::min<uint64_t>(r.width, width - r.x),
          std::min<uint64_t>(r.height, height - r.y),
      });
    }
    return clamped;
  };
  // A clamped region can still cover one attachment whole (smaller RT
  // in an MRT set, viewport sized to the DS but not the RT); that
  // attachment keeps the loadAction-folding clear (DXVK's fullClear
  // split).
  auto covers_whole = [](const std::vector<WMTScissorRect> &rects, uint32_t width, uint32_t height) {
    return rects.size() == 1 && rects[0].x == 0 && rects[0].y == 0 && rects[0].width == width &&
           rects[0].height == height;
  };

  if (Flags & D3DCLEAR_TARGET) {
    // Decode D3DCOLOR (0xAARRGGBB). DXVK DecodeD3DCOLOR same shape.
    const float color[4] = {
        ((Color >> 16) & 0xFF) / 255.0f,
        ((Color >> 8) & 0xFF) / 255.0f,
        (Color & 0xFF) / 255.0f,
        ((Color >> 24) & 0xFF) / 255.0f,
    };
    // D3D9 Clear(D3DCLEAR_TARGET) hits every bound colour target, not
    // just slot 0; see drainPendingClear's loop for the references.
    const bool srgb_write_pass = m_renderStates[D3DRS_SRGBWRITEENABLE] != 0;
    for (unsigned i = 0; i < D3D_MAX_SIMULTANEOUS_RENDERTARGETS; ++i) {
      MTLD3D9Surface *rt = m_renderTargets[i].ptr();
      if (!rt || IsNullFormat(rt->desc().Format))
        continue;
      Rc<dxmt::Texture> rt_tex = rt->dxmtTexture();
      if (!rt_tex)
        continue;
      auto rt_regions = clamp_regions(rt->desc().Width, rt->desc().Height);
      if (rt_regions.empty())
        continue;
      auto base_fmt = rt->metalPixelFormat();
      auto effective_fmt = base_fmt;
      bool srgb_swapped = false;
      if (srgb_write_pass) {
        auto srgb_fmt = Recall_sRGB(base_fmt);
        if (srgb_fmt != base_fmt) {
          effective_fmt = srgb_fmt;
          srgb_swapped = true;
        }
      }
      TextureViewKey rt_view = rt_tex->createView({
          .format = effective_fmt,
          .type = surface_view_type(rt_tex.ptr()),
          .firstMiplevel = static_cast<uint16_t>(rt->mipLevel()),
          .miplevelCount = 1,
          .firstArraySlice = static_cast<uint16_t>(rt->arraySlice()),
          .arraySize = 1,
      });
      if (covers_whole(rt_regions, rt->desc().Width, rt->desc().Height)) {
        // loadAction stamping bypasses the sRGB view's encode, hence
        // the pre-encoded colour; see encode_srgb_channel.
        WMTClearColor clear_color{
            srgb_swapped ? encode_srgb_channel(color[0]) : color[0],
            srgb_swapped ? encode_srgb_channel(color[1]) : color[1],
            srgb_swapped ? encode_srgb_channel(color[2]) : color[2],
            color[3],
        };
        chunk->emitcc([rt_tex_cap = rt_tex, rt_view, clear_color](ArgumentEncodingContext &ctx) mutable {
          ctx.clearColor(std::move(rt_tex_cap), rt_view, 1, clear_color);
        });
        continue;
      }
      // The quad's fragment output stores through the sRGB view's
      // encode, so it takes the linear colour as-is.
      std::array<float, 4> quad_color = {color[0], color[1], color[2], color[3]};
      chunk->emitcc([quad = &m_clearQuad, rt_tex_cap = rt_tex, rt_view, rects = std::move(rt_regions),
                     quad_color](ArgumentEncodingContext &ctx) mutable {
        // First use of a format compiles the clear PSO on this thread;
        // wine's encode worker has no outer NSAutoreleasePool.
        auto pool = WMT::MakeAutoreleasePool();
        quad->begin(ctx, std::move(rt_tex_cap), rt_view);
        for (const auto &r : rects)
          quad->clear(ctx, r.x, r.y, r.width, r.height, quad_color);
        quad->end(ctx);
      });
    }
  }

  MTLD3D9Surface *ds = m_depthStencilSurface.ptr();
  Rc<dxmt::Texture> ds_tex = ds ? ds->dxmtTexture() : nullptr;
  const bool ds_has_stencil = ds && HasStencilAspect(ds->desc().Format);
  const unsigned ds_flag = ((Flags & D3DCLEAR_ZBUFFER) ? 1u : 0u) |
                           (((Flags & D3DCLEAR_STENCIL) && ds_has_stencil) ? 2u : 0u);
  if (ds_tex && ds_flag) {
    auto ds_regions = clamp_regions(ds->desc().Width, ds->desc().Height);
    if (ds_regions.empty())
      return;
    TextureViewKey ds_view = ds_tex->createView({
        .format = ds->metalPixelFormat(),
        .type = surface_view_type(ds_tex.ptr()),
        .firstMiplevel = static_cast<uint16_t>(ds->mipLevel()),
        .miplevelCount = 1,
        .firstArraySlice = static_cast<uint16_t>(ds->arraySlice()),
        .arraySize = 1,
    });
    const float clear_depth = (Flags & D3DCLEAR_ZBUFFER) ? Z : 0.0f;
    const uint8_t clear_stencil = static_cast<uint8_t>(Stencil);
    if (covers_whole(ds_regions, ds->desc().Width, ds->desc().Height)) {
      chunk->emitcc([ds_tex_cap = ds_tex, ds_view, ds_flag, clear_depth,
                     clear_stencil](ArgumentEncodingContext &ctx) mutable {
        ctx.clearDepthStencil(std::move(ds_tex_cap), ds_view, 1, ds_flag, clear_depth, clear_stencil);
      });
      return;
    }
    chunk->emitcc([quad = &m_clearQuad, ds_tex_cap = ds_tex, ds_view, ds_flag, clear_depth, clear_stencil,
                   rects = std::move(ds_regions)](ArgumentEncodingContext &ctx) mutable {
      // Same first-use PSO compile concern as the colour quad above.
      auto pool = WMT::MakeAutoreleasePool();
      quad->beginDepthStencil(ctx, std::move(ds_tex_cap), ds_view, ds_flag, clear_stencil);
      for (const auto &r : rects)
        quad->clear(ctx, r.x, r.y, r.width, r.height, {clear_depth, 0.0f, 0.0f, 0.0f});
      quad->end(ctx);
    });
  }
}

void
MTLD3D9Device::generateMipmaps(WMT::Texture texture, bool drain_pending_draws) {
  if (texture.handle == 0)
    return;
  // Metal's generateMipmaps requires mipmapLevelCount > 1; on a
  // 1-level texture the blit is invalid and wedges the command buffer.
  // Reset can legally re-run the AUTOGENMIPMAP path against a 1-level
  // texture, so no-op.
  if (texture.mipmapLevelCount() <= 1)
    return;
  // Drain queued draws onto a chunk, then post the mip-gen blit as its
  // own chunk lambda. Metal serialises submissions on the queue, so
  // generated mips are visible to subsequent draws without an explicit
  // wait. drain_pending_draws=false when the caller is already inside
  // FlushDrawBatch's flushOpenWork chain: re-entering would re-resolve
  // the same m_pendingDraws and blow the i386 thread stack
  // (FlushDrawBatch → generateMipmaps → FlushDrawBatch recursion).
  if (drain_pending_draws && !m_pendingOps.empty())
    FlushDrawBatch();

  // Reference(WMT::Texture): calls Reference(Class non_retained) which
  // retains; that NSObject_retain is paired with the chunk-lambda's
  // tex_retain destructor's release. Using `{texture.handle}` here
  // would invoke Reference(obj_handle_t) which takes ownership of a
  // pre-existing retain that the caller never gave us; the lambda's
  // destructor then over-releases every call, walking the texture's
  // refcount toward zero one mip-gen at a time.
  WMT::Reference<WMT::Texture> tex_retain(texture);
  obj_handle_t tex_handle = texture.handle;

  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;

  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([tex_retain = std::move(tex_retain), tex_handle, event_handle,
                 signal_seq](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();
    auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_generate_mipmaps>();
    cmd.type = WMTBlitCommandGenerateMipmaps;
    cmd.texture = tex_handle;
    ctx.endPass();
    ctx.signalEventByHandle(event_handle, signal_seq);
  });
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();
}

void
MTLD3D9Device::flushDeferredBlitWork() {
  // stageTextureUpload emits chunk lambdas directly. Only the
  // AUTOGENMIPMAP-mipsDirty sweep remains here; each
  // dirty bound texture posts its own mip-gen chunk (deduped within this
  // call across PS+VTF aliasing).
  bool any_dirty_mips = false;
  for (uint32_t slot = 0; slot < D3D9_MAX_TEXTURE_UNITS; ++slot) {
    auto *t = m_textures[slot].ptr();
    if (t && t->mipsDirty()) {
      any_dirty_mips = true;
      break;
    }
  }
  if (!any_dirty_mips)
    return;

  // Same texture may be bound at multiple stages (VTF + PS, or two PS
  // stages aliasing one source). Track handles we've already emitted
  // to avoid double generateMipmaps on the same Metal texture.
  obj_handle_t emitted[D3D9_MAX_TEXTURE_UNITS] = {};
  uint32_t emitted_count = 0;
  for (uint32_t slot = 0; slot < D3D9_MAX_TEXTURE_UNITS; ++slot) {
    auto *t = m_textures[slot].ptr();
    if (!t || !t->mipsDirty())
      continue;
    auto mt = t->metalTexture();
    bool seen = false;
    for (uint32_t i = 0; i < emitted_count; ++i)
      if (emitted[i] == mt.handle) {
        seen = true;
        break;
      }
    if (!seen) {
      // Already inside FlushDrawBatch's flushOpenWork chain; pass
      // drain_pending_draws=false so the inner generateMipmaps doesn't
      // recurse into another FlushDrawBatch on the same m_pendingDraws.
      generateMipmaps(mt, /*drain_pending_draws=*/false);
      emitted[emitted_count++] = mt.handle;
    }
    t->clearMipsDirty();
  }
}

// Post buffer→texture upload as chunk lambda (shared path for both
// host-memcpy and pre-existing buffer cases). No per-chunk signalEvent.
// Ring recycle via emitCmdbufTailSignal at Present-time.
static void
EmitTextureUploadChunk_d9(
    MTLD3D9Device *self, dxmt::CommandQueue &queue, WMT::Texture dst, obj_handle_t src_buffer_handle,
    uint64_t src_offset, uint32_t src_pitch, uint32_t src_bytes_per_image, uint32_t mip_level, uint32_t slice,
    WMTOrigin origin, WMTSize size
) {
  // RETAIN dst texture: chunk lambda runs encode-side LATER; calling thread
  // may drop owner before then. NSZombie found UAF mid-Reset when resource torn down.
  WMT::Reference<WMT::Texture> dst_retain(dst);
  obj_handle_t dst_handle = dst.handle;
  auto *chunk = queue.CurrentChunk();
  chunk->emitcc([dst_retain = std::move(dst_retain), dst_handle, src_buffer_handle, src_offset, src_pitch,
                 src_bytes_per_image, mip_level, slice, origin, size](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();
    auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
    cmd.type = WMTBlitCommandCopyFromBufferToTexture;
    cmd.src = src_buffer_handle;
    cmd.src_offset = src_offset;
    cmd.bytes_per_row = src_pitch;
    cmd.bytes_per_image = src_bytes_per_image;
    cmd.size = size;
    cmd.dst = dst_handle;
    cmd.slice = slice;
    cmd.level = mip_level;
    cmd.origin = origin;
    ctx.endPass();
  });
}

void
MTLD3D9Device::stageTextureUpload(
    WMT::Texture dst, uint32_t mip_level, uint32_t slice, WMTOrigin origin, WMTSize size, const void *src,
    uint32_t src_pitch, bool is_compressed, uint32_t src_slice_pitch
) {
  if (dst.handle == 0 || src == nullptr || src_pitch == 0 || size.width == 0 || size.height == 0)
    return;

  // Per-destination-slice + total staging bytes (texture_upload_layout
  // handles the compressed block-row rounding and the depth scaling).
  // Volume (3D) textures stage every depth slice, not just one; source
  // slices are spaced by src_slice_pitch (the full mip's slice stride,
  // which a sub-box upload makes larger than bytes_per_image), 0 meaning
  // contiguous (2D, or a full-box 3D upload). Ignoring depth left 3D
  // textures with only their first slice written, the rest reading
  // unallocated ring memory (zero); wined3d carries the slice pitch
  // through its upload for the same reason (texture.c
  // wined3d_texture_get_pitch).
  const uint32_t depth = size.depth ? static_cast<uint32_t>(size.depth) : 1u;
  const auto layout = texture_upload_layout(src_pitch, static_cast<uint32_t>(size.height), depth, is_compressed);
  const uint32_t bytes_per_image = layout.bytes_per_image;
  const uint32_t src_slice = src_slice_pitch ? src_slice_pitch : bytes_per_image;
  const size_t total_bytes = layout.total_bytes;

  // Coherent_id reads the GPU's last signalled cmdbuf seq so the ring
  // can recycle blocks whose tag has retired. Same shape as the
  // per-draw uploads on the Resolve/EmitDrawBatch path. Cached value
  // refreshed at flushOpenWork (post-commit); saves a wine_unix_call
  // per stageTextureUpload invocation, which streaming workloads hit
  // hundreds of times per frame.
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  // 16-byte alignment matches the per-draw upload's VB/IB shape and
  // is sufficient for any format Metal accepts on this path. The
  // bump allocator pads to alignment but doesn't enforce a row
  // alignment beyond what src_pitch already encodes.
  auto [block, offset] = m_uploadRing.allocate(m_currentCmdSeq, coherent_id, total_bytes, 16);
  char *staged = static_cast<char *>(block.mapped_address) + offset;
  if (src_slice == bytes_per_image) {
    std::memcpy(staged, src, total_bytes);
  } else {
    // Sub-box 3D upload: copy slice by slice, the staged slices packed
    // tightly while the source skips the rest of each full mip slice.
    for (uint32_t z = 0; z < depth; ++z)
      std::memcpy(
          staged + static_cast<size_t>(z) * bytes_per_image,
          static_cast<const char *>(src) + static_cast<size_t>(z) * src_slice, bytes_per_image
      );
  }

  // Post the upload blit as a chunk lambda. The m_completionEvent tail
  // signal matches the FlushDrawBatch shape; m_uploadRing.free_blocks(
  // signaledValue) recycles this block once the chunk's GPU side retires.
  EmitTextureUploadChunk_d9(
      this, *m_dxmtQueue, dst, block.buffer.handle, offset, src_pitch, bytes_per_image, mip_level, slice, origin, size
  );
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DDevice9)) {
    *ppvObject = static_cast<IDirect3DDevice9 *>(this);
    AddRef();
    return S_OK;
  }
  if (m_isEx && riid == __uuidof(IDirect3DDevice9Ex)) {
    *ppvObject = static_cast<IDirect3DDevice9Ex *>(this);
    AddRef();
    return S_OK;
  }
  // Private dxmt diag surface. Borrowed pointer; lifetime
  // tied to the IDirect3DDevice9 ref the caller already holds, so we
  // do NOT AddRef here. See d3d9_diag.hpp for the lifetime contract.
  // Apps never QI for this UUID; only tests do.
  if (riid == dxmt::IID_IDxmtDiag9) {
    *ppvObject = static_cast<dxmt::IDxmtDiag9 *>(this);
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::TestCooperativeLevel() {
  // D3D9Ex spec: always returns S_OK; apps probe device loss via
  // CheckDeviceState on the Ex interface. wined3d device.c d3d9_device_
  // TestCooperativeLevel and DXVK both match this.
  if (m_isEx)
    return D3D_OK;
  switch (m_deviceState.load(std::memory_order_relaxed)) {
  case DeviceState::Ok:
    return D3D_OK;
  case DeviceState::Lost:
    return D3DERR_DEVICELOST;
  case DeviceState::NotReset:
    return D3DERR_DEVICENOTRESET;
  }
  return D3D_OK;
}
UINT STDMETHODCALLTYPE
MTLD3D9Device::GetAvailableTextureMem() {
  // Returning the strictly-truthful UMA answer (0) drives era-typical
  // engines into recreate-every-frame fallbacks. Mirror dxgi/d3d11:
  // half of recommendedMaxWorkingSetSize, clamped to UINT32 since the
  // API returns UINT.
  uint64_t bytes = m_metalDevice.recommendedMaxWorkingSetSize();
  if (m_metalDevice.hasUnifiedMemory())
    bytes /= 2;
  if (bytes > UINT32_MAX)
    bytes = UINT32_MAX;
  return static_cast<UINT>(bytes);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EvictManagedResources() {
  // Hint to flush the sysmem-shadow → VRAM mirror for unused
  // D3DPOOL_MANAGED resources. UMA collapses that split, so the
  // hint has nothing to do. cf. DXVK d3d9_device.cpp.
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDirect3D(IDirect3D9 **ppD3D9) {
  if (!ppD3D9)
    return D3DERR_INVALIDCALL;
  *ppD3D9 = m_parent.ref();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDeviceCaps(D3DCAPS9 *pCaps) {
  return m_parent->GetDeviceCaps(m_creationParams.AdapterOrdinal, m_creationParams.DeviceType, pCaps);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) {
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return m_parent->GetAdapterDisplayMode(m_creationParams.AdapterOrdinal, pMode);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters) {
  if (!pParameters)
    return D3DERR_INVALIDCALL;
  *pParameters = m_creationParams;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) {
  // DXVK d3d9_device.cpp enforces the per-spec validation gates,
  // then on Windows uploads the bitmap to a HW cursor. macOS draws the
  // cursor itself (no Metal-side HW cursor API), so dxmt validates and
  // silently accepts; apps' init paths hr-check the validation; the
  // visible cursor is whatever the OS renders.
  if (!pCursorBitmap)
    return D3DERR_INVALIDCALL;
  auto *bitmap = static_cast<MTLD3D9Surface *>(pCursorBitmap);
  const D3DSURFACE_DESC &desc = bitmap->desc();
  if (desc.Format != D3DFMT_A8R8G8B8)
    return D3DERR_INVALIDCALL;
  const UINT w = desc.Width;
  const UINT h = desc.Height;
  // Cursor bitmap dimensions must be powers of two (DXVK).
  if ((w && (w & (w - 1))) || (h && (h & (h - 1))))
    return D3DERR_INVALIDCALL;
  // Hotspot must lie within the bitmap.
  if ((w && XHotSpot > w - 1) || (h && YHotSpot > h - 1))
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}
void STDMETHODCALLTYPE
MTLD3D9Device::SetCursorPosition(int X, int Y, DWORD Flags) {
  // wined3d device.c forwards to Win32 SetCursorPos. Games
  // warp the cursor for mouse-lock toggles, intro skip, menu nav;
  // silently dropping breaks those flows. Flags is documented as a
  // hint set (D3DCURSOR_IMMEDIATE_UPDATE) the runtime can ignore.
  (void)Flags;
#ifdef _WIN32
  ::SetCursorPos(X, Y);
#else
  (void)X;
  (void)Y;
#endif
}
BOOL STDMETHODCALLTYPE
MTLD3D9Device::ShowCursor(BOOL bShow) {
  // Returns the previous visibility per the wined3d_device_show_cursor
  // contract (wined3d device.c). UI toggle code that reads
  // the return to drive its own state was broken pre-this by the
  // always-FALSE return.
  BOOL prev = m_cursorVisible;
  m_cursorVisible = bShow;
  return prev;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateAdditionalSwapChain(
    D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DSwapChain9 **ppSwapChain
) {
  (void)pPresentationParameters;
  if (ppSwapChain)
    *ppSwapChain = nullptr;
  if (!pPresentationParameters || !ppSwapChain)
    return D3DERR_INVALIDCALL;
  // dxmt currently supports only the implicit swapchain. DXVK supports
  // multi-swapchain via CreateAdditionalSwapChainEx; adding it here
  // needs the Presenter to bind multiple CAMetalLayers + per-chain
  // PSO state. Spec-correct error per MSDN is D3DERR_NOTAVAILABLE
  // ("driver doesn't support"); E_NOTIMPL was breaking init for apps
  // that hr-strict-check the call (many launchers / overlays do).
  return D3DERR_NOTAVAILABLE;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) { return E_NOTIMPL; }

UINT STDMETHODCALLTYPE
MTLD3D9Device::GetNumberOfSwapChains() {
  return 1;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters) { return E_NOTIMPL; }


void
MTLD3D9Device::createAutoDepthStencil(const D3DPRESENT_PARAMETERS &params) {
  if (!params.EnableAutoDepthStencil) {
    // EnableAutoDepthStencil=FALSE on a Reset that previously had an
    // auto-DS: drop the cache. Any app pub-ref to the auto-DS keeps
    // the surface object alive with its old Metal texture; new draws
    // see no DS bound (post-Reset state).
    m_autoDepthStencilSurface = nullptr;
    return;
  }
  WMTPixelFormat fmt = D3DFormatToMetal(params.AutoDepthStencilFormat, D3D9FormatUsage::DepthStencil);
  if (fmt == WMTPixelFormatInvalid)
    return;
  WMTTextureInfo info{};
  info.pixel_format = fmt;
  info.width = params.BackBufferWidth;
  info.height = params.BackBufferHeight;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsageRenderTarget;
  info.options = WMTResourceStorageModePrivate;
  Rc<dxmt::Texture> dxmt_ds_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> ds_flags;
  ds_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto ds_allocation = dxmt_ds_texture->allocate(ds_flags);
  if (!ds_allocation || !ds_allocation->texture())
    return;
  WMT::Texture dsRawTex = ds_allocation->texture();
  dxmt_ds_texture->rename(std::move(ds_allocation));
  D3DSURFACE_DESC dsDesc{};
  dsDesc.Format = params.AutoDepthStencilFormat;
  dsDesc.Type = D3DRTYPE_SURFACE;
  dsDesc.Usage = D3DUSAGE_DEPTHSTENCIL;
  dsDesc.Pool = D3DPOOL_DEFAULT;
  // sample_count=1 above forces single-sample storage; mirror that
  // here so GetDesc reads back the actual MSAA mode, not the
  // requested-but-not-honored one. Matches the swapchain backbuffer's
  // descriptor coercion in backBufferDescriptors. Real MSAA auto-DS
  // support would lift both at once.
  dsDesc.MultiSampleType = D3DMULTISAMPLE_NONE;
  dsDesc.MultiSampleQuality = 0;
  dsDesc.Width = params.BackBufferWidth;
  dsDesc.Height = params.BackBufferHeight;
  // Identity-preserving Reset path; if the auto-DS already exists
  // (every call from Reset; the device ctor sees m_autoDepthStencilSurface
  // null and falls through to fresh-create), reuse the same
  // MTLD3D9Surface and swap its Metal backing in place. Apps that
  // held GetDepthStencilSurface() across Reset get the same surface
  // object back, now pointing at the new texture. Mirrors the
  // swapchain backbuffer's resetBacking shape.
  if (m_autoDepthStencilSurface.ptr()) {
    m_autoDepthStencilSurface->resetBacking(dsDesc, WMT::Reference<WMT::Texture>(dsRawTex), std::move(dxmt_ds_texture));
  } else {
    auto *dsSurface = new MTLD3D9Surface(
        this, dsDesc,
        /*container=*/static_cast<IDirect3DDevice9 *>(this), WMT::Reference<WMT::Texture>(dsRawTex),
        /*mipLevel=*/0,
        /*selfPin=*/false,
        /*parentTextureType=*/WMTTextureType2D,
        /*buffer=*/{},
        /*cpuPtr=*/nullptr,
        /*pitch=*/0,
        /*arraySlice=*/0,
        /*ownedBacking=*/nullptr,
        /*dxmtTexture=*/std::move(dxmt_ds_texture)
    );
    m_autoDepthStencilSurface = dsSurface;
  }
  m_depthStencilSurface = m_autoDepthStencilSurface.ptr();
  // Op-stream mirror: the inline assignment above bypasses
  // SetDepthStencilSurface, so push the SetRef explicitly. The op
  // takes one outstanding AddRefPrivate that the walker consumes when
  // it installs into m_encodeSideRefs.depth_stencil_surface.
  if (auto *ds = m_autoDepthStencilSurface.ptr()) {
    ds->AddRefPrivate();
    QueueRefOp(PendingRefOp::DepthStencilSurface, ds);
  }
}
// Present: device-level forwards to the implicit swapchain. wined3d
// device.c forwards iSwapChain=0 by hand. Multi-swapchain
// (CreateAdditionalSwapChain) is a TODO; for now there is only one.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Present(
    const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion
) { return E_NOTIMPL; }

// Device-level GetBackBuffer is a thin forwarder to the chain identified
// by iSwapChain (we only have one). wined3d device.c same shape.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetBackBuffer(
    UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer
) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetDialogBoxMode(BOOL bEnableDialogs) {
  // MSDN documents many error conditions; DXVK's note (d3d9_swapchain.cpp:
  // 795-800) is "doesn't appear to error at all in any of my tests of
  // these cases." Silently accept; apps' init paths hr-check this and
  // fail device-bring-up if it returns E_NOTIMPL. There's no Metal-side
  // mode to toggle (GDI dialog interop is Win32-only).
  (void)bEnableDialogs;
  return D3D_OK;
}
void STDMETHODCALLTYPE
MTLD3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP *pRamp) {}

void STDMETHODCALLTYPE
MTLD3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP *pRamp) {}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture,
    HANDLE *pSharedHandle
) {
  if (!ppTexture)
    return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  if (Width == 0 || Height == 0)
    return D3DERR_INVALIDCALL;
  if (Width > 16384 || Height > 16384)
    return D3DERR_INVALIDCALL;

  // wined3d texture.c; D3DPOOL_MANAGED is invalid on d3d9ex
  // devices. (Non-Ex devices honour MANAGED; we currently downgrade
  // it to a plain CPU-resident allocation, see the storage map below.)
  if (Pool == D3DPOOL_MANAGED && m_isEx)
    return D3DERR_INVALIDCALL;

  // wined3d texture.c; D3DUSAGE_WRITEONLY is buffer-only.
  if (Usage & D3DUSAGE_WRITEONLY)
    return D3DERR_INVALIDCALL;

  // RT and DS usage are mutually exclusive at the surface-bind level
  // and combining them on a texture has no defined meaning.
  if ((Usage & D3DUSAGE_RENDERTARGET) && (Usage & D3DUSAGE_DEPTHSTENCIL))
    return D3DERR_INVALIDCALL;
  // RT/DS textures must live in DEFAULT pool; the GPU-only side has
  // no managed mirror to push back from CPU.
  if ((Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;

  // wined3d texture.c; AUTOGENMIPMAP forbids SYSTEMMEM and
  // restricts levels to 0/1. The texture exposes 1 level to the app;
  // the Metal allocation holds the full chain that the UnlockRect
  // auto-fire fills.
  if (Usage & D3DUSAGE_AUTOGENMIPMAP) {
    if (Pool == D3DPOOL_SYSTEMMEM)
      return D3DERR_INVALIDCALL;
    if (Levels != 0 && Levels != 1)
      return D3DERR_INVALIDCALL;
  }

  // pSharedHandle doubles as an initial-data pointer for single-level
  // SYSTEMMEM textures (the Vista-era user-memory path; the rows are
  // tightly packed). DXVK accepts it on any device and copies at
  // create time; wined3d additionally gates it on Ex. Cross-process
  // sharing (DEFAULT pool) stays unimplemented.
  const void *initial_data = nullptr;
  if (pSharedHandle) {
    if (Pool == D3DPOOL_SYSTEMMEM && Levels == 1) {
      initial_data = *reinterpret_cast<void **>(pSharedHandle);
      pSharedHandle = nullptr;
    } else if (Pool != D3DPOOL_DEFAULT) {
      return D3DERR_INVALIDCALL;
    } else {
      // TODO: cross-process resource share.
      return E_NOTIMPL;
    }
  }

  // Format gating depends on the requested role. Attachment usage on
  // a format Metal cannot attach (RENDERTARGET on compressed, say) is
  // rejected here even though wined3d accepts the create: wined3d's GL
  // backend can convert or render such formats, Metal never can, and
  // apps ship a create-time fallback for the rejection because native
  // runtimes and DXVK (CheckImageSupport) reject it too. Accepting
  // would strand them past their fallback at an unbindable texture.
  D3D9FormatUsage formatUsage = D3D9FormatUsage::SampleableTexture;
  if (Usage & D3DUSAGE_RENDERTARGET)
    formatUsage = D3D9FormatUsage::RenderTarget;
  else if (Usage & D3DUSAGE_DEPTHSTENCIL)
    formatUsage = D3D9FormatUsage::DepthStencil;
  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, formatUsage);
  if (pixelFormat == WMTPixelFormatInvalid) {
    Logger::warn(str::format("d3d9: CreateTexture: unsupported format ", (unsigned)Format, " usage ", (unsigned)Usage));
    return D3DERR_INVALIDCALL;
  }

  // Levels=0 means full chain to 1x1. wined3d's wined3d_log2i +1.
  uint32_t real_levels;
  if (Levels == 0) {
    real_levels = 1;
    UINT m = std::max(Width, Height);
    while (m > 1) {
      m >>= 1;
      ++real_levels;
    }
  } else {
    real_levels = Levels;
    UINT max_dim = std::max(Width, Height);
    uint32_t max_levels = 1;
    UINT m = max_dim;
    while (m > 1) {
      m >>= 1;
      ++max_levels;
    }
    if (real_levels > max_levels)
      return D3DERR_INVALIDCALL;
  }

  // AUTOGENMIPMAP: app sees one level (D3D9 spec; GetSurfaceLevel /
  // LockRect on level > 0 returns INVALIDCALL). The Metal allocation
  // gets the full chain so generateMipmapsForTexture has somewhere to
  // write; auto-fire on UnlockRect(0) per spec keeps the chain in
  // sync with level-0 edits.
  uint32_t metal_levels = real_levels;
  uint32_t app_levels = real_levels;
  if (Usage & D3DUSAGE_AUTOGENMIPMAP) {
    app_levels = 1;
  }

  // Pool → storage (like CreateOffscreenPlainSurface + usage flags).
  // RT-promotion: every DEFAULT color texture gets RenderTarget unconditionally.
  WMTResourceOptions storage;
  WMTTextureUsage usage_bits = WMTTextureUsageShaderRead;
  // RT bit: D3DUSAGE_DEPTHSTENCIL (DS is render-target + sampler)
  // or DEFAULT-pool color (promotion, DXVK pattern).
  // Skip compressed: BC textures reject RenderTarget on Apple Silicon.
  if (Usage & D3DUSAGE_DEPTHSTENCIL)
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsageRenderTarget);
  else if (Pool == D3DPOOL_DEFAULT && !IsCompressedFormat(Format))
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsageRenderTarget);
  // PixelFormatView enables the sRGB-aliased sample view. Include BC formats: an
  // sRGB view of a BC texture decodes correctly on sample (matches DXVK), at the
  // cost of opting that texture out of AGX lossless. Without it, BC albedo samples
  // linear (no sRGB decode) and reads too bright, blowing out HDR scenes.
  if (!(Usage & D3DUSAGE_DEPTHSTENCIL) && Recall_sRGB(D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture)) !=
                                              D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture))
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsagePixelFormatView);
  switch (Pool) {
  case D3DPOOL_DEFAULT:
    storage = WMTResourceStorageModePrivate;
    break;
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_SCRATCH:
    storage = WMTResourceStorageModeShared;
    break;
  case D3DPOOL_MANAGED:
    // Non-Ex MANAGED. Real D3D9 would keep both a sysmem master and a
    // GPU mirror with eviction; on Apple Silicon's unified memory the
    // distinction collapses. Track in project memory if real games
    // start hitting eviction-sensitive paths.
    storage = WMTResourceStorageModeShared;
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = metal_levels;
  info.sample_count = 1;
  info.usage = usage_bits;
  info.options = storage;

  // UMA-correct MANAGED single-buffer: Level=1 uncompressed 2D sampler-only. newTexture aliases level-0; LockRect →
  // Metal-mapped pBits; on UMA GPU samples host pages directly (no Unlock memcpy).
  bool buffer_backed_eligible = Pool == D3DPOOL_MANAGED && app_levels == 1 && metal_levels == 1 &&
                                !(Usage & D3DUSAGE_AUTOGENMIPMAP) && !(Usage & D3DUSAGE_RENDERTARGET) &&
                                !(Usage & D3DUSAGE_DEPTHSTENCIL) && !(Usage & D3DUSAGE_DYNAMIC) &&
                                !IsCompressedFormat(Format);
  // Zero-copy aliasing exposes Metal's linear-texture row alignment
  // as the LockRect pitch. wined3d and DXVK hand back (near-)tight
  // pitches, and shipped titles write rows at width*bpp regardless of
  // the reported value, so a padded pitch shears every such upload.
  // Widths whose tight pitch misses the alignment take the mirror
  // path, where the pitch is unconstrained and tight.
  uint64_t linear_alignment = 1;
  if (buffer_backed_eligible) {
    linear_alignment = m_metalDevice.minimumLinearTextureAlignmentForPixelFormat(pixelFormat);
    if (linear_alignment == 0)
      linear_alignment = 1;
    if ((D3DFormatRowPitch(Format, Width) % linear_alignment) != 0)
      buffer_backed_eligible = false;
  }

  // All textures rooted in Rc<dxmt::Texture> (survives thread boundaries). Buffer-backed: caller-managed page (pool
  // reuse, pre-fault optimizations); regular: allocate(). Same MTLD3D9Texture ctor; bufferPitch signals mode.
  Rc<dxmt::Texture> dxmt_texture;
  uint32_t backingPitch = 0;
  if (buffer_backed_eligible) {
    // The eligibility gate above guarantees the tight pitch satisfies
    // linear_alignment, so this round-up is a provable no-op kept for
    // shape parity with the offscreen-plain branch.
    const uint64_t row_bytes = static_cast<uint64_t>(D3DFormatRowPitch(Format, Width));
    backingPitch = static_cast<uint32_t>((row_bytes + linear_alignment - 1) & ~(linear_alignment - 1));
    const uint64_t backing_bytes = static_cast<uint64_t>(backingPitch) * Height;
    // Pool hit skips newBuffer XPC + page-fault cliff (pre-fault memset cost). Pool fed by VB/IB/mirrors; texture
    // donation deferred for ref_tracker safety (in-flight chunks retain allocation).
    WMT::Reference<WMT::Buffer> backingBuffer{};
    uint64_t backing_gpu_addr = 0;
    void *backingPtr = nullptr;
    void *backingHostPtr = nullptr;
    if (!acquireBufferBacking(
            static_cast<size_t>(backing_bytes), backingBuffer, backing_gpu_addr, backingHostPtr, backingPtr
        )) {
      backingPtr = wsi::aligned_malloc(backing_bytes, DXMT_PAGE_SIZE);
      if (!backingPtr)
        return D3DERR_OUTOFVIDEOMEMORY;
      // Pre-fault every page now so the app's first Lock+memcpy doesn't
      // pay the 100ms+/page Rosetta x86_32 first-touch cliff streamed
      // mid-frame. Same pattern as the texture-mirror path.
      std::memset(backingPtr, 0, backing_bytes);

      WMTBufferInfo binfo{};
      binfo.length = backing_bytes;
      // Shared (UMA aliasing), Default cache. Hazard tracking left at
      // Metal's default (Tracked): Untracked here suppressed barriers
      // between LockRect-time CPU writes (via the aliased pointer) and
      // GPU samples within the same cmdbuf, and between blit-encoder
      // generateMipmaps / replaceRegion fall-throughs and the render
      // encoder that samples them next.
      binfo.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
      binfo.memory.set(backingPtr);
      backingBuffer = m_metalDevice.newBuffer(binfo);
      if (backingBuffer == nullptr) {
        wsi::aligned_free(backingPtr);
        return D3DERR_OUTOFVIDEOMEMORY;
      }
    }
    info.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
    dxmt_texture = new dxmt::Texture(
        static_cast<unsigned>(backing_bytes), static_cast<unsigned>(backingPitch), info, m_metalDevice
    );
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    auto allocation = dxmt_texture->wrapBuffer(std::move(backingBuffer), backingPtr, alloc_flags);
    if (!allocation || !allocation->texture()) {
      // dxmt::Texture destructor will run as `dxmt_texture` falls out
      // of scope; the half-built allocation owns the (buffer, mapped)
      // pair and tears them down via its dtor's i386 aligned_free.
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    dxmt_texture->rename(std::move(allocation));
  } else {
    dxmt_texture = new dxmt::Texture(info, m_metalDevice);
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    if (Pool == D3DPOOL_DEFAULT)
      alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
    auto allocation = dxmt_texture->allocate(alloc_flags);
    if (!allocation || !allocation->texture())
      return D3DERR_OUTOFVIDEOMEMORY;
    dxmt_texture->rename(std::move(allocation));
  }

  auto *tex =
      new MTLD3D9Texture(this, Width, Height, app_levels, Usage, Format, Pool, std::move(dxmt_texture), backingPitch);
  if (Pool == D3DPOOL_DEFAULT)
    tex->markLosable();
  if (initial_data) {
    // The lock path absorbs every backing variant; at create time a
    // SYSTEMMEM lock is a plain host copy. A lock failure here means
    // the mirror allocation failed; fail the create rather than hand
    // back a texture that silently dropped its data.
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
      tex->AddRef();
      tex->Release();
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    const uint32_t src_pitch = D3DFormatRowPitch(Format, Width);
    const uint32_t rows = D3DFormatRowCount(Format, Height);
    const uint8_t *src = static_cast<const uint8_t *>(initial_data);
    uint8_t *dst = static_cast<uint8_t *>(lr.pBits);
    const uint32_t copy_pitch = std::min<uint32_t>(src_pitch, lr.Pitch);
    for (uint32_t r = 0; r < rows; ++r)
      std::memcpy(dst + static_cast<size_t>(r) * lr.Pitch, src + static_cast<size_t>(r) * src_pitch, copy_pitch);
    tex->UnlockRect(0);
  }
  tex->AddRef();
  *ppTexture = tex;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVolumeTexture(
    UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle
) {
  if (!ppVolumeTexture)
    return D3DERR_INVALIDCALL;
  *ppVolumeTexture = nullptr;
  if (pSharedHandle)
    return E_NOTIMPL; // cross-process shared 3D textures deferred
  if (Width == 0 || Height == 0 || Depth == 0)
    return D3DERR_INVALIDCALL;
  // Mirror GetDeviceCaps's MaxVolumeExtent (2048). The 2D/cube/RT/DS
  // create paths reject over-cap dimensions up front rather than
  // forwarding them to the allocator to fail as OUTOFVIDEOMEMORY.
  if (Width > 2048 || Height > 2048 || Depth > 2048)
    return D3DERR_INVALIDCALL;
  // wined3d texture.c; D3DUSAGE_WRITEONLY is buffer-only.
  if (Usage & D3DUSAGE_WRITEONLY)
    return D3DERR_INVALIDCALL;
  switch (Pool) {
  case D3DPOOL_DEFAULT:
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_MANAGED:
  case D3DPOOL_SCRATCH:
    break;
  default:
    return D3DERR_INVALIDCALL;
  }
  // D3D9 spec: volume textures can't be render targets or depth-stencil.
  if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL))
    return D3DERR_INVALIDCALL;
  // D3D9 spec: AUTOGENMIPMAP is a 2D/cube-only feature; there's no
  // hardware path for sampler-time mipmap regen on 3D textures. wined3d
  // texture.c (d3d9_texture_init) rejects with INVALIDCALL on
  // WINED3D_RTYPE_TEXTURE_3D; DXVK d3d9_common_texture.cpp mirrors
  // the same gate inside NormalizeTextureProperties.
  if (Usage & D3DUSAGE_AUTOGENMIPMAP)
    return D3DERR_INVALIDCALL;
  // Lower the format. Volume textures are sampled-only on Apple Silicon
  // (no 3D RT), so use the SampleableTexture path. Compressed formats
  // (DXT*) are NOT legal on 3D textures per D3D9 spec; D3DFormatToMetal
  // returns WMTPixelFormatInvalid for them on the sampleable path.
  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture);
  if (pixelFormat == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;

  UINT real_levels = Levels;
  if (real_levels == 0) {
    real_levels = 1;
    UINT m = std::max({Width, Height, Depth});
    while ((m >>= 1) != 0)
      ++real_levels;
  } else {
    uint32_t max_levels = 1;
    UINT m = std::max({Width, Height, Depth});
    while (m > 1) {
      m >>= 1;
      ++max_levels;
    }
    if (real_levels > max_levels)
      return D3DERR_INVALIDCALL;
  }

  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = Width;
  info.height = Height;
  info.depth = Depth;
  info.array_length = 1;
  info.type = WMTTextureType3D;
  info.mipmap_level_count = real_levels;
  info.sample_count = 1;
  info.usage = WMTTextureUsageShaderRead;
  // PixelFormatView for the sRGB-aliased sample view (D3DSAMP_SRGBTEXTURE), same
  // as the 2D/cube paths. Gated on an sRGB pair existing so non-sRGB LUT volumes
  // keep AGX lossless.
  if (Recall_sRGB(pixelFormat) != pixelFormat)
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
  info.options = WMTResourceStorageModePrivate;

  Rc<dxmt::Texture> dxmt_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  if (Pool == D3DPOOL_DEFAULT)
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = dxmt_texture->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return D3DERR_OUTOFVIDEOMEMORY;
  dxmt_texture->rename(std::move(allocation));

  auto *tex =
      new MTLD3D9VolumeTexture(this, Width, Height, Depth, real_levels, Usage, Format, Pool, std::move(dxmt_texture));
  if (Pool == D3DPOOL_DEFAULT)
    tex->markLosable();
  tex->AddRef();
  *ppVolumeTexture = tex;
  return D3D_OK;
}

// CreateCubeTexture: same validation as CreateTexture, one dimension (EdgeLength).
// Allocates 6 faces × N levels sharing one TextureCube handle.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture,
    HANDLE *pSharedHandle
) {
  if (!ppCubeTexture)
    return D3DERR_INVALIDCALL;
  *ppCubeTexture = nullptr;
  if (EdgeLength == 0)
    return D3DERR_INVALIDCALL;
  if (EdgeLength > 16384)
    return D3DERR_INVALIDCALL;

  if (Pool == D3DPOOL_MANAGED && m_isEx)
    return D3DERR_INVALIDCALL;
  if (Usage & D3DUSAGE_WRITEONLY)
    return D3DERR_INVALIDCALL;
  if ((Usage & D3DUSAGE_RENDERTARGET) && (Usage & D3DUSAGE_DEPTHSTENCIL))
    return D3DERR_INVALIDCALL;
  if ((Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) && Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  if (Usage & D3DUSAGE_AUTOGENMIPMAP) {
    if (Pool == D3DPOOL_SYSTEMMEM)
      return D3DERR_INVALIDCALL;
    if (Levels != 0 && Levels != 1)
      return D3DERR_INVALIDCALL;
  }

  if (pSharedHandle)
    return E_NOTIMPL;

  // Attachment usage on a format Metal cannot attach is rejected at
  // create; see CreateTexture.
  D3D9FormatUsage formatUsage = D3D9FormatUsage::SampleableTexture;
  if (Usage & D3DUSAGE_RENDERTARGET)
    formatUsage = D3D9FormatUsage::RenderTarget;
  else if (Usage & D3DUSAGE_DEPTHSTENCIL)
    formatUsage = D3D9FormatUsage::DepthStencil;
  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, formatUsage);
  if (pixelFormat == WMTPixelFormatInvalid) {
    Logger::warn(str::format("d3d9: CreateCubeTexture: unsupported format ", (unsigned)Format, " usage ", (unsigned)Usage));
    return D3DERR_INVALIDCALL;
  }

  uint32_t real_levels;
  if (Levels == 0) {
    real_levels = 1;
    UINT m = EdgeLength;
    while (m > 1) {
      m >>= 1;
      ++real_levels;
    }
  } else {
    real_levels = Levels;
    uint32_t max_levels = 1;
    UINT m = EdgeLength;
    while (m > 1) {
      m >>= 1;
      ++max_levels;
    }
    if (real_levels > max_levels)
      return D3DERR_INVALIDCALL;
  }

  // AUTOGENMIPMAP: see CreateTexture for the full-chain rationale.
  uint32_t metal_levels = real_levels;
  uint32_t app_levels = real_levels;
  if (Usage & D3DUSAGE_AUTOGENMIPMAP) {
    app_levels = 1;
  }

  WMTResourceOptions storage;
  WMTTextureUsage usage_bits = WMTTextureUsageShaderRead;
  // RT bit: D3DUSAGE_DEPTHSTENCIL (DS is render-target + sampler)
  // or DEFAULT-pool color (promotion, DXVK pattern).
  // Skip compressed: BC textures reject RenderTarget on Apple Silicon.
  if (Usage & D3DUSAGE_DEPTHSTENCIL)
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsageRenderTarget);
  else if (Pool == D3DPOOL_DEFAULT && !IsCompressedFormat(Format))
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsageRenderTarget);
  // PixelFormatView capability: see CreateTexture above for rationale (BC sRGB
  // sample views decode correctly; without it BC env-maps read too bright).
  if (!(Usage & D3DUSAGE_DEPTHSTENCIL) && Recall_sRGB(D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture)) !=
                                              D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture))
    usage_bits = (WMTTextureUsage)(usage_bits | WMTTextureUsagePixelFormatView);
  switch (Pool) {
  case D3DPOOL_DEFAULT:
    storage = WMTResourceStorageModePrivate;
    break;
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_SCRATCH:
  case D3DPOOL_MANAGED:
    storage = WMTResourceStorageModeShared;
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = EdgeLength;
  info.height = EdgeLength;
  info.depth = 1;
  // Metal TextureCube is a single texture with 6 implicit slices.
  // array_length=1 selects TextureCube (vs TextureCubeArray which
  // wants array_length=#cubes).
  info.array_length = 1;
  info.type = WMTTextureTypeCube;
  info.mipmap_level_count = metal_levels;
  info.sample_count = 1;
  info.usage = usage_bits;
  info.options = storage;

  // Wrap in dxmt::Texture so chunk lambdas can capture a Rc<>. Cube
  // textures take only the regular ctor; MTLBuffer.newTexture rejects
  // non-Type2D so the buffer-backed shape doesn't apply here.
  // Pool → flags mirrors CreateTexture above.
  Rc<dxmt::Texture> dxmt_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  if (Pool == D3DPOOL_DEFAULT)
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = dxmt_texture->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return D3DERR_OUTOFVIDEOMEMORY;
  dxmt_texture->rename(std::move(allocation));

  auto *tex = new MTLD3D9CubeTexture(this, EdgeLength, app_levels, Usage, Format, Pool, std::move(dxmt_texture));
  if (Pool == D3DPOOL_DEFAULT)
    tex->markLosable();
  tex->AddRef();
  *ppCubeTexture = tex;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle
) {
  if (!ppVertexBuffer)
    return D3DERR_INVALIDCALL;
  *ppVertexBuffer = nullptr;
  if (Length == 0)
    return D3DERR_INVALIDCALL;

  // wined3d buffer.c; SCRATCH not allowed for buffers (unlike
  // surfaces, scratch buffers have no defined CPU-only role).
  if (Pool == D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;
  // wined3d buffer.c; MANAGED on Ex device is invalid.
  if (Pool == D3DPOOL_MANAGED && m_isEx)
    return D3DERR_INVALIDCALL;
  // wined3d buffer.c; buffers can't be RT or DS. AUTOGENMIPMAP
  // is texture-only (DXVK d3d9_common_buffer.cpp rejects).
  if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP))
    return D3DERR_INVALIDCALL;
  // MANAGED + DYNAMIC is spec-forbidden; wined3d permits it at the
  // d3d9 layer but DXVK d3d9_common_buffer.cpp rejects. Reject
  // for spec-correctness; apps shipping the combo hit a defined
  // INVALIDCALL instead of silent acceptance.
  if (Pool == D3DPOOL_MANAGED && (Usage & D3DUSAGE_DYNAMIC))
    return D3DERR_INVALIDCALL;
  // WRITEONLY is buffer-only and is the *expected* flag for vertex
  // buffers; allow it.

  // pSharedHandle: wined3d device.c returns NOTAVAILABLE rather
  // than INVALIDCALL for Ex+non-DEFAULT (different from texture/OPS
  // paths: buffer-specific contract).
  if (pSharedHandle) {
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool != D3DPOOL_DEFAULT)
      return D3DERR_NOTAVAILABLE;
    return E_NOTIMPL; // cross-process buffer share deferred
  }

  // Pool → Metal storage. Every D3D9 vertex buffer is Lockable per the
  // API contract (the WRITEONLY flag is a hint, not a gate), so the
  // backing has to be CPU-mappable. Shared collapses to the same
  // physical pages as Private on Apple-Silicon UMA; Private would
  // save nothing and would force a staging-upload path on every Lock.
  // Pool only gates validity here; it doesn't change the storage
  // choice.
  switch (Pool) {
  case D3DPOOL_DEFAULT:
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_MANAGED: // non-Ex MANAGED collapses to CPU-resident
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  // 32-bit WoW64: pre-allocate backing (4GB limit). DYNAMIC: per-DISCARD retire-pool (wined3d pattern), bounded by
  // cmdbuf depth. Safety: within-cmdbuf GPU-read-during-write impossible (retire on DISCARD prevents overlap).
  WMT::Reference<WMT::Buffer> buffer{};
  uint64_t gpu_address = 0;
  void *backing = nullptr;
  void *host_ptr = nullptr;
  if (HRESULT hr = acquireOrAllocateBufferBacking(Length, buffer, gpu_address, host_ptr, backing); FAILED(hr))
    return hr;

  auto *vb = new MTLD3D9VertexBuffer(this, Length, Usage, FVF, Pool, std::move(buffer), gpu_address, host_ptr, backing);
  if (Pool == D3DPOOL_DEFAULT)
    vb->markLosable();
  vb->AddRef();
  *ppVertexBuffer = vb;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer,
    HANDLE *pSharedHandle
) {
  if (!ppIndexBuffer)
    return D3DERR_INVALIDCALL;
  *ppIndexBuffer = nullptr;
  if (Length == 0)
    return D3DERR_INVALIDCALL;

  // Index format must be one of the two D3D9-defined index formats.
  // wined3d defers this to wined3d_format lookup; dxmt rejects up
  // front so unsupported formats fail closed without reaching the
  // Metal allocator.
  if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32)
    return D3DERR_INVALIDCALL;

  // Same pool / usage gating as CreateVertexBuffer (buffer.c).
  if (Pool == D3DPOOL_SCRATCH)
    return D3DERR_INVALIDCALL;
  if (Pool == D3DPOOL_MANAGED && m_isEx)
    return D3DERR_INVALIDCALL;
  if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_AUTOGENMIPMAP))
    return D3DERR_INVALIDCALL;
  if (Pool == D3DPOOL_MANAGED && (Usage & D3DUSAGE_DYNAMIC))
    return D3DERR_INVALIDCALL;

  if (pSharedHandle) {
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool != D3DPOOL_DEFAULT)
      return D3DERR_NOTAVAILABLE;
    return E_NOTIMPL; // cross-process buffer share deferred
  }

  // Pool → Metal storage (mirrors CreateVertexBuffer; see that body
  // for the rationale on always going Shared on UMA).
  switch (Pool) {
  case D3DPOOL_DEFAULT:
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_MANAGED:
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  // CpuPlaced backing: see CreateVertexBuffer for the 32-bit
  // rationale, device-pool routing, and DYNAMIC retire-pool shape.
  WMT::Reference<WMT::Buffer> buffer{};
  uint64_t gpu_address = 0;
  void *backing = nullptr;
  void *host_ptr = nullptr;
  if (HRESULT hr = acquireOrAllocateBufferBacking(Length, buffer, gpu_address, host_ptr, backing); FAILED(hr))
    return hr;

  auto *ib =
      new MTLD3D9IndexBuffer(this, Length, Usage, Format, Pool, std::move(buffer), gpu_address, host_ptr, backing);
  if (Pool == D3DPOOL_DEFAULT)
    ib->markLosable();
  ib->AddRef();
  *ppIndexBuffer = ib;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  if (Width == 0 || Height == 0)
    return D3DERR_INVALIDCALL;
  // Mirrors GetDeviceCaps's MaxTextureWidth/Height. Silicon GPUs go
  // higher in practice, but we report 16384 in caps so we should
  // honour the same bound here.
  if (Width > 16384 || Height > 16384)
    return D3DERR_INVALIDCALL;
  // pSharedHandle: the cross-process handle. Non-Ex devices reject
  // any non-null pSharedHandle with E_NOTIMPL (matches wined3d's
  // d3d9_device_CreateRenderTarget early-out). Ex devices accept
  // the pointer and silently proceed without sharing; wined3d
  // FIXMEs and proceeds; we match the outcome. Resource sharing
  // is a future feature; the no-op stance is the right placeholder.
  if (pSharedHandle && !m_isEx)
    return E_NOTIMPL;

  // 'NULL' FOURCC sentinel: app wants a colour RT slot bound but
  // never written. There's no real Metal pixel format; the surface
  // still needs a placeholder Metal texture so the rest of the dxmt
  // surface plumbing (refcount, GetRenderTarget round-trips, swizzle
  // math) doesn't have to special-case a null storage. Allocate a
  // 1×1 BGRA8 dummy and rely on the batched-draw render-pass open +
  // bindPSOAndDraw to skip the slot whenever the surface's D3DFORMAT is NULL.
  // Reference: DXVK src/d3d9/d3d9_common_texture.cpp.
  const bool isNullRT = IsNullFormat(Format);
  WMTPixelFormat pixelFormat =
      isNullRT ? WMTPixelFormatBGRA8Unorm : D3DFormatToMetal(Format, D3D9FormatUsage::RenderTarget);
  if (pixelFormat == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;

  // Multisample mapping shared with CreateDepthStencilSurface and
  // probed by CheckDeviceMultiSampleType; see the helper at the top
  // of this file for the rationale.
  auto [sampleCount, msHr] = multisample_type_to_metal_sample_count(MultiSample, m_metalDevice);
  if (FAILED(msHr))
    return msHr;
  (void)MultisampleQuality;

  // Metal rejects MSAA + Shared storage at descriptor validation.
  // catch it up front with INVALIDCALL so the failure surfaces with
  // a sensible HRESULT rather than D3DERR_OUTOFVIDEOMEMORY at
  // newTexture time. D3D9 itself disallows the combination.
  if (sampleCount > 1 && Lockable)
    return D3DERR_INVALIDCALL;

  // Build the Metal texture descriptor. Render targets are private-
  // storage by default; callers that asked for a Lockable RT get
  // Shared storage instead so CPU map paths can land later. Apple
  // Silicon's unified memory makes Shared cheap; on discrete GPUs
  // this would be a perf hit but dxmt only targets Apple Silicon.
  // Reference: MGL/MGLRenderer.m newCommandQueue / texture descriptor
  // setup; the MTLTextureUsage.renderTarget + .shaderRead pair is the
  // canonical RT shape so subsequent SetTexture binds the same handle.
  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  // NULL-RT placeholder: 1×1, single sample, plain RT usage. The
  // Width/Height the app asked for stay on the D3DSURFACE_DESC so
  // queries round-trip, but no Metal storage proportional to the
  // real RT size gets allocated.
  info.width = isNullRT ? 1u : Width;
  info.height = isNullRT ? 1u : Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = (!isNullRT && sampleCount > 1) ? WMTTextureType2DMultisample : WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = isNullRT ? 1u : sampleCount;
  info.usage = static_cast<WMTTextureUsage>(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  // PixelFormatView for D3DRS_SRGBWRITEENABLE: the render-pass
  // attachment swaps to an sRGB-format view of the same texture.
  // NULL-RT placeholder skips the flag: it never participates in a
  // colour write that would care about gamma encoding, and BGRA8Unorm
  // already has an sRGB pair so the alias would succeed but be unused.
  if (!isNullRT && Recall_sRGB(pixelFormat) != pixelFormat)
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
  info.options = Lockable ? WMTResourceStorageModeShared : WMTResourceStorageModePrivate;

  Rc<dxmt::Texture> dxmt_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  if (!Lockable)
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = dxmt_texture->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return D3DERR_OUTOFVIDEOMEMORY;
  WMT::Texture rawTex = allocation->texture();
  dxmt_texture->rename(std::move(allocation));

  D3DSURFACE_DESC desc{};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Width = Width;
  desc.Height = Height;

  auto *surface = new MTLD3D9Surface(
      this, desc,
      /*container=*/static_cast<IDirect3DDevice9 *>(this), WMT::Reference<WMT::Texture>(rawTex),
      /*mipLevel=*/0,
      /*selfPin=*/true,
      /*parentTextureType=*/WMTTextureType2D,
      /*buffer=*/{},
      /*cpuPtr=*/cpuPtr,
      /*pitch=*/pitch,
      /*arraySlice=*/0,
      /*ownedBacking=*/ownedBacking,
      /*dxmtTexture=*/std::move(dxmt_texture)
  );
  // CreateRenderTarget surfaces are always D3DPOOL_DEFAULT.
  surface->markLosable();
  surface->AddRef();
  *ppSurface = surface;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  if (Width == 0 || Height == 0)
    return D3DERR_INVALIDCALL;
  if (Width > 16384 || Height > 16384)
    return D3DERR_INVALIDCALL;
  // pSharedHandle: see CreateRenderTarget for the policy rationale.
  if (pSharedHandle && !m_isEx)
    return E_NOTIMPL;

  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, D3D9FormatUsage::DepthStencil);
  if (pixelFormat == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;

  // Shared MSAA-resolve with CreateRenderTarget / CheckDeviceMultiSampleType.
  auto [sampleCount, msHr] = multisample_type_to_metal_sample_count(MultiSample, MultisampleQuality, m_metalDevice);
  if (FAILED(msHr))
    return msHr;

  // Always Private. dxmt's encoder layer splits a frame across multiple
  // command encoders (on a Clear, an RT/DS change, or a blit), and a
  // Memoryless depth attachment does not survive an encoder boundary, so
  // a later encoder's load=Load would read back undefined data mid-frame.
  // D3D9's Discard hint only means the contents need not survive a Present
  // or a SetDepthStencilSurface swap; it does not let the app manage Metal
  // encoder boundaries, so it cannot select Memoryless here.
  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = sampleCount > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = sampleCount;
  // Depth-stencil attachments need .renderTarget on Apple GPUs;
  // ShaderRead is intentionally omitted; depth-textures (the
  // shadow-map case) come from CreateTexture with D3DUSAGE_DEPTHSTENCIL,
  // not from CreateDepthStencilSurface.
  info.usage = WMTTextureUsageRenderTarget;
  info.options = WMTResourceStorageModePrivate;
  (void)Discard;
  Rc<dxmt::Texture> dxmt_texture = new dxmt::Texture(info, m_metalDevice);
  dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
  alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = dxmt_texture->allocate(alloc_flags);
  if (!allocation || !allocation->texture())
    return D3DERR_OUTOFVIDEOMEMORY;
  WMT::Texture rawTex = allocation->texture();
  dxmt_texture->rename(std::move(allocation));

  D3DSURFACE_DESC desc{};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_DEPTHSTENCIL;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = MultiSample;
  desc.MultiSampleQuality = MultisampleQuality;
  desc.Width = Width;
  desc.Height = Height;

  auto *surface = new MTLD3D9Surface(
      this, desc,
      /*container=*/static_cast<IDirect3DDevice9 *>(this), WMT::Reference<WMT::Texture>(rawTex),
      /*mipLevel=*/0,
      /*selfPin=*/true,
      /*parentTextureType=*/WMTTextureType2D,
      /*buffer=*/{},
      /*cpuPtr=*/nullptr,
      /*pitch=*/0,
      /*arraySlice=*/0,
      /*ownedBacking=*/nullptr,
      /*dxmtTexture=*/std::move(dxmt_texture)
  );
  // CreateDepthStencilSurface surfaces are always D3DPOOL_DEFAULT.
  surface->markLosable();
  surface->AddRef();
  *ppSurface = surface;
  return D3D_OK;
}
// UpdateSurface: SYSTEMMEM → DEFAULT blit. Symmetric inverse of
// GetRenderTargetData. Validation per DXVK d3d9_device.cpp.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::UpdateSurface(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface,
    const POINT *pDestPoint
) {
  if (!pSourceSurface || !pDestinationSurface)
    return D3DERR_INVALIDCALL;
  auto *src = static_cast<MTLD3D9Surface *>(pSourceSurface);
  auto *dst = static_cast<MTLD3D9Surface *>(pDestinationSurface);
  if (src->deviceRaw() != this || dst->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  const D3DSURFACE_DESC &sd = src->desc();
  const D3DSURFACE_DESC &dd = dst->desc();
  if (sd.Pool != D3DPOOL_SYSTEMMEM || dd.Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  if (sd.Format != dd.Format)
    return D3DERR_INVALIDCALL;
  if (sd.MultiSampleType != D3DMULTISAMPLE_NONE || dd.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;
  // UpdateSurface has no defined depth/stencil semantics; wined3d's
  // wined3d_device_update_sub_resource rejects on either aspect, DXVK
  // d3d9_device.cpp does the same via the dst-Usage RT/DS gate (a DS
  // pool=DEFAULT dst always carries D3DUSAGE_DEPTHSTENCIL). Format-equal
  // on both sides means one check covers src and dst.
  if (IsDepthStencilFormat(sd.Format))
    return D3DERR_INVALIDCALL;

  uint32_t src_x0 = 0, src_y0 = 0;
  uint32_t extent_w = sd.Width, extent_h = sd.Height;
  if (pSourceRect) {
    if (pSourceRect->left < 0 || pSourceRect->top < 0 || pSourceRect->right <= pSourceRect->left ||
        pSourceRect->bottom <= pSourceRect->top || (uint32_t)pSourceRect->right > sd.Width ||
        (uint32_t)pSourceRect->bottom > sd.Height)
      return D3DERR_INVALIDCALL;
    src_x0 = pSourceRect->left;
    src_y0 = pSourceRect->top;
    extent_w = pSourceRect->right - pSourceRect->left;
    extent_h = pSourceRect->bottom - pSourceRect->top;
  }
  uint32_t dst_x0 = 0, dst_y0 = 0;
  if (pDestPoint) {
    if (pDestPoint->x < 0 || pDestPoint->y < 0)
      return D3DERR_INVALIDCALL;
    dst_x0 = pDestPoint->x;
    dst_y0 = pDestPoint->y;
  }
  if (dst_x0 + extent_w > dd.Width || dst_y0 + extent_h > dd.Height)
    return D3DERR_INVALIDCALL;
  // BC compressed formats require 4x4 block alignment on RECT edges +
  // dst point. DXVK d3d9_device.cpp enforces. Without this,
  // an unaligned rect (e.g. (1, 1)..(33, 33) into a BC1 dst) smashes
  // the dst blit at the Metal level. Exception: full-extent locks
  // that round up to the texture extent are allowed even if the
  // texture's nominal width/height isn't a multiple of 4 (DXVK same
  // shape: apps creating sub-block-sized BC textures via mip chains
  // are a real pattern). Detect on the (DXT*/BC*) Format set.
  switch (sd.Format) {
  case D3DFMT_DXT1:
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5: {
    auto block_aligned = [](uint32_t v, uint32_t extent) { return (v % 4u == 0u) || (v == extent); };
    if (!block_aligned(src_x0, sd.Width) || !block_aligned(src_y0, sd.Height) ||
        !block_aligned(src_x0 + extent_w, sd.Width) || !block_aligned(src_y0 + extent_h, sd.Height) ||
        !block_aligned(dst_x0, dd.Width) || !block_aligned(dst_y0, dd.Height) ||
        !block_aligned(dst_x0 + extent_w, dd.Width) || !block_aligned(dst_y0 + extent_h, dd.Height))
      return D3DERR_INVALIDCALL;
    break;
  }
  default:
    break;
  }

  // Drain any queued batched draws onto a chunk first so their writes
  // serialise before this blit on the EncodingThread's queue (Metal
  // queue ordering is caller-issue FIFO). Then post the blit as its own
  // chunk lambda so the blit runs on EncodingThread, not the calling
  // thread.
  if (!m_pendingOps.empty())
    FlushDrawBatch();

  // Stage the SYSMEM source through the upload ring instead of a
  // deferred blit from its host-backed buffer: the app frees the
  // SYSMEM source right after UpdateSurface, so a buffer-direct copy
  // reads freed memory by the time the encode-thread blit runs. Same
  // lifetime shape as the UpdateTexture path: DXVK AllocStagingBuffer.
  if (void *src_host = src->cpuPtr()) {
    const bool compressed = IsCompressedFormat(sd.Format);
    uint64_t row_off, col_off;
    if (compressed) {
      uint32_t block_bytes = (sd.Format == D3DFMT_DXT1) ? 8u : 16u;
      row_off = static_cast<uint64_t>(src_y0 >> 2) * src->pitch();
      col_off = static_cast<uint64_t>(src_x0 >> 2) * block_bytes;
    } else {
      row_off = static_cast<uint64_t>(src_y0) * src->pitch();
      col_off = static_cast<uint64_t>(src_x0) * D3DFormatBytesPerPixel(sd.Format);
    }
    const uint8_t *src_ptr = static_cast<const uint8_t *>(src_host) + row_off + col_off;
    stageTextureUpload(
        dst->metalTexture(), dst->mipLevel(), /*slice=*/0, WMTOrigin{dst_x0, dst_y0, 0}, WMTSize{extent_w, extent_h, 1},
        src_ptr, src->pitch(), compressed
    );
    dst->flagContainerAutoGenDirty();
    return D3D_OK;
  }

  // Capture the source's host backing (SYSMEM Shared MTLBuffer) and
  // the source/destination Metal textures by retaining handles. The
  // chunk lambda runs on EncodingThread; the retains pin the Metal
  // resources beyond the calling thread's next Set*/Release.
  WMT::Reference<WMT::Buffer> src_buf_retain(src->metalBuffer());
  WMT::Reference<WMT::Texture> src_tex_retain(src->metalTexture());
  WMT::Reference<WMT::Texture> dst_tex_retain(dst->metalTexture());
  obj_handle_t src_buffer_handle = src->metalBuffer().handle;
  obj_handle_t src_texture_handle = src->metalTexture().handle;
  obj_handle_t dst_texture_handle = dst->metalTexture().handle;
  uint32_t src_pitch = src->pitch();
  uint32_t src_mip = src->mipLevel();
  uint32_t dst_mip = dst->mipLevel();
  uint32_t bpp = D3DFormatBytesPerPixel(sd.Format);

  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;

  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([src_buf_retain = std::move(src_buf_retain), src_tex_retain = std::move(src_tex_retain),
                 dst_tex_retain = std::move(dst_tex_retain), src_buffer_handle, src_texture_handle, dst_texture_handle,
                 src_pitch, src_mip, dst_mip, bpp, src_x0, src_y0, dst_x0, dst_y0, extent_w, extent_h, event_handle,
                 signal_seq](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();
    // Symmetric to GetRenderTargetData: the linear-texture-of-buffer
    // view path drops trailing rows on virtualized Apple Silicon;
    // route the copy through the buffer with explicit bytesPerRow.
    if (src_buffer_handle != 0) {
      auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
      cmd.type = WMTBlitCommandCopyFromBufferToTexture;
      cmd.src = src_buffer_handle;
      cmd.src_offset = static_cast<uint64_t>(src_y0) * src_pitch + static_cast<uint64_t>(src_x0) * bpp;
      cmd.bytes_per_row = src_pitch;
      cmd.bytes_per_image = src_pitch * extent_h;
      cmd.size = WMTSize{extent_w, extent_h, 1};
      cmd.dst = dst_texture_handle;
      cmd.slice = 0;
      cmd.level = dst_mip;
      cmd.origin = WMTOrigin{dst_x0, dst_y0, 0};
    } else {
      auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
      cmd.type = WMTBlitCommandCopyFromTextureToTexture;
      cmd.src = src_texture_handle;
      cmd.src_slice = 0;
      cmd.src_level = src_mip;
      cmd.src_origin = WMTOrigin{src_x0, src_y0, 0};
      cmd.src_size = WMTSize{extent_w, extent_h, 1};
      cmd.dst = dst_texture_handle;
      cmd.dst_slice = 0;
      cmd.dst_level = dst_mip;
      cmd.dst_origin = WMTOrigin{dst_x0, dst_y0, 0};
    }
    ctx.endPass();
    // Pair with FlushDrawBatch's signal-tail pattern; keeps the
    // const/upload-ring's free_blocks(signaledValue) recycling correct
    // and pins the captured retains until GPU-side completion.
    ctx.signalEventByHandle(event_handle, signal_seq);
  });
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();
  // Same-queue: next draw after this cmdbuf; no waitUntilCompleted needed.
  // AUTOGENMIPMAP: flag dest texture's auto-gen state (StretchRect/Clear pattern).
  dst->flagContainerAutoGenDirty();
  return D3D_OK;
}
// UpdateTexture: SYSTEMMEM/MANAGED master → DEFAULT/MANAGED mirror (push to GPU).
// Validation: same type/format/level-0 dims, src_levels >= dst_levels.
// MANAGED Lock/Unlock already pushes per-Unlock (independent push path).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) {
  // Wine main thread has no outer NSAutoreleasePool; the upload path
  // touches Metal APIs (texture view, fence access) that return
  // autoreleased handles, so create one here.
  auto pool = WMT::MakeAutoreleasePool();
  if (!pSourceTexture || !pDestinationTexture)
    return D3DERR_INVALIDCALL;
  if (pSourceTexture == pDestinationTexture)
    return D3DERR_INVALIDCALL;
  D3DRESOURCETYPE src_type = pSourceTexture->GetType();
  D3DRESOURCETYPE dst_type = pDestinationTexture->GetType();
  if (src_type != dst_type)
    return D3DERR_INVALIDCALL;
  if (src_type != D3DRTYPE_TEXTURE && src_type != D3DRTYPE_CUBETEXTURE && src_type != D3DRTYPE_VOLUMETEXTURE)
    return D3DERR_NOTAVAILABLE;

  // Common validation: pool gates, format/dimension match. Accepts {SYSTEMMEM,MANAGED}→{DEFAULT,MANAGED} (superset of
  // spec). Liberal: MANAGED cases no-op (auto-pushes on Lock/Unlock); some apps depend on success.
  auto check_pools = [](D3DPOOL src_pool, D3DPOOL dst_pool) {
    if (src_pool != D3DPOOL_SYSTEMMEM && src_pool != D3DPOOL_MANAGED)
      return D3DERR_INVALIDCALL;
    if (dst_pool != D3DPOOL_DEFAULT && dst_pool != D3DPOOL_MANAGED)
      return D3DERR_INVALIDCALL;
    return D3D_OK;
  };

  if (src_type == D3DRTYPE_TEXTURE) {
    auto *src = static_cast<MTLD3D9Texture *>(pSourceTexture);
    auto *dst = static_cast<MTLD3D9Texture *>(pDestinationTexture);
    if (src->deviceRaw() != this || dst->deviceRaw() != this)
      return D3DERR_INVALIDCALL;
    if (HRESULT hr = check_pools(src->pool(), dst->pool()); FAILED(hr))
      return hr;
    if (src->d3dFormat() != dst->d3dFormat())
      return D3DERR_INVALIDCALL;
    // DXVK d3d9_device.cpp; dst can't have more levels than
    // src unless dst has D3DUSAGE_AUTOGENMIPMAP (in which case the
    // missing levels regenerate from the uploaded base).
    const UINT src_levels = src->levelCount();
    const UINT dst_levels = dst->levelCount();
    if (dst_levels > src_levels && !(dst->usage() & D3DUSAGE_AUTOGENMIPMAP))
      return D3DERR_INVALIDCALL;
    // Per-spec mip-tail correspondence (DXVK d3d9_device.cpp):
    // when src has more levels than dst, the src bottom-tail of size
    // dst.MipLevels maps onto dst, not src's top. src_level_offset is
    // the offset to add to dst's level index to get the src level. For
    // matching-chain apps (the common case) src_levels == dst_levels
    // and the offset is zero, so the loop is byte-identical to the
    // pre-this shape.
    const UINT src_level_offset = src_levels > dst_levels ? src_levels - dst_levels : 0;
    D3DSURFACE_DESC src_tail{}, dst0{};
    if (FAILED(src->GetLevelDesc(src_level_offset, &src_tail)) || FAILED(dst->GetLevelDesc(0, &dst0)))
      return D3DERR_INVALIDCALL;
    if (src_tail.Width != dst0.Width || src_tail.Height != dst0.Height)
      return D3DERR_INVALIDCALL;
    WMT::Buffer src_mirror = src->mirrorBuffer();
    if (src_mirror == nullptr)
      return D3DERR_INVALIDCALL;
    WMT::Texture dst_tex = dst->metalTexture();
    if (dst_tex == nullptr)
      return D3DERR_INVALIDCALL;
    // Sub-E: walk only the dirty region. wined3d texture.c::texture_resource_sub_resource_unmap
    // records dirty at level-0 coords; consumer scales down per level
    // by >> level. If src isn't dirty, UpdateTexture is a no-op (the
    // GPU side already reflects the source's current contents).
    if (!src->isDirty())
      return D3D_OK;
    const RECT dr0 = src->dirtyRectLevel0();
    const bool compressed = IsCompressedFormat(src->d3dFormat());
    const UINT levels_to_copy = std::min(src_levels - src_level_offset, dst_levels);
    for (UINT dst_level = 0; dst_level < levels_to_copy; ++dst_level) {
      const UINT src_level = src_level_offset + dst_level;
      D3DSURFACE_DESC d{};
      if (FAILED(src->GetLevelDesc(src_level, &d)))
        continue;
      // Scale level-0 dirty rect down to src_level coords. Round-out
      // for safety (a partially-touched pixel at level N may come from
      // multiple level-0 pixels). For compressed formats, clamp to the
      // 4x4 block grid in src_level coords.
      LONG l = dr0.left >> src_level, t = dr0.top >> src_level;
      LONG r = (dr0.right + ((1 << src_level) - 1)) >> src_level;
      LONG b = (dr0.bottom + ((1 << src_level) - 1)) >> src_level;
      if (compressed) {
        l &= ~3;
        t &= ~3;
        r = (r + 3) & ~3;
        b = (b + 3) & ~3;
      }
      LONG lw = static_cast<LONG>(d.Width), lh = static_cast<LONG>(d.Height);
      if (l < 0)
        l = 0;
      if (t < 0)
        t = 0;
      if (r > lw)
        r = lw;
      if (b > lh)
        b = lh;
      if (r <= l || b <= t)
        continue;
      WMTOrigin origin{};
      origin.x = static_cast<uint32_t>(l);
      origin.y = static_cast<uint32_t>(t);
      origin.z = 0;
      WMTSize size{};
      size.width = static_cast<uint32_t>(r - l);
      size.height = static_cast<uint32_t>(b - t);
      size.depth = 1;
      uint32_t src_pitch = D3DFormatRowPitch(src->d3dFormat(), d.Width);
      if (src_pitch == 0)
        continue;
      // Byte offset into the mirror for the dirty sub-rect: the level
      // starts at mirrorOffset(src_level); within the level, the rect
      // origin shifts by t rows × pitch + l columns × bpp (compressed:
      // block-row pitch × block-row + block-column bytes).
      uint64_t row_off, col_off;
      if (compressed) {
        // DXT1: 8 bytes/block. DXT2-5: 16 bytes/block. Same switch as
        // D3DFormatRowPitch: kept inline since this is the only caller.
        uint32_t bytes_per_block = (src->d3dFormat() == D3DFMT_DXT1) ? 8u : 16u;
        row_off = static_cast<uint64_t>(t >> 2) * src_pitch;
        col_off = static_cast<uint64_t>(l >> 2) * bytes_per_block;
      } else {
        row_off = static_cast<uint64_t>(t) * src_pitch;
        col_off = static_cast<uint64_t>(l) * D3DFormatBytesPerPixel(src->d3dFormat());
      }
      // Stage the copy through the upload ring, not a direct blit from the
      // source's mirror buffer: the app frees the SYSTEMMEM source right after
      // UpdateTexture, but the blit chunk runs later on the encode thread, so a
      // buffer-direct copy would read freed host memory. The ring copy runs now
      // while the source is alive, into lifetime-safe storage the chunk owns.
      // Matches DXVK (AllocStagingBuffer + packImageData) and wined3d upload_bo.
      if (src->mirrorBase() == nullptr)
        continue;
      const uint8_t *src_ptr =
          static_cast<const uint8_t *>(src->mirrorBase()) + src->mirrorOffset(src_level) + row_off + col_off;
      stageTextureUpload(dst_tex, dst_level, /*slice=*/0, origin, size, src_ptr, src_pitch, compressed);
    }
    src->clearDirty();
    // wined3d device.c flags the dst's auto-gen mipmap state after
    // UpdateTexture success: mirrors the d3d9_texture_flag_auto_gen_mipmap
    // call in the GL path. flagAutoGenDirty is a no-op unless dst was
    // created with D3DUSAGE_AUTOGENMIPMAP.
    dst->flagAutoGenDirty();
    return D3D_OK;
  }

  if (src_type == D3DRTYPE_VOLUMETEXTURE) {
    auto *src = static_cast<MTLD3D9VolumeTexture *>(pSourceTexture);
    auto *dst = static_cast<MTLD3D9VolumeTexture *>(pDestinationTexture);
    if (src->deviceRaw() != this || dst->deviceRaw() != this)
      return D3DERR_INVALIDCALL;
    if (HRESULT hr = check_pools(src->pool(), dst->pool()); FAILED(hr))
      return hr;
    if (src->d3dFormat() != dst->d3dFormat())
      return D3DERR_INVALIDCALL;
    // Mip-count + mip-tail correspondence: same shape as the 2D path;
    // see comments there for the spec reference.
    const UINT src_levels = src->levelCount();
    const UINT dst_levels = dst->levelCount();
    if (dst_levels > src_levels && !(dst->usage() & D3DUSAGE_AUTOGENMIPMAP))
      return D3DERR_INVALIDCALL;
    const UINT src_level_offset = src_levels > dst_levels ? src_levels - dst_levels : 0;
    D3DVOLUME_DESC src_tail{}, dst0{};
    if (FAILED(src->GetLevelDesc(src_level_offset, &src_tail)) || FAILED(dst->GetLevelDesc(0, &dst0)))
      return D3DERR_INVALIDCALL;
    if (src_tail.Width != dst0.Width || src_tail.Height != dst0.Height || src_tail.Depth != dst0.Depth)
      return D3DERR_INVALIDCALL;
    const uint8_t *src_base = src->mirrorBase();
    if (!src_base)
      return D3DERR_INVALIDCALL;
    WMT::Texture dst_tex = dst->metalTexture();
    if (dst_tex == nullptr)
      return D3DERR_INVALIDCALL;
    if (!src->isDirty())
      return D3D_OK;
    const D3DBOX db0 = src->dirtyBoxLevel0();
    const UINT levels_to_copy = std::min(src_levels - src_level_offset, dst_levels);
    const uint32_t bpp = D3DFormatBytesPerPixel(src->d3dFormat());
    for (UINT dst_level = 0; dst_level < levels_to_copy; ++dst_level) {
      const UINT src_level = src_level_offset + dst_level;
      D3DVOLUME_DESC d{};
      if (FAILED(src->GetLevelDesc(src_level, &d)))
        continue;
      // Scale level-0 dirty box to src_level coords (round-out).
      uint32_t l = db0.Left >> src_level, t = db0.Top >> src_level, f = db0.Front >> src_level;
      uint32_t r = (db0.Right + ((1u << src_level) - 1u)) >> src_level;
      uint32_t b = (db0.Bottom + ((1u << src_level) - 1u)) >> src_level;
      uint32_t bk = (db0.Back + ((1u << src_level) - 1u)) >> src_level;
      if (r > d.Width)
        r = d.Width;
      if (b > d.Height)
        b = d.Height;
      if (bk > d.Depth)
        bk = d.Depth;
      if (r <= l || b <= t || bk <= f)
        continue;
      WMTOrigin origin{};
      origin.x = l;
      origin.y = t;
      origin.z = f;
      WMTSize size{};
      size.width = r - l;
      size.height = b - t;
      size.depth = bk - f;
      uint32_t src_pitch = D3DFormatRowPitch(src->d3dFormat(), d.Width);
      if (src_pitch == 0 || bpp == 0)
        continue;
      // 3D mirror layout: level base + slice_pitch×Front + row_pitch×Top + bpp×Left.
      uint32_t slice_pitch = src_pitch * d.Height;
      const uint8_t *src_ptr = src_base + src->mirrorOffset(src_level) + static_cast<size_t>(f) * slice_pitch +
                               static_cast<size_t>(t) * src_pitch + static_cast<size_t>(l) * bpp;
      // slice=0 for 3D textures; depth lives in the Origin/Size triplet,
      // not in the array dimension. slice_pitch threads the source's
      // per-depth-slice stride so every slice is staged, not just the first.
      stageTextureUpload(dst_tex, dst_level, 0, origin, size, src_ptr, src_pitch, /*compressed=*/false, slice_pitch);
    }
    src->clearDirty();
    return D3D_OK;
  }

  // Cube branch: same shape, per face × level. Cube mirror is a
  // plain std::vector<uint8_t> (no Metal buffer), so the upload routes
  // through stageTextureUpload (CPU pointer + staging-ring memcpy)
  // rather than the buffer-direct path.
  {
    auto *src = static_cast<MTLD3D9CubeTexture *>(pSourceTexture);
    auto *dst = static_cast<MTLD3D9CubeTexture *>(pDestinationTexture);
    if (src->deviceRaw() != this || dst->deviceRaw() != this)
      return D3DERR_INVALIDCALL;
    if (HRESULT hr = check_pools(src->pool(), dst->pool()); FAILED(hr))
      return hr;
    if (src->d3dFormat() != dst->d3dFormat())
      return D3DERR_INVALIDCALL;
    // Mip-count + mip-tail correspondence: same shape as the 2D path;
    // see comments there for the spec reference.
    const UINT src_levels = src->levelCount();
    const UINT dst_levels = dst->levelCount();
    if (dst_levels > src_levels && !(dst->usage() & D3DUSAGE_AUTOGENMIPMAP))
      return D3DERR_INVALIDCALL;
    const UINT src_level_offset = src_levels > dst_levels ? src_levels - dst_levels : 0;
    D3DSURFACE_DESC src_tail{}, dst0{};
    if (FAILED(src->GetLevelDesc(src_level_offset, &src_tail)) || FAILED(dst->GetLevelDesc(0, &dst0)))
      return D3DERR_INVALIDCALL;
    if (src_tail.Width != dst0.Width)
      return D3DERR_INVALIDCALL;
    const uint8_t *src_base = src->mirrorBase();
    if (!src_base)
      return D3DERR_INVALIDCALL;
    WMT::Texture dst_tex = dst->metalTexture();
    if (dst_tex == nullptr)
      return D3DERR_INVALIDCALL;
    const bool compressed = IsCompressedFormat(src->d3dFormat());
    const UINT levels_to_copy = std::min(src_levels - src_level_offset, dst_levels);
    const uint32_t bpp = compressed ? 0u : D3DFormatBytesPerPixel(src->d3dFormat());
    const uint32_t bytes_per_block = compressed ? (src->d3dFormat() == D3DFMT_DXT1 ? 8u : 16u) : 0u;
    for (uint32_t face = 0; face < 6; ++face) {
      if (!src->isDirty(face))
        continue;
      const RECT dr0 = src->dirtyRectLevel0(face);
      for (UINT dst_level = 0; dst_level < levels_to_copy; ++dst_level) {
        const UINT src_level = src_level_offset + dst_level;
        D3DSURFACE_DESC d{};
        if (FAILED(src->GetLevelDesc(src_level, &d)))
          continue;
        LONG l = dr0.left >> src_level, t = dr0.top >> src_level;
        LONG r = (dr0.right + ((1 << src_level) - 1)) >> src_level;
        LONG b = (dr0.bottom + ((1 << src_level) - 1)) >> src_level;
        if (compressed) {
          l &= ~3;
          t &= ~3;
          r = (r + 3) & ~3;
          b = (b + 3) & ~3;
        }
        LONG lw = static_cast<LONG>(d.Width), lh = static_cast<LONG>(d.Height);
        if (l < 0)
          l = 0;
        if (t < 0)
          t = 0;
        if (r > lw)
          r = lw;
        if (b > lh)
          b = lh;
        if (r <= l || b <= t)
          continue;
        WMTOrigin origin{};
        origin.x = static_cast<uint32_t>(l);
        origin.y = static_cast<uint32_t>(t);
        origin.z = 0;
        WMTSize size{};
        size.width = static_cast<uint32_t>(r - l);
        size.height = static_cast<uint32_t>(b - t);
        size.depth = 1;
        uint32_t src_pitch = D3DFormatRowPitch(src->d3dFormat(), d.Width);
        if (src_pitch == 0)
          continue;
        size_t row_off, col_off;
        if (compressed) {
          row_off = static_cast<size_t>(t >> 2) * src_pitch;
          col_off = static_cast<size_t>(l >> 2) * bytes_per_block;
        } else {
          row_off = static_cast<size_t>(t) * src_pitch;
          col_off = static_cast<size_t>(l) * bpp;
        }
        const void *src_ptr = src_base + src->mirrorOffset(face, src_level) + row_off + col_off;
        // slice=face: cube faces are array slices on a MTLTextureCube;
        // stageTextureUpload's slice parameter routes the blit to the
        // correct face plane.
        stageTextureUpload(dst_tex, dst_level, face, origin, size, src_ptr, src_pitch, compressed);
      }
      src->clearDirty(face);
    }
    // Cube AUTOGENMIPMAP regen: same shape as the 2D branch (wined3d
    // device.c). Flag the dst's lazy mipmap-dirty bit; no-op
    // unless the cube was created with D3DUSAGE_AUTOGENMIPMAP.
    dst->flagAutoGenDirty();
    return D3D_OK;
  }
}
// GetRenderTargetData: RT → SYSTEMMEM blit. Validation per DXVK
// d3d9_device.cpp. DEFAULT-pool dst would forward to StretchRect
// in DXVK; we return INVALIDCALL until StretchRect lands.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) {
  // Wine main thread has no outer NSAutoreleasePool. GetRenderTargetData
  // commits a sync chunk and waits; autoreleased Metal handles (blit
  // encoder, fence) leak across every screenshot capture without it.
  auto pool = WMT::MakeAutoreleasePool();
  // TODO: IsDeviceLost early-return (DXVK) once Reset/Lost lands.
  if (!pRenderTarget || !pDestSurface)
    return D3DERR_INVALIDCALL;
  auto *src = static_cast<MTLD3D9Surface *>(pRenderTarget);
  auto *dst = static_cast<MTLD3D9Surface *>(pDestSurface);
  if (src->deviceRaw() != this || dst->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (src == dst)
    return D3D_OK;
  const D3DSURFACE_DESC &sd = src->desc();
  const D3DSURFACE_DESC &dd = dst->desc();
  if (sd.Format != dd.Format)
    return D3DERR_INVALIDCALL;
  // TODO: when surfaces expose sub-level mips (CreateTexture +
  // GetSurfaceLevel path), compare mip-extent of (texture, mipLevel)
  // rather than the level-0 desc.Width/Height stored here.
  if (sd.Width != dd.Width || sd.Height != dd.Height)
    return D3DERR_INVALIDCALL;
  if (dd.Pool == D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  if (sd.MultiSampleType != D3DMULTISAMPLE_NONE)
    return D3DERR_INVALIDCALL;

  // TODO: device lock once dxmt becomes multithreaded.
  // Drain queued draws and any staged clear onto chunks first so the
  // read sees them. Then post the blit as its own chunk lambda + wait
  // on the chunk's completion before returning so the caller's next
  // LockRect sees fresh bytes.
  FlushDrawBatch();
  flushOpenWork();

  WMT::Reference<WMT::Texture> src_tex_retain(src->metalTexture());
  WMT::Reference<WMT::Texture> dst_tex_retain(dst->metalTexture());
  WMT::Reference<WMT::Buffer> dst_buf_retain(dst->metalBuffer());
  obj_handle_t src_texture_handle = src->metalTexture().handle;
  obj_handle_t dst_texture_handle = dst->metalTexture().handle;
  obj_handle_t dst_buffer_handle = dst->metalBuffer().handle;
  uint32_t src_mip = src->mipLevel();
  uint32_t dst_mip = dst->mipLevel();
  uint32_t dst_pitch = dst->pitch();
  uint32_t width = sd.Width;
  uint32_t height = sd.Height;
  uint32_t dst_height = dd.Height;

  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;

  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([src_tex_retain = std::move(src_tex_retain), dst_tex_retain = std::move(dst_tex_retain),
                 dst_buf_retain = std::move(dst_buf_retain), src_texture_handle, dst_texture_handle, dst_buffer_handle,
                 src_mip, dst_mip, dst_pitch, width, height, dst_height, event_handle,
                 signal_seq](ArgumentEncodingContext &ctx) mutable {
    ctx.startBlitPass();
    // Prefer copyFromTexture:toBuffer: when dst has a host-visible
    // buffer backing. The linear-texture-of-buffer view path drops
    // trailing rows on virtualized Apple Silicon; route the copy
    // through the buffer with explicit bytesPerRow.
    if (dst_buffer_handle != 0) {
      auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
      cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
      cmd.src = src_texture_handle;
      cmd.slice = 0;
      cmd.level = src_mip;
      cmd.origin = WMTOrigin{0, 0, 0};
      cmd.size = WMTSize{width, height, 1};
      cmd.dst = dst_buffer_handle;
      cmd.offset = 0;
      cmd.bytes_per_row = dst_pitch;
      cmd.bytes_per_image = dst_pitch * dst_height;
    } else {
      auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
      cmd.type = WMTBlitCommandCopyFromTextureToTexture;
      cmd.src = src_texture_handle;
      cmd.src_slice = 0;
      cmd.src_level = src_mip;
      cmd.src_origin = WMTOrigin{0, 0, 0};
      cmd.src_size = WMTSize{width, height, 1};
      cmd.dst = dst_texture_handle;
      cmd.dst_slice = 0;
      cmd.dst_level = dst_mip;
      cmd.dst_origin = WMTOrigin{0, 0, 0};
    }
    ctx.endPass();
    ctx.signalEventByHandle(event_handle, signal_seq);
  });
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();

  // Synchronous from the app's perspective; the destination is
  // mapped immediately after this call by LockRect, so wait for the
  // chunk's GPU-side commit to complete before returning. The blit
  // itself ran on EncodingThread; the calling thread blocks only on
  // its retirement.
  uint64_t seq = m_dxmtQueue->CurrentSeqId();
  m_dxmtQueue->CommitCurrentChunk();
  m_dxmtQueue->WaitCPUFence(seq);
  // Also ensure the GPU has actually retired the cmdbuf; WaitCPUFence
  // waits for the chunk's encode thread; the per-cmdbuf m_completionEvent
  // signal is what tells us the GPU side is done. m_currentCmdSeq was
  // bumped after posting, so the chunk's signal target is the pre-bump
  // value.
  m_completionEvent.waitUntilSignaledValue(signal_seq, UINT64_MAX);
  return D3D_OK;
}
// GetFrontBufferData: backbuffer → SYSMEM blit.
// Swapchain uses single persistent backbuffer (same as DXVK windowed shortcut).
// Same-extent + same-format only; resolve/stretch/convert deferred.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) { return E_NOTIMPL; }

// StretchRect: DEFAULT→DEFAULT surface blit. Validation per DXVK
// d3d9_device.cpp. MVP path: same-format, same-extent, no MSAA,
// no depth-stencil. Stretch / format-convert / resolve / DS land in
// follow-ups (each routes through a different Metal path: render-pass
// blit, MTLBlitCommandEncoder copy with format reinterpret, or a DS-
// aware copy that respects aspectMask).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::StretchRect(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, const RECT *pDestRect,
    D3DTEXTUREFILTERTYPE Filter
) { return E_NOTIMPL; }

// ColorFill: solid color via render-pass clear (empty pass, loadAction=Clear).
// MVP scope: full-surface only (subrect needs draw path).
// All DEFAULT-pool color surfaces are RT-capable (promotion).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR Color) {
  // Wine main thread has no outer NSAutoreleasePool. Clear-encoder
  // chunk emit touches Metal APIs (view, fence) that return
  // autoreleased handles.
  auto pool = WMT::MakeAutoreleasePool();
  if (!pSurface)
    return D3DERR_INVALIDCALL;
  auto *dst = static_cast<MTLD3D9Surface *>(pSurface);
  if (dst->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  const D3DSURFACE_DESC &dd = dst->desc();
  if (dd.Pool != D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  // DXVK gates on `aspectMask != COLOR_BIT`, which is broader than just
  // DS rejection. dxmt's DEFAULT-pool surface-creation paths
  // (CreateRenderTarget, CreateOffscreenPlainSurface, CreateTexture)
  // all reject formats that don't lower via D3DFormatToMetal, so the
  // only non-color DEFAULT surfaces we can construct are DS; making
  // the IsDepthStencilFormat check sufficient at this site.
  if (IsDepthStencilFormat(dd.Format))
    return D3DERR_INVALIDCALL;
  // Compressed-format DEFAULT surfaces (CreateOffscreenPlainSurface on
  // BC1/2/3) are allocated with ShaderRead usage only; no RenderTarget
  // bit because Apple Silicon rejects BC formats as RT. ctx.clearColor
  // opens a render pass against this surface as RT and would fail at
  // Metal encode-time with an opaque validation error. Reject up front
  // with the documented INVALIDCALL so the app sees a clean fallback
  // path instead.
  if (IsCompressedFormat(dd.Format))
    return D3DERR_INVALIDCALL;
  // Resolve the fill rect. NULL means full surface; otherwise validate
  // against the surface bounds (wined3d/DXVK both INVALIDCALL on an
  // out-of-bounds or inverted rect). The full-surface shortcut bypasses
  // the render-pass quad path entirely; it stays on the cheap
  // loadAction=Clear coalesce.
  uint32_t fill_x = 0, fill_y = 0;
  uint32_t fill_w = dd.Width, fill_h = dd.Height;
  bool full_surface = true;
  if (pRect) {
    if (pRect->left < 0 || pRect->top < 0 || pRect->right <= pRect->left || pRect->bottom <= pRect->top ||
        (uint32_t)pRect->right > dd.Width || (uint32_t)pRect->bottom > dd.Height)
      return D3DERR_INVALIDCALL;
    fill_x = pRect->left;
    fill_y = pRect->top;
    fill_w = pRect->right - pRect->left;
    fill_h = pRect->bottom - pRect->top;
    full_surface = (fill_x == 0 && fill_y == 0 && fill_w == dd.Width && fill_h == dd.Height);
  }

  const double r = ((Color >> 16) & 0xFF) / 255.0;
  const double g = ((Color >> 8) & 0xFF) / 255.0;
  const double b = (Color & 0xFF) / 255.0;
  const double a = ((Color >> 24) & 0xFF) / 255.0;

  // ColorFill posts a chunk lambda that routes through ctx.clearColor.
  // d3d11's ClearRenderTargetView shape. The chunk's ClearEncoderData
  // fast-path coalesces with an immediately-following render pass against
  // the same attachment, matching d3d11's load-action folding. Drain
  // queued draws first so they land on their own attachments before this
  // clear retargets.
  if (!m_pendingOps.empty())
    FlushDrawBatch();

  // ColorFill requires a dxmt::Texture wrapper to take the ctx.access
  // path. Every DEFAULT-pool surface (CreateRenderTarget,
  // CreateOffscreenPlainSurface, CreateTexture with the RT-promotion
  // ctor) carries one; guard defensively against legacy callsites.
  Rc<dxmt::Texture> dst_tex = dst->dxmtTexture();
  if (!dst_tex)
    return D3DERR_INVALIDCALL;

  // Per-level + per-slice view of the surface, since ctx.clearColor's
  // ClearEncoderData carries a TextureViewRef. The Rc<>'s fullView is
  // the level-0/slice-0 view; for cube faces and mip surfaces the
  // MTLD3D9Surface's mipLevel() + arraySlice() select the right one.
  uint16_t dst_level = static_cast<uint16_t>(dst->mipLevel());
  uint16_t dst_slice = static_cast<uint16_t>(dst->arraySlice());
  TextureViewKey view_key = dst_tex->createView({
      .format = dst->metalPixelFormat(),
      .type = surface_view_type(dst_tex.ptr()),
      .firstMiplevel = dst_level,
      .miplevelCount = 1,
      .firstArraySlice = dst_slice,
      .arraySize = 1,
  });
  unsigned array_length = 1;

  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;

  auto *chunk = m_dxmtQueue->CurrentChunk();
  if (full_surface) {
    // Whole-attachment loadAction=Clear; coalesces with the next
    // render pass against the same RT into a single encoder.
    chunk->emitcc([dst_tex = std::move(dst_tex), view_key, array_length, r, g, b, a, event_handle,
                   signal_seq](ArgumentEncodingContext &ctx) mutable {
      ctx.clearColor(std::move(dst_tex), view_key, array_length, WMTClearColor{r, g, b, a});
      ctx.signalEventByHandle(event_handle, signal_seq);
    });
  } else {
    // Sub-rect path: load-then-scissored-clear via the render-pass
    // quad in ClearRenderTargetContext. Loses the loadAction=Clear
    // coalesce since the pass has loadAction=Load, but preserves the
    // out-of-rect pixels: which is the whole point of a sub-rect
    // ColorFill. DXVK's clearImageView with an extent maps to this
    // same render-pass-with-scissor pattern.
    std::array<float, 4> color_f = {
        static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), static_cast<float>(a)
    };
    chunk->emitcc([dst_tex = std::move(dst_tex), view_key, fill_x, fill_y, fill_w, fill_h, color_f, event_handle,
                   signal_seq](ArgumentEncodingContext &ctx) mutable {
      ctx.clear_rt_cmd.begin(std::move(dst_tex), view_key);
      ctx.clear_rt_cmd.clear(fill_x, fill_y, fill_w, fill_h, color_f);
      ctx.clear_rt_cmd.end();
      ctx.signalEventByHandle(event_handle, signal_seq);
    });
  }
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();
  // AUTOGENMIPMAP regen: DXVK d3d9_device.cpp calls
  // MarkTextureMipsDirty when IsAutomaticMip after a ColorFill. Routes
  // through the shared surface helper (see flagContainerAutoGenDirty)
  // so the standalone-surface / swapchain-backbuffer cases stay no-op.
  dst->flagContainerAutoGenDirty();
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) {
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  *ppSurface = nullptr;
  if (Width == 0 || Height == 0)
    return D3DERR_INVALIDCALL;
  if (Width > 16384 || Height > 16384)
    return D3DERR_INVALIDCALL;

  // wined3d device.c; D3DPOOL_MANAGED on offscreen plain is
  // contract-illegal (managed pool implies a GPU mirror, but offscreen
  // plain has no defined GPU-bind path that would feed the mirror).
  if (Pool == D3DPOOL_MANAGED)
    return D3DERR_INVALIDCALL;

  // pSharedHandle: non-Ex always E_NOTIMPL; Ex+SYSTEMMEM/DEFAULT not yet.
  // Collapse implementable branches to E_NOTIMPL; illegal to INVALIDCALL.
  if (pSharedHandle) {
    if (!m_isEx)
      return E_NOTIMPL;
    if (Pool != D3DPOOL_SYSTEMMEM && Pool != D3DPOOL_DEFAULT)
      return D3DERR_INVALIDCALL;
    return E_NOTIMPL;
  }

  // Depth-stencil formats are valid as sampleable textures (shadow
  // maps go through CreateTexture with D3DUSAGE_DEPTHSTENCIL), but the
  // plain-surface path has no defined DS attachment role; reject up
  // front rather than silently allocating something the runtime can't
  // bind.
  if (IsDepthStencilFormat(Format))
    return D3DERR_INVALIDCALL;

  WMTPixelFormat pixelFormat = D3DFormatToMetal(Format, D3D9FormatUsage::SampleableTexture);
  if (pixelFormat == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;

  // Pool → Metal storage. D3DPOOL_DEFAULT lives GPU-side and is the
  // legal StretchRect destination; SYSTEMMEM/SCRATCH live CPU-side and
  // are the legal UpdateSurface source. Apple Silicon's unified memory
  // makes Shared a zero-copy fit for the CPU pools.
  WMTResourceOptions storage;
  WMTTextureUsage usage;
  switch (Pool) {
  case D3DPOOL_DEFAULT:
    storage = WMTResourceStorageModePrivate;
    // ShaderRead so the surface can be a blit source / sampled texture
    // standin for StretchRect; RenderTarget so it can be a StretchRect
    // destination via render pass when the format gates blit out.
    // BC-compressed formats can't carry the RenderTarget bit on Apple
    // Silicon: same gate as CreateTexture. A DEFAULT-pool DXT
    // offscreen surface stays sampler-only; StretchRect to it goes
    // through the blit-encoder path, not a render pass.
    usage = IsCompressedFormat(Format) ? WMTTextureUsageShaderRead
                                       : (WMTTextureUsage)(WMTTextureUsageShaderRead | WMTTextureUsageRenderTarget);
    break;
  case D3DPOOL_SYSTEMMEM:
  case D3DPOOL_SCRATCH:
    storage = WMTResourceStorageModeShared;
    usage = WMTTextureUsageShaderRead;
    break;
  default:
    return D3DERR_INVALIDCALL;
  }

  WMTTextureInfo info{};
  info.pixel_format = pixelFormat;
  info.width = Width;
  info.height = Height;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = usage;
  info.options = storage;

  WMT::Reference<WMT::Texture> texture;
  WMT::Reference<WMT::Buffer> buffer;
  void *cpuPtr = nullptr;
  void *ownedBacking = nullptr;
  uint32_t pitch = 0;
  // DEFAULT offscreen: dxmt::Texture keeps MTLTexture alive (EncodingThread). fullView carries intendedUsage for
  // RT-substitution. SYSTEMMEM/SCRATCH: buffer-backed (can't add RenderTarget on Apple Silicon; lacks 32-bit
  // addressing).
  Rc<dxmt::Texture> dxmt_texture;

  if (Pool == D3DPOOL_DEFAULT) {
    dxmt_texture = new dxmt::Texture(info, m_metalDevice);
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
    auto allocation = dxmt_texture->allocate(alloc_flags);
    if (!allocation || !allocation->texture())
      return D3DERR_OUTOFVIDEOMEMORY;
    texture = WMT::Reference<WMT::Texture>(allocation->texture());
    dxmt_texture->rename(std::move(allocation));
    // DEFAULT surfaces always lockable (MSDN). Private texture can't carry CPU pointer; buffer-backed loses
    // RenderTarget (Apple Silicon disallows RT on linear textures). Solution: host-side mirror (MANAGED pattern).
    const uint32_t block_h = IsCompressedFormat(Format) ? 4u : 1u;
    pitch = D3DFormatRowPitch(Format, Width);
    if (pitch == 0)
      return D3DERR_INVALIDCALL;
    const uint64_t mirror_bytes = static_cast<uint64_t>(pitch) * ((Height + block_h - 1) / block_h);
    ownedBacking = wsi::aligned_malloc(mirror_bytes, DXMT_PAGE_SIZE);
    if (!ownedBacking)
      return D3DERR_OUTOFVIDEOMEMORY;
    std::memset(ownedBacking, 0, mirror_bytes);
    cpuPtr = ownedBacking;
    // buffer stays null → UnlockRect takes the mirror-upload path, not the
    // zero-copy buffer-backed path SYSTEMMEM/SCRATCH use.
  } else {
    // Lockable pools: back the texture with an MTLBuffer so LockRect
    // can hand out a CPU pointer without a getBytes copy. Pitch is
    // padded to the device's per-format alignment requirement.
    uint32_t bpp = D3DFormatBytesPerPixel(Format);
    if (bpp == 0)
      return D3DERR_INVALIDCALL;
    uint64_t alignment = m_metalDevice.minimumLinearTextureAlignmentForPixelFormat(pixelFormat);
    if (alignment == 0)
      alignment = 1;
    uint64_t row_bytes = static_cast<uint64_t>(Width) * bpp;
    pitch = static_cast<uint32_t>((row_bytes + alignment - 1) & ~(alignment - 1));
    // Apps that write rows at width*bpp regardless of the reported
    // pitch shear on the padded value (CreateTexture's zero-copy path
    // falls back to a tight mirror for the same reason); surface the
    // divergence until this path grows the same fallback.
    if (pitch != row_bytes)
      Logger::warn(str::format(
          "d3d9: CreateOffscreenPlainSurface: padded pitch ", pitch, " (tight ", (unsigned)row_bytes,
          ") exposed for format ", (unsigned)Format, " width ", Width
      ));
    const uint64_t backing_bytes = static_cast<uint64_t>(pitch) * Height;
    // 32-bit WoW64: pre-allocate backing in process address space.
    // LockRect pBits always 32-bit-addressable (WoW64 thunk space limit).
    ownedBacking = wsi::aligned_malloc(backing_bytes, DXMT_PAGE_SIZE);
    if (!ownedBacking)
      return D3DERR_OUTOFVIDEOMEMORY;
    // Pre-fault: see CreateVertexBuffer's matching comment for the
    // Rosetta x86_32 first-touch cliff rationale.
    std::memset(ownedBacking, 0, backing_bytes);
    WMTBufferInfo binfo{};
    binfo.length = backing_bytes;
    binfo.options = storage;
    binfo.memory.set(ownedBacking);
    buffer = m_metalDevice.newBuffer(binfo);
    if (buffer == nullptr) {
      wsi::aligned_free(ownedBacking);
      return D3DERR_OUTOFVIDEOMEMORY;
    }
    cpuPtr = ownedBacking;
    texture = buffer.newTexture(info, /*offset=*/0, /*bytes_per_row=*/pitch);
    if (texture == nullptr) {
      buffer = WMT::Reference<WMT::Buffer>{};
      wsi::aligned_free(ownedBacking);
      return D3DERR_OUTOFVIDEOMEMORY;
    }
  }

  D3DSURFACE_DESC desc{};
  desc.Format = Format;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = 0;
  desc.Pool = Pool;
  desc.MultiSampleType = D3DMULTISAMPLE_NONE;
  desc.MultiSampleQuality = 0;
  desc.Width = Width;
  desc.Height = Height;

  auto *surface = new MTLD3D9Surface(
      this, desc,
      /*container=*/static_cast<IDirect3DDevice9 *>(this), std::move(texture),
      /*mipLevel=*/0,
      /*selfPin=*/true,
      /*parentTextureType=*/WMTTextureType2D, std::move(buffer), cpuPtr, pitch,
      /*arraySlice=*/0, ownedBacking,
      /*dxmtTexture=*/std::move(dxmt_texture)
  );
  // CreateOffscreenPlainSurface in DEFAULT pool is losable; SYSTEMMEM /
  // SCRATCH copies live in CPU pools and never go through Reset's gate.
  if (Pool == D3DPOOL_DEFAULT)
    surface->markLosable();
  surface->AddRef();
  *ppSurface = surface;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) {
  if (RenderTargetIndex >= D3D_MAX_SIMULTANEOUS_RENDERTARGETS)
    return D3DERR_INVALIDCALL;
  // wined3d device.c: slot 0 cannot be unbound. Without a
  // primary RT the rest of the pipeline has nothing to write into,
  // so the runtime hard-rejects the case rather than letting a draw
  // produce no output.
  if (RenderTargetIndex == 0 && pRenderTarget == nullptr)
    return D3DERR_INVALIDCALL;

  auto *surface = static_cast<MTLD3D9Surface *>(pRenderTarget);
  // wined3d device.c: the surface must belong to *this* device.
  // Cross-device binding would break the Metal allocator that owns
  // the texture handle and is meaningless across separate D3D9
  // devices anyway. deviceRaw() avoids the AddRef/Release that the
  // public GetDevice path would require; this is a hot path.
  if (surface && surface->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  // DXVK SetRenderTargetInternal (d3d9_device.cpp) rejects a
  // surface that wasn't created with D3DUSAGE_RENDERTARGET. Apps and
  // tools that probe with an offscreen-plain / SYSTEMMEM surface
  // (anti-cheat fingerprinting, capability tests) expect INVALIDCALL,
  // not an opaque Metal encode-time validation error later. The
  // implicit-DS auto-target path passes through, and the swapchain
  // back-buffer surfaces are created with the usage bit set, so this
  // gate has no effect on dxmt's own creates.
  if (surface && !(surface->desc().Usage & D3DUSAGE_RENDERTARGET))
    return D3DERR_INVALIDCALL;
  // The usage bit alone is not sufficient: a texture created with
  // stray RENDERTARGET usage on a sampler-only format (CreateTexture
  // accepts those, matching wined3d) carries the bit but has no
  // attachable Metal format. Fail the bind here, not in the encoder.
  // NULL-FOURCC targets stay bindable; the render pass drops the slot.
  if (surface && !IsNullFormat(surface->desc().Format) &&
      D3DFormatToMetal(surface->desc().Format, D3D9FormatUsage::RenderTarget) == WMTPixelFormatInvalid)
    return D3DERR_INVALIDCALL;

  // No-op rebind on a non-zero slot. Slot 0 falls through because D3D9
  // spec resets viewport+scissor on every SetRenderTarget(0, ...) call,
  // even with the same surface (DXVK d3d9_device.cpp, wined3d
  // device.c). Slot >0 has no such semantic: pure refcount churn
  // if the surface didn't change.
  if (RenderTargetIndex != 0 && m_renderTargets[RenderTargetIndex].ptr() == surface)
    return D3D_OK;

  // If the bound surface for this slot is actually changing, drain any
  // pending clear onto the OLD RT before m_renderTargets mutates.
  // without it a Clear → SetRT (no draws between) would land the clear
  // on the new RT instead of the old, and the old RT would carry
  // forward stale content into subsequent frames. drainPendingClear
  // captures the current RT0/DS resources in its emitcc closure, so it
  // remains RT-correct even though we no longer FlushDrawBatch here.
  // Queued draws resolve before the SetRef op below is applied, so
  // they still observe the pre-Set binding.
  bool surface_changed = m_renderTargets[RenderTargetIndex].ptr() != surface;
  if (surface_changed) {
    drainPendingClear();
  }

  // Com<,false> assignment drops the previously-bound surface's priv
  // ref and AddRefPrivate's the new one. surface=nullptr is the
  // unbind path (idx>0).
  m_renderTargets[RenderTargetIndex] = surface;
  // Flag an AUTOGENMIPMAP render target for mip regen on bind, matching
  // DXVK (SetRenderTargetInternal: IsAutomaticMip -> SetNeedsMipGen).
  // dxmt already flags on Clear/StretchRect/ColorFill but not the
  // draw-fill path, so a target filled only by draws would never
  // regenerate; the mipsDirty sweep regenerates before the next sampler
  // bind. The usage pre-check keeps the QI off the common RT-bind path.
  if (surface && (surface->desc().Usage & D3DUSAGE_AUTOGENMIPMAP))
    surface->flagContainerAutoGenDirty();
  // D3D9 spec: a successful SetRenderTarget on slot 0 resets viewport
  // and scissor to cover the new RT (DXVK d3d9_device.cpp,
  // wined3d device.c). Apps that swap RTs without re-issuing
  // SetViewport rely on this.
  D3DVIEWPORT9 new_viewport;
  RECT new_scissor;
  bool need_viewport_op = false;
  bool need_scissor_op = false;
  if (RenderTargetIndex == 0 && surface) {
    const D3DSURFACE_DESC &d = surface->desc();
    m_viewport.X = 0;
    m_viewport.Y = 0;
    m_viewport.Width = d.Width;
    m_viewport.Height = d.Height;
    m_viewport.MinZ = 0.0f;
    m_viewport.MaxZ = 1.0f;
    m_scissorRect.left = 0;
    m_scissorRect.top = 0;
    m_scissorRect.right = static_cast<LONG>(d.Width);
    m_scissorRect.bottom = static_cast<LONG>(d.Height);
    new_viewport = m_viewport;
    new_scissor = m_scissorRect;
    need_viewport_op = true;
    need_scissor_op = true;
  }
  // The encode-thread walker applies the SetRef op below in arrival
  // order and bumps m_encodeSideRefsGen, so the next BatchedDraw picks
  // up the new RT slot while earlier queued draws resolve against the
  // pre-Set binding.
  // Op-stream mirror: push a SetRef only when the surface actually
  // changed. SetRenderTarget(0, same_surface) is a documented re-bind
  // that resets viewport/scissor (POD axes, handled separately below)
  // without touching the ref-counted slot; pushing an op there would
  // be a no-op AddRef/Release pair on the encode side.
  if (surface_changed) {
    if (surface)
      surface->AddRefPrivate();
    QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::RenderTarget0 + RenderTargetIndex), surface);
  }
  // viewport/scissor live in the per-draw pod_snapshot now; the
  // SetRenderTarget reset above already wrote to the calling-thread
  // shadows (m_viewport / m_scissorRect), so we just flag the axes
  // dirty so the next QueueBatchedDraw rebuilds them.
  if (need_viewport_op)
    m_encShadowDirty |= dxmt::D9ES_DIRTY_VIEWPORT;
  if (need_scissor_op)
    m_encShadowDirty |= dxmt::D9ES_DIRTY_SCISSOR_RECT;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) {
  // Unlike RT slot 0, depth-stencil is allowed to be NULL; depth-
  // disabled rendering is a valid pipeline configuration. wined3d
  // device.c d3d9_device_SetDepthStencilSurface accepts NULL.
  auto *surface = static_cast<MTLD3D9Surface *>(pNewZStencil);
  if (surface && surface->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  // Symmetric to SetRenderTarget's D3DUSAGE_RENDERTARGET gate: the
  // surface must carry D3DUSAGE_DEPTHSTENCIL or it has no business as
  // a depth attachment. wined3d / DXVK both validate this; Metal
  // would otherwise reject the texture at render-pass build time with
  // an opaque encode-time error. CreateDepthStencilSurface stamps
  // the flag; CreateTexture(D3DUSAGE_DEPTHSTENCIL) propagates it to
  // every level surface so shadow-map texture-as-DS still passes.
  if (surface && !(surface->desc().Usage & D3DUSAGE_DEPTHSTENCIL))
    return D3DERR_INVALIDCALL;
  if (m_depthStencilSurface.ptr() == surface)
    return D3D_OK;
  // DS surface is actually changing: drain any staged depth/stencil
  // clear onto the OLD DS before m_depthStencilSurface mutates, else
  // the clear leaks onto whatever DS the next draw binds.
  drainPendingClear();
  m_depthStencilSurface = surface;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (surface)
    surface->AddRefPrivate();
  QueueRefOp(PendingRefOp::DepthStencilSurface, surface);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) {
  if (!ppZStencilSurface)
    return D3DERR_INVALIDCALL;
  *ppZStencilSurface = nullptr;
  MTLD3D9Surface *bound = m_depthStencilSurface.ptr();
  if (!bound)
    return D3DERR_NOTFOUND;
  *ppZStencilSurface = ::dxmt::ref<IDirect3DSurface9>(bound);
  return D3D_OK;
}
// BeginScene / EndScene: pair-bracketed scene marker. DXVK
// (d3d9_device.cpp) and wined3d (device.c) both track an
// in_scene flag and reject misnested calls with INVALIDCALL. The
// bracket is also where DXVK fires an implicit-flush hint at EndScene;
// EndScene drains the batch below, matching that hint.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::BeginScene() {
  if (m_inScene)
    return D3DERR_INVALIDCALL;
  m_inScene = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EndScene() {
  if (!m_inScene)
    return D3DERR_INVALIDCALL;
  m_inScene = false;
  // Frame boundary. Drain queued batched draws onto a chunk first so
  // Present + downstream sync paths observe the frame's actual draws.
  // flushOpenWork() then catches any residual sync cmdbuf work: blits
  // queued post-FlushDrawBatch via the legacy path; so its commit
  // serialises against the chunk's commit through Metal queue ordering.
  auto pool = WMT::MakeAutoreleasePool();
  FlushDrawBatch();
  flushOpenWork();
  return D3D_OK;
}
// Clear: validation per DXVK d3d9_device.cpp and wined3d
// device.c. The execution model is *lazy*: the colour / depth /
// stencil values land in m_pendingClear and the next render pass
// opened by StartRenderPassForBatch_d9 (or drainPendingClear on the
// lone-Clear-then-Present path) folds them into its loadAction.
// D3D9 allows scissored Clear; until that lands the rect arguments
// widen to the whole attachment; acceptable for apps that always
// full-clear.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::Clear(DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) { return E_NOTIMPL; }

// Compaction matches DXVK d3d9_util.h. The D3DTRANSFORMSTATETYPE
// enum is sparse (VIEW=2, PROJECTION=3, TEXTURE0..7=16..23,
// WORLD=256, WORLDMATRIX(N)=256+N); the 266-entry table holds the
// dense indices: 0=VIEW, 1=PROJECTION, 2..9=TEXTURE0..7, 10..265=WORLD..
// WORLD(255).
static uint32_t
TransformIndex(D3DTRANSFORMSTATETYPE State) {
  if (State == D3DTS_VIEW)
    return 0;
  if (State == D3DTS_PROJECTION)
    return 1;
  if (State >= D3DTS_TEXTURE0 && State <= D3DTS_TEXTURE7)
    return 2 + (State - D3DTS_TEXTURE0);
  return 10 + (State - D3DTS_WORLD);
}

// Row-major 4x4 multiply, D3D9 convention (M = A * B means apply A
// first, then B). DXVK uses Matrix4 from its math header; we stay
// inline to keep the dependency surface small.
static D3DMATRIX
MatrixMultiply(const D3DMATRIX &a, const D3DMATRIX &b) {
  D3DMATRIX out{};
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      float s = 0.0f;
      for (int k = 0; k < 4; ++k)
        s += a.m[i][k] * b.m[k][j];
      out.m[i][j] = s;
    }
  }
  return out;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  // Validate before flipping the StateBlock-recording mask. wined3d
  // device.c sets the per-category dirty bit only after the underlying
  // wined3d_state_X call succeeds; recording a category whose Set
  // failed makes Apply restore stale snapshot values for a state the
  // app never touched.
  if (!pMatrix)
    return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= kMaxTransforms)
    return D3DERR_INVALIDCALL;
  // Recording arm: route the value into the block's snapshot and
  // leave live state untouched (wine update_state / DXVK m_recorder
  // shape; same for every recording arm below).
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapTransforms[idx] = *pMatrix;
    m_recordingBlock->m_changes.transforms = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit. D3DX-style engines re-set the same
  // world/view/projection matrix every pass; the memcmp here saves the
  // 64-byte memcpy on a hot setter that may fire per-draw.
  if (std::memcmp(&m_transforms[idx], pMatrix, sizeof(D3DMATRIX)) == 0)
    return D3D_OK;
  m_transforms[idx] = *pMatrix;
  // transforms aren't carried in pod_snapshot today (no Resolve / Emit
  // reader; FFP shader generator hasn't landed). When it lands, extend
  // QueueBatchedDraw's snapshot copy and OR a new D9ES_DIRTY_TRANSFORMS
  // bit into m_encShadowDirty here.
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) {
  if (!pMatrix)
    return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= kMaxTransforms)
    return D3DERR_INVALIDCALL;
  *pMatrix = m_transforms[idx];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix) {
  if (!pMatrix)
    return D3DERR_INVALIDCALL;
  uint32_t idx = TransformIndex(State);
  if (idx >= kMaxTransforms)
    return D3DERR_INVALIDCALL;
  m_transforms[idx] = MatrixMultiply(m_transforms[idx], *pMatrix);
  // Transforms aren't in pod_snapshot today (see SetTransform).
  // TODO when FFP lands: dirty FFVertexData (always) and FFVertexBlend
  // when idx is VIEW or in the WORLD range; DXVK.
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetViewport(const D3DVIEWPORT9 *pViewport) {
  if (!pViewport)
    return D3DERR_INVALIDCALL;
  D3DVIEWPORT9 vp = *pViewport;
  // DXVK normalises inverted Z (d3d9_device.cpp); Metal's
  // viewport rejects MaxZ <= MinZ at draw time.
  if (!(vp.MinZ < vp.MaxZ))
    vp.MaxZ = vp.MinZ + 0.001f;
  // Record the normalised viewport so Apply restores exactly what a
  // live SetViewport would have stored.
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapViewport = vp;
    m_recordingBlock->m_changes.viewport = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit: D3D9 effect frameworks re-set the
  // same viewport every pass.
  if (std::memcmp(&m_viewport, &vp, sizeof(D3DVIEWPORT9)) == 0)
    return D3D_OK;
  m_viewport = vp;
  // The "scissor disabled" branch in wmt_scissor_from_d3d9 returns
  // viewport bounds, so a viewport change implicitly invalidates the
  // applied scissor too.
  m_encShadowDirty |= dxmt::D9ES_DIRTY_VIEWPORT | dxmt::D9ES_DIRTY_SCISSOR_RECT;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetViewport(D3DVIEWPORT9 *pViewport) {
  if (!pViewport)
    return D3DERR_INVALIDCALL;
  *pViewport = m_viewport;
  return D3D_OK;
}
// FFP material / light bookkeeping. wined3d device.c
// d3d9_device_SetMaterial / SetLight / LightEnable. The FFP shader
// generator reads m_material / m_lights / m_lightEnables when it
// lands; until then these are bookkeeping calls; apps still issue
// them with a programmable PS bound, and a STUB_HR (E_NOTIMPL)
// trips apps that don't hr-check.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetMaterial(const D3DMATERIAL9 *pMaterial) {
  if (!pMaterial)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapMaterial = *pMaterial;
    m_recordingBlock->m_changes.material = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit. Same FFP-bookkeeping-only rationale
  // as SetTransform; no encShadowGen bump today (no Resolve reader),
  // but the memcpy of D3DMATERIAL9 (68 bytes) still costs on a setter
  // that hr-strict apps issue every frame even without an FFP draw.
  if (std::memcmp(&m_material, pMaterial, sizeof(D3DMATERIAL9)) == 0)
    return D3D_OK;
  m_material = *pMaterial;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetMaterial(D3DMATERIAL9 *pMaterial) {
  if (!pMaterial)
    return D3DERR_INVALIDCALL;
  *pMaterial = m_material;
  return D3D_OK;
}

// SetLight at index Idx. wined3d device.c d3d9_device_SetLight
// grows the underlying light array on demand; new slots default to
// disabled. Negative Type is INVALIDCALL.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetLight(DWORD Index, const D3DLIGHT9 *pLight) {
  if (!pLight)
    return D3DERR_INVALIDCALL;
  if (pLight->Type < D3DLIGHT_POINT || pLight->Type > D3DLIGHT_DIRECTIONAL)
    return D3DERR_INVALIDCALL;
  // Per wined3d stateblock.c, attenuation < 0 is INVALIDCALL
  // for POINT/SPOT (DIRECTIONAL ignores attenuation entirely). wined3d
  // notes that some titles set junk light data that confuses the GL
  // driver; on Metal the symptom would be NaN-poisoned FFP lighting once
  // the FFP generator lands. Cheap gate, prevents bad state from being
  // captured into StateBlocks.
  if (pLight->Type == D3DLIGHT_POINT || pLight->Type == D3DLIGHT_SPOT) {
    if (pLight->Attenuation0 < 0.0f || pLight->Attenuation1 < 0.0f || pLight->Attenuation2 < 0.0f)
      return D3DERR_INVALIDCALL;
  }
  // Recording: the seed-captured snapshot vectors get the same
  // grow-on-demand treatment the live vectors do below.
  std::vector<D3DLIGHT9> &lights = m_inStateBlockRecord ? m_recordingBlock->m_snapLights : m_lights;
  std::vector<BOOL> &enables = m_inStateBlockRecord ? m_recordingBlock->m_snapLightEnables : m_lightEnables;
  if (Index >= lights.size()) {
    lights.resize(Index + 1, D3DLIGHT9{});
    enables.resize(Index + 1, FALSE);
  }
  lights[Index] = *pLight;
  if (m_inStateBlockRecord)
    m_recordingBlock->m_changes.lights = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetLight(DWORD Index, D3DLIGHT9 *pLight) {
  if (!pLight)
    return D3DERR_INVALIDCALL;
  // wined3d returns INVALIDCALL when the index was never set
  // (stateblock.c, wined3d_stateblock_get_light path).
  // Sparse-grown vector slots default to
  // zero-init D3DLIGHT9 with Type=0, which sits below the valid
  // D3DLIGHT_POINT..DIRECTIONAL (1..3) range; Type==0 is the
  // "implicitly grown but never Set" sentinel.
  if (Index >= m_lights.size() || m_lights[Index].Type == 0)
    return D3DERR_INVALIDCALL;
  *pLight = m_lights[Index];
  return D3D_OK;
}

// LightEnable on an unset index implicitly creates a default
// directional light there; wined3d device.c mirrors this so
// apps can LightEnable(0, TRUE) without first SetLight'ing.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::LightEnable(DWORD Index, BOOL Enable) {
  // Recording targets the block's seed-captured vectors instead of
  // live state; the implicit default-light creation applies the same
  // way on either side.
  std::vector<D3DLIGHT9> &lights = m_inStateBlockRecord ? m_recordingBlock->m_snapLights : m_lights;
  std::vector<BOOL> &enables = m_inStateBlockRecord ? m_recordingBlock->m_snapLightEnables : m_lightEnables;
  if (Index >= lights.size()) {
    D3DLIGHT9 def{};
    def.Type = D3DLIGHT_DIRECTIONAL;
    def.Diffuse = {1.0f, 1.0f, 1.0f, 0.0f};
    def.Direction = {0.0f, 0.0f, 1.0f};
    lights.resize(Index + 1, D3DLIGHT9{});
    enables.resize(Index + 1, FALSE);
    lights[Index] = def;
  }
  enables[Index] = Enable ? TRUE : FALSE;
  if (m_inStateBlockRecord)
    m_recordingBlock->m_changes.lights = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetLightEnable(DWORD Index, BOOL *pEnable) {
  if (!pEnable)
    return D3DERR_INVALIDCALL;
  // Same sparse-grown sentinel as GetLight; Type==0 means the slot
  // exists in the underlying vector only because a higher-index Set
  // resized it, not because the app ever touched this index.
  if (Index >= m_lights.size() || m_lights[Index].Type == 0)
    return D3DERR_INVALIDCALL;
  *pEnable = m_lightEnables[Index];
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetClipPlane(DWORD Index, const float *pPlane) {
  if (!pPlane)
    return D3DERR_INVALIDCALL;
  // D3D9 caps higher indices to the last valid slot rather than
  // erroring. cf. DXVK d3d9_device.cpp.
  if (Index >= 8)
    Index = 7;
  if (m_inStateBlockRecord) {
    for (uint32_t i = 0; i < 4; ++i)
      m_recordingBlock->m_snapClipPlanes[Index][i] = pPlane[i];
    m_recordingBlock->m_changes.clip_planes = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit. The clip-plane array is in
  // pod_snapshot so a no-op rewrite would otherwise force a fresh
  // D9EncodingState COW on the next QueueBatchedDraw.
  if (std::memcmp(&m_clipPlanes[Index][0], pPlane, sizeof(float) * 4) == 0)
    return D3D_OK;
  for (uint32_t i = 0; i < 4; ++i)
    m_clipPlanes[Index][i] = pPlane[i];
  m_encShadowDirty |= dxmt::D9ES_DIRTY_CLIP_PLANES;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetClipPlane(DWORD Index, float *pPlane) {
  if (!pPlane)
    return D3DERR_INVALIDCALL;
  if (Index >= 8)
    Index = 7;
  for (uint32_t i = 0; i < 4; ++i)
    pPlane[i] = m_clipPlanes[Index][i];
  return D3D_OK;
}
// Hot path: pure DWORD store/load (no Value validation; rasterizer clamps).
// State range: 0,7..255 valid; 1..6 D3D8-era no-ops; 256+ out-of-enum.
// D3D9_RESZ_CODE depth-resolve skipped (NVIDIA-specific MSAA trigger, not on Apple Silicon).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) {
  if (!pValue)
    return D3DERR_INVALIDCALL;
  // DXVK d3d9_device.cpp: out of the live-storage range is
  // INVALIDCALL on Get (asymmetric with Set, which silently no-ops).
  if (State > 255 || (State < D3DRS_ZENABLE && State != 0))
    return D3DERR_INVALIDCALL;
  // DXVK d3d9_device.cpp: slots inside the live-storage range
  // but outside the D3DRS_ZENABLE..D3DRS_BLENDOPALPHA enum (state 0,
  // 210..255 reserved) always read back as 0,
  // regardless of whether Set wrote to them. The asymmetry is part
  // of D3D9's contract; apps observe it.
  if (State < D3DRS_ZENABLE || State > D3DRS_BLENDOPALPHA)
    *pValue = 0;
  else
    *pValue = m_renderStates[State];
  return D3D_OK;
}
// State-block creation. wined3d device.c d3d9_device_CreateStateBlock
// gates Type to D3DSBT_ALL / VERTEXSTATE / PIXELSTATE; anything else
// is INVALIDCALL. The block round-trips every D3D9 state-block
// category on Apply.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) {
  if (!ppSB)
    return D3DERR_INVALIDCALL;
  *ppSB = nullptr;
  if (Type != D3DSBT_ALL && Type != D3DSBT_VERTEXSTATE && Type != D3DSBT_PIXELSTATE)
    return D3DERR_INVALIDCALL;
  // Issuing CreateStateBlock between Begin/EndStateBlock is an error;
  // the runtime is mid-recording and conflating the two would
  // corrupt the recorded mask. wined3d returns INVALIDCALL.
  if (m_inStateBlockRecord)
    return D3DERR_INVALIDCALL;
  auto *sb = new MTLD3D9StateBlock(this, Type);
  // D3D9: CreateStateBlock captures state immediately (wined3d pattern).
  // Mask drives which categories Apply restores (D3DSBT_ALL/PIXELSTATE/VERTEXSTATE).
  D3D9StateBlockChanges changes;
  switch (Type) {
  case D3DSBT_ALL:
    changes.markAll();
    break;
  case D3DSBT_PIXELSTATE:
    changes.markPixelStateSubset();
    break;
  case D3DSBT_VERTEXSTATE:
    changes.markVertexStateSubset();
    break;
  default:
    break; // Unreachable; Type was already validated above.
  }
  sb->setChanges(changes);
  sb->Capture();
  sb->AddRef();
  *ppSB = sb;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::BeginStateBlock() {
  if (m_inStateBlockRecord)
    return D3DERR_INVALIDCALL;
  // Recording redirects every Set* into this block's snapshot storage
  // so live device state stays untouched until the app Apply()s the
  // returned block (wine d3d9 device.c repoints device->update_state;
  // DXVK allocates m_recorder the same way at BeginStateBlock).
  //
  // Seed-capture the coarse-masked categories from live state. KNOWN
  // DIVERGENCE from the per-element tracking wined3d / DXVK do: where
  // the changed mask is one bit per category (sampler states, texture
  // stage states, transforms, clip planes, lights, VS/PS I+B constant
  // files, gaps inside the recorded F-constant range), recording ONE
  // element marks the whole category and Apply restores the
  // un-recorded siblings to these Begin-time values, not the live
  // values at Apply time. Render states are per-state exact; textures
  // and streams are per-slot; F constants are range-tracked. The
  // ref-pinned single-slot categories (textures, streams, index
  // buffer, decl, shaders) are NOT seeded: a recorded Set wholly
  // overwrites those snapshot slots, and skipping the seed keeps the
  // recording block from pinning every object bound at Begin time.
  auto *sb = new MTLD3D9StateBlock(this, D3DSBT_ALL);
  D3D9StateBlockChanges seed;
  seed.markAll();
  seed.textures = 0;
  seed.streams = 0;
  seed.index_buffer = false;
  seed.vertex_declaration = false;
  seed.vertex_shader = false;
  seed.pixel_shader = false;
  sb->setChanges(seed);
  sb->Capture();
  sb->setChanges(D3D9StateBlockChanges{});
  m_recordingBlock = sb;
  m_inStateBlockRecord = true;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) {
  // End-without-Begin must leave the out-pointer untouched (wine
  // dlls/d3d9/tests/device.c test_begin_end_state_block asserts the
  // caller's sentinel survives), so the recording gate runs before any
  // write through ppSB.
  if (!ppSB || !m_inStateBlockRecord)
    return D3DERR_INVALIDCALL;
  m_inStateBlockRecord = false;
  // Hand the recording block out as-is: it already carries the
  // recorded values and the touched-state mask, and a capture-from-
  // live here would overwrite the recorded fields with live state the
  // recording deliberately never modified.
  auto *sb = m_recordingBlock;
  m_recordingBlock = nullptr;
  sb->AddRef();
  *ppSB = sb;
  return D3D_OK;
}
// SetClipStatus/GetClipStatus: vestigial FFP-era occlusion bookkeeping.
// wined3d device.c routes to wined3d_device_set_clip_status /
// get_clip_status which both succeed; the wined3d layer just stores the
// struct. Apps still call these and don't always check the hr; E_NOTIMPL
// trips them. Spec-correct shape: round-trip the struct, return D3D_OK.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetClipStatus(const D3DCLIPSTATUS9 *pClipStatus) {
  if (!pClipStatus)
    return D3DERR_INVALIDCALL;
  m_clipStatus = *pClipStatus;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) {
  if (!pClipStatus)
    return D3DERR_INVALIDCALL;
  *pClipStatus = m_clipStatus;
  return D3D_OK;
}
// Stage layout (wined3d pattern): PS 0..15 (slots 0..15), VS samplers 257..260→16..19.
// Out-of-range: GetTexture→NULL/D3D_OK; SetTexture→silent no-op (DMAP without cap check).
// GPU-side mapping at draw time (sampler slot N → m_textures[N]).
namespace {
// Returns 0..19 for a valid stage, or UINT32_MAX for stages that the
// runtime ignores (D3DDMAPSAMPLER and out-of-range values).
inline uint32_t
texture_stage_to_slot(DWORD stage) {
  if (stage < 16)
    return stage;
  if (stage >= D3DVERTEXTEXTURESAMPLER0 && stage <= D3DVERTEXTEXTURESAMPLER3)
    return 16 + (stage - D3DVERTEXTEXTURESAMPLER0);
  return UINT32_MAX;
}
} // namespace

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) {
  if (!ppTexture)
    return D3DERR_INVALIDCALL;
  *ppTexture = nullptr;
  uint32_t slot = texture_stage_to_slot(Stage);
  if (slot == UINT32_MAX)
    return D3D_OK;
  MTLD3D9CommonTexture *bound = m_textures[slot].ptr();
  if (!bound)
    return D3D_OK;
  // Hand back the IDirect3DBaseTexture9 view of the leaf. The leaf
  // type tag picks which IDirect3D*Texture9 sub-interface the bound
  // pointer is castable to; static_cast to that, then to the base.
  IDirect3DBaseTexture9 *iface = nullptr;
  switch (bound->commonTextureType()) {
  case D3DRTYPE_TEXTURE:
    iface = static_cast<IDirect3DTexture9 *>(static_cast<MTLD3D9Texture *>(bound));
    break;
  case D3DRTYPE_CUBETEXTURE:
    iface = static_cast<IDirect3DCubeTexture9 *>(static_cast<MTLD3D9CubeTexture *>(bound));
    break;
  case D3DRTYPE_VOLUMETEXTURE:
    iface = static_cast<IDirect3DVolumeTexture9 *>(static_cast<MTLD3D9VolumeTexture *>(bound));
    break;
  default:
    // Defensive branch; every concrete commonTextureType() returns
    // one of the three D3DRTYPE_* values, so this is dead code in
    // practice. Match wined3d's looser shape (device.c
    // unconditionally hands back the parent regardless of type tag);
    // silent OK + null output keeps the contract uniform with the
    // unbound-slot branch above.
    return D3D_OK;
  }
  *ppTexture = ::dxmt::ref(iface);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) {
  uint32_t slot = texture_stage_to_slot(Stage);
  if (slot == UINT32_MAX)
    return D3D_OK;

  MTLD3D9CommonTexture *common = nullptr;
  if (pTexture) {
    // Dispatch to the leaf based on the D3D9 type tag, then cross-cast
    // to MTLD3D9CommonTexture. Two static_casts because the leaf is
    // multi-inherited (ComObject<IDirect3D*Texture9> + MTLD3D9CommonTexture);
    // going via the leaf is the only way the C++ object model can
    // resolve which CommonTexture sub-object to land on.
    switch (pTexture->GetType()) {
    case D3DRTYPE_TEXTURE:
      common = static_cast<MTLD3D9Texture *>(static_cast<IDirect3DTexture9 *>(pTexture));
      break;
    case D3DRTYPE_CUBETEXTURE:
      common = static_cast<MTLD3D9CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(pTexture));
      break;
    case D3DRTYPE_VOLUMETEXTURE:
      common = static_cast<MTLD3D9VolumeTexture *>(static_cast<IDirect3DVolumeTexture9 *>(pTexture));
      break;
    default:
      return D3DERR_INVALIDCALL;
    }
    // Cross-device check matches Set(RT|DepthStencilSurface). Same
    // reasoning: deviceRaw() avoids an AddRef/Release cycle that
    // GetDevice would force on a hot path.
    if (common->deviceRaw() != this)
      return D3DERR_INVALIDCALL;
    // D3DPOOL_SCRATCH is rejected per MSDN SetTexture Remarks ("not
    // allowed if the texture is created with a pool type of
    // D3DPOOL_SCRATCH"). The texture's backing isn't bindable to a
    // sampler; silently accepting would let the GPU read garbage on
    // the next draw. Neither wined3d nor DXVK gates this at the d3d9
    // layer (both rely on deeper failures); strict-spec here surfaces
    // app bugs cleanly.
    if (common->commonTexturePool() == D3DPOOL_SCRATCH)
      return D3DERR_INVALIDCALL;
  }
  // Recording arm AFTER the validation gates above (DXVK validates
  // then records). The Com<,false> assignment pins the target exactly
  // like Capture's snapshot does; the per-slot mask bit keeps Apply
  // away from slots the recording never touched.
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapTextures[slot] = common;
    m_recordingBlock->m_changes.textures |= 1u << slot;
    return D3D_OK;
  }
  // Defensive same-slot rebind; common in D3D9 engines that re-issue
  // every per-draw state-set unconditionally; would otherwise force a
  // fresh D9EncodingRefs COW snapshot at the next QueueBatchedDraw
  // (~50 AddRefPrivate ops walking every bound slot).
  if (m_textures[slot].ptr() == common)
    return D3D_OK;
  m_textures[slot] = common;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (common)
    common->AddRefPrivate();
  QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::Texture0 + slot), common);
  return D3D_OK;
}

// FFP texture-blend: stage 0..7, type D3DTSS_COLOROP..CONSTANT (1..32).
// Out-of-range: INVALIDCALL (strict gate, no DMAP-style ignore).
// Programmable-PS apps call even with active shaders; return OK (not E_NOTIMPL) matching DXVK.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  // wined3d d3d9/device.c returns D3D_OK silently for out-of-range
  // Type and does NOT bound Stage at all; DXVK d3d9_device.cpp
  // clamps and also returns D3D_OK. Silently ignore OOR: hr-strict
  // app init paths fail on an INVALIDCALL here.
  if (Stage >= 8 || Type == 0 || Type > D3DTSS_CONSTANT)
    return D3D_OK;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapTextureStageStates[Stage][Type] = Value;
    m_recordingBlock->m_changes.texture_stage_states = true;
    return D3D_OK;
  }
  if (m_textureStageStates[Stage][Type] == Value)
    return D3D_OK;
  m_textureStageStates[Stage][Type] = Value;
  // texture_stage_states isn't read by Resolve/Emit today (FFP shader
  // generator hasn't landed) so we don't dirty m_encShadowDirty; the
  // POD snapshot doesn't carry these. Re-enable the dirty flag when
  // the FFP generator starts consuming them.
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) {
  if (!pValue)
    return D3DERR_INVALIDCALL;
  *pValue = 0;
  // Match the loose Set shape; wined3d d3d9/device.c returns
  // D3D_OK with *pValue=0 for OOR Type and doesn't bound Stage.
  if (Stage >= 8 || Type == 0 || Type > D3DTSS_CONSTANT)
    return D3D_OK;
  *pValue = m_textureStageStates[Stage][Type];
  return D3D_OK;
}
// One of hottest entry points: pure DWORD store/load.
// Stage layout matches SetTexture: PS 0..15, VS 257..260→16..19, out-of-range→no-op.
// D3DSAMP_INVALID slot (index 0) accepted; indices >14 out-of-enum rejected.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) {
  if (!pValue)
    return D3DERR_INVALIDCALL;
  *pValue = 0;
  if (Type > D3DSAMP_DMAPOFFSET)
    return D3DERR_INVALIDCALL;
  uint32_t slot = texture_stage_to_slot(Sampler);
  if (slot == UINT32_MAX)
    return D3D_OK;
  *pValue = m_samplerStates[slot][Type];
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
  // wined3d (device.c) and DXVK (d3d9_device.cpp) silently
  // accept Type > D3DSAMP_DMAPOFFSET; they store it in a wider array.
  // dxmt rejects with INVALIDCALL to keep m_samplerStates bounded
  // (each slot is per-stage POD; widening costs RAM and doesn't help
  // any caller; no shader path consumes Type > 13). hr-strict apps
  // that check for D3D_OK on bogus Type bits land here; if a real app
  // requires the pass-through shape, widen the array and remove this
  // gate (no code-shape consequence on encode-time consumption).
  if (Type > D3DSAMP_DMAPOFFSET)
    return D3DERR_INVALIDCALL;
  uint32_t slot = texture_stage_to_slot(Sampler);
  if (slot == UINT32_MAX)
    return D3D_OK;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapSamplerStates[slot][Type] = Value;
    m_recordingBlock->m_changes.sampler_states = true;
    return D3D_OK;
  }
  if (m_samplerStates[slot][Type] == Value)
    return D3D_OK;
  m_samplerStates[slot][Type] = Value;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_SAMPLER_STATES;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ValidateDevice(DWORD *pNumPasses) {
  // Apps query: can current state render in single pass? Metal validates at PSO build.
  // Always claim single-pass. Accept NULL out-pointer (DXVK/wined3d pattern).
  if (pNumPasses)
    *pNumPasses = 1;
  return D3D_OK;
}
// Texture-palette state: storage-only port of DXVK
// D3D9DeviceEx::Set/GetPaletteEntries / Set/GetCurrentTexturePalette
// (d3d9_device.cpp). dxmt has no FFP P8 sampler yet, so
// SetCurrentTexturePalette doesn't translate paletted reads; the
// palette state still needs a faithful round-trip per spec, since
// apps' init paths hr-check these.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY *pEntries) {
  if (pEntries == nullptr)
    return D3DERR_INVALIDCALL;
  // 256 entries per D3D9 spec; emplace-or-overwrite the map slot.
  auto it = m_texturePalettes.find(PaletteNumber);
  if (it == m_texturePalettes.end()) {
    std::array<PALETTEENTRY, 256> palette;
    std::memcpy(palette.data(), pEntries, sizeof(PALETTEENTRY) * 256);
    m_texturePalettes.emplace(PaletteNumber, palette);
  } else {
    std::memcpy(it->second.data(), pEntries, sizeof(PALETTEENTRY) * 256);
  }
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) {
  if (pEntries == nullptr)
    return D3DERR_INVALIDCALL;
  auto it = m_texturePalettes.find(PaletteNumber);
  if (it == m_texturePalettes.end())
    return D3DERR_INVALIDCALL;
  std::memcpy(pEntries, it->second.data(), sizeof(PALETTEENTRY) * 256);
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetCurrentTexturePalette(UINT PaletteNumber) {
  // DXVK note: when FFP P8 sampler lands, this should kick a texture
  // re-translate pass for all active paletted stages. Storage-only
  // for now matches DXVK's TODO at d3d9_device.cpp.
  m_currentTexturePalette = PaletteNumber;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetCurrentTexturePalette(UINT *PaletteNumber) {
  if (PaletteNumber == nullptr)
    return D3DERR_INVALIDCALL;
  *PaletteNumber = m_currentTexturePalette;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetScissorRect(const RECT *pRect) {
  if (!pRect)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapScissorRect = *pRect;
    m_recordingBlock->m_changes.scissor = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit (DXVK d3d9_device.cpp).
  if (std::memcmp(&m_scissorRect, pRect, sizeof(RECT)) == 0)
    return D3D_OK;
  m_scissorRect = *pRect;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_SCISSOR_RECT;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetScissorRect(RECT *pRect) {
  if (!pRect)
    return D3DERR_INVALIDCALL;
  *pRect = m_scissorRect;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetSoftwareVertexProcessing(BOOL) {
  // wined3d's d3d9 layer accepts unconditionally; DXVK rejects only
  // its SWVP-capability mismatches. On a pure-HWVP device the value
  // is stored and has no effect. Metal is always hardware-VP; the bool has no effect either
  // way. Apps issue SetSoftwareVertexProcessing(FALSE) as a defensive
  // "stay in HW" call and hr-check it; wined3d and DXVK report
  // success.
  return D3D_OK;
}
BOOL STDMETHODCALLTYPE
MTLD3D9Device::GetSoftwareVertexProcessing() {
  return FALSE;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetNPatchMode(float) {
  // DXVK D3D9DeviceEx::SetNPatchMode silently accepts. N-patches were
  // removed in D3D10; apps that issue SetNPatchMode(0.0f); disabling
  // N-patches; expect D3D_OK from any modern runtime. Same E_NOTIMPL
  // ⇒ silent-OK rationale as SetSoftwareVertexProcessing above.
  return D3D_OK;
}
float STDMETHODCALLTYPE
MTLD3D9Device::GetNPatchMode() {
  return 0.0f;
}
// All entry points (DP/DIP/DPUP/DIPUP) queue: BuildDrawCapture→QueueBatchedDraw.
// Encode-side: ResolveBatchedDrawForChunk + EmitCommonRenderSetup_d9 + EmitDrawCommand_d9.
// Per-(RT,DS) encoder batching avoids tile-store/load; BatchedDraw POD-COW is DXVK m_dirty analogue.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) { return E_NOTIMPL; }


// DrawIndexedPrimitive: same as DrawPrimitive + bound IB + BaseVertexIndex.
// Metal resolves indices + adds baseVertex; manual-fetch lowering consumes value as-is.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex,
    UINT PrimitiveCount
) {
  (void)MinVertexIndex;
  // wined3d d3d9_device_DrawIndexedPrimitive (device.c) gates on
  // vertex_declaration AND index_buffer; no BeginScene gate, no
  // stream-0 gate (see DrawPrimitive for the multi-stream rationale).
  if (!m_vertexDeclaration.ptr())
    return D3DERR_INVALIDCALL;
  if (!m_indexBuffer.ptr())
    return D3DERR_INVALIDCALL;
  // DXVK D3D9DeviceEx::DrawIndexedPrimitive early-outs D3D_OK on
  // (!PrimitiveCount || !NumVertices); a zero-vertex range is a
  // degenerate no-op, matching the DrawIndexedPrimitiveUP sibling below.
  if (PrimitiveCount == 0 || NumVertices == 0)
    return D3D_OK;
  // No autorelease pool; see DrawPrimitive for the rationale.
  // Fan emulation against a bound IB; read the source indices through
  // the host pointer at (currentOffset() + StartIndex * indexSize) and
  // remap into a fresh u32 list. m_hostPtr is null only for pool
  // combinations that have no sysmem mirror (a future DEFAULT-static
  // path); we reject those rather than silently mis-rendering. The
  // resulting IB rides m_constRing; pinned to m_completionEvent via
  // the chunk lambda's signal_seq tail.
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto *ib_obj = m_indexBuffer.ptr();
    const void *src_base = ib_obj->hostPointer();
    if (!src_base)
      return D3DERR_INVALIDCALL;
    uint32_t src_idx_size = (ib_obj->indexFormat() == D3DFMT_INDEX32) ? 4u : 2u;
    const void *src = static_cast<const char *>(src_base) + static_cast<size_t>(StartIndex) * src_idx_size;
    auto [ib_handle, ib_offset_u32] = BuildFanIndexBuffer(PrimitiveCount, src, src_idx_size);
    BatchedDraw draw{};
    draw.cap = BuildDrawCapture();
    draw.type = BatchedDraw::kIndexed;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.vertex_or_index_count = PrimitiveCount * 3;
    draw.base_vertex = BaseVertexIndex;
    draw.override_ib_buffer = ib_handle;
    draw.override_ib_offset = ib_offset_u32;
    draw.override_ib_format = D3DFMT_INDEX32;
    QueueBatchedDraw(std::move(draw));
    // Accumulate. State-change handlers + Present drain m_pendingDraws.
    return D3D_OK;
  }
  UINT index_count = prim_to_vertex_count(PrimitiveType, PrimitiveCount);
  BatchedDraw draw{};
  draw.cap = BuildDrawCapture();
  draw.type = BatchedDraw::kIndexed;
  draw.primitive_type = PrimitiveType;
  draw.vertex_or_index_count = index_count;
  draw.start_vertex_or_index = StartIndex;
  draw.base_vertex = BaseVertexIndex;
  QueueBatchedDraw(std::move(draw));
  // Accumulate. State-change handlers + Present drain m_pendingDraws.
  return D3D_OK;
}

// Shared body: bound-stream differ in IB/count/BaseVertexIndex; UP inject transient slot-0.
// Validation gate reads from capture (caller-provided or UP-built at queue time).
// UP build at queue time ensures validation sees shader/RT bindings from draw thread, not FlushDrawBatch thread.
MTLD3D9Device::D3D9DrawCapture
MTLD3D9Device::BuildDrawCapture() {
  // POD state read by Resolve from D9EncodingState; setter-flush invariant ensures batch shares one snapshot.
  // Ref-counted state via setter ops into m_encodeSideRefs.
  // BuildDrawCapture must freeze per-draw rename cursors (gpu_address/currentOffset advance on Lock(DISCARD)).
  D3D9DrawCapture cap;
  // vb_slots is value-initialized (zero-filled) by the struct default
  // ctor (= {}), so unbound slots already report buffer=0,gpu_address=0.
  // The bound buffer pointer is the single source of truth for stream
  // liveness; wined3d (context.c wined3d_stream_info_from_declaration)
  // and DXVK both derive it per draw rather than trusting a cached
  // mask.
  for (uint32_t s = 0; s < D3D9_MAX_VERTEX_STREAMS; ++s) {
    auto *vb = m_vertexBuffers[s].ptr();
    if (!vb)
      continue;
    cap.vb_slots[s].offset = m_streamOffsets[s];
    cap.vb_slots[s].stride = m_streamStrides[s];
    cap.vb_slots[s].buffer = vb->metalBuffer().handle;
    cap.vb_slots[s].gpu_address = vb->gpuAddress();
    // Stamp the open chunk's seq: the buffer's last GPU read lands in
    // a chunk <= this one, so Lock's plain-map sync can gate on it.
    vb->markPendingGpuUse(m_currentCmdSeq);
  }
  if (m_indexBuffer.ptr() != nullptr) {
    cap.ib_buffer = m_indexBuffer->metalBuffer().handle;
    cap.ib_offset = m_indexBuffer->currentOffset();
    cap.ib_format = m_indexBuffer->indexFormat();
    m_indexBuffer->markPendingGpuUse(m_currentCmdSeq);
  } else {
    cap.ib_buffer = 0;
    cap.ib_offset = 0;
    cap.ib_format = D3DFMT_UNKNOWN;
  }
  return cap;
}

void
MTLD3D9Device::QueueBatchedDraw(BatchedDraw &&draw) {
  // Freeze POD state: m_encShadowDirty==0 means all draws share one shared_ptr.
  // Non-zero: copy-construct fresh snapshot, overwrite ONLY dirty axes (~13KB vs ~20KB rebuild).
  // Resolve reads draw.pod_snapshot, letting setters skip FlushDrawBatch (each frozen independently).
  if (m_encShadowDirty != 0) {
    auto snap = m_encShadowLastSnap ? std::make_shared<dxmt::D9EncodingState>(*m_encShadowLastSnap)
                                    : std::make_shared<dxmt::D9EncodingState>();
    const uint32_t dirty = m_encShadowDirty;
    if (dirty & dxmt::D9ES_DIRTY_RENDER_STATES)
      std::memcpy(snap->render_states, m_renderStates, sizeof(snap->render_states));
    if (dirty & dxmt::D9ES_DIRTY_SAMPLER_STATES)
      std::memcpy(snap->sampler_states, m_samplerStates, sizeof(snap->sampler_states));
    if (dirty & dxmt::D9ES_DIRTY_CLIP_PLANES)
      std::memcpy(snap->clip_planes, m_clipPlanes, sizeof(snap->clip_planes));
    if (dirty & dxmt::D9ES_DIRTY_STREAM_FREQ)
      std::memcpy(snap->stream_freq, m_streamFreq, sizeof(snap->stream_freq));
    if (dirty & dxmt::D9ES_DIRTY_VS_CONST_F)
      std::memcpy(snap->vs_const_F, m_vsConstantsF, sizeof(snap->vs_const_F));
    if (dirty & dxmt::D9ES_DIRTY_VS_CONST_I)
      std::memcpy(snap->vs_const_I, m_vsConstantsI, sizeof(snap->vs_const_I));
    if (dirty & dxmt::D9ES_DIRTY_VS_CONST_B)
      std::memcpy(snap->vs_const_B, m_vsConstantsB, sizeof(snap->vs_const_B));
    if (dirty & dxmt::D9ES_DIRTY_PS_CONST_F)
      std::memcpy(snap->ps_const_F, m_psConstantsF, sizeof(snap->ps_const_F));
    if (dirty & dxmt::D9ES_DIRTY_PS_CONST_I)
      std::memcpy(snap->ps_const_I, m_psConstantsI, sizeof(snap->ps_const_I));
    if (dirty & dxmt::D9ES_DIRTY_PS_CONST_B)
      std::memcpy(snap->ps_const_B, m_psConstantsB, sizeof(snap->ps_const_B));
    if (dirty & dxmt::D9ES_DIRTY_VS_CONST_F_MAX)
      snap->vs_const_f_max = m_vsConstFMax;
    if (dirty & dxmt::D9ES_DIRTY_PS_CONST_F_MAX)
      snap->ps_const_f_max = m_psConstFMax;
    if (dirty & dxmt::D9ES_DIRTY_VIEWPORT)
      snap->viewport = m_viewport;
    if (dirty & dxmt::D9ES_DIRTY_SCISSOR_RECT)
      snap->scissor_rect = m_scissorRect;
    m_encShadowLastSnap = std::move(snap);
    m_encShadowDirty = 0;
  }
  draw.pod_snapshot = m_encShadowLastSnap;

  // No per-draw ref snapshot; ref-counted state lives on the
  // device-side m_encodeSideRefs mirror that the chunk walker mutates
  // as it processes SetRef ops in arrival order. Resolve reads from
  // there directly. The migration deletes the 40-Com<>-slot AddRef
  // pair the COW model paid per cluster boundary (~4400 ref setters/frame
  // in a heavy draw scene, clustered such that effectively every draw
  // rebuilt the snapshot under COW). wined3d CS shape.

  // Push to the arrival-order op stream FIRST so the index field
  // points at the slot we're about to occupy in m_pendingDraws.
  m_pendingOps.push_back({PendingOpRef::Draw, static_cast<uint32_t>(m_pendingDraws.size())});
  m_pendingDraws.push_back(std::move(draw));
  // Per-frame draw rate. All four Draw* entry points funnel through
  // here (DrawPrimitive / DrawIndexedPrimitive and the UP siblings),
  // so a single bump here covers the full draw stream.
}

void
MTLD3D9Device::QueueBlitOp(PendingBlitOp &&op) {
  // Same arrival-order discipline as QueueBatchedDraw; record the
  // ref before pushing the payload so the index field stays consistent.
  // Blits ride the same chunk lambda as draws; arrival-order across
  // kinds matters for sequencing a blit's GPU writes against the
  // draws that read its destination.
  m_pendingOps.push_back({PendingOpRef::Blit, static_cast<uint32_t>(m_pendingBlits.size())});
  m_pendingBlits.push_back(std::move(op));
}

void
MTLD3D9Device::QueueRefOp(PendingRefOp::Slot slot, void *new_com) {
  // Same arrival-order discipline as the Draw / Blit queues. The caller
  // (the ref-state setter) AddRefPrivate'd new_com exactly once before
  // calling; that single ref is the lifetime guarantee until the chunk
  // walker installs it into m_encodeSideRefs via ApplyRefOp_d9.
  m_pendingOps.push_back({PendingOpRef::SetRef, static_cast<uint32_t>(m_pendingRefOps.size())});
  m_pendingRefOps.push_back({slot, new_com});
}

// Encode-thread walker hook: install one SetRef op into m_encodeSideRefs.
// The op carries one outstanding AddRefPrivate (or nullptr); the static_cast
// + Com<,false>::operator=(T*) path would AddRef again, so we manage the
// install manually: take_old via prvRef() pattern (release the prior slot)
// then poke the raw pointer into the slot's Com<,false>. The
// implementation lives in d3d9_device.cpp (here) rather than as a free
// helper because it needs the slot enum + every D9 resource type
// definition in scope, all of which are already known to this TU.
void
MTLD3D9Device::ApplyRefOp_d9(const PendingRefOp &op) {
  // Helper: install a raw pointer (with one outstanding AddRefPrivate)
  // into a Com<,false> slot. Releases the prior slot value's private
  // ref, takes the new ref by raw assignment (no further AddRef; the
  // setter's AddRef is the lifetime). nullptr is a valid unbind.
  auto install = [](auto &slot_ref, void *new_com) {
    using ComT = std::remove_reference_t<decltype(slot_ref)>;
    using T = typename std::remove_pointer<decltype(slot_ref.ptr())>::type;
    auto *prev = slot_ref.ptr();
    // Reset slot to null while releasing the prior ref. Com<,false>::
    // operator=(nullptr) does decRef() on m_ptr.
    slot_ref = nullptr;
    // Move the new pointer in WITHOUT re-AddRef. Move-assign from a
    // Com<,false> built via takeOwnership idiom: construct a temporary
    // Com<,false> that holds the pointer with zero outstanding refs,
    // then move-assign; move-assign skips both decRef-and-incRef.
    if (new_com) {
      ComT tmp;
      // Bypass public ctor via move-assign of temporary built with
      // takeOwnership idiom: move-assign skips incRef on the source.
      *(&tmp) = static_cast<T *>(new_com);
      slot_ref = std::move(tmp);
    }
    (void)prev; // prev was already released by the `= nullptr` above
  };

  if (op.slot >= PendingRefOp::Texture0 && op.slot <= PendingRefOp::Texture19) {
    unsigned i = op.slot - PendingRefOp::Texture0;
    install(m_encodeSideRefs.textures[i], op.com_ptr);
    return;
  }
  if (op.slot >= PendingRefOp::VertexBuffer0 && op.slot <= PendingRefOp::VertexBuffer15) {
    unsigned i = op.slot - PendingRefOp::VertexBuffer0;
    install(m_encodeSideRefs.vertex_buffers[i], op.com_ptr);
    return;
  }
  if (op.slot >= PendingRefOp::RenderTarget0 && op.slot <= PendingRefOp::RenderTarget3) {
    unsigned i = op.slot - PendingRefOp::RenderTarget0;
    install(m_encodeSideRefs.render_targets[i], op.com_ptr);
    return;
  }
  switch (op.slot) {
  case PendingRefOp::VertexShader:
    install(m_encodeSideRefs.vertex_shader, op.com_ptr);
    return;
  case PendingRefOp::PixelShader:
    install(m_encodeSideRefs.pixel_shader, op.com_ptr);
    return;
  case PendingRefOp::VertexDeclaration:
    install(m_encodeSideRefs.vertex_declaration, op.com_ptr);
    return;
  case PendingRefOp::DepthStencilSurface:
    install(m_encodeSideRefs.depth_stencil_surface, op.com_ptr);
    return;
  case PendingRefOp::IndexBuffer:
    install(m_encodeSideRefs.index_buffer, op.com_ptr);
    return;
  default:
    return;
  }
}

// ===========================================================================
// chunk-emit helpers
// ===========================================================================
// Run on dxmt's encode thread, calling ArgumentEncodingContext primitives
// with pre-captured BatchedDraw state. File-level static to avoid vtable.

namespace {

inline bool
RtDsAttachmentsMatch(const MTLD3D9Device::BatchedDraw &a, const MTLD3D9Device::BatchedDraw &b) {
  if (a.resolved_rt_count != b.resolved_rt_count)
    return false;
  if (a.resolved_ds_handle != b.resolved_ds_handle)
    return false;
  for (unsigned i = 0; i < a.resolved_rt_count; ++i) {
    if (a.resolved_rt_handles[i] != b.resolved_rt_handles[i])
      return false;
    if (a.resolved_rt_level[i] != b.resolved_rt_level[i])
      return false;
    if (a.resolved_rt_slice[i] != b.resolved_rt_slice[i])
      return false;
    // View key carries sRGB aliasing chosen by Resolve; SRGBWRITEENABLE
    // bit flips select different views for same handle/level/slice.
    // Encoder must close when view changes (PSO compiled for one format).
    if (a.resolved_rt_view[i] != b.resolved_rt_view[i])
      return false;
  }
  if (a.resolved_ds_handle) {
    if (a.resolved_ds_level != b.resolved_ds_level)
      return false;
    if (a.resolved_ds_slice != b.resolved_ds_slice)
      return false;
    if (a.resolved_ds_view != b.resolved_ds_view)
      return false;
  }
  return true;
}

inline void
StartRenderPassForBatch_d9(ArgumentEncodingContext &ctx, const MTLD3D9Device::BatchedDraw &bd) {
  uint8_t dsv_planar = 0;
  if (bd.resolved_ds_dxmt) {
    dsv_planar = 1 | (bd.resolved_ds_has_stencil ? 2 : 0);
  }
  auto *info = ctx.startRenderPass(dsv_planar, /*dsv_readonly_flags=*/0, bd.resolved_rt_count, /*argbuf_size=*/0);

  for (unsigned i = 0; i < bd.resolved_rt_count; ++i) {
    auto &color = info->colors[i];
    // resolved_rt_dxmt[i] is the universal predicate now; every
    // surface, including buffer-backed ones, routes through a
    // dxmt::Texture wrapper after the unified-allocation refactor.
    // ctx.access does both fence tracking and Metal handle resolution.
    if (!bd.resolved_rt_dxmt[i])
      continue;
    color.attachment =
        ctx.access<PipelineStage::Pixel>(bd.resolved_rt_dxmt[i], bd.resolved_rt_view[i], ResourceAccess::ReadWrite);
    color.level = bd.resolved_rt_level[i];
    color.slice = bd.resolved_rt_slice[i];
    color.depth_plane = 0;
    // loadAction=Load is the right default; any pending Clear was
    // emitted as a standalone Clear chunk by drainPendingClear, and
    // the coalescer's Clear→Render fold (dxmt_context.cpp)
    // will upgrade this attachment's load_action to Clear and import
    // the Clear encoder's color when the targets match.
    color.load_action = WMTLoadActionLoad;
    color.store_action = WMTStoreActionStore;
  }

  if (bd.resolved_ds_dxmt) {
    auto &depth = info->depth;
    depth.attachment =
        ctx.access<PipelineStage::Pixel>(bd.resolved_ds_dxmt, bd.resolved_ds_view, ResourceAccess::ReadWrite);
    depth.level = bd.resolved_ds_level;
    depth.slice = bd.resolved_ds_slice;
    depth.depth_plane = 0;
    // loadAction=Load; pending depth/stencil clears flow through
    // drainPendingClear's standalone Clear chunk and get folded into
    // this attachment by the coalescer (dxmt_context.cpp).
    depth.load_action = WMTLoadActionLoad;
    depth.store_action = WMTStoreActionStore;
    if (bd.resolved_ds_has_stencil) {
      auto &stencil = info->stencil;
      stencil.attachment =
          ctx.access<PipelineStage::Pixel>(bd.resolved_ds_dxmt, bd.resolved_ds_view, ResourceAccess::ReadWrite);
      stencil.level = bd.resolved_ds_level;
      stencil.slice = bd.resolved_ds_slice;
      stencil.depth_plane = 0;
      stencil.load_action = WMTLoadActionLoad;
      stencil.store_action = WMTStoreActionStore;
    }
  }

  info->render_target_width = bd.resolved_rt_width;
  info->render_target_height = bd.resolved_rt_height;
  info->render_target_array_length = 1;
  // Match the PSO's raster_sample_count resolved in ResolveBatchedDrawForChunk.
  // Metal validates equality at setRenderPipelineState; a mismatch
  // hard-errors under MTL_DEBUG_LAYER.
  info->default_raster_sample_count = bd.resolved_raster_sample_count;
}

inline void
EmitCommonRenderSetup_d9(
    ArgumentEncodingContext &ctx, const MTLD3D9Device::BatchedDraw &bd, MTLD3D9Device::ChunkEmitState &s
) {
  // Per-draw POD state lives on bd.pod_snapshot now; Resolve already
  // dereferenced the same shared_ptr above to populate bd.resolved_*,
  // so reading rs here observes the same frozen snapshot.
  const DWORD *rs = bd.pod_snapshot->render_states;

  // Emit setVertex/FragmentBufferOffset when only the offset changed
  // (same buffer handle). Metal's offset-only update is roughly half
  // the cost of a full setBuffer. With all 8 const-buffer slots
  // sharing one ring allocation per draw, the handle changes only on
  // m_constRing block rotation, so the offset-only path catches the
  // steady state.
  auto enc_setbuffer = [&](WMTRenderCommandType ty, obj_handle_t buf, uint64_t off, uint8_t idx) {
    obj_handle_t *handle_shadow = (ty == WMTRenderCommandSetVertexBuffer) ? s.vs_buf_handle : s.fs_buf_handle;
    uint64_t *offset_shadow = (ty == WMTRenderCommandSetVertexBuffer) ? s.vs_buf_offset : s.fs_buf_offset;
    if (handle_shadow[idx] == buf && buf != 0) {
      // (buffer, offset) match; encoder already has the right binding.
      // Post P1b, the 7 const-upload slots hit this every cluster-hit
      // draw because their (buffer, offset) are reused verbatim.
      if (offset_shadow[idx] == off)
        return;
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setbufferoffset>();
      cmd.type = (ty == WMTRenderCommandSetVertexBuffer) ? WMTRenderCommandSetVertexBufferOffset
                                                         : WMTRenderCommandSetFragmentBufferOffset;
      cmd.offset = off;
      cmd.index = idx;
      offset_shadow[idx] = off;
      return;
    }
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setbuffer>();
    cmd.type = ty;
    cmd.buffer = buf;
    cmd.offset = off;
    cmd.index = idx;
    handle_shadow[idx] = buf;
    offset_shadow[idx] = off;
  };

  // useResource hints for each active VS stream (manual-fetch from the
  // vbuf-table reads through these by GPU address).
  for (uint32_t slot = 0; slot < D3D9_MAX_VERTEX_STREAMS; ++slot) {
    obj_handle_t h = bd.resolved_vs_resident_handles[slot];
    if (!h)
      continue;
    if (s.vs_resident[slot] == h)
      continue;
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_useresource>();
    cmd.type = WMTRenderCommandUseResource;
    cmd.resource = h;
    cmd.usage = WMTResourceUsageRead;
    cmd.stages = WMTRenderStageVertex;
    s.vs_resident[slot] = h;
  }

  // PSO bind.
  if (s.pso != bd.resolved_pso) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setpso>();
    cmd.type = WMTRenderCommandSetPSO;
    cmd.pso = bd.resolved_pso;
    s.pso = bd.resolved_pso;
  }

  // VS/PS constant buffers + vbuf table; these always re-bind because
  // m_constRing returns a fresh offset every draw.
  const auto &cu = bd.resolved_const_uploads;
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[0].buffer, cu[0].offset, 0);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[1].buffer, cu[1].offset, 1);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[2].buffer, cu[2].offset, 2);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[6].buffer, cu[6].offset, 3);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, cu[7].buffer, cu[7].offset, 4);
  enc_setbuffer(WMTRenderCommandSetVertexBuffer, bd.resolved_vbuf_table_buffer, bd.resolved_vbuf_table_offset, 16);
  enc_setbuffer(WMTRenderCommandSetFragmentBuffer, cu[3].buffer, cu[3].offset, 0);
  enc_setbuffer(WMTRenderCommandSetFragmentBuffer, cu[4].buffer, cu[4].offset, 1);
  enc_setbuffer(WMTRenderCommandSetFragmentBuffer, cu[5].buffer, cu[5].offset, 2);
  if (bd.override_vb_buffer)
    enc_setbuffer(WMTRenderCommandSetVertexBuffer, bd.override_vb_buffer, 0, 29);
  if (bd.override_ib_buffer)
    enc_setbuffer(WMTRenderCommandSetVertexBuffer, bd.override_ib_buffer, 0, 28);

  // Viewport / scissor: emit on change only. Draws within one encoder
  // can carry different viewports/scissors, and re-setting an
  // unchanged one is flagged redundant by the Metal debug layer.
  // Matches the rasterizer / DSSO / blend skips below.
  if (!s.viewport_set || std::memcmp(&s.viewport, &bd.resolved_viewport, sizeof(WMTViewport)) != 0) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setviewport>();
    cmd.type = WMTRenderCommandSetViewport;
    cmd.viewport = bd.resolved_viewport;
    s.viewport = bd.resolved_viewport;
    s.viewport_set = true;
  }
  if (!s.scissor_set || std::memcmp(&s.scissor, &bd.resolved_scissor, sizeof(WMTScissorRect)) != 0) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setscissorrect>();
    cmd.type = WMTRenderCommandSetScissorRect;
    cmd.scissor_rect = bd.resolved_scissor;
    s.scissor = bd.resolved_scissor;
    s.scissor_set = true;
  }

  // Rasterizer state.
  {
    auto fm = to_mtl_fill_mode(rs[D3DRS_FILLMODE]);
    auto cm = to_mtl_cull_mode(rs[D3DRS_CULLMODE]);
    uint32_t db_bits = rs[D3DRS_DEPTHBIAS];
    uint32_t ss_bits = rs[D3DRS_SLOPESCALEDEPTHBIAS];
    if (s.fill_mode != static_cast<int>(fm) || s.cull_mode != static_cast<int>(cm) || s.depth_bias_bits != db_bits ||
        s.slope_scale_bits != ss_bits) {
      float depth_bias;
      float slope_scale;
      std::memcpy(&depth_bias, &db_bits, sizeof(float));
      std::memcpy(&slope_scale, &ss_bits, sizeof(float));
      // D3D9 specifies bias in normalized depth space; Metal applies
      // `depth_bias * r` where r is the DS format's minimum resolvable
      // difference. Multiply by 1/r baked into resolved_depth_bias_scale
      // to restore D3D9 semantics. Slope-scale needs no scaling; both
      // APIs define it as a multiplier of dz/dx.
      depth_bias *= bd.resolved_depth_bias_scale;
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      cmd.type = WMTRenderCommandSetRasterizerState;
      cmd.fill_mode = fm;
      cmd.cull_mode = cm;
      cmd.depth_clip_mode = WMTDepthClipModeClip;
      cmd.winding = WMTWindingClockwise;
      cmd.depth_bias = depth_bias;
      cmd.scole_scale = slope_scale; // sic; typo in winemetal.h
      cmd.depth_bias_clamp = 0.0f;
      s.fill_mode = static_cast<int>(fm);
      s.cull_mode = static_cast<int>(cm);
      s.depth_bias_bits = db_bits;
      s.slope_scale_bits = ss_bits;
    }
    // D3DRS_MULTISAMPLEMASK is stored but not applied: Metal exposes no
    // API-level per-sample coverage mask on the render encoder, only a
    // shader-side [[sample_mask]] output. Honoring it would need a PS
    // variant that ANDs the mask into the coverage, the future shape if
    // an app depends on it; DXVK gates its setSampleMask on a NONMASKABLE
    // RT for the same reason.
    //
    // D3DRS_MULTISAMPLEANTIALIAS is ignored, matching DXVK: toggling MSAA
    // per-draw is hard in their API too (it forks the pipeline sample
    // count), so both backends leave it to the RT's own sample count.
  }

  // DSSO + stencil ref.
  if (bd.resolved_dsso && (s.dsso != bd.resolved_dsso || s.stencil_ref != static_cast<int>(bd.resolved_stencil_ref))) {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setdsso>();
    cmd.type = WMTRenderCommandSetDSSO;
    cmd.dsso = bd.resolved_dsso;
    cmd.stencil_ref = bd.resolved_stencil_ref;
    s.dsso = bd.resolved_dsso;
    s.stencil_ref = static_cast<int>(bd.resolved_stencil_ref);
  }

  // Blend color from D3DRS_BLENDFACTOR.
  {
    DWORD bf = rs[D3DRS_BLENDFACTOR];
    if (!s.blend_color_set || s.blend_color_bits != bf) {
      float r = static_cast<float>((bf >> 16) & 0xFF) / 255.0f;
      float g = static_cast<float>((bf >> 8) & 0xFF) / 255.0f;
      float b = static_cast<float>(bf & 0xFF) / 255.0f;
      float a = static_cast<float>((bf >> 24) & 0xFF) / 255.0f;
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setblendcolor_only>();
      cmd.type = WMTRenderCommandSetBlendColor;
      cmd.red = r;
      cmd.green = g;
      cmd.blue = b;
      cmd.alpha = a;
      s.blend_color_bits = bf;
      s.blend_color_set = true;
    }
  }

  // Per-stage textures + samplers. Bind every stage every draw; the
  // accumulated-batch path puts many BatchedDraws into one encoder, and
  // shadow-skipping a stage whose handle didn't change is fine, but
  // shadow-skipping a stage whose handle dropped to null leaves the
  // PREVIOUS draw's texture bound in the encoder. The next draw whose
  // PSO actually samples that stage then reads stale data. Track the
  // bound handle including the null state to catch the unbind transition.
  for (uint32_t stage = 0; stage < 16; ++stage) {
    const auto &rc = bd.resolved_frag_texture_dxmt[stage];
    dxmt::Texture *rc_ptr = rc.ptr();
    obj_handle_t mt;
    if (rc_ptr) {
      // Access retains allocation owning view (survives wrapper Reset via ownership).
      // Re-access on SetLOD / sRGB-toggle / swizzle change.
      uint64_t vkey = bd.resolved_frag_view[stage];
      if (rc_ptr != s.frag_tex_access[stage] || vkey != s.frag_view[stage]) {
        auto &view = ctx.access<PipelineStage::Pixel>(rc, vkey, ResourceAccess::Read);
        s.frag_tex_access[stage] = rc_ptr;
        s.frag_view[stage] = vkey;
        mt = view.texture.handle;
      } else {
        mt = s.frag_tex[stage]; // unchanged since last bind this encoder
      }
    } else {
      // No app texture: immutable device-owned dummy placeholder, bound
      // by raw handle (no fence tracking). Clear the access shadow so a
      // later app-texture rebind at this stage re-accesses; the dummy
      // bind in between moved s.frag_tex off the app handle.
      mt = bd.resolved_frag_textures[stage];
      s.frag_tex_access[stage] = nullptr;
      s.frag_view[stage] = 0;
    }
    if (s.frag_tex[stage] != mt) {
      if (mt) {
        auto &uc = ctx.encodeRenderCommand<wmtcmd_render_useresource>();
        uc.type = WMTRenderCommandUseResource;
        uc.resource = mt;
        uc.usage = WMTResourceUsageRead;
        uc.stages = WMTRenderStageFragment;
      }
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_settexture>();
      cmd.type = WMTRenderCommandSetFragmentTexture;
      cmd.texture = mt;
      cmd.index = static_cast<uint8_t>(stage);
      s.frag_tex[stage] = mt;
    }
    obj_handle_t smp = bd.resolved_frag_samplers[stage];
    if (s.frag_smp[stage] != smp) {
      auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_setsamplerstate>();
      cmd.type = WMTRenderCommandSetFragmentSamplerState;
      cmd.sampler = smp;
      cmd.index = static_cast<uint8_t>(stage);
      s.frag_smp[stage] = smp;
    }
  }
}

inline void
EmitDrawCommand_d9(ArgumentEncodingContext &ctx, const MTLD3D9Device::BatchedDraw &bd) {
  // Per MSDN + wined3d (device.c) + DXVK (d3d9_device.cpp):
  // "Instancing is ignored for non-indexed draws." Stream-0 frequency
  // INDEXEDDATA is the source of the instance multiplier and only
  // takes effect when the IB drives the per-vertex address; apps
  // that OR INSTANCEDATA into stream[0] freq before a non-indexed
  // draw observe instance_count = 1 on the reference layers.
  uint32_t instance_count = 1;
  if (bd.type == MTLD3D9Device::BatchedDraw::kIndexed) {
    UINT s0_freq = bd.pod_snapshot->stream_freq[0];
    if (s0_freq & D3DSTREAMSOURCE_INDEXEDDATA)
      instance_count = std::max(s0_freq & 0x007FFFFFu, 1u);
  }

  if (bd.type == MTLD3D9Device::BatchedDraw::kIndexed) {
    uint32_t index_size = (bd.resolved_ib_fmt == static_cast<uint32_t>(DXSO_INDEX_BUFFER_FORMAT_UINT32)) ? 4u : 2u;
    WMTIndexType index_type = (bd.resolved_ib_fmt == static_cast<uint32_t>(DXSO_INDEX_BUFFER_FORMAT_UINT32))
                                  ? WMTIndexTypeUInt32
                                  : WMTIndexTypeUInt16;
    obj_handle_t ib_handle = bd.resolved_ib_handle;
    uint64_t ib_base = bd.resolved_ib_base_offset;
    uint64_t index_offset = ib_base + static_cast<uint64_t>(bd.start_vertex_or_index) * index_size;
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_draw_indexed>();
    cmd.type = WMTRenderCommandDrawIndexed;
    cmd.primitive_type = to_mtl_prim_type(bd.primitive_type);
    cmd.index_type = index_type;
    cmd.index_count = bd.vertex_or_index_count;
    cmd.index_buffer = ib_handle;
    cmd.index_buffer_offset = index_offset;
    cmd.instance_count = instance_count;
    cmd.base_vertex = bd.base_vertex;
    cmd.base_instance = 0;
  } else {
    auto &cmd = ctx.encodeRenderCommand<wmtcmd_render_draw>();
    cmd.type = WMTRenderCommandDraw;
    cmd.primitive_type = to_mtl_prim_type(bd.primitive_type);
    cmd.vertex_start = bd.start_vertex_or_index;
    cmd.vertex_count = bd.vertex_or_index_count;
    cmd.instance_count = instance_count;
    cmd.base_instance = 0;
  }
}

inline void
EmitBlitOp_d9(ArgumentEncodingContext &ctx, MTLD3D9Device::PendingBlitOp &op) {
  // Register src/dst access for cross-encoder dependency tracking.
  // Without them, same-RT Render-merge folds across blit, executing blit before renders.
  auto src_tex = ctx.access<PipelineStage::Compute>(op.src_tex, op.src_mip, op.src_slice, ResourceAccess::Read);
  auto dst_tex = ctx.access<PipelineStage::Compute>(op.dst_tex, op.dst_mip, op.dst_slice, ResourceAccess::Write);
  auto &cmd = ctx.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
  cmd.type = WMTBlitCommandCopyFromTextureToTexture;
  cmd.src = src_tex.handle;
  cmd.src_slice = op.src_slice;
  cmd.src_level = op.src_mip;
  cmd.src_origin = op.src_origin;
  cmd.src_size = op.size;
  cmd.dst = dst_tex.handle;
  cmd.dst_slice = op.dst_slice;
  cmd.dst_level = op.dst_mip;
  cmd.dst_origin = op.dst_origin;
}

// Render-pass StretchRect for different extents/format aliases.
// ctx.stretchBlit opens its own encoder; TextureViewKey selects mip level.
inline void
EmitStretchBlitOp_d9(StretchBlitContext &stretch_cmd, MTLD3D9Device::PendingBlitOp &op) {
  TextureViewKey src_view = op.src_tex->createView({
      .format = op.src_tex->pixelFormat(),
      .type = WMTTextureType2D,
      .firstMiplevel = static_cast<uint32_t>(op.src_mip),
      .miplevelCount = 1,
      .firstArraySlice = static_cast<uint32_t>(op.src_slice),
      .arraySize = 1,
  });
  TextureViewKey dst_view = op.dst_tex->createView({
      .format = op.dst_tex->pixelFormat(),
      .type = WMTTextureType2D,
      .firstMiplevel = static_cast<uint32_t>(op.dst_mip),
      .miplevelCount = 1,
      .firstArraySlice = static_cast<uint32_t>(op.dst_slice),
      .arraySize = 1,
  });
  auto filter = (op.filter == D3DTEXF_LINEAR) ? StretchBlitContext::Filter::Linear : StretchBlitContext::Filter::Point;
  stretch_cmd.blit(
      op.src_tex, src_view, op.dst_tex, dst_view, filter, op.src_origin, op.size, op.dst_origin, op.dst_size
  );
}

// MSAA-resolve via StretchRect: src is multisampled, dst is single-
// sampled, formats lower to the same Metal pixel format (gated at
// the calling-thread site). Routes through ResolveTextureContext,
// which builds a per-format PSO that averages the samples in the
// fragment shader (DXMTResolveMetadata src_origin + size give the
// scissor). The encoder opens its own render pass; like Stretch, the
// walker must end any open pass first.
inline void
EmitResolveBlitOp_d9(ResolveTextureContext &resolve_cmd, MTLD3D9Device::PendingBlitOp &op) {
  TextureViewKey src_view = op.src_tex->createView({
      .format = op.src_tex->pixelFormat(),
      .type = WMTTextureType2DMultisample,
      .firstMiplevel = static_cast<uint32_t>(op.src_mip),
      .miplevelCount = 1,
      .firstArraySlice = static_cast<uint32_t>(op.src_slice),
      .arraySize = 1,
  });
  TextureViewKey dst_view = op.dst_tex->createView({
      .format = op.dst_tex->pixelFormat(),
      .type = WMTTextureType2D,
      .firstMiplevel = static_cast<uint32_t>(op.dst_mip),
      .miplevelCount = 1,
      .firstArraySlice = static_cast<uint32_t>(op.dst_slice),
      .arraySize = 1,
  });
  WMTScissorRect src_rect = {
      op.src_origin.x,
      op.src_origin.y,
      op.size.width,
      op.size.height,
  };
  resolve_cmd.resolve(
      op.src_tex, src_view, op.dst_tex, dst_view, ResolveTextureMode::Average, src_rect, op.dst_origin, op.dst_size
  );
}

// Pass-kind discriminant used by the op-stream walker below to track
// the currently-open encoder (Render vs Blit vs None). Lifted out of
// the inline walker so the walker body (which lives inside the
// chunk->emitcc lambda in FlushDrawBatch) can stay flat.
enum class D9PassKind { None, Render, Blit };

} // namespace

bool
MTLD3D9Device::ResolveBatchedDrawForChunk(
    BatchedDraw &bd, uint64_t chunk_seq, uint64_t chunk_coherent_id, ConstUploadCache &const_cache,
    ResolveCache &resolve_cache
) { return false; }


HRESULT
MTLD3D9Device::FlushDrawBatch() { return E_NOTIMPL; }


void
MTLD3D9Device::emitCmdbufTailSignal() {
  uint64_t signal_seq = m_currentCmdSeq;
  obj_handle_t event_handle = m_completionEvent.handle;
  auto *chunk = m_dxmtQueue->CurrentChunk();
  chunk->emitcc([event_handle, signal_seq](ArgumentEncodingContext &ctx) mutable {
    ctx.signalEventByHandle(event_handle, signal_seq);
  });
  ++m_currentCmdSeq;
}

void
MTLD3D9Device::forceFlushAndCommit() {
  FlushDrawBatch();
  flushOpenWork();
  emitCmdbufTailSignal();
  m_dxmtQueue->CommitCurrentChunk();
}

void
MTLD3D9Device::refreshSignaledAndTrimRings() {
  // Post-3.8 old finalize-tail path is dead; without explicit signaledValue refresh,
  // ring reuse predicate never holds, burning fresh placed-buffer blocks per sub-allocate.
  constexpr uint64_t kRingRefreshGap = 8;
  if (m_currentCmdSeq - m_lastRingRefreshSeq < kRingRefreshGap)
    return;
  m_lastRingRefreshSeq = m_currentCmdSeq;
  uint64_t signalled = m_completionEvent.signaledValue();
  m_cachedSignaled.store(signalled, std::memory_order_release);
  m_constRing.free_blocks(signalled);
  m_uploadRing.free_blocks(signalled);
}

// Shared fan-list buffer [0,1,2, 0,2,3, ...] up to kFanListPrimCap.
// Static-fan draws avoid per-call m_constRing allocate.
obj_handle_t
MTLD3D9Device::fanListIBForPrimCount(uint32_t prim_count) {
  constexpr uint32_t kFanListPrimCap = 4096;
  if (prim_count == 0 || prim_count > kFanListPrimCap)
    return 0;
  if (m_fanListIB == nullptr) {
    const size_t bytes = static_cast<size_t>(kFanListPrimCap) * 3 * sizeof(uint32_t);
    void *backing = wsi::aligned_malloc(bytes, DXMT_PAGE_SIZE);
    if (!backing)
      return 0;
    auto *idx = static_cast<uint32_t *>(backing);
    for (uint32_t k = 0; k < kFanListPrimCap; ++k) {
      idx[k * 3 + 0] = 0;
      idx[k * 3 + 1] = k + 1;
      idx[k * 3 + 2] = k + 2;
    }
    WMTBufferInfo info{};
    info.length = bytes;
    info.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
    info.memory.set(backing);
    m_fanListIB = m_metalDevice.newBuffer(info);
    if (m_fanListIB == nullptr) {
      wsi::aligned_free(backing);
      return 0;
    }
    m_fanListIBBacking = backing;
  }
  return m_fanListIB.handle;
}

// Shared fan-emulation IB resolver: dedupes the four Draw* fan
// branches. Synthesised cases (src=null) hit the pre-baked cache when
// PrimitiveCount fits; remapped cases (DIP / DIP_UP) and cache-miss
// cases allocate from m_constRing and call fill_fan_to_list_indices
// directly. The returned offset is bytes into the buffer's
// mapped_address; the caller stores it in BatchedDraw.override_ib_offset.
std::pair<obj_handle_t, uint32_t>
MTLD3D9Device::BuildFanIndexBuffer(uint32_t prim_count, const void *src, uint32_t src_idx_size) {
  if (src == nullptr) {
    obj_handle_t cached = fanListIBForPrimCount(prim_count);
    if (cached != 0)
      return {cached, 0};
  }
  size_t ib_bytes = static_cast<size_t>(prim_count) * 3 * sizeof(uint32_t);
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto [ib_block, ib_offset] = m_constRing.allocate(m_currentCmdSeq, coherent_id, ib_bytes, 4);
  fill_fan_to_list_indices(
      reinterpret_cast<uint32_t *>(static_cast<char *>(ib_block.mapped_address) + ib_offset), prim_count, src,
      src_idx_size
  );
  return {ib_block.buffer.handle, static_cast<uint32_t>(ib_offset)};
}

// DrawPrimitiveUP: inline vertex data via transient Shared MTLBuffer.
// Buffer lifetime pinned by encoder; cmdbuf retains through completion.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride
) {
  // wined3d device.c gates on vertex_declaration only; no
  // BeginScene gate. UP-draws on loading screens / OSD overlays
  // frequently fire outside any BeginScene/EndScene bracket.
  //
  // Validation order: both INVALIDCALL gates run before the
  // PrimitiveCount==0 D3D_OK early-out. DXVK D3D9DeviceEx::DrawPrimitiveUP
  // checks stride==0 (INVALIDCALL), then vertex declaration (INVALIDCALL),
  // then PrimitiveCount==0 (D3D_OK); wined3d d3d9_device_DrawPrimitiveUP
  // checks stride==0 (INVALIDCALL) then the declaration too. A zero-stride
  // call must surface INVALIDCALL even when PrimitiveCount is also 0.
  // wined3d does not null-check pVertexStreamZeroData, so neither do we.
  if (VertexStreamZeroStride == 0)
    return D3DERR_INVALIDCALL;
  // Vertex declaration must be set before any draw. wined3d device.c
  // enforces this for both UP variants (and the non-UP
  // paths defer the same gate to the wined3d core). Without it the
  // encode-side PSO build hits a null decl and produces a cryptic
  // Metal validation error instead of the spec-correct D3DERR_INVALIDCALL.
  if (!m_vertexDeclaration)
    return D3DERR_INVALIDCALL;
  if (PrimitiveCount == 0)
    return D3D_OK;
  // DrawPrimitiveUP implicitly unbinds stream source 0 after the draw
  // (wined3d device.c). Apps that mix UP and non-UP draws observe NULL
  // via GetStreamSource(0) afterwards; some gate fallback paths on
  // an implicit unbind. The clear runs after QueueBatchedDraw so the
  // UP draw's BatchedDraw snapshot still captures the active stream
  // 0 binding at the point of the draw; what we're clearing is the
  // state visible to subsequent calls, not the draw itself.
  auto clear_up_stream0 = [this]() {
    if (m_vertexBuffers[0].ptr())
      QueueRefOp(PendingRefOp::VertexBuffer0, nullptr);
    m_vertexBuffers[0] = nullptr;
  };

  UINT vertex_count = prim_to_vertex_count(PrimitiveType, PrimitiveCount);
  uint64_t total_bytes = static_cast<uint64_t>(vertex_count) * VertexStreamZeroStride;

  // No autorelease pool; see DrawPrimitive for the rationale.
  // m_constRing.allocate and fanListIBForPrimCount only ever fire +1
  // retained newBuffer when they grow; the UP path otherwise just
  // memcpys into existing ring blocks and pushes a BatchedDraw.

  // Route the inline VB (and the synthesised fan IB, if any) through
  // the queue's staging_allocator instead of allocating a fresh
  // MTLBuffer per call.
  // wined3d uses the same primitive (wined3d_streaming_buffer_upload).
  // Per-call newBuffer crosses WoW64 every time and contends Metal's
  // allocator; UI / loading screens that hammer DrawPrimitiveUP fall
  // off a cliff without a ring. 16-byte alignment is the conservative
  // floor for Metal vertex-buffer offsets across all stride shapes.
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto [vb_block, vb_offset] = m_constRing.allocate(m_currentCmdSeq, coherent_id, static_cast<size_t>(total_bytes), 16);
  std::memcpy(static_cast<char *>(vb_block.mapped_address) + vb_offset, pVertexStreamZeroData, total_bytes);
  uint64_t vb_gpu_address = vb_block.gpu_address + vb_offset;

  // Fan emulation: synth a TRIANGLELIST IB and route through the
  // indexed common path. Same ring-allocator shape as the VB above.
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto [ib_handle, ib_offset_u32] = BuildFanIndexBuffer(PrimitiveCount, nullptr, 0);
    BatchedDraw draw{};
    draw.cap = BuildDrawCapture();
    draw.type = BatchedDraw::kIndexed;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.vertex_or_index_count = PrimitiveCount * 3;
    draw.override_vb_buffer = vb_block.buffer.handle;
    draw.override_vb_addr = vb_gpu_address;
    draw.override_vb_length = static_cast<uint32_t>(total_bytes);
    draw.override_vb_stride = VertexStreamZeroStride;
    draw.override_ib_buffer = ib_handle;
    draw.override_ib_offset = ib_offset_u32;
    draw.override_ib_format = D3DFMT_INDEX32;
    QueueBatchedDraw(std::move(draw));
    // Accumulate. State-change handlers + Present drain m_pendingDraws.
    clear_up_stream0();
    return D3D_OK;
  }

  BatchedDraw draw{};
  draw.cap = BuildDrawCapture();
  draw.type = BatchedDraw::kNonIndexed;
  draw.primitive_type = PrimitiveType;
  draw.vertex_or_index_count = vertex_count;
  draw.override_vb_buffer = vb_block.buffer.handle;
  draw.override_vb_addr = vb_gpu_address;
  draw.override_vb_length = static_cast<uint32_t>(total_bytes);
  draw.override_vb_stride = VertexStreamZeroStride;
  QueueBatchedDraw(std::move(draw));
  // Accumulate. State-change handlers + Present drain m_pendingDraws.
  clear_up_stream0();
  return D3D_OK;
}
// DrawIndexedPrimitiveUP: inline vertex + index via transient buffers.
// Vertex buffer sized to (MinVertexIndex + NumVertices) * stride.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData,
    D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride
) {
  // wined3d device.c gates on vertex_declaration only; no
  // BeginScene gate. Same rationale as DrawPrimitiveUP above.
  //
  // Validation order matches DXVK D3D9DeviceEx::DrawIndexedPrimitiveUP:
  // stride==0 (INVALIDCALL), then vertex declaration (INVALIDCALL), then
  // the (!PrimitiveCount || !NumVertices) D3D_OK early-out. wined3d
  // d3d9_device_DrawIndexedPrimitiveUP checks stride==0 first the same
  // way; a zero-stride call must surface INVALIDCALL even when the count
  // is also 0. wined3d/DXVK do not null-check pIndexData or
  // pVertexStreamZeroData, so neither do we.
  if (VertexStreamZeroStride == 0)
    return D3DERR_INVALIDCALL;
  // Vertex declaration must be set before any draw; wined3d device.c:
  // 3401-3406. Same rationale as DrawPrimitiveUP above.
  if (!m_vertexDeclaration)
    return D3DERR_INVALIDCALL;
  // NumVertices==0 is spec-legal (degenerate draw → no-op), same as the
  // non-UP DrawIndexedPrimitive sibling. DXVK returns D3D_OK on
  // (!PrimitiveCount || !NumVertices); wined3d doesn't check at the
  // d3d9 layer. Apps passing 0 (rare but possible from procedural mesh
  // generators that may emit empty batches) see a spurious failure if
  // we reject. Treat as a no-op.
  if (PrimitiveCount == 0 || NumVertices == 0)
    return D3D_OK;

  // Per D3D9 spec, indexed UP draws clear bound stream 0 AND the
  // bound index buffer on return (wined3d device.c). Same
  // rationale as DrawPrimitiveUP above; affects post-call state
  // observability, not the queued UP draw itself (which carries its
  // own override_vb_* / override_ib_* fields).
  auto clear_up_state = [this]() {
    if (m_vertexBuffers[0].ptr())
      QueueRefOp(PendingRefOp::VertexBuffer0, nullptr);
    m_vertexBuffers[0] = nullptr;
    if (m_indexBuffer.ptr())
      QueueRefOp(PendingRefOp::IndexBuffer, nullptr);
    m_indexBuffer = nullptr;
  };

  UINT index_count = prim_to_vertex_count(PrimitiveType, PrimitiveCount);
  // DXVK d3d9_device.cpp treats any format that is not INDEX16 as
  // 32-bit rather than rejecting it; mirror that so an unusual format
  // never trips a spurious INVALIDCALL.
  uint32_t index_size = (IndexDataFormat == D3DFMT_INDEX16) ? 2u : 4u;
  uint64_t vb_total_bytes = static_cast<uint64_t>(MinVertexIndex + NumVertices) * VertexStreamZeroStride;

  // No autorelease pool; see DrawPrimitive for the rationale.

  // Both VB and IB go through the queue's staging_allocator (same
  // shape as DrawPrimitiveUP above); a fresh newBuffer per call
  // would dominate UI/loading hot paths.
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  auto [vb_block, vb_offset] =
      m_constRing.allocate(m_currentCmdSeq, coherent_id, static_cast<size_t>(vb_total_bytes), 16);
  std::memcpy(static_cast<char *>(vb_block.mapped_address) + vb_offset, pVertexStreamZeroData, vb_total_bytes);
  uint64_t vb_gpu_address = vb_block.gpu_address + vb_offset;

  // Fan emulation: caller-supplied indices are at pIndexData[0..N-1];
  // remap them into a u32 TRIANGLELIST and route through the indexed
  // common path. The fan IB always lives in u32 (one allocation
  // covers index_size 16 / 32 inputs uniformly).
  if (PrimitiveType == D3DPT_TRIANGLEFAN) {
    auto [ib_handle, ib_offset_u32] = BuildFanIndexBuffer(PrimitiveCount, pIndexData, index_size);
    BatchedDraw draw{};
    draw.cap = BuildDrawCapture();
    draw.type = BatchedDraw::kIndexed;
    draw.primitive_type = D3DPT_TRIANGLELIST;
    draw.vertex_or_index_count = PrimitiveCount * 3;
    draw.override_vb_buffer = vb_block.buffer.handle;
    draw.override_vb_addr = vb_gpu_address;
    draw.override_vb_length = static_cast<uint32_t>(vb_total_bytes);
    draw.override_vb_stride = VertexStreamZeroStride;
    draw.override_ib_buffer = ib_handle;
    draw.override_ib_offset = ib_offset_u32;
    draw.override_ib_format = D3DFMT_INDEX32;
    QueueBatchedDraw(std::move(draw));
    // Accumulate. State-change handlers + Present drain m_pendingDraws.
    clear_up_state();
    return D3D_OK;
  }

  size_t ib_bytes = static_cast<size_t>(index_count) * index_size;
  auto [ib_block, ib_offset] = m_constRing.allocate(m_currentCmdSeq, coherent_id, ib_bytes, index_size);
  std::memcpy(static_cast<char *>(ib_block.mapped_address) + ib_offset, pIndexData, ib_bytes);

  BatchedDraw draw{};
  draw.cap = BuildDrawCapture();
  draw.type = BatchedDraw::kIndexed;
  draw.primitive_type = PrimitiveType;
  draw.vertex_or_index_count = index_count;
  draw.override_vb_buffer = vb_block.buffer.handle;
  draw.override_vb_addr = vb_gpu_address;
  draw.override_vb_length = static_cast<uint32_t>(vb_total_bytes);
  draw.override_vb_stride = VertexStreamZeroStride;
  draw.override_ib_buffer = ib_block.buffer.handle;
  draw.override_ib_offset = static_cast<uint32_t>(ib_offset);
  // Canonicalise to INDEX16/INDEX32 so the encode-side index-type
  // mapping (resolved_ib_fmt) agrees with index_size above; a non-INDEX16
  // format was uploaded as 32-bit indices.
  draw.override_ib_format = (IndexDataFormat == D3DFMT_INDEX16) ? D3DFMT_INDEX16 : D3DFMT_INDEX32;
  QueueBatchedDraw(std::move(draw));
  // Accumulate. State-change handlers + Present drain m_pendingDraws.
  clear_up_state();
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer9 *, IDirect3DVertexDeclaration9 *, DWORD) {
  // wined3d device.c implements this fully (CPU vertex
  // processing via the sysmem VB path); DXVK also implements. dxmt
  // defers; Metal has no equivalent compute-then-readback path that
  // would beat just running the vertex shader at draw time. Return
  // D3DERR_NOTAVAILABLE rather than E_NOTIMPL so apps that probe
  // ProcessVertices for capability see the spec-idiomatic "feature
  // not available" rather than the COM-generic "not implemented",
  // and gracefully skip the SW-vertex path.
  return D3DERR_NOTAVAILABLE;
}
// CreateVertexDeclaration: wined3d device.c. The element array
// includes a D3DDECL_END() terminator; MTLD3D9VertexDeclaration's
// ctor scans for it and stores the inclusive range so GetDeclaration's
// returned count matches wined3d.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) {
  if (!ppDecl)
    return D3DERR_INVALIDCALL;
  // InitReturnPtr; DXVK d3d9_device.cpp zeroes the out-pointer
  // before any other validation so failure paths leave the app's
  // local at NULL rather than a stale value.
  *ppDecl = nullptr;
  if (!pVertexElements)
    return D3DERR_INVALIDCALL;
  // Reject any non-terminator element whose Type is past the documented
  // D3DDECLTYPE range (D3DDECLTYPE_UNUSED is legal only as the
  // terminator). Pre-this check dxmt stored any byte verbatim and
  // silently emitted MTLAttributeFormatInvalid in to_mtl_attr_format,
  // producing a broken PSO at draw time.
  if (HRESULT hr = validate_vertex_elements(pVertexElements); FAILED(hr))
    return hr;
  *ppDecl = ::dxmt::ref<IDirect3DVertexDeclaration9>(new MTLD3D9VertexDeclaration(this, pVertexElements));
  return D3D_OK;
}

// SetVertexDeclaration / GetVertexDeclaration: same priv-pin shape
// as SetTexture / SetRenderTarget; cross-device check via deviceRaw().
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) {
  auto *decl = static_cast<MTLD3D9VertexDeclaration *>(pDecl);
  if (decl && decl->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapVertexDeclaration = decl;
    m_recordingBlock->m_changes.vertex_declaration = true;
    return D3D_OK;
  }
  if (m_vertexDeclaration.ptr() == decl)
    return D3D_OK;
  m_vertexDeclaration = decl;
  // Op-stream mirror: two independent refs (calling-thread shadow +
  // encode-side mirror) stay in lockstep during dual-tracking.
  if (decl)
    decl->AddRefPrivate();
  QueueRefOp(PendingRefOp::VertexDeclaration, decl);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) {
  if (!ppDecl)
    return D3DERR_INVALIDCALL;
  *ppDecl = nullptr;
  MTLD3D9VertexDeclaration *bound = m_vertexDeclaration.ptr();
  if (bound)
    *ppDecl = ::dxmt::ref<IDirect3DVertexDeclaration9>(bound);
  return D3D_OK;
}
// FVF → synthesized vertex declaration, cached per FVF dword. Shared
// by SetFVF's live and recording arms so the recording path can pin
// the decl a recorded SetFVF implies without touching the live slot.
MTLD3D9VertexDeclaration *
MTLD3D9Device::getOrCreateFvfDecl(DWORD FVF) {
  auto it = m_fvfDeclCache.find(FVF);
  if (it == m_fvfDeclCache.end()) {
    std::vector<D3DVERTEXELEMENT9> elements;
    build_fvf_decl_elements(FVF, elements);
    // CreateVertexDeclaration requires a D3DDECL_END terminator at
    // the back. build_fvf_decl_elements emits the body without it so
    // the helper is reusable for tools that want raw element arrays.
    D3DVERTEXELEMENT9 terminator{};
    terminator.Stream = 0xFF;
    terminator.Type = D3DDECLTYPE_UNUSED;
    elements.push_back(terminator);
    auto *raw = new MTLD3D9VertexDeclaration(this, elements.data(), /*selfPin=*/false);
    auto [ins, _] = m_fvfDeclCache.emplace(FVF, Com<MTLD3D9VertexDeclaration, false>{});
    ins->second = raw;
    it = ins;
  }
  return it->second.ptr();
}

// SetFVF / GetFVF: synthesise vertex decl from FVF dword.
// SetFVF and SetVertexDeclaration alias same slot; last call wins.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetFVF(DWORD FVF) {
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapFvf = FVF;
    m_recordingBlock->m_changes.fvf = true;
    // A recorded non-zero SetFVF also pins the synthesized decl, the
    // same dual-slot effect the live arm has (DXVK's SetFVF routes
    // through SetVertexDeclaration, which records the decl).
    if (FVF != 0) {
      m_recordingBlock->m_snapVertexDeclaration = getOrCreateFvfDecl(FVF);
      m_recordingBlock->m_changes.vertex_declaration = true;
    }
    return D3D_OK;
  }
  m_fvf = FVF;
  if (FVF == 0) {
    // FVF=0 is the "I'll bind my own decl" marker. Per spec it does
    // not by itself unbind the current decl; apps typically follow
    // up with SetVertexDeclaration. Mirror wined3d: leave
    // m_vertexDeclaration alone.
    return D3D_OK;
  }
  auto *new_decl = getOrCreateFvfDecl(FVF);
  if (m_vertexDeclaration.ptr() == new_decl)
    return D3D_OK;
  m_vertexDeclaration = new_decl;
  // Op-stream mirror; same shape as SetVertexDeclaration; this site
  // bypasses that setter so we push the SetRef inline.
  if (new_decl)
    new_decl->AddRefPrivate();
  QueueRefOp(PendingRefOp::VertexDeclaration, new_decl);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetFVF(DWORD *pFVF) {
  if (!pFVF)
    return D3DERR_INVALIDCALL;
  *pFVF = m_fvf;
  return D3D_OK;
}
// CreateVertexShader: freeze bytecode; AIR translation at draw time (lazy).
// Length via shader_bytecode_dword_count helper (not full decoder; swappable later).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) {
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  if (!pFunction)
    return D3DERR_INVALIDCALL;
  size_t dwords = shader_bytecode_dword_count(pFunction);
  if (dwords == 0)
    return D3DERR_INVALIDCALL;
  // pFunction is `const DWORD *` per the D3D9 COM signature; the DXSO
  // walker works on `const uint32_t *` (matching the storage type the
  // compiler keeps the bytecode in). DWORD aliases differently across
  // toolchains; uint32_t under our native macOS shim, unsigned long
  // under mingw; so the bytecode pointer needs a one-line cast at
  // the boundary rather than at every walker call site.
  const auto *words = reinterpret_cast<const uint32_t *>(pFunction);
  // Reject non-VS bytecode (PS blob bound as VS, or malformed
  // version) up front. DXVK d3d9_device.cpp does the same kind
  // mismatch check; we validate the version DWORD itself so future
  // AIR-emit doesn't have to re-walk it.
  auto header = parse_dxso_header(words, dwords);
  if (!header || header->kind != DxsoShaderKind::Vertex)
    return D3DERR_INVALIDCALL;
  auto metadata = walk_dxso_shader(words, static_cast<uint32_t>(dwords), *header);
  if (!metadata)
    return D3DERR_INVALIDCALL;
  log_shader_dump("CreateVertexShader", *header, *metadata, pFunction, dwords);
  // Dedup the compiled module by bytecode hash so re-creates of the same
  // shader share one variant cache + MTLFunctions (DXVK
  // D3D9ShaderModuleSet). The wrapper is thin and per-call; the module is
  // device-lifetime.
  auto module = getOrCreateVertexShaderModule(pFunction, dwords, std::move(*metadata));
  *ppShader = ::dxmt::ref<IDirect3DVertexShader9>(new MTLD3D9VertexShader(this, std::move(module)));
  return D3D_OK;
}

// SetVertexShader / GetVertexShader: same priv-pin shape as the
// other slot bindings. NULL is allowed (apps unbind to switch to FFP
// vertex processing).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShader(IDirect3DVertexShader9 *pShader) {
  auto *shader = static_cast<MTLD3D9VertexShader *>(pShader);
  if (shader && shader->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapVertexShader = shader;
    m_recordingBlock->m_changes.vertex_shader = true;
    return D3D_OK;
  }
  if (m_vertexShader.ptr() == shader)
    return D3D_OK;
  m_vertexShader = shader;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (shader)
    shader->AddRefPrivate();
  QueueRefOp(PendingRefOp::VertexShader, shader);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppShader) {
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  MTLD3D9VertexShader *bound = m_vertexShader.ptr();
  if (bound)
    *ppShader = ::dxmt::ref<IDirect3DVertexShader9>(bound);
  return D3D_OK;
}
// VS constant Set/Get: DXVK SetShaderConstants (d3d9_device.cpp).
// HWVP-only path: DXVK's software/hardware reg-count split collapses to
// a single bound. Get keeps an explicit overflow guard that DXVK omits.
// without it, a wrap on StartRegister+Count slips past the bound check
// and we'd memcpy out-of-range. Bool storage is a flat BOOL[] so Set
// normalises to TRUE/FALSE on store and Get is a pass-through.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4fCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4fCount > D3D9_MAX_VS_CONST_F)
    return D3DERR_INVALIDCALL;
  if (Vector4fCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    std::memcpy(
        &m_recordingBlock->m_snapVsConstantsF[StartRegister][0], pConstantData,
        static_cast<size_t>(Vector4fCount) * sizeof(float) * 4
    );
    // Grow the recorded F range (Apply restores only this span; gaps
    // inside it hold the Begin-time seed) and the reach mark Apply
    // raises the device's upload clamp from.
    auto &changes = m_recordingBlock->m_changes;
    const uint16_t rec_lo = static_cast<uint16_t>(StartRegister);
    const uint16_t rec_hi = static_cast<uint16_t>(StartRegister + Vector4fCount);
    if (changes.vs_const_f_hi <= changes.vs_const_f_lo) {
      changes.vs_const_f_lo = rec_lo;
      changes.vs_const_f_hi = rec_hi;
    } else {
      changes.vs_const_f_lo = std::min(changes.vs_const_f_lo, rec_lo);
      changes.vs_const_f_hi = std::max(changes.vs_const_f_hi, rec_hi);
    }
    if (rec_hi > m_recordingBlock->m_snapVsConstFMax)
      m_recordingBlock->m_snapVsConstFMax = rec_hi;
    return D3D_OK;
  }
  // Sticky high-water mark; bump before the memcmp short-circuit so
  // a no-op Set still advances coverage. snapshot copies this into
  // the POD pod_snapshot; encode-side clamps the upload memcpy.
  const uint16_t reach = static_cast<uint16_t>(StartRegister + Vector4fCount);
  if (reach > m_vsConstFMax) {
    m_vsConstFMax = reach;
    m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_F_MAX;
  }
  const size_t bytes = static_cast<size_t>(Vector4fCount) * sizeof(float) * 4;
  // Unchanged-value short-circuit. D3DX effect frameworks push the
  // same constant table after every technique pass; the memcmp here
  // dominates only when the data actually changed, otherwise we skip
  // the std::vector alloc + FlushDrawBatch + EmitOP entirely.
  if (std::memcmp(&m_vsConstantsF[StartRegister][0], pConstantData, bytes) == 0)
    return D3D_OK;
  std::memcpy(&m_vsConstantsF[StartRegister][0], pConstantData, bytes);
  m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_F;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4fCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4fCount > D3D9_MAX_VS_CONST_F)
    return D3DERR_INVALIDCALL;
  if (Vector4fCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  std::memcpy(pConstantData, &m_vsConstantsF[StartRegister][0], Vector4fCount * sizeof(float) * 4);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4iCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4iCount > D3D9_MAX_VS_CONST_I)
    return D3DERR_INVALIDCALL;
  if (Vector4iCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  const size_t bytes = static_cast<size_t>(Vector4iCount) * sizeof(int) * 4;
  if (m_inStateBlockRecord) {
    std::memcpy(&m_recordingBlock->m_snapVsConstantsI[StartRegister][0], pConstantData, bytes);
    m_recordingBlock->m_changes.vs_constants = true;
    return D3D_OK;
  }
  if (std::memcmp(&m_vsConstantsI[StartRegister][0], pConstantData, bytes) == 0)
    return D3D_OK;
  std::memcpy(&m_vsConstantsI[StartRegister][0], pConstantData, bytes);
  m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_I;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4iCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4iCount > D3D9_MAX_VS_CONST_I)
    return D3DERR_INVALIDCALL;
  if (Vector4iCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  std::memcpy(pConstantData, &m_vsConstantsI[StartRegister][0], Vector4iCount * sizeof(int) * 4);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - BoolCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + BoolCount > D3D9_MAX_VS_CONST_B)
    return D3DERR_INVALIDCALL;
  if (BoolCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    for (UINT i = 0; i < BoolCount; ++i)
      m_recordingBlock->m_snapVsConstantsB[StartRegister + i] = pConstantData[i] ? TRUE : FALSE;
    m_recordingBlock->m_changes.vs_constants = true;
    return D3D_OK;
  }
  // Unchanged-value short-circuit: normalise on the stack so we can
  // compare against the stored values; only commit + bump the shadow
  // generation when at least one bit actually changed.
  bool any_change = false;
  for (UINT i = 0; i < BoolCount; ++i) {
    BOOL norm = pConstantData[i] ? TRUE : FALSE;
    if (m_vsConstantsB[StartRegister + i] != norm) {
      any_change = true;
      m_vsConstantsB[StartRegister + i] = norm;
    }
  }
  if (!any_change)
    return D3D_OK;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_VS_CONST_B;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - BoolCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + BoolCount > D3D9_MAX_VS_CONST_B)
    return D3DERR_INVALIDCALL;
  if (BoolCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < BoolCount; ++i)
    pConstantData[i] = m_vsConstantsB[StartRegister + i];
  return D3D_OK;
}
// First record of a stream between Begin/EndStateBlock: copy the
// whole per-stream record from live state before the caller overwrites
// the recorded components. One streams-mask bit covers buffer + offset
// + stride + freq together, so a freq-only or source-only record must
// not leave the un-recorded components as zeros for Apply to restore
// (freq 0 isn't even a legal stream setting).
void
MTLD3D9Device::seedRecordedStream(UINT StreamNumber) {
  auto *blk = m_recordingBlock;
  const uint16_t bit = static_cast<uint16_t>(1u << StreamNumber);
  if (blk->m_changes.streams & bit)
    return;
  blk->m_changes.streams |= bit;
  blk->m_snapStreamOffsets[StreamNumber] = m_streamOffsets[StreamNumber];
  blk->m_snapStreamStrides[StreamNumber] = m_streamStrides[StreamNumber];
  blk->m_snapStreamFreq[StreamNumber] = m_streamFreq[StreamNumber];
  blk->m_snapVertexBuffers[StreamNumber] = m_vertexBuffers[StreamNumber];
}

// SetStreamSource / GetStreamSource: hot path with strict out-of-range validation
// (bad index corrupts fetch at draw). Unbind with NULL preserves offset/stride.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride
) {
  if (StreamNumber >= D3D9_MAX_VERTEX_STREAMS)
    return D3DERR_INVALIDCALL;
  auto *buffer = static_cast<MTLD3D9VertexBuffer *>(pStreamData);
  if (buffer && buffer->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    seedRecordedStream(StreamNumber);
    m_recordingBlock->m_snapVertexBuffers[StreamNumber] = buffer;
    // NULL buffer preserves offset/stride, mirroring the live arm's
    // wined3d shape on the seeded values.
    if (buffer) {
      m_recordingBlock->m_snapStreamOffsets[StreamNumber] = OffsetInBytes;
      m_recordingBlock->m_snapStreamStrides[StreamNumber] = Stride;
    }
    return D3D_OK;
  }
  // No-op rebind: same buffer + same offset/stride (or both-NULL, which
  // preserves offset/stride per wined3d device.c). Offsets and
  // strides feed BuildDrawCapture directly (not via the POD snapshot),
  // so a stride-only change still needs the gen bump to propagate.
  bool buffer_changed = m_vertexBuffers[StreamNumber].ptr() != buffer;
  if (!buffer_changed) {
    if (buffer == nullptr)
      return D3D_OK;
    if (m_streamOffsets[StreamNumber] == OffsetInBytes && m_streamStrides[StreamNumber] == Stride)
      return D3D_OK;
  }
  m_vertexBuffers[StreamNumber] = buffer;
  if (buffer) {
    m_streamOffsets[StreamNumber] = OffsetInBytes;
    m_streamStrides[StreamNumber] = Stride;
  }
  // Buffer == NULL: preserve previous offset/stride (wined3d behaviour).
  // Op-stream mirror; only push a SetRef when the BUFFER changes (the
  // ref-counted slot). Offset/stride-only changes flow through
  // BuildDrawCapture's per-stream snapshot, not D9EncodingRefs, so the
  // op stream doesn't need to record them. See SetVertexDeclaration for
  // the dual-tracking shape.
  if (buffer_changed) {
    if (buffer)
      buffer->AddRefPrivate();
    QueueRefOp(static_cast<PendingRefOp::Slot>(PendingRefOp::VertexBuffer0 + StreamNumber), buffer);
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride
) {
  // wined3d device.c; buffer out-pointer must be non-null;
  // offset is optional, stride is required. Match that.
  if (!ppStreamData || !pStride)
    return D3DERR_INVALIDCALL;
  *ppStreamData = nullptr;
  if (pOffsetInBytes)
    *pOffsetInBytes = 0;
  *pStride = 0;
  if (StreamNumber >= D3D9_MAX_VERTEX_STREAMS)
    return D3DERR_INVALIDCALL;
  MTLD3D9VertexBuffer *bound = m_vertexBuffers[StreamNumber].ptr();
  if (bound)
    *ppStreamData = ::dxmt::ref<IDirect3DVertexBuffer9>(bound);
  if (pOffsetInBytes)
    *pOffsetInBytes = m_streamOffsets[StreamNumber];
  *pStride = m_streamStrides[StreamNumber];
  return D3D_OK;
}

// SetStreamSourceFreq: wined3d device.c d3d9_device_SetStreamSourceFreq.
// Validation rules match DXVK d3d9_device.cpp: stream index in
// range, INSTANCEDATA on stream 0 is INVALIDCALL, INSTANCEDATA + INDEXED
// together is INVALIDCALL, and Setting==0 is INVALIDCALL (apps must
// either pass a divisor / count or one of the two flags). Setting==1
// (the spec default) reverts the stream to per-vertex stepping.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) {
  if (StreamNumber >= D3D9_MAX_VERTEX_STREAMS)
    return D3DERR_INVALIDCALL;
  // Native D3D9 (Wine visual.c stream_test) accepts a plain frequency
  // other than 1 and INSTANCEDATA with a zero divider; both round-trip
  // through Get; the draw path clamps the GPU step rate to >= 1. Only a
  // zero setting, both flags at once, and INSTANCEDATA on stream 0 are
  // rejected.
  if (HRESULT hr = validate_stream_source_freq(StreamNumber, Setting); FAILED(hr))
    return hr;
  if (m_inStateBlockRecord) {
    seedRecordedStream(StreamNumber);
    m_recordingBlock->m_snapStreamFreq[StreamNumber] = Setting;
    return D3D_OK;
  }
  // Unchanged-value short-circuit. stream_freq is in pod_snapshot;
  // a no-op rewrite would force a fresh COW snapshot rebuild on the
  // next QueueBatchedDraw.
  if (m_streamFreq[StreamNumber] == Setting)
    return D3D_OK;
  m_streamFreq[StreamNumber] = Setting;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_STREAM_FREQ;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) {
  if (StreamNumber >= D3D9_MAX_VERTEX_STREAMS || !pSetting)
    return D3DERR_INVALIDCALL;
  *pSetting = m_streamFreq[StreamNumber];
  return D3D_OK;
}

// SetIndices / GetIndices: wined3d device.c. Single slot,
// no stream-index validation. NULL is allowed (apps unbind before
// switching to a different draw-call shape).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData) {
  auto *buffer = static_cast<MTLD3D9IndexBuffer *>(pIndexData);
  if (buffer && buffer->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapIndexBuffer = buffer;
    m_recordingBlock->m_changes.index_buffer = true;
    return D3D_OK;
  }
  if (m_indexBuffer.ptr() == buffer)
    return D3D_OK;
  m_indexBuffer = buffer;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (buffer)
    buffer->AddRefPrivate();
  QueueRefOp(PendingRefOp::IndexBuffer, buffer);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) {
  if (!ppIndexData)
    return D3DERR_INVALIDCALL;
  *ppIndexData = nullptr;
  MTLD3D9IndexBuffer *bound = m_indexBuffer.ptr();
  if (bound)
    *ppIndexData = ::dxmt::ref<IDirect3DIndexBuffer9>(bound);
  return D3D_OK;
}
// Mirror image of CreateVertexShader. Same bytecode-length helper,
// same InitReturnPtr discipline, same lifetime shape, same kind-
// mismatch reject (DXVK d3d9_device.cpp).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) {
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  if (!pFunction)
    return D3DERR_INVALIDCALL;
  size_t dwords = shader_bytecode_dword_count(pFunction);
  if (dwords == 0)
    return D3DERR_INVALIDCALL;
  // See CreateVertexShader: DWORD aliases differently across toolchains;
  // walker takes uint32_t * to match the storage type.
  const auto *words = reinterpret_cast<const uint32_t *>(pFunction);
  auto header = parse_dxso_header(words, dwords);
  if (!header || header->kind != DxsoShaderKind::Pixel)
    return D3DERR_INVALIDCALL;
  auto metadata = walk_dxso_shader(words, static_cast<uint32_t>(dwords), *header);
  if (!metadata)
    return D3DERR_INVALIDCALL;
  log_shader_dump("CreatePixelShader", *header, *metadata, pFunction, dwords);
  // See CreateVertexShader: dedup the module by bytecode hash.
  auto module = getOrCreatePixelShaderModule(pFunction, dwords, std::move(*metadata));
  *ppShader = ::dxmt::ref<IDirect3DPixelShader9>(new MTLD3D9PixelShader(this, std::move(module)));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShader(IDirect3DPixelShader9 *pShader) {
  auto *shader = static_cast<MTLD3D9PixelShader *>(pShader);
  if (shader && shader->deviceRaw() != this)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    m_recordingBlock->m_snapPixelShader = shader;
    m_recordingBlock->m_changes.pixel_shader = true;
    return D3D_OK;
  }
  if (m_pixelShader.ptr() == shader)
    return D3D_OK;
  m_pixelShader = shader;
  // Op-stream mirror; see SetVertexDeclaration for the dual-tracking shape.
  if (shader)
    shader->AddRefPrivate();
  QueueRefOp(PendingRefOp::PixelShader, shader);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppShader) {
  if (!ppShader)
    return D3DERR_INVALIDCALL;
  *ppShader = nullptr;
  MTLD3D9PixelShader *bound = m_pixelShader.ptr();
  if (bound)
    *ppShader = ::dxmt::ref<IDirect3DPixelShader9>(bound);
  return D3D_OK;
}
// PS constant Set/Get: same shape as the VS path above; bound is
// SM3's 224 floats / 16 int / 16 bool. SM2 apps only ever address
// [0..31] of F but the API surface uses the SM3 limit.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantF(UINT StartRegister, const float *pConstantData, UINT Vector4fCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4fCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4fCount > D3D9_MAX_PS_CONST_F)
    return D3DERR_INVALIDCALL;
  if (Vector4fCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    std::memcpy(
        &m_recordingBlock->m_snapPsConstantsF[StartRegister][0], pConstantData,
        static_cast<size_t>(Vector4fCount) * sizeof(float) * 4
    );
    // See SetVertexShaderConstantF for the range + reach tracking.
    auto &changes = m_recordingBlock->m_changes;
    const uint16_t rec_lo = static_cast<uint16_t>(StartRegister);
    const uint16_t rec_hi = static_cast<uint16_t>(StartRegister + Vector4fCount);
    if (changes.ps_const_f_hi <= changes.ps_const_f_lo) {
      changes.ps_const_f_lo = rec_lo;
      changes.ps_const_f_hi = rec_hi;
    } else {
      changes.ps_const_f_lo = std::min(changes.ps_const_f_lo, rec_lo);
      changes.ps_const_f_hi = std::max(changes.ps_const_f_hi, rec_hi);
    }
    if (rec_hi > m_recordingBlock->m_snapPsConstFMax)
      m_recordingBlock->m_snapPsConstFMax = rec_hi;
    return D3D_OK;
  }
  // Sticky high-water mark; see SetVertexShaderConstantF for the
  // rationale. Bump before the memcmp short-circuit.
  const uint16_t reach = static_cast<uint16_t>(StartRegister + Vector4fCount);
  if (reach > m_psConstFMax) {
    m_psConstFMax = reach;
    m_encShadowDirty |= dxmt::D9ES_DIRTY_PS_CONST_F_MAX;
  }
  const size_t bytes = static_cast<size_t>(Vector4fCount) * sizeof(float) * 4;
  if (std::memcmp(&m_psConstantsF[StartRegister][0], pConstantData, bytes) == 0)
    return D3D_OK;
  std::memcpy(&m_psConstantsF[StartRegister][0], pConstantData, bytes);
  m_encShadowDirty |= dxmt::D9ES_DIRTY_PS_CONST_F;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4fCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4fCount > D3D9_MAX_PS_CONST_F)
    return D3DERR_INVALIDCALL;
  if (Vector4fCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  std::memcpy(pConstantData, &m_psConstantsF[StartRegister][0], Vector4fCount * sizeof(float) * 4);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantI(UINT StartRegister, const int *pConstantData, UINT Vector4iCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4iCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4iCount > D3D9_MAX_PS_CONST_I)
    return D3DERR_INVALIDCALL;
  if (Vector4iCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  const size_t bytes = static_cast<size_t>(Vector4iCount) * sizeof(int) * 4;
  if (m_inStateBlockRecord) {
    std::memcpy(&m_recordingBlock->m_snapPsConstantsI[StartRegister][0], pConstantData, bytes);
    m_recordingBlock->m_changes.ps_constants = true;
    return D3D_OK;
  }
  if (std::memcmp(&m_psConstantsI[StartRegister][0], pConstantData, bytes) == 0)
    return D3D_OK;
  std::memcpy(&m_psConstantsI[StartRegister][0], pConstantData, bytes);
  m_encShadowDirty |= dxmt::D9ES_DIRTY_PS_CONST_I;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - Vector4iCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + Vector4iCount > D3D9_MAX_PS_CONST_I)
    return D3DERR_INVALIDCALL;
  if (Vector4iCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  std::memcpy(pConstantData, &m_psConstantsI[StartRegister][0], Vector4iCount * sizeof(int) * 4);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShaderConstantB(UINT StartRegister, const BOOL *pConstantData, UINT BoolCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - BoolCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + BoolCount > D3D9_MAX_PS_CONST_B)
    return D3DERR_INVALIDCALL;
  if (BoolCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  if (m_inStateBlockRecord) {
    for (UINT i = 0; i < BoolCount; ++i)
      m_recordingBlock->m_snapPsConstantsB[StartRegister + i] = pConstantData[i] ? TRUE : FALSE;
    m_recordingBlock->m_changes.ps_constants = true;
    return D3D_OK;
  }
  bool any_change = false;
  for (UINT i = 0; i < BoolCount; ++i) {
    BOOL norm = pConstantData[i] ? TRUE : FALSE;
    if (m_psConstantsB[StartRegister + i] != norm) {
      any_change = true;
      m_psConstantsB[StartRegister + i] = norm;
    }
  }
  if (!any_change)
    return D3D_OK;
  m_encShadowDirty |= dxmt::D9ES_DIRTY_PS_CONST_B;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) {
  if (StartRegister > std::numeric_limits<UINT>::max() - BoolCount)
    return D3DERR_INVALIDCALL;
  if (StartRegister + BoolCount > D3D9_MAX_PS_CONST_B)
    return D3DERR_INVALIDCALL;
  if (BoolCount == 0)
    return D3D_OK;
  if (!pConstantData)
    return D3DERR_INVALIDCALL;
  for (UINT i = 0; i < BoolCount; ++i)
    pConstantData[i] = m_psConstantsB[StartRegister + i];
  return D3D_OK;
}
// Higher-Order Surface (N-patch / rect-patch) draws. Deprecated in
// D3D10+; almost no modern app uses these. DXVK's stub returns D3D_OK
// for the Draw* pair (warns once, silently skips the draw) so apps that
// speculatively issue them don't bail on hr-check. DeletePatch returns
// INVALIDCALL because deleting an unknown handle is per-spec illegal.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawRectPatch(UINT Handle, const float *pNumSegs, const D3DRECTPATCH_INFO *pRectPatchInfo) {
  (void)Handle;
  (void)pNumSegs;
  (void)pRectPatchInfo;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    Logger::warn("d3d9: DrawRectPatch is a stub (HOS deprecated); silently skipping");
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawTriPatch(UINT Handle, const float *pNumSegs, const D3DTRIPATCH_INFO *pTriPatchInfo) {
  (void)Handle;
  (void)pNumSegs;
  (void)pTriPatchInfo;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    Logger::warn("d3d9: DrawTriPatch is a stub (HOS deprecated); silently skipping");
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DeletePatch(UINT Handle) {
  (void)Handle;
  // No patch storage today, so any Handle is "unknown"; D3DERR_INVALIDCALL
  // matches the per-spec answer DXVK returns.
  return D3DERR_INVALIDCALL;
}
// CreateQuery: wined3d device.c d3d9_device_CreateQuery (~3940). The
// Type-only call (ppQuery == NULL) is the "is this query type
// supported?" probe; D3D_OK means yes. With ppQuery, allocate a real
// IDirect3DQuery9.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetConvolutionMonoKernel(UINT, UINT, float *, float *) {
  // DXVK d3d9_device.cpp: this is exposed via a CAPS bit
  // (D3DPMISCCAPS_TSSARGTEMP family) which neither DXVK nor dxmt
  // advertise, so the per-spec answer is INVALIDCALL. STUB_HR's
  // E_NOTIMPL was breaking hr-strict app init paths.
  return D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ComposeRects(
    IDirect3DSurface9 *pSrc, IDirect3DSurface9 *pDst, IDirect3DVertexBuffer9 *pSrcRectDescs, UINT NumRects,
    IDirect3DVertexBuffer9 *pDstRectDescs, D3DCOMPOSERECTSOP Operation, INT Xoffset, INT Yoffset
) {
  // MSDN: any of the four surface/buffer pointers null is INVALIDCALL.
  // DXVK enforces. Without this gate an app passing nulls; even a
  // smoke-test that expects the failure HRESULT to fall back; saw a
  // silent OK from the stub.
  if (!pSrc || !pDst || !pSrcRectDescs || !pDstRectDescs)
    return D3DERR_INVALIDCALL;
  (void)NumRects;
  (void)Operation;
  (void)Xoffset;
  (void)Yoffset;
  // DXVK d3d9_device.cpp: warn once + silent D3D_OK so the
  // few apps that probe this niche D3D9Ex blit-compose helper at init
  // don't bail on E_NOTIMPL. The compose itself is dropped; apps that
  // depend on it will visibly miss the blit, but ComposeRects is rare
  // (used for video overlay multi-rect composition).
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true))
    Logger::warn("d3d9: ComposeRects is a stub; silently skipping");
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::PresentEx(
    const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags
) { return E_NOTIMPL; }

// Device9Ex bookkeeping returns. Pre-Reset / pre-frame-pacing dxmt
// has nothing meaningful to back any of these with; Metal doesn't
// expose GPU-thread-priority or vblank waits, residency is implicit,
// and "device state" is always OK until Reset/Lost lands. Returning
// E_NOTIMPL here pushes engines into device-lost recovery loops on
// the per-frame callers (CheckDeviceState in particular). Match
// DXVK's contract: D3D_OK with a one-shot warn and round-trip storage
// where the API has a getter.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetGPUThreadPriority(INT *pPriority) {
  if (!pPriority)
    return D3DERR_INVALIDCALL;
  *pPriority = 0;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetGPUThreadPriority(INT Priority) {
  // MSDN: Priority must be in [-7, 7]; out-of-range is INVALIDCALL.
  // DXVK d3d9_device.cpp validates the range and otherwise no-ops.
  // wined3d returns E_NOTIMPL which trips hr-strict apps that init
  // with a default priority of 0; DXVK's silent-OK + range-gate is
  // the safer permissive shape.
  if (Priority < -7 || Priority > 7)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::WaitForVBlank(UINT iSwapChain) {
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CheckResourceResidency(IDirect3DResource9 **pResourceArray, UINT32 NumResources) {
  // Per MSDN: D3DERR_INVALIDCALL if pResourceArray is NULL while
  // NumResources is non-zero. DXVK enforces the same gate; the prior
  // silent-OK shape let app-side null-pointer bugs slip past.
  // On UMA every resource is "always resident", so the only reason to
  // walk the array would be a per-resource sanity check; leave the
  // body as the always-OK stance for now.
  if (NumResources > 0 && !pResourceArray)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetMaximumFrameLatency(UINT MaxLatency) {
  if (MaxLatency > 30)
    return D3DERR_INVALIDCALL;
  m_frameLatency = (MaxLatency == 0) ? 3u : MaxLatency;
  // No queue push here; the swapchain's Present re-pushes
  // min(m_frameLatency, BackBufferCount + 1u) per frame, mirroring DXVK
  // d3d9_swapchain.cpp GetActualFrameLatency. Pushing the raw
  // m_frameLatency here would briefly let the queue race ahead of the
  // BackBufferCount-implied limit between this setter and the next
  // Present; the per-Present clamp closes that window.
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetMaximumFrameLatency(UINT *pMaxLatency) {
  if (!pMaxLatency)
    return D3DERR_INVALIDCALL;
  *pMaxLatency = m_frameLatency;
  return D3D_OK;
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CheckDeviceState(HWND hDestinationWindow) { return E_NOTIMPL; }

// Shared Usage-bit validation for the three Create*Ex methods. DXVK
// d3d9_device.cpp (and the equivalent depth-stencil/offscreen
// blocks) gates the new D3D9Ex Usage bits: only the three RESTRICT_*
// flags are accepted; passing the corresponding D3DUSAGE_RENDERTARGET
// / D3DUSAGE_DEPTHSTENCIL bits explicitly is INVALIDCALL on Windows
// (the Ex Create methods imply the resource type). Then either of the
// shared-resource flags requires pSharedHandle.
static HRESULT
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
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateRenderTargetEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage
) {
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  if (HRESULT hr = validateCreateExUsage(Usage, pSharedHandle); hr != D3D_OK)
    return hr;
  // dxmt doesn't yet support cross-process resource sharing, so the
  // RESTRICT_* Usage bits are accepted but effectively ignored. The
  // validation above is the spec-faithful part apps hr-check.
  return CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateOffscreenPlainSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle,
    DWORD Usage
) {
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  if (HRESULT hr = validateCreateExUsage(Usage, pSharedHandle); hr != D3D_OK)
    return hr;
  return CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateDepthStencilSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle, DWORD Usage
) {
  if (!ppSurface)
    return D3DERR_INVALIDCALL;
  if (HRESULT hr = validateCreateExUsage(Usage, pSharedHandle); hr != D3D_OK)
    return hr;
  return CreateDepthStencilSurface(
      Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle
  );
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::ResetEx(D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode) {
  if (!m_isEx)
    return D3DERR_INVALIDCALL;
  // pFullscreenDisplayMode ignored (no WSI mode switch on macOS).
  // Mode change happens via swapchain rebuild.
  (void)pFullscreenDisplayMode;
  return Reset(pPresentationParameters);
}
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  if (iSwapChain != 0)
    return D3DERR_INVALIDCALL;
  if (!m_isEx)
    return D3DERR_INVALIDCALL;
  return m_parent->GetAdapterDisplayModeEx(m_creationParams.AdapterOrdinal, pMode, pRotation);
}
} // namespace dxmt
