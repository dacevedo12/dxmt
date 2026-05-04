// CreateTexture smoke: device + a few textures across pools/formats/
// mip-counts, walk the level chain via GetSurfaceLevel + GetLevelDesc,
// hit the rejection paths the runtime is expected to gate (MANAGED on
// Ex, RT+DS combined, RT in non-DEFAULT pool, AUTOGENMIPMAP +
// SYSTEMMEM, levels > log2(max(w,h))+1, etc.). Lock/Unlock + actual
// pixel access lands in the next commits — those return
// D3DERR_INVALIDCALL today and the failure pattern is part of what we
// hash.

#include "../dx9_smoke.h"
static void
DescribeLevel(IDirect3DTexture9 *t, UINT level, const char *label) {
  D3DSURFACE_DESC d = {};
  HRESULT hr = t->GetLevelDesc(level, &d);
  printf(
      "  %s L%u GetLevelDesc: hr=0x%08lx fmt=0x%08lx w=%u h=%u\n", label, (unsigned)level, (unsigned long)hr,
      (unsigned long)d.Format, (unsigned)d.Width, (unsigned)d.Height
  );

  IDirect3DSurface9 *s = NULL;
  HRESULT shr = t->GetSurfaceLevel(level, &s);
  printf(
      "  %s L%u GetSurfaceLevel: hr=0x%08lx out=%s\n", label, (unsigned)level, (unsigned long)shr,
      s ? "non-null" : "null"
  );
  if (s) {
    // GetSurfaceLevel twice on the same level must return the same
    // object (D3D9 caching contract). Compare pointer identity.
    IDirect3DSurface9 *s2 = NULL;
    t->GetSurfaceLevel(level, &s2);
    printf("  %s L%u GetSurfaceLevel(twice): same=%s\n", label, (unsigned)level, (s == s2) ? "yes" : "no");
    if (s2)
      s2->Release();
    s->Release();
  }
}

void
test_create_texture(void) {
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

  // Happy path: 256x128 A8R8G8B8 with explicit 5 levels in DEFAULT.
  IDirect3DTexture9 *t0 = NULL;
  HRESULT r0 = dev->CreateTexture(256, 128, 5, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t0, NULL);
  printf(
      "CreateTexture(A8R8G8B8/256x128/L=5/DEFAULT): hr=0x%08lx out=%s "
      "levels=%lu\n",
      (unsigned long)r0, t0 ? "non-null" : "null", t0 ? (unsigned long)t0->GetLevelCount() : 0ul
  );
  if (t0) {
    DescribeLevel(t0, 0, "t0");
    DescribeLevel(t0, 2, "t0");
    DescribeLevel(t0, 4, "t0");
    // Out-of-range level rejects.
    IDirect3DSurface9 *bad = NULL;
    HRESULT bhr = t0->GetSurfaceLevel(99, &bad);
    printf("  t0->GetSurfaceLevel(99): hr=0x%08lx out=%s\n", (unsigned long)bhr, bad ? "non-null" : "null");
  }

  // Levels=0 → full chain. 64x32 → log2(64)+1 = 7 levels.
  IDirect3DTexture9 *t1 = NULL;
  HRESULT r1 = dev->CreateTexture(64, 32, 0, 0, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &t1, NULL);
  printf(
      "CreateTexture(X8R8G8B8/64x32/L=0/SYSTEMMEM): hr=0x%08lx out=%s "
      "levels=%lu\n",
      (unsigned long)r1, t1 ? "non-null" : "null", t1 ? (unsigned long)t1->GetLevelCount() : 0ul
  );
  if (t1) {
    DescribeLevel(t1, 0, "t1");
    DescribeLevel(t1, 6, "t1"); // last level (1x1)
  }

  // Single-level R5G6B5 in SCRATCH.
  IDirect3DTexture9 *t2 = NULL;
  HRESULT r2 = dev->CreateTexture(32, 32, 1, 0, D3DFMT_R5G6B5, D3DPOOL_SCRATCH, &t2, NULL);
  printf("CreateTexture(R5G6B5/32/L=1/SCRATCH): hr=0x%08lx out=%s\n", (unsigned long)r2, t2 ? "non-null" : "null");
  if (t2)
    DescribeLevel(t2, 0, "t2");

  // RT-usage texture (e.g. shadow map / offscreen render). Must live
  // in DEFAULT.
  IDirect3DTexture9 *t_rt = NULL;
  HRESULT r_rt = dev->CreateTexture(128, 128, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t_rt, NULL);
  printf("CreateTexture(A8R8G8B8/RT/DEFAULT): hr=0x%08lx out=%s\n", (unsigned long)r_rt, t_rt ? "non-null" : "null");

  // DS-usage texture (sampleable depth — INTZ-style shadow map case).
  IDirect3DTexture9 *t_ds = NULL;
  HRESULT r_ds = dev->CreateTexture(128, 128, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &t_ds, NULL);
  printf("CreateTexture(D24S8/DS/DEFAULT): hr=0x%08lx out=%s\n", (unsigned long)r_ds, t_ds ? "non-null" : "null");

  // ---- Failure paths ----
  // RT and DS combined.
  IDirect3DTexture9 *t_bad1 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb1 = dev->CreateTexture(
      64, 64, 1, D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t_bad1, NULL
  );
  printf(
      "CreateTexture(RT|DS): hr=0x%08lx out=%s (mutually-exclusive must reject)\n", (unsigned long)rb1,
      t_bad1 == NULL ? "null" : "non-null"
  );

  // RT-usage in SYSTEMMEM.
  IDirect3DTexture9 *t_bad2 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb2 = dev->CreateTexture(64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &t_bad2, NULL);
  printf(
      "CreateTexture(RT in SYSTEMMEM): hr=0x%08lx out=%s\n", (unsigned long)rb2, t_bad2 == NULL ? "null" : "non-null"
  );

  // D3DUSAGE_WRITEONLY on a texture (buffer-only flag).
  IDirect3DTexture9 *t_bad3 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb3 = dev->CreateTexture(64, 64, 1, D3DUSAGE_WRITEONLY, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t_bad3, NULL);
  printf("CreateTexture(WRITEONLY): hr=0x%08lx out=%s\n", (unsigned long)rb3, t_bad3 == NULL ? "null" : "non-null");

  // AUTOGENMIPMAP in SYSTEMMEM.
  IDirect3DTexture9 *t_bad4 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb4 =
      dev->CreateTexture(64, 64, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &t_bad4, NULL);
  printf(
      "CreateTexture(AUTOGENMIPMAP+SYSTEMMEM): hr=0x%08lx out=%s\n", (unsigned long)rb4,
      t_bad4 == NULL ? "null" : "non-null"
  );

  // AUTOGENMIPMAP with explicit Levels > 1 — wined3d rejects.
  IDirect3DTexture9 *t_bad5 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb5 = dev->CreateTexture(64, 64, 4, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t_bad5, NULL);
  printf(
      "CreateTexture(AUTOGENMIPMAP+L=4): hr=0x%08lx out=%s\n", (unsigned long)rb5, t_bad5 == NULL ? "null" : "non-null"
  );

  // AUTOGENMIPMAP, valid: app sees one level.
  IDirect3DTexture9 *t_agm = NULL;
  HRESULT r_agm =
      dev->CreateTexture(128, 128, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t_agm, NULL);
  printf(
      "CreateTexture(AUTOGENMIPMAP/DEFAULT/L=0): hr=0x%08lx out=%s "
      "levels=%lu\n",
      (unsigned long)r_agm, t_agm ? "non-null" : "null", t_agm ? (unsigned long)t_agm->GetLevelCount() : 0ul
  );
  if (t_agm) {
    // Level 1+ access on AUTOGENMIPMAP texture: INVALIDCALL.
    IDirect3DSurface9 *s = NULL;
    HRESULT shr = t_agm->GetSurfaceLevel(1, &s);
    printf(
        "  t_agm L1 GetSurfaceLevel: hr=0x%08lx out=%s "
        "(AUTOGENMIPMAP L>0 must reject)\n",
        (unsigned long)shr, s ? "non-null" : "null"
    );
  }

  // Levels > full-chain. 32x32 → max 6 levels; ask for 8.
  IDirect3DTexture9 *t_bad6 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb6 = dev->CreateTexture(32, 32, 8, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t_bad6, NULL);
  printf(
      "CreateTexture(32x32/L=8): hr=0x%08lx out=%s (over full-chain must reject)\n", (unsigned long)rb6,
      t_bad6 == NULL ? "null" : "non-null"
  );

  // Zero-area.
  IDirect3DTexture9 *t_bad7 = (IDirect3DTexture9 *)0xdead;
  HRESULT rb7 = dev->CreateTexture(0, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t_bad7, NULL);
  printf("CreateTexture(0x32): hr=0x%08lx out=%s\n", (unsigned long)rb7, t_bad7 == NULL ? "null" : "non-null");

  // ---- Lifetime: surface from GetSurfaceLevel pins the texture and
  // (transitively) the device. Release the texture and outliving
  // surface should still let the device walk back via GetDevice.
  IDirect3DSurface9 *outliver = NULL;
  if (t0) {
    t0->GetSurfaceLevel(0, &outliver);
    ULONG t0_ref = t0->Release();
    printf("Release(t0) while L0 surface held: %lu (expect non-zero — pinned)\n", (unsigned long)t0_ref);
  }
  if (outliver) {
    IDirect3DDevice9 *back = NULL;
    HRESULT bhr = outliver->GetDevice(&back);
    printf(
        "outliver->GetDevice (post t0 release): hr=0x%08lx dev=%s\n", (unsigned long)bhr, back ? "non-null" : "null"
    );
    if (back)
      back->Release();
    outliver->Release();
  }

  if (t1)
    t1->Release();
  if (t2)
    t2->Release();
  if (t_rt)
    t_rt->Release();
  if (t_ds)
    t_ds->Release();
  if (t_agm)
    t_agm->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
