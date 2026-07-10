/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "d3d9.h"

#include <cstdint>

// The per-category + per-render-state change-tracking mask a state block owns,
// factored out of d3d9_device.hpp so the host unit tier can pin the
// D3DSBT_PIXELSTATE / D3DSBT_VERTEXSTATE / D3DSBT_ALL membership tables without
// the device / Metal include surface (the d3d9_viewport.hpp / d3d9_gamma.hpp
// shape). A drifted membership list is exactly the bug class markPixelStateSubset
// / markVertexStateSubset guard against. References: wined3d (PRIMARY)
// dlls/wined3d/stateblock.c savedstates + pixel_states_* / vertex_states_*
// tables; DXVK (secondary) src/d3d9/d3d9_stateblock.cpp.

namespace dxmt {

// Mirrors of the device register-file / binding-slot dimensions
// (d3d9_device.hpp D3D9_MAX_VS_CONST_F / _PS_CONST_F / _TEXTURE_UNITS), kept
// local so this header stays free of the device/Metal include surface, exactly
// as d3d9_ia_element.hpp mirrors D3D9_MAX_VERTEX_STREAMS. d3d9_device.cpp
// static_asserts the mirrors equal the originals so the two cannot drift.
inline constexpr uint16_t kSbcMaxVsConstF = 256;
inline constexpr uint16_t kSbcMaxPsConstF = 224;
inline constexpr uint32_t kSbcMaxTextureUnits = 20;

// Per-category + per-render-state mask of states an app touched between
// BeginStateBlock and EndStateBlock, OR every state for D3DSBT_ALL
// blocks. Each MTLD3D9StateBlock owns one; the device's recording arms
// mark bits on the in-progress block as Set* calls land so Apply
// restores only the touched states (wined3d's wined3d_saved_states
// shape, dlls/wined3d/stateblock.c). The render-state slot is
// per-element because layering bugs hinge on stomping
// ALPHABLENDENABLE / ZENABLE alongside the one render state the app
// meant to flip.
struct D3D9StateBlockChanges {
  bool render_states[256] = {};
  bool sampler_states = false;
  bool texture_stage_states = false;
  // Predefined PIXELSTATE / VERTEXSTATE subsets restore only the per-stage
  // texture-stage / sampler ELEMENTS wined3d lists (pixel_states_texture /
  // _sampler, vertex_states_texture / _sampler in stateblock.c). 0 restores the
  // whole array (D3DSBT_ALL and Begin/End-recorded blocks stay coarse); a
  // non-zero mask restores only the (1u << D3DTSS_* / D3DSAMP_*) elements set.
  // Paired with the texture_stage_states / sampler_states bools.
  uint32_t tss_element_mask = 0;
  uint32_t samp_element_mask = 0;
  bool transforms = false;
  bool clip_planes = false;
  bool viewport = false;
  bool scissor = false;
  bool fvf = false;
  // Per-stream masks, split like wined3d's changed.streamSource vs
  // changed.streamFreq (stateblock.c): a recorded SetStreamSource marks
  // stream_source (buffer binding + offset + stride), a recorded
  // SetStreamSourceFreq marks stream_freq (freq + divider). Keeping them
  // separate means a freq-only recorded block does not revert the buffer
  // binding on Apply, and a source-only block does not revert the frequency.
  uint16_t stream_source = 0;
  uint16_t stream_freq = 0;
  // wined3d store_stream_offset: a CreateStateBlock block captures the stream
  // offset at create time, but a SUBSEQUENT Capture updates only the buffer
  // and stride, freezing the offset. CreateStateBlock clears this after the
  // initial capture; Begin/End-recorded blocks leave it set so their captures
  // keep updating the offset. wined3d stateblock.c.
  bool store_stream_offset = true;
  bool material = false;
  // I + B register files only; the F file is range-tracked below so a
  // recorded handful of registers doesn't restore the whole 256-slot
  // file on Apply.
  bool vs_constants = false;
  bool ps_constants = false;
  // Recorded F-register range, half-open [lo, hi); empty when
  // hi <= lo. Capture / Apply touch only this span.
  uint16_t vs_const_f_lo = 0;
  uint16_t vs_const_f_hi = 0;
  uint16_t ps_const_f_lo = 0;
  uint16_t ps_const_f_hi = 0;
  // Per-slot bit, PS samplers 0..15 + VS samplers 16..19. A SetTexture
  // wholly overwrites its snapshot slot, so per-slot bits also keep
  // recorded blocks from pinning every texture bound at Begin time.
  uint32_t textures = 0;
  bool index_buffer = false;
  bool vertex_declaration = false;
  bool vertex_shader = false;
  bool pixel_shader = false;
  bool lights = false;

  void
  reset() {
    *this = D3D9StateBlockChanges{};
  }

  void
  markAll() {
    for (auto &b : render_states)
      b = true;
    sampler_states = texture_stage_states = transforms = clip_planes = true;
    viewport = scissor = fvf = material = true;
    stream_source = stream_freq = 0xFFFF;
    vs_constants = ps_constants = true;
    vs_const_f_lo = 0;
    vs_const_f_hi = kSbcMaxVsConstF;
    ps_const_f_lo = 0;
    ps_const_f_hi = kSbcMaxPsConstF;
    textures = (1u << kSbcMaxTextureUnits) - 1;
    index_buffer = vertex_declaration = vertex_shader = pixel_shader = lights = true;
  }

  // D3DSBT_PIXELSTATE subset: render states from wined3d's
  // pixel_states_render[] (dlls/wined3d/stateblock.c), plus the
  // pixel-pipeline categories (sampler / texture-stage / pixel shader /
  // PS constants; bound textures belong to D3DSBT_ALL, not here). The
  // texture-stage and sampler restore is narrowed to the exact
  // pixel_states_texture[] / pixel_states_sampler[] elements via the
  // tss_element_mask / samp_element_mask populated below.
  void
  markPixelStateSubset() {
    static constexpr D3DRENDERSTATETYPE pixel_states_render[] = {
        D3DRS_ALPHABLENDENABLE,
        D3DRS_ALPHAFUNC,
        D3DRS_ALPHAREF,
        D3DRS_ALPHATESTENABLE,
        D3DRS_ANTIALIASEDLINEENABLE,
        D3DRS_BLENDFACTOR,
        D3DRS_BLENDOP,
        D3DRS_BLENDOPALPHA,
        D3DRS_CCW_STENCILFAIL,
        D3DRS_CCW_STENCILPASS,
        D3DRS_CCW_STENCILZFAIL,
        D3DRS_COLORWRITEENABLE,
        D3DRS_COLORWRITEENABLE1,
        D3DRS_COLORWRITEENABLE2,
        D3DRS_COLORWRITEENABLE3,
        D3DRS_DEPTHBIAS,
        D3DRS_DESTBLEND,
        D3DRS_DESTBLENDALPHA,
        D3DRS_DITHERENABLE,
        D3DRS_FILLMODE,
        D3DRS_FOGDENSITY,
        D3DRS_FOGEND,
        D3DRS_FOGSTART,
        D3DRS_LASTPIXEL,
        D3DRS_SCISSORTESTENABLE,
        D3DRS_SEPARATEALPHABLENDENABLE,
        D3DRS_SHADEMODE,
        D3DRS_SLOPESCALEDEPTHBIAS,
        D3DRS_SRCBLEND,
        D3DRS_SRCBLENDALPHA,
        D3DRS_SRGBWRITEENABLE,
        D3DRS_STENCILENABLE,
        D3DRS_STENCILFAIL,
        D3DRS_STENCILFUNC,
        D3DRS_STENCILMASK,
        D3DRS_STENCILPASS,
        D3DRS_STENCILREF,
        D3DRS_STENCILWRITEMASK,
        D3DRS_STENCILZFAIL,
        D3DRS_TEXTUREFACTOR,
        D3DRS_TWOSIDEDSTENCILMODE,
        D3DRS_WRAP0,
        D3DRS_WRAP1,
        D3DRS_WRAP10,
        D3DRS_WRAP11,
        D3DRS_WRAP12,
        D3DRS_WRAP13,
        D3DRS_WRAP14,
        D3DRS_WRAP15,
        D3DRS_WRAP2,
        D3DRS_WRAP3,
        D3DRS_WRAP4,
        D3DRS_WRAP5,
        D3DRS_WRAP6,
        D3DRS_WRAP7,
        D3DRS_WRAP8,
        D3DRS_WRAP9,
        D3DRS_ZENABLE,
        D3DRS_ZFUNC,
        D3DRS_ZWRITEENABLE,
    };
    for (auto rs : pixel_states_render)
      render_states[rs] = true;
    // wined3d pixel_states_texture[] / pixel_states_sampler[] (stateblock.c):
    // the per-stage TSS / sampler elements PIXELSTATE restores. The sampler
    // list notably omits D3DSAMP_DMAPOFFSET (a vertex-only element).
    static constexpr D3DTEXTURESTAGESTATETYPE pixel_states_texture[] = {
        D3DTSS_ALPHAARG0,
        D3DTSS_ALPHAARG1,
        D3DTSS_ALPHAARG2,
        D3DTSS_ALPHAOP,
        D3DTSS_BUMPENVLOFFSET,
        D3DTSS_BUMPENVLSCALE,
        D3DTSS_BUMPENVMAT00,
        D3DTSS_BUMPENVMAT01,
        D3DTSS_BUMPENVMAT10,
        D3DTSS_BUMPENVMAT11,
        D3DTSS_COLORARG0,
        D3DTSS_COLORARG1,
        D3DTSS_COLORARG2,
        D3DTSS_COLOROP,
        D3DTSS_RESULTARG,
        D3DTSS_TEXCOORDINDEX,
        D3DTSS_TEXTURETRANSFORMFLAGS,
    };
    static constexpr D3DSAMPLERSTATETYPE pixel_states_sampler[] = {
        D3DSAMP_ADDRESSU,    D3DSAMP_ADDRESSV,      D3DSAMP_ADDRESSW,    D3DSAMP_BORDERCOLOR,
        D3DSAMP_MAGFILTER,   D3DSAMP_MINFILTER,     D3DSAMP_MIPFILTER,   D3DSAMP_MIPMAPLODBIAS,
        D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE, D3DSAMP_ELEMENTINDEX,
    };
    for (auto e : pixel_states_texture)
      tss_element_mask |= 1u << e;
    for (auto e : pixel_states_sampler)
      samp_element_mask |= 1u << e;
    texture_stage_states = true;
    sampler_states = true;
    pixel_shader = true;
    ps_constants = true;
    ps_const_f_lo = 0;
    ps_const_f_hi = kSbcMaxPsConstF;
    // Bound textures are a D3DSBT_ALL category, not part of PIXELSTATE:
    // wined3d stateblock_savedstates_set_pixel does not mark them, so an
    // applied PIXELSTATE block must leave the bound textures untouched.
  }

  // D3DSBT_VERTEXSTATE subset: render states from wined3d's
  // vertex_states_render[] (dlls/wined3d/stateblock.c), plus the
  // vertex-pipeline categories. The texture-stage and sampler restore is
  // narrowed to the exact vertex_states_texture[] (TEXCOORDINDEX,
  // TEXTURETRANSFORMFLAGS) / vertex_states_sampler[] (DMAPOFFSET) elements
  // via tss_element_mask / samp_element_mask populated below.
  void
  markVertexStateSubset() {
    static constexpr D3DRENDERSTATETYPE vertex_states_render[] = {
        D3DRS_ADAPTIVETESS_W,
        D3DRS_ADAPTIVETESS_X,
        D3DRS_ADAPTIVETESS_Y,
        D3DRS_ADAPTIVETESS_Z,
        D3DRS_AMBIENT,
        D3DRS_AMBIENTMATERIALSOURCE,
        D3DRS_CLIPPING,
        D3DRS_CLIPPLANEENABLE,
        D3DRS_COLORVERTEX,
        D3DRS_CULLMODE,
        D3DRS_DIFFUSEMATERIALSOURCE,
        D3DRS_EMISSIVEMATERIALSOURCE,
        D3DRS_ENABLEADAPTIVETESSELLATION,
        D3DRS_FOGCOLOR,
        D3DRS_FOGDENSITY,
        D3DRS_FOGENABLE,
        D3DRS_FOGEND,
        D3DRS_FOGSTART,
        D3DRS_FOGTABLEMODE,
        D3DRS_FOGVERTEXMODE,
        D3DRS_INDEXEDVERTEXBLENDENABLE,
        D3DRS_LIGHTING,
        D3DRS_LOCALVIEWER,
        D3DRS_MAXTESSELLATIONLEVEL,
        D3DRS_MINTESSELLATIONLEVEL,
        D3DRS_MULTISAMPLEANTIALIAS,
        D3DRS_MULTISAMPLEMASK,
        D3DRS_NORMALDEGREE,
        D3DRS_NORMALIZENORMALS,
        D3DRS_PATCHEDGESTYLE,
        D3DRS_POINTSCALE_A,
        D3DRS_POINTSCALE_B,
        D3DRS_POINTSCALE_C,
        D3DRS_POINTSCALEENABLE,
        D3DRS_POINTSIZE,
        D3DRS_POINTSIZE_MAX,
        D3DRS_POINTSIZE_MIN,
        D3DRS_POINTSPRITEENABLE,
        D3DRS_POSITIONDEGREE,
        D3DRS_RANGEFOGENABLE,
        D3DRS_SHADEMODE,
        D3DRS_SPECULARENABLE,
        D3DRS_SPECULARMATERIALSOURCE,
        D3DRS_TWEENFACTOR,
        D3DRS_VERTEXBLEND,
    };
    for (auto rs : vertex_states_render)
      render_states[rs] = true;
    // wined3d vertex_states_texture[] / vertex_states_sampler[] (stateblock.c):
    // VERTEXSTATE restores only these per-stage elements, and its sampler list
    // is exactly D3DSAMP_DMAPOFFSET (the element PIXELSTATE omits).
    static constexpr D3DTEXTURESTAGESTATETYPE vertex_states_texture[] = {
        D3DTSS_TEXCOORDINDEX,
        D3DTSS_TEXTURETRANSFORMFLAGS,
    };
    for (auto e : vertex_states_texture)
      tss_element_mask |= 1u << e;
    samp_element_mask |= 1u << D3DSAMP_DMAPOFFSET;
    texture_stage_states = true;
    sampler_states = true;
    vertex_shader = true;
    vs_constants = true;
    vs_const_f_lo = 0;
    vs_const_f_hi = kSbcMaxVsConstF;
    vertex_declaration = true;
    lights = true;
    // The VERTEXSTATE subset is the vertex pipeline only. The stream / index
    // / material / transform / clip-plane / viewport / scissor / fvf
    // categories belong to D3DSBT_ALL, not here: wined3d
    // stateblock_savedstates_set_vertex marks none of them, so an applied
    // VERTEXSTATE block must leave the bound vertex buffers, index buffer,
    // transforms etc. untouched.
  }
};

} // namespace dxmt
