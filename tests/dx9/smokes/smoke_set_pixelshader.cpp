// CreatePixelShader / SetPixelShader / GetPixelShader smoke. The
// shader_bytecode_dword_count comment-skip path is already covered
// by smoke_set_vertexshader; this smoke focuses on the ps-side
// vtable contract (Create + Set/Get round-trip + lifetime).
//
// We deliberately don't include a "malformed bytecode without END"
// test here: the scanner is documented (in d3d9_shader.cpp's
// kMaxScanDwords comment) as having an unbounded-OOB-read window
// when the blob has no terminator — D3D9's API gives no length, so
// wined3d / DXVK have the same property via their full DXSO
// walkers. Real apps never pass malformed bytecode; a fuzz-style
// smoke would only be a regression test for the limitation, not a
// useful behavioural assertion.

#include "../dx9_smoke.h"
// Minimal ps_2_0 blob — header + END.
static const DWORD ps_blob[] = {
    0xFFFF0200u, // ps_2_0 header
    0x0000FFFFu, // D3DSIO_END
};

void
test_set_pixelshader(void) {
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

  // Initial state.
  IDirect3DPixelShader9 *got = NULL;
  HRESULT hr = dev->GetPixelShader(&got);
  printf("GetPixelShader initial: hr=0x%08lx out=%s\n", (unsigned long)hr, got ? "non-null" : "null");
  if (got)
    got->Release();

  // ---- InitReturnPtr: NULL bytecode → out is zeroed before reject.
  IDirect3DPixelShader9 *bad_out = (IDirect3DPixelShader9 *)1;
  hr = dev->CreatePixelShader(NULL, &bad_out);
  printf("CreatePixelShader(NULL, _): hr=0x%08lx out=%s\n", (unsigned long)hr, bad_out ? "non-null" : "null");

  // ---- Create a real one. ----
  IDirect3DPixelShader9 *ps = NULL;
  hr = dev->CreatePixelShader(ps_blob, &ps);
  printf("CreatePixelShader: hr=0x%08lx out=%s\n", (unsigned long)hr, ps ? "ok" : "null");
  if (FAILED(hr) || !ps) {
    dev->Release();
    d3d->Release();
    return;
  }

  // GetFunction round-trip.
  UINT need = 0;
  ps->GetFunction(NULL, &need);
  printf("GetFunction(NULL, &size): size=%u (want 8)\n", need);

  DWORD readback[4] = {};
  UINT size = sizeof(readback);
  hr = ps->GetFunction(readback, &size);
  printf(
      "GetFunction(readback): hr=0x%08lx [0]=0x%08lx [1]=0x%08lx\n", (unsigned long)hr, (unsigned long)readback[0],
      (unsigned long)readback[1]
  );

  // Set/Get round-trip.
  HRESULT s = dev->SetPixelShader(ps);
  printf("SetPixelShader(ps): hr=0x%08lx\n", (unsigned long)s);
  got = NULL;
  dev->GetPixelShader(&got);
  printf("GetPixelShader: same=%s\n", got == ps ? "yes" : "no");
  if (got)
    got->Release();

  // Unbind.
  dev->SetPixelShader(NULL);
  got = (IDirect3DPixelShader9 *)0xdeadbeef;
  dev->GetPixelShader(&got);
  printf("GetPixelShader post-unbind: out=%s\n", got ? "non-null" : "null");

  // ---- Cross-device rejection. ----
  IDirect3DDevice9 *dev2 = NULL;
  HRESULT cd2 = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev2);
  printf("CreateDevice(2): hr=0x%08lx\n", (unsigned long)cd2);
  if (SUCCEEDED(cd2) && dev2) {
    IDirect3DPixelShader9 *foreign = NULL;
    dev2->CreatePixelShader(ps_blob, &foreign);
    if (foreign) {
      HRESULT bad_dev = dev->SetPixelShader(foreign);
      printf("SetPixelShader(foreign): hr=0x%08lx\n", (unsigned long)bad_dev);
      foreign->Release();
    }
    dev2->Release();
  }

  // ---- Lifetime: release-while-bound. ----
  dev->SetPixelShader(ps);
  ULONG ps_ref = ps->Release();
  printf("Release(ps) while bound: %lu (slot keeps it alive)\n", (unsigned long)ps_ref);
  got = NULL;
  HRESULT still = dev->GetPixelShader(&got);
  printf("GetPixelShader post-release: hr=0x%08lx ptr_match=%s\n", (unsigned long)still, got == ps ? "yes" : "no");
  if (got)
    got->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
