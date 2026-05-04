// SetGammaRamp / GetGammaRamp spec-gate smoke. No prior smoke covers
// this surface; calibration apps and titles with custom gamma curves
// (most NFS / Source-era engines) need the round-trip contract.
//
// Contract (MSDN + wined3d d3d9 device.c::d3d9_device_SetGammaRamp +
// dxmt d3d9_swapchain.cpp:752):
//
//   * Both Set and Get return void — no HRESULT to check; the
//     observable contract is round-trip + safe handling of bad input.
//   * Get before any Set returns an identity ramp (entry i = i*257).
//   * Set with a custom ramp + Get must round-trip bit-exact across
//     all 256 entries × 3 channels — apps re-read the ramp they wrote
//     and compare WORD-for-WORD.
//   * Set / Get on iSwapChain != 0 is a silent no-op (additional
//     swapchains aren't implemented); Get on a bad swapchain index
//     still synthesises identity into the caller's buffer rather
//     than leaving it uninitialised (some calibration tools probe
//     unconditionally).
//   * Set with NULL pRamp must not crash and must not mutate the
//     stored ramp (Get after must still return whatever was last
//     committed).
//   * Get with NULL pRamp must not crash.

#include "../dx9_smoke.h"

#include <string.h>

static bool
ramp_is_identity(const D3DGAMMARAMP *r) {
  for (uint32_t i = 0; i < 256; ++i) {
    WORD v = static_cast<WORD>(i * 257);
    if (r->red[i] != v || r->green[i] != v || r->blue[i] != v)
      return false;
  }
  return true;
}

static bool
ramp_equal(const D3DGAMMARAMP *a, const D3DGAMMARAMP *b) {
  return memcmp(a->red, b->red, sizeof(a->red)) == 0 && memcmp(a->green, b->green, sizeof(a->green)) == 0 &&
         memcmp(a->blue, b->blue, sizeof(a->blue)) == 0;
}

void
test_gamma_ramp_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- Get before any Set returns identity ramp. ----
  D3DGAMMARAMP got = {};
  dev->GetGammaRamp(0, &got);
  check_true(ramp_is_identity(&got));

  // ---- Set + Get round-trips bit-exact. ----
  D3DGAMMARAMP custom = {};
  for (uint32_t i = 0; i < 256; ++i) {
    // Mildly non-monotonic across channels to catch any per-channel
    // mix-ups in the storage / readback path.
    custom.red[i] = static_cast<WORD>((i * 257) ^ 0x00FFu);
    custom.green[i] = static_cast<WORD>(0xFFFF - i * 257);
    custom.blue[i] = static_cast<WORD>((i * 257) & 0xFE00u);
  }
  dev->SetGammaRamp(0, 0, &custom);
  D3DGAMMARAMP rt = {};
  dev->GetGammaRamp(0, &rt);
  check_true(ramp_equal(&custom, &rt));

  // ---- Set with NULL is a silent no-op; last-set ramp persists. ----
  dev->SetGammaRamp(0, 0, NULL);
  D3DGAMMARAMP after_null = {};
  dev->GetGammaRamp(0, &after_null);
  check_true(ramp_equal(&custom, &after_null));

  // ---- Set with D3DSGR_CALIBRATE flag still round-trips. The flag
  // is a colour-management hint with no Metal-side knob (wined3d
  // ignores it too); the ramp itself still has to land in storage.
  D3DGAMMARAMP cal = {};
  for (uint32_t i = 0; i < 256; ++i) {
    cal.red[i] = static_cast<WORD>(i * 200);
    cal.green[i] = static_cast<WORD>(i * 200);
    cal.blue[i] = static_cast<WORD>(i * 200);
  }
  dev->SetGammaRamp(0, D3DSGR_CALIBRATE, &cal);
  D3DGAMMARAMP rt_cal = {};
  dev->GetGammaRamp(0, &rt_cal);
  check_true(ramp_equal(&cal, &rt_cal));

  // ---- Bad iSwapChain on Get fills identity rather than leaving the
  // buffer uninitialised. ----
  D3DGAMMARAMP oor = {};
  memset(&oor, 0xCD, sizeof(oor)); // poison
  dev->GetGammaRamp(99, &oor);
  check_true(ramp_is_identity(&oor));

  // ---- Bad iSwapChain on Set is a silent no-op (the implicit chain
  // keeps the previously-stored ramp). ----
  D3DGAMMARAMP zeros = {};
  dev->SetGammaRamp(99, 0, &zeros);
  D3DGAMMARAMP after_oor = {};
  dev->GetGammaRamp(0, &after_oor);
  check_true(ramp_equal(&cal, &after_oor));

  // ---- Get with NULL must not crash. (Nothing to assert beyond the
  // call returning; in a crash the test binary aborts.) ----
  dev->GetGammaRamp(0, NULL);
  check_true(true);

  check_zero_losable_count(dev);
}
