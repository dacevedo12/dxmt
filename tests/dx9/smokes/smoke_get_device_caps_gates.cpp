// GetDeviceCaps spec-gate smoke. Apps interrogate the D3DCAPS9 struct
// at init to pick rendering paths; under-claiming a cap (e.g.
// PixelShaderVersion < 3.0, MaxAnisotropy < 16, missing DECLTYPE bits)
// pushes engines into fallback shaders that produce visibly different
// output. No prior smoke locked the cap matrix.
//
// Coverage focuses on the fields apps gate on (sourced from the cap
// values commercial D3D9 engines branch on — DXVK's d3d9_caps.cpp
// audits the same set as the reference set):
//
//   * NULL pCaps                 → D3DERR_INVALIDCALL.
//   * Adapter out-of-range       → D3DERR_INVALIDCALL.
//   * AdapterOrdinal / DeviceType echoed back as-passed.
//   * Shader-model + per-shader bounds:
//       VertexShaderVersion >= 3.0,
//       PixelShaderVersion  >= 3.0,
//       MaxVertexShaderConst == 256,
//       MaxStreams == 16,
//       MaxSimultaneousTextures == 8,
//       MaxTextureBlendStages == 8,
//       NumSimultaneousRTs == 4.
//   * Geometry / format limits:
//       MaxTextureWidth/Height == 16384,
//       MaxAnisotropy == 16,
//       MaxActiveLights == 8,
//       MaxUserClipPlanes == 8.
//   * Capability bitfields apps treat as required:
//       Caps2  has FULLSCREENGAMMA | DYNAMICTEXTURES | CANAUTOGENMIPMAP,
//       DevCaps has HWTRANSFORMANDLIGHT | DRAWPRIMTLVERTEX,
//       PresentationIntervals has ONE | IMMEDIATE,
//       StretchRectFilterCaps has POINT | LINEAR,
//       ZCmpCaps is the full 8-op set,
//       DeclTypes covers SHORT4N + FLOAT16_2 + UBYTE4N.
//   * Adapter-group metadata for the single-adapter case.

#include "../dx9_smoke.h"

void
test_get_device_caps_gates(void) {
  Dx9Fixture fx;
  if (!fx.create())
    return;
  IDirect3DDevice9 *dev = fx.dev;

  // ---- NULL pCaps ----
  check_hr_eq(dev->GetDeviceCaps(NULL), D3DERR_INVALIDCALL);

  // ---- Cap fetch ----
  D3DCAPS9 caps = {};
  check_hr(dev->GetDeviceCaps(&caps));

  // Adapter / device-type echo. Fixture's CreateDevice uses
  // Adapter=0 + DeviceType=HAL.
  check_eq_u32(caps.AdapterOrdinal, 0u);
  check_eq_u32(caps.DeviceType, D3DDEVTYPE_HAL);

  // ---- Shader-model & per-shader bounds. ----
  check_eq_u32(caps.VertexShaderVersion, D3DVS_VERSION(3, 0));
  check_eq_u32(caps.PixelShaderVersion, D3DPS_VERSION(3, 0));
  check_eq_u32(caps.MaxVertexShaderConst, 256u);
  check_eq_u32(caps.MaxStreams, 16u);
  check_eq_u32(caps.MaxSimultaneousTextures, 8u);
  check_eq_u32(caps.MaxTextureBlendStages, 8u);
  check_eq_u32(caps.NumSimultaneousRTs, 4u);

  // ---- Geometry / format limits. ----
  check_eq_u32(caps.MaxTextureWidth, 16384u);
  check_eq_u32(caps.MaxTextureHeight, 16384u);
  check_eq_u32(caps.MaxAnisotropy, 16u);
  check_eq_u32(caps.MaxActiveLights, 8u);
  check_eq_u32(caps.MaxUserClipPlanes, 8u);

  // ---- Required capability bits. ----
  check_true((caps.Caps2 & D3DCAPS2_FULLSCREENGAMMA) != 0);
  check_true((caps.Caps2 & D3DCAPS2_DYNAMICTEXTURES) != 0);
  check_true((caps.Caps2 & D3DCAPS2_CANAUTOGENMIPMAP) != 0);

  check_true((caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) != 0);
  check_true((caps.DevCaps & D3DDEVCAPS_DRAWPRIMTLVERTEX) != 0);
  check_true((caps.DevCaps & D3DDEVCAPS_PUREDEVICE) != 0);

  check_true((caps.PresentationIntervals & D3DPRESENT_INTERVAL_ONE) != 0);
  check_true((caps.PresentationIntervals & D3DPRESENT_INTERVAL_IMMEDIATE) != 0);

  // Metal's blit encoder supports only POINT and LINEAR — apps that
  // requested ANISOTROPIC on StretchRect should see it absent and
  // downgrade explicitly.
  check_true((caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MINFPOINT) != 0);
  check_true((caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) != 0);
  check_true((caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MAGFPOINT) != 0);
  check_true((caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR) != 0);
  check_true((caps.StretchRectFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) == 0);

  // Compare ops — full 8-op set. Alpha-test branches in older games
  // bit-test individual values.
  const DWORD allCmp = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
                       D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
  check_eq_u32(caps.ZCmpCaps & allCmp, allCmp);
  check_eq_u32(caps.AlphaCmpCaps & allCmp, allCmp);

  // Vertex declaration types apps lean on for compact streams.
  check_true((caps.DeclTypes & D3DDTCAPS_SHORT4N) != 0);
  check_true((caps.DeclTypes & D3DDTCAPS_FLOAT16_2) != 0);
  check_true((caps.DeclTypes & D3DDTCAPS_FLOAT16_4) != 0);
  check_true((caps.DeclTypes & D3DDTCAPS_UBYTE4N) != 0);

  // ---- Adapter-group metadata for the single-adapter case. ----
  check_eq_u32(caps.MasterAdapterOrdinal, 0u);
  check_eq_u32(caps.AdapterOrdinalInGroup, 0u);
  check_eq_u32(caps.NumberOfAdaptersInGroup, 1u);

  // ---- IDirect3D9::GetDeviceCaps Adapter-out-of-range gate. ----
  // Probes the lower-level entry that the device-side method
  // forwards to — Adapter > device count → INVALIDCALL.
  D3DCAPS9 cap_bad = {};
  check_hr_eq(fx.d3d->GetDeviceCaps(0xDEADBEEF, D3DDEVTYPE_HAL, &cap_bad), D3DERR_INVALIDCALL);
  check_hr_eq(fx.d3d->GetDeviceCaps(0, D3DDEVTYPE_HAL, NULL), D3DERR_INVALIDCALL);

  check_zero_losable_count(dev);
}
