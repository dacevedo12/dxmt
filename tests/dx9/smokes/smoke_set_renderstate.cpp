// SetRenderState / GetRenderState smoke. Validates: D3D9-spec
// defaults at CreateDevice (a sample of the float-encoded ones
// caught by Get's DWORD return), the live-storage range
// (0,7..255 store; 1..6 and 256+ no-op), simple round-trip, and
// EnableAutoDepthStencil influencing D3DRS_ZENABLE default.

#include "../dx9_smoke.h"
static void
check_defaults_basic(IDirect3DDevice9 *dev) {
  DWORD v = 0xdeadbeef;
  HRESULT hr = dev->GetRenderState(D3DRS_ZFUNC, &v);
  printf(
      "Default ZFUNC: hr=0x%08lx v=%lu (want %d=LESSEQUAL)\n", (unsigned long)hr, (unsigned long)v, D3DCMP_LESSEQUAL
  );

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_CULLMODE, &v);
  printf("Default CULLMODE: hr=0x%08lx v=%lu (want %d=CCW)\n", (unsigned long)hr, (unsigned long)v, D3DCULL_CCW);

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_ALPHAFUNC, &v);
  printf("Default ALPHAFUNC: hr=0x%08lx v=%lu (want %d=ALWAYS)\n", (unsigned long)hr, (unsigned long)v, D3DCMP_ALWAYS);

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_COLORWRITEENABLE, &v);
  printf("Default COLORWRITEENABLE: hr=0x%08lx v=0x%lx (want 0xf)\n", (unsigned long)hr, (unsigned long)v);

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_COLORWRITEENABLE3, &v);
  printf("Default COLORWRITEENABLE3: hr=0x%08lx v=0x%lx (want 0xf)\n", (unsigned long)hr, (unsigned long)v);

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_BLENDFACTOR, &v);
  printf("Default BLENDFACTOR: hr=0x%08lx v=0x%lx (want 0xffffffff)\n", (unsigned long)hr, (unsigned long)v);

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_LIGHTING, &v);
  printf("Default LIGHTING: hr=0x%08lx v=%lu (want 1=TRUE)\n", (unsigned long)hr, (unsigned long)v);

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_FOGENABLE, &v);
  printf("Default FOGENABLE: hr=0x%08lx v=%lu (want 0=FALSE)\n", (unsigned long)hr, (unsigned long)v);

  // Float-encoded defaults — IEEE-754 bits.
  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_POINTSIZE, &v);
  printf("Default POINTSIZE: hr=0x%08lx v=0x%lx (want 0x3f800000=1.0f)\n", (unsigned long)hr, (unsigned long)v);

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_FOGEND, &v);
  printf("Default FOGEND: hr=0x%08lx v=0x%lx (want 0x3f800000=1.0f)\n", (unsigned long)hr, (unsigned long)v);

  v = 0xdeadbeef;
  hr = dev->GetRenderState(D3DRS_DEPTHBIAS, &v);
  printf("Default DEPTHBIAS: hr=0x%08lx v=0x%lx (want 0=0.0f)\n", (unsigned long)hr, (unsigned long)v);
}

void
test_set_renderstate(void) {
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
  // First device: auto-DS off → ZENABLE default = D3DZB_FALSE.
  pp.EnableAutoDepthStencil = FALSE;

  IDirect3DDevice9 *dev = NULL;
  HRESULT cdhr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  printf("CreateDevice (no auto-DS): hr=0x%08lx\n", (unsigned long)cdhr);
  if (FAILED(cdhr) || !dev) {
    d3d->Release();
    return;
  }

  DWORD v = 0xdeadbeef;
  HRESULT hr = dev->GetRenderState(D3DRS_ZENABLE, &v);
  printf(
      "Default ZENABLE (no auto-DS): hr=0x%08lx v=%lu (want %d=FALSE)\n", (unsigned long)hr, (unsigned long)v,
      D3DZB_FALSE
  );

  check_defaults_basic(dev);

  // Round-trip.
  HRESULT s = dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  printf("SetRenderState(CULLMODE, NONE): hr=0x%08lx\n", (unsigned long)s);
  v = 0;
  hr = dev->GetRenderState(D3DRS_CULLMODE, &v);
  printf(
      "GetRenderState(CULLMODE): hr=0x%08lx v=%lu match=%s\n", (unsigned long)hr, (unsigned long)v,
      v == (DWORD)D3DCULL_NONE ? "yes" : "no"
  );

  // Cross-state isolation — Set CULLMODE shouldn't perturb FILLMODE.
  v = 0;
  dev->GetRenderState(D3DRS_FILLMODE, &v);
  printf("GetRenderState(FILLMODE) after CULLMODE: v=%lu (still SOLID=%d)\n", (unsigned long)v, D3DFILL_SOLID);

  // ---- D3D8/DX7 holdover stages (1..6) — Set silently no-ops with
  // D3D_OK, Get returns INVALIDCALL (DXVK contract). ----
  s = dev->SetRenderState((D3DRENDERSTATETYPE)1, 0xdeadbeef);
  printf("SetRenderState(1, 0xdeadbeef): hr=0x%08lx (no-op)\n", (unsigned long)s);
  v = 0xfeedface;
  hr = dev->GetRenderState((D3DRENDERSTATETYPE)1, &v);
  printf("GetRenderState(1): hr=0x%08lx (must be INVALIDCALL)\n", (unsigned long)hr);

  // ---- 256+ also: Set no-ops, Get is INVALIDCALL. ----
  s = dev->SetRenderState((D3DRENDERSTATETYPE)256, 1);
  printf("SetRenderState(256, 1): hr=0x%08lx (no-op)\n", (unsigned long)s);
  v = 0xfeedface;
  hr = dev->GetRenderState((D3DRENDERSTATETYPE)256, &v);
  printf("GetRenderState(256): hr=0x%08lx (must be INVALIDCALL)\n", (unsigned long)hr);

  // ---- State 0 quirk: live-storage slot, but Get always reads 0
  // (DXVK contract — asymmetric with Set). ----
  s = dev->SetRenderState((D3DRENDERSTATETYPE)0, 0x1234);
  printf("SetRenderState(0, 0x1234): hr=0x%08lx\n", (unsigned long)s);
  v = 0xfeedface;
  hr = dev->GetRenderState((D3DRENDERSTATETYPE)0, &v);
  printf("GetRenderState(0): hr=0x%08lx v=0x%lx (want 0 — quirk)\n", (unsigned long)hr, (unsigned long)v);

  // ---- Float-encoded round-trip: write a real bit pattern and
  // confirm we read it back unchanged. ----
  const float bias = -1.5e-5f;
  DWORD bias_dword;
  __builtin_memcpy(&bias_dword, &bias, sizeof(bias_dword));
  dev->SetRenderState(D3DRS_DEPTHBIAS, bias_dword);
  v = 0;
  dev->GetRenderState(D3DRS_DEPTHBIAS, &v);
  printf(
      "Round-trip DEPTHBIAS bits: set=0x%lx got=0x%lx match=%s\n", (unsigned long)bias_dword, (unsigned long)v,
      v == bias_dword ? "yes" : "no"
  );

  // ---- Null pointer. ----
  hr = dev->GetRenderState(D3DRS_ZFUNC, NULL);
  printf("GetRenderState(ZFUNC, NULL): hr=0x%08lx\n", (unsigned long)hr);

  dev->Release();

  // Second device: auto-DS on → ZENABLE default = D3DZB_TRUE.
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;
  IDirect3DDevice9 *dev2 = NULL;
  cdhr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev2);
  printf("CreateDevice (auto-DS=TRUE): hr=0x%08lx\n", (unsigned long)cdhr);
  if (SUCCEEDED(cdhr) && dev2) {
    v = 0xdeadbeef;
    hr = dev2->GetRenderState(D3DRS_ZENABLE, &v);
    printf(
        "Default ZENABLE (auto-DS=TRUE): hr=0x%08lx v=%lu (want %d=TRUE)\n", (unsigned long)hr, (unsigned long)v,
        D3DZB_TRUE
    );
    dev2->Release();
  }

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
