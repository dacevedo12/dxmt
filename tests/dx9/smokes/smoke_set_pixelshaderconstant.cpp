// SetPixelShaderConstantF / I / B + Get counterparts.
//
// PS constant register file sizes (DXVK d3d9_caps.h):
//   F: 224 vec4 floats  (c0..c223 — SM3 max; SM2 only addresses c0..c31)
//   I: 16  vec4 ints
//   B: 16  bools
//
// Mirror of smoke_set_vertexshaderconstant; the only behavioural
// difference we test for is the F-bound at 224 vs 256. We also assert
// PS and VS constant slots are independent — a Set on the VS side
// must not surface in PS Get.

#include "../dx9_smoke.h"
void
test_set_pixelshaderconstant(void) {
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

  // ---- Default-zero readback. ----
  float fr[4] = {1, 2, 3, 4};
  HRESULT hr = dev->GetPixelShaderConstantF(0, fr, 1);
  printf("GetF(c0) initial: hr=0x%08lx [%g %g %g %g] (want 0 0 0 0)\n", (unsigned long)hr, fr[0], fr[1], fr[2], fr[3]);

  int ir[4] = {1, 2, 3, 4};
  hr = dev->GetPixelShaderConstantI(0, ir, 1);
  printf("GetI(i0) initial: hr=0x%08lx [%d %d %d %d] (want 0 0 0 0)\n", (unsigned long)hr, ir[0], ir[1], ir[2], ir[3]);

  BOOL br[4] = {TRUE, TRUE, TRUE, TRUE};
  hr = dev->GetPixelShaderConstantB(0, br, 4);
  printf(
      "GetB(b0..b3) initial: hr=0x%08lx [%d %d %d %d] (want 0 0 0 0)\n", (unsigned long)hr, br[0], br[1], br[2], br[3]
  );

  // ---- Float round-trip. ----
  float fw[12] = {0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f, 10.5f, 11.5f};
  hr = dev->SetPixelShaderConstantF(5, fw, 3);
  printf("SetF(c5..c7): hr=0x%08lx\n", (unsigned long)hr);

  float fb[12] = {};
  hr = dev->GetPixelShaderConstantF(5, fb, 3);
  printf("GetF(c5..c7): hr=0x%08lx match=%s\n", (unsigned long)hr, memcmp(fb, fw, sizeof(fw)) == 0 ? "yes" : "no");

  // ---- Int round-trip. ----
  int iw[8] = {-1, -2, -3, -4, 100, 200, 300, 400};
  hr = dev->SetPixelShaderConstantI(2, iw, 2);
  printf("SetI(i2..i3): hr=0x%08lx\n", (unsigned long)hr);

  int ib[8] = {};
  hr = dev->GetPixelShaderConstantI(2, ib, 2);
  printf("GetI(i2..i3): hr=0x%08lx match=%s\n", (unsigned long)hr, memcmp(ib, iw, sizeof(iw)) == 0 ? "yes" : "no");

  // ---- Bool round-trip with normalize-on-store. ----
  BOOL bw[4] = {FALSE, TRUE, 7, (BOOL)-3};
  hr = dev->SetPixelShaderConstantB(0, bw, 4);
  printf("SetB(b0..b3 = {0, TRUE, 7, -3}): hr=0x%08lx\n", (unsigned long)hr);

  BOOL bb[4] = {};
  hr = dev->GetPixelShaderConstantB(0, bb, 4);
  printf("GetB(b0..b3): hr=0x%08lx [%d %d %d %d] (want 0 1 1 1)\n", (unsigned long)hr, bb[0], bb[1], bb[2], bb[3]);

  // ---- PS / VS storage independence: a write to the VS side must not
  // bleed into PS slots. ----
  float vs_only[4] = {99.0f, 99.0f, 99.0f, 99.0f};
  dev->SetVertexShaderConstantF(50, vs_only, 1);
  float ps_check[4] = {};
  dev->GetPixelShaderConstantF(50, ps_check, 1);
  printf(
      "PS c50 after VS-only write: [%g %g %g %g] (want 0 0 0 0)\n", ps_check[0], ps_check[1], ps_check[2], ps_check[3]
  );

  // ---- F bound at 224 (vs 256 for VS). ----
  hr = dev->SetPixelShaderConstantF(223, fw, 1);
  check_hr(hr);
  hr = dev->SetPixelShaderConstantF(223, fw, 2);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  hr = dev->SetPixelShaderConstantF(224, fw, 0);
  check_hr(hr);
  hr = dev->SetPixelShaderConstantF(224, fw, 1);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  // VS-side limit still applies separately — c224..c255 are valid VS
  // slots but invalid PS slots.
  hr = dev->SetVertexShaderConstantF(224, fw, 1);
  check_hr(hr);

  // ---- I / B range checks. ----
  hr = dev->SetPixelShaderConstantI(15, iw, 2);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  hr = dev->SetPixelShaderConstantB(15, bw, 2);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- UINT overflow + NULL-data shape. ----
  hr = dev->SetPixelShaderConstantF(0xFFFFFFFEu, fw, 4);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  hr = dev->SetPixelShaderConstantF(0, NULL, 1);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  hr = dev->SetPixelShaderConstantF(0, NULL, 0);
  printf("SetF(NULL, 0): hr=0x%08lx (want D3D_OK no-op)\n", (unsigned long)hr);

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
