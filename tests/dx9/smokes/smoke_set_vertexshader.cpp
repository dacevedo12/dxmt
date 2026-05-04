// CreateVertexShader / SetVertexShader / GetVertexShader smoke.
// Validates: bytecode round-trip via GetFunction (NULL-buf size
// query, sized-buf copy, and undersized-buf rejection), the
// scan-to-END length helper across a hand-authored vs_2_0 blob
// containing a comment with embedded 0xFFFF bit patterns, the
// bind round-trip + cross-device rejection, and the priv-pin
// lifetime probe.
//
// Bytecode layout we feed CreateVertexShader (vs_2_0):
//   [0] 0xFFFE0200      version: vs_2_0
//   [1] 0x0001FFFE      comment header, 1 DWORD body
//   [2] 0x0000FFFF      comment body — a 0xFFFF bit pattern that
//                       MUST NOT be misread as the END token. This
//                       is the explicit regression test for
//                       shader_bytecode_dword_count's comment-skip.
//   [3] 0x0000FFFF      D3DSIO_END
// dword_count = 4, byte_length = 16.

#include "../dx9_smoke.h"
static const DWORD vs_blob[] = {
    0xFFFE0200u, // vs_2_0 header
    0x0001FFFEu, // comment, 1 DWORD body
    0x0000FFFFu, // body — looks like END, must be skipped
    0x0000FFFFu, // real D3DSIO_END
};

void
test_set_vertexshader(void) {
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

  // Initial state: nothing bound.
  IDirect3DVertexShader9 *got = NULL;
  HRESULT hr = dev->GetVertexShader(&got);
  printf("GetVertexShader initial: hr=0x%08lx out=%s\n", (unsigned long)hr, got ? "non-null" : "null");
  if (got)
    got->Release();

  // ---- InitReturnPtr on Create(NULL bytecode). ----
  IDirect3DVertexShader9 *bad_out = (IDirect3DVertexShader9 *)1;
  hr = dev->CreateVertexShader(NULL, &bad_out);
  printf(
      "CreateVertexShader(NULL, _): hr=0x%08lx out=%s "
      "(InitReturnPtr must zero)\n",
      (unsigned long)hr, bad_out ? "non-null" : "null"
  );

  // ---- Create. ----
  IDirect3DVertexShader9 *vs = NULL;
  HRESULT cv = dev->CreateVertexShader(vs_blob, &vs);
  printf("CreateVertexShader: hr=0x%08lx out=%s\n", (unsigned long)cv, vs ? "ok" : "null");
  if (FAILED(cv) || !vs) {
    dev->Release();
    d3d->Release();
    return;
  }

  // ---- Length-only query via GetFunction(NULL, &size). ----
  UINT need = 0xdeadbeef;
  hr = vs->GetFunction(NULL, &need);
  printf("GetFunction(NULL, &size): hr=0x%08lx size=%u (want 16)\n", (unsigned long)hr, need);

  // ---- Full readback. wined3d/DXVK leave *pSizeOfData unchanged on
  // success-with-buffer — we match. ----
  DWORD readback[8] = {};
  UINT size = sizeof(readback);
  hr = vs->GetFunction(readback, &size);
  printf("GetFunction(readback): hr=0x%08lx size=%u (unchanged)\n", (unsigned long)hr, size);
  printf(
      "  [0]=0x%08lx [1]=0x%08lx [2]=0x%08lx [3]=0x%08lx\n", (unsigned long)readback[0], (unsigned long)readback[1],
      (unsigned long)readback[2], (unsigned long)readback[3]
  );

  // ---- Undersized buffer rejection. ----
  DWORD tiny[1] = {};
  size = sizeof(tiny);
  hr = vs->GetFunction(tiny, &size);
  printf("GetFunction(tiny=4 bytes, need=16): hr=0x%08lx (must reject)\n", (unsigned long)hr);

  // ---- NULL pSizeOfData. ----
  hr = vs->GetFunction(readback, NULL);
  printf("GetFunction(_, NULL): hr=0x%08lx\n", (unsigned long)hr);

  // ---- Set/Get round-trip. ----
  HRESULT s = dev->SetVertexShader(vs);
  printf("SetVertexShader(vs): hr=0x%08lx\n", (unsigned long)s);
  got = NULL;
  dev->GetVertexShader(&got);
  printf("GetVertexShader: same=%s\n", got == vs ? "yes" : "no");
  if (got)
    got->Release();

  // Unbind.
  s = dev->SetVertexShader(NULL);
  printf("SetVertexShader(NULL): hr=0x%08lx\n", (unsigned long)s);
  got = (IDirect3DVertexShader9 *)0xdeadbeef;
  dev->GetVertexShader(&got);
  printf("GetVertexShader post-unbind: out=%s\n", got ? "non-null" : "null");

  // ---- Cross-device rejection. ----
  IDirect3DDevice9 *dev2 = NULL;
  HRESULT cd2 = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev2);
  printf("CreateDevice(2): hr=0x%08lx\n", (unsigned long)cd2);
  if (SUCCEEDED(cd2) && dev2) {
    IDirect3DVertexShader9 *foreign = NULL;
    dev2->CreateVertexShader(vs_blob, &foreign);
    if (foreign) {
      HRESULT bad = dev->SetVertexShader(foreign);
      printf("SetVertexShader(foreign): hr=0x%08lx\n", (unsigned long)bad);
      foreign->Release();
    }
    dev2->Release();
  }

  // ---- Lifetime: release-while-bound. ----
  dev->SetVertexShader(vs);
  ULONG vs_ref = vs->Release();
  printf("Release(vs) while bound: %lu (slot keeps it alive)\n", (unsigned long)vs_ref);
  got = NULL;
  HRESULT still = dev->GetVertexShader(&got);
  printf("GetVertexShader post-release: hr=0x%08lx ptr_match=%s\n", (unsigned long)still, got == vs ? "yes" : "no");
  if (got)
    got->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
