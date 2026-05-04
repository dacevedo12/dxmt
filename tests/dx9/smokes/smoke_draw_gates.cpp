// DrawPrimitive / DrawIndexedPrimitive / *UP spec-gate smokes.
//
// The four Draw methods share a common validation contract derived
// from wined3d (wine/dlls/d3d9/device.c:3236-3406) and DXVK
// (dxvk/src/d3d9/d3d9_device.cpp Draw* paths):
//
//   * PrimitiveCount == 0          → D3D_OK (no-op).
//   * PrimitiveCount > MaxPrimitiveCount → D3DERR_INVALIDCALL.
//   * !vertex_declaration          → D3DERR_INVALIDCALL.
//   * DrawIndexedPrimitive only: !IB → D3DERR_INVALIDCALL.
//   * DrawPrimitiveUP / DrawIndexedPrimitiveUP only:
//       - !pVertexStreamZeroData    → D3DERR_INVALIDCALL.
//       - VertexStreamZeroStride == 0 → D3DERR_INVALIDCALL.
//       - DrawIndexedPrimitiveUP only:
//           !pIndexData             → D3DERR_INVALIDCALL.
//           IndexDataFormat != INDEX16 && != INDEX32 → D3DERR_INVALIDCALL.
//           NumVertices == 0        → D3D_OK (DXVK d3d9_device.cpp:3229).
//
// Existing draw smokes (smoke_draw_triangle / smoke_draw_indexed /
// smoke_drawprimitive_up / smoke_drawindexed_up) exercise the happy
// paths. This smoke covers the negative-return contract only, so the
// HRESULT shape is locked down separately from the rendered-output
// shape.

#include "../dx9_smoke.h"

void
test_draw_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- DrawPrimitive ----
  // No decl set yet.
  check_hr_eq(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1), D3DERR_INVALIDCALL);

  // Set a decl so subsequent gates can fire on other args.
  D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *decl = NULL;
  check_hr(dev->CreateVertexDeclaration(elements, &decl));
  if (!decl)
    return;
  check_hr(dev->SetVertexDeclaration(decl));

  // PrimitiveCount=0 → D3D_OK no-op.
  check_hr(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 0));

  // PrimitiveCount > MaxPrimitiveCount (0x00FFFFFF). 0x01000000 is
  // explicitly above the cap reported via GetDeviceCaps.
  check_hr_eq(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 0x01000000u), D3DERR_INVALIDCALL);

  // ---- DrawIndexedPrimitive ----
  // No index buffer bound yet.
  check_hr_eq(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1), D3DERR_INVALIDCALL);

  // Bind an IB so subsequent gates can fire on other args.
  IDirect3DIndexBuffer9 *ib = NULL;
  check_hr(dev->CreateIndexBuffer(
      6 * sizeof(uint16_t), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL
  ));
  check_true(ib != NULL);
  if (ib)
    check_hr(dev->SetIndices(ib));

  // PrimitiveCount=0 → D3D_OK.
  check_hr(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 0));

  // PrimitiveCount > Max → INVALIDCALL.
  check_hr_eq(dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, 3, 0, 0x01000000u), D3DERR_INVALIDCALL);

  // ---- DrawPrimitiveUP ----
  const float verts[12] = {0};

  // PrimitiveCount=0 → D3D_OK regardless of pointer / stride.
  check_hr(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 0, NULL, 0));

  // Above-cap → INVALIDCALL.
  check_hr_eq(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 0x01000000u, verts, sizeof(float) * 4), D3DERR_INVALIDCALL);

  // Null pointer with non-zero count → INVALIDCALL.
  check_hr_eq(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, NULL, sizeof(float) * 4), D3DERR_INVALIDCALL);

  // Stride 0 with non-zero count → INVALIDCALL.
  check_hr_eq(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, verts, 0), D3DERR_INVALIDCALL);

  // ---- DrawIndexedPrimitiveUP ----
  const uint16_t indices[3] = {0, 1, 2};

  // PrimitiveCount=0 → D3D_OK.
  check_hr(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 0, indices, D3DFMT_INDEX16, verts, sizeof(float) * 4));

  // NumVertices=0 → D3D_OK (DXVK contract).
  check_hr(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 0, 1, indices, D3DFMT_INDEX16, verts, sizeof(float) * 4));

  // Above-cap → INVALIDCALL.
  check_hr_eq(
      dev->DrawIndexedPrimitiveUP(
          D3DPT_TRIANGLELIST, 0, 3, 0x01000000u, indices, D3DFMT_INDEX16, verts, sizeof(float) * 4
      ),
      D3DERR_INVALIDCALL
  );

  // Null index data → INVALIDCALL.
  check_hr_eq(
      dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, NULL, D3DFMT_INDEX16, verts, sizeof(float) * 4),
      D3DERR_INVALIDCALL
  );

  // Null vertex data → INVALIDCALL.
  check_hr_eq(
      dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, indices, D3DFMT_INDEX16, NULL, sizeof(float) * 4),
      D3DERR_INVALIDCALL
  );

  // Stride 0 → INVALIDCALL.
  check_hr_eq(
      dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, indices, D3DFMT_INDEX16, verts, 0), D3DERR_INVALIDCALL
  );

  // Bad index format (DXT1, R8G8B8, X8R8G8B8 — all non-INDEX*).
  check_hr_eq(
      dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, 3, 1, indices, D3DFMT_X8R8G8B8, verts, sizeof(float) * 4),
      D3DERR_INVALIDCALL
  );

  if (ib)
    ib->Release();
  decl->Release();
  check_zero_losable_count(dev);
}
