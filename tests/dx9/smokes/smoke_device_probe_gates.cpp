// Device init/runtime probe-method smoke. ValidateDevice,
// EvictManagedResources, CheckResourceResidency,
// SetMaximumFrameLatency / Get, CheckDeviceState — apps poll these
// during init or per-frame for cooperative-level / pacing decisions.
// None had a dedicated smoke previously.
//
// Contract:
//   * ValidateDevice(NULL)               → D3D_OK (NULL pNumPasses
//                                          is documented; do not
//                                          write through it).
//   * ValidateDevice(&n)                 → D3D_OK, n == 1 (Metal
//                                          PSO validation is at
//                                          PSO-build, not here, so
//                                          we always claim single-
//                                          pass per DXVK :2906).
//   * EvictManagedResources              → D3D_OK (no-op on UMA).
//   * CheckResourceResidency(...)        → D3D_OK.
//   * SetMaximumFrameLatency(0)          → D3D_OK; Get reads default
//                                          (3 per Ex spec).
//   * SetMaximumFrameLatency(N<=30)      → D3D_OK; Get reads N.
//   * SetMaximumFrameLatency(>30)        → D3DERR_INVALIDCALL.
//   * GetMaximumFrameLatency(NULL)       → D3DERR_INVALIDCALL.
//   * CheckDeviceState(...)              → D3D_OK.
//
// Set/GetMaximumFrameLatency and CheckDeviceState live on the Ex
// interface — QI for IDirect3DDevice9Ex first; skip those probes
// on non-Ex.

#include "../dx9_smoke.h"

void
test_device_probe_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- ValidateDevice ----
  check_hr(dev->ValidateDevice(NULL));
  DWORD passes = 0xCDCDu;
  check_hr(dev->ValidateDevice(&passes));
  check_eq_u32(passes, 1u);

  // ---- EvictManagedResources ----
  check_hr(dev->EvictManagedResources());

  // ---- Ex-only probes — CheckResourceResidency, frame-latency, and
  //      CheckDeviceState all live on IDirect3DDevice9Ex. ----
  IDirect3DDevice9Ex *devEx = NULL;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&devEx)) && devEx) {
    // CheckResourceResidency on the fixture's RT0.
    IDirect3DSurface9 *rt = NULL;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
      IDirect3DResource9 *res = static_cast<IDirect3DResource9 *>(rt);
      check_hr(devEx->CheckResourceResidency(&res, 1));
      rt->Release();
    }

    // GetMaximumFrameLatency NULL gate.
    check_hr_eq(devEx->GetMaximumFrameLatency(NULL), D3DERR_INVALIDCALL);

    // Round-trip with valid value.
    check_hr(devEx->SetMaximumFrameLatency(2));
    UINT lat = 0;
    check_hr(devEx->GetMaximumFrameLatency(&lat));
    check_eq_u32(lat, 2u);

    // 0 means "default" — the device picks 3 per DXVK.
    check_hr(devEx->SetMaximumFrameLatency(0));
    lat = 0;
    check_hr(devEx->GetMaximumFrameLatency(&lat));
    check_eq_u32(lat, 3u);

    // Out-of-range max latency (>30) → INVALIDCALL.
    check_hr_eq(devEx->SetMaximumFrameLatency(99), D3DERR_INVALIDCALL);

    // CheckDeviceState — accepts a window handle that we don't have
    // here; NULL is fine.
    check_hr(devEx->CheckDeviceState(NULL));

    devEx->Release();
  }

  check_zero_losable_count(dev);
}
