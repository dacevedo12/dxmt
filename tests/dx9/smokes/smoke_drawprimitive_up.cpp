// DrawPrimitiveUP smoke. Same shape as smoke_draw_triangle, but the
// vertex data is passed inline via the UP variant — no
// CreateVertexBuffer / Lock / Unlock / SetStreamSource. Validates
// that the helper allocates a transient Shared MTLBuffer, that the
// override path through drawCommonInScene resolves slot 0 from the
// transient (not from m_vertexBuffers[0]), and that the
// setVertexBuffer-at-slot-29 retain pin keeps the buffer alive for
// the GPU to read after DrawPrimitiveUP returns.

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

static const Vertex tri[3] = {
    {-0.9f, -0.9f, 0.0f, 1.0f},
    {0.0f, 0.9f, 0.0f, 1.0f},
    {0.9f, -0.9f, 0.0f, 1.0f},
};

void
test_drawprimitive_up(void) {
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

  // A missing resource early-returns below with no pixel assertion —
  // record the failure so it can't pass vacuously.
  check_true(vs && ps && decl && sys);
  if (!vs || !ps || !decl || !sys) {
    printf("setup: incomplete\n");
    dev->Release();
    d3d->Release();
    return;
  }

  dev->SetVertexShader(vs);
  dev->SetPixelShader(ps);
  dev->SetVertexDeclaration(decl);

  float c0[4] = {1.0f, 0.5f, 0.25f, 1.0f};
  dev->SetPixelShaderConstantF(0, c0, 1);

  const D3DCOLOR clearColor = 0xFF101010u;
  dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);

  HRESULT bs = dev->BeginScene();
  printf("BeginScene: hr=0x%08lx\n", (unsigned long)bs);
  HRESULT dp = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
  printf("DrawPrimitiveUP(TRIANGLELIST,1,tri,stride): hr=0x%08lx\n", (unsigned long)dp);
  HRESULT es = dev->EndScene();
  printf("EndScene: hr=0x%08lx\n", (unsigned long)es);

  HRESULT grh = dev->GetRenderTargetData(rt, sys);
  printf("GetRenderTargetData: hr=0x%08lx\n", (unsigned long)grh);

  D3DLOCKED_RECT lr = {};
  HRESULT lhr = sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  printf("LockRect(sys): hr=0x%08lx pBits=%s\n", (unsigned long)lhr, lr.pBits ? "non-null" : "null");
  check_true(lr.pBits != nullptr);
  if (lr.pBits) {
    DWORD centre = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
    DWORD corner = ((DWORD *)lr.pBits)[0];
    DWORD centre_rgb = centre & 0x00FFFFFFu;
    DWORD corner_rgb = corner & 0x00FFFFFFu;
    DWORD clear_rgb = clearColor & 0x00FFFFFFu;
    // DrawPrimitiveUP covers the centre, leaves the corner clear.
    check_true(centre_rgb != clear_rgb);
    check_true(corner_rgb == clear_rgb);
    sys->UnlockRect();
  }

  // Pre-condition gates — issuing UP outside BeginScene/EndScene
  // must reject; PrimitiveCount==0 is OK; NULL data is INVALIDCALL;
  // zero stride is INVALIDCALL.
  HRESULT outside = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vertex));
  printf("DrawPrimitiveUP outside scene: hr=0x%08lx (must reject)\n", (unsigned long)outside);

  dev->BeginScene();
  HRESULT zero = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 0, tri, sizeof(Vertex));
  printf("DrawPrimitiveUP PrimitiveCount=0: hr=0x%08lx\n", (unsigned long)zero);
  HRESULT null_data = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, NULL, sizeof(Vertex));
  printf("DrawPrimitiveUP NULL data: hr=0x%08lx (must reject)\n", (unsigned long)null_data);
  HRESULT zero_stride = dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, 0);
  printf("DrawPrimitiveUP stride=0: hr=0x%08lx (must reject)\n", (unsigned long)zero_stride);
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
