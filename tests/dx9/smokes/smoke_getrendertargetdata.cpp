// GetRenderTargetData — first end-to-end correctness smoke for the
// GPU pipeline. Clear an RT to a known color, blit it down to a
// SYSTEMMEM surface, LockRect, read back the pixel and compare.
//
// This is also the first place pixel-level correctness of Clear is
// actually verified (smoke_clear only asserted hr=S_OK because there
// was no readback path).

#include "../dx9_smoke.h"
void
test_getrendertargetdata(void) {
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

  // ---- Allocate matched RT (DEFAULT) and SYSTEMMEM destination. ----
  IDirect3DSurface9 *rt = NULL;
  dev->CreateRenderTarget(64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  IDirect3DSurface9 *sys = NULL;
  dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
  printf("Created rt=%s sys=%s\n", rt ? "ok" : "null", sys ? "ok" : "null");
  if (!rt || !sys) {
    dev->Release();
    d3d->Release();
    return;
  }

  dev->SetRenderTarget(0, rt);

  // ---- Clear RT to a known color, blit, lock, read. ----
  // D3DCOLOR is 0xAARRGGBB. After the X8R8G8B8 round-trip the alpha
  // channel is undefined (X means "ignored"), so we mask it out
  // before comparing.
  const D3DCOLOR clearColor = 0xFF8040C0u;
  HRESULT chr = dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);
  printf("Clear(0xFF8040C0): hr=0x%08lx\n", (unsigned long)chr);

  HRESULT grh = dev->GetRenderTargetData(rt, sys);
  printf("GetRenderTargetData: hr=0x%08lx\n", (unsigned long)grh);

  D3DLOCKED_RECT lr = {};
  HRESULT lhr = sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  printf("LockRect(sys): hr=0x%08lx pBits=%s pitch=%d\n", (unsigned long)lhr, lr.pBits ? "non-null" : "null", lr.Pitch);
  if (!lr.pBits)
    printf("FAIL: first-blit LockRect skipped readback\n");
  if (lr.pBits) {
    DWORD pixel = ((DWORD *)lr.pBits)[0];
    DWORD pixel_rgb = pixel & 0x00FFFFFFu;
    DWORD want_rgb = clearColor & 0x00FFFFFFu;
    printf(
        "  pixel(0,0)=0x%08lx rgb=0x%06lx want_rgb=0x%06lx match=%s\n", (unsigned long)pixel, (unsigned long)pixel_rgb,
        (unsigned long)want_rgb, pixel_rgb == want_rgb ? "yes" : "no"
    );
    // Also probe a far pixel — the loadAction=clear must paint the
    // whole attachment, not just the origin.
    int far_y = 47, far_x = 63;
    DWORD far_pixel = ((DWORD *)((char *)lr.pBits + (size_t)far_y * lr.Pitch))[far_x];
    DWORD far_rgb = far_pixel & 0x00FFFFFFu;
    printf(
        "  pixel(63,47)=0x%08lx rgb=0x%06lx match=%s\n", (unsigned long)far_pixel, (unsigned long)far_rgb,
        far_rgb == want_rgb ? "yes" : "no"
    );
    sys->UnlockRect();
  }

  // ---- Second clear with a different color confirms the first wasn't
  // a coincidence and that the buffer-backed dst is actually
  // re-blittable. ----
  const D3DCOLOR color2 = 0xFF112233u;
  dev->Clear(0, NULL, D3DCLEAR_TARGET, color2, 0.0f, 0);
  dev->GetRenderTargetData(rt, sys);
  D3DLOCKED_RECT lr2 = {};
  sys->LockRect(&lr2, NULL, D3DLOCK_READONLY);
  if (!lr2.pBits)
    printf("FAIL: second-blit LockRect skipped readback\n");
  if (lr2.pBits) {
    DWORD p = ((DWORD *)lr2.pBits)[0] & 0x00FFFFFFu;
    DWORD w = color2 & 0x00FFFFFFu;
    printf(
        "Second blit: pixel(0,0).rgb=0x%06lx want=0x%06lx match=%s\n", (unsigned long)p, (unsigned long)w,
        p == w ? "yes" : "no"
    );
    sys->UnlockRect();
  }

  // ---- Rejection paths. ----
  HRESULT bad = dev->GetRenderTargetData(NULL, sys);
  check_hr_eq(bad, D3DERR_INVALIDCALL);
  bad = dev->GetRenderTargetData(rt, NULL);
  check_hr_eq(bad, D3DERR_INVALIDCALL);
  bad = dev->GetRenderTargetData(rt, rt);
  printf("GetRenderTargetData(src==dst): hr=0x%08lx (want D3D_OK no-op)\n", (unsigned long)bad);

  // Format mismatch (A8R8G8B8 dst).
  IDirect3DSurface9 *sys_a = NULL;
  dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys_a, NULL);
  if (sys_a) {
    bad = dev->GetRenderTargetData(rt, sys_a);
    printf(
        "GetRenderTargetData(format mismatch): hr=0x%08lx "
        "(want INVALIDCALL)\n",
        (unsigned long)bad
    );
    sys_a->Release();
  }

  // Size mismatch.
  IDirect3DSurface9 *sys_big = NULL;
  dev->CreateOffscreenPlainSurface(128, 96, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys_big, NULL);
  if (sys_big) {
    bad = dev->GetRenderTargetData(rt, sys_big);
    printf(
        "GetRenderTargetData(size mismatch): hr=0x%08lx "
        "(want INVALIDCALL)\n",
        (unsigned long)bad
    );
    sys_big->Release();
  }

  // ---- A8R8G8B8 round-trip — unlike X8 the alpha byte must survive
  // the blit/readback pair. ----
  IDirect3DSurface9 *rt_a = NULL;
  IDirect3DSurface9 *sys_a8 = NULL;
  dev->CreateRenderTarget(64, 48, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_a, NULL);
  dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys_a8, NULL);
  if (!rt_a || !sys_a8)
    printf("FAIL: A8R8G8B8 surface alloc skipped\n");
  if (rt_a && sys_a8) {
    dev->SetRenderTarget(0, rt_a);
    const D3DCOLOR a_color = 0x77AABB99u;
    dev->Clear(0, NULL, D3DCLEAR_TARGET, a_color, 0.0f, 0);
    dev->GetRenderTargetData(rt_a, sys_a8);
    D3DLOCKED_RECT alr = {};
    sys_a8->LockRect(&alr, NULL, D3DLOCK_READONLY);
    if (!alr.pBits)
      printf("FAIL: A8R8G8B8 LockRect skipped round-trip\n");
    if (alr.pBits) {
      DWORD p = ((DWORD *)alr.pBits)[0];
      printf(
          "A8R8G8B8 round-trip: pixel=0x%08lx want=0x%08lx match=%s\n", (unsigned long)p, (unsigned long)a_color,
          p == a_color ? "yes" : "no"
      );
      sys_a8->UnlockRect();

      // ---- Persistence across Unlock/Relock: reading the same
      // surface again without a fresh blit must return the same
      // bytes. Guards SYSTEMMEM storage-mode regressions (Managed
      // would need didModifyRange; Private would lose contents). ----
      D3DLOCKED_RECT alr2 = {};
      sys_a8->LockRect(&alr2, NULL, D3DLOCK_READONLY);
      DWORD p2 = ((DWORD *)alr2.pBits)[0];
      printf("Persistence after relock: pixel=0x%08lx match=%s\n", (unsigned long)p2, p2 == a_color ? "yes" : "no");
      sys_a8->UnlockRect();
    }
    rt_a->Release();
    sys_a8->Release();
    // Restore RT0 binding for the cleanup paths below.
    dev->SetRenderTarget(0, rt);
  }

  // ---- MSAA RT source must reject (D3D9 spec; Metal can't blit
  // multisampled textures via the texture-to-texture path). ----
  IDirect3DSurface9 *rt_msaa = NULL;
  HRESULT cmh = dev->CreateRenderTarget(64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, 0, FALSE, &rt_msaa, NULL);
  printf("CreateRenderTarget(2x MSAA): hr=0x%08lx\n", (unsigned long)cmh);
  if (rt_msaa) {
    bad = dev->GetRenderTargetData(rt_msaa, sys);
    check_hr_eq(bad, D3DERR_INVALIDCALL);
    rt_msaa->Release();
  }

  // DEFAULT-pool dst (the StretchRect-fallback case we don't support).
  IDirect3DSurface9 *rt_dst = NULL;
  dev->CreateRenderTarget(64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt_dst, NULL);
  if (rt_dst) {
    bad = dev->GetRenderTargetData(rt, rt_dst);
    printf(
        "GetRenderTargetData(DEFAULT dst): hr=0x%08lx "
        "(want INVALIDCALL — no StretchRect yet)\n",
        (unsigned long)bad
    );
    rt_dst->Release();
  }

  sys->Release();
  rt->Release();
  ULONG dr = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dr);
  ULONG r = d3d->Release();
  printf("Release: %lu\n", (unsigned long)r);
}
