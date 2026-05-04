// BeginScene / EndScene pair-bracket. State-only — the flush hint
// DXVK fires at EndScene isn't observable here yet.
//
// Contract (DXVK d3d9_device.cpp:1878-1898, wined3d d3d9 device.c:2315):
//   - First BeginScene → S_OK, sets in_scene
//   - Nested BeginScene without intervening EndScene → INVALIDCALL
//   - EndScene with no prior BeginScene → INVALIDCALL
//   - Second EndScene without re-Begin → INVALIDCALL
//   - Begin/End pair re-entrance after a clean cycle works

#include "../dx9_smoke.h"
void
test_begin_end_scene(void) {
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

  // ---- EndScene before BeginScene → reject. ----
  HRESULT hr = dev->EndScene();
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- First BeginScene. ----
  hr = dev->BeginScene();
  check_hr(hr);

  // ---- Nested BeginScene → reject (no state change). ----
  hr = dev->BeginScene();
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- EndScene closes the bracket. ----
  hr = dev->EndScene();
  check_hr(hr);

  // ---- Second EndScene without re-Begin → reject. ----
  hr = dev->EndScene();
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- Re-entrance: a fresh Begin/End cycle works. ----
  hr = dev->BeginScene();
  check_hr(hr);
  hr = dev->EndScene();
  check_hr(hr);

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
