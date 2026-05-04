// DXT1/3/5 compressed texture creation smoke. LoL's biggest format
// blocker — without these in D3DFormatToMetal, every CreateTexture
// for a compressed asset (~1900/match in the LoL trace) returns
// INVALIDCALL.
//
// LoL's specific shapes from the API trace:
//   - DXT1 256×256 levels=1 / levels=6 in MANAGED
//   - DXT3 256×256 levels=1 / levels=6 in MANAGED
//   - DXT5 256×256 levels=1 / levels=6 in MANAGED
//   - DXT5 cube (1 call) in MANAGED
// Plus the DEFAULT-pool RT-promotion gate: BC formats can't be RTs
// on Apple Silicon, so the gate must skip the RT bit for compressed
// textures. The smoke covers a DEFAULT-pool DXT1 to verify the
// gate doesn't kill the create.

#include "../dx9_smoke.h"
void
test_create_dxt(void) {
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

  // LoL shape: 256×256 DXT5 MANAGED, 6 levels (mip chain to 8×8).
  IDirect3DTexture9 *t_dxt5 = NULL;
  HRESULT r1 = dev->CreateTexture(256, 256, 6, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &t_dxt5, NULL);
  printf("CreateTexture(DXT5/256/L=6/MANAGED): hr=0x%08lx out=%s\n", (unsigned long)r1, t_dxt5 ? "non-null" : "null");
  if (t_dxt5) {
    D3DSURFACE_DESC d = {};
    t_dxt5->GetLevelDesc(0, &d);
    printf(
        "  L0 Format=0x%lx (expect 0x35545844=DXT5) w=%u h=%u\n", (unsigned long)d.Format, (unsigned)d.Width,
        (unsigned)d.Height
    );

    // Compressed-LockRect contract today: D3DFormatBytesPerPixel
    // returns 0 for compressed, surface LockRect rejects on bpp==0
    // (parity with the 2D MANAGED LockRect gap — no cpu_ptr is
    // allocated for any MANAGED texture). When the per-face
    // shared-buffer wiring lands the math will switch to block-bytes;
    // until then this asserts the rejection so the gap doesn't
    // silently regress.
    D3DLOCKED_RECT lr = {};
    HRESULT lhr = t_dxt5->LockRect(0, &lr, NULL, 0);
    printf(
        "  LockRect: hr=0x%08lx (must reject — MANAGED LockRect "
        "is the next gap)\n",
        (unsigned long)lhr
    );

    t_dxt5->Release();
  }

  // DXT1 (alpha-or-opaque, 4 bits/texel).
  IDirect3DTexture9 *t_dxt1 = NULL;
  HRESULT r2 = dev->CreateTexture(128, 128, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &t_dxt1, NULL);
  printf("CreateTexture(DXT1/128/L=1/MANAGED): hr=0x%08lx out=%s\n", (unsigned long)r2, t_dxt1 ? "non-null" : "null");
  if (t_dxt1)
    t_dxt1->Release();

  // DXT3.
  IDirect3DTexture9 *t_dxt3 = NULL;
  HRESULT r3 = dev->CreateTexture(64, 64, 1, 0, D3DFMT_DXT3, D3DPOOL_MANAGED, &t_dxt3, NULL);
  printf("CreateTexture(DXT3/64/L=1/MANAGED): hr=0x%08lx out=%s\n", (unsigned long)r3, t_dxt3 ? "non-null" : "null");
  if (t_dxt3)
    t_dxt3->Release();

  // DEFAULT pool — verifies the RT-promotion gate skips compressed
  // (Apple Silicon rejects BC formats with the RT bit).
  IDirect3DTexture9 *t_def = NULL;
  HRESULT r4 = dev->CreateTexture(64, 64, 1, 0, D3DFMT_DXT5, D3DPOOL_DEFAULT, &t_def, NULL);
  printf(
      "CreateTexture(DXT5/64/L=1/DEFAULT): hr=0x%08lx out=%s "
      "(RT-promotion must skip BC)\n",
      (unsigned long)r4, t_def ? "non-null" : "null"
  );
  if (t_def)
    t_def->Release();

  // BC as RT — must reject (D3DUSAGE_RENDERTARGET on a BC format is
  // a contract violation).
  IDirect3DTexture9 *t_bad = (IDirect3DTexture9 *)0xdead;
  HRESULT rb = dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_DXT5, D3DPOOL_DEFAULT, &t_bad, NULL);
  printf(
      "CreateTexture(DXT5 + USAGE_RT): hr=0x%08lx out=%s "
      "(must reject)\n",
      (unsigned long)rb, t_bad == NULL ? "null" : "non-null"
  );

  // BC as DS — must reject.
  IDirect3DTexture9 *t_bad2 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb2 = dev->CreateTexture(64, 64, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_DXT5, D3DPOOL_DEFAULT, &t_bad2, NULL);
  printf(
      "CreateTexture(DXT5 + USAGE_DS): hr=0x%08lx out=%s "
      "(must reject)\n",
      (unsigned long)rb2, t_bad2 == NULL ? "null" : "non-null"
  );

  // DXT5 cube (LoL's reflection-probe shape, 1 call/match).
  IDirect3DCubeTexture9 *t_cube = NULL;
  HRESULT r5 = dev->CreateCubeTexture(64, 1, 0, D3DFMT_DXT5, D3DPOOL_MANAGED, &t_cube, NULL);
  printf(
      "CreateCubeTexture(DXT5/64/L=1/MANAGED): hr=0x%08lx out=%s\n", (unsigned long)r5, t_cube ? "non-null" : "null"
  );
  if (t_cube)
    t_cube->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
