#pragma once

#include "d3d9.h"

namespace dxmt {

// Initialize 256-entry D3DRS render-state array with D3D9 defaults.
// enableAutoDepthStencil controls D3DRS_ZENABLE default (wined3d ref).
void init_default_render_states(DWORD (&rs)[256], bool enableAutoDepthStencil);

// Seed `count` sampler stages with the D3D9 power-on sampler-state
// defaults (reference: wined3d stateblock.c init_default_sampler_states).
// Checked host-native in tests/dx9/unit/test_sampler_state_defaults.cpp.
void init_default_sampler_states(DWORD (*samp)[D3DSAMP_DMAPOFFSET + 1], unsigned int count);

// Zero, then seed, `stages` texture-stage-state blocks with the D3D9
// power-on defaults (reference: Wine d3d9 test_texture_stage_states).
// Checked host-native in tests/dx9/unit/test_texture_stage_defaults.cpp.
void init_default_texture_stage_states(DWORD (*tss)[D3DTSS_CONSTANT + 1], unsigned int stages);

} // namespace dxmt
