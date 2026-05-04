// SetTexture / GetTexture smoke. Exercises stage-range mapping (PS
// 0..15, D3DVERTEXTEXTURESAMPLER0..3 → internal 16..19, D3DDMAPSAMPLER
// silently ignored), the unbound default (Get returns NULL +
// D3D_OK, never NOTFOUND — different from GetRenderTarget!), the
// cross-device rejection path, and the priv-ref pin (texture released
// publicly while bound stays alive).

#include "../dx9_smoke.h"
void
test_set_texture(void) {
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

  // Initial state: nothing bound at any stage. Get* returns
  // *ppOut = NULL with D3D_OK (NOT NOTFOUND — wined3d device.c:2811-21).
  IDirect3DBaseTexture9 *initial0 = NULL;
  HRESULT g0 = dev->GetTexture(0, &initial0);
  printf("GetTexture(0) at start: hr=0x%08lx out=%s\n", (unsigned long)g0, initial0 ? "non-null" : "null");
  if (initial0)
    initial0->Release();

  // Out-of-range probe: D3DDMAPSAMPLER (256). wined3d ignores it; Get
  // must return NULL/D3D_OK.
  IDirect3DBaseTexture9 *dmap = NULL;
  HRESULT gd = dev->GetTexture(D3DDMAPSAMPLER, &dmap);
  printf("GetTexture(D3DDMAPSAMPLER): hr=0x%08lx out=%s\n", (unsigned long)gd, dmap ? "non-null" : "null");
  if (dmap)
    dmap->Release();

  // Make a few textures.
  IDirect3DTexture9 *tex0 = NULL;
  dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex0, NULL);
  IDirect3DTexture9 *tex_vs = NULL;
  dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex_vs, NULL);
  printf("Created tex0=%s tex_vs=%s\n", tex0 ? "ok" : "null", tex_vs ? "ok" : "null");

  // Happy path: PS stage 0 + VS stage 0 (D3DVERTEXTEXTURESAMPLER0).
  HRESULT s0 = dev->SetTexture(0, tex0);
  HRESULT sv = dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, tex_vs);
  printf("SetTexture(0, tex0): hr=0x%08lx\n", (unsigned long)s0);
  printf("SetTexture(D3DVERTEXTEXTURESAMPLER0, tex_vs): hr=0x%08lx\n", (unsigned long)sv);

  // Round-trip via Get.
  IDirect3DBaseTexture9 *got0 = NULL;
  HRESULT gg0 = dev->GetTexture(0, &got0);
  printf("GetTexture(0): hr=0x%08lx same=%s\n", (unsigned long)gg0, got0 == tex0 ? "yes" : "no");
  if (got0)
    got0->Release();

  IDirect3DBaseTexture9 *gv = NULL;
  HRESULT ggv = dev->GetTexture(D3DVERTEXTEXTURESAMPLER0, &gv);
  printf("GetTexture(D3DVERTEXTEXTURESAMPLER0): hr=0x%08lx same=%s\n", (unsigned long)ggv, gv == tex_vs ? "yes" : "no");
  if (gv)
    gv->Release();

  // Unbound stage in the valid range still returns NULL/D3D_OK.
  IDirect3DBaseTexture9 *got5 = NULL;
  HRESULT gg5 = dev->GetTexture(5, &got5);
  printf("GetTexture(5) (unbound): hr=0x%08lx out=%s\n", (unsigned long)gg5, got5 ? "non-null" : "null");
  if (got5)
    got5->Release();

  // Unbind PS stage 0 with NULL (allowed — different from
  // SetRenderTarget(0, NULL) which is rejected).
  HRESULT u0 = dev->SetTexture(0, NULL);
  printf("SetTexture(0, NULL) (unbind): hr=0x%08lx\n", (unsigned long)u0);
  IDirect3DBaseTexture9 *got0b = NULL;
  HRESULT gg0b = dev->GetTexture(0, &got0b);
  printf("GetTexture(0) post-unbind: hr=0x%08lx out=%s\n", (unsigned long)gg0b, got0b ? "non-null" : "null");
  if (got0b)
    got0b->Release();

  // ---- Out-of-range stages: silent no-op, no error returned. ----
  HRESULT dmap_set = dev->SetTexture(D3DDMAPSAMPLER, tex0);
  printf("SetTexture(D3DDMAPSAMPLER, tex0): hr=0x%08lx (ignored, no error)\n", (unsigned long)dmap_set);
  HRESULT crazy_set = dev->SetTexture(9999, tex0);
  printf("SetTexture(9999, tex0): hr=0x%08lx (ignored, no error)\n", (unsigned long)crazy_set);

  // ---- Cross-device rejection ----
  IDirect3DDevice9 *dev2 = NULL;
  HRESULT cd2 = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev2);
  printf("CreateDevice(2): hr=0x%08lx\n", (unsigned long)cd2);
  if (SUCCEEDED(cd2) && dev2) {
    IDirect3DTexture9 *foreign = NULL;
    dev2->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &foreign, NULL);
    if (foreign) {
      HRESULT bad_dev = dev->SetTexture(2, foreign);
      printf("SetTexture(2, foreign-device-texture): hr=0x%08lx\n", (unsigned long)bad_dev);
      foreign->Release();
    }
    dev2->Release();
  }

  // ---- Lifetime: release tex_vs publicly while still bound at
  // VS stage 0. Device's priv ref keeps it alive; replacement at the
  // same stage drops the priv ref and destructs it.
  IDirect3DTexture9 *replacement = NULL;
  dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &replacement, NULL);
  ULONG vs_ref = tex_vs->Release();
  printf("Release(tex_vs) while bound: %lu (device priv ref keeps it alive)\n", (unsigned long)vs_ref);
  IDirect3DBaseTexture9 *still_vs = NULL;
  HRESULT still_hr = dev->GetTexture(D3DVERTEXTEXTURESAMPLER0, &still_vs);
  printf(
      "GetTexture(D3DVERTEXTEXTURESAMPLER0) post-release: hr=0x%08lx "
      "ptr_match=%s\n",
      (unsigned long)still_hr, still_vs == tex_vs ? "yes" : "no"
  );
  if (still_vs)
    still_vs->Release();
  dev->SetTexture(D3DVERTEXTEXTURESAMPLER0, replacement);
  if (replacement)
    replacement->Release();

  if (tex0)
    tex0->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
