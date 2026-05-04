// Reset / TestCooperativeLevel spec-gate smoke (audit G2 state machine
// + transitive cover of G1's losable-resource counter). The counter
// itself isn't directly exposed through IDirect3DDevice9 — it's
// observable today only through the Reset rejection it drives, so the
// smoke asserts that *behavioral* contract instead of reading the
// counter. When G1's diag-export lands, check_zero_losable_count's
// macro flips from SKIP to a real probe and stops being a duplicate.
//
// Coverage:
//   * Reset(NULL)                              → D3DERR_INVALIDCALL.
//   * Reset with outstanding DEFAULT RT        → D3DERR_INVALIDCALL,
//                                                state advances to
//                                                NotReset (observable
//                                                via TestCooperativeLevel
//                                                returning DEVICENOTRESET).
//   * Release the RT, Reset with valid params  → D3D_OK, state Ok.
//   * BeginScene before Reset, no EndScene     → Reset closes the
//                                                scene; the next
//                                                BeginScene succeeds.
//   * StateBlock created pre-Reset             → Capture / Apply on
//                                                that block returns
//                                                INVALIDCALL post-Reset.
//
// Refs: DXVK d3d9_device.cpp:515-570, wined3d device.c:1145-1212.

#include "../dx9_smoke.h"

#include <string.h>

void
test_reset_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 320;
  pp.BackBufferHeight = 240;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = TRUE;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;

  // ---- TCL on a freshly-created device is S_OK. ----
  check_hr(dev->TestCooperativeLevel());

  // ---- Reset(NULL) → INVALIDCALL. ----
  check_hr_eq(dev->Reset(NULL), D3DERR_INVALIDCALL);

  // ---- Outstanding DEFAULT RT → Reset INVALIDCALL + NotReset. ----
  IDirect3DSurface9 *rt = NULL;
  check_hr(dev->CreateRenderTarget(64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL));
  if (!rt)
    return;

  D3DPRESENT_PARAMETERS pp_for_reset = pp;
  check_hr_eq(dev->Reset(&pp_for_reset), D3DERR_INVALIDCALL);
  check_hr_eq(dev->TestCooperativeLevel(), D3DERR_DEVICENOTRESET);

  // ---- Drop the RT, re-Reset → S_OK + state back to Ok. ----
  rt->Release();
  rt = NULL;
  pp_for_reset = pp;
  check_hr(dev->Reset(&pp_for_reset));
  check_hr(dev->TestCooperativeLevel());

  // ---- BeginScene → Reset → BeginScene succeeds. ----
  // Non-Ex Reset closes the implicit scene per MSDN. Without that, the
  // second BeginScene would return INVALIDCALL ("BeginScene already
  // called").
  check_hr(dev->BeginScene());
  pp_for_reset = pp;
  check_hr(dev->Reset(&pp_for_reset));
  check_hr(dev->BeginScene());
  check_hr(dev->EndScene());

  // ---- StateBlock pre-Reset, Capture / Apply post-Reset → INVALIDCALL.
  IDirect3DStateBlock9 *sb = NULL;
  check_hr(dev->CreateStateBlock(D3DSBT_ALL, &sb));
  if (sb) {
    pp_for_reset = pp;
    check_hr(dev->Reset(&pp_for_reset));
    check_hr_eq(sb->Capture(), D3DERR_INVALIDCALL);
    check_hr_eq(sb->Apply(), D3DERR_INVALIDCALL);
    sb->Release();
  }

  check_zero_losable_count(dev);
}
