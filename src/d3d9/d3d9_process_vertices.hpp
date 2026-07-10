#pragma once

#include "d3d9.h"
#include "d3d9_matrix.hpp"

namespace dxmt {

// Pure CPU vertex-position transform for ProcessVertices, ported from wined3d's
// process_vertices_strided (dlls/wined3d/device.c): a row-vector object-space
// position is carried through the folded world*view*projection, perspective
// divided, and mapped onto the D3D9 viewport as screen-space XYZRHW. wined3d
// disables the clip branch in this path (its clip arm is documented-broken and
// FIXME-gated off), so this is the unclipped transform: the one the wine visual
// test test_process_vertices (dlls/d3d9/tests/visual.c) pins byte-exact on a
// plain HARDWARE_VERTEXPROCESSING device. out receives {x, y, z, rhw}.
//
// The arithmetic mirrors wined3d step for step so the pinned output matches:
// divide the clip coordinates by w, negate y (D3D screen space grows downward),
// scale by half the viewport extent, bias by the viewport centre + origin, map
// z into [MinZ, MaxZ], and store rhw = 1 / w. wined3d builds wvp as
// world * view * projection under the row-vector convention (multiply_matrix),
// which is exactly mat4_multiply(mat4_multiply(world, view), projection); the
// device folds the same product for the generated fixed-function shader.
inline void
process_vertex_to_screen(const D3DMATRIX &wvp, const float pos[3], const D3DVIEWPORT9 &vp, float out[4]) {
  // clip = [pos, 1] * wvp (wined3d: x = p0*_11 + p1*_21 + p2*_31 + _41, ...).
  float clip[4];
  for (int c = 0; c < 4; ++c)
    clip[c] = pos[0] * wvp.m[0][c] + pos[1] * wvp.m[1][c] + pos[2] * wvp.m[2][c] + wvp.m[3][c];

  const float w = clip[3];
  float x = clip[0] / w;
  float y = clip[1] / w;
  float z = clip[2] / w;

  y = -y;

  const float half_w = static_cast<float>(vp.Width) * 0.5f;
  const float half_h = static_cast<float>(vp.Height) * 0.5f;

  x = x * half_w;
  y = y * half_h;
  z = z * (vp.MaxZ - vp.MinZ);

  x = x + (half_w + static_cast<float>(vp.X));
  y = y + (half_h + static_cast<float>(vp.Y));
  z = z + vp.MinZ;

  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = 1.0f / w;
}

// Byte size of a D3DDECLTYPE, for the ProcessVertices copy-through of the
// non-position destination elements (diffuse / specular / texcoords). Covers
// every D3DDECLTYPE a vertex declaration can carry (the D3D9 spec table),
// unlike the FVF-only lowering table in d3d9_fvf.cpp; D3DDECLTYPE_UNUSED and
// any out-of-enum value report 0. Kept in this header so the copy walk and its
// host pin share one source of truth.
inline uint32_t
decl_element_byte_size(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return 4;
  case D3DDECLTYPE_FLOAT2:
    return 8;
  case D3DDECLTYPE_FLOAT3:
    return 12;
  case D3DDECLTYPE_FLOAT4:
    return 16;
  case D3DDECLTYPE_D3DCOLOR:
    return 4;
  case D3DDECLTYPE_UBYTE4:
  case D3DDECLTYPE_UBYTE4N:
    return 4;
  case D3DDECLTYPE_SHORT2:
  case D3DDECLTYPE_SHORT2N:
  case D3DDECLTYPE_USHORT2N:
    return 4;
  case D3DDECLTYPE_SHORT4:
  case D3DDECLTYPE_SHORT4N:
  case D3DDECLTYPE_USHORT4N:
    return 8;
  case D3DDECLTYPE_UDEC3:
  case D3DDECLTYPE_DEC3N:
    return 4;
  case D3DDECLTYPE_FLOAT16_2:
    return 4;
  case D3DDECLTYPE_FLOAT16_4:
    return 8;
  default:
    return 0;
  }
}

} // namespace dxmt
