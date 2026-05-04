// Viewport / scissor state-only round-trip. The interesting
// behavioural divergence from native D3D9 is inverted-Z normalisation
// (DXVK d3d9_device.cpp:2154 — collapses clip-space otherwise).

#include "../dx9_smoke.h"
void
test_set_viewport(void) {
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

  // ---- Default viewport. ----
  D3DVIEWPORT9 vp = {};
  HRESULT hr = dev->GetViewport(&vp);
  printf(
      "GetViewport default: hr=0x%08lx X=%lu Y=%lu W=%lu H=%lu "
      "MinZ=%g MaxZ=%g (want 0,0,320,240,0,1)\n",
      (unsigned long)hr, (unsigned long)vp.X, (unsigned long)vp.Y, (unsigned long)vp.Width, (unsigned long)vp.Height,
      vp.MinZ, vp.MaxZ
  );

  // ---- Default scissor. ----
  RECT sr = {};
  hr = dev->GetScissorRect(&sr);
  printf(
      "GetScissorRect default: hr=0x%08lx (%ld,%ld)-(%ld,%ld) "
      "(want 0,0-320,240)\n",
      (unsigned long)hr, (long)sr.left, (long)sr.top, (long)sr.right, (long)sr.bottom
  );

  // ---- Round-trip viewport with non-default values. ----
  D3DVIEWPORT9 set = {16, 32, 200, 150, 0.25f, 0.75f};
  hr = dev->SetViewport(&set);
  printf("SetViewport(custom): hr=0x%08lx\n", (unsigned long)hr);

  D3DVIEWPORT9 back = {};
  hr = dev->GetViewport(&back);
  printf(
      "GetViewport custom: hr=0x%08lx X=%lu Y=%lu W=%lu H=%lu "
      "MinZ=%g MaxZ=%g\n",
      (unsigned long)hr, (unsigned long)back.X, (unsigned long)back.Y, (unsigned long)back.Width,
      (unsigned long)back.Height, back.MinZ, back.MaxZ
  );

  // ---- Inverted depth range — degenerate (MinZ==MaxZ) and true
  // inversion (MinZ > MaxZ) both must produce MaxZ > MinZ on readback.
  D3DVIEWPORT9 degenerate = {0, 0, 320, 240, 0.5f, 0.5f};
  dev->SetViewport(&degenerate);
  D3DVIEWPORT9 readback = {};
  dev->GetViewport(&readback);
  printf(
      "Inverted-Z (==) readback: MinZ=%g MaxZ=%g valid=%s\n", readback.MinZ, readback.MaxZ,
      readback.MaxZ > readback.MinZ ? "yes" : "no"
  );

  D3DVIEWPORT9 inverted = {0, 0, 320, 240, 0.9f, 0.1f};
  dev->SetViewport(&inverted);
  dev->GetViewport(&readback);
  printf(
      "Inverted-Z (>) readback: MinZ=%g MaxZ=%g valid=%s\n", readback.MinZ, readback.MaxZ,
      readback.MaxZ > readback.MinZ ? "yes" : "no"
  );

  // ---- Round-trip scissor. ----
  RECT s_set = {5, 10, 100, 200};
  hr = dev->SetScissorRect(&s_set);
  printf("SetScissorRect: hr=0x%08lx\n", (unsigned long)hr);

  RECT s_back = {};
  hr = dev->GetScissorRect(&s_back);
  printf(
      "GetScissorRect: hr=0x%08lx (%ld,%ld)-(%ld,%ld)\n", (unsigned long)hr, (long)s_back.left, (long)s_back.top,
      (long)s_back.right, (long)s_back.bottom
  );

  // ---- Out-of-bounds scissor (negative + larger than RT) — D3D9
  // accepts the value as-is. ----
  RECT s_oob = {-50, -50, 1000, 1000};
  hr = dev->SetScissorRect(&s_oob);
  printf("SetScissorRect(oob): hr=0x%08lx\n", (unsigned long)hr);
  dev->GetScissorRect(&s_back);
  printf(
      "GetScissorRect(oob): (%ld,%ld)-(%ld,%ld)\n", (long)s_back.left, (long)s_back.top, (long)s_back.right,
      (long)s_back.bottom
  );

  // ---- NULL rejection must NOT clobber prior state. Set a known
  // viewport, fire NULL, read back. ----
  D3DVIEWPORT9 anchor = {1, 2, 3, 4, 0.1f, 0.9f};
  dev->SetViewport(&anchor);
  hr = dev->SetViewport(NULL);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  D3DVIEWPORT9 after_null = {};
  dev->GetViewport(&after_null);
  printf(
      "After NULL: X=%lu Y=%lu W=%lu H=%lu (want 1 2 3 4)\n", (unsigned long)after_null.X, (unsigned long)after_null.Y,
      (unsigned long)after_null.Width, (unsigned long)after_null.Height
  );

  hr = dev->GetViewport(NULL);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  hr = dev->SetScissorRect(NULL);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  hr = dev->GetScissorRect(NULL);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- SetRenderTarget(0, …) must reseed viewport+scissor to the
  // new RT's extent (D3D9 spec). Bind a smaller offscreen RT and
  // verify the viewport snaps to it. ----
  IDirect3DSurface9 *offscreen = NULL;
  HRESULT crh = dev->CreateRenderTarget(64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &offscreen, NULL);
  printf("CreateRenderTarget(64x48): hr=0x%08lx\n", (unsigned long)crh);
  if (SUCCEEDED(crh) && offscreen) {
    // Stale viewport before SRT — the reseed must overwrite this.
    D3DVIEWPORT9 stale = {5, 6, 7, 8, 0.0f, 1.0f};
    dev->SetViewport(&stale);
    HRESULT srt = dev->SetRenderTarget(0, offscreen);
    printf("SetRenderTarget(0, 64x48): hr=0x%08lx\n", (unsigned long)srt);
    D3DVIEWPORT9 reseeded = {};
    dev->GetViewport(&reseeded);
    printf(
        "Reseeded viewport: X=%lu Y=%lu W=%lu H=%lu MinZ=%g MaxZ=%g "
        "(want 0 0 64 48 0 1)\n",
        (unsigned long)reseeded.X, (unsigned long)reseeded.Y, (unsigned long)reseeded.Width,
        (unsigned long)reseeded.Height, reseeded.MinZ, reseeded.MaxZ
    );
    RECT reseeded_sr = {};
    dev->GetScissorRect(&reseeded_sr);
    printf(
        "Reseeded scissor: (%ld,%ld)-(%ld,%ld) (want 0,0-64,48)\n", (long)reseeded_sr.left, (long)reseeded_sr.top,
        (long)reseeded_sr.right, (long)reseeded_sr.bottom
    );
    offscreen->Release();
  }

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
