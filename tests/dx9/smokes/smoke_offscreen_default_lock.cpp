// DEFAULT-pool off-screen plain surface LockRect (issue #314).
//
// MSDN: off-screen plain surfaces are ALWAYS lockable, regardless of
// pool. dxmt previously gave a DEFAULT-pool off-screen surface a
// Private (CpuInvisible) texture with no CPU pointer, so LockRect
// returned D3DERR_INVALIDCALL. COD4's RB_StretchRaw creates exactly
// such a surface (X8R8G8B8, DEFAULT), LockRect(DISCARD)s it to fill a
// raw image, then StretchRects it to the back buffer — and calls
// Com_Error(ERR_FATAL) if the lock fails. This smoke reproduces that
// pattern at the API level plus the rejection paths.
//
// Validates: LockRect succeeds with a non-null pointer + sane pitch;
// the write survives Unlock + re-Lock (the host mirror persists);
// the surface is a valid StretchRect source (its Private texture is
// kept, so StretchRect-to-backbuffer works); NULL-out / double-lock /
// unlock-without-lock all reject.

#include "../dx9_smoke.h"

void
test_offscreen_default_lock(void) {
  Dx9Fixture fx;
  if (!fx.create(320, 240, D3DFMT_X8R8G8B8))
    return;

  // ---- DEFAULT-pool 64x48 X8R8G8B8 off-screen plain surface. ----
  IDirect3DSurface9 *surf = nullptr;
  check_hr(fx.dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &surf, nullptr));
  if (!surf)
    return;

  // ---- Full-surface LockRect(DISCARD): the previously-failing call. ----
  D3DLOCKED_RECT lr = {};
  check_hr(surf->LockRect(&lr, nullptr, D3DLOCK_DISCARD));
  check_true(lr.pBits != nullptr);
  check_true(lr.Pitch >= 64 * 4); // X8R8G8B8 = 4 bpp, 64 wide

  // Paint a deterministic pattern so the re-lock read-back can verify
  // the host mirror is the CPU master between locks.
  if (lr.pBits) {
    for (int y = 0; y < 48; ++y) {
      DWORD *row = (DWORD *)((char *)lr.pBits + (size_t)y * lr.Pitch);
      for (int x = 0; x < 64; ++x)
        row[x] = 0xFF204060u + (DWORD)(y * 64 + x);
    }
  }
  check_hr(surf->UnlockRect());

  // ---- Re-lock: the written pattern must still be there. ----
  D3DLOCKED_RECT lr2 = {};
  check_hr(surf->LockRect(&lr2, nullptr, 0));
  if (lr2.pBits) {
    DWORD got = ((DWORD *)lr2.pBits)[0];
    check_eq_u32(got, 0xFF204060u);
    DWORD last = ((DWORD *)((char *)lr2.pBits + (size_t)47 * lr2.Pitch))[63];
    check_eq_u32(last, 0xFF204060u + (DWORD)(47 * 64 + 63));
  }

  // ---- Double-lock rejected. ----
  D3DLOCKED_RECT lr_dbl = {};
  check_hr_eq(surf->LockRect(&lr_dbl, nullptr, 0), D3DERR_INVALIDCALL);
  check_hr(surf->UnlockRect());

  // ---- Unlock-without-lock rejected (standalone surface stays strict). ----
  check_hr_eq(surf->UnlockRect(), D3DERR_INVALIDCALL);

  // ---- NULL out-pointer rejected. ----
  check_hr_eq(surf->LockRect(nullptr, nullptr, 0), D3DERR_INVALIDCALL);

  // ---- COD4 RB_StretchRaw shape: fill then StretchRect to the back
  // buffer. Exercises the upload-on-unlock landing in the Private
  // texture + the surface being a valid (non-null dxmtTexture)
  // StretchRect source scaled into the larger back buffer. ----
  D3DLOCKED_RECT lr3 = {};
  check_hr(surf->LockRect(&lr3, nullptr, D3DLOCK_DISCARD));
  if (lr3.pBits) {
    for (int y = 0; y < 48; ++y) {
      DWORD *row = (DWORD *)((char *)lr3.pBits + (size_t)y * lr3.Pitch);
      for (int x = 0; x < 64; ++x)
        row[x] = 0xFF80C0FFu;
    }
  }
  check_hr(surf->UnlockRect());

  IDirect3DSurface9 *bb = nullptr;
  check_hr(fx.dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb));
  if (bb) {
    check_hr(fx.dev->StretchRect(surf, nullptr, bb, nullptr, D3DTEXF_NONE));
    bb->Release();
  }

  surf->Release();
}
