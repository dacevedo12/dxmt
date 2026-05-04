// ColorFill spec-gate smoke. The existing smoke_colorfill.cpp covers
// the happy paths with a pixel-readback; this one nails down the
// rejection contract (no current smoke asserts INVALIDCALL on the
// pool / aspect / rect-shape gates).
//
// Contract (DXVK d3d9_device.cpp:1518-1538, wined3d device.c equiv):
//   * NULL pSurface                              → D3DERR_INVALIDCALL.
//   * Surface from a different device            → D3DERR_INVALIDCALL.
//   * Pool != DEFAULT (SYSTEMMEM, MANAGED)       → D3DERR_INVALIDCALL.
//   * Depth-stencil format                       → D3DERR_INVALIDCALL
//     (DXVK's broader aspect-mask check; dxmt's IsDepthStencilFormat
//     is sufficient because the DEFAULT-pool surface paths never
//     accept a non-DS non-color format).
//   * Out-of-bounds / inverted / negative rect   → D3DERR_INVALIDCALL.
//   * NULL pRect                                 → D3D_OK (whole surface).
//   * Full-extent explicit pRect                 → D3D_OK (full-surface
//                                                  fast path).
//   * Sub-rect                                   → D3D_OK (draw path).

#include "../dx9_smoke.h"

void
test_colorfill_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- NULL pSurface ----
  check_hr_eq(dev->ColorFill(NULL, NULL, 0xFF000000u), D3DERR_INVALIDCALL);

  // ---- SYSTEMMEM (non-DEFAULT) surface ----
  IDirect3DSurface9 *sys = NULL;
  check_hr(dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL));
  if (sys) {
    check_hr_eq(dev->ColorFill(sys, NULL, 0xFFAABBCCu), D3DERR_INVALIDCALL);
    sys->Release();
  }

  // ---- Depth-stencil format ----
  IDirect3DSurface9 *ds = NULL;
  if (SUCCEEDED(dev->CreateDepthStencilSurface(64, 48, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, NULL)) && ds) {
    check_hr_eq(dev->ColorFill(ds, NULL, 0xFF000000u), D3DERR_INVALIDCALL);
    ds->Release();
  }

  // ---- DEFAULT-pool RT for the rect probes + happy paths. ----
  IDirect3DSurface9 *rt = NULL;
  check_hr(dev->CreateRenderTarget(64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL));
  if (!rt)
    return;

  // ---- OOB rect ----
  RECT oob = {0, 0, 200, 200};
  check_hr_eq(dev->ColorFill(rt, &oob, 0xFF112233u), D3DERR_INVALIDCALL);

  // ---- Inverted rect ----
  RECT inv = {32, 32, 32, 32};
  check_hr_eq(dev->ColorFill(rt, &inv, 0xFF112233u), D3DERR_INVALIDCALL);

  // ---- Negative coord ----
  RECT neg = {-1, 0, 16, 16};
  check_hr_eq(dev->ColorFill(rt, &neg, 0xFF112233u), D3DERR_INVALIDCALL);

  // ---- Happy path: NULL rect (whole surface). ----
  check_hr(dev->ColorFill(rt, NULL, 0xFF112233u));

  // ---- Happy path: full-extent explicit rect (fast path). ----
  RECT full = {0, 0, 64, 48};
  check_hr(dev->ColorFill(rt, &full, 0xFF224466u));

  // ---- Happy path: sub-rect (render-pass-quad path). ----
  RECT sub = {16, 12, 48, 36};
  check_hr(dev->ColorFill(rt, &sub, 0xFFAA5588u));

  rt->Release();

  check_zero_losable_count(dev);
}
