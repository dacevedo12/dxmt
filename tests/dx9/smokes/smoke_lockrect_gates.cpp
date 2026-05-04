// LockRect / UnlockRect spec-gate smoke.
//
// Audits B1 (compressed-format alignment) and B2 (DISCARD/NOOVERWRITE
// validation) from the conformance audit. The validation contract from
// MSDN + DXVK (d3d9_device.cpp::LockImage) + wined3d (texture.c
// d3d9_texture_2d_lock_rect):
//
//   * NULL pLockedRect                       → D3DERR_INVALIDCALL.
//   * Already-locked surface                 → D3DERR_INVALIDCALL.
//   * Unlock when not locked                 → D3DERR_INVALIDCALL.
//   * Out-of-bounds rect                     → D3DERR_INVALIDCALL.
//   * Inverted rect (left >= right etc.)     → D3DERR_INVALIDCALL.
//   * DXT/BCn with unaligned (left|top) % 4  → D3DERR_INVALIDCALL.
//   * DISCARD + READONLY combination         → D3DERR_INVALIDCALL.
//
// AddDirtyRect(NULL) marks the whole sub-resource set; we can't read
// back the internal m_dirty_rect from the API, but the call must
// still return S_OK for both NULL and non-NULL inputs.

#include "../dx9_smoke.h"

void
test_lockrect_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- SYSTEMMEM 64x48 X8R8G8B8 surface — exercises rect bounds ----
  IDirect3DSurface9 *sys = NULL;
  check_hr(dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL));
  check_true(sys != NULL);

  if (sys) {
    // NULL pLockedRect → INVALIDCALL.
    check_hr_eq(sys->LockRect(NULL, NULL, 0), D3DERR_INVALIDCALL);

    // Out-of-bounds rect → INVALIDCALL.
    RECT oob = {0, 0, 1000, 1000};
    D3DLOCKED_RECT lr = {};
    check_hr_eq(sys->LockRect(&lr, &oob, 0), D3DERR_INVALIDCALL);

    // Inverted rect (left >= right) → INVALIDCALL.
    RECT inv = {32, 32, 32, 32};
    check_hr_eq(sys->LockRect(&lr, &inv, 0), D3DERR_INVALIDCALL);

    // DISCARD + READONLY → INVALIDCALL (DXVK d9vk LockImage).
    check_hr_eq(sys->LockRect(&lr, NULL, D3DLOCK_DISCARD | D3DLOCK_READONLY), D3DERR_INVALIDCALL);

    // Unlock when not locked → INVALIDCALL.
    check_hr_eq(sys->UnlockRect(), D3DERR_INVALIDCALL);

    // Happy path lock. Negative-tested again below for double-lock.
    check_hr(sys->LockRect(&lr, NULL, 0));
    check_true(lr.pBits != NULL);

    // Double-lock → INVALIDCALL.
    D3DLOCKED_RECT lr2 = {};
    check_hr_eq(sys->LockRect(&lr2, NULL, 0), D3DERR_INVALIDCALL);

    check_hr(sys->UnlockRect());

    sys->Release();
  }

  // ---- DXT1 32x32 MANAGED texture — exercises compressed-format
  // 4-block alignment gate at LockRect (audit B1).
  // 32x32 is the smallest DXT1 surface the smoke can use without
  // forcing the alignment rules to interact with non-block-multiple
  // surface extents. wined3d texture.c d3d9_texture_2d_lock_rect:142
  // is the reference enforcement.
  IDirect3DTexture9 *dxt = NULL;
  check_hr(dev->CreateTexture(32, 32, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &dxt, NULL));
  check_true(dxt != NULL);

  if (dxt) {
    D3DLOCKED_RECT lr = {};

    // Unaligned left → INVALIDCALL.
    RECT bad1 = {1, 0, 8, 8};
    check_hr_eq(dxt->LockRect(0, &lr, &bad1, 0), D3DERR_INVALIDCALL);

    // Unaligned top → INVALIDCALL.
    RECT bad2 = {0, 1, 8, 8};
    check_hr_eq(dxt->LockRect(0, &lr, &bad2, 0), D3DERR_INVALIDCALL);

    // Both unaligned → INVALIDCALL.
    RECT bad3 = {3, 3, 8, 8};
    check_hr_eq(dxt->LockRect(0, &lr, &bad3, 0), D3DERR_INVALIDCALL);

    // Aligned 4-block rect → S_OK; right/bottom are at the surface
    // extent (which IS block-aligned by D3D9 contract on the create)
    // so they may exceed the strict 4-multiple requirement.
    RECT good = {4, 4, 8, 8};
    check_hr(dxt->LockRect(0, &lr, &good, 0));
    check_true(lr.pBits != NULL);
    check_hr(dxt->UnlockRect(0));

    // Whole-surface lock (NULL rect) → S_OK regardless of compressed
    // alignment (the surface extent is always block-aligned).
    check_hr(dxt->LockRect(0, &lr, NULL, 0));
    check_hr(dxt->UnlockRect(0));

    // AddDirtyRect(NULL) marks the whole sub-resource set; the call
    // must return S_OK and not crash even on a freshly-created
    // texture with no Lock history.
    check_hr(dxt->AddDirtyRect(NULL));

    // AddDirtyRect with a real rect — should also S_OK (Set,
    // not Get; we can't read back from the API).
    RECT dirty = {0, 0, 16, 16};
    check_hr(dxt->AddDirtyRect(&dirty));

    dxt->Release();
  }

  check_zero_losable_count(dev);
}
