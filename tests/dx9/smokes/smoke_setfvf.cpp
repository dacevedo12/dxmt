// SetFVF / GetFVF round-trip. LoL hits SetFVF(0) 13k/match — the
// "programmable-pipeline marker" call the app issues alongside
// SetVertexDeclaration. The 0-FVF case is a no-op semantically but
// the call must succeed so the app's hr-check path doesn't bail.

#include "../dx9_smoke.h"
void
test_setfvf(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("Direct3DCreate9: NULL\n");
    return;
  }

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
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

  // Default FVF = 0 (no FVF set).
  DWORD f0 = 0xdeadbeefu;
  HRESULT g0 = dev->GetFVF(&f0);
  printf("GetFVF(default): hr=0x%08lx fvf=0x%lx (expect 0)\n", (unsigned long)g0, (unsigned long)f0);

  // LoL hot path: SetFVF(0).
  HRESULT s0 = dev->SetFVF(0);
  printf("SetFVF(0): hr=0x%08lx\n", (unsigned long)s0);
  DWORD f1 = 0xdeadbeefu;
  dev->GetFVF(&f1);
  printf("  GetFVF after SetFVF(0): fvf=0x%lx (expect 0)\n", (unsigned long)f1);

  // Round-trip a non-zero FVF — accepted but draw-time conversion
  // not wired. D3DFVF_XYZ | D3DFVF_DIFFUSE = 0x42 (a common shape).
  HRESULT s1 = dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
  printf("SetFVF(XYZ|DIFFUSE): hr=0x%08lx\n", (unsigned long)s1);
  DWORD f2 = 0;
  dev->GetFVF(&f2);
  printf(
      "  GetFVF after SetFVF(XYZ|DIFFUSE): fvf=0x%lx (expect 0x%lx)\n", (unsigned long)f2,
      (unsigned long)(D3DFVF_XYZ | D3DFVF_DIFFUSE)
  );

  // Cycle back to 0.
  dev->SetFVF(0);
  DWORD f3 = 0xdeadbeefu;
  dev->GetFVF(&f3);
  printf("  GetFVF after SetFVF(0) again: fvf=0x%lx (expect 0)\n", (unsigned long)f3);

  // GetFVF(NULL) must reject (out param contract).
  HRESULT g_null = dev->GetFVF(NULL);
  printf("GetFVF(NULL): hr=0x%08lx (must reject)\n", (unsigned long)g_null);

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
