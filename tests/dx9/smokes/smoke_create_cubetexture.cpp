// CreateCubeTexture smoke. Validates the LoL-named boot blocker:
// reflection probes are 64×64 D3DPOOL_MANAGED no-mip cubes that LoL
// expects to allocate at startup. Beyond the happy path, exercise the
// validation gates that share a shape with CreateTexture (RT|DS, RT in
// non-DEFAULT pool, AUTOGENMIPMAP+SYSTEMMEM, levels > full-chain),
// face/level out-of-range, and SetTexture binding the cube to a stage.

#include "../dx9_smoke.h"
void
test_create_cubetexture(void) {
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

  // LoL probe shape: 64×64 A8R8G8B8 MANAGED, single level.
  IDirect3DCubeTexture9 *probe = NULL;
  HRESULT rp = dev->CreateCubeTexture(64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, NULL);
  printf(
      "CreateCubeTexture(A8R8G8B8/64/L=1/MANAGED): hr=0x%08lx out=%s "
      "levels=%lu\n",
      (unsigned long)rp, probe ? "non-null" : "null", probe ? (unsigned long)probe->GetLevelCount() : 0ul
  );
  if (probe) {
    printf(
        "  GetType: 0x%lx (expect 0x%x = D3DRTYPE_CUBETEXTURE)\n", (unsigned long)probe->GetType(),
        (unsigned)D3DRTYPE_CUBETEXTURE
    );

    D3DSURFACE_DESC d = {};
    HRESULT ghr = probe->GetLevelDesc(0, &d);
    printf(
        "  GetLevelDesc(0): hr=0x%08lx fmt=0x%08lx w=%u h=%u\n", (unsigned long)ghr, (unsigned long)d.Format,
        (unsigned)d.Width, (unsigned)d.Height
    );

    // Walk all 6 faces at level 0; each face surface must be valid and
    // distinct from the others, but stable across calls (same-object
    // contract).
    IDirect3DSurface9 *faces[6] = {};
    for (uint32_t f = 0; f < 6; ++f) {
      HRESULT shr = probe->GetCubeMapSurface((D3DCUBEMAP_FACES)f, 0, &faces[f]);
      printf(
          "  GetCubeMapSurface(face=%u, L=0): hr=0x%08lx out=%s\n", (unsigned)f, (unsigned long)shr,
          faces[f] ? "non-null" : "null"
      );
    }
    bool all_distinct = true;
    for (uint32_t i = 0; i < 6 && all_distinct; ++i)
      for (uint32_t j = i + 1; j < 6 && all_distinct; ++j)
        if (faces[i] && faces[j] && faces[i] == faces[j])
          all_distinct = false;
    printf("  faces 0..5 all distinct: %s\n", all_distinct ? "yes" : "no");

    // Same-object contract: GetCubeMapSurface twice on (face=2, L=0)
    // returns the same pointer.
    IDirect3DSurface9 *again = NULL;
    probe->GetCubeMapSurface(D3DCUBEMAP_FACE_NEGATIVE_X, 0, &again);
    printf("  GetCubeMapSurface(NEG_X) twice: same=%s\n", (again && again == faces[1]) ? "yes" : "no");
    if (again)
      again->Release();
    for (uint32_t i = 0; i < 6; ++i)
      if (faces[i])
        faces[i]->Release();

    // Bind to stage 0 — exercises SetTexture's GetType() switch.
    HRESULT shr = dev->SetTexture(0, probe);
    printf("  SetTexture(0, probe): hr=0x%08lx\n", (unsigned long)shr);
    IDirect3DBaseTexture9 *back = NULL;
    HRESULT ghr2 = dev->GetTexture(0, &back);
    printf("  GetTexture(0): hr=0x%08lx same=%s\n", (unsigned long)ghr2, (back == probe) ? "yes" : "no");
    if (back)
      back->Release();
    dev->SetTexture(0, NULL);
  }

  // Failure paths.
  IDirect3DCubeTexture9 *bad = (IDirect3DCubeTexture9 *)0xdead;
  HRESULT rb1 = dev->CreateCubeTexture(0, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &bad, NULL);
  printf("CreateCubeTexture(edge=0): hr=0x%08lx out=%s\n", (unsigned long)rb1, bad == NULL ? "null" : "non-null");

  IDirect3DCubeTexture9 *bad2 = (IDirect3DCubeTexture9 *)0xdead;
  HRESULT rb2 = dev->CreateCubeTexture(
      64, 1, D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &bad2, NULL
  );
  printf("CreateCubeTexture(RT|DS): hr=0x%08lx out=%s\n", (unsigned long)rb2, bad2 == NULL ? "null" : "non-null");

  IDirect3DCubeTexture9 *bad3 = (IDirect3DCubeTexture9 *)0xdead;
  HRESULT rb3 = dev->CreateCubeTexture(64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &bad3, NULL);
  printf(
      "CreateCubeTexture(RT in SYSTEMMEM): hr=0x%08lx out=%s\n", (unsigned long)rb3, bad3 == NULL ? "null" : "non-null"
  );

  IDirect3DCubeTexture9 *bad4 = (IDirect3DCubeTexture9 *)0xdead;
  HRESULT rb4 = dev->CreateCubeTexture(64, 8, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &bad4, NULL);
  printf(
      "CreateCubeTexture(64/L=8): hr=0x%08lx out=%s "
      "(over full-chain must reject)\n",
      (unsigned long)rb4, bad4 == NULL ? "null" : "non-null"
  );

  // Levels=0 → full chain. 64 → log2(64)+1 = 7.
  IDirect3DCubeTexture9 *full = NULL;
  HRESULT rfc = dev->CreateCubeTexture(64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &full, NULL);
  printf(
      "CreateCubeTexture(64/L=0/DEFAULT): hr=0x%08lx out=%s levels=%lu\n", (unsigned long)rfc,
      full ? "non-null" : "null", full ? (unsigned long)full->GetLevelCount() : 0ul
  );
  if (full) {
    // Out-of-range face / level.
    IDirect3DSurface9 *s = NULL;
    HRESULT shr1 = full->GetCubeMapSurface((D3DCUBEMAP_FACES)6, 0, &s);
    printf(
        "  GetCubeMapSurface(face=6): hr=0x%08lx out=%s (must reject)\n", (unsigned long)shr1, s ? "non-null" : "null"
    );
    HRESULT shr2 = full->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 99, &s);
    printf(
        "  GetCubeMapSurface(L=99): hr=0x%08lx out=%s (must reject)\n", (unsigned long)shr2, s ? "non-null" : "null"
    );

    // Level 6 = 1×1.
    D3DSURFACE_DESC d = {};
    full->GetLevelDesc(6, &d);
    printf("  GetLevelDesc(6): w=%u h=%u (expect 1)\n", (unsigned)d.Width, (unsigned)d.Height);

    full->Release();
  }

  if (probe)
    probe->Release();

  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
