// First runtime smoke that actually issues a draw — validates the
// DrawPrimitive path landed in feat(d3d9): DrawPrimitive (a5db9b1).
//
// Pipeline shape:
//   - 64×64 X8R8G8B8 RT (CreateRenderTarget, set as RT0).
//   - vs_2_0 / ps_2_0 hand-authored to the minimum useful shape:
//       VS: dcl_position v0; mov oPos, v0
//       PS: mov oC0, c0
//   - Vertex declaration with one POSITION (FLOAT4) at slot 0.
//   - DEFAULT/DYN+WO vertex buffer holding three NDC vertices that
//     form a centred triangle covering the centre pixel but leaving
//     the (0,0) corner outside the triangle.
//   - PS constant c0 set to a known RGB so the triangle fragments
//     output a deterministic colour.
//   - Clear RT to a different colour first; load_action=Load on the
//     DrawPrimitive encoder preserves cleared pixels outside the
//     triangle, so the corner reads back as clearColor.
//   - GetRenderTargetData → SYSTEMMEM surface, LockRect, sample
//     centre and corner pixels.
//
// The hash captures: HRESULTs for each step + ptr-non-null checks +
// whether the centre / corner pixels match their expected colours
// (booleans, not raw values — exact pixel values can drift slightly
// depending on rasterizer rounding, but the centre pixel must NOT
// equal the clear colour and the corner pixel MUST equal the clear
// colour).

#include "../dx9_smoke.h"
// vs_2_0:
//   dcl_position v0
//   mov oPos, v0
//   end
//
// Encoding details — every parameter token has bit 31 set; the
// register-type field is split across bits 11..12 (high) and 28..30
// (low), so types ≥ 8 must split (project_dxso_encoding_gotcha).
//
//  [0] 0xFFFE0200  vs_2_0 header
//  [1] 0x0200001F  Dcl  (opcode=0x1F, body=2 in bits 24..27)
//  [2] 0x80000000  dcl semantic: usage=Position(0), index=0
//  [3] 0x900F0000  dst v0  (type=Input(1) → 1<<28; mask 0xF<<16)
//  [4] 0x02000001  Mov  (opcode=0x01, body=2)
//  [5] 0xC00F0000  dst oPos (type=RasterizerOut(4) → 4<<28; mask 0xF)
//  [6] 0x90E40000  src v0   (type=Input(1) → 1<<28; swizzle 0xE4 = .xyzw)
//  [7] 0x0000FFFF  END
static const DWORD vs_blob[] = {
    0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x02000001u, 0xC00F0000u, 0x90E40000u, 0x0000FFFFu,
};

// ps_2_0:
//   mov oC0, c0
//   end
//
//  [0] 0xFFFF0200  ps_2_0 header
//  [1] 0x02000001  Mov
//  [2] 0x800F0800  dst oC0  (type=ColorOut(8): low=0<<28, high=1<<11; mask 0xF)
//  [3] 0xA0E40000  src c0   (type=Const(2) → 2<<28; swizzle 0xE4)
//  [4] 0x0000FFFF  END
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
test_draw_triangle(void) {
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
  HRESULT crh = dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  printf("CreateRenderTarget: hr=0x%08lx out=%s\n", (unsigned long)crh, rt ? "ok" : "null");
  if (!rt) {
    dev->Release();
    d3d->Release();
    return;
  }
  dev->SetRenderTarget(0, rt);

  IDirect3DSurface9 *sys = NULL;
  dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
  printf("CreateOffscreenPlainSurface: out=%s\n", sys ? "ok" : "null");

  IDirect3DVertexShader9 *vs = NULL;
  HRESULT cv = dev->CreateVertexShader(vs_blob, &vs);
  printf("CreateVertexShader: hr=0x%08lx out=%s\n", (unsigned long)cv, vs ? "ok" : "null");

  IDirect3DPixelShader9 *ps = NULL;
  HRESULT cp = dev->CreatePixelShader(ps_blob, &ps);
  printf("CreatePixelShader: hr=0x%08lx out=%s\n", (unsigned long)cp, ps ? "ok" : "null");

  D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *decl = NULL;
  HRESULT cd = dev->CreateVertexDeclaration(elements, &decl);
  printf("CreateVertexDeclaration: hr=0x%08lx out=%s\n", (unsigned long)cd, decl ? "ok" : "null");

  IDirect3DVertexBuffer9 *vb = NULL;
  HRESULT cvb =
      dev->CreateVertexBuffer(sizeof(tri), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL);
  printf("CreateVertexBuffer: hr=0x%08lx out=%s\n", (unsigned long)cvb, vb ? "ok" : "null");

  if (vb) {
    void *p = NULL;
    HRESULT lhr = vb->Lock(0, 0, &p, 0);
    printf("VB Lock: hr=0x%08lx ptr=%s\n", (unsigned long)lhr, p ? "non-null" : "null");
    if (p) {
      memcpy(p, tri, sizeof(tri));
      vb->Unlock();
    }
  }

  if (vs && ps && decl && vb) {
    dev->SetVertexShader(vs);
    dev->SetPixelShader(ps);
    dev->SetVertexDeclaration(decl);
    dev->SetStreamSource(0, vb, 0, sizeof(Vertex));

    // PS constant c0 — the triangle's flat colour. RGBA with R=1.0,
    // G=0.5, B=0.25, A=1.0 lands at 0x00FF7F40 in X8R8G8B8 readback
    // (alpha is the X channel, undefined). We mask alpha out before
    // comparing to keep the smoke deterministic across drivers.
    float c0[4] = {1.0f, 0.5f, 0.25f, 1.0f};
    dev->SetPixelShaderConstantF(0, c0, 1);

    const D3DCOLOR clearColor = 0xFF101010u; // dark grey
    dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);

    HRESULT bs = dev->BeginScene();
    printf("BeginScene: hr=0x%08lx\n", (unsigned long)bs);
    HRESULT dp = dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    printf("DrawPrimitive(TRIANGLELIST,0,1): hr=0x%08lx\n", (unsigned long)dp);
    HRESULT es = dev->EndScene();
    printf("EndScene: hr=0x%08lx\n", (unsigned long)es);

    if (sys) {
      HRESULT grh = dev->GetRenderTargetData(rt, sys);
      printf("GetRenderTargetData: hr=0x%08lx\n", (unsigned long)grh);

      D3DLOCKED_RECT lr = {};
      HRESULT lhr = sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
      printf("LockRect(sys): hr=0x%08lx pBits=%s\n", (unsigned long)lhr, lr.pBits ? "non-null" : "null");
      if (lr.pBits) {
        // The triangle covers (-0.9,-0.9) to (0.9,0.9) in NDC, with
        // its third vertex at (0, 0.9). The centre pixel (32,32) is
        // inside the triangle regardless of viewport-Y orientation;
        // the corner (0,0) is outside. We print booleans only — raw
        // RGB values would lock the golden to a specific GPU's
        // rasterizer rounding / pixel-centre convention. Booleans
        // capture the only contract DrawPrimitive owes here: the
        // covered pixel changes, the uncovered one doesn't.
        DWORD centre = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
        DWORD corner = ((DWORD *)lr.pBits)[0];
        DWORD centre_rgb = centre & 0x00FFFFFFu;
        DWORD corner_rgb = corner & 0x00FFFFFFu;
        DWORD clear_rgb = clearColor & 0x00FFFFFFu;
        printf("centre(32,32) is_clear=%s\n", centre_rgb == clear_rgb ? "yes" : "no");
        printf("corner(0,0) is_clear=%s\n", corner_rgb == clear_rgb ? "yes" : "no");
        sys->UnlockRect();
      }
    }
  }

  if (vb)
    vb->Release();
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
