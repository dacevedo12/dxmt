// SetClipPlane / GetClipPlane spec-gate smoke. No prior smoke covered
// this surface; user-clip-plane apps (CAD viewports, water-plane
// reflections) need the index-saturation contract to round-trip.
//
// Contract (DXVK d3d9_device.cpp:2293-2321, wined3d d3d9 device.c
// :2569 + :2584):
//   * NULL pPlane                 → D3DERR_INVALIDCALL.
//   * Index >= 8                  → silently saturated to 7 (NOT
//                                   D3DERR_INVALIDCALL). Get likewise
//                                   reads from slot 7.
//   * Set / Get round-trip on
//     every slot 0..7              → S_OK, bit-exact float fidelity.
//   * Cross-slot independence     → writes to slot N don't disturb
//                                   slot M.

#include "../dx9_smoke.h"

void
test_clip_plane_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- NULL pPlane ----
  check_hr_eq(dev->SetClipPlane(0, NULL), D3DERR_INVALIDCALL);
  check_hr_eq(dev->GetClipPlane(0, NULL), D3DERR_INVALIDCALL);

  // ---- Default-state Get on every slot returns the zero plane. ----
  for (DWORD i = 0; i < 8; ++i) {
    float p[4] = {1.0f, 2.0f, 3.0f, 4.0f}; // sentinel
    check_hr(dev->GetClipPlane(i, p));
    check_float_eq_eps(p[0], 0.0f, 0.0f);
    check_float_eq_eps(p[1], 0.0f, 0.0f);
    check_float_eq_eps(p[2], 0.0f, 0.0f);
    check_float_eq_eps(p[3], 0.0f, 0.0f);
  }

  // ---- Round-trip on slot 0 and slot 7 (bounds). ----
  const float plane0[4] = {1.0f, 0.0f, 0.0f, -0.5f};
  check_hr(dev->SetClipPlane(0, plane0));
  float r0[4] = {};
  check_hr(dev->GetClipPlane(0, r0));
  check_float_eq_eps(r0[0], 1.0f, 0.0f);
  check_float_eq_eps(r0[3], -0.5f, 0.0f);

  const float plane7[4] = {0.0f, 1.0f, 0.0f, 100.0f};
  check_hr(dev->SetClipPlane(7, plane7));
  float r7[4] = {};
  check_hr(dev->GetClipPlane(7, r7));
  check_float_eq_eps(r7[1], 1.0f, 0.0f);
  check_float_eq_eps(r7[3], 100.0f, 0.0f);

  // ---- Out-of-range Index saturates to 7, not INVALIDCALL. ----
  // wined3d + DXVK both do this — apps that pass an unintentionally-
  // large index (loose-typed enum cast) get the corner-case write
  // rather than a hard failure.
  const float plane_oor[4] = {7.0f, 7.0f, 7.0f, 7.0f};
  check_hr(dev->SetClipPlane(99, plane_oor));
  float r_after[4] = {};
  check_hr(dev->GetClipPlane(99, r_after));
  // The saturated write landed in slot 7 — also observable through
  // a Get on the literal index 7.
  check_float_eq_eps(r_after[0], 7.0f, 0.0f);
  check_float_eq_eps(r_after[3], 7.0f, 0.0f);

  float r7_after[4] = {};
  check_hr(dev->GetClipPlane(7, r7_after));
  check_float_eq_eps(r7_after[0], 7.0f, 0.0f);

  // ---- Cross-slot independence. ----
  // Slot 0 still holds the plane we wrote earlier — slot-7 writes
  // mustn't bleed into it.
  float r0_after[4] = {};
  check_hr(dev->GetClipPlane(0, r0_after));
  check_float_eq_eps(r0_after[0], 1.0f, 0.0f);
  check_float_eq_eps(r0_after[3], -0.5f, 0.0f);

  // ---- All four float lanes carry through. ----
  const float lanes[4] = {-1.25f, 2.5f, -3.75f, 4.875f};
  check_hr(dev->SetClipPlane(3, lanes));
  float rl[4] = {};
  check_hr(dev->GetClipPlane(3, rl));
  check_float_eq_eps(rl[0], lanes[0], 0.0f);
  check_float_eq_eps(rl[1], lanes[1], 0.0f);
  check_float_eq_eps(rl[2], lanes[2], 0.0f);
  check_float_eq_eps(rl[3], lanes[3], 0.0f);

  check_zero_losable_count(dev);
}
