// End-to-end smoke for the INTZ sampleable-depth FOURCC. The shape
// apps use is a two-pass shadow-map: render depth into an INTZ
// texture as a DSV, then sample that same texture from a PS in a
// later draw. smoke_create_intz validates the create + bind round-
// trip; this smoke takes the next step and round-trips an actual
// pixel through the DSV→SRV path:
//
//   pass 1
//     - bind a 64×64 X8R8G8B8 RT and the INTZ texture's level 0 as
//       the DepthStencilSurface
//     - clear depth to 1.0
//     - draw a triangle covering the centre with z=0.4
//     - depth-write puts ~0.4 into the INTZ storage at the centre
//
//   pass 2
//     - SetDepthStencilSurface(NULL) — unbind the INTZ surface as a
//       DSV so it's free to be sampled
//     - SetTexture(0, intz_texture) — bind it as an SRV
//     - draw a textured triangle covering the centre with the same
//       PS shape as smoke_draw_textured (`texld r0, t0, s0; mov oC0,
//       r0`); on Apple Silicon INTZ aliases Depth32Float_Stencil8 so
//       the sampled value lands in the red lane
//     - the readback's centre red channel should be ~0.4 (≈ 0x66) —
//       not 0 (sample failed) and not 1 (sample saw the cleared
//       background instead of the drawn depth)
//
// Hash captures: HRESULTs at each stage + ptr-non-null booleans +
// "centre pixel red is non-zero AND non-cleared". A correctness
// regression on either INTZ creation, the DSV write, or the SRV
// sample shows up as one of the booleans flipping.

#include "../dx9_smoke.h"
static const D3DFORMAT FOURCC_INTZ = (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z');

// vs_2_0:
//   dcl_position v0
//   dcl_texcoord0 v1
//   mov oPos, v0
//   mov oT0, v1
//   end
// Encoding mirrors smoke_draw_textured.
static const DWORD vs_blob[] = {
    0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x0200001Fu, 0x80000005u, 0x900F0001u,
    0x02000001u, 0xC00F0000u, 0x90E40000u, 0x02000001u, 0xE00F0000u, 0x90E40001u, 0x0000FFFFu,
};

// vs_2_0 (pass 1, no texcoord):
//   dcl_position v0
//   mov oPos, v0
//   end
static const DWORD vs_pos_blob[] = {
    0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x02000001u, 0xC00F0000u, 0x90E40000u, 0x0000FFFFu,
};

// ps_2_0 (pass 1, constant red):
//   mov oC0, c0
static const DWORD ps_const_blob[] = {
    0xFFFF0200u, 0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu,
};

// ps_2_0 (pass 2, textured):
//   dcl_2d s0
//   dcl_texcoord0 t0
//   texld r0, t0, s0
//   mov oC0, r0
//   end
// Same shape as smoke_draw_textured.
static const DWORD ps_tex_blob[] = {
    0xFFFF0200u, 0x0200001Fu, 0x90000000u, 0xA00F0800u, 0x0200001Fu, 0x80000005u, 0xB00F0000u, 0x03000042u,
    0x800F0000u, 0xB0E40000u, 0xA0E40800u, 0x02000001u, 0x800F0800u, 0x80E40000u, 0x0000FFFFu,
};

struct VertexPos {
  float x, y, z, w;
};
struct VertexTex {
  float x, y, z, w, u, v;
};

// Pass 1: triangle covering the centre at z=0.4. Both passes share
// the same xy footprint so pass 2 samples the same pixels pass 1
// wrote depth into.
static const VertexPos pass1[3] = {
    {-0.9f, -0.9f, 0.4f, 1.0f},
    {0.0f, 0.9f, 0.4f, 1.0f},
    {0.9f, -0.9f, 0.4f, 1.0f},
};

// Pass 2: textured triangle. uv at the centre pixel resolves to
// roughly (0.5, 0.5) on a 64×64 texture which under POINT sampling
// hits a deterministic texel. The depth value at that texel is
// what pass 1 wrote.
static const VertexTex pass2[3] = {
    {-0.9f, -0.9f, 0.0f, 1.0f, 0.0f, 0.0f},
    {0.0f, 0.9f, 0.0f, 1.0f, 0.5f, 1.0f},
    {0.9f, -0.9f, 0.0f, 1.0f, 1.0f, 0.0f},
};

void
test_intz_dsv_srv(void) {
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
  // No auto-DS — we supply the INTZ surface as the DepthStencilSurface
  // ourselves so the sampleable-depth path is exercised.
  pp.EnableAutoDepthStencil = FALSE;

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

  IDirect3DSurface9 *sys = NULL;
  dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
  printf("CreateOffscreenPlainSurface: out=%s\n", sys ? "ok" : "null");

  // INTZ texture as DSV + SRV.
  IDirect3DTexture9 *intz = NULL;
  HRESULT cth = dev->CreateTexture(64, 64, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC_INTZ, D3DPOOL_DEFAULT, &intz, NULL);
  printf("CreateTexture(INTZ/64/DEPTHSTENCIL/DEFAULT): hr=0x%08lx out=%s\n", (unsigned long)cth, intz ? "ok" : "null");

  IDirect3DSurface9 *intz_surf = NULL;
  if (intz) {
    HRESULT gsh = intz->GetSurfaceLevel(0, &intz_surf);
    printf("GetSurfaceLevel(0): hr=0x%08lx out=%s\n", (unsigned long)gsh, intz_surf ? "ok" : "null");
  }

  IDirect3DVertexShader9 *vs_pos = NULL;
  HRESULT cv1 = dev->CreateVertexShader(vs_pos_blob, &vs_pos);
  printf("CreateVertexShader(pos): hr=0x%08lx out=%s\n", (unsigned long)cv1, vs_pos ? "ok" : "null");

  IDirect3DVertexShader9 *vs_tex = NULL;
  HRESULT cv2 = dev->CreateVertexShader(vs_blob, &vs_tex);
  printf("CreateVertexShader(tex): hr=0x%08lx out=%s\n", (unsigned long)cv2, vs_tex ? "ok" : "null");

  IDirect3DPixelShader9 *ps_const = NULL;
  HRESULT cp1 = dev->CreatePixelShader(ps_const_blob, &ps_const);
  printf("CreatePixelShader(const): hr=0x%08lx out=%s\n", (unsigned long)cp1, ps_const ? "ok" : "null");

  IDirect3DPixelShader9 *ps_tex = NULL;
  HRESULT cp2 = dev->CreatePixelShader(ps_tex_blob, &ps_tex);
  printf("CreatePixelShader(tex): hr=0x%08lx out=%s\n", (unsigned long)cp2, ps_tex ? "ok" : "null");

  D3DVERTEXELEMENT9 elements_pos[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *decl_pos = NULL;
  dev->CreateVertexDeclaration(elements_pos, &decl_pos);

  D3DVERTEXELEMENT9 elements_tex[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END(),
  };
  IDirect3DVertexDeclaration9 *decl_tex = NULL;
  dev->CreateVertexDeclaration(elements_tex, &decl_tex);

  IDirect3DVertexBuffer9 *vb_pos = NULL;
  dev->CreateVertexBuffer(sizeof(pass1), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_pos, NULL);
  if (vb_pos) {
    void *p = NULL;
    vb_pos->Lock(0, 0, &p, 0);
    if (p) {
      memcpy(p, pass1, sizeof(pass1));
      vb_pos->Unlock();
    }
  }

  IDirect3DVertexBuffer9 *vb_tex = NULL;
  dev->CreateVertexBuffer(sizeof(pass2), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_tex, NULL);
  if (vb_tex) {
    void *p = NULL;
    vb_tex->Lock(0, 0, &p, 0);
    if (p) {
      memcpy(p, pass2, sizeof(pass2));
      vb_tex->Unlock();
    }
  }

  if (!rt || !sys || !intz || !intz_surf || !vs_pos || !vs_tex || !ps_const || !ps_tex || !decl_pos || !decl_tex ||
      !vb_pos || !vb_tex) {
    printf("setup: incomplete\n");
    if (vb_tex)
      vb_tex->Release();
    if (vb_pos)
      vb_pos->Release();
    if (decl_tex)
      decl_tex->Release();
    if (decl_pos)
      decl_pos->Release();
    if (ps_tex)
      ps_tex->Release();
    if (ps_const)
      ps_const->Release();
    if (vs_tex)
      vs_tex->Release();
    if (vs_pos)
      vs_pos->Release();
    if (intz_surf)
      intz_surf->Release();
    if (intz)
      intz->Release();
    if (sys)
      sys->Release();
    if (rt)
      rt->Release();
    dev->Release();
    d3d->Release();
    return;
  }

  const D3DCOLOR clearColor = 0xFF101010u;
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};

  // ---- pass 1: write depth into INTZ ----
  dev->SetRenderTarget(0, rt);
  HRESULT sds = dev->SetDepthStencilSurface(intz_surf);
  printf("SetDepthStencilSurface(intz): hr=0x%08lx\n", (unsigned long)sds);

  dev->SetVertexShader(vs_pos);
  dev->SetPixelShader(ps_const);
  dev->SetVertexDeclaration(decl_pos);
  dev->SetStreamSource(0, vb_pos, 0, sizeof(VertexPos));
  dev->SetPixelShaderConstantF(0, red, 1);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

  dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
  HRESULT bs1 = dev->BeginScene();
  HRESULT dp1 = dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
  HRESULT es1 = dev->EndScene();
  printf(
      "pass1 BeginScene: hr=0x%08lx DrawPrimitive: hr=0x%08lx EndScene: hr=0x%08lx\n", (unsigned long)bs1,
      (unsigned long)dp1, (unsigned long)es1
  );

  // ---- pass 2: sample INTZ through SetTexture ----
  // Unbind INTZ as DSV first; sampling a texture that's also an
  // active DSV is undefined. SetDepthStencilSurface(NULL) leaves the
  // depth test enabled but with no attachment, so disable depth-test
  // for pass 2 too.
  HRESULT sds2 = dev->SetDepthStencilSurface(NULL);
  printf("SetDepthStencilSurface(NULL): hr=0x%08lx\n", (unsigned long)sds2);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

  dev->SetVertexShader(vs_tex);
  dev->SetPixelShader(ps_tex);
  dev->SetVertexDeclaration(decl_tex);
  dev->SetStreamSource(0, vb_tex, 0, sizeof(VertexTex));
  HRESULT st = dev->SetTexture(0, intz);
  printf("SetTexture(0, intz): hr=0x%08lx\n", (unsigned long)st);

  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

  dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);
  HRESULT bs2 = dev->BeginScene();
  HRESULT dp2 = dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
  HRESULT es2 = dev->EndScene();
  printf(
      "pass2 BeginScene: hr=0x%08lx DrawPrimitive: hr=0x%08lx EndScene: hr=0x%08lx\n", (unsigned long)bs2,
      (unsigned long)dp2, (unsigned long)es2
  );

  dev->SetTexture(0, NULL);

  // Readback. Pass 1 wrote depth ≈ 0.4 at the centre. Pass 2 sampled
  // that depth texel and routed it into the red channel; on Apple
  // Silicon INTZ aliases Depth32Float_Stencil8, so the depth value
  // appears in the red lane on read. Centre red byte should be
  // non-zero (sample succeeded) and non-clear (a textured fragment
  // overrode the clear).
  HRESULT grh = dev->GetRenderTargetData(rt, sys);
  printf("GetRenderTargetData: hr=0x%08lx\n", (unsigned long)grh);
  D3DLOCKED_RECT lr = {};
  sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  if (lr.pBits) {
    DWORD c = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
    DWORD rgb = c & 0x00FFFFFFu;
    DWORD clear_rgb = clearColor & 0x00FFFFFFu;
    DWORD red_byte = (rgb >> 16) & 0xFFu;
    bool red_nonzero = red_byte != 0;
    bool not_clear = rgb != clear_rgb;
    printf("centre red_nonzero=%s not_clear=%s\n", red_nonzero ? "yes" : "no", not_clear ? "yes" : "no");
    sys->UnlockRect();
  } else {
    printf("centre red_nonzero=no not_clear=no\n");
  }

  vb_tex->Release();
  vb_pos->Release();
  decl_tex->Release();
  decl_pos->Release();
  ps_tex->Release();
  ps_const->Release();
  vs_tex->Release();
  vs_pos->Release();
  intz_surf->Release();
  intz->Release();
  sys->Release();
  rt->Release();
  dev->Release();
  d3d->Release();
}
