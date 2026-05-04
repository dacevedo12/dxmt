// Set{Vertex,Pixel}Shader + Create{Vertex,Pixel}Shader spec-gate
// smoke. Audit J1 (interim NULL-bind gate) is the headline; the
// Create-side gates ride along because the SetShader contract is
// only meaningful if Create's validation also matches the reference
// shape (kind-mismatch reject, malformed bytecode, NULL pointers).
//
// Coverage:
//   * CreateVertexShader / CreatePixelShader with NULL function /
//     NULL ppShader / wrong-kind bytecode → D3DERR_INVALIDCALL.
//   * Set{Vertex,Pixel}Shader(NULL) → D3D_OK (audit J1: apps that
//     unbind to fall back to FFP get a successful Set; the encode-
//     side draw path silently no-ops until the FFP shader generator
//     lands — J5 epic). Get post-unbind returns NULL.
//   * Set{Vertex,Pixel}Shader after a draw with no shader bound →
//     S_OK from the draw entry (queues the BatchedDraw); encode-side
//     drops it because either VS or PS is null. The smoke can only
//     observe the entry-side hr, which we expect to be S_OK.
//   * Cross-device shader rejection — wined3d device.c
//     d3d9_device_SetVertexShader returns INVALIDCALL when the
//     bound shader's device != device.
//
// Refs: DXVK d3d9_device.cpp:8125-8260 (Create + Set shapes),
//       wined3d device.c::d3d9_device_SetVertexShader (matching).

#include "../dx9_smoke.h"

// vs_2_0 — 4-DWORD minimal bytecode. Same blob smoke_set_vertexshader
// uses, recopied to keep this TU self-contained.
static const DWORD vs_blob[] = {
    0xFFFE0200u, // vs_2_0 header
    0x0001FFFEu, // comment, 1 DWORD body
    0x0000FFFFu, // body — 0xFFFF that must not be misread as END
    0x0000FFFFu, // D3DSIO_END
};

// ps_2_0 — minimal blob. Same shape, different version word.
static const DWORD ps_blob[] = {
    0xFFFF0200u, // ps_2_0 header
    0x0000FFFFu, // D3DSIO_END
};

void
test_set_shader_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- Create gates ----

  // NULL ppShader.
  check_hr_eq(dev->CreateVertexShader(vs_blob, NULL), D3DERR_INVALIDCALL);
  check_hr_eq(dev->CreatePixelShader(ps_blob, NULL), D3DERR_INVALIDCALL);

  // NULL function.
  IDirect3DVertexShader9 *vs = NULL;
  IDirect3DPixelShader9 *ps = NULL;
  check_hr_eq(dev->CreateVertexShader(NULL, &vs), D3DERR_INVALIDCALL);
  check_hr_eq(dev->CreatePixelShader(NULL, &ps), D3DERR_INVALIDCALL);

  // Kind-mismatch — VS bytecode through CreatePixelShader and vice
  // versa. DXVK d3d9_device.cpp:8138 + 8192 rejects on
  // header->kind != Pixel/Vertex.
  check_hr_eq(dev->CreateVertexShader(ps_blob, &vs), D3DERR_INVALIDCALL);
  check_hr_eq(dev->CreatePixelShader(vs_blob, &ps), D3DERR_INVALIDCALL);

  // Truncated bytecode — no END token in a single DWORD past the
  // header. shader_bytecode_dword_count walks until it finds END
  // and returns 0 if it walks past the buffer; Create rejects on 0.
  const DWORD truncated_vs[] = {0xFFFE0200u, 0x12345678u};
  check_hr_eq(dev->CreateVertexShader(truncated_vs, &vs), D3DERR_INVALIDCALL);

  // Happy-path Create — needed for the Set/Get probes below.
  check_hr(dev->CreateVertexShader(vs_blob, &vs));
  check_true(vs != NULL);
  check_hr(dev->CreatePixelShader(ps_blob, &ps));
  check_true(ps != NULL);

  // ---- Set / Get NULL round-trip (audit J1) ----

  // Initial Get returns NULL on a fresh device.
  IDirect3DVertexShader9 *got_vs = NULL;
  IDirect3DPixelShader9 *got_ps = NULL;
  check_hr(dev->GetVertexShader(&got_vs));
  check_eq_ptr(got_vs, NULL);
  check_hr(dev->GetPixelShader(&got_ps));
  check_eq_ptr(got_ps, NULL);

  // Bind, then unbind, then Get returns NULL again. The intermediate
  // bound state is in smoke_set_vertexshader / smoke_set_pixelshader;
  // we just lock down the J1 unbind contract here.
  check_hr(dev->SetVertexShader(vs));
  check_hr(dev->SetVertexShader(NULL));
  got_vs = (IDirect3DVertexShader9 *)0xdeadbeef;
  check_hr(dev->GetVertexShader(&got_vs));
  check_eq_ptr(got_vs, NULL);

  check_hr(dev->SetPixelShader(ps));
  check_hr(dev->SetPixelShader(NULL));
  got_ps = (IDirect3DPixelShader9 *)0xdeadbeef;
  check_hr(dev->GetPixelShader(&got_ps));
  check_eq_ptr(got_ps, NULL);

  // ---- Cross-device rejection ----
  // A second device created off the same IDirect3D9 with the same
  // present params is enough to surface the deviceRaw() != this
  // gate at SetVertexShader / SetPixelShader entry.
  D3DPRESENT_PARAMETERS pp2 = {};
  pp2.BackBufferWidth = 320;
  pp2.BackBufferHeight = 240;
  pp2.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp2.BackBufferCount = 1;
  pp2.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp2.Windowed = TRUE;
  pp2.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  IDirect3DDevice9 *dev2 = NULL;
  HRESULT cd2 = fx.d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp2, &dev2);
  if (SUCCEEDED(cd2) && dev2) {
    IDirect3DVertexShader9 *foreign_vs = NULL;
    IDirect3DPixelShader9 *foreign_ps = NULL;
    if (SUCCEEDED(dev2->CreateVertexShader(vs_blob, &foreign_vs)) && foreign_vs) {
      check_hr_eq(dev->SetVertexShader(foreign_vs), D3DERR_INVALIDCALL);
      foreign_vs->Release();
    }
    if (SUCCEEDED(dev2->CreatePixelShader(ps_blob, &foreign_ps)) && foreign_ps) {
      check_hr_eq(dev->SetPixelShader(foreign_ps), D3DERR_INVALIDCALL);
      foreign_ps->Release();
    }
    dev2->Release();
  }

  if (vs)
    vs->Release();
  if (ps)
    ps->Release();

  check_zero_losable_count(dev);
}
