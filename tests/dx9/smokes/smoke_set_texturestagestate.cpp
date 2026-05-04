// SetTextureStageState / GetTextureStageState smoke. FFP texture-
// blend operations: stage 0..7, type D3DTSS_COLOROP..D3DTSS_CONSTANT.
// Validates the D3D9 contracts the runtime enforces:
//
//   - Round-trip Set/Get on every legal stage (0..7).
//   - Cross-stage isolation: changing stage 3 doesn't leak into stage 0.
//   - Out-of-range stage (>=8) returns INVALIDCALL on both Set and Get.
//   - Out-of-range type (0 or > D3DTSS_CONSTANT) returns INVALIDCALL.
//   - GetTextureStageState with NULL pValue returns INVALIDCALL.
//
// LoL hits SetTextureStageState even with a programmable PS bound;
// the prior STUB_HR returned E_NOTIMPL and tripped apps that don't
// hr-check the call. Storing-and-OK is wined3d / DXVK's shape.

#include "../dx9_smoke.h"
void
test_set_texturestagestate(void) {
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

  // Round-trip on stage 0.
  HRESULT s = dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
  printf("Set(0, COLOROP=MODULATE): hr=0x%08lx\n", (unsigned long)s);
  DWORD v = 0;
  HRESULT hr = dev->GetTextureStageState(0, D3DTSS_COLOROP, &v);
  printf(
      "Get(0, COLOROP): hr=0x%08lx v=%lu match=%s\n", (unsigned long)hr, (unsigned long)v,
      v == (DWORD)D3DTOP_MODULATE ? "yes" : "no"
  );

  // Round-trip on highest legal stage (7) — locks the array bound.
  s = dev->SetTextureStageState(7, D3DTSS_COLORARG1, D3DTA_TEXTURE);
  printf("Set(7, COLORARG1=TEXTURE): hr=0x%08lx\n", (unsigned long)s);
  v = 0;
  hr = dev->GetTextureStageState(7, D3DTSS_COLORARG1, &v);
  printf(
      "Get(7, COLORARG1): hr=0x%08lx v=%lu match=%s\n", (unsigned long)hr, (unsigned long)v,
      v == (DWORD)D3DTA_TEXTURE ? "yes" : "no"
  );

  // Cross-stage isolation.
  v = 0xdeadbeef;
  hr = dev->GetTextureStageState(3, D3DTSS_COLOROP, &v);
  printf("Get(3, COLOROP) after Set(0): hr=0x%08lx v=%lu (want 0)\n", (unsigned long)hr, (unsigned long)v);

  // D3DTSS_CONSTANT (32) — top of enum.
  s = dev->SetTextureStageState(0, D3DTSS_CONSTANT, 0xCAFEF00D);
  printf("Set(0, CONSTANT=0xCAFEF00D): hr=0x%08lx\n", (unsigned long)s);
  v = 0;
  hr = dev->GetTextureStageState(0, D3DTSS_CONSTANT, &v);
  printf(
      "Get(0, CONSTANT): hr=0x%08lx v=0x%08lx match=%s\n", (unsigned long)hr, (unsigned long)v,
      v == 0xCAFEF00Du ? "yes" : "no"
  );

  // Out-of-range stage — INVALIDCALL.
  s = dev->SetTextureStageState(8, D3DTSS_COLOROP, 0);
  printf("Set(8, COLOROP): hr=0x%08lx (must reject)\n", (unsigned long)s);
  v = 0xdeadbeef;
  hr = dev->GetTextureStageState(8, D3DTSS_COLOROP, &v);
  printf("Get(8, COLOROP): hr=0x%08lx v=%lu\n", (unsigned long)hr, (unsigned long)v);

  // Out-of-enum type — INVALIDCALL.
  s = dev->SetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)0, 0);
  printf("Set(0, type=0): hr=0x%08lx (must reject)\n", (unsigned long)s);
  s = dev->SetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)0xff, 0);
  printf("Set(0, type=0xff): hr=0x%08lx (must reject)\n", (unsigned long)s);
  v = 0xdeadbeef;
  hr = dev->GetTextureStageState(0, (D3DTEXTURESTAGESTATETYPE)0xff, &v);
  printf("Get(0, type=0xff): hr=0x%08lx v=%lu\n", (unsigned long)hr, (unsigned long)v);

  // NULL pValue.
  hr = dev->GetTextureStageState(0, D3DTSS_COLOROP, NULL);
  printf("Get(0, COLOROP, NULL): hr=0x%08lx\n", (unsigned long)hr);

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
