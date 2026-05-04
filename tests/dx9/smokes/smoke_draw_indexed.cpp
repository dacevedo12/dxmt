// Runtime smoke for DrawIndexedPrimitive — same shape as
// smoke_draw_triangle but the geometry feeds through an index buffer.
// Four passes, all expected to render the centre triangle:
//   P1 BaseVertexIndex=0 / 16-bit indices [0,1,2] — basic indexed
//      draw, 3-vertex VB.
//   P2 BaseVertexIndex=1 / 16-bit indices [0,1,2] — 4-vertex VB with
//      a dummy at slot 0; Metal resolves baseVertex into [[vertex_id]]
//      so the manual-fetch lowering pulls VB[1..3] = the triangle
//      vertices.
//   P3 BaseVertexIndex=0 / 32-bit indices [0,1,2] — INDEX32 path.
//   PC BaseVertexIndex=0 / 16-bit indices [1,2,3] — same 4-vertex VB
//      as P2 but the offset is in the indices, not baseVertex. Same
//      effective fetch as P2 — kept as a control so a future
//      baseVertex regression doesn't silently move both branches.
//
// Hash captures booleans only (is_clear=yes/no per sampled pixel) for
// the same cross-machine determinism reasons as smoke_draw_triangle.

#include "../dx9_smoke.h"
// vs_2_0: dcl_position v0; mov oPos, v0; end
// (See smoke_draw_triangle.cpp for the per-token encoding walk.)
static const DWORD vs_blob[] = {
    0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x02000001u, 0xC00F0000u, 0x90E40000u, 0x0000FFFFu,
};

// ps_2_0: mov oC0, c0; end
static const DWORD ps_blob[] = {
    0xFFFF0200u, 0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu,
};

struct Vertex {
  float x, y, z, w;
};

// Three triangle vertices used as-is in pass 1 and as the [1..3] tail
// of a 4-vertex VB in pass 2.
static const Vertex tri[3] = {
    {-0.9f, -0.9f, 0.0f, 1.0f},
    {0.0f, 0.9f, 0.0f, 1.0f},
    {0.9f, -0.9f, 0.0f, 1.0f},
};

static void
run_indexed_pass(
    IDirect3DDevice9 *dev, IDirect3DSurface9 *rt, IDirect3DSurface9 *sys, IDirect3DVertexShader9 *vs,
    IDirect3DPixelShader9 *ps, IDirect3DVertexDeclaration9 *decl, IDirect3DVertexBuffer9 *vb, IDirect3DIndexBuffer9 *ib,
    UINT vertex_stride, INT base_vertex_index, const char *label
) {
  // Every pass needs the full resource chain; a null here would draw
  // nothing and pass the pass vacuously.
  check_true(rt && sys && vs && ps && decl && vb && ib);
  dev->SetVertexShader(vs);
  dev->SetPixelShader(ps);
  dev->SetVertexDeclaration(decl);
  dev->SetStreamSource(0, vb, 0, vertex_stride);
  dev->SetIndices(ib);

  float c0[4] = {1.0f, 0.5f, 0.25f, 1.0f};
  dev->SetPixelShaderConstantF(0, c0, 1);

  const D3DCOLOR clearColor = 0xFF101010u;
  dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);

  HRESULT bs = dev->BeginScene();
  printf("%s BeginScene: hr=0x%08lx\n", label, (unsigned long)bs);
  // DrawIndexedPrimitive(type, BaseVertexIndex, MinVertexIndex,
  //                      NumVertices, StartIndex, PrimitiveCount).
  // MinVertexIndex/NumVertices are validation hints; the runtime
  // doesn't need them for the Metal encoder.
  HRESULT dp = dev->DrawIndexedPrimitive(
      D3DPT_TRIANGLELIST, base_vertex_index,
      /*MinVertexIndex=*/0,
      /*NumVertices=*/3,
      /*StartIndex=*/0,
      /*PrimitiveCount=*/1
  );
  printf("%s DrawIndexedPrimitive: hr=0x%08lx\n", label, (unsigned long)dp);
  HRESULT es = dev->EndScene();
  printf("%s EndScene: hr=0x%08lx\n", label, (unsigned long)es);

  HRESULT grh = dev->GetRenderTargetData(rt, sys);
  printf("%s GetRenderTargetData: hr=0x%08lx\n", label, (unsigned long)grh);

  D3DLOCKED_RECT lr = {};
  HRESULT lhr = sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  printf("%s LockRect: hr=0x%08lx pBits=%s\n", label, (unsigned long)lhr, lr.pBits ? "non-null" : "null");
  check_true(lr.pBits != nullptr);
  if (lr.pBits) {
    DWORD centre = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
    DWORD corner = ((DWORD *)lr.pBits)[0];
    DWORD centre_rgb = centre & 0x00FFFFFFu;
    DWORD corner_rgb = corner & 0x00FFFFFFu;
    DWORD clear_rgb = clearColor & 0x00FFFFFFu;
    // Indexed draw covers the centre, leaves the corner clear (the label
    // printf above identifies which base-vertex variant this pass is).
    check_true(centre_rgb != clear_rgb);
    check_true(corner_rgb == clear_rgb);
    sys->UnlockRect();
  }
}

void
test_draw_indexed(void) {
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

  IDirect3DSurface9 *rt = NULL;
  dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  if (rt)
    dev->SetRenderTarget(0, rt);

  IDirect3DSurface9 *sys = NULL;
  dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);

  IDirect3DVertexShader9 *vs = NULL;
  dev->CreateVertexShader(vs_blob, &vs);
  IDirect3DPixelShader9 *ps = NULL;
  dev->CreatePixelShader(ps_blob, &ps);

  D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *decl = NULL;
  dev->CreateVertexDeclaration(elements, &decl);

  IDirect3DVertexBuffer9 *vb_three = NULL;
  IDirect3DVertexBuffer9 *vb_four = NULL;
  IDirect3DIndexBuffer9 *ib16 = NULL;
  IDirect3DIndexBuffer9 *ib32 = NULL;

  // Pass 1: 3-vertex VB, 16-bit indices [0,1,2].
  dev->CreateVertexBuffer(sizeof(tri), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_three, NULL);
  if (vb_three) {
    void *p = NULL;
    if (SUCCEEDED(vb_three->Lock(0, 0, &p, 0)) && p) {
      memcpy(p, tri, sizeof(tri));
      vb_three->Unlock();
    }
  }
  dev->CreateIndexBuffer(
      3 * sizeof(uint16_t), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib16, NULL
  );
  if (ib16) {
    void *p = NULL;
    if (SUCCEEDED(ib16->Lock(0, 0, &p, 0)) && p) {
      uint16_t idx[3] = {0, 1, 2};
      memcpy(p, idx, sizeof(idx));
      ib16->Unlock();
    }
  }

  // Pass 2: 4-vertex VB (dummy + tri), 16-bit indices [0,1,2],
  // BaseVertexIndex=1 so vertex_id+base_vertex selects the same
  // triangle vertices as pass 1.
  dev->CreateVertexBuffer(
      4 * sizeof(Vertex), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_four, NULL
  );
  if (vb_four) {
    void *p = NULL;
    if (SUCCEEDED(vb_four->Lock(0, 0, &p, 0)) && p) {
      Vertex padded[4];
      // Slot 0: a dummy off-screen vertex. It must NOT be one of the
      // triangle vertices; if base_vertex is misapplied (e.g. silently
      // ignored) the draw would pick this up and the centre pixel
      // would land outside the (now-mis-rotated) triangle.
      padded[0] = {-5.0f, -5.0f, 0.0f, 1.0f};
      padded[1] = tri[0];
      padded[2] = tri[1];
      padded[3] = tri[2];
      memcpy(p, padded, sizeof(padded));
      vb_four->Unlock();
    }
  }

  // Also exercise INDEX32 to cover both index_type branches.
  dev->CreateIndexBuffer(
      3 * sizeof(uint32_t), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &ib32, NULL
  );
  if (ib32) {
    void *p = NULL;
    if (SUCCEEDED(ib32->Lock(0, 0, &p, 0)) && p) {
      uint32_t idx[3] = {0, 1, 2};
      memcpy(p, idx, sizeof(idx));
      ib32->Unlock();
    }
  }

  // Control: 4-vertex VB with indices [1,2,3] / BaseVertexIndex=0.
  // Equivalent to P2 in *effective* vertex selection but bypasses the
  // base_vertex path entirely. If P2 fails and ctrl works, the issue
  // is in base_vertex wiring, not VB/draw shape.
  IDirect3DIndexBuffer9 *ib16_ctrl = NULL;
  dev->CreateIndexBuffer(
      3 * sizeof(uint16_t), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib16_ctrl, NULL
  );
  if (ib16_ctrl) {
    void *p = NULL;
    if (SUCCEEDED(ib16_ctrl->Lock(0, 0, &p, 0)) && p) {
      uint16_t idx[3] = {1, 2, 3};
      memcpy(p, idx, sizeof(idx));
      ib16_ctrl->Unlock();
    }
  }

  if (vs && ps && decl && vb_three && ib16) {
    run_indexed_pass(dev, rt, sys, vs, ps, decl, vb_three, ib16, sizeof(Vertex), /*BaseVertexIndex=*/0, "P1[16i,bv=0]");
  }
  if (vs && ps && decl && vb_four && ib16) {
    run_indexed_pass(dev, rt, sys, vs, ps, decl, vb_four, ib16, sizeof(Vertex), /*BaseVertexIndex=*/1, "P2[16i,bv=1]");
  }
  if (vs && ps && decl && vb_three && ib32) {
    run_indexed_pass(dev, rt, sys, vs, ps, decl, vb_three, ib32, sizeof(Vertex), /*BaseVertexIndex=*/0, "P3[32i,bv=0]");
  }
  if (vs && ps && decl && vb_four && ib16_ctrl) {
    run_indexed_pass(
        dev, rt, sys, vs, ps, decl, vb_four, ib16_ctrl, sizeof(Vertex), /*BaseVertexIndex=*/0, "PC[idx=1..3,bv=0]"
    );
  }

  if (ib16_ctrl)
    ib16_ctrl->Release();
  if (ib32)
    ib32->Release();
  if (ib16)
    ib16->Release();
  if (vb_four)
    vb_four->Release();
  if (vb_three)
    vb_three->Release();
  if (decl)
    decl->Release();
  if (ps)
    ps->Release();
  if (vs)
    vs->Release();
  if (sys)
    sys->Release();
  if (rt)
    rt->Release();
  ULONG dr = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dr);
  ULONG ir = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ir);
}
