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
    // 10-10-10-2 unsigned UNnormalized per D3D9 spec; Metal has no
    // unnormalized 10-bit integer attribute, so we map to the closest
    // normalized variant. Apps that wrote x∈[0,1023] now read x∈[0,1];
    // d3d9_interface.cpp advertises D3DDTCAPS_UDEC3 so this is a
    // documented spec/Metal-gap, matching wined3d's GL-side pragma.
    // The alternative (CPU rewrite to Float4) is what DXVK does on
    // hardware that lacks the format; not worth the per-frame cost on
    // Apple Silicon where the use is rare.
    return 41; // UInt1010102Normalized
  default:
    return 0; // Invalid
  }
}

// D3DPRIMITIVETYPE → Metal primitive class. The Metal type enum is at
// winemetal.h; the D3D9 enum is contiguous from 1
// (D3DPT_POINTLIST). D3D9 fans have no Metal equivalent; the entry
// points emulate them with an index-buffer rewrite that arrives at
// drawCommonInScene as TRIANGLELIST, so the encoder never sees a fan.
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
// PrimitiveCount cap reported via D3DCAPS9::MaxPrimitiveCount in
// d3d9_interface.cpp. D3D9 spec: a draw with PrimitiveCount above
// this cap is INVALIDCALL. wined3d defers to the wined3d core; DXVK
// gates at the d3d9 entry. We match DXVK's shape; the gate produces
// the spec-correct return code before any allocation work happens.
inline constexpr UINT D9_MAX_PRIMITIVE_COUNT = 0x00FFFFFFu;

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
apply_blend_state_to_attachment(WMTColorAttachmentBlendInfo &att, const DWORD *renderStates, DWORD writeMaskDw) {
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
  // pick by the alpha and luma of the D3DCOLOR. Same approach DXVK
  // takes in d3d9_sampler.cpp.
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
  // has the same direction. wined3d state.c sampler_lod_min_max and
  // DXVK d3d9_device.cpp setLodRange(maxMipLevel, ...) both wire it
  // 1:1; without this the sampler always reaches the largest mip even
  // when the app asked for a lower-detail view, which silently
  // overrides D3DXCreateTextureFromFile's "skip top N" behaviour.
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
// bypasses the view's encode (per Apple's clearColor docs), so the
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

// D3DMULTISAMPLE_TYPE → Metal sampleCount + accept/reject HRESULT.
// Keeps CreateRenderTarget / CreateDepthStencilSurface in lock-step
// with MTLD3D9Interface::CheckDeviceMultiSampleType: the same
// supportsTextureSampleCount probe runs at probe time and at create
// time, so apps that see the probe accept 8x on M2+/M3 GPUs don't
// then get NOTAVAILABLE from the create call. Returns
// (sample_count=1, INVALIDCALL) on out-of-enum, (1, NOTAVAILABLE) on
// in-enum-but-unsupported, (N, S_OK) on success.
inline std::pair<uint32_t, HRESULT>
multisample_type_to_metal_sample_count(D3DMULTISAMPLE_TYPE ms, WMT::Device device) {
  switch (ms) {
  case D3DMULTISAMPLE_NONE:
    return {1u, D3D_OK};
  case D3DMULTISAMPLE_2_SAMPLES:
  case D3DMULTISAMPLE_4_SAMPLES:
  case D3DMULTISAMPLE_8_SAMPLES:
  case D3DMULTISAMPLE_16_SAMPLES: {
    const uint32_t n = static_cast<uint32_t>(ms);
    return device.supportsTextureSampleCount(static_cast<uint8_t>(n))
               ? std::make_pair(n, S_OK)
               : std::make_pair(1u, static_cast<HRESULT>(D3DERR_NOTAVAILABLE));
  }
  // NONMASKABLE: Apple GPUs don't honour driver-defined quality
  // levels, so the create path stays strict here. CheckDeviceMultiSampleType
  // returns OK with quality=1 to keep capability probes happy, but a
  // resource create would have no Metal sampleCount to bind to.
  case D3DMULTISAMPLE_NONMASKABLE:
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
MTLD3D9Device::resetStateToDefaults(bool enableAutoDepthStencil) {}


void
MTLD3D9Device::initDefaultRenderStates(bool enableAutoDepthStencil) {}


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
MTLD3D9Device::drainPendingClear() {}


void
MTLD3D9Device::generateMipmaps(WMT::Texture texture, bool drain_pending_draws) {
  if (texture.handle == 0)
    return;
  // Metal's generateMipmaps REQUIRES mipmapLevelCount > 1; a single-level texture
  // has nothing to generate. Calling it on a 1-level texture trips the blit-encoder
  // validation ("[tex mipmapLevelCount](1) must be > 1"); without MTL_DEBUG_LAYER the
  // invalid blit silently HANGS the command buffer forever (the finish thread blocks in
  // waitUntilCompleted, the calling thread spins waiting for the frame to retire).
  // This can be triggered by a video-settings Reset (e.g. toggling vsync) that
  // re-runs the AUTOGENMIPMAP path against a now-1-level texture. No-op here.
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
  // stageTextureUpload / stageTextureUploadFromBuffer emit chunk lambdas
  // directly. Only the AUTOGENMIPMAP-mipsDirty sweep remains here; each
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
    uint32_t src_pitch, bool is_compressed
) {
  if (dst.handle == 0 || src == nullptr || src_pitch == 0 || size.width == 0 || size.height == 0)
    return;

  // Total bytes in the staged slice. Compressed formats are addressed
  // by 4x4 blocks, so the row count is rounded up; Metal's contract
  // for bytes_per_image when uploading a compressed texture region.
  uint32_t row_count = is_compressed ? (size.height + 3u) / 4u : size.height;
  size_t total_bytes = static_cast<size_t>(src_pitch) * static_cast<size_t>(row_count);
  uint32_t bytes_per_image = is_compressed ? src_pitch * row_count : 0u;

  // Coherent_id reads the GPU's last signalled cmdbuf seq so the ring
  // can recycle blocks whose tag has retired. Same shape as the
  // per-draw upload lambda in drawCommonInScene. Cached value
  // refreshed at flushOpenWork (post-commit); saves a wine_unix_call
  // per stageTextureUpload invocation, which streaming workloads hit
  // hundreds of times per frame.
  uint64_t coherent_id = m_cachedSignaled.load(std::memory_order_acquire);
  // 16-byte alignment matches the per-draw upload's VB/IB shape and
  // is sufficient for any format Metal accepts on this path. The
  // bump allocator pads to alignment but doesn't enforce a row
  // alignment beyond what src_pitch already encodes.
  auto [block, offset] = m_uploadRing.allocate(m_currentCmdSeq, coherent_id, total_bytes, 16);
  std::memcpy(static_cast<char *>(block.mapped_address) + offset, src, total_bytes);

  // Post the upload blit as a chunk lambda. The m_completionEvent tail
  // signal matches the FlushDrawBatch shape; m_uploadRing.free_blocks(
  // signaledValue) recycles this block once the chunk's GPU side retires.
  EmitTextureUploadChunk_d9(
      this, *m_dxmtQueue, dst, block.buffer.handle, offset, src_pitch, bytes_per_image, mip_level, slice, origin, size
  );
  ++m_currentCmdSeq;
  refreshSignaledAndTrimRings();
}

void
MTLD3D9Device::stageTextureUploadFromBuffer(
    WMT::Texture dst, uint32_t mip_level, uint32_t slice, WMTOrigin origin, WMTSize size, obj_handle_t src_buffer,
    uint64_t src_offset, uint32_t src_pitch, bool is_compressed
) {
  if (dst.handle == 0 || src_buffer == 0 || src_pitch == 0 || size.width == 0 || size.height == 0)
    return;
  uint32_t row_count = is_compressed ? (size.height + 3u) / 4u : size.height;
  uint32_t bytes_per_image = is_compressed ? src_pitch * row_count : 0u;

  EmitTextureUploadChunk_d9(
      this, *m_dxmtQueue, dst, src_buffer, src_offset, src_pitch, bytes_per_image, mip_level, slice, origin, size
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
  // Private dxmt diag surface (audit G1). Borrowed pointer; lifetime
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
  // 2005-era engines pool-size their
  // texture caches against this value; returning 0; the strictly-
  // truthful UMA answer: drives them into fallback paths that re-
  // create resources every frame. Mirror dxgi/d3d11 (dxgi_adapter.cpp
  // GetDesc / QueryVideoMemoryInfo): on UMA, half of
  // recommendedMaxWorkingSetSize. The half-split is the d3d11 shape;
  // here we clamp to UINT32 because D3D9's API returns UINT, not
  // UINT64, and apps store it in a 32-bit handle on 32-bit titles.
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
MTLD3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) { return E_NOTIMPL; }

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
MTLD3D9Device::createAutoDepthStencil(const D3DPRESENT_PARAMETERS &params) {}

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
) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVolumeTexture(
    UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle
) { return E_NOTIMPL; }


// CreateCubeTexture: same validation as CreateTexture, one dimension (EdgeLength).
// Allocates 6 faces × N levels sharing one TextureCube handle.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture,
    HANDLE *pSharedHandle
) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle
) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer,
    HANDLE *pSharedHandle
) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) { return E_NOTIMPL; }

// UpdateSurface: SYSTEMMEM → DEFAULT blit. Symmetric inverse of
// GetRenderTargetData. Validation per DXVK d3d9_device.cpp.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::UpdateSurface(
    IDirect3DSurface9 *pSourceSurface, const RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface,
    const POINT *pDestPoint
) { return E_NOTIMPL; }

// UpdateTexture: SYSTEMMEM/MANAGED master → DEFAULT/MANAGED mirror (push to GPU).
// Validation: same type/format/level-0 dims, src_levels >= dst_levels.
// MANAGED Lock/Unlock already pushes per-Unlock (independent push path).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) { return E_NOTIMPL; }

// GetRenderTargetData: RT → SYSTEMMEM blit. Validation per DXVK
// d3d9_device.cpp. DEFAULT-pool dst would forward to StretchRect
// in DXVK; we return INVALIDCALL until StretchRect lands.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) { return E_NOTIMPL; }

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
MTLD3D9Device::ColorFill(IDirect3DSurface9 *pSurface, const RECT *pRect, D3DCOLOR Color) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle
) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) { return E_NOTIMPL; }

// BeginScene / EndScene: pair-bracketed scene marker. DXVK
// (d3d9_device.cpp) and wined3d (device.c) both track an
// in_scene flag and reject misnested calls with INVALIDCALL. The
// bracket is also where DXVK fires an implicit-flush hint at EndScene;
// we'll wire that in once command-buffer plumbing lands.
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
  if (m_inStateBlockRecord)
    m_recordingChanges.transforms = true;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.viewport = true;
  D3DVIEWPORT9 vp = *pViewport;
  // DXVK normalises inverted Z (d3d9_device.cpp); Metal's
  // viewport rejects MaxZ <= MinZ at draw time.
  if (!(vp.MinZ < vp.MaxZ))
    vp.MaxZ = vp.MinZ + 0.001f;
  // Unchanged-value short-circuit: D3D9 effect frameworks re-set the
  // same viewport every pass.
  if (std::memcmp(&m_viewport, &vp, sizeof(D3DVIEWPORT9)) == 0)
    return D3D_OK;
  m_viewport = vp;
  m_viewportDirty = true;
  // The "scissor disabled" branch in wmt_scissor_from_d3d9 returns
  // viewport bounds, so a viewport change implicitly invalidates the
  // applied scissor too.
  m_scissorDirty = true;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.material = true;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.lights = true;
  if (Index >= m_lights.size()) {
    m_lights.resize(Index + 1, D3DLIGHT9{});
    m_lightEnables.resize(Index + 1, FALSE);
  }
  m_lights[Index] = *pLight;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetLight(DWORD Index, D3DLIGHT9 *pLight) {
  if (!pLight)
    return D3DERR_INVALIDCALL;
  // wined3d state.c (light_index_get) returns INVALIDCALL when the
  // index has never been set. Sparse-grown vector slots default to
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
  if (m_inStateBlockRecord)
    m_recordingChanges.lights = true;
  if (Index >= m_lights.size()) {
    D3DLIGHT9 def{};
    def.Type = D3DLIGHT_DIRECTIONAL;
    def.Diffuse = {1.0f, 1.0f, 1.0f, 0.0f};
    def.Direction = {0.0f, 0.0f, 1.0f};
    m_lights.resize(Index + 1, D3DLIGHT9{});
    m_lightEnables.resize(Index + 1, FALSE);
    m_lights[Index] = def;
  }
  m_lightEnables[Index] = Enable ? TRUE : FALSE;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.clip_planes = true;
  // D3D9 caps higher indices to the last valid slot rather than
  // erroring. cf. DXVK d3d9_device.cpp.
  if (Index >= 8)
    Index = 7;
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
  // 1..6 holdovers, 210..255 reserved) always read back as 0,
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
MTLD3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::BeginStateBlock() {
  if (m_inStateBlockRecord)
    return D3DERR_INVALIDCALL;
  m_inStateBlockRecord = true;
  // wined3d_stateblock_create starts a recorded block with a fresh
  // changed mask: anything Set* between Begin and End sets the
  // corresponding bit, EndStateBlock hands the mask to the new block.
  m_recordingChanges.reset();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Device::EndStateBlock(IDirect3DStateBlock9 **ppSB) { return E_NOTIMPL; }

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
MTLD3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) { return E_NOTIMPL; }


// FFP texture-blend: stage 0..7, type D3DTSS_COLOROP..CONSTANT (1..32).
// Out-of-range: INVALIDCALL (strict gate, no DMAP-style ignore).
// Programmable-PS apps call even with active shaders; return OK (not E_NOTIMPL) matching DXVK.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  // wined3d d3d9/device.c returns D3D_OK silently for out-of-range
  // Type and does NOT bound Stage at all; DXVK d3d9_device.cpp
  // clamps and also returns D3D_OK. dxmt previously returned INVALIDCALL
  // on OOR which tripped hr-strict app init paths. Drop the failure;
  // silently ignore OOR.
  if (Stage >= 8 || Type == 0 || Type > D3DTSS_CONSTANT)
    return D3D_OK;
  if (m_inStateBlockRecord)
    m_recordingChanges.texture_stage_states = true;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.sampler_states = true;
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
// apps' init paths hr-check these. Pre-port the four methods returned
// E_NOTIMPL, which made hr-strict apps fail device-create.
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
  if (m_inStateBlockRecord)
    m_recordingChanges.scissor = true;
  // Unchanged-value short-circuit (DXVK d3d9_device.cpp).
  if (std::memcmp(&m_scissorRect, pRect, sizeof(RECT)) == 0)
    return D3D_OK;
  m_scissorRect = *pRect;
  m_scissorDirty = true;
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
  // DXVK D3D9DeviceEx::SetSoftwareVertexProcessing silently accepts.
  // Metal is always hardware-VP; the bool has no effect either way.
  // The prior STUB_HR returned E_NOTIMPL, which made apps that init
  // with SetSoftwareVertexProcessing(FALSE); a defensive "stay in
  // HW" call; see a failure where wined3d / DXVK report success.
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
) { return E_NOTIMPL; }


// Shared body: bound-stream differ in IB/count/BaseVertexIndex; UP inject transient slot-0.
// Validation gate reads from capture (caller-provided or UP-built at queue time).
// UP build at queue time ensures validation sees shader/RT bindings from draw thread, not FlushDrawBatch thread.
MTLD3D9Device::D3D9DrawCapture
MTLD3D9Device::BuildDrawCapture() { return {}; }


void
MTLD3D9Device::QueueBatchedDraw(BatchedDraw &&draw) {
  // Freeze POD state: m_encShadowDirty==0 means all draws share one shared_ptr.
  // Non-zero: copy-construct fresh snapshot, overwrite ONLY dirty axes (~13KB vs ~20KB rebuild).
  // Resolve reads draw.pod_snapshot (not m_d9EncState), letting setters skip FlushDrawBatch (each frozen
  // independently).
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
    ArgumentEncodingContext &ctx, const MTLD3D9Device::BatchedDraw &bd, MTLD3D9Device::ChunkEmitState &s,
    bool first_in_pass
) {
  // Per-draw POD state lives on bd.pod_snapshot now; Resolve already
  // dereferenced the same shared_ptr above to populate bd.resolved_*,
  // so reading rs here observes the same frozen snapshot.
  const DWORD *rs = bd.pod_snapshot->render_states;

  // Emit setVertex/FragmentBufferOffset when only the offset changed
  // (same buffer handle). Metal's offset-only update is roughly half
  // the cost of a full setBuffer. After the task #23 const-ring pack,
  // all 8 const-buffer slots share one buffer per draw; that handle
  // changes only on m_constRing block rotation, so the offset-only
  // path catches the steady state.
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

  // Viewport / scissor: emit on change only. An earlier first_in_pass-only
  // path was wrong with accumulated batches (HUD/UI draws with a different
  // viewport/scissor than scene draws silently inherited the first draw's
  // setup); the unconditional per-draw emit that replaced it is correct but
  // re-sets the same value every draw, which Metal flags as redundant. The
  // shadow compare keeps the accumulated-batch correctness; a changed
  // viewport/scissor still forces the emit; while skipping the steady-state
  // re-set. Matches the rasterizer / DSSO / blend skips below.
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
    m_fanListIBPrimCount = kFanListPrimCap;
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
) { return E_NOTIMPL; }

// DrawIndexedPrimitiveUP: inline vertex + index via transient buffers.
// Vertex buffer sized to (MinVertexIndex + NumVertices) * stride.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData,
    D3DFORMAT IndexDataFormat, const void *pVertexStreamZeroData, UINT VertexStreamZeroStride
) { return E_NOTIMPL; }

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
MTLD3D9Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) { return E_NOTIMPL; }


// SetVertexDeclaration / GetVertexDeclaration: same priv-pin shape
// as SetTexture / SetRenderTarget; cross-device check via deviceRaw().
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) { return E_NOTIMPL; }

// SetFVF / GetFVF: synthesise vertex decl from FVF dword.
// SetFVF and SetVertexDeclaration alias same slot; last call wins.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetFVF(DWORD FVF) { return E_NOTIMPL; }


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
MTLD3D9Device::CreateVertexShader(const DWORD *pFunction, IDirect3DVertexShader9 **ppShader) { return E_NOTIMPL; }


// SetVertexShader / GetVertexShader: same priv-pin shape as the
// other slot bindings. NULL is allowed (apps unbind to switch to FFP
// vertex processing).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetVertexShader(IDirect3DVertexShader9 *pShader) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetVertexShader(IDirect3DVertexShader9 **ppShader) { return E_NOTIMPL; }

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
  if (m_inStateBlockRecord)
    m_recordingChanges.vs_constants = true;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.vs_constants = true;
  const size_t bytes = static_cast<size_t>(Vector4iCount) * sizeof(int) * 4;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.vs_constants = true;
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
// SetStreamSource / GetStreamSource: hot path with strict out-of-range validation
// (bad index corrupts fetch at draw). Unbind with NULL preserves offset/stride.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride
) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride
) { return E_NOTIMPL; }


// SetStreamSourceFreq: wined3d device.c d3d9_device_SetStreamSourceFreq.
// Validation rules match DXVK d3d9_device.cpp: stream index in
// range, INSTANCEDATA on stream 0 is INVALIDCALL, INSTANCEDATA + INDEXED
// together is INVALIDCALL, and Setting==0 is INVALIDCALL (apps must
// either pass a divisor / count or one of the two flags). Setting==1
// (the spec default) reverts the stream to per-vertex stepping.
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Setting) { return E_NOTIMPL; }


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
MTLD3D9Device::SetIndices(IDirect3DIndexBuffer9 *pIndexData) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetIndices(IDirect3DIndexBuffer9 **ppIndexData) { return E_NOTIMPL; }

// Mirror image of CreateVertexShader. Same bytecode-length helper,
// same InitReturnPtr discipline, same lifetime shape, same kind-
// mismatch reject (DXVK d3d9_device.cpp).
HRESULT STDMETHODCALLTYPE
MTLD3D9Device::CreatePixelShader(const DWORD *pFunction, IDirect3DPixelShader9 **ppShader) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::SetPixelShader(IDirect3DPixelShader9 *pShader) { return E_NOTIMPL; }


HRESULT STDMETHODCALLTYPE
MTLD3D9Device::GetPixelShader(IDirect3DPixelShader9 **ppShader) { return E_NOTIMPL; }

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
  if (m_inStateBlockRecord)
    m_recordingChanges.ps_constants = true;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.ps_constants = true;
  const size_t bytes = static_cast<size_t>(Vector4iCount) * sizeof(int) * 4;
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
  if (m_inStateBlockRecord)
    m_recordingChanges.ps_constants = true;
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
