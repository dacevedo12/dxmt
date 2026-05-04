// Runtime smoke for the auto-DS + depth-test path. Two overlapping
// triangles drawn into the same pixels at different z; the LESSEQUAL
// depth test must keep the closer fragment regardless of submission
// order. Validates:
//
//   1. CreateDevice with EnableAutoDepthStencil=TRUE allocates the
//      implicit DS surface (commit 85a725f).
//   2. DrawPrimitive attaches the DS to the render pass and binds an
//      MTLDepthStencilState that reflects D3DRS_ZENABLE / ZFUNC /
//      ZWRITEENABLE (commit 976c1c6).
//   3. Clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER) prepares the DS for
//      a meaningful test.
//
// Two passes:
//   pass 1 — back-to-front: draw GREEN at z=0.7 first, then RED at
//            z=0.3. Both triangles cover (32,32). The RED draw wins
//            because 0.3 ≤ 0.3 and beats the cleared 1.0; the GREEN
//            draw also wrote (since 0.7 ≤ 1.0), but RED comes after
//            and 0.3 ≤ 0.7 also passes — final pixel is RED.
//   pass 2 — front-to-back: RED at z=0.3 first, then GREEN at z=0.7.
//            RED writes (0.3 ≤ 1.0 = depth at clear). GREEN's
//            LESSEQUAL test then runs 0.7 against the depth buffer
//            value 0.3 — fails, GREEN is rejected. Final pixel is
//            still RED.
//
// Both passes assert "centre pixel is RED, not GREEN, not clear".
// If depth test were broken (always-pass): pass 1 would still be
// RED (last draw wins) but pass 2 would be GREEN (last draw wins).
// If DS attachment were missing: pass 2 falls back to last-draw-
// wins → GREEN.
//
// We print booleans only (matches smoke_draw_triangle's
// determinism rationale).

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

// Two triangles overlapping at the centre, different z. CW order so
// back-face culling (default D3DCULL_CCW) keeps them visible.
// Layout: indices 0..2 = RED at z=0.3, indices 3..5 = GREEN at z=0.7.
// Both triangles get pre-uploaded to a single VB, then each draw
// picks its triangle via StartVertex. Going through Lock/Unlock per
// draw with one shared dynamic VB races against in-flight cmdbufs:
// the second Lock's memcpy can clobber vertices the first cmdbuf
// hasn't read yet.
static const Vertex verts[6] = {
    // RED (z=0.3)
    {-0.9f, -0.9f, 0.3f, 1.0f},
    {0.0f, 0.9f, 0.3f, 1.0f},
    {0.9f, -0.9f, 0.3f, 1.0f},
    // GREEN (z=0.7)
    {-0.9f, -0.9f, 0.7f, 1.0f},
    {0.0f, 0.9f, 0.7f, 1.0f},
    {0.9f, -0.9f, 0.7f, 1.0f},
};

static void
draw_one(IDirect3DDevice9 *dev, UINT startVertex, const float *color, const char *label) {
  dev->SetPixelShaderConstantF(0, color, 1);
  HRESULT hr = dev->DrawPrimitive(D3DPT_TRIANGLELIST, startVertex, 1);
  printf("%s DrawPrimitive: hr=0x%08lx\n", label, (unsigned long)hr);
}

void
test_depth_test(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d)
    return;

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 64;
  pp.BackBufferHeight = 64;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
  pp.EnableAutoDepthStencil = TRUE;
  pp.AutoDepthStencilFormat = D3DFMT_D24S8;

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

  IDirect3DVertexBuffer9 *vb = NULL;
  dev->CreateVertexBuffer(sizeof(verts), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, NULL);

  if (!vs || !ps || !decl || !vb || !sys) {
    printf("setup: incomplete\n");
    dev->Release();
    d3d->Release();
    return;
  }

  // One-shot upload of both triangles. After this the VB is read-only
  // for the rest of the smoke, so no Lock-vs-cmdbuf race.
  {
    void *p = NULL;
    vb->Lock(0, 0, &p, 0);
    if (p) {
      memcpy(p, verts, sizeof(verts));
      vb->Unlock();
    }
  }

  dev->SetVertexShader(vs);
  dev->SetPixelShader(ps);
  dev->SetVertexDeclaration(decl);
  dev->SetStreamSource(0, vb, 0, sizeof(Vertex));

  // ZENABLE / ZFUNC / ZWRITEENABLE all default to enabled / LESSEQUAL
  // / TRUE on the auto-DS path; spell them out anyway so the smoke
  // doesn't silently start passing if those defaults flip later.
  dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);

  const D3DCOLOR clearColor = 0xFF101010u; // dark grey
  const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  const float green[4] = {0.0f, 1.0f, 0.0f, 1.0f};

  // ---- pass 1: GREEN first, RED second ----
  dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
  HRESULT bs = dev->BeginScene();
  draw_one(dev, /*StartVertex=*/3, green, "pass1.green");
  draw_one(dev, /*StartVertex=*/0, red, "pass1.red");
  HRESULT es = dev->EndScene();
  printf("pass1 BeginScene: hr=0x%08lx EndScene: hr=0x%08lx\n", (unsigned long)bs, (unsigned long)es);
  {
    dev->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr = {};
    sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
    check_true(lr.pBits != nullptr);
    if (lr.pBits) {
      DWORD c = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
      // X8R8G8B8: 0x00FF0000 = red, 0x0000FF00 = green.
      DWORD rgb = c & 0x00FFFFFFu;
      bool is_red = (rgb & 0x00FF0000u) > 0x00800000u && (rgb & 0x0000FF00u) < 0x00008000u;
      bool is_green = (rgb & 0x0000FF00u) > 0x00008000u && (rgb & 0x00FF0000u) < 0x00800000u;
      // Red is the nearer primitive; drawn second here, it must still win
      // the depth test and overwrite the green drawn first.
      check_true(is_red);
      check_true(!is_green);
      sys->UnlockRect();
    }
  }

  // ---- pass 2: RED first, GREEN second ----
  dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
  bs = dev->BeginScene();
  draw_one(dev, /*StartVertex=*/0, red, "pass2.red");
  draw_one(dev, /*StartVertex=*/3, green, "pass2.green");
  es = dev->EndScene();
  printf("pass2 BeginScene: hr=0x%08lx EndScene: hr=0x%08lx\n", (unsigned long)bs, (unsigned long)es);
  {
    dev->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr = {};
    sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
    check_true(lr.pBits != nullptr);
    if (lr.pBits) {
      DWORD c = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
      DWORD rgb = c & 0x00FFFFFFu;
      bool is_red = (rgb & 0x00FF0000u) > 0x00800000u && (rgb & 0x0000FF00u) < 0x00008000u;
      bool is_green = (rgb & 0x0000FF00u) > 0x00008000u && (rgb & 0x00FF0000u) < 0x00800000u;
      // Same nearer-red contract with the draw order reversed (red first,
      // green second): green is farther and must fail the depth test.
      check_true(is_red);
      check_true(!is_green);
      sys->UnlockRect();
    }
  }

  // ---- pass 4: ZFUNC=LESSEQUAL, only RED at z=0.3 — sanity check
  // that a single-draw pass does write depth and color ----
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
  bs = dev->BeginScene();
  draw_one(dev, /*StartVertex=*/0, red, "pass4.red(only)");
  es = dev->EndScene();
  printf("pass4 BeginScene: hr=0x%08lx EndScene: hr=0x%08lx\n", (unsigned long)bs, (unsigned long)es);
  {
    dev->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr = {};
    sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
    check_true(lr.pBits != nullptr);
    if (lr.pBits) {
      DWORD c = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
      DWORD rgb = c & 0x00FFFFFFu;
      bool is_red = (rgb & 0x00FF0000u) > 0x00800000u;
      // ZFUNC=LESSEQUAL, single red draw against a cleared depth of 1.0:
      // the fragment passes and writes red.
      check_true(is_red);
      sys->UnlockRect();
    }
  }

  // ---- pass 3: ZFUNC=NEVER ; nothing must draw ----
  dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clearColor, 1.0f, 0);
  dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_NEVER);
  bs = dev->BeginScene();
  draw_one(dev, /*StartVertex=*/0, red, "pass3.red(NEVER)");
  es = dev->EndScene();
  printf("pass3 BeginScene: hr=0x%08lx EndScene: hr=0x%08lx\n", (unsigned long)bs, (unsigned long)es);
  {
    dev->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr = {};
    sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
    check_true(lr.pBits != nullptr);
    if (lr.pBits) {
      DWORD c = ((DWORD *)((char *)lr.pBits + 32 * (size_t)lr.Pitch))[32];
      DWORD rgb = c & 0x00FFFFFFu;
      DWORD clear_rgb = clearColor & 0x00FFFFFFu;
      // ZFUNC=NEVER: every fragment is discarded, so the centre must keep
      // the clear colour.
      check_true(rgb == clear_rgb);
      sys->UnlockRect();
    }
  }

  vb->Release();
  decl->Release();
  ps->Release();
  vs->Release();
  sys->Release();
  rt->Release();
  dev->Release();
  d3d->Release();
}
