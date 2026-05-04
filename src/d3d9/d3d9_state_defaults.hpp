#pragma once

#include "d3d9.h"

namespace dxmt {

// Seed the device's 256-entry D3DRS_* render-state array with the
// D3D9 power-on defaults. Pulled out of MTLD3D9Device::initDefaultRenderStates
// as a free function so the ~90-value default table can be checked
// host-native against the reference (wined3d stateblock.c
// render_state_default_data_init) without standing up a device — see
// tests/dx9/unit/test_render_state_defaults.cpp.
//
// `enableAutoDepthStencil` only affects D3DRS_ZENABLE, whose default is
// D3DZB_TRUE when the device was created with an implicit depth buffer
// and D3DZB_FALSE otherwise.
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
