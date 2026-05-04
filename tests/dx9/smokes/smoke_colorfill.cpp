// ColorFill — render-pass-clear path. Fill an RT to a known color
// via ColorFill, read it back via GetRenderTargetData, assert pixels
// match. Same Clear-style oracle as smoke_getrendertargetdata, but
// targeting an arbitrary surface instead of the bound RT0.

#include "../dx9_smoke.h"
void
test_colorfill(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("Direct3DCreate9: NULL\n");
    return;
  }

  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 320;
  pp.BackBufferHeight = 240;
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

  // CreateRenderTarget RT and an offscreen plain DEFAULT surface —
  // both should be ColorFill-able; both should round-trip via
  // GetRenderTargetData → SYSMEM read.
  IDirect3DSurface9 *rt = NULL, *offplain = NULL, *sys = NULL;
  dev->CreateRenderTarget(64, 48, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
  dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &offplain, NULL);
  dev->CreateOffscreenPlainSurface(64, 48, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
  printf("Created rt=%s offplain=%s sys=%s\n", rt ? "ok" : "null", offplain ? "ok" : "null", sys ? "ok" : "null");
  if (!rt || !offplain || !sys) {
    dev->Release();
    d3d->Release();
    return;
  }

  // ---- Fill RT, read back. ----
  const D3DCOLOR colorRT = 0xFF44CC11u;
  HRESULT cf1 = dev->ColorFill(rt, NULL, colorRT);
  printf("ColorFill(rt): hr=0x%08lx\n", (unsigned long)cf1);
  dev->GetRenderTargetData(rt, sys);
  D3DLOCKED_RECT lr = {};
  sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  if (!lr.pBits)
    printf("FAIL: rt readback skipped\n");
  if (lr.pBits) {
    DWORD origin = ((DWORD *)lr.pBits)[0] & 0x00FFFFFFu;
    DWORD far_p = ((DWORD *)((char *)lr.pBits + 47 * lr.Pitch))[63] & 0x00FFFFFFu;
    DWORD want = colorRT & 0x00FFFFFFu;
    printf(
        "  rt origin.rgb=0x%06lx want=0x%06lx match=%s\n", (unsigned long)origin, (unsigned long)want,
        origin == want ? "yes" : "no"
    );
    printf("  rt far(63,47).rgb=0x%06lx match=%s\n", (unsigned long)far_p, far_p == want ? "yes" : "no");
    sys->UnlockRect();
  }

  // ---- Fill offscreen plain DEFAULT, read back. ----
  const D3DCOLOR colorOP = 0xFFAA22EEu;
  HRESULT cf2 = dev->ColorFill(offplain, NULL, colorOP);
  printf("ColorFill(offplain DEFAULT): hr=0x%08lx\n", (unsigned long)cf2);
  dev->GetRenderTargetData(offplain, sys);
  sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  if (!lr.pBits)
    printf("FAIL: offplain readback skipped\n");
  if (lr.pBits) {
    DWORD pixel = ((DWORD *)lr.pBits)[0] & 0x00FFFFFFu;
    DWORD want = colorOP & 0x00FFFFFFu;
    printf(
        "  offplain origin.rgb=0x%06lx want=0x%06lx match=%s\n", (unsigned long)pixel, (unsigned long)want,
        pixel == want ? "yes" : "no"
    );
    sys->UnlockRect();
  }

  // ---- Plain CreateTexture(DEFAULT) sub-level. Without the RT-bit
  // promotion in CreateTexture's DEFAULT path, this would assert at
  // renderCommandEncoder creation since plain non-USAGE_RT textures
  // would lack the Metal RT usage bit. ----
  IDirect3DTexture9 *tex = NULL;
  dev->CreateTexture(64, 48, 1, /*Usage=*/0, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL);
  printf("CreateTexture(DEFAULT, no RT usage): %s\n", tex ? "ok" : "null");
  if (tex) {
    IDirect3DSurface9 *texLevel = NULL;
    tex->GetSurfaceLevel(0, &texLevel);
    if (texLevel) {
      const D3DCOLOR colorTL = 0xFF77BB33u;
      HRESULT cf3 = dev->ColorFill(texLevel, NULL, colorTL);
      printf("ColorFill(plain texture level): hr=0x%08lx\n", (unsigned long)cf3);
      dev->GetRenderTargetData(texLevel, sys);
      sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
      if (!lr.pBits)
        printf("FAIL: tex-level readback skipped\n");
      if (lr.pBits) {
        DWORD pixel = ((DWORD *)lr.pBits)[0] & 0x00FFFFFFu;
        DWORD want = colorTL & 0x00FFFFFFu;
        printf(
            "  tex-level origin.rgb=0x%06lx want=0x%06lx match=%s\n", (unsigned long)pixel, (unsigned long)want,
            pixel == want ? "yes" : "no"
        );
        sys->UnlockRect();
      }
      texLevel->Release();
    }
    tex->Release();
  }

  // ---- Two consecutive fills — second color must replace first. ----
  const D3DCOLOR color2 = 0xFF010203u;
  dev->ColorFill(rt, NULL, color2);
  dev->GetRenderTargetData(rt, sys);
  sys->LockRect(&lr, NULL, D3DLOCK_READONLY);
  if (lr.pBits) {
    DWORD pixel = ((DWORD *)lr.pBits)[0] & 0x00FFFFFFu;
    DWORD want = color2 & 0x00FFFFFFu;
    printf(
        "Second fill: pixel.rgb=0x%06lx want=0x%06lx match=%s\n", (unsigned long)pixel, (unsigned long)want,
        pixel == want ? "yes" : "no"
    );
    sys->UnlockRect();
  }

  // ---- Rejection paths. ----
  HRESULT bad = dev->ColorFill(NULL, NULL, 0);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  // SYSTEMMEM surface — Pool != DEFAULT.
  bad = dev->ColorFill(sys, NULL, 0);
  check_hr_eq(bad, D3DERR_INVALIDCALL);

  // Subrect — defer until draw path.
  RECT r = {0, 0, 16, 16};
  bad = dev->ColorFill(rt, &r, 0);
  printf(
      "ColorFill(subrect): hr=0x%08lx "
      "(want INVALIDCALL until draw path lands)\n",
      (unsigned long)bad
  );

  // Depth-stencil surface.
  IDirect3DSurface9 *ds = NULL;
  dev->CreateDepthStencilSurface(64, 48, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, NULL);
  printf("CreateDepthStencilSurface: %s\n", ds ? "ok" : "null");
  if (ds) {
    bad = dev->ColorFill(ds, NULL, 0);
    check_hr_eq(bad, D3DERR_INVALIDCALL);
    ds->Release();
  }

  sys->Release();
  offplain->Release();
  rt->Release();
  ULONG dr = dev->Release();
  printf("Release(dev): %lu\n", (unsigned long)dr);
  ULONG r2 = d3d->Release();
  printf("Release: %lu\n", (unsigned long)r2);
}
