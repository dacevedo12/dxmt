#pragma once
#include "d3d9.h"

#include <cstdint>

namespace dxmt {

// Per-axis dirty mask: each POD setter ORs its bit on state change.
// QueueBatchedDraw copies only dirty axes (5 KB typical) vs full snapshot (10 KB).
enum D9EncStateDirtyBit : uint32_t {
  D9ES_DIRTY_RENDER_STATES = 1u << 0,
  D9ES_DIRTY_SAMPLER_STATES = 1u << 1,
  D9ES_DIRTY_CLIP_PLANES = 1u << 2,
  D9ES_DIRTY_STREAM_FREQ = 1u << 3,
  D9ES_DIRTY_VS_CONST_F = 1u << 4,
  D9ES_DIRTY_VS_CONST_I = 1u << 5,
  D9ES_DIRTY_VS_CONST_B = 1u << 6,
  D9ES_DIRTY_PS_CONST_F = 1u << 7,
  D9ES_DIRTY_PS_CONST_I = 1u << 8,
  D9ES_DIRTY_PS_CONST_B = 1u << 9,
  D9ES_DIRTY_VS_CONST_F_MAX = 1u << 10,
  D9ES_DIRTY_PS_CONST_F_MAX = 1u << 11,
  D9ES_DIRTY_VIEWPORT = 1u << 12,
  D9ES_DIRTY_SCISSOR_RECT = 1u << 13,
  D9ES_DIRTY_TEXTURE_STAGE_STATES = 1u << 14,
  D9ES_DIRTY_FFP = 1u << 15,
  // Software-vertex-processing mode, captured per draw so the encode side
  // knows whether a bound-but-hardware-unrunnable vertex shader must fall
  // back to fixed-function. Toggled by SetSoftwareVertexProcessing.
  D9ES_DIRTY_SWVP = 1u << 16,
  D9ES_DIRTY_ALL = (1u << 17) - 1,
};

// Sizes mirror the matching MTLD3D9Device::m_* shadow fields. The
// values aren't symbol-shared because d3d9_device.hpp pulls in
// CommandQueue / Texture / Sampler etc., which would bloat the
// dependency surface of this header. Keep them in sync by hand;
// static_asserts in d3d9_device.cpp verify the pairs.
inline constexpr unsigned D9ES_MAX_TEXTURE_UNITS = 20;
// FFP texture-blend stages, D3DCAPS9::MaxTextureBlendStages; matches the
// device's m_textureStageStates first dimension.
inline constexpr unsigned D9ES_MAX_TEXTURE_STAGES = 8;
inline constexpr unsigned D9ES_MAX_VERTEX_STREAMS = 16;
inline constexpr unsigned D9ES_MAX_VS_CONST_F = 256;
inline constexpr unsigned D9ES_MAX_VS_CONST_I = 16;
inline constexpr unsigned D9ES_MAX_VS_CONST_B = 16;
inline constexpr unsigned D9ES_MAX_PS_CONST_F = 224;
inline constexpr unsigned D9ES_MAX_PS_CONST_I = 16;
inline constexpr unsigned D9ES_MAX_PS_CONST_B = 16;

// Per-draw POD snapshot (COW per state cluster). Compact: ~10 KB
// (transforms parked on the calling thread until FFP).
// ~200 clusters/frame cuts bandwidth from 5 MB to 2 MB.
struct D9EncodingState {
  // Render state. D3DRS_* enum runs up to 209; storage sized to 256
  // to match the calling-thread m_renderStates shape (DXVK matches).
  DWORD render_states[256] = {};

  // Per-stage sampler state, indexed [stage][D3DSAMP_*]. wined3d's
  // shape (combined PS + VS samplers).
  DWORD sampler_states[D9ES_MAX_TEXTURE_UNITS][D3DSAMP_DMAPOFFSET + 1] = {};

  // FETCH4 arm-latch, one bit per PS sampler slot. The magic rides the
  // LOD-bias sampler state, but the armed/disarmed bit is tracked in a
  // separate device latch; Resolve reads it to pick the gather sampler
  // kind, so it rides the sampler-state axis here rather than be read
  // live off the device member. Copied alongside sampler_states.
  uint16_t fetch4_latch = 0;

  // Per-stage texture-stage state, indexed [stage][D3DTSS_*]. Read on the
  // encode thread by Resolve for PS bump-env constants and the SM1.x
  // projected-texturing mask, so it must be frozen per draw here rather
  // than read live off the device member.
  DWORD texture_stage_states[D9ES_MAX_TEXTURE_STAGES][D3DTSS_CONSTANT + 1] = {};

  // User clip planes. VS path reads these when
  // D3DRS_CLIPPLANEENABLE bit i is set; Resolve packs the active
  // subset for upload.
  float clip_planes[8][4] = {};

  // Stream-source frequency / divider (SetStreamSourceFreq packing).
  // Pure POD: the stream's buffer/offset/stride is ref-counted state
  // that lives on BatchedDraw::ref_snapshot.
  UINT stream_freq[D9ES_MAX_VERTEX_STREAMS] = {};

  // VS/PS constant register files.
  float vs_const_F[D9ES_MAX_VS_CONST_F][4] = {};
  int vs_const_I[D9ES_MAX_VS_CONST_I][4] = {};
  BOOL vs_const_B[D9ES_MAX_VS_CONST_B] = {};
  float ps_const_F[D9ES_MAX_PS_CONST_F][4] = {};
  int ps_const_I[D9ES_MAX_PS_CONST_I][4] = {};
  BOOL ps_const_B[D9ES_MAX_PS_CONST_B] = {};

  // App-side high-water mark of Set*ShaderConstantF coverage:
  // StartRegister + Vector4fCount of every setter, monotonic. ResolveBatchedDrawForChunk
  // clamps the per-draw m_constRing memcpy to (max * 16) bytes
  // instead of always copying the full register file. Sticky:
  // never decreases through the device's lifetime (matches DXVK
  // d3d9_device.cpp::maxChangedConstF: apps rarely shrink the
  // active range, so going down isn't worth the extra bookkeeping).
  // u16 fits 256 / 224 cleanly.
  uint16_t vs_const_f_max = 0;
  uint16_t ps_const_f_max = 0;

  // Software-vertex-processing mode at draw time. Resolve reads it to force
  // the fixed-function path for a bound vertex shader that references the
  // extended constant file (c256..) while the device is in hardware VP.
  uint32_t is_swvp = 0;
  // Whether the device can enter software VP at all (created SOFTWARE or
  // MIXED). Gates the fixed-function fallback above so a pure hardware-VP
  // device stays byte-identical even for a malformed shader that reads c256.
  uint32_t sw_vp_capable = 0;

  // Software / mixed-VP extended float constants c256.. for this draw. Points
  // at a copy captured from the device's overflow store into the queue's
  // command-data ring (same lifetime as this snapshot); null for a
  // hardware-VP device or a shader that does not reach the extended file.
  // vs_const_F_overflow_count is the number of float4 registers stored.
  const float (*vs_const_F_overflow)[4] = nullptr;
  uint32_t vs_const_F_overflow_count = 0;

  // Fixed-function world*view*projection, precomputed on the calling
  // thread when a transform changes (never multiplied in the shader).
  // Row-major, the generated vertex function's ffp_uniforms block.
  float ffp_wvp[16] = {};
  // Inverse of view*projection, used only by an FFP-VS draw with clip planes
  // enabled: the pack step transforms each world-space clip plane by it so the
  // shader's clip-space dot equals the world-space dot (see d3d9_matrix.hpp).
  float ffp_vp_inv[16] = {};
  // The vertex-blend companions: world matrices 1..3 folded with
  // view*projection, consumed only when D3DRS_VERTEXBLEND names them.
  float ffp_wvp_blend[3][16] = {};
  // The world*view product's z column: the generated vertex fog factor
  // computes view-space depth as dot(model_pos, this). The x and y
  // columns join it for the point-scale eye distance.
  float ffp_wv_z[4] = {};
  float ffp_wv_x[4] = {};
  float ffp_wv_y[4] = {};
  // Per-matrix world*view columns for the D3DRS_VERTEXBLEND eye-space
  // blend: index b holds WORLDMATRIX(b + 1)'s x/y/z columns (12 floats,
  // four per column). The generated shader blends the eye position and
  // normal across the same matrices the clip position uses, so every
  // eye-space consumer (lighting, fog, texgen, point scale) reads the
  // blended value; matrix 0's columns ride ffp_wv_x/y/z.
  float ffp_wv_blend[3][12] = {};
  // Inverse-transpose of the matrix-0 world*view: the x/y/z rows of
  // inverse(WV), dotted in the shader to move the normal into eye space
  // so lighting holds under non-uniform scale. The blend arm above keeps
  // plain per-matrix WV (both references).
  float ffp_normal[3][4] = {};
  // Fixed-function lighting state, captured with the FFP axis: the
  // material, the global-ambient-independent light array (the first
  // eight ENABLED lights, wined3d's active-light limit) and how many
  // are live. Padded plain-old-data mirror of D3DLIGHT9 (13 float4s
  // per light: diffuse, specular, ambient, position+range,
  // direction+falloff, attenuation0/1/2+theta, phi+type+pad2).
  float ffp_material[17] = {};
  float ffp_lights[8][28] = {};
  uint32_t ffp_light_count = 0;
  // The eight texture matrices (D3DTS_TEXTURE0..7), row-major.
  float ffp_tex_mats[8][16] = {};
  // Table-fog coordinate source, derived from the projection matrix at
  // capture (a typical perspective matrix selects eye-space w, anything
  // else device z; wined3d keys the same way). Pre-transformed draws
  // force z at resolve regardless.
  uint32_t ffp_fog_coord_w = 0;

  // Viewport / scissor: stored in D3D9 shape; Resolve runs the
  // wmt_*_from_d3d9 helpers.
  D3DVIEWPORT9 viewport = {};
  RECT scissor_rect = {};
};

} // namespace dxmt
