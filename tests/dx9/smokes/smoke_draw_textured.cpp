// Runtime smoke for textured rendering — validates the PS sampler /
// texture-binding path landed alongside this commit. Pipeline shape:
//
//   - 64×64 X8R8G8B8 RT (CreateRenderTarget, set as RT0).
//   - 2×2 X8R8G8B8 source texture (CreateTexture, MANAGED pool, then
//     LockRect-fill with a recognisable red / green / blue / white
//     pattern across the four texels).
//   - vs_2_0:  dcl_position v0; dcl_texcoord0 v1; mov oPos, v0;
//              mov oT0, v1
//   - ps_2_0:  dcl_2d s0; dcl_texcoord0 v0;
//              texld r0, v0, s0; mov oC0, r0
//   - Vertex declaration: POSITION (FLOAT4) + TEXCOORD0 (FLOAT2).
//   - Three NDC verts forming a centred triangle whose centre pixel
//     falls inside the texture's red texel (uv ≈ (0,0)) under D3D9
//     half-pixel-offset / Metal nearest-sampling.
//   - Sampler state: POINT min/mag/mip + CLAMP — deterministic across
//     filter implementations.
//
// The hash captures: HRESULTs at each stage + ptr-non-null booleans
// + whether the centre pixel matches the clear colour (it must NOT
// — the triangle is covered and a textured fragment is sampled). We
// do not assert the exact RGB readback because rasterizer / sampler
// rounding varies; the only contract we lock here is "the textured
// draw produced a fragment that overrode the clear".

#include "../dx9_smoke.h"
// vs_2_0:
//   dcl_position v0
//   dcl_texcoord0 v1
//   mov oPos, v0
//   mov oT0, v1
//   end
//
// Encoding mirrors smoke_draw_triangle. Texcoord usage = 5 (DxsoUsage),
// usage idx = 0; the dcl info token is 0x80000005. oT0 is TexcoordOut
// (type 6, no high bit), mask .xyzw → 0xE00F0000.
static const DWORD vs_blob[] = {
    0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, // dcl_position v0
    0x0200001Fu, 0x80000005u, 0x900F0001u,              // dcl_texcoord0 v1
    0x02000001u, 0xC00F0000u, 0x90E40000u,              // mov oPos, v0
    0x02000001u, 0xE00F0000u, 0x90E40001u,              // mov oT0, v1
    0x0000FFFFu,
};

// ps_2_0:
//   dcl_2d s0
//   dcl_texcoord0 t0
//   texld r0, t0, s0
//   mov oC0, r0
//   end
//
// dcl_2d s0: dcl info has texture_type=Texture2D(2) in bits 27..30
// → 0x90000000. dst s0: Sampler(10) splits as low=2<<28 (0x20000000)
// + high=1<<11 (0x800), mask 0xF, bit 31 → 0xA00F0800.
//
// SM<3 PS reads texcoord inputs through the Texture register file
// (t#), not the Input register file (v#). v0/v1 are the two COLOR
// registers in ps_2_0; t0..t7 are TEXCOORD0..7. dst t0: Texture(3),
// low=3<<28 (0x30000000), no high bit, mask 0xF, bit 31 → 0xB00F0000.
// src t0 with .xyzw swizzle (0xE4 in bits 16..23) → 0xB0E40000.
//
// Tex (texld) = opcode 66 (0x42), body=3 (dst + 2 srcs). src s0
// follows the same Sampler split + .xyzw swizzle (0xE4) encoding as
// the dst side, but with swizzle in bits 16..23 → 0xA0E40800.
static const DWORD ps_blob[] = {
    0xFFFF0200u, 0x0200001Fu, 0x90000000u, 0xA00F0800u, // dcl_2d s0
    0x0200001Fu, 0x80000005u, 0xB00F0000u,              // dcl_texcoord0 t0
    0x03000042u, 0x800F0000u, 0xB0E40000u, 0xA0E40800u, // texld r0, t0, s0
    0x02000001u, 0x800F0800u, 0x80E40000u,              // mov oC0, r0
    0x0000FFFFu,
};

struct Vertex {
  float x, y, z, w, u, v;
};

// Three NDC verts. The triangle covers (0,0) NDC (centre pixel) and
// the (0,0) corner stays outside. The texcoord at (0,0) NDC sits at
// uv ≈ (0.5, 0.5), which under POINT sampling on a 2×2 texture lands
// in one of the four texels deterministically.
static const Vertex tri[3] = {
    {-0.9f, -0.9f, 0.0f, 1.0f, 0.0f, 0.0f},
    {0.0f, 0.9f, 0.0f, 1.0f, 0.5f, 1.0f},
    {0.9f, -0.9f, 0.0f, 1.0f, 1.0f, 0.0f},
};

void
test_draw_textured(void) {
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
  printf("CreateRenderTarget: out=%s\n", rt ? "ok" : "null");
  if (!rt) {
    dev->Release();
    d3d->Release();
    return;
  }
  dev->SetRenderTarget(0, rt);

  IDirect3DSurface9 *sys = NULL;
  dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
  printf("CreateOffscreenPlainSurface: out=%s\n", sys ? "ok" : "null");

  IDirect3DTexture9 *tex = NULL;
  HRESULT cth = dev->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
  printf("CreateTexture: hr=0x%08lx out=%s\n", (unsigned long)cth, tex ? "ok" : "null");

  if (tex) {
    D3DLOCKED_RECT lr = {};
    HRESULT lrh = tex->LockRect(0, &lr, NULL, 0);
    printf("Tex LockRect: hr=0x%08lx pBits=%s\n", (unsigned long)lrh, lr.pBits ? "non-null" : "null");
    if (lr.pBits) {
      // 2×2 ARGB texels, row-major. Pitch may exceed 8 bytes on
      // platforms that pad rows; index per-row.
      DWORD *row0 = (DWORD *)lr.pBits;
      DWORD *row1 = (DWORD *)((char *)lr.pBits + lr.Pitch);
      row0[0] = 0xFFFF0000u; // red    (texel (0,0))
      row0[1] = 0xFF00FF00u; // green
      row1[0] = 0xFF0000FFu; // blue
      row1[1] = 0xFFFFFFFFu; // white
      tex->UnlockRect(0);
    }
  }

  IDirect3DVertexShader9 *vs = NULL;
  HRESULT cv = dev->CreateVertexShader(vs_blob, &vs);
  printf("CreateVertexShader: hr=0x%08lx out=%s\n", (unsigned long)cv, vs ? "ok" : "null");

  IDirect3DPixelShader9 *ps = NULL;
  HRESULT cp = dev->CreatePixelShader(ps_blob, &ps);
  printf("CreatePixelShader: hr=0x%08lx out=%s\n", (unsigned long)cp, ps ? "ok" : "null");

  D3DVERTEXELEMENT9 elements[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
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
    vb->Lock(0, 0, &p, 0);
    if (p) {
      memcpy(p, tri, sizeof(tri));
      vb->Unlock();
    }
  }

  // Skipping the draw on any null would pass the test vacuously — assert
  // the full setup chain so a regression in shader / texture creation fails.
  check_true(vs && ps && decl && vb && tex && sys);
  if (vs && ps && decl && vb && tex) {
    dev->SetVertexShader(vs);
    dev->SetPixelShader(ps);
    dev->SetVertexDeclaration(decl);
    dev->SetStreamSource(0, vb, 0, sizeof(Vertex));
    dev->SetTexture(0, tex);
    // POINT min/mag/mip — pin the sampling result to a single texel
    // regardless of the rasterizer's pixel-centre convention.
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const D3DCOLOR clearColor = 0xFF101010u;
    dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);

    HRESULT bs = dev->BeginScene();
    HRESULT dp = dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
    HRESULT es = dev->EndScene();
    printf("BeginScene: hr=0x%08lx\n", (unsigned long)bs);
    printf("DrawPrimitive: hr=0x%08lx\n", (unsigned long)dp);
    printf("EndScene: hr=0x%08lx\n", (unsigned long)es);

    if (sys) {
      HRESULT grh = dev->GetRenderTargetData(rt, sys);
      printf("GetRenderTargetData: hr=0x%08lx\n", (unsigned long)grh);
      D3DLOCKED_RECT lr = {};
      sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
      check_true(lr.pBits != nullptr);
      if (lr.pBits) {
        DWORD centre = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
        DWORD corner = ((DWORD *)lr.pBits)[0];
        DWORD centre_rgb = centre & 0x00FFFFFFu;
        DWORD corner_rgb = corner & 0x00FFFFFFu;
        DWORD clear_rgb = clearColor & 0x00FFFFFFu;
        // The textured fragment must override the clear at the centre; the
        // uncovered corner must keep it. Exact texel RGB is left unchecked —
        // the contract here is "a sampled fragment landed", not its value.
        check_true(centre_rgb != clear_rgb);
        check_true(corner_rgb == clear_rgb);
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
  if (tex)
    tex->Release();
  if (sys)
    sys->Release();
  if (rt)
    rt->Release();
  dev->Release();
  d3d->Release();
}
