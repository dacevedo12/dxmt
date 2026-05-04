// Hardware-PCF shadow sampling end-to-end (#233). The sibling of
// smoke_intz_dsv_srv: same two-pass shadow-map shape, but the depth
// texture is D3DFMT_D24S8 (a hardware-PCF format) instead of INTZ (raw
// depth). Binding a HW-PCF depth texture to a sampler must make `texld`
// emit Metal sample_compare against a LessEqual sampler, so the result
// is the filtered depth COMPARISON (lit/shadow 0..1), not the raw stored
// depth.
//
//   pass 1: render depth 0.4 into the D24S8 texture (as DSV) at the centre.
//   pass 2: bind it as an SRV and `texld r0, t0, s0` where t0.z = 0.2 is
//           the comparison reference. With LessEqual PCF the result is
//           (0.2 <= 0.4) = 1.0 (lit), routed to the red channel.
//
// The discriminator vs raw depth: a raw-depth sample would read the
// stored 0.4 (red ~0x66); the PCF comparison reads ~1.0 (red ~0xFF). So
// the centre red byte must be HIGH (>0xC0) — that proves sample_compare
// ran and compared, not that it returned raw depth. A compile/PSO-link
// failure of the sample_compare variant would instead leave the clear
// colour (the draw never lands).

#include "../dx9_smoke.h"

// vs_2_0: dcl_position v0; dcl_texcoord0 v1; mov oPos,v0; mov oT0,v1
static const DWORD vs_blob[] = {
    0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x0200001Fu, 0x80000005u, 0x900F0001u,
    0x02000001u, 0xC00F0000u, 0x90E40000u, 0x02000001u, 0xE00F0000u, 0x90E40001u, 0x0000FFFFu,
};
// vs_2_0 (pass 1, position only): dcl_position v0; mov oPos,v0
static const DWORD vs_pos_blob[] = {
    0xFFFE0200u, 0x0200001Fu, 0x80000000u, 0x900F0000u, 0x02000001u, 0xC00F0000u, 0x90E40000u, 0x0000FFFFu,
};
// ps_2_0 (pass 1, constant): mov oC0,c0
static const DWORD ps_const_blob[] = {
    0xFFFF0200u, 0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu,
};
// ps_2_0 (pass 2): dcl_2d s0; dcl_texcoord0 t0; texld r0,t0,s0; mov oC0,r0
static const DWORD ps_tex_blob[] = {
    0xFFFF0200u, 0x0200001Fu, 0x90000000u, 0xA00F0800u, 0x0200001Fu, 0x80000005u, 0xB00F0000u, 0x03000042u,
    0x800F0000u, 0xB0E40000u, 0xA0E40800u, 0x02000001u, 0x800F0800u, 0x80E40000u, 0x0000FFFFu,
};

struct VertexPos {
  float x, y, z, w;
};
// Texcoord is FLOAT4 so .z carries the shadow comparison reference.
struct VertexTex {
  float x, y, z, w, u, v, refz, tw;
};

static const VertexPos pass1[3] = {{-0.9f, -0.9f, 0.4f, 1.0f}, {0.0f, 0.9f, 0.4f, 1.0f}, {0.9f, -0.9f, 0.4f, 1.0f}};
// refz = 0.2 < stored 0.4 → LessEqual PCF returns 1.0 (lit).
static const VertexTex pass2[3] = {
    {-0.9f, -0.9f, 0.0f, 1.0f, 0.0f, 0.0f, 0.2f, 1.0f},
    {0.0f, 0.9f, 0.0f, 1.0f, 0.5f, 1.0f, 0.2f, 1.0f},
    {0.9f, -0.9f, 0.0f, 1.0f, 1.0f, 0.0f, 0.2f, 1.0f}
};

void
test_shadow_pcf(void) {
  Dx9Fixture fx;
  if (!fx.create(64, 64, D3DFMT_X8R8G8B8))
    return;
  IDirect3DDevice9 *dev = fx.dev;

  IDirect3DSurface9 *rt = nullptr;
  dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  IDirect3DSurface9 *sys = nullptr;
  dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);

  // D24S8 depth texture as DSV + SRV (the HW-PCF path; COD4's shadow map).
  IDirect3DTexture9 *shadow = nullptr;
  HRESULT cth = dev->CreateTexture(64, 64, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &shadow, NULL);
  check_hr(cth);
  check_true(shadow != nullptr);
  IDirect3DSurface9 *shadow_surf = nullptr;
  if (shadow)
    check_hr(shadow->GetSurfaceLevel(0, &shadow_surf));

  IDirect3DVertexShader9 *vs_pos = nullptr, *vs_tex = nullptr;
  check_hr(dev->CreateVertexShader(vs_pos_blob, &vs_pos));
  check_hr(dev->CreateVertexShader(vs_blob, &vs_tex));
  IDirect3DPixelShader9 *ps_const = nullptr, *ps_tex = nullptr;
  check_hr(dev->CreatePixelShader(ps_const_blob, &ps_const));
  check_hr(dev->CreatePixelShader(ps_tex_blob, &ps_tex));

  D3DVERTEXELEMENT9 elements_pos[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()
  };
  IDirect3DVertexDeclaration9 *decl_pos = nullptr;
  dev->CreateVertexDeclaration(elements_pos, &decl_pos);
  D3DVERTEXELEMENT9 elements_tex[] = {
      {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()
  };
  IDirect3DVertexDeclaration9 *decl_tex = nullptr;
  dev->CreateVertexDeclaration(elements_tex, &decl_tex);

  IDirect3DVertexBuffer9 *vb_pos = nullptr, *vb_tex = nullptr;
  dev->CreateVertexBuffer(sizeof(pass1), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_pos, NULL);
  if (vb_pos) {
    void *p = nullptr;
    vb_pos->Lock(0, 0, &p, 0);
    if (p) {
      memcpy(p, pass1, sizeof(pass1));
      vb_pos->Unlock();
    }
  }
  dev->CreateVertexBuffer(sizeof(pass2), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_tex, NULL);
  if (vb_tex) {
    void *p = nullptr;
    vb_tex->Lock(0, 0, &p, 0);
    if (p) {
      memcpy(p, pass2, sizeof(pass2));
      vb_tex->Unlock();
    }
  }

  if (!rt || !sys || !shadow || !shadow_surf || !vs_pos || !vs_tex || !ps_const || !ps_tex || !decl_pos || !decl_tex ||
      !vb_pos || !vb_tex) {
    release_object(&vb_tex);
    release_object(&vb_pos);
    release_object(&decl_tex);
    release_object(&decl_pos);
    release_object(&ps_tex);
    release_object(&ps_const);
    release_object(&vs_tex);
    release_object(&vs_pos);
    release_object(&shadow_surf);
    release_object(&shadow);
    release_object(&sys);
    release_object(&rt);
    return;
  }

  const D3DCOLOR clearColor = 0xFF101010u;
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};

  // ---- pass 1: write depth 0.4 into the D24S8 texture. ----
  dev->SetRenderTarget(0, rt);
  check_hr(dev->SetDepthStencilSurface(shadow_surf));
  dev->SetVertexShader(vs_pos);
  dev->SetPixelShader(ps_const);
  dev->SetVertexDeclaration(decl_pos);
  dev->SetStreamSource(0, vb_pos, 0, sizeof(VertexPos));
  dev->SetPixelShaderConstantF(0, red, 1);
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
  dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
  check_hr(dev->BeginScene());
  check_hr(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
  check_hr(dev->EndScene());

  // ---- pass 2: bind D24S8 as SRV and sample_compare it. ----
  check_hr(dev->SetDepthStencilSurface(NULL));
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
  dev->SetVertexShader(vs_tex);
  dev->SetPixelShader(ps_tex);
  dev->SetVertexDeclaration(decl_tex);
  dev->SetStreamSource(0, vb_tex, 0, sizeof(VertexTex));
  check_hr(dev->SetTexture(0, shadow));
  dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  dev->Clear(0, NULL, D3DCLEAR_TARGET, clearColor, 0.0f, 0);
  check_hr(dev->BeginScene());
  check_hr(dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1));
  check_hr(dev->EndScene());
  dev->SetTexture(0, NULL);

  // Readback: centre red is the PCF comparison (ref 0.2 <= stored 0.4 →
  // lit → ~1.0 → ~0xFF). HIGH red proves sample_compare ran and compared.
  // Raw-depth sampling would read the stored 0.4 (~0x66); a compile/link
  // failure would leave the clear colour (red 0x10).
  check_hr(dev->GetRenderTargetData(rt, sys));
  D3DLOCKED_RECT lr = {};
  sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  check_true(lr.pBits != nullptr);
  if (lr.pBits) {
    DWORD c = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
    DWORD red_byte = (c >> 16) & 0xFFu;
    // Lit PCF result is ~1.0; require HIGH (>0xC0) to distinguish from the
    // raw stored depth (~0x66) and the clear colour (0x10).
    check_true(red_byte > 0xC0);
    sys->UnlockRect();
  }

  vb_tex->Release();
  vb_pos->Release();
  decl_tex->Release();
  decl_pos->Release();
  ps_tex->Release();
  ps_const->Release();
  vs_tex->Release();
  vs_pos->Release();
  shadow_surf->Release();
  shadow->Release();
  sys->Release();
  rt->Release();
}
