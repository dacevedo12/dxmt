#pragma once

#include "d3d9.h"

namespace dxmt {

// D3D9 transform-state index compaction (DXVK d3d9_util.h GetTransformIndex).
// The D3DTRANSFORMSTATETYPE enum is sparse (VIEW=2, PROJECTION=3,
// TEXTURE0..7=16..23, WORLD=256, WORLDMATRIX(N)=256+N); the dense table holds
// 0=VIEW, 1=PROJECTION, 2..9=TEXTURE0..7, 10..265=WORLD..WORLDMATRIX(255).
// kTransformStateCount is the table size. A State outside the defined space (a
// gap value, or above WORLDMATRIX(255)) maps to an index at or beyond the count
// through unsigned underflow, so a caller's `idx >= kTransformStateCount` bound
// rejects it without a separate range test.
inline constexpr uint32_t kTransformStateCount = 10 + 256;

inline uint32_t
transform_index(D3DTRANSFORMSTATETYPE State) {
  if (State == D3DTS_VIEW)
    return 0;
  if (State == D3DTS_PROJECTION)
    return 1;
  if (State >= D3DTS_TEXTURE0 && State <= D3DTS_TEXTURE7)
    return 2 + (State - D3DTS_TEXTURE0);
  return 10 + (State - D3DTS_WORLD);
}

// Row-major 4x4 multiply in D3D9's row-vector convention: mat4_multiply(a, b)
// returns the product with out[i][j] = sum_k a[i][k] * b[k][j], so a row-vector
// point transforms as v * (a * b) == (v * a) * b, i.e. a is applied first, then
// b. Every world*view*projection fold composes through this, and
// MultiplyTransform composes pMatrix * current (pMatrix applied innermost),
// matching wined3d multiply_matrix and MSDN's "pMatrix times State" order.
inline D3DMATRIX
mat4_multiply(const D3DMATRIX &a, const D3DMATRIX &b) {
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

// 4x4 inverse via the classic cofactor/adjugate expansion (the MESA
// gluInvertMatrix layout). D3DMATRIX is row-major (m[row][col]); the flat
// index used below is row*4 + col. Returns the identity for a singular matrix
// so a degenerate transform degrades to a no-op rather than NaNs.
inline D3DMATRIX
mat4_inverse(const D3DMATRIX &src) {
  const float *m = &src.m[0][0];
  float inv[16];

  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
           m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
           m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
           m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
            m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
           m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
           m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
           m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
            m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
           m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
           m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
            m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
            m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
           m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
           m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
            m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
            m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

  float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  D3DMATRIX out{};
  if (det == 0.0f) {
    for (int i = 0; i < 4; ++i)
      out.m[i][i] = 1.0f;
    return out;
  }
  det = 1.0f / det;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      out.m[i][j] = inv[i * 4 + j] * det;
  return out;
}

// Transform a clip plane so a clip-space dot equals a world-space dot.
// D3D9 uses row-vector points (clip = world * VP), and a fixed-function clip
// plane is defined in world space. Under a row-vector point transform x' = x*M,
// a plane P satisfying P.x == 0 transforms as P' = M^-1 * P (column multiply)
// to keep P.x invariant, i.e. dot(P', x') == dot(P, x). Here M = View*Projection
// and vp_inverse is its inverse, so dot(P', clip_pos) == dot(P, world_pos).
inline void
transform_clip_plane(const D3DMATRIX &vp_inverse, const float plane[4], float out[4]) {
  for (int i = 0; i < 4; ++i) {
    float s = 0.0f;
    for (int k = 0; k < 4; ++k)
      s += vp_inverse.m[i][k] * plane[k];
    out[i] = s;
  }
}

// Transform a 3-component vector by a row-vector matrix (out = [in, w] * m) and
// write back the xyz of the result. w = 1 transforms a point (picks up the
// translation row), w = 0 a direction. D3D9 fixed-function lights are specified
// in world space but the vertex pipe lights in view space, so the host runs
// each light's position (w = 1) and direction (w = 0) through the view matrix.
inline void
transform_row_vec3(const D3DMATRIX &m, const float in[3], float w, float out[3]) {
  for (int j = 0; j < 3; ++j)
    out[j] = in[0] * m.m[0][j] + in[1] * m.m[1][j] + in[2] * m.m[2][j] + w * m.m[3][j];
}

} // namespace dxmt
