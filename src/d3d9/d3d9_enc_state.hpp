#pragma once
#include "d3d9.h"

#include <array>
#include <bit>
#include <cstdint>

namespace dxmt {

// Per-draw state reaches the encode thread as a snapshot. wined3d, DXVK and
// the d3d11 sibling all stream deltas into persistent consumer-side state
// instead, so this is a deliberate divergence and the reason is placement:
// resolve runs on the encode thread because the calling thread is where this
// backend is bound, and a snapshot pointer is then an O(1) cross-thread token
// for "nothing changed", since pointer equality implies byte equality. That
// token is what lets the resolve cache and the constant upload skip whole
// draws. A delta model keeps its dirty flags on the producer thread and offers
// nothing equivalent to a consumer on another thread. The per-axis split below
// is what keeps the snapshot cheap enough for the trade to pay.
//
// Per-axis dirty mask: each POD setter ORs its bit on state change, and
// QueueBatchedDraw rebuilds exactly the axes it names. Most rebuilds carry one
// or two axes, so keeping the axes separable is what keeps the per-draw cost
// off the size of the state as a whole.
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

// Number of axes the mask defines; the block-size table below is indexed by
// bit position and must cover all of them.
inline constexpr unsigned D9ES_AXIS_COUNT = 17;
static_assert(
    D9ES_DIRTY_ALL == (1u << D9ES_AXIS_COUNT) - 1, "a new dirty bit needs a matching entry in the block-size table"
);

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

// Each axis of the per-draw snapshot lives in its own block, and the snapshot
// points at one block per axis. A draw that leaves an axis clean shares the
// previous draw's block instead of copying it, so the per-draw cost is the
// pointer header plus only the axes the app actually touched. The single
// payload member is named v throughout: the axis name is on the pointer in
// D9EncodingState, and repeating it here would only lengthen every read site.

// Render state. D3DRS_* enum runs up to 209; storage sized to 256
// to match the calling-thread m_renderStates shape (DXVK matches).
struct D9RenderStateBlock {
  DWORD v[256];
};

// Per-stage sampler state, indexed [stage][D3DSAMP_*]. wined3d's
// shape (combined PS + VS samplers).
struct D9SamplerStateBlock {
  DWORD v[D9ES_MAX_TEXTURE_UNITS][D3DSAMP_DMAPOFFSET + 1];
};

// Per-stage texture-stage state, indexed [stage][D3DTSS_*]. Read on the
// encode thread by Resolve for PS bump-env constants and the SM1.x
// projected-texturing mask, so it must be frozen per draw here rather
// than read live off the device member.
struct D9TextureStageBlock {
  DWORD v[D9ES_MAX_TEXTURE_STAGES][D3DTSS_CONSTANT + 1];
};

// User clip planes. VS path reads these when
// D3DRS_CLIPPLANEENABLE bit i is set; Resolve packs the active
// subset for upload.
struct D9ClipPlaneBlock {
  float v[8][4];
};

// Stream-source frequency / divider (SetStreamSourceFreq packing).
// The stream's offset and stride are snapshotted per draw into the
// draw capture; the buffer reference itself lives on the encode-side mirror.
struct D9StreamFreqBlock {
  UINT v[D9ES_MAX_VERTEX_STREAMS];
};

// VS/PS constant register files, one block per file so a draw that moves
// only the float file does not carry the integer and boolean ones with it.
struct D9VsConstFBlock {
  float v[D9ES_MAX_VS_CONST_F][4];
};
struct D9VsConstIBlock {
  int v[D9ES_MAX_VS_CONST_I][4];
};
struct D9VsConstBBlock {
  BOOL v[D9ES_MAX_VS_CONST_B];
};
struct D9PsConstFBlock {
  float v[D9ES_MAX_PS_CONST_F][4];
};
struct D9PsConstIBlock {
  int v[D9ES_MAX_PS_CONST_I][4];
};
struct D9PsConstBBlock {
  BOOL v[D9ES_MAX_PS_CONST_B];
};

// Fixed-function state, one block because a transform, a light and a material
// all land on the same dirty bit.
struct D9FfpBlock {
  // Fixed-function world*view*projection, precomputed on the calling
  // thread when a transform changes (never multiplied in the shader).
  // Row-major, the generated vertex function's ffp_uniforms block.
  float wvp[16];
  // Inverse of view*projection, used only by an FFP-VS draw with clip planes
  // enabled: the pack step transforms each world-space clip plane by it so the
  // shader's clip-space dot equals the world-space dot (see d3d9_matrix.hpp).
  float vp_inv[16];
  // The vertex-blend companions: world matrices 1..3 folded with
  // view*projection, consumed only when D3DRS_VERTEXBLEND names them.
  float wvp_blend[3][16];
  // The world*view product's z column: the generated vertex fog factor
  // computes view-space depth as dot(model_pos, this). The x and y
  // columns join it for the point-scale eye distance.
  float wv_z[4];
  float wv_x[4];
  float wv_y[4];
  // Per-matrix world*view columns for the D3DRS_VERTEXBLEND eye-space
  // blend: index b holds WORLDMATRIX(b + 1)'s x/y/z columns (12 floats,
  // four per column). The generated shader blends the eye position and
  // normal across the same matrices the clip position uses, so every
  // eye-space consumer (lighting, fog, texgen, point scale) reads the
  // blended value; matrix 0's columns ride wv_x/y/z.
  float wv_blend[3][12];
  // Inverse-transpose of the matrix-0 world*view: the x/y/z rows of
  // inverse(WV), dotted in the shader to move the normal into eye space
  // so lighting holds under non-uniform scale. The blend arm above keeps
  // plain per-matrix WV (both references).
  float normal[3][4];
  // Fixed-function lighting state: the material, the global-ambient-independent
  // light array (the first eight ENABLED lights, wined3d's active-light limit)
  // and how many are live. Padded plain-old-data mirror of D3DLIGHT9 (13 float4s
  // per light: diffuse, specular, ambient, position+range, direction+falloff,
  // attenuation0/1/2+theta, phi+type+pad2).
  float material[17];
  float lights[8][28];
  // The eight texture matrices (D3DTS_TEXTURE0..7), row-major.
  float tex_mats[8][16];
};

// Byte size of each axis block, indexed by dirty-bit position. QueueBatchedDraw
// sizes its one allocation from this table and then carves the blocks out of
// it, so an entry that disagreed with its block would overrun the carve. Keying
// each entry off its own dirty bit rather than listing them in order means
// reordering the enum cannot silently shift the table under the carve. A slot
// left zero is an axis whose state is a scalar the snapshot header carries.
inline constexpr std::array<size_t, D9ES_AXIS_COUNT> D9ES_AXIS_BYTES = []() constexpr {
  std::array<size_t, D9ES_AXIS_COUNT> bytes{};
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_RENDER_STATES)] = sizeof(D9RenderStateBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_SAMPLER_STATES)] = sizeof(D9SamplerStateBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_TEXTURE_STAGE_STATES)] = sizeof(D9TextureStageBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_CLIP_PLANES)] = sizeof(D9ClipPlaneBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_STREAM_FREQ)] = sizeof(D9StreamFreqBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_VS_CONST_F)] = sizeof(D9VsConstFBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_VS_CONST_I)] = sizeof(D9VsConstIBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_VS_CONST_B)] = sizeof(D9VsConstBBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_PS_CONST_F)] = sizeof(D9PsConstFBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_PS_CONST_I)] = sizeof(D9PsConstIBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_PS_CONST_B)] = sizeof(D9PsConstBBlock);
  bytes[std::countr_zero<uint32_t>(D9ES_DIRTY_FFP)] = sizeof(D9FfpBlock);
  return bytes;
}();
// Two properties the carve depends on. Every block holds nothing but four-byte
// scalars, which is what lets QueueBatchedDraw lay them back to back in one
// allocation without realigning between them. And the twelve block axes must
// land on twelve distinct bits: two initialisers naming the same bit would
// leave another slot at zero, and an axis that carves zero bytes would hand the
// next one the same address.
static_assert(
    []() constexpr {
      unsigned blocks = 0;
      for (size_t bytes : D9ES_AXIS_BYTES) {
        if (bytes % 4 != 0)
          return false;
        blocks += bytes != 0;
      }
      return blocks == 12;
    }(),
    "the axis block table is malformed: check each entry's size and its dirty bit"
);

// Per-draw POD snapshot. The blocks above hold the state; this holds one
// pointer per axis plus the values small enough that a pointer would cost
// more than the copy. Rebuilding a snapshot copies this header forward from
// the previous one, which is what makes an untouched axis free.
struct D9EncodingState {
  const D9RenderStateBlock *render_states = nullptr;
  const D9SamplerStateBlock *sampler_states = nullptr;
  const D9TextureStageBlock *texture_stage_states = nullptr;
  const D9ClipPlaneBlock *clip_planes = nullptr;
  const D9StreamFreqBlock *stream_freq = nullptr;
  const D9VsConstFBlock *vs_const_F = nullptr;
  const D9VsConstIBlock *vs_const_I = nullptr;
  const D9VsConstBBlock *vs_const_B = nullptr;
  const D9PsConstFBlock *ps_const_F = nullptr;
  const D9PsConstIBlock *ps_const_I = nullptr;
  const D9PsConstBBlock *ps_const_B = nullptr;
  const D9FfpBlock *ffp = nullptr;

  // FETCH4 arm-latch, one bit per PS sampler slot. The magic rides the
  // LOD-bias sampler state, but the armed/disarmed bit is tracked in a
  // separate device latch; Resolve reads it to pick the gather sampler
  // kind, so it rides the sampler-state axis here rather than be read
  // live off the device member. Captured alongside sampler_states.
  uint16_t fetch4_latch = 0;

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

  // How many of the FFP block's light slots are live. Rides the FFP axis but
  // stays here because a count is smaller than the pointer that would reach it.
  uint32_t ffp_light_count = 0;
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
