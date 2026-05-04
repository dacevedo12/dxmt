// UpdateTexture spec-gate smoke. Streaming texture systems (Source-era
// + later engines) drive UpdateTexture from a SYSTEMMEM staging
// texture into a DEFAULT-pool GPU texture every frame; the validation
// gates are load-bearing for keeping bogus uploads from reaching the
// Metal blit encoder.
//
// Contract (DXVK d3d9_device.cpp:1080-1130 + wined3d device.c:3826,
// with dxmt's deliberate superset for MANAGED — see UpdateTexture in
// d3d9_device.cpp:3422 for the rationale):
//
//   * NULL src / NULL dst                        → D3DERR_INVALIDCALL.
//   * src == dst (alias)                         → D3DERR_INVALIDCALL.
//   * Resource-type mismatch (Tex / CubeTex /
//     VolumeTex don't cross)                     → D3DERR_INVALIDCALL.
//   * Non-{Texture, CubeTexture, VolumeTexture}
//     dst type                                   → D3DERR_NOTAVAILABLE.
//   * Format mismatch                            → D3DERR_INVALIDCALL.
//   * Pool gate: src must be SYSTEMMEM or
//     MANAGED, dst must be DEFAULT or MANAGED.   → D3DERR_INVALIDCALL.
//   * dst.MipLevels > src.MipLevels without
//     D3DUSAGE_AUTOGENMIPMAP on dst              → D3DERR_INVALIDCALL.
//   * level-0 extent mismatch (after src-tail
//     correspondence)                            → D3DERR_INVALIDCALL.
//
// Happy path is a SYSTEMMEM → DEFAULT same-format same-extent transfer.

#include "../dx9_smoke.h"

#include <string.h>

void
test_update_texture_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- NULL src / dst ----
  check_hr_eq(dev->UpdateTexture(NULL, NULL), D3DERR_INVALIDCALL);

  // ---- Build a SYSTEMMEM source + DEFAULT destination at the same
  // extent + format. These are the textures the rest of the rejection
  // probes mutate around.
  IDirect3DTexture9 *src = NULL;
  IDirect3DTexture9 *dst = NULL;
  check_hr(dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src, NULL));
  check_hr(dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, NULL));
  if (!src || !dst) {
    if (src)
      src->Release();
    if (dst)
      dst->Release();
    return;
  }

  // ---- src == dst ----
  check_hr_eq(
      dev->UpdateTexture(static_cast<IDirect3DBaseTexture9 *>(src), static_cast<IDirect3DBaseTexture9 *>(src)),
      D3DERR_INVALIDCALL
  );

  // ---- NULL on one side only ----
  check_hr_eq(dev->UpdateTexture(NULL, static_cast<IDirect3DBaseTexture9 *>(dst)), D3DERR_INVALIDCALL);
  check_hr_eq(dev->UpdateTexture(static_cast<IDirect3DBaseTexture9 *>(src), NULL), D3DERR_INVALIDCALL);

  // ---- Resource-type mismatch (Texture ↔ CubeTexture). ----
  IDirect3DCubeTexture9 *cube = NULL;
  if (SUCCEEDED(dev->CreateCubeTexture(32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &cube, NULL)) && cube) {
    check_hr_eq(
        dev->UpdateTexture(static_cast<IDirect3DBaseTexture9 *>(src), static_cast<IDirect3DBaseTexture9 *>(cube)),
        D3DERR_INVALIDCALL
    );
    cube->Release();
  }

  // ---- Format mismatch ----
  IDirect3DTexture9 *dst_x8 = NULL;
  if (SUCCEEDED(dev->CreateTexture(32, 32, 1, 0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &dst_x8, NULL)) && dst_x8) {
    check_hr_eq(
        dev->UpdateTexture(static_cast<IDirect3DBaseTexture9 *>(src), static_cast<IDirect3DBaseTexture9 *>(dst_x8)),
        D3DERR_INVALIDCALL
    );
    dst_x8->Release();
  }

  // ---- Pool gate: SYSTEMMEM → SYSTEMMEM is invalid (dst must be
  // DEFAULT or MANAGED). ----
  IDirect3DTexture9 *dst_sysmem = NULL;
  if (SUCCEEDED(dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &dst_sysmem, NULL)) &&
      dst_sysmem) {
    check_hr_eq(
        dev->UpdateTexture(static_cast<IDirect3DBaseTexture9 *>(src), static_cast<IDirect3DBaseTexture9 *>(dst_sysmem)),
        D3DERR_INVALIDCALL
    );
    dst_sysmem->Release();
  }

  // ---- Pool gate: DEFAULT → DEFAULT is invalid (src must be
  // SYSTEMMEM or MANAGED). ----
  IDirect3DTexture9 *src_default = NULL;
  if (SUCCEEDED(dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &src_default, NULL)) &&
      src_default) {
    check_hr_eq(
        dev->UpdateTexture(
            static_cast<IDirect3DBaseTexture9 *>(src_default), static_cast<IDirect3DBaseTexture9 *>(dst)
        ),
        D3DERR_INVALIDCALL
    );
    src_default->Release();
  }

  // ---- Extent mismatch ----
  IDirect3DTexture9 *dst_small = NULL;
  if (SUCCEEDED(dev->CreateTexture(16, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst_small, NULL)) && dst_small) {
    check_hr_eq(
        dev->UpdateTexture(static_cast<IDirect3DBaseTexture9 *>(src), static_cast<IDirect3DBaseTexture9 *>(dst_small)),
        D3DERR_INVALIDCALL
    );
    dst_small->Release();
  }

  // ---- dst.MipLevels > src.MipLevels without AUTOGENMIPMAP. ----
  // src is Levels=1; dst must be Levels=1 too (or have AUTOGENMIPMAP).
  // Build a 2-level dst with no AUTOGENMIPMAP and expect INVALIDCALL.
  IDirect3DTexture9 *dst_many = NULL;
  if (SUCCEEDED(dev->CreateTexture(32, 32, 2, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst_many, NULL)) && dst_many) {
    check_hr_eq(
        dev->UpdateTexture(static_cast<IDirect3DBaseTexture9 *>(src), static_cast<IDirect3DBaseTexture9 *>(dst_many)),
        D3DERR_INVALIDCALL
    );
    dst_many->Release();
  }

  // ---- Happy path: SYSTEMMEM → DEFAULT, same format, same extent.
  // SYSTEMMEM textures don't populate their GPU-side mirror buffer
  // until a Lock/Unlock cycle runs (that's what stages the bytes); a
  // freshly-created source has no mirror and UpdateTexture would
  // INVALIDCALL on the missing-mirror gate. Drive the realistic
  // streaming pattern: Lock → write → Unlock → UpdateTexture.
  IDirect3DSurface9 *src_lvl0 = NULL;
  if (SUCCEEDED(src->GetSurfaceLevel(0, &src_lvl0)) && src_lvl0) {
    D3DLOCKED_RECT lr = {};
    if (SUCCEEDED(src_lvl0->LockRect(&lr, NULL, 0)) && lr.pBits) {
      // Stamp a recognizable pattern (any non-zero write is enough to
      // commit the mirror; content correctness isn't asserted here —
      // smoke_updatesurface already covers pixel-readback).
      memset(lr.pBits, 0x55, static_cast<size_t>(lr.Pitch) * 32);
      src_lvl0->UnlockRect();
    }
    src_lvl0->Release();
  }
  check_hr(dev->UpdateTexture(static_cast<IDirect3DBaseTexture9 *>(src), static_cast<IDirect3DBaseTexture9 *>(dst)));

  src->Release();
  dst->Release();

  check_zero_losable_count(dev);
}
