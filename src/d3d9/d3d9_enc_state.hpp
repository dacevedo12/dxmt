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
  D9ES_DIRTY_ALL = (1u << 14) - 1,
};

// Sizes mirror the matching MTLD3D9Device::m_* shadow fields. The
// values aren't symbol-shared because d3d9_device.hpp pulls in
// CommandQueue / Texture / Sampler etc., which would bloat the
// dependency surface of this header. Keep them in sync by hand;
// static_asserts in d3d9_device.cpp verify the pairs.
inline constexpr unsigned D9ES_MAX_TEXTURE_UNITS = 20;
inline constexpr unsigned D9ES_MAX_VERTEX_STREAMS = 16;
inline constexpr unsigned D9ES_MAX_VS_CONST_F = 256;
inline constexpr unsigned D9ES_MAX_VS_CONST_I = 16;
inline constexpr unsigned D9ES_MAX_VS_CONST_B = 16;
inline constexpr unsigned D9ES_MAX_PS_CONST_F = 224;
inline constexpr unsigned D9ES_MAX_PS_CONST_I = 16;
inline constexpr unsigned D9ES_MAX_PS_CONST_B = 16;

// Per-draw POD snapshot (COW per state cluster). Compact: ~10 KB
// (transforms/texture_stage_states parked on calling-thread until FFP).
// ~200 clusters/frame cuts bandwidth from 5 MB to 2 MB.
struct D9EncodingState {
  // Render state. D3DRS_* enum runs up to 209; storage sized to 256
  // to match the calling-thread m_renderStates shape (DXVK matches).
  DWORD render_states[256] = {};

  // Per-stage sampler state, indexed [stage][D3DSAMP_*]. wined3d's
  // shape (combined PS + VS samplers).
  DWORD sampler_states[D9ES_MAX_TEXTURE_UNITS][D3DSAMP_DMAPOFFSET + 1] = {};

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

  // Viewport / scissor: stored in D3D9 shape; Resolve runs the
  // wmt_*_from_d3d9 helpers.
  D3DVIEWPORT9 viewport = {};
  RECT scissor_rect = {};
};

} // namespace dxmt
