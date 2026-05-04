// DXSO header validation — first piece of the D3D9 shader-compilation
// path. CreateVertexShader / CreatePixelShader now reject bytecode
// whose version token doesn't match the requested kind, before the
// shader handle is handed out and before any later AIR-emit pass
// tries to walk a malformed blob.
//
// Layout of the version token (DXVK dxso_common.h):
//   bits 31..16: 0xFFFE (vertex) or 0xFFFF (pixel)
//   bits 15..8:  major version
//   bits 7..0:   minor version

#include "../dx9_smoke.h"
// END token marks the end of the bytecode. shader_bytecode_dword_count
// in the device walks from index 1 and stops at this value.
#define DXSO_END 0x0000FFFFu
// Trivial shaders: header DWORD + END. The bytecode-length walker
// accepts this because END is at index 1.
#define MAKE_VS(major, minor) (0xFFFE0000u | ((uint32_t)(major) << 8) | (uint32_t)(minor))
#define MAKE_PS(major, minor) (0xFFFF0000u | ((uint32_t)(major) << 8) | (uint32_t)(minor))

static HRESULT
try_create_vs(IDirect3DDevice9 *dev, DWORD header_dword) {
  DWORD blob[2] = {header_dword, DXSO_END};
  IDirect3DVertexShader9 *vs = NULL;
  HRESULT hr = dev->CreateVertexShader(blob, &vs);
  if (vs)
    vs->Release();
  return hr;
}

static HRESULT
try_create_ps(IDirect3DDevice9 *dev, DWORD header_dword) {
  DWORD blob[2] = {header_dword, DXSO_END};
  IDirect3DPixelShader9 *ps = NULL;
  HRESULT hr = dev->CreatePixelShader(blob, &ps);
  if (ps)
    ps->Release();
  return hr;
}

void
test_dxso_header(void) {
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

  // ---- Accepted headers. ----
  printf("CreateVertexShader(vs_1_0): hr=0x%08lx\n", (unsigned long)try_create_vs(dev, MAKE_VS(1, 0)));
  printf("CreateVertexShader(vs_1_1): hr=0x%08lx\n", (unsigned long)try_create_vs(dev, MAKE_VS(1, 1)));
  printf("CreateVertexShader(vs_2_0): hr=0x%08lx\n", (unsigned long)try_create_vs(dev, MAKE_VS(2, 0)));
  // SM2 minor markers FXC emits (2_x / 2_a / 2_b) — the parser
  // deliberately doesn't reject them, so smoke that intent.
  printf("CreateVertexShader(vs_2_x): hr=0x%08lx\n", (unsigned long)try_create_vs(dev, MAKE_VS(2, 1)));
  printf("CreateVertexShader(vs_2_a): hr=0x%08lx\n", (unsigned long)try_create_vs(dev, MAKE_VS(2, 2)));
  printf("CreateVertexShader(vs_2_b): hr=0x%08lx\n", (unsigned long)try_create_vs(dev, MAKE_VS(2, 3)));
  printf("CreateVertexShader(vs_3_0): hr=0x%08lx\n", (unsigned long)try_create_vs(dev, MAKE_VS(3, 0)));
  printf("CreatePixelShader(ps_1_0): hr=0x%08lx\n", (unsigned long)try_create_ps(dev, MAKE_PS(1, 0)));
  printf("CreatePixelShader(ps_1_1): hr=0x%08lx\n", (unsigned long)try_create_ps(dev, MAKE_PS(1, 1)));
  printf("CreatePixelShader(ps_1_4): hr=0x%08lx\n", (unsigned long)try_create_ps(dev, MAKE_PS(1, 4)));
  printf("CreatePixelShader(ps_2_0): hr=0x%08lx\n", (unsigned long)try_create_ps(dev, MAKE_PS(2, 0)));
  printf("CreatePixelShader(ps_2_x): hr=0x%08lx\n", (unsigned long)try_create_ps(dev, MAKE_PS(2, 1)));
  printf("CreatePixelShader(ps_2_a): hr=0x%08lx\n", (unsigned long)try_create_ps(dev, MAKE_PS(2, 2)));
  printf("CreatePixelShader(ps_2_b): hr=0x%08lx\n", (unsigned long)try_create_ps(dev, MAKE_PS(2, 3)));
  printf("CreatePixelShader(ps_3_0): hr=0x%08lx\n", (unsigned long)try_create_ps(dev, MAKE_PS(3, 0)));

  // ---- Cross-kind rejection (DXVK d3d9_device.cpp:8138). ----
  printf(
      "CreateVertexShader(ps_2_0 blob): hr=0x%08lx (want INVALIDCALL)\n",
      (unsigned long)try_create_vs(dev, MAKE_PS(2, 0))
  );
  printf(
      "CreatePixelShader(vs_2_0 blob): hr=0x%08lx (want INVALIDCALL)\n",
      (unsigned long)try_create_ps(dev, MAKE_VS(2, 0))
  );

  // ---- Bad version tags (top 16 bits ≠ 0xFFFE/0xFFFF). ----
  printf(
      "CreateVertexShader(0x12345678): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_vs(dev, 0x12345678u)
  );
  printf(
      "CreatePixelShader(0xDEADBEEF): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_ps(dev, 0xDEADBEEFu)
  );

  // ---- Out-of-range majors. ----
  printf(
      "CreateVertexShader(vs_4_0): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_vs(dev, MAKE_VS(4, 0))
  );
  printf(
      "CreateVertexShader(vs_0_5): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_vs(dev, MAKE_VS(0, 5))
  );
  printf(
      "CreatePixelShader(ps_5_0): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_ps(dev, MAKE_PS(5, 0))
  );

  // ---- Out-of-range minors. ----
  printf(
      "CreateVertexShader(vs_1_5): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_vs(dev, MAKE_VS(1, 5))
  );
  printf(
      "CreateVertexShader(vs_3_1): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_vs(dev, MAKE_VS(3, 1))
  );
  printf(
      "CreatePixelShader(ps_1_5): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_ps(dev, MAKE_PS(1, 5))
  );
  printf(
      "CreatePixelShader(ps_3_1): hr=0x%08lx (want INVALIDCALL)\n", (unsigned long)try_create_ps(dev, MAKE_PS(3, 1))
  );

  // ---- NULL pFunction (was already rejected by length walk). ----
  IDirect3DVertexShader9 *vs = NULL;
  HRESULT bad = dev->CreateVertexShader(NULL, &vs);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  // ---- Header without END token (length walker rejects). ----
  DWORD no_end[2] = {MAKE_VS(2, 0), 0xCAFEBABEu};
  bad = dev->CreateVertexShader(no_end, &vs);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  ULONG dr = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dr);
  ULONG r = d3d->Release();
  printf("Release: %lu\n", (unsigned long)r);
}
