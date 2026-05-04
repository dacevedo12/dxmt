// CreateVertexBuffer / CreateIndexBuffer spec-gate smoke. Audits the
// pool / usage matrix wined3d + DXVK enforce, plus the index-format
// gate that's unique to CreateIndexBuffer.
//
// The validation contract (wined3d buffer.c:284-310 + DXVK
// d3d9_common_buffer.cpp:55-66):
//
//   * NULL out-pointer / Length == 0           → D3DERR_INVALIDCALL.
//   * D3DPOOL_SCRATCH                          → D3DERR_INVALIDCALL.
//   * D3DPOOL_MANAGED + D3DUSAGE_DYNAMIC       → D3DERR_INVALIDCALL.
//   * D3DUSAGE_RENDERTARGET / DEPTHSTENCIL /
//     AUTOGENMIPMAP on buffer creation         → D3DERR_INVALIDCALL.
//   * Index format != INDEX16 / INDEX32        → D3DERR_INVALIDCALL.
//
// The happy paths (DEFAULT / SYSTEMMEM / MANAGED with no exotic usage)
// stay in the existing smoke_create_buffer.cpp — this smoke focuses on
// the rejection contract.

#include "../dx9_smoke.h"

void
test_create_buffer_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- CreateVertexBuffer ----
  {
    // NULL out-pointer.
    check_hr_eq(dev->CreateVertexBuffer(64, 0, 0, D3DPOOL_DEFAULT, NULL, NULL), D3DERR_INVALIDCALL);

    IDirect3DVertexBuffer9 *vb = NULL;

    // Length == 0.
    check_hr_eq(dev->CreateVertexBuffer(0, 0, 0, D3DPOOL_DEFAULT, &vb, NULL), D3DERR_INVALIDCALL);

    // SCRATCH pool.
    check_hr_eq(dev->CreateVertexBuffer(64, 0, 0, D3DPOOL_SCRATCH, &vb, NULL), D3DERR_INVALIDCALL);

    // MANAGED + DYNAMIC — DXVK rejection (wined3d permits but the
    // combination has no defined meaning).
    check_hr_eq(dev->CreateVertexBuffer(64, D3DUSAGE_DYNAMIC, 0, D3DPOOL_MANAGED, &vb, NULL), D3DERR_INVALIDCALL);

    // RT / DS / AUTOGENMIPMAP usage — texture-only flags.
    check_hr_eq(dev->CreateVertexBuffer(64, D3DUSAGE_RENDERTARGET, 0, D3DPOOL_DEFAULT, &vb, NULL), D3DERR_INVALIDCALL);
    check_hr_eq(dev->CreateVertexBuffer(64, D3DUSAGE_DEPTHSTENCIL, 0, D3DPOOL_DEFAULT, &vb, NULL), D3DERR_INVALIDCALL);
    check_hr_eq(dev->CreateVertexBuffer(64, D3DUSAGE_AUTOGENMIPMAP, 0, D3DPOOL_DEFAULT, &vb, NULL), D3DERR_INVALIDCALL);

    // Happy path — the canonical DYNAMIC + WRITEONLY DEFAULT VB the
    // streaming renderers use every frame.
    check_hr(
        dev->CreateVertexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb, NULL)
    );
    check_true(vb != NULL);
    if (vb)
      vb->Release();
  }

  // ---- CreateIndexBuffer ----
  {
    check_hr_eq(dev->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, NULL, NULL), D3DERR_INVALIDCALL);

    IDirect3DIndexBuffer9 *ib = NULL;

    check_hr_eq(dev->CreateIndexBuffer(0, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL), D3DERR_INVALIDCALL);

    // Non-index format.
    check_hr_eq(dev->CreateIndexBuffer(64, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &ib, NULL), D3DERR_INVALIDCALL);
    check_hr_eq(dev->CreateIndexBuffer(64, 0, D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, &ib, NULL), D3DERR_INVALIDCALL);

    // SCRATCH pool.
    check_hr_eq(dev->CreateIndexBuffer(64, 0, D3DFMT_INDEX16, D3DPOOL_SCRATCH, &ib, NULL), D3DERR_INVALIDCALL);

    // MANAGED + DYNAMIC.
    check_hr_eq(
        dev->CreateIndexBuffer(64, D3DUSAGE_DYNAMIC, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL), D3DERR_INVALIDCALL
    );

    // RT / DS / AUTOGENMIPMAP usage.
    check_hr_eq(
        dev->CreateIndexBuffer(64, D3DUSAGE_RENDERTARGET, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL),
        D3DERR_INVALIDCALL
    );
    check_hr_eq(
        dev->CreateIndexBuffer(64, D3DUSAGE_DEPTHSTENCIL, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL),
        D3DERR_INVALIDCALL
    );
    check_hr_eq(
        dev->CreateIndexBuffer(64, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL),
        D3DERR_INVALIDCALL
    );

    // Happy path — both INDEX16 and INDEX32 are legal.
    check_hr(
        dev->CreateIndexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL)
    );
    check_true(ib != NULL);
    if (ib)
      ib->Release();

    check_hr(dev->CreateIndexBuffer(64, 0, D3DFMT_INDEX32, D3DPOOL_MANAGED, &ib, NULL));
    check_true(ib != NULL);
    if (ib)
      ib->Release();
  }

  check_zero_losable_count(dev);
}
