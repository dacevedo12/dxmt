// BeginScene / EndScene spec-gate smoke. The existing
// smoke_begin_end_scene.cpp is legacy printf-era; this one nails the
// per-MSDN state-machine contract with deterministic asserts so
// regressions get caught at TAP-fail time.
//
// Contract (MSDN + dxmt d3d9_device.cpp:4610):
//   * BeginScene on already-in-scene device       → D3DERR_INVALIDCALL.
//   * EndScene on not-in-scene device              → D3DERR_INVALIDCALL.
//   * Begin → End round-trip                        → D3D_OK.
//   * Begin → End → Begin again                     → D3D_OK
//     (scenes can re-open).
//   * EndScene drains any queued work — apps can rely on draws being
//     submitted before the next Present.

#include "../dx9_smoke.h"

void
test_begin_end_scene_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- EndScene without a matching Begin → INVALIDCALL. ----
  check_hr_eq(dev->EndScene(), D3DERR_INVALIDCALL);

  // ---- Normal Begin / End round-trip. ----
  check_hr(dev->BeginScene());
  check_hr(dev->EndScene());

  // ---- Double-Begin → INVALIDCALL. ----
  check_hr(dev->BeginScene());
  check_hr_eq(dev->BeginScene(), D3DERR_INVALIDCALL);
  // After the failed Begin, the scene state should still be open —
  // EndScene must succeed.
  check_hr(dev->EndScene());

  // ---- Re-open: Begin → End → Begin → End. ----
  check_hr(dev->BeginScene());
  check_hr(dev->EndScene());
  check_hr(dev->BeginScene());
  check_hr(dev->EndScene());

  // ---- Double-End → INVALIDCALL. ----
  check_hr_eq(dev->EndScene(), D3DERR_INVALIDCALL);

  check_zero_losable_count(dev);
}
