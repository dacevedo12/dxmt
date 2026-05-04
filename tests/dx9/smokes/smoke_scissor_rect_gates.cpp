// SetScissorRect / GetScissorRect spec-gate smoke. UI engines and
// Source-era games drive scissor each frame; the round-trip + NULL
// gates need deterministic coverage.
//
// Contract (DXVK d3d9_device.cpp:2989 + wined3d d3d9 device.c:3026):
//   * NULL pRect on Set / Get  → D3DERR_INVALIDCALL.
//   * Initial scissor (no Set) reflects the bound RT extents — apps
//     read it before any explicit Set to size their UI clip regions.
//     The exact initial value depends on the implicit RT bind in
//     CreateDevice; assertion uses the backbuffer dimensions the
//     fixture configures (320x240 by default).
//   * Set / Get round-trip bit-exact across all four RECT fields.
//   * Successive Sets overwrite (no merge / no clamp to viewport).
//     wined3d + DXVK both pass the RECT through unmodified.

#include "../dx9_smoke.h"

void
test_scissor_rect_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- NULL pRect ----
  check_hr_eq(dev->SetScissorRect(NULL), D3DERR_INVALIDCALL);
  check_hr_eq(dev->GetScissorRect(NULL), D3DERR_INVALIDCALL);

  // ---- Initial scissor reflects the backbuffer extents (320x240). ----
  RECT initial = {};
  check_hr(dev->GetScissorRect(&initial));
  check_eq_u32(static_cast<uint32_t>(initial.left), 0);
  check_eq_u32(static_cast<uint32_t>(initial.top), 0);
  check_eq_u32(static_cast<uint32_t>(initial.right), 320);
  check_eq_u32(static_cast<uint32_t>(initial.bottom), 240);

  // ---- Round-trip on a sub-rect. ----
  RECT r1 = {10, 20, 100, 200};
  check_hr(dev->SetScissorRect(&r1));
  RECT rt1 = {};
  check_hr(dev->GetScissorRect(&rt1));
  check_eq_u32(static_cast<uint32_t>(rt1.left), 10);
  check_eq_u32(static_cast<uint32_t>(rt1.top), 20);
  check_eq_u32(static_cast<uint32_t>(rt1.right), 100);
  check_eq_u32(static_cast<uint32_t>(rt1.bottom), 200);

  // ---- Successive Set overwrites — no merge / no clamp. ----
  RECT r2 = {0, 0, 64, 48};
  check_hr(dev->SetScissorRect(&r2));
  RECT rt2 = {};
  check_hr(dev->GetScissorRect(&rt2));
  check_eq_u32(static_cast<uint32_t>(rt2.right), 64);
  check_eq_u32(static_cast<uint32_t>(rt2.bottom), 48);

  // ---- Rect outside the backbuffer extents passes through unclamped
  // — wined3d + DXVK both store the user-supplied RECT verbatim and
  // let the rasterizer clip at draw time. dxmt mirrors this; apps
  // that pre-compute scissor for an offscreen RT before binding it
  // would otherwise see their values stomped here.
  RECT r3 = {-50, -25, 1024, 768};
  check_hr(dev->SetScissorRect(&r3));
  RECT rt3 = {};
  check_hr(dev->GetScissorRect(&rt3));
  check_true(rt3.left == -50);
  check_true(rt3.top == -25);
  check_true(rt3.right == 1024);
  check_true(rt3.bottom == 768);

  check_zero_losable_count(dev);
}
