// Device read-back family spec-gate smoke. Apps poll these at init
// (GetDirect3D / GetDeviceCaps / GetCreationParameters / GetSwapChain
// / GetNumberOfSwapChains / GetAvailableTextureMem) and on resize
// (GetDisplayMode). None had dedicated gate coverage previously.
//
// Contract (MSDN + wined3d device.c equivalents):
//
//   * GetDirect3D(NULL)              → INVALIDCALL. Otherwise
//                                      returns the IDirect3D9 the
//                                      device was created off of
//                                      with an AddRef'd pointer.
//   * GetCreationParameters(NULL)    → INVALIDCALL. Otherwise echoes
//                                      AdapterOrdinal, DeviceType,
//                                      BehaviorFlags, hFocusWindow.
//   * GetDisplayMode(iSwapChain!=0)  → INVALIDCALL (no additional
//                                      swapchains yet).
//   * GetDisplayMode(0, &mode)       → S_OK, mode filled with the
//                                      adapter's current mode.
//   * GetSwapChain(NULL pp)          → INVALIDCALL.
//   * GetSwapChain(iSwapChain!=0,
//     non-NULL pp)                   → INVALIDCALL, AND must NOT
//                                      clobber the caller's
//                                      sentinel (matches wined3d
//                                      swapchain.c:200; game engines
//                                      probe a sentinel).
//   * GetSwapChain(0, &p)            → S_OK, p == implicit chain.
//   * GetNumberOfSwapChains()        → 1 today (implicit only).
//   * GetAvailableTextureMem()       → > 0 (project_d3d9_get_available_
//                                      texture_mem memory: returning
//                                      0 was the 2005-era pool-sizing
//                                      bug).

#include "../dx9_smoke.h"

void
test_device_readback_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- GetDirect3D ----
  check_hr_eq(dev->GetDirect3D(NULL), D3DERR_INVALIDCALL);

  IDirect3D9 *d3d_back = NULL;
  check_hr(dev->GetDirect3D(&d3d_back));
  check_eq_ptr(d3d_back, fx.d3d);
  if (d3d_back)
    d3d_back->Release();

  // ---- GetCreationParameters ----
  check_hr_eq(dev->GetCreationParameters(NULL), D3DERR_INVALIDCALL);

  D3DDEVICE_CREATION_PARAMETERS cp = {};
  check_hr(dev->GetCreationParameters(&cp));
  check_eq_u32(cp.AdapterOrdinal, 0u);
  check_eq_u32(cp.DeviceType, D3DDEVTYPE_HAL);
  // BehaviorFlags must include HARDWARE_VERTEXPROCESSING — the
  // Dx9Fixture creates with exactly that flag.
  check_true((cp.BehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) != 0);

  // ---- GetDisplayMode ----
  D3DDISPLAYMODE mode = {};
  check_hr_eq(dev->GetDisplayMode(99, &mode), D3DERR_INVALIDCALL);
  check_hr(dev->GetDisplayMode(0, &mode));
  // Format / dimensions are adapter-dependent; just assert non-zero
  // so a regression that zeros them shows up.
  check_true(mode.Width > 0);
  check_true(mode.Height > 0);

  // ---- GetSwapChain ----
  check_hr_eq(dev->GetSwapChain(0, NULL), D3DERR_INVALIDCALL);

  // Sentinel-preservation on iSwapChain!=0 failure path.
  IDirect3DSwapChain9 *sentinel = reinterpret_cast<IDirect3DSwapChain9 *>(static_cast<uintptr_t>(0xCAFEBABE));
  IDirect3DSwapChain9 *probe = sentinel;
  check_hr_eq(dev->GetSwapChain(1, &probe), D3DERR_INVALIDCALL);
  check_eq_ptr(probe, sentinel);

  // Happy path.
  IDirect3DSwapChain9 *sc = NULL;
  check_hr(dev->GetSwapChain(0, &sc));
  check_true(sc != NULL);
  if (sc)
    sc->Release();

  // ---- GetNumberOfSwapChains ----
  check_eq_u32(dev->GetNumberOfSwapChains(), 1u);

  // ---- GetAvailableTextureMem ----
  // Returning 0 is the documented bug from project_d3d9_get_available_
  // texture_mem.md (2005-era engines pool-size against it). Assert
  // non-zero so a regression is loud.
  UINT mem = dev->GetAvailableTextureMem();
  check_true(mem > 0);

  check_zero_losable_count(dev);
}
