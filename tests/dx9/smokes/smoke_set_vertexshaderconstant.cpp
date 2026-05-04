// SetVertexShaderConstantF / I / B + Get counterparts.
//
// D3D9 hardware-VP register file sizes (DXVK d3d9_caps.h, wined3d
// d3d9_private.h):
//   F: 256 vec4 floats  (c0..c255)
//   I: 16  vec4 ints    (i0..i15)
//   B: 16  bools        (b0..b15)
//
// Validation we exercise (DXVK SetShaderConstants /
// GetShaderConstants ordering):
//   1. StartRegister + Count overflows UINT — INVALIDCALL
//   2. StartRegister + Count > regfile size — INVALIDCALL
//   3. Count == 0 — D3D_OK no-op (skips null check)
//   4. pConstantData == NULL with Count > 0 — INVALIDCALL
//   5. round-trip across all three slot types
//   6. default-zero readback before any Set
//   7. partial reads / writes
//
// We don't smoke-test against a SetVertexShader binding here — these
// are independent state. The constants live on the device and persist
// across shader rebinds.

#include "../dx9_smoke.h"
void
test_set_vertexshaderconstant(void) {
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

  // ---- Default-zero readback before any Set. ----
  float fr[4] = {1.0f, 2.0f, 3.0f, 4.0f};
  HRESULT hr = dev->GetVertexShaderConstantF(0, fr, 1);
  printf("GetF(c0) initial: hr=0x%08lx [%g %g %g %g] (want 0 0 0 0)\n", (unsigned long)hr, fr[0], fr[1], fr[2], fr[3]);

  int ir[4] = {1, 2, 3, 4};
  hr = dev->GetVertexShaderConstantI(0, ir, 1);
  printf("GetI(i0) initial: hr=0x%08lx [%d %d %d %d] (want 0 0 0 0)\n", (unsigned long)hr, ir[0], ir[1], ir[2], ir[3]);

  BOOL br[4] = {TRUE, TRUE, TRUE, TRUE};
  hr = dev->GetVertexShaderConstantB(0, br, 4);
  printf(
      "GetB(b0..b3) initial: hr=0x%08lx [%d %d %d %d] (want 0 0 0 0)\n", (unsigned long)hr, br[0], br[1], br[2], br[3]
  );

  // ---- Float round-trip: write c5..c7, read back. ----
  float fw[12] = {0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 6.5f, 7.5f, 8.5f, 9.5f, 10.5f, 11.5f};
  hr = dev->SetVertexShaderConstantF(5, fw, 3);
  printf("SetF(c5..c7): hr=0x%08lx\n", (unsigned long)hr);

  float fb[12] = {};
  hr = dev->GetVertexShaderConstantF(5, fb, 3);
  printf("GetF(c5..c7): hr=0x%08lx match=%s\n", (unsigned long)hr, memcmp(fb, fw, sizeof(fw)) == 0 ? "yes" : "no");

  // Neighbouring slots untouched.
  float fneigh[8] = {};
  hr = dev->GetVertexShaderConstantF(3, fneigh, 2);
  printf(
      "GetF(c3..c4) untouched: hr=0x%08lx [%g %g %g %g | %g %g %g %g]\n", (unsigned long)hr, fneigh[0], fneigh[1],
      fneigh[2], fneigh[3], fneigh[4], fneigh[5], fneigh[6], fneigh[7]
  );

  // ---- Int round-trip: write i2..i3. ----
  int iw[8] = {-1, -2, -3, -4, 100, 200, 300, 400};
  hr = dev->SetVertexShaderConstantI(2, iw, 2);
  printf("SetI(i2..i3): hr=0x%08lx\n", (unsigned long)hr);

  int ib[8] = {};
  hr = dev->GetVertexShaderConstantI(2, ib, 2);
  printf("GetI(i2..i3): hr=0x%08lx match=%s\n", (unsigned long)hr, memcmp(ib, iw, sizeof(iw)) == 0 ? "yes" : "no");

  // ---- Bool round-trip: normalize-on-store. We pass mixed values
  // (0, 1, 7, -3) and expect everything non-zero to read back as TRUE. -
  BOOL bw[4] = {FALSE, TRUE, 7, (BOOL)-3};
  hr = dev->SetVertexShaderConstantB(0, bw, 4);
  printf("SetB(b0..b3 = {0, TRUE, 7, -3}): hr=0x%08lx\n", (unsigned long)hr);

  BOOL bb[4] = {};
  hr = dev->GetVertexShaderConstantB(0, bb, 4);
  printf(
      "GetB(b0..b3): hr=0x%08lx [%d %d %d %d] "
      "(want 0 1 1 1 — normalized)\n",
      (unsigned long)hr, bb[0], bb[1], bb[2], bb[3]
  );

  // ---- Partial overlap: write c5..c10 with one pattern then c8..c12
  // with another. Read c5..c12 back; c5..c7 must keep the first
  // pattern, c8..c12 must hold the second. Catches accidental
  // memcpy-clobber-the-tail bugs. ----
  float fa[6 * 4];
  for (int i = 0; i < 24; ++i)
    fa[i] = (float)(100 + i);
  dev->SetVertexShaderConstantF(5, fa, 6);
  float fbov[5 * 4];
  for (int i = 0; i < 20; ++i)
    fbov[i] = (float)(900 + i);
  dev->SetVertexShaderConstantF(8, fbov, 5);
  float fck[8 * 4] = {};
  dev->GetVertexShaderConstantF(5, fck, 8);
  // c5..c7 = 100..111 (first), c8..c12 = 900..919 (second)
  printf(
      "PartialOverlap: c5=%g c7=%g c8=%g c12=%g "
      "(want 100 111 900 919)\n",
      fck[0], fck[11], fck[12], fck[31]
  );

  // ---- Range checks. ----
  hr = dev->SetVertexShaderConstantF(255, fw, 1); // 255+1=256 == limit, ok
  check_hr(hr);

  hr = dev->SetVertexShaderConstantF(255, fw, 2); // 255+2=257 > 256
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  hr = dev->SetVertexShaderConstantF(256, fw, 0); // 256+0 == 256, ok-no-op
  check_hr(hr);

  hr = dev->SetVertexShaderConstantI(15, iw, 2);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  hr = dev->SetVertexShaderConstantB(15, bw, 2);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- UINT overflow on StartRegister + Count. ----
  hr = dev->SetVertexShaderConstantF(0xFFFFFFFEu, fw, 4);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- NULL data with Count > 0. ----
  hr = dev->SetVertexShaderConstantF(0, NULL, 1);
  check_hr_eq(hr, D3DERR_INVALIDCALL);
  hr = dev->GetVertexShaderConstantF(0, NULL, 1);
  check_hr_eq(hr, D3DERR_INVALIDCALL);

  // ---- NULL data with Count == 0 — no-op, must succeed. ----
  hr = dev->SetVertexShaderConstantF(0, NULL, 0);
  printf("SetF(NULL, 0): hr=0x%08lx (want D3D_OK no-op)\n", (unsigned long)hr);
  hr = dev->GetVertexShaderConstantF(0, NULL, 0);
  printf("GetF(NULL, 0): hr=0x%08lx (want D3D_OK no-op)\n", (unsigned long)hr);

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
