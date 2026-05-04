// Clear spec-gate smoke (audit B3 + DXVK / wined3d cross-check).
//
// The execution contract dxmt enforces (matching DXVK d3d9_device.cpp
// :1921-1990 and wined3d device.c:3755-3779):
//
//   * No DS bound + (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)
//                                            → D3DERR_INVALIDCALL.
//   * Count == 0 + pRects != NULL            → D3D_OK (documented no-op).
//   * Flags == 0                             → D3D_OK (no-op).
//   * D3DCLEAR_STENCIL against a depth-only DS (D24X8, DF24, DF16)
//                                            → silent drop of the
//     stencil bit, call returns D3D_OK. MSDN: stencil flag is ignored
//     when the bound DS has no stencil aspect; DXVK does the same; this
//     IS the "stencil-against-non-stencil rejection" the polish-plan
//     audit B3 calls out — the rejection is of the bit, not the call.
//
// The lazy-clear execution path (m_pendingClear coalesced into the
// next render pass's loadAction) doesn't change any of these contract
// returns — they're synchronous validation responses.

#include "../dx9_smoke.h"

void
test_clear_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // Dx9Fixture's CreateDevice uses default present params with no
  // EnableAutoDepthStencil — the device has no DS bound out of the
  // box, so the Z / STENCIL probes need an explicit D24S8 / D24X8 we
  // create here. Bind D24S8 up front so the triple-clear probe below
  // has all three aspects available.
  IDirect3DSurface9 *ds_d24s8 = NULL;
  check_hr(dev->CreateDepthStencilSurface(320, 240, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds_d24s8, NULL));
  check_hr(dev->SetDepthStencilSurface(ds_d24s8));

  // ---- No-op / empty-flag returns ----
  // Documented no-op — D3D_OK, not INVALIDCALL.
  D3DRECT dummy = {0, 0, 16, 16};
  check_hr(dev->Clear(0, &dummy, 0, 0, 0.0f, 0));

  // No flags set → no-op D3D_OK.
  check_hr(dev->Clear(0, NULL, 0, 0, 0.0f, 0));

  // Plain TARGET clear with the fixture's bound RT+DS.
  check_hr(dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF112233, 1.0f, 0));

  // TARGET + ZBUFFER + STENCIL — the D24S8 we just bound has all
  // three aspects so the call must S_OK.
  check_hr(dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFFAABBCC, 0.5f, 0x42));

  // ---- DS-bind-state gates ----
  // Detach the DS, then Z / STENCIL flags must INVALIDCALL.
  check_hr(dev->SetDepthStencilSurface(NULL));
  check_hr_eq(dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0), D3DERR_INVALIDCALL);
  check_hr_eq(dev->Clear(0, NULL, D3DCLEAR_STENCIL, 0, 0.0f, 0), D3DERR_INVALIDCALL);
  check_hr_eq(dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0), D3DERR_INVALIDCALL);

  // ---- Audit B3 — D3DCLEAR_STENCIL silent-drop on depth-only DS ----
  // Create a D24X8 (depth-only, no stencil aspect) DS and bind it.
  // Clearing with D3DCLEAR_STENCIL must not return INVALIDCALL — the
  // stencil bit is silently dropped per MSDN + DXVK. Clearing with
  // D3DCLEAR_ZBUFFER still works (depth aspect is present).
  IDirect3DSurface9 *ds_d24x8 = NULL;
  HRESULT cds = dev->CreateDepthStencilSurface(64, 48, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds_d24x8, NULL);
  if (SUCCEEDED(cds) && ds_d24x8) {
    check_hr(dev->SetDepthStencilSurface(ds_d24x8));

    // Stencil-only — silently dropped, D3D_OK.
    check_hr(dev->Clear(0, NULL, D3DCLEAR_STENCIL, 0, 0.0f, 0x7F));

    // Z + Stencil combined — Z lands, stencil drops, D3D_OK.
    check_hr(dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.5f, 0x12));

    // Z alone — happy path on depth-only.
    check_hr(dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0));

    ds_d24x8->Release();
  }

  // Unbind any DS held by the device before the losable-count check —
  // SetDepthStencilSurface AddRefPrivate's the bound surface, so a
  // surface left bound at this point would still pin its losable slot
  // and the count would read 1 even though the public ref hit zero
  // when the smoke Release()'d above.
  check_hr(dev->SetDepthStencilSurface(NULL));
  if (ds_d24s8)
    ds_d24s8->Release();

  check_zero_losable_count(dev);
}
