// SetSamplerState / GetSamplerState smoke. Validates the D3D9 fixed
// defaults at CreateDevice (apps query them pre-Set), the round-trip
// through Set/Get on PS and VS sampler stages, the silent-no-op
// contract for out-of-range stages (D3DDMAPSAMPLER etc.), and the
// INVALIDCALL on out-of-enum state types.

#include "../dx9_smoke.h"

void
test_set_samplerstate(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // Defaults out of the box. Should match wined3d's
  // init_default_sampler_states (ADDRESSU/V/W=WRAP, MAG/MIN=POINT,
  // MIP=NONE, MAXANISOTROPY=1, others 0). Bulk-asserted via the T0.1
  // dict macro so a future divergence shows up as a TAP fail at the
  // exact state index rather than printf noise.
  check_sampler_state_dict(
      dev, 0, {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP}, {D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP},
      {D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP}, {D3DSAMP_MAGFILTER, D3DTEXF_POINT}, {D3DSAMP_MINFILTER, D3DTEXF_POINT},
      {D3DSAMP_MIPFILTER, D3DTEXF_NONE}, {D3DSAMP_MAXANISOTROPY, 1}, {D3DSAMP_BORDERCOLOR, 0}
  );

  // VS sampler stage default — same set of defaults at the lowest VS
  // sampler (translates to internal slot 16).
  check_sampler_state_dict(
      dev, D3DVERTEXTEXTURESAMPLER0, {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP}, {D3DSAMP_MAGFILTER, D3DTEXF_POINT}
  );

  // Highest VS sampler — locks the ctor loop bound (catches an
  // accidental i<16 instead of i<20). Slot 19.
  check_sampler_state_dict(dev, D3DVERTEXTEXTURESAMPLER3, {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP});

  // Round-trip on PS stage 7.
  check_hr(dev->SetSamplerState(7, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
  check_sampler_state_dict(dev, 7, {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP});

  // Cross-stage isolation: stage 7 changed but stage 0 should still
  // hold its default.
  check_sampler_state_dict(dev, 0, {D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP});

  // Round-trip on VS stage D3DVERTEXTEXTURESAMPLER3 (internal slot 19).
  check_hr(dev->SetSamplerState(D3DVERTEXTEXTURESAMPLER3, D3DSAMP_MAXANISOTROPY, 16));
  check_sampler_state_dict(dev, D3DVERTEXTEXTURESAMPLER3, {D3DSAMP_MAXANISOTROPY, 16});

  // Out-of-range sampler — silent no-op for Set, *value=0 + S_OK for Get.
  // Per wined3d device.c:2934 + DXVK d3d9_device.cpp:2897 (InvalidSampler).
  check_hr(dev->SetSamplerState(D3DDMAPSAMPLER, D3DSAMP_ADDRESSU, 0));
  check_sampler_state_dict(dev, D3DDMAPSAMPLER, {D3DSAMP_ADDRESSU, 0});

  check_hr(dev->SetSamplerState(9999, D3DSAMP_ADDRESSU, 0));
  check_sampler_state_dict(dev, 9999, {D3DSAMP_ADDRESSU, 0});

  // Out-of-enum state type — INVALIDCALL.
  check_hr_eq(dev->SetSamplerState(0, (D3DSAMPLERSTATETYPE)0xff, 0), D3DERR_INVALIDCALL);
  DWORD v = 0xdeadbeef;
  check_hr_eq(dev->GetSamplerState(0, (D3DSAMPLERSTATETYPE)0xff, &v), D3DERR_INVALIDCALL);

  // GetSamplerState NULL pointer — INVALIDCALL.
  check_hr_eq(dev->GetSamplerState(0, D3DSAMP_ADDRESSU, NULL), D3DERR_INVALIDCALL);

  check_zero_losable_count(dev);
}
