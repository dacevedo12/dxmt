// LockRect / UnlockRect against a SYSTEMMEM offscreen-plain surface.
// Buffer-backed texture; pBits points directly at the buffer's CPU
// pointer, no copy on lock or unlock. Verifies pitch, sub-rect offset
// math, write-then-read round-trip, and the rejection paths
// (non-lockable surface, NULL out, double-lock, out-of-bounds rect).

#include "../dx9_smoke.h"
void
test_lockrect(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("Direct3DCreate9: NULL\n");
    return;
  }

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 320;
  pp.BackBufferHeight = 240;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  IDirect3DDevice9 *dev = NULL;
  HRESULT cdhr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  printf("CreateDevice: hr=0x%08lx\n", (unsigned long)cdhr);
  if (FAILED(cdhr) || !dev) {
    d3d->Release();
    return;
  }

  // ---- SYSTEMMEM 64x48 X8R8G8B8 surface. ----
  IDirect3DSurface9 *sys = NULL;
  HRESULT crh = dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
  printf("CreateOffscreenPlainSurface(SYSTEMMEM): hr=0x%08lx\n", (unsigned long)crh);

  // ---- Full-surface lock; pitch must be at least width*bpp. ----
  D3DLOCKED_RECT lr = {};
  HRESULT lhr = sys->LockRect(&lr, NULL, 0);
  printf(
      "LockRect(full): hr=0x%08lx pitch>=256=%s pBits=%s\n", (unsigned long)lhr, lr.Pitch >= 256 ? "yes" : "no",
      lr.pBits ? "non-null" : "null"
  );

  // Paint a known pattern across rows 0..2 so the sub-rect read at
  // (4,2) below has something deterministic to verify against.
  if (lr.pBits) {
    for (int y = 0; y < 3; ++y) {
      DWORD *row = (DWORD *)((char *)lr.pBits + (size_t)y * lr.Pitch);
      for (int x = 0; x < 64; ++x)
        row[x] = 0xFF112233u + (DWORD)(y * 100 + x);
    }
  }

  // ---- Double-lock rejected. ----
  D3DLOCKED_RECT lr2 = {};
  HRESULT dbl = sys->LockRect(&lr2, NULL, 0);
  check_hr_eq(dbl, D3DERR_INVALIDCALL);

  HRESULT uhr = sys->UnlockRect();
  printf("UnlockRect: hr=0x%08lx\n", (unsigned long)uhr);

  // ---- Double-unlock rejected. ----
  HRESULT uhr2 = sys->UnlockRect();
  check_hr_eq(uhr2, D3DERR_INVALIDCALL);

  // ---- Sub-rect lock — pBits must be offset into the buffer. ----
  RECT sub = {4, 2, 8, 5}; // 4x3 region starting at (4, 2)
  D3DLOCKED_RECT slr = {};
  HRESULT shr = sys->LockRect(&slr, &sub, 0);
  printf("LockRect(sub 4x3 @4,2): hr=0x%08lx\n", (unsigned long)shr);
  if (slr.pBits) {
    // pBits should be the full-lock pBits + 2*pitch + 4*4 bytes.
    // The painted value at full-coord (4,2) is 0xFF112233 + (2*100+4).
    DWORD got = *(DWORD *)((char *)slr.pBits);
    DWORD want = 0xFF112233u + (DWORD)(2 * 100 + 4);
    printf(
        "  sub[0,0]=0x%08lx want=0x%08lx match=%s\n", (unsigned long)got, (unsigned long)want,
        got == want ? "yes" : "no"
    );
    // Pitch unchanged across sub-rect locks.
    printf("  sub.pitch == full.pitch: %s\n", slr.Pitch == lr.Pitch ? "yes" : "no");
    // Write into the sub-rect; should be visible at the matching
    // full-lock coordinates after re-lock.
    DWORD *p = (DWORD *)slr.pBits;
    p[0] = 0xDEADBEEFu;
    sys->UnlockRect();

    D3DLOCKED_RECT lr3 = {};
    sys->LockRect(&lr3, NULL, 0);
    DWORD readback = *(DWORD *)((char *)lr3.pBits + 2 * lr3.Pitch + 4 * 4);
    printf("  full-lock readback at (4,2): 0x%08lx (want 0xDEADBEEF)\n", (unsigned long)readback);
    sys->UnlockRect();
  }

  // ---- Rejection paths. ----
  HRESULT bad = sys->LockRect(NULL, NULL, 0);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  RECT oob = {-1, 0, 64, 48};
  D3DLOCKED_RECT olr = {};
  bad = sys->LockRect(&olr, &oob, 0);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  RECT past = {0, 0, 65, 48};
  bad = sys->LockRect(&olr, &past, 0);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  RECT empty = {5, 5, 5, 5};
  bad = sys->LockRect(&olr, &empty, 0);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  // ---- A8 surface — bpp=1, pitch padding becomes non-trivial.
  // The minimum-linear-texture-alignment for A8 is typically 64+ bytes,
  // so pitch on a 64-wide A8 is >> width. Verifies row indexing uses
  // pitch, not width*bpp. ----
  IDirect3DSurface9 *sys_a8 = NULL;
  HRESULT crh_a8 = dev->CreateOffscreenPlainSurface(64, 16, D3DFMT_A8, D3DPOOL_SYSTEMMEM, &sys_a8, NULL);
  printf("CreateOffscreenPlainSurface(A8 SYSTEMMEM): hr=0x%08lx\n", (unsigned long)crh_a8);
  if (sys_a8) {
    D3DLOCKED_RECT alr = {};
    sys_a8->LockRect(&alr, NULL, 0);
    printf("LockRect(A8 full): pitch=%d width*bpp=64 padded=%s\n", alr.Pitch, alr.Pitch >= 64 ? "yes" : "no");
    if (alr.pBits) {
      // Write a row 1 marker, then re-lock and read it back through
      // pitch-aware indexing.
      uint8_t *row1 = (uint8_t *)alr.pBits + alr.Pitch;
      row1[7] = 0xA5;
      sys_a8->UnlockRect();
      D3DLOCKED_RECT alr2 = {};
      sys_a8->LockRect(&alr2, NULL, 0);
      uint8_t got = ((uint8_t *)alr2.pBits + alr2.Pitch)[7];
      printf("  row1[7] readback: 0x%02x (want 0xa5)\n", got);
      sys_a8->UnlockRect();
    }
    sys_a8->Release();
  }

  // ---- D3DPOOL_SCRATCH — same buffer-backed path as SYSTEMMEM. ----
  IDirect3DSurface9 *scratch = NULL;
  HRESULT csh = dev->CreateOffscreenPlainSurface(8, 8, D3DFMT_X8R8G8B8, D3DPOOL_SCRATCH, &scratch, NULL);
  printf("CreateOffscreenPlainSurface(SCRATCH): hr=0x%08lx\n", (unsigned long)csh);
  if (scratch) {
    D3DLOCKED_RECT clr = {};
    HRESULT slh = scratch->LockRect(&clr, NULL, 0);
    printf("LockRect(SCRATCH): hr=0x%08lx pBits=%s\n", (unsigned long)slh, clr.pBits ? "non-null" : "null");
    if (SUCCEEDED(slh))
      scratch->UnlockRect();
    scratch->Release();
  }

  // ---- pRect == full rect should match NULL-rect lock. ----
  RECT full = {0, 0, 64, 48};
  D3DLOCKED_RECT flr = {};
  HRESULT fhr = sys->LockRect(&flr, &full, 0);
  printf("LockRect(full-rect): hr=0x%08lx pitch=%d (want match NULL-rect)\n", (unsigned long)fhr, flr.Pitch);
  if (SUCCEEDED(fhr))
    sys->UnlockRect();

  // ---- Non-lockable surface (RT in DEFAULT pool) must reject. ----
  IDirect3DSurface9 *rt = NULL;
  dev->CreateRenderTarget(
      64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0,
      /*Lockable=*/FALSE, &rt, NULL
  );
  if (rt) {
    bad = rt->LockRect(&olr, NULL, 0);
    printf(
        "LockRect(non-lockable RT): hr=0x%08lx out=%s "
        "(want INVALIDCALL, NULL)\n",
        (unsigned long)bad, olr.pBits ? "non-null" : "null"
    );
    rt->Release();
  }

  if (sys)
    sys->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
