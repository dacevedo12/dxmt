// INTZ/DF24/DF16 sampleable-depth FOURCC creation smoke. The
// validation pattern apps use is:
//   1. CheckDeviceFormat(DEPTHSTENCIL, INTZ)  — gate
//   2. CreateTexture(USAGE_DEPTHSTENCIL, INTZ, DEFAULT)  — bind as DSV
//   3. SetDepthStencilSurface(level0)         — render shadow
//   4. SetTexture(stage, intz_texture)        — sample at PCF time
//
// Until CheckDeviceFormat lands, apps that hardcode INTZ availability
// (notably LoL's shadow path) jump straight to step 2. Validates that
// CreateTexture succeeds for INTZ/DF24/DF16 in DEFAULT pool with
// USAGE_DEPTHSTENCIL.

#include "../dx9_smoke.h"
static const D3DFORMAT FOURCC_INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');
static const D3DFORMAT FOURCC_DF24 = (D3DFORMAT)MAKEFOURCC('D', 'F', '2', '4');
static const D3DFORMAT FOURCC_DF16 = (D3DFORMAT)MAKEFOURCC('D', 'F', '1', '6');

void
test_create_intz(void) {
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

  // INTZ texture as DSV. LoL shadow-map shape: 256×256 single level.
  IDirect3DTexture9 *t_intz = NULL;
  HRESULT r1 = dev->CreateTexture(256, 256, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC_INTZ, D3DPOOL_DEFAULT, &t_intz, NULL);
  printf(
      "CreateTexture(INTZ/256/DEPTHSTENCIL/DEFAULT): hr=0x%08lx out=%s\n", (unsigned long)r1,
      t_intz ? "non-null" : "null"
  );

  // DF24 texture (no stencil aspect).
  IDirect3DTexture9 *t_df24 = NULL;
  HRESULT r2 = dev->CreateTexture(128, 128, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC_DF24, D3DPOOL_DEFAULT, &t_df24, NULL);
  printf(
      "CreateTexture(DF24/128/DEPTHSTENCIL/DEFAULT): hr=0x%08lx out=%s\n", (unsigned long)r2,
      t_df24 ? "non-null" : "null"
  );

  // DF16 texture.
  IDirect3DTexture9 *t_df16 = NULL;
  HRESULT r3 = dev->CreateTexture(128, 128, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC_DF16, D3DPOOL_DEFAULT, &t_df16, NULL);
  printf(
      "CreateTexture(DF16/128/DEPTHSTENCIL/DEFAULT): hr=0x%08lx out=%s\n", (unsigned long)r3,
      t_df16 ? "non-null" : "null"
  );

  // RT-usage with INTZ — depth format, must reject.
  IDirect3DTexture9 *t_bad2 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb2 = dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, FOURCC_INTZ, D3DPOOL_DEFAULT, &t_bad2, NULL);
  printf("CreateTexture(INTZ as RT): hr=0x%08lx out=%s\n", (unsigned long)rb2, t_bad2 == NULL ? "null" : "non-null");

  // Unrecognised FOURCC — must reject. Picks 'XYZW' as a marker.
  IDirect3DTexture9 *t_bad3 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb3 = dev->CreateTexture(
      64, 64, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('X', 'Y', 'Z', 'W'), D3DPOOL_DEFAULT, &t_bad3, NULL
  );
  printf(
      "CreateTexture(unknown fourcc): hr=0x%08lx out=%s\n", (unsigned long)rb3, t_bad3 == NULL ? "null" : "non-null"
  );

  // Bind INTZ texture for sampling — validates the texture is usable
  // through the SetTexture path (no cross-device or type mismatch).
  if (t_intz) {
    HRESULT shr = dev->SetTexture(0, t_intz);
    printf("  SetTexture(0, INTZ): hr=0x%08lx\n", (unsigned long)shr);
    dev->SetTexture(0, NULL);
  }

  if (t_intz)
    t_intz->Release();
  if (t_df24)
    t_df24->Release();
  if (t_df16)
    t_df16->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
