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
  // D3D9 auto-binds the implicit backbuffer to RT0 at create time.
  IDirect3DSurface9 *initial_rt0 = NULL;
  HRESULT g0 = dev->GetRenderTarget(0, &initial_rt0);
  check_hr(g0);
  check_true(initial_rt0 != nullptr);
  if (initial_rt0)
    initial_rt0->Release();

  // Unbound slot 1 → NOTFOUND, out NULL.
  IDirect3DSurface9 *initial_rt1 = NULL;
  HRESULT g1 = dev->GetRenderTarget(1, &initial_rt1);
  check_hr_eq(g1, D3DERR_NOTFOUND);
  check_true(initial_rt1 == NULL);

  // EnableAutoDepthStencil=FALSE, so no DS is bound → NOTFOUND, out NULL.
  IDirect3DSurface9 *initial_ds = NULL;
  HRESULT gds = dev->GetDepthStencilSurface(&initial_ds);
  check_hr_eq(gds, D3DERR_NOTFOUND);
  check_true(initial_ds == NULL);

  // Make a couple of RTs and a DS to bind.
  IDirect3DSurface9 *rt0 = NULL;
  dev->CreateRenderTarget(256, 256, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt0, NULL);
  IDirect3DSurface9 *rt1 = NULL;
  dev->CreateRenderTarget(128, 128, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt1, NULL);
  IDirect3DSurface9 *ds = NULL;
  dev->CreateDepthStencilSurface(256, 256, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, NULL);
  check_true(rt0 && rt1 && ds);

  // Happy path: bind RT0, RT1, DS; round-trip via Get*.
  HRESULT s0 = dev->SetRenderTarget(0, rt0);
  HRESULT s1 = dev->SetRenderTarget(1, rt1);
  HRESULT sd = dev->SetDepthStencilSurface(ds);
  check_hr(s0);
  check_hr(s1);
  check_hr(sd);

  IDirect3DSurface9 *got0 = NULL;
  HRESULT gg0 = dev->GetRenderTarget(0, &got0);
  check_hr(gg0);
  check_eq_ptr(got0, rt0);
  if (got0)
    got0->Release();

  IDirect3DSurface9 *got1 = NULL;
  HRESULT gg1 = dev->GetRenderTarget(1, &got1);
  check_hr(gg1);
  check_eq_ptr(got1, rt1);
  if (got1)
    got1->Release();

  IDirect3DSurface9 *gotds = NULL;
  HRESULT ggds = dev->GetDepthStencilSurface(&gotds);
  check_hr(ggds);
  check_eq_ptr(gotds, ds);
  if (gotds)
    gotds->Release();

  // Slot 2 still empty → NOTFOUND, out NULL.
  IDirect3DSurface9 *got2 = NULL;
  HRESULT gg2 = dev->GetRenderTarget(2, &got2);
  check_hr_eq(gg2, D3DERR_NOTFOUND);
  check_true(got2 == NULL);

  // Unbind RT1.
  HRESULT u1 = dev->SetRenderTarget(1, NULL);
  check_hr(u1);
  IDirect3DSurface9 *got1b = NULL;
  HRESULT gg1b = dev->GetRenderTarget(1, &got1b);
  check_hr_eq(gg1b, D3DERR_NOTFOUND);
  check_true(got1b == NULL);

  // Unbind DS — allowed (depth-disabled rendering).
  HRESULT uds = dev->SetDepthStencilSurface(NULL);
  check_hr(uds);
  IDirect3DSurface9 *gotdsb = NULL;
  HRESULT ggdsb = dev->GetDepthStencilSurface(&gotdsb);
  check_hr_eq(ggdsb, D3DERR_NOTFOUND);
  check_true(gotdsb == NULL);

  // ---- Failure paths ----
  // Slot >= MAX (4).
  HRESULT bad_idx = dev->SetRenderTarget(4, rt0);
  check_hr_eq(bad_idx, D3DERR_INVALIDCALL);
  HRESULT bad_idx_g = dev->GetRenderTarget(4, &got0);
  check_hr_eq(bad_idx_g, D3DERR_INVALIDCALL);

  // Slot 0 NULL — must reject, and RT0 must stay bound.
  HRESULT bad_null0 = dev->SetRenderTarget(0, NULL);
  check_hr_eq(bad_null0, D3DERR_INVALIDCALL);
  IDirect3DSurface9 *still0 = NULL;
  dev->GetRenderTarget(0, &still0);
  check_eq_ptr(still0, rt0);
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
      check_hr_eq(bad_dev, D3DERR_INVALIDCALL);
      foreign->Release();
    }
    dev2->Release();
  }

  // ---- Lifetime: release rt0 publicly while still bound. Device's
  // priv ref keeps it alive. SetRenderTarget(0, replacement) drops
  // the priv ref → rt0 destructs (chain release tears device too).
  IDirect3DSurface9 *replacement = NULL;
  dev->CreateRenderTarget(256, 256, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &replacement, NULL);
  rt0->Release(); // public ref drops; device's private ref keeps it alive
  // GetRenderTarget(0) must still return the (private-pinned) rt0.
  IDirect3DSurface9 *still_rt0 = NULL;
  HRESULT still_hr = dev->GetRenderTarget(0, &still_rt0);
  check_hr(still_hr);
  check_eq_ptr(still_rt0, rt0);
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
