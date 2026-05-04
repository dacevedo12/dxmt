// First smoke: walk the IDirect3D9 interface as far as the current
// implementation allows. Print one observable per line; the runner
// SHA-256s the whole stdout block. The point isn't pixels yet — it's
// "every line we print is a behaviour we've nailed down."
//
// Once CreateDevice succeeds with non-stub bring-up code, the smoke
// will be extended to round-trip a device and dump the same handful
// of values from there.

#include "../dx9_smoke.h"
void
test_create_device(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
  if (!d3d) {
    printf("Direct3DCreate9: NULL\n");
    return;
  }
  printf("Direct3DCreate9: ok\n");

  UINT adapters = d3d->GetAdapterCount();
  printf("GetAdapterCount: %u\n", adapters);

  D3DADAPTER_IDENTIFIER9 ident = {};
  HRESULT hr = d3d->GetAdapterIdentifier(0, 0, &ident);
  printf("GetAdapterIdentifier(0): hr=0x%08lx\n", (unsigned long)hr);
  if (SUCCEEDED(hr)) {
    // Don't dump Description: it varies by host GPU model, and we want
    // the same golden to hold across machines. Dump only the fields
    // that are stable to dxmt's policy.
    printf("  Driver=%s\n", ident.Driver);
    printf("  DeviceName=%s\n", ident.DeviceName);
    printf("  VendorId=0x%04x\n", (unsigned)ident.VendorId);
    printf("  DeviceId=0x%04x\n", (unsigned)ident.DeviceId);
    printf("  WHQLLevel=%lu\n", (unsigned long)ident.WHQLLevel);
  }

  D3DCAPS9 caps = {};
  hr = d3d->GetDeviceCaps(0, D3DDEVTYPE_HAL, &caps);
  printf("GetDeviceCaps(HAL): hr=0x%08lx\n", (unsigned long)hr);
  if (SUCCEEDED(hr)) {
    printf("  DeviceType=%u\n", (unsigned)caps.DeviceType);
    printf("  AdapterOrdinal=%u\n", (unsigned)caps.AdapterOrdinal);
    printf("  MaxTextureWidth=%u\n", (unsigned)caps.MaxTextureWidth);
    printf("  MaxTextureHeight=%u\n", (unsigned)caps.MaxTextureHeight);
    printf("  MaxAnisotropy=%u\n", (unsigned)caps.MaxAnisotropy);
    printf("  MaxStreams=%u\n", (unsigned)caps.MaxStreams);
    printf("  NumSimultaneousRTs=%u\n", (unsigned)caps.NumSimultaneousRTs);
    printf("  VertexShaderVersion=0x%08lx\n", (unsigned long)caps.VertexShaderVersion);
    printf("  PixelShaderVersion=0x%08lx\n", (unsigned long)caps.PixelShaderVersion);
    printf("  MaxVertexShaderConst=%u\n", (unsigned)caps.MaxVertexShaderConst);
  }

  HMONITOR mon = d3d->GetAdapterMonitor(0);
  printf("GetAdapterMonitor(0): %s\n", mon ? "non-null" : "null");

  // CheckDeviceType: probe the format combinations LoL touches at startup.
  HRESULT t1 = d3d->CheckDeviceType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE);
  HRESULT t2 = d3d->CheckDeviceType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, FALSE);
  HRESULT t3 = d3d->CheckDeviceType(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_UNKNOWN, TRUE);
  HRESULT t4 = d3d->CheckDeviceType(0, D3DDEVTYPE_REF, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE);
  printf(
      "CheckDeviceType: HAL/X8R8G8B8/X8R8G8B8/win=0x%08lx "
      "HAL/X8R8G8B8/A8R8G8B8/full=0x%08lx "
      "HAL/X8R8G8B8/UNK/win=0x%08lx REF=0x%08lx\n",
      (unsigned long)t1, (unsigned long)t2, (unsigned long)t3, (unsigned long)t4
  );

  // CheckDeviceFormat: probe the queries LoL hits at startup —
  // backbuffer RT format, depth/stencil, common texture formats, and
  // a few "must say no" cases.
  struct {
    const char *label;
    D3DRESOURCETYPE rtype;
    DWORD usage;
    D3DFORMAT fmt;
  } cdf[] = {
      {"RT_X8R8G8B8", D3DRTYPE_SURFACE, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8},
      {"RT_A8R8G8B8", D3DRTYPE_SURFACE, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8},
      {"DS_D24S8", D3DRTYPE_SURFACE, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8},
      {"DS_D16", D3DRTYPE_SURFACE, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D16},
      {"TEX_DXT1", D3DRTYPE_TEXTURE, 0, D3DFMT_DXT1},
      {"TEX_DXT5", D3DRTYPE_TEXTURE, 0, D3DFMT_DXT5},
      {"TEX_A8L8", D3DRTYPE_TEXTURE, 0, D3DFMT_A8L8},
      {"VB_VERTEXDATA", D3DRTYPE_VERTEXBUFFER, 0, D3DFMT_VERTEXDATA},
      {"IB_INDEX16", D3DRTYPE_INDEXBUFFER, 0, D3DFMT_INDEX16},
      {"RT_DXT1", D3DRTYPE_SURFACE, D3DUSAGE_RENDERTARGET, D3DFMT_DXT1},
      {"TEX_D24S8", D3DRTYPE_TEXTURE, 0, D3DFMT_D24S8},
      {"TEX3D_DXT1", D3DRTYPE_VOLUMETEXTURE, 0, D3DFMT_DXT1},
  };
  for (size_t i = 0; i < sizeof(cdf) / sizeof(cdf[0]); ++i) {
    HRESULT r = d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, cdf[i].usage, cdf[i].rtype, cdf[i].fmt);
    printf("CheckDeviceFormat[%s]=0x%08lx\n", cdf[i].label, (unsigned long)r);
  }

  // CheckDeviceMultiSampleType across the bands games actually probe.
  struct {
    const char *label;
    D3DFORMAT fmt;
    D3DMULTISAMPLE_TYPE ms;
  } cdm[] = {
      {"X8R8G8B8/NONE", D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE},
      {"X8R8G8B8/2x", D3DFMT_X8R8G8B8, D3DMULTISAMPLE_2_SAMPLES},
      {"X8R8G8B8/4x", D3DFMT_X8R8G8B8, D3DMULTISAMPLE_4_SAMPLES},
      {"X8R8G8B8/8x", D3DFMT_X8R8G8B8, D3DMULTISAMPLE_8_SAMPLES},
      {"D24S8/4x", D3DFMT_D24S8, D3DMULTISAMPLE_4_SAMPLES},
      {"DXT1/4x", D3DFMT_DXT1, D3DMULTISAMPLE_4_SAMPLES},
  };
  for (size_t i = 0; i < sizeof(cdm) / sizeof(cdm[0]); ++i) {
    DWORD q = 0xdeadbeef;
    HRESULT r = d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, cdm[i].fmt, FALSE, cdm[i].ms, &q);
    printf("CheckDeviceMultiSampleType[%s]=0x%08lx q=%lu\n", cdm[i].label, (unsigned long)r, (unsigned long)q);
  }

  // CheckDepthStencilMatch: a few RT × DS pairs.
  HRESULT m1 = d3d->CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_D24S8);
  HRESULT m2 = d3d->CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_D16);
  HRESULT m3 = d3d->CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_DXT1, D3DFMT_D24S8);
  HRESULT m4 = d3d->CheckDepthStencilMatch(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8);
  printf(
      "CheckDepthStencilMatch: rt=X8/ds=D24S8=0x%08lx rt=A8/ds=D16=0x%08lx "
      "rt=DXT1/ds=D24S8=0x%08lx rt=X8/ds=X8=0x%08lx\n",
      (unsigned long)m1, (unsigned long)m2, (unsigned long)m3, (unsigned long)m4
  );

  HRESULT c1 = d3d->CheckDeviceFormatConversion(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8);
  HRESULT c2 = d3d->CheckDeviceFormatConversion(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8);
  HRESULT c3 = d3d->CheckDeviceFormatConversion(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_DXT1);
  printf(
      "CheckDeviceFormatConversion: same=0x%08lx X8->A8=0x%08lx X8->DXT1=0x%08lx\n", (unsigned long)c1,
      (unsigned long)c2, (unsigned long)c3
  );

  D3DDISPLAYMODE dm = {};
  HRESULT dmhr = d3d->GetAdapterDisplayMode(0, &dm);
  printf("GetAdapterDisplayMode(0): hr=0x%08lx\n", (unsigned long)dmhr);
  if (SUCCEEDED(dmhr)) {
    // Width/Height/RefreshRate vary by host monitor — only hash Format.
    printf("  Format=0x%08lx\n", (unsigned long)dm.Format);
  }

  // CreateDevice round-trip. Use HWND_DESKTOP for hFocusWindow / hDeviceWindow
  // so the smoke does not depend on a real top-level window — wined3d
  // tolerates this and dxmt should too. Windowed=TRUE so we don't tug at
  // the host display state.
  D3DPRESENT_PARAMETERS pp = {};
  pp.BackBufferWidth = 320;
  pp.BackBufferHeight = 240;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.BackBufferCount = 1;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.Windowed = TRUE;
  pp.hDeviceWindow = NULL;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
  IDirect3DDevice9 *dev = nullptr;
  HRESULT cdhr = d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  printf("CreateDevice: hr=0x%08lx dev=%s\n", (unsigned long)cdhr, dev ? "non-null" : "null");
  if (SUCCEEDED(cdhr) && dev) {
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    HRESULT cphr = dev->GetCreationParameters(&cp);
    printf(
        "  GetCreationParameters: hr=0x%08lx adapter=%u type=%u flags=0x%08lx\n", (unsigned long)cphr,
        (unsigned)cp.AdapterOrdinal, (unsigned)cp.DeviceType, (unsigned long)cp.BehaviorFlags
    );
    UINT n = dev->GetNumberOfSwapChains();
    printf("  GetNumberOfSwapChains: %u\n", n);
    IDirect3D9 *back = nullptr;
    HRESULT ghr = dev->GetDirect3D(&back);
    printf("  GetDirect3D: hr=0x%08lx same=%s\n", (unsigned long)ghr, (back == d3d) ? "yes" : "no");
    if (back)
      back->Release();
    IDirect3DSwapChain9 *sc = nullptr;
    HRESULT schr = dev->GetSwapChain(0, &sc);
    printf("  GetSwapChain(0): hr=0x%08lx sc=%s\n", (unsigned long)schr, sc ? "non-null" : "null");
    IDirect3DSwapChain9 *sc1 = (IDirect3DSwapChain9 *)0x1; // sentinel
    HRESULT sc1hr = dev->GetSwapChain(1, &sc1);
    printf(
        "  GetSwapChain(1): hr=0x%08lx out=%s (out-of-range expected)\n", (unsigned long)sc1hr,
        sc1 ? "non-null" : "null"
    );
    if (sc) {
      D3DPRESENT_PARAMETERS sp = {};
      HRESULT pphr = sc->GetPresentParameters(&sp);
      printf(
          "    GetPresentParameters: hr=0x%08lx w=%u h=%u fmt=0x%08lx "
          "swap=%u win=%d interval=0x%08lx\n",
          (unsigned long)pphr, (unsigned)sp.BackBufferWidth, (unsigned)sp.BackBufferHeight,
          (unsigned long)sp.BackBufferFormat, (unsigned)sp.SwapEffect, (int)sp.Windowed,
          (unsigned long)sp.PresentationInterval
      );
      IDirect3DDevice9 *back_dev = nullptr;
      HRESULT gdhr = sc->GetDevice(&back_dev);
      printf("    GetDevice: hr=0x%08lx same=%s\n", (unsigned long)gdhr, (back_dev == dev) ? "yes" : "no");
      if (back_dev)
        back_dev->Release();
      D3DRASTER_STATUS rs = {};
      HRESULT rshr = sc->GetRasterStatus(&rs);
      printf(
          "    GetRasterStatus: hr=0x%08lx vblank=%d scanline=%u\n", (unsigned long)rshr, (int)rs.InVBlank,
          (unsigned)rs.ScanLine
      );
      ULONG sref = sc->Release();
      printf("    Release(sc): %lu\n", (unsigned long)sref);
    }

    // Lifetime probe: hold a chain ref past the device's last public
    // ref. The chain's first AddRef pinned the device, so the device
    // must stay alive until the chain is released. wined3d's
    // d3d9_swapchain_AddRef / Release contract.
    IDirect3DSwapChain9 *outliver = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &outliver))) {
      ULONG dev_ref_with_chain = dev->Release();
      printf("  Release(dev) while chain held: %lu (expect non-zero)\n", (unsigned long)dev_ref_with_chain);
      D3DPRESENT_PARAMETERS sp2 = {};
      HRESULT pp2 = outliver->GetPresentParameters(&sp2);
      printf(
          "  chain->GetPresentParameters after dev release: hr=0x%08lx w=%u\n", (unsigned long)pp2,
          (unsigned)sp2.BackBufferWidth
      );
      // Touch m_device through the chain's GetDevice — m_params is
      // copied by value, so the GetPresentParameters line above does
      // not actually deref m_device. This call does. A regression
      // that lets the chain outlive the device crashes here.
      IDirect3DDevice9 *back_after_release = nullptr;
      HRESULT bhr = outliver->GetDevice(&back_after_release);
      printf(
          "  chain->GetDevice after dev release: hr=0x%08lx dev=%s\n", (unsigned long)bhr,
          back_after_release ? "non-null" : "null"
      );
      if (back_after_release)
        back_after_release->Release();
      ULONG cref = outliver->Release();
      printf("  Release(chain): %lu (chain release should also tear the device)\n", (unsigned long)cref);
    } else {
      // Defensive — if GetSwapChain failed for some reason, still drop
      // our dev ref so the smoke doesn't leak a refcount.
      dev->Release();
    }
  }

  ULONG ref = d3d->Release();
  printf("Release: %lu\n", (unsigned long)ref);
}
