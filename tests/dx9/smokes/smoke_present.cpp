// Present — headless path. The smoke harness creates the device with
// no HWND, so the swapchain runs in headless mode (no CAMetalLayer)
// and Present is a no-op success. Exercising it confirms the
// device-side forward works and the swapchain doesn't bring up a
// layer when there's no window. The windowed-present path (real
// blit + presentDrawable) is exercised by real apps; we have no
// per-pixel oracle for it from a CLI smoke.
//
// Sweep #1 coverage extension: Present must accept (and ignore where
// our Presenter can't honor them) the four args plus dwFlags shapes
// the MSDN docs and wined3d / DXVK call out. We don't have a windowed
// path to assert *visible* effect, but we DO have a contract that
// every call should return D3D_OK and not crash regardless of arg
// shape. Asserting that here pins down regressions where, e.g., a
// future Presenter retarget patch mishandles the override path.

#include "../dx9_smoke.h"

// D3DPRESENT_FORCEIMMEDIATE is a D3D9Ex-era flag (introduced with
// IDirect3DDevice9Ex). Our wine d3d9.h ships the pre-Ex header which
// doesn't declare it; the value is documented as 0x100. The dxmt
// Present path consumes it via DWORD bitmask test, so this is purely
// a header gap on the smoke side, not a runtime gate gap.
#ifndef D3DPRESENT_FORCEIMMEDIATE
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100
#endif

void
test_present(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;

  IDirect3DDevice9 *dev = fx.dev;

  // Device-level Present (forwards to swapchain).
  check_hr(dev->Present(NULL, NULL, NULL, NULL));

  // Same via the explicit swapchain handle. swap_interval=0 (DEFAULT
  // is what fx.create() configured).
  IDirect3DSwapChain9 *chain = NULL;
  check_hr(dev->GetSwapChain(0, &chain));
  check_true(chain != NULL);
  if (chain) {
    check_hr(chain->Present(NULL, NULL, NULL, NULL, 0));

    // hDestWindowOverride: dxmt currently ignores. Spec lets the runtime
    // retarget to an alternate HWND; ignoring is preferable to crashing.
    // Test passes a non-null override (the focus HWND would be 0 in our
    // headless setup; pass a synthetic non-null cookie to exercise the
    // non-null branch).
    HWND dummy = reinterpret_cast<HWND>(0x1234);
    check_hr(chain->Present(NULL, NULL, dummy, NULL, 0));

    // D3DPRESENT_DONOTWAIT: spec says return D3DERR_WASSTILLDRAWING if
    // GPU is still drawing the previous frame; our path currently strips
    // and blocks (Presenter has no non-blocking acquire). Headless
    // Present is unconditionally D3D_OK so the gate still passes.
    check_hr(chain->Present(NULL, NULL, NULL, NULL, D3DPRESENT_DONOTWAIT));

    // D3DPRESENT_FORCEIMMEDIATE: per-Present sync-interval override to 0.
    // Legal only when the swapchain was created with D3DSWAPEFFECT_FLIPEX
    // per MSDN D3DPRESENT remarks; "improperly specified" is INVALIDCALL.
    // Dx9Fixture creates the implicit chain with D3DSWAPEFFECT_DISCARD,
    // so the flag must be rejected. Apps probe-and-fall-back on this.
    check_hr_eq(chain->Present(NULL, NULL, NULL, NULL, D3DPRESENT_FORCEIMMEDIATE), D3DERR_INVALIDCALL);

    // Unknown flag bits: D3D9 spec leaves the upper bits reserved. Driver
    // contract is "ignore unknown" rather than INVALIDCALL — apps shipping
    // pre-FORCEIMMEDIATE binaries set whatever PIX-era flags they want.
    check_hr(chain->Present(NULL, NULL, NULL, NULL, 0x80000000u));

    chain->Release();
  }

  // Multiple consecutive Presents must remain S_OK — no per-frame
  // state that latches errors from the no-layer path.
  check_hr(dev->Present(NULL, NULL, NULL, NULL));
  check_hr(dev->Present(NULL, NULL, NULL, NULL));

  // Present after Clear — confirms no interaction between Clear's
  // render-pass cmdbufs and the headless Present path.
  check_hr(dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF112233u, 0.0f, 0));
  check_hr(dev->Present(NULL, NULL, NULL, NULL));

  // Sub-rects: dxmt currently ignores them (Presenter has no sub-region
  // blit path), but the call must still succeed. Pass non-NULL rects to
  // exercise the ignore branch.
  RECT src = {0, 0, 64, 64};
  RECT dst = {0, 0, 64, 64};
  check_hr(dev->Present(&src, &dst, NULL, NULL));

  // Releases happen in Dx9Fixture's dtor; check_zero_losable_count
  // would assert here once audit G1 lands.
  check_zero_losable_count(dev);
}
