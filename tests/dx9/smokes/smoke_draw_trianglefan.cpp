// TRIANGLEFAN end-to-end smoke. Metal has no fan topology, so each of
// the four Draw* entry points splits a fan with N vertices into N-2
// (0, k+1, k+2) triangles and dispatches a TRIANGLELIST indexed draw.
// This smoke exercises the three paths most likely to appear in real
// 2D / UI workloads — bound-stream non-indexed (DrawPrimitive),
// vertex-only UP (DrawPrimitiveUP), and fully UP (DrawIndexedPrimitive
// UP) — by drawing a 4-vertex fan that fills the RT, then asserting
// the centre pixel is no longer the clear color. Skipping the bound-
// IB indexed-fan path is deliberate; it requires Lock/Unlock plumbing
// that's irrelevant to validating the fan→list rewrite itself.

#include "../dx9_smoke.h"
// vs_2_0:  dcl_position v0; mov oPos, v0
static const DWORD vs_blob[] = {
    0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x02000001u, 0xC00F0000u, 0x90E40000u, 0x0000FFFFu,
};

// ps_2_0:  mov oC0, c0
static const DWORD ps_blob[] = {
    0xFFFF0200u, 0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu,
};

struct Vertex {
  float x, y, z, w;
};

// Fan covering the full clip-space rect: vertex 0 is the pivot, the
// remaining three sweep around in CCW order. With only 4 vertices the
// fan emits 2 triangles, (0,1,2) and (0,2,3), which together cover
// the whole RT.
static const Vertex fan_quad[4] = {
    {-0.9f, 0.9f, 0.5f, 1.0f},  // top-left  (pivot)
    {0.9f, 0.9f, 0.5f, 1.0f},   // top-right
    {0.9f, -0.9f, 0.5f, 1.0f},  // bottom-right
    {-0.9f, -0.9f, 0.5f, 1.0f}, // bottom-left
};

static const UINT FAN_PRIM_COUNT = 2;
static const UINT FAN_VTX_COUNT = FAN_PRIM_COUNT + 2;

static const D3DCOLOR clearColor = 0xFF101010u;

static bool
centre_is_clear(const D3DLOCKED_RECT &lr) {
  if (!lr.pBits)
    return false;
  DWORD pix = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
  return (pix & 0x00FFFFFFu) == (clearColor & 0x00FFFFFFu);
}

void
test_draw_trianglefan(void) {
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
  if (!rt) {
    dev->Release();
    d3d->Release();
    return;
  }
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

  if (!vs || !ps || !decl || !sys) {
    printf("setup: incomplete\n");
    dev->Release();
    d3d->Release();
    return;
  }

  dev->SetVertexShader(vs);
  dev->SetPixelShader(ps);
  dev->SetVertexDeclaration(decl);

  float c0[4] = {0.25f, 0.75f, 0.5f, 1.0f};
  dev->SetPixelShaderConstantF(0, c0, 1);

  // ---- pass 1: DrawPrimitiveUP(FAN) ----
  dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);
  dev->BeginScene();
  HRESULT dpup = dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, FAN_PRIM_COUNT, fan_quad, sizeof(Vertex));
  dev->EndScene();
  printf("DrawPrimitiveUP(FAN): hr=0x%08lx\n", (unsigned long)dpup);

  dev->GetRenderTargetData(rt, sys);
  D3DLOCKED_RECT lr = {};
  sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  printf("DrawPrimitiveUP(FAN) centre is_clear=%s\n", centre_is_clear(lr) ? "yes" : "no");
  sys->UnlockRect();

  // ---- pass 2: DrawIndexedPrimitiveUP(FAN) ----
  // Author the fan as if it were the source of an indexed draw; the
  // remap path reads pIndexData[0..N-1] and emits (s[0], s[k+1], s[k+2]).
  static const WORD fan_idx[FAN_VTX_COUNT] = {0, 1, 2, 3};
  dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);
  dev->BeginScene();
  HRESULT dipup = dev->DrawIndexedPrimitiveUP(
      D3DPT_TRIANGLEFAN, /*MinVertexIndex=*/0,
      /*NumVertices=*/FAN_VTX_COUNT, FAN_PRIM_COUNT, fan_idx, D3DFMT_INDEX16, fan_quad, sizeof(Vertex)
  );
  dev->EndScene();
  printf("DrawIndexedPrimitiveUP(FAN): hr=0x%08lx\n", (unsigned long)dipup);

  dev->GetRenderTargetData(rt, sys);
  lr = {};
  sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  printf("DrawIndexedPrimitiveUP(FAN) centre is_clear=%s\n", centre_is_clear(lr) ? "yes" : "no");
  sys->UnlockRect();

  // ---- pass 3: DrawPrimitive(FAN) bound stream ----
  IDirect3DVertexBuffer9 *vb = NULL;
  dev->CreateVertexBuffer(sizeof(fan_quad), 0, 0, D3DPOOL_DEFAULT, &vb, NULL);
  if (vb) {
    void *p = NULL;
    vb->Lock(0, sizeof(fan_quad), &p, 0);
    if (p)
      memcpy(p, fan_quad, sizeof(fan_quad));
    vb->Unlock();
    dev->SetStreamSource(0, vb, 0, sizeof(Vertex));
    dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);
    dev->BeginScene();
    HRESULT dp = dev->DrawPrimitive(D3DPT_TRIANGLEFAN, /*StartVertex=*/0, FAN_PRIM_COUNT);
    dev->EndScene();
    printf("DrawPrimitive(FAN, bound VB): hr=0x%08lx\n", (unsigned long)dp);

    dev->GetRenderTargetData(rt, sys);
    lr = {};
    sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
    printf("DrawPrimitive(FAN) centre is_clear=%s\n", centre_is_clear(lr) ? "yes" : "no");
    sys->UnlockRect();
    vb->Release();
  } else {
    printf("CreateVertexBuffer(VB): failed\n");
  }

  // ---- pre-condition gates ----
  // PrimitiveCount == 0 must succeed (no draw, no error). Outside-
  // scene must reject. Both gates apply identically to fans.
  HRESULT outside = dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, FAN_PRIM_COUNT, fan_quad, sizeof(Vertex));
  printf("DrawPrimitiveUP(FAN) outside scene: hr=0x%08lx (must reject)\n", (unsigned long)outside);

  dev->BeginScene();
  HRESULT zero = dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, 0, fan_quad, sizeof(Vertex));
  printf("DrawPrimitiveUP(FAN) PrimitiveCount=0: hr=0x%08lx\n", (unsigned long)zero);
  dev->EndScene();

  decl->Release();
  ps->Release();
  vs->Release();
  sys->Release();
  rt->Release();
  ULONG dev_ref = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dev_ref);
  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
