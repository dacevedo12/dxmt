#pragma once

#include "d3d9.h"

#include <cstdint>

// Pure draw-path decisions (per-element input-assembler fetch layout, plus the
// triangle-fan index-buffer read bound) that ResolveBatchedDrawForChunk and the
// indexed-draw remap build from the bound declaration and index buffer, factored
// out so the host unit tier can exercise them without a Metal device or a live
// draw (the d3d9_image_lock.hpp / d3d9_update_mip.hpp shape). References:
// wined3d (PRIMARY) dlls/wined3d/context.c wined3d_stream_info_from_declaration
// + dlls/wined3d/vertexdeclaration.c (the stream-slot filtering); the generated
// fixed-function register contract lives in airconv_public.h.

namespace dxmt {

// Mirror of D3D9_MAX_VERTEX_STREAMS (d3d9_device.hpp), kept local so this
// header stays free of the device/Metal include surface; d3d9_device.cpp
// static_asserts the two equal.
inline constexpr uint32_t kIaMaxVertexStreams = 16;

// Whether a declaration element drops out of the fetch layout. Two reasons,
// both reference-shaped:
//   - Its stream is past the 16-stream cap. wined3d filters such elements out
//     of stream processing wholesale (vertexdeclaration.c, "filter tessellation
//     pseudo streams"); the stream carries no vertex_buffers[]/stream_freq[]
//     slot, so consuming it would read past those UINT[16] tables and 1u<<Stream
//     would shift out of range. CreateVertexDeclaration still accepts the decl
//     (both refs do; native rejects at create, but neither ref nor any wine test
//     pins that), so the drop happens here at draw time.
//   - Its stream has no bound vertex buffer. wined3d derives stream liveness per
//     draw (context.c wined3d_stream_info_from_declaration) and still renders,
//     with the unfed shader input reading its zero-fill default; the element
//     simply leaves the fetch layout.
// has_live_stream is the caller's per-draw liveness result for this element's
// stream (a bound VB, or the transient UP slot on stream 0); it is only meaningful
// for an in-cap stream, so the cap test short-circuits it.
inline bool
should_skip_ia_element(uint32_t stream, bool has_live_stream) {
  if (stream >= kIaMaxVertexStreams)
    return true;
  return !has_live_stream;
}

// Generated fixed-function VS input-register contract (airconv_public.h): the
// injective (usage, usage_index) -> input register map the FFP vertex shader
// fetches against. Returns -1 for a (usage, index) the generated VS does not
// consume, so the caller drops the element from the fetch layout. POSITIONT is
// remapped to POSITION by the caller before the lookup (a pre-transformed draw
// feeds its window-space position into the position slot). The programmable-VS
// path matches by the bound shader's dcl list instead and stays in the device
// (it needs the shader metadata, not a pure (usage, index) function).
inline int
ffp_input_register(uint32_t usage, uint32_t usage_index) {
  if (usage == D3DDECLUSAGE_POSITION && usage_index == 0)
    return 0;
  if (usage == D3DDECLUSAGE_COLOR && usage_index == 0)
    return 1;
  if (usage == D3DDECLUSAGE_TEXCOORD && usage_index == 0)
    return 2;
  if (usage == D3DDECLUSAGE_COLOR && usage_index == 1)
    return 3;
  if (usage == D3DDECLUSAGE_NORMAL && usage_index == 0)
    return 4;
  if (usage == D3DDECLUSAGE_TEXCOORD && usage_index < 8)
    return static_cast<int>(4 + usage_index);
  if (usage == D3DDECLUSAGE_BLENDWEIGHT && usage_index == 0)
    return 12;
  if (usage == D3DDECLUSAGE_PSIZE && usage_index == 0)
    return 13;
  // Pre-transformed (POSITIONT) passthrough set. These usages carry no
  // fixed-function vertex-pipe meaning of their own, so before POSITIONT
  // support they returned -1 and dropped out of the fetch layout; a
  // pre-transformed draw's pixel shader can still read them by semantic
  // (dcl_tangent v0, dcl_fog v0, ...), so the generated VS forwards the raw
  // attribute to a matching user-named varying (ffp_compile.cpp). Each takes
  // a distinct input register past the legacy set; Metal allows 31 vertex
  // attributes, so 14..18 stay well inside the fetch cap. A draw that is not
  // pre-transformed still adds the element to the layout, but the generated
  // VS never reads the slot, so its output is unchanged.
  if (usage == D3DDECLUSAGE_TANGENT && usage_index == 0)
    return 14;
  if (usage == D3DDECLUSAGE_BINORMAL && usage_index == 0)
    return 15;
  if (usage == D3DDECLUSAGE_DEPTH && usage_index == 0)
    return 16;
  if (usage == D3DDECLUSAGE_FOG && usage_index == 0)
    return 17;
  if (usage == D3DDECLUSAGE_BLENDINDICES && usage_index == 0)
    return 18;
  return -1;
}

// Component count of a texcoord declaration element's D3DDECLTYPE, matching
// wined3d's per-format component_count (dlls/d3d9/vertexdeclaration.c decl_types
// table). The fixed-function texture-matrix fold copies matrix columns against
// this width (a fetched coordinate pads with 0, 0, 1, so the width says which
// column carries the 1): a width-2 coordinate moves row 3 into the fourth
// column, a width-4 coordinate moves none. A Type-based guess (Type + 1) is only
// right for the FLOAT1..4 types; a packed non-float texcoord (D3DCOLOR, UBYTE4,
// SHORT4, FLOAT16_4, ...) carries 4 components, and UDEC3/DEC3N carry 3. An
// out-of-enum type falls back to 2, the same untyped default the fold assumes
// for a texcoord the declaration does not carry.
inline uint32_t
texcoord_component_count(uint32_t decl_type) {
  switch (decl_type) {
  case D3DDECLTYPE_FLOAT1:
    return 1;
  case D3DDECLTYPE_FLOAT2:
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N:
  case D3DDECLTYPE_FLOAT16_2:
    return 2;
  case D3DDECLTYPE_FLOAT3:
  case D3DDECLTYPE_UDEC3:
  case D3DDECLTYPE_DEC3N:
    return 3;
  case D3DDECLTYPE_FLOAT4:
  case D3DDECLTYPE_D3DCOLOR:
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_SHORT4:
  case D3DDECLTYPE_UBYTE4N:
  case D3DDECLTYPE_SHORT4N:
  case D3DDECLTYPE_USHORT4N:
  case D3DDECLTYPE_FLOAT16_4:
    return 4;
  default:
    return 2;
  }
}

// Triangle-fan primitive count clamped to what the index buffer's host mirror
// can supply. DrawIndexedPrimitive(TRIANGLEFAN) is remapped to a triangle list
// on the CPU by reading source indices from the mirror at StartIndex; a valid
// fan draw reads prim_count + 2 indices from there. wined3d / DXVK issue the
// equivalent read GPU-side, where an over-large StartIndex or PrimitiveCount
// yields undefined indices without a host fault; dxmt reads the mirror on the
// CPU, so it must bound the read itself. Returns the largest prim count the
// mirror can back (0 when it cannot supply the two base indices), matching the
// references' GPU-garbage tail without walking off the allocation. index_size
// is 2 or 4; a valid draw clamps to itself and is unchanged.
inline uint32_t
fan_index_prim_clamp(uint32_t ib_size_bytes, uint32_t index_size, uint32_t start_index, uint32_t prim_count) {
  const uint32_t avail = index_size ? ib_size_bytes / index_size : 0u;
  const uint32_t readable = avail > start_index ? avail - start_index : 0u;
  if (readable < 2u)
    return 0u;
  const uint32_t max_prims = readable - 2u;
  return prim_count < max_prims ? prim_count : max_prims;
}

} // namespace dxmt
