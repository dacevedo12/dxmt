// Clear — first GPU-touching method. We can't pixel-verify yet
// (GetRenderTargetData / LockRect aren't implemented), so the smoke
// only exercises the API contract and that committing the cmd buffer
// to Metal doesn't fault.
//
// Contract (DXVK d3d9_device.cpp:1921 / wined3d device.c:2120):
//   - Count==0 with non-null pRects: D3D_OK no-op
//   - D3DCLEAR_ZBUFFER or D3DCLEAR_STENCIL set with no DS bound:
//     INVALIDCALL
//   - D3DCLEAR_TARGET with bound RT: D3D_OK, attachment cleared
//   - Combined TARGET + ZBUFFER + STENCIL with both bound: D3D_OK
//
// We use a manually-created offscreen RT + DS rather than the implicit
// backbuffer because device-side auto-bind of the implicit RT/DS
// hasn't landed yet.

#include "../dx9_smoke.h"
void
test_clear(void) {
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

  // ---- Bind a 64x48 offscreen RT. ----
  IDirect3DSurface9 *rt = NULL;
  HRESULT crh = dev->CreateRenderTarget(64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  printf("CreateRenderTarget: hr=0x%08lx\n", (unsigned long)crh);
  if (FAILED(crh) || !rt) {
    dev->Release();
    d3d->Release();
    return;
  }
  HRESULT srh = dev->SetRenderTarget(0, rt);
  printf("SetRenderTarget(0): hr=0x%08lx\n", (unsigned long)srh);

  // ---- Count==0 + pRects != NULL → no-op success. ----
  D3DRECT phantom = {0, 0, 0, 0};
  HRESULT hr = dev->Clear(0, &phantom, D3DCLEAR_TARGET, 0, 0.0f, 0);
  check_hr(hr);

  // ---- Z without DS bound → INVALIDCALL. ----
  hr = dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  hr = dev->Clear(0, NULL, D3DCLEAR_STENCIL, 0, 0.0f, 0);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  hr = dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- TARGET clear succeeds with just RT bound. ----
  hr = dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF8040C0u, 0.0f, 0);
  check_hr(hr);

  // ---- Bind a DS, then full TARGET+Z+STENCIL clear. ----
  IDirect3DSurface9 *ds = NULL;
  HRESULT cdsh = dev->CreateDepthStencilSurface(64, 48, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL);
  printf("CreateDepthStencilSurface: hr=0x%08lx\n", (unsigned long)cdsh);
  if (SUCCEEDED(cdsh) && ds) {
    HRESULT sds = dev->SetDepthStencilSurface(ds);
    printf("SetDepthStencilSurface: hr=0x%08lx\n", (unsigned long)sds);

    hr = dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0xFF112233u, 0.5f, 0x42);
    check_hr(hr);

    hr = dev->Clear(0, NULL, D3DCLEAR_ZBUFFER, 0, 0.25f, 0);
    check_hr(hr);

    hr = dev->Clear(0, NULL, D3DCLEAR_STENCIL, 0, 0.0f, 0xAA);
    check_hr(hr);

    ds->Release();
  }

  // ---- All-flags-zero must be a no-op success, not a Metal fault. ----
  hr = dev->Clear(0, NULL, 0, 0, 0.0f, 0);
  printf("Clear(Flags=0): hr=0x%08lx (want D3D_OK no-op)\n", (unsigned long)hr);

  // ---- Count > 0 with NULL pRects — DXVK treats Count as zero and
  // does a full-RT clear. ----
  hr = dev->Clear(3, NULL, D3DCLEAR_TARGET, 0xFF000000u, 0.0f, 0);
  check_hr(hr);

  // ---- Stencil clear against a depth-only DS format must silently
  // drop the stencil bit (HasStencilAspect false), not error. ----
  IDirect3DSurface9 *ds_depth_only = NULL;
  HRESULT cdsd =
      dev->CreateDepthStencilSurface(64, 48, D3DFMT_D32, D3DMULTISAMPLE_NONE, 0, FALSE, &ds_depth_only, NULL);
  printf("CreateDepthStencilSurface(D32): hr=0x%08lx\n", (unsigned long)cdsd);
  if (SUCCEEDED(cdsd) && ds_depth_only) {
    dev->SetDepthStencilSurface(ds_depth_only);
    hr = dev->Clear(0, NULL, D3DCLEAR_STENCIL, 0, 0.0f, 0x55);
    printf("Clear(STENCIL on D32): hr=0x%08lx (want D3D_OK silent-drop)\n", (unsigned long)hr);
    hr = dev->Clear(0, NULL, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 0.5f, 0x33);
    printf("Clear(Z|S on D32): hr=0x%08lx (want D3D_OK, S dropped)\n", (unsigned long)hr);
    dev->SetDepthStencilSurface(NULL);
    ds_depth_only->Release();
  }

  // ---- Surface refcount sanity: Clear must not have AddRef'd the
  // bound RT publicly. After we drop our app-side ref the slot's
  // private pin is the only thing keeping it alive — Release returns 0.
  ULONG rt_ref = rt->Release();
  printf("Release(rt): %lu (want 0 — slot owns priv pin)\n", (unsigned long)rt_ref);

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
