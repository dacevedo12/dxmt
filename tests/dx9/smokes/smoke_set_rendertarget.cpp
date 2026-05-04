// SetRenderTarget / GetRenderTarget / SetDepthStencilSurface /
// GetDepthStencilSurface smoke. Bind a few surfaces, validate the
// rejection paths the runtime is expected to gate (idx out of range,
// slot 0 NULL, cross-device surface), exercise the round-trip via
// Get*. The lifetime probe walks the priv-ref pin: surface released
// publicly while still bound stays alive via the device's private
// ref, drops only on device teardown.

#include "../dx9_smoke.h"
void
test_set_rendertarget(void) {
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

  // Initial state: D3D9 spec auto-binds the implicit backbuffer to RT0
  // at device-create time, so GetRenderTarget(0) returns the backbuffer
  // surface (same pointer as GetBackBuffer(0,0,MONO) would). Slots 1+
  // are unbound (NOTFOUND). DS isn't auto-bound here because we passed
  // EnableAutoDepthStencil=FALSE; the AutoDS path is a separate commit.
  IDirect3DSurface9 *initial_rt0 = NULL;
  HRESULT g0 = dev->GetRenderTarget(0, &initial_rt0);
  printf(
      "GetRenderTarget(0) at start: hr=0x%08lx out=%s (auto-bound bb)\n", (unsigned long)g0,
      initial_rt0 ? "non-null" : "null"
  );
  if (initial_rt0)
    initial_rt0->Release();

  IDirect3DSurface9 *initial_rt1 = NULL;
  HRESULT g1 = dev->GetRenderTarget(1, &initial_rt1);
  printf(
      "GetRenderTarget(1) at start: hr=0x%08lx out=%s (unbound slot)\n", (unsigned long)g1,
      initial_rt1 ? "non-null" : "null"
  );
  if (initial_rt1)
    initial_rt1->Release();

  IDirect3DSurface9 *initial_ds = NULL;
  HRESULT gds = dev->GetDepthStencilSurface(&initial_ds);
  printf("GetDepthStencilSurface at start: hr=0x%08lx out=%s\n", (unsigned long)gds, initial_ds ? "non-null" : "null");
  if (initial_ds)
    initial_ds->Release();

  // Make a couple of RTs and a DS to bind.
  IDirect3DSurface9 *rt0 = NULL;
  dev->CreateRenderTarget(256, 256, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt0, NULL);
  IDirect3DSurface9 *rt1 = NULL;
  dev->CreateRenderTarget(128, 128, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt1, NULL);
  IDirect3DSurface9 *ds = NULL;
  dev->CreateDepthStencilSurface(256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, NULL);
  printf("Created rt0=%s rt1=%s ds=%s\n", rt0 ? "ok" : "null", rt1 ? "ok" : "null", ds ? "ok" : "null");

  // Happy path: bind RT0, RT1, DS; round-trip via Get*.
  HRESULT s0 = dev->SetRenderTarget(0, rt0);
  HRESULT s1 = dev->SetRenderTarget(1, rt1);
  HRESULT sd = dev->SetDepthStencilSurface(ds);
  printf("SetRenderTarget(0): hr=0x%08lx\n", (unsigned long)s0);
  printf("SetRenderTarget(1): hr=0x%08lx\n", (unsigned long)s1);
  printf("SetDepthStencilSurface: hr=0x%08lx\n", (unsigned long)sd);

  IDirect3DSurface9 *got0 = NULL;
  HRESULT gg0 = dev->GetRenderTarget(0, &got0);
  printf("GetRenderTarget(0): hr=0x%08lx same=%s\n", (unsigned long)gg0, got0 == rt0 ? "yes" : "no");
  if (got0)
    got0->Release();

  IDirect3DSurface9 *got1 = NULL;
  HRESULT gg1 = dev->GetRenderTarget(1, &got1);
  printf("GetRenderTarget(1): hr=0x%08lx same=%s\n", (unsigned long)gg1, got1 == rt1 ? "yes" : "no");
  if (got1)
    got1->Release();

  IDirect3DSurface9 *gotds = NULL;
  HRESULT ggds = dev->GetDepthStencilSurface(&gotds);
  printf("GetDepthStencilSurface: hr=0x%08lx same=%s\n", (unsigned long)ggds, gotds == ds ? "yes" : "no");
  if (gotds)
    gotds->Release();

  // Slot 2 still empty.
  IDirect3DSurface9 *got2 = NULL;
  HRESULT gg2 = dev->GetRenderTarget(2, &got2);
  printf("GetRenderTarget(2) (unbound): hr=0x%08lx out=%s\n", (unsigned long)gg2, got2 ? "non-null" : "null");
  if (got2)
    got2->Release();

  // Unbind RT1.
  HRESULT u1 = dev->SetRenderTarget(1, NULL);
  printf("SetRenderTarget(1, NULL) (unbind): hr=0x%08lx\n", (unsigned long)u1);
  IDirect3DSurface9 *got1b = NULL;
  HRESULT gg1b = dev->GetRenderTarget(1, &got1b);
  printf("GetRenderTarget(1) post-unbind: hr=0x%08lx out=%s\n", (unsigned long)gg1b, got1b ? "non-null" : "null");
  if (got1b)
    got1b->Release();

  // Unbind DS — allowed (depth-disabled rendering).
  HRESULT uds = dev->SetDepthStencilSurface(NULL);
  printf("SetDepthStencilSurface(NULL) (unbind): hr=0x%08lx\n", (unsigned long)uds);
  IDirect3DSurface9 *gotdsb = NULL;
  HRESULT ggdsb = dev->GetDepthStencilSurface(&gotdsb);
  printf("GetDepthStencilSurface post-unbind: hr=0x%08lx out=%s\n", (unsigned long)ggdsb, gotdsb ? "non-null" : "null");
  if (gotdsb)
    gotdsb->Release();

  // ---- Failure paths ----
  // Slot >= MAX (4).
  HRESULT bad_idx = dev->SetRenderTarget(4, rt0);
  printf("SetRenderTarget(4): hr=0x%08lx (idx >= MAX must reject)\n", (unsigned long)bad_idx);
  HRESULT bad_idx_g = dev->GetRenderTarget(4, &got0);
  printf("GetRenderTarget(4): hr=0x%08lx\n", (unsigned long)bad_idx_g);

  // Slot 0 NULL — must reject.
  HRESULT bad_null0 = dev->SetRenderTarget(0, NULL);
  printf("SetRenderTarget(0, NULL): hr=0x%08lx (slot 0 NULL must reject)\n", (unsigned long)bad_null0);
  // RT0 should still be bound.
  IDirect3DSurface9 *still0 = NULL;
  dev->GetRenderTarget(0, &still0);
  printf("  RT0 unchanged after rejected NULL: same=%s\n", still0 == rt0 ? "yes" : "no");
  if (still0)
    still0->Release();

  // ---- Cross-device rejection ----
  IDirect3DDevice9 *dev2 = NULL;
  HRESULT cd2 = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev2);
  printf("CreateDevice(2): hr=0x%08lx\n", (unsigned long)cd2);
  if (SUCCEEDED(cd2) && dev2) {
    IDirect3DSurface9 *foreign = NULL;
    dev2->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &foreign, NULL);
    if (foreign) {
      HRESULT bad_dev = dev->SetRenderTarget(0, foreign);
      printf("SetRenderTarget(0, foreign-device-surface): hr=0x%08lx\n", (unsigned long)bad_dev);
      foreign->Release();
    }
    dev2->Release();
  }

  // ---- Lifetime: release rt0 publicly while still bound. Device's
  // priv ref keeps it alive. SetRenderTarget(0, replacement) drops
  // the priv ref → rt0 destructs (chain release tears device too).
  IDirect3DSurface9 *replacement = NULL;
  dev->CreateRenderTarget(256, 256, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &replacement, NULL);
  ULONG rt0_ref = rt0->Release();
  printf("Release(rt0) while bound: %lu (device priv ref keeps it alive)\n", (unsigned long)rt0_ref);
  // GetRenderTarget(0) should still return the (private-pinned) rt0.
  IDirect3DSurface9 *still_rt0 = NULL;
  HRESULT still_hr = dev->GetRenderTarget(0, &still_rt0);
  printf(
      "GetRenderTarget(0) post-release: hr=0x%08lx ptr_match=%s\n", (unsigned long)still_hr,
      still_rt0 == rt0 ? "yes" : "no"
  );
  if (still_rt0)
    still_rt0->Release();
  // Replace RT0 — drops device's priv ref on rt0, which now destructs.
  dev->SetRenderTarget(0, replacement);
  if (replacement)
    replacement->Release();

  if (rt1)
    rt1->Release();
  if (ds)
    ds->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
