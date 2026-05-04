// CreateTexture / CreateCubeTexture / CreateVolumeTexture spec-gate
// smoke. Audits the negative paths the D3D9 spec + wined3d enforce
// before any Metal allocation is attempted, plus the AUTOGENMIPMAP /
// pool-vs-usage matrix where dxmt's gates have historically drifted
// from wined3d (texture.c:1245-1290 is the reference shape).
//
// Coverage (per the polish-plan sweep #5):
//   * NULL ppTexture / ppCubeTexture / ppVolumeTexture → INVALIDCALL.
//   * Zero or >16384 dimension → INVALIDCALL.
//   * D3DUSAGE_WRITEONLY (buffer-only) → INVALIDCALL on all 3.
//   * D3DUSAGE_RENDERTARGET + D3DUSAGE_DEPTHSTENCIL combo → INVALIDCALL.
//   * RT or DS in non-DEFAULT pool → INVALIDCALL.
//   * Volume textures with RT or DS usage → INVALIDCALL (spec: no
//     volume render targets in D3D9).
//   * D3DUSAGE_AUTOGENMIPMAP + D3DPOOL_SYSTEMMEM → INVALIDCALL.
//   * D3DUSAGE_AUTOGENMIPMAP + Levels != {0,1} → INVALIDCALL.
//   * Levels exceeding the log2(max-dim)+1 maximum → INVALIDCALL.
//
// Happy paths kept light — full create-and-Lock coverage lives in the
// older create_texture / create_cubetexture / create_dxt smokes.

#include "../dx9_smoke.h"

void
test_create_texture_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- CreateTexture ----
  {
    // NULL out-pointer → INVALIDCALL.
    check_hr_eq(dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, NULL, NULL), D3DERR_INVALIDCALL);

    IDirect3DTexture9 *t = NULL;

    // Zero dimension → INVALIDCALL.
    check_hr_eq(dev->CreateTexture(0, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL), D3DERR_INVALIDCALL);
    check_hr_eq(dev->CreateTexture(16, 0, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL), D3DERR_INVALIDCALL);

    // Dimension > 16384 (Apple-Silicon max-texture cap) → INVALIDCALL.
    check_hr_eq(dev->CreateTexture(32768, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL), D3DERR_INVALIDCALL);

    // WRITEONLY (buffer-only usage) → INVALIDCALL.
    check_hr_eq(
        dev->CreateTexture(16, 16, 1, D3DUSAGE_WRITEONLY, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL),
        D3DERR_INVALIDCALL
    );

    // RT + DS combo → INVALIDCALL.
    check_hr_eq(
        dev->CreateTexture(
            16, 16, 1, D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL
        ),
        D3DERR_INVALIDCALL
    );

    // RT in non-DEFAULT pool → INVALIDCALL.
    check_hr_eq(
        dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL),
        D3DERR_INVALIDCALL
    );
    check_hr_eq(
        dev->CreateTexture(16, 16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &t, NULL),
        D3DERR_INVALIDCALL
    );

    // AUTOGENMIPMAP + SYSTEMMEM → INVALIDCALL.
    check_hr_eq(
        dev->CreateTexture(16, 16, 1, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &t, NULL),
        D3DERR_INVALIDCALL
    );

    // AUTOGENMIPMAP + Levels=2 → INVALIDCALL (only 0 or 1 valid).
    check_hr_eq(
        dev->CreateTexture(16, 16, 2, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL),
        D3DERR_INVALIDCALL
    );

    // Levels > log2(max-dim)+1 → INVALIDCALL. For 4x4 the max is 3.
    check_hr_eq(dev->CreateTexture(4, 4, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL), D3DERR_INVALIDCALL);

    // Happy path — closes out a clean state.
    check_hr(dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL));
    check_true(t != NULL);
    if (t)
      t->Release();
  }

  // ---- CreateCubeTexture ----
  {
    check_hr_eq(dev->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, NULL, NULL), D3DERR_INVALIDCALL);

    IDirect3DCubeTexture9 *c = NULL;

    check_hr_eq(dev->CreateCubeTexture(0, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c, NULL), D3DERR_INVALIDCALL);
    check_hr_eq(dev->CreateCubeTexture(32768, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c, NULL), D3DERR_INVALIDCALL);

    check_hr_eq(
        dev->CreateCubeTexture(16, 1, D3DUSAGE_WRITEONLY, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c, NULL),
        D3DERR_INVALIDCALL
    );

    check_hr_eq(
        dev->CreateCubeTexture(
            16, 1, D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c, NULL
        ),
        D3DERR_INVALIDCALL
    );

    check_hr_eq(
        dev->CreateCubeTexture(16, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &c, NULL),
        D3DERR_INVALIDCALL
    );

    check_hr_eq(
        dev->CreateCubeTexture(16, 1, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &c, NULL),
        D3DERR_INVALIDCALL
    );

    check_hr_eq(
        dev->CreateCubeTexture(16, 3, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c, NULL),
        D3DERR_INVALIDCALL
    );

    check_hr_eq(dev->CreateCubeTexture(4, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c, NULL), D3DERR_INVALIDCALL);

    check_hr(dev->CreateCubeTexture(16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &c, NULL));
    check_true(c != NULL);
    if (c)
      c->Release();
  }

  // ---- CreateVolumeTexture ----
  {
    check_hr_eq(
        dev->CreateVolumeTexture(8, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, NULL, NULL), D3DERR_INVALIDCALL
    );

    IDirect3DVolumeTexture9 *v = NULL;

    check_hr_eq(
        dev->CreateVolumeTexture(0, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &v, NULL), D3DERR_INVALIDCALL
    );
    check_hr_eq(
        dev->CreateVolumeTexture(8, 0, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &v, NULL), D3DERR_INVALIDCALL
    );
    check_hr_eq(
        dev->CreateVolumeTexture(8, 8, 0, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &v, NULL), D3DERR_INVALIDCALL
    );

    // WRITEONLY gate — added in this sweep to match the other Create
    // methods + wined3d texture.c:1260.
    check_hr_eq(
        dev->CreateVolumeTexture(8, 8, 8, 1, D3DUSAGE_WRITEONLY, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &v, NULL),
        D3DERR_INVALIDCALL
    );

    // Volume textures can't be RT or DS — D3D9 spec, both wined3d and
    // DXVK reject this at validation before format gating.
    check_hr_eq(
        dev->CreateVolumeTexture(8, 8, 8, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &v, NULL),
        D3DERR_INVALIDCALL
    );
    check_hr_eq(
        dev->CreateVolumeTexture(8, 8, 8, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &v, NULL),
        D3DERR_INVALIDCALL
    );

    // Levels > log2(max-dim)+1 → INVALIDCALL — added in this sweep.
    // For 4x4x4 the maximum legal level count is 3.
    check_hr_eq(
        dev->CreateVolumeTexture(4, 4, 4, 4, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &v, NULL), D3DERR_INVALIDCALL
    );

    // DXT/BC on volume textures: spec is grey here (MSDN doesn't
    // explicitly forbid it for 3D textures, just for cube; dxmt's
    // D3DFormatToMetal lowers DXT1 through the sampleable path and
    // succeeds, matching what some apps probe). No assertion either
    // way — defer until a target app surfaces a concrete contract.

    check_hr(dev->CreateVolumeTexture(8, 8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &v, NULL));
    check_true(v != NULL);
    if (v)
      v->Release();
  }

  check_zero_losable_count(dev);
}
