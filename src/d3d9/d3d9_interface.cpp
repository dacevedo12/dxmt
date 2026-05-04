#include "d3d9_interface.hpp"

#include "Metal.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_trace.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "wsi_monitor.hpp"

#include <cstdio>
#include <cstring>

namespace dxmt {

MTLD3D9Interface::MTLD3D9Interface(UINT SDKVersion, bool isEx) : m_sdkVersion(SDKVersion), m_isEx(isEx) {}

MTLD3D9Interface::~MTLD3D9Interface() = default;

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::QueryInterface(REFIID riid, void **ppvObject) {
  D9_TRACE("IDirect3D9::QueryInterface");
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3D9)) {
    *ppvObject = static_cast<IDirect3D9 *>(this);
    AddRef();
    return S_OK;
  }
  if (m_isEx && riid == __uuidof(IDirect3D9Ex)) {
    *ppvObject = static_cast<IDirect3D9Ex *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::RegisterSoftwareDevice(void *pInitializeFunction) {
  D9_TRACE("IDirect3D9::RegisterSoftwareDevice");
  if (!pInitializeFunction)
    return D3DERR_INVALIDCALL;
  return D3DERR_NOTAVAILABLE;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterCount() {
  D9_TRACE("IDirect3D9::GetAdapterCount");
  return WMT::CopyAllDevices().count();
}

// Canonical D3D9 device UID. wined3d hands this back from
// wined3d_adapter_get_identifier (directx.c:57, :1805). Some 2005-era
// titles compare DeviceIdentifier against this exact GUID and treat
// any other value as a "non-standard" adapter, falling into a SetupAPI/
// EnumDisplayDevices fallback that walks the registry every frame —
// NFS:MW capping at 1 fps under dxmt traced back to this.
static const GUID kD3DDeviceD3DUID = {0xaeb2cdd4, 0x6e41, 0x43ea, {0x94, 0x1c, 0x83, 0x61, 0xcc, 0x76, 0x07, 0x81}};

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
  D9_TRACE("IDirect3D9::GetAdapterIdentifier");
  if (!pIdentifier)
    return D3DERR_INVALIDCALL;

  auto devices = WMT::CopyAllDevices();
  if (Adapter >= devices.count())
    return D3DERR_INVALIDCALL;
  auto device = devices.object(Adapter);

  std::memset(pIdentifier, 0, sizeof(*pIdentifier));
  std::snprintf(pIdentifier->Driver, MAX_DEVICE_IDENTIFIER_STRING, "%s", "DXMT (Metal)");
  device.name().getCString(pIdentifier->Description, MAX_DEVICE_IDENTIFIER_STRING, WMTUTF8StringEncoding);
  std::snprintf(pIdentifier->DeviceName, sizeof(pIdentifier->DeviceName), "%s", "\\\\.\\DISPLAY1");

  // 0x106B is Apple's PCI vendor ID, the same value dxgi_adapter.cpp
  // hands back to D3D11 callers. DeviceId / SubSysId / Revision aren't
  // meaningful for Metal but keep DeviceId non-zero — some titles
  // treat 0 as "no adapter" and fall through to a different code path.
  pIdentifier->VendorId = 0x106B;
  pIdentifier->DeviceId = 1;
  // SubSysId / Revision intentionally 0 — wined3d does the same.

  // Mirror wined3d's shape: a non-zero driver version. Zero reads as
  // "uninitialised" to anti-piracy / driver-validation code paths.
  pIdentifier->DriverVersion.HighPart = 0x000a;
  pIdentifier->DriverVersion.LowPart = 0x0001;

  pIdentifier->DeviceIdentifier = kD3DDeviceD3DUID;
  pIdentifier->WHQLLevel = (Flags & D3DENUM_WHQL_LEVEL) ? 1 : 0;

  return D3D_OK;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
  D9_TRACE("IDirect3D9::GetAdapterModeCount");
  if (Adapter >= WMT::CopyAllDevices().count())
    return 0;
  // wined3d filters: D3D9 only enumerates X8R8G8B8 / R5G6B5 here, and
  // Apple displays don't expose 16-bit modes any more. Reject everything
  // except X8R8G8B8. wine dlls/d3d9/directx.c:226 (Ex variant).
  if (Format != D3DFMT_X8R8G8B8)
    return 0;
  HMONITOR mon = wsi::enumMonitors(Adapter);
  if (!mon)
    return 0;
  // wsi has no count primitive; walk until getDisplayMode fails.
  UINT n = 0;
  wsi::WsiMode wm{};
  while (wsi::getDisplayMode(mon, n, &wm))
    ++n;
  return n;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) {
  D9_TRACE("IDirect3D9::EnumAdapterModes");
  if (!pMode || Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;
  if (Format != D3DFMT_X8R8G8B8)
    return D3DERR_INVALIDCALL;

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getDisplayMode(mon, Mode, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = wm.refreshRate.denominator ? (wm.refreshRate.numerator / wm.refreshRate.denominator) : 60;
  pMode->Format = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
  D9_TRACE("IDirect3D9::GetAdapterDisplayMode");
  if (!pMode || Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getCurrentDisplayMode(mon, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = wm.refreshRate.denominator ? (wm.refreshRate.numerator / wm.refreshRate.denominator) : 60;
  // D3D9 only ever advertises X8R8G8B8 / R5G6B5 here. Pick X8R8G8B8 —
  // matches what wined3d does on a 32-bit GL desktop and what every
  // modern Windows desktop reports.
  pMode->Format = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceType(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed
) {
  D9_TRACE("IDirect3D9::CheckDeviceType");
  if (Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;
  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // wined3d filters this out before consulting its format tables: D3D9
  // fullscreen swap chains can only run on X8R8G8B8 or R5G6B5, even
  // though ddraw paths advertise other formats. wine dlls/d3d9/
  // directx.c:188.
  if (!bWindowed && DisplayFormat != D3DFMT_X8R8G8B8 && DisplayFormat != D3DFMT_R5G6B5)
    return D3DERR_NOTAVAILABLE;

  // Backbuffer = D3DFMT_UNKNOWN means "use the display format" — spec.
  if (BackBufferFormat == D3DFMT_UNKNOWN)
    BackBufferFormat = DisplayFormat;

  switch (DisplayFormat) {
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    break;
  default:
    return D3DERR_NOTAVAILABLE;
  }

  switch (BackBufferFormat) {
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A2R10G10B10:
    return D3D_OK;
  default:
    return D3DERR_NOTAVAILABLE;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceFormat(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
    D3DFORMAT CheckFormat
) {
  D9_TRACE("IDirect3D9::CheckDeviceFormat");
  if (Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // wined3d takes the same set; ddraw advertises others but D3D9 doesn't.
  // wine dlls/d3d9/directx.c:233.
  if (AdapterFormat != D3DFMT_X8R8G8B8 && AdapterFormat != D3DFMT_R5G6B5 && AdapterFormat != D3DFMT_X1R5G5B5)
    return AdapterFormat ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL;

  const bool wantRT = (Usage & D3DUSAGE_RENDERTARGET) != 0;
  const bool wantDS = (Usage & D3DUSAGE_DEPTHSTENCIL) != 0;
  if (wantRT && wantDS)
    return D3DERR_NOTAVAILABLE;

  // D3DUSAGE_QUERY_SRGBREAD / SRGBWRITE: apps probe whether the format
  // supports sRGB-decoded sampling (SRGBTEXTURE sampler state) or
  // sRGB-encoded writes (SRGBWRITEENABLE render state). dxmt honors
  // both via texture-view aliasing onto the *_sRGB Metal pixel format,
  // so the probe should return OK for any format with an sRGB alias —
  // every format Recall_sRGB returns a non-identity result for. Apps
  // that get NOTAVAILABLE fall through to their non-sRGB shader path,
  // which matches wined3d's behavior for formats with no GL_SRGB
  // equivalent. wine dlls/d3d9/directx.c CheckDeviceFormat handles
  // these query bits with the same pattern.
  const bool querySrgbRead = (Usage & D3DUSAGE_QUERY_SRGBREAD) != 0;
  const bool querySrgbWrite = (Usage & D3DUSAGE_QUERY_SRGBWRITE) != 0;
  if (querySrgbRead || querySrgbWrite) {
    auto metal_fmt = D3DFormatToMetal(CheckFormat, D3D9FormatUsage::SampleableTexture);
    if (metal_fmt == WMTPixelFormatInvalid || Recall_sRGB(metal_fmt) == metal_fmt)
      return D3DERR_NOTAVAILABLE;
  }

  auto isColorRTFormat = [](D3DFORMAT f) {
    switch (f) {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_R16F:
    case D3DFMT_G16R16F:
    case D3DFMT_R32F:
    case D3DFMT_G32R32F:
    case D3DFMT_A32B32G32R32F:
    case D3DFMT_G16R16:
      return true;
    // D3DFMT_R8G8B8 (24-bit RGB) is intentionally not listed: Metal has
    // no padded-RGB8 pixel format and Vulkan/DXVK also reject it. Apps
    // that probe and get NOTAVAILABLE fall back to a 32-bit variant
    // (typically X8R8G8B8 → BGRX8Unorm).
    default:
      return false;
    }
  };

  auto isDSFormat = [](D3DFORMAT f) {
    switch (f) {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D24FS8:
    // FOURCC sampleable-depth aliases (D3DFormatToMetal:159-168 maps
    // them). Games probe via CheckDeviceFormat(... DEPTHSTENCIL ...,
    // INTZ) before allocating a shadow-map intermediate; NOTAVAILABLE
    // here forces them off the fast PCF path even though creation
    // would succeed. wined3d directx.c reports these as available
    // whenever the matching native depth format is.
    case D3DFMT_INTZ:
    case D3DFMT_DF24:
    case D3DFMT_DF16:
      return true;
    default:
      return false;
    }
  };

  auto isSamplable2D = [&](D3DFORMAT f) {
    if (isColorRTFormat(f))
      return true;
    switch (f) {
    // A4R4G4B4 / X4R4G4B4 — Apple Silicon has no native 4-bit-per-
    // channel format and the runtime would have to upconvert at
    // upload time. Reject for now; apps that branch on this will
    // fall back to A8R8G8B8.
    case D3DFMT_A8:
    case D3DFMT_A8L8:
    case D3DFMT_L8:
    case D3DFMT_L16:
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
    case D3DFMT_V8U8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
      return true;
    default:
      return false;
    }
  };

  switch (RType) {
  case D3DRTYPE_SURFACE:
    if (wantDS)
      return isDSFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (wantRT) {
      // 'NULL' FOURCC is a vendor-defined sentinel for a colour RT
      // slot the app binds but never writes — accepted as RT-capable
      // here so apps gate creation on the standard ENABLE pattern.
      // dxmt drops the slot at render-pass + PSO build time.
      if (IsNullFormat(CheckFormat))
        return D3D_OK;
      return isColorRTFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    return (isSamplable2D(CheckFormat) || isDSFormat(CheckFormat)) ? D3D_OK : D3DERR_NOTAVAILABLE;

  case D3DRTYPE_TEXTURE:
  case D3DRTYPE_CUBETEXTURE:
    if (wantDS)
      return isDSFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (wantRT)
      return isColorRTFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    return isSamplable2D(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;

  case D3DRTYPE_VOLUMETEXTURE:
  case D3DRTYPE_VOLUME:
    // 3D textures: no compressed (DXT) on Metal, no DS, no RT.
    if (wantDS || wantRT)
      return D3DERR_NOTAVAILABLE;
    switch (CheckFormat) {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_A8L8:
    case D3DFMT_R16F:
    case D3DFMT_G16R16F:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_R32F:
      return D3D_OK;
    default:
      return D3DERR_NOTAVAILABLE;
    }

  case D3DRTYPE_VERTEXBUFFER:
    return CheckFormat == D3DFMT_VERTEXDATA ? D3D_OK : D3DERR_NOTAVAILABLE;

  case D3DRTYPE_INDEXBUFFER:
    return (CheckFormat == D3DFMT_INDEX16 || CheckFormat == D3DFMT_INDEX32) ? D3D_OK : D3DERR_NOTAVAILABLE;

  default:
    return D3DERR_INVALIDCALL;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceMultiSampleType(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL, D3DMULTISAMPLE_TYPE MultiSampleType,
    DWORD *pQualityLevels
) {
  D9_TRACE("IDirect3D9::CheckDeviceMultiSampleType");
  if (Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // wined3d caps the request at 16 — anything beyond is invalid input.
  // wine dlls/d3d9/directx.c:262.
  if (MultiSampleType > D3DMULTISAMPLE_16_SAMPLES)
    return D3DERR_INVALIDCALL;

  auto isMSAACapable = [](D3DFORMAT f) {
    switch (f) {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_R16F:
    case D3DFMT_G16R16F:
    case D3DFMT_D16:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D32:
      return true;
    default:
      return false;
    }
  };

  HRESULT hr;
  if (MultiSampleType == D3DMULTISAMPLE_NONE || MultiSampleType == D3DMULTISAMPLE_NONMASKABLE) {
    hr = D3D_OK;
  } else if (!isMSAACapable(SurfaceFormat)) {
    hr = D3DERR_NOTAVAILABLE;
  } else if (MultiSampleType == D3DMULTISAMPLE_2_SAMPLES || MultiSampleType == D3DMULTISAMPLE_4_SAMPLES) {
    // Apple Silicon GPUs support 2x/4x MSAA on color and depth formats.
    hr = D3D_OK;
  } else {
    // 8x/16x — Apple GPUs don't generally support these on the
    // backbuffer-class formats games actually use.
    hr = D3DERR_NOTAVAILABLE;
  }

  if (pQualityLevels) {
    // D3D9 quality levels are vendor-specific; "1" is the safe answer
    // for a non-NVIDIA GPU. dxmt deliberately writes this even on the
    // NOTAVAILABLE path so callers that read pQualityLevels regardless
    // of HRESULT (a common bug we don't want to be the trigger for)
    // see a defined value rather than uninitialised memory. wined3d
    // leaves the out-pointer untouched on failure — we are stricter
    // by design.
    *pQualityLevels = 1;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDepthStencilMatch(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
    D3DFORMAT DepthStencilFormat
) {
  D9_TRACE("IDirect3D9::CheckDepthStencilMatch");
  if (Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;
  if (AdapterFormat != D3DFMT_X8R8G8B8 && AdapterFormat != D3DFMT_R5G6B5 && AdapterFormat != D3DFMT_X1R5G5B5)
    return D3DERR_NOTAVAILABLE;

  // Lists must match CheckDeviceFormat's color-RT + DS coverage: apps
  // that probe one then the other expect consistent answers. wined3d
  // does real bit-depth pairing; on Metal the depth/stencil attachment
  // is independent of the colour attachment, so any legal (RT, DS) is
  // compatible.
  auto isColorRT = [](D3DFORMAT f) {
    switch (f) {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_R16F:
    case D3DFMT_G16R16F:
    case D3DFMT_R32F:
    case D3DFMT_G32R32F:
    case D3DFMT_A32B32G32R32F:
    case D3DFMT_G16R16:
      return true;
    default:
      return false;
    }
  };
  auto isDS = [](D3DFORMAT f) {
    switch (f) {
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
    case D3DFMT_D24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D32:
    case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_D24FS8:
      return true;
    default:
      return false;
    }
  };

  return (isColorRT(RenderTargetFormat) && isDS(DepthStencilFormat)) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceFormatConversion(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat
) {
  D9_TRACE("IDirect3D9::CheckDeviceFormatConversion");
  if (Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // wined3d short-circuits the same-format case before consulting its
  // table. wine dlls/d3d9/directx.c:347.
  if (SourceFormat == TargetFormat)
    return D3D_OK;

  // Used by StretchRect's format-compatibility check. Metal's blit
  // encoder can copy between any two formats whose pixel sizes match,
  // and shader-based conversion handles the rest at draw time, so the
  // safe answer for the common RGB/RGBA family is yes.
  auto isRGBStretchTarget = [](D3DFORMAT f) {
    switch (f) {
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8R8G8B8:
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_R16F:
    case D3DFMT_G16R16F:
      return true;
    default:
      return false;
    }
  };
  return (isRGBStretchTarget(SourceFormat) && isRGBStretchTarget(TargetFormat)) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) {
  D9_TRACE("IDirect3D9::GetDeviceCaps");
  if (!pCaps)
    return D3DERR_INVALIDCALL;

  auto devices = WMT::CopyAllDevices();
  if (Adapter >= devices.count())
    return D3DERR_INVALIDCALL;

  std::memset(pCaps, 0, sizeof(*pCaps));

  // wined3d_caps_from_wined3dcaps masks each field down to the bits
  // D3D9 actually defines. We start from those masks directly — they
  // describe a hardware-T&L SM 3.0 GPU, which is what most D3D9 games
  // expect. wine dlls/d3d9/device.c:369 d3d9_caps_from_wined3dcaps.
  pCaps->DeviceType = DeviceType;
  pCaps->AdapterOrdinal = Adapter;

  pCaps->Caps = D3DCAPS_READ_SCANLINE;
  pCaps->Caps2 = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_CANCALIBRATEGAMMA | D3DCAPS2_RESERVED |
                 D3DCAPS2_CANMANAGERESOURCE | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_CANAUTOGENMIPMAP;
  pCaps->Caps3 = D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION |
                 D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
  // Per DXVK d3d9_caps.cpp:431-436 — claim the full set the Present
  // path honours. swapchain.cpp:486-497 already maps INTERVAL_TWO/
  // THREE/FOUR to their N-vsync dwell; pre-port we under-claimed
  // (apps fell back to ONE without knowing TWO+ was available).
  // D3DPRESENT_INTERVAL_DEFAULT (=0) is reportable too; some apps
  // bit-test for it as a "no-vsync hint accepted" probe.
  pCaps->PresentationIntervals = D3DPRESENT_INTERVAL_DEFAULT | D3DPRESENT_INTERVAL_ONE | D3DPRESENT_INTERVAL_TWO |
                                 D3DPRESENT_INTERVAL_THREE | D3DPRESENT_INTERVAL_FOUR | D3DPRESENT_INTERVAL_IMMEDIATE;
  pCaps->CursorCaps = D3DCURSORCAPS_COLOR | D3DCURSORCAPS_LOWRES;

  pCaps->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY | D3DDEVCAPS_TLVERTEXSYSTEMMEMORY |
                   D3DDEVCAPS_TLVERTEXVIDEOMEMORY | D3DDEVCAPS_TEXTURESYSTEMMEMORY | D3DDEVCAPS_TEXTUREVIDEOMEMORY |
                   D3DDEVCAPS_DRAWPRIMTLVERTEX | D3DDEVCAPS_CANRENDERAFTERFLIP | D3DDEVCAPS_TEXTURENONLOCALVIDMEM |
                   D3DDEVCAPS_DRAWPRIMITIVES2 | D3DDEVCAPS_DRAWPRIMITIVES2EX | D3DDEVCAPS_HWTRANSFORMANDLIGHT |
                   D3DDEVCAPS_CANBLTSYSTONONLOCAL | D3DDEVCAPS_HWRASTERIZATION | D3DDEVCAPS_PUREDEVICE;
  pCaps->DevCaps2 = D3DDEVCAPS2_STREAMOFFSET | D3DDEVCAPS2_VERTEXELEMENTSCANSHARESTREAMOFFSET |
                    D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES;

  pCaps->PrimitiveMiscCaps = D3DPMISCCAPS_MASKZ | D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                             D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_CLIPPLANESCALEDPOINTS |
                             D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP | D3DPMISCCAPS_INDEPENDENTWRITEMASKS |
                             D3DPMISCCAPS_SEPARATEALPHABLEND | D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS |
                             D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING | D3DPMISCCAPS_FOGVERTEXCLAMPED;

  pCaps->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX |
                      D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGRANGE |
                      D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG |
                      D3DPRASTERCAPS_COLORPERSPECTIVE | D3DPRASTERCAPS_SCISSORTEST |
                      D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;

  // Compare ops, blend factors, alpha-test ops — claim the full set.
  pCaps->ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
                    D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
  pCaps->AlphaCmpCaps = pCaps->ZCmpCaps;
  pCaps->SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
                        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA |
                        D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                        D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHINVSRCALPHA | D3DPBLENDCAPS_BLENDFACTOR |
                        D3DPBLENDCAPS_INVSRCCOLOR2 | D3DPBLENDCAPS_SRCCOLOR2;
  pCaps->DestBlendCaps = pCaps->SrcBlendCaps;

  pCaps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                     D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;

  pCaps->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_ALPHAPALETTE |
                       D3DPTEXTURECAPS_PROJECTED | D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP |
                       D3DPTEXTURECAPS_MIPMAP | D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_MIPCUBEMAP |
                       D3DPTEXTURECAPS_NOPROJECTEDBUMPENV;

  // Filter caps cluster — wined3d strips its superset to exactly this
  // mask in d3d9_caps_from_wined3dcaps; mirror the same set.
  const DWORD textureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                                  D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT |
                                  D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR |
                                  D3DPTFILTERCAPS_MAGFANISOTROPIC;
  pCaps->TextureFilterCaps = textureFilterCaps;
  pCaps->CubeTextureFilterCaps = textureFilterCaps;
  pCaps->VolumeTextureFilterCaps = textureFilterCaps;
  pCaps->VertexTextureFilterCaps = textureFilterCaps;
  // StretchRect uses Metal's blit encoder which supports only point and
  // linear filtering — anisotropic is sampler-only. DXVK d3d9_caps.cpp:
  // 728-740 reports only POINT|LINEAR. Pre-port dxmt reused the full
  // textureFilterCaps mask, so apps that requested D3DTEXF_ANISOTROPIC
  // on StretchRect silently degraded to LINEAR.
  pCaps->StretchRectFilterCaps =
      D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;

  pCaps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
  pCaps->VolumeTextureAddressCaps = pCaps->TextureAddressCaps;

  pCaps->LineCaps = D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP |
                    D3DLINECAPS_FOG | D3DLINECAPS_ANTIALIAS;

  pCaps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                       D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
                       D3DSTENCILCAPS_TWOSIDED;

  pCaps->FVFCaps = 8 | D3DFVFCAPS_PSIZE; // 8 = max texture coord count

  pCaps->TextureOpCaps =
      D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
      D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
      D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
      D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM |
      D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_PREMODULATE | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR |
      D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA | D3DTEXOPCAPS_DOTPRODUCT3 | D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP |
      D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE;
  pCaps->MaxTextureBlendStages = 8;
  pCaps->MaxSimultaneousTextures = 8;

  pCaps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
                                D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TWEENING |
                                D3DVTXPCAPS_TEXGEN_SPHEREMAP | D3DVTXPCAPS_NO_TEXGEN_NONLOCALVIEWER;
  pCaps->MaxActiveLights = 8;
  pCaps->MaxUserClipPlanes = 8;
  pCaps->MaxVertexBlendMatrices = 4;
  pCaps->MaxVertexBlendMatrixIndex = 0;

  // Geometry / format limits. 16384 is the Metal-on-Apple-Silicon
  // texture max for non-MSAA 2D textures.
  pCaps->MaxTextureWidth = 16384;
  pCaps->MaxTextureHeight = 16384;
  pCaps->MaxVolumeExtent = 2048;
  pCaps->MaxTextureRepeat = 8192;
  pCaps->MaxTextureAspectRatio = 16384;
  pCaps->MaxAnisotropy = 16;
  pCaps->MaxVertexW = 1e10f;
  pCaps->GuardBandLeft = -1e9f;
  pCaps->GuardBandTop = -1e9f;
  pCaps->GuardBandRight = 1e9f;
  pCaps->GuardBandBottom = 1e9f;
  pCaps->ExtentsAdjust = 0.0f;
  // Apple Silicon caps point primitives at 511.0 per Metal Feature Set
  // Tables (Apple1+). dxmt's own D3DRS_POINTSIZE_MAX default at
  // d3d9_device.cpp:1086 already uses 511.0f, citing the Vulkan
  // pointSizeRange[1] equivalent — pre-port the CAPS value claimed 256,
  // so games that scaled their POINTSIZE register against MaxPointSize
  // got points clamped half the size they expected. NOTE: VS-side oPts
  // output (dxso_compile.cpp:628 TODO) is not yet plumbed; apps that
  // read this cap and write oPts will still see rasterizer-default
  // size 1.0 until the oPts emitter lands.
  pCaps->MaxPointSize = 511.0f;
  pCaps->MaxPrimitiveCount = 0x00FFFFFF;
  pCaps->MaxVertexIndex = 0x00FFFFFF;
  pCaps->MaxStreams = 16;
  pCaps->MaxStreamStride = 508;
  pCaps->MaxNpatchTessellationLevel = 0.0f;

  pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
  pCaps->MaxVertexShaderConst = 256;
  pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
  pCaps->PixelShader1xMaxValue = 65504.0f;

  // SM 2.0/3.0 sub-caps: claim full predication / dynamic flow control.
  pCaps->VS20Caps.Caps = D3DVS20CAPS_PREDICATION;
  pCaps->VS20Caps.DynamicFlowControlDepth = D3DVS20_MAX_DYNAMICFLOWCONTROLDEPTH;
  pCaps->VS20Caps.NumTemps = D3DVS20_MAX_NUMTEMPS;
  pCaps->VS20Caps.StaticFlowControlDepth = D3DVS20_MAX_STATICFLOWCONTROLDEPTH;
  pCaps->PS20Caps.Caps = D3DPS20CAPS_ARBITRARYSWIZZLE | D3DPS20CAPS_GRADIENTINSTRUCTIONS | D3DPS20CAPS_PREDICATION |
                         D3DPS20CAPS_NODEPENDENTREADLIMIT | D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT;
  pCaps->PS20Caps.DynamicFlowControlDepth = D3DPS20_MAX_DYNAMICFLOWCONTROLDEPTH;
  pCaps->PS20Caps.NumTemps = D3DPS20_MAX_NUMTEMPS;
  pCaps->PS20Caps.StaticFlowControlDepth = D3DPS20_MAX_STATICFLOWCONTROLDEPTH;
  pCaps->PS20Caps.NumInstructionSlots = D3DPS20_MAX_NUMINSTRUCTIONSLOTS;
  pCaps->MaxVShaderInstructionsExecuted = 65535;
  pCaps->MaxPShaderInstructionsExecuted = 65535;
  pCaps->MaxVertexShader30InstructionSlots = 32768;
  pCaps->MaxPixelShader30InstructionSlots = 32768;

  pCaps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N | D3DDTCAPS_USHORT2N |
                     D3DDTCAPS_USHORT4N | D3DDTCAPS_UDEC3 | D3DDTCAPS_DEC3N | D3DDTCAPS_FLOAT16_2 | D3DDTCAPS_FLOAT16_4;

  pCaps->NumSimultaneousRTs = 4;

  // Single-adapter group; matches what wined3d does when there's only
  // one output on the wined3d_adapter.
  pCaps->MasterAdapterOrdinal = 0;
  pCaps->AdapterOrdinalInGroup = 0;
  pCaps->NumberOfAdaptersInGroup = 1;

  return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterMonitor(UINT Adapter) {
  D9_TRACE("IDirect3D9::GetAdapterMonitor");
  if (Adapter >= WMT::CopyAllDevices().count())
    return nullptr;
  return wsi::enumMonitors(Adapter);
}

// All D3DPRESENTFLAG_* bits the D3D9 runtime knows about. Anything
// outside this mask is a higher-version or vendor extension we don't
// understand. Mirrors dlls/d3d9/d3d9_private.h:44 D3DPRESENTFLAGS_MASK.
static constexpr DWORD kD3DPresentFlagsMask = 0x00000fffu;

// Mirror of wined3d_swapchain_desc_from_d3d9 — validates the parameters
// the runtime contract pins down. Apple Silicon does not honour any
// fullscreen-only fields directly, but the validation gates are still
// the contract callers expect. Reference: dlls/d3d9/device.c:303
// wined3d_swapchain_desc_from_d3d9.
bool
ValidatePresentParams(const D3DPRESENT_PARAMETERS &p, bool isEx) {
  D3DSWAPEFFECT highestSwapEffect = isEx ? D3DSWAPEFFECT_FLIPEX : D3DSWAPEFFECT_COPY;
  UINT highestBackBufferCount = isEx ? 30 : 3;

  if (p.SwapEffect == 0 || p.SwapEffect > highestSwapEffect)
    return false;
  if (p.BackBufferCount > highestBackBufferCount)
    return false;
  if (p.SwapEffect == D3DSWAPEFFECT_COPY && p.BackBufferCount > 1)
    return false;

  switch (p.PresentationInterval) {
  case D3DPRESENT_INTERVAL_DEFAULT:
  case D3DPRESENT_INTERVAL_ONE:
  case D3DPRESENT_INTERVAL_TWO:
  case D3DPRESENT_INTERVAL_THREE:
  case D3DPRESENT_INTERVAL_FOUR:
  case D3DPRESENT_INTERVAL_IMMEDIATE:
    break;
  default:
    return false;
  }
  return true;
}

// Resolve the spec-permitted "use the runtime default" placeholders the
// caller may have left in the params struct. Mutates a private copy
// (the chain ctor and the device ctor both expect concrete values).
// Returns false if a value is invalid in a way ValidatePresentParams
// didn't catch — currently a backbuffer format dxmt can't lower to
// Metal, or a windowed-mode zero extent on a window that has no
// client rect (typically a minimised or un-shown window).
//
// hwndFallback is the CreateDevice hFocusWindow; the spec says the
// effective render window is hDeviceWindow when set, hFocusWindow
// otherwise. Reference: wined3d swapchain.c:1436-1452 — same shape,
// same 8-pixel floor for zero-rect windows.
bool
CanonicalisePresentParams(D3DPRESENT_PARAMETERS &p, HWND hwndFallback) {
  if (p.BackBufferCount == 0)
    p.BackBufferCount = 1;
  if (p.BackBufferFormat == D3DFMT_UNKNOWN)
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
  if (D3DFormatToMetal(p.BackBufferFormat, D3D9FormatUsage::RenderTarget) == WMTPixelFormatInvalid)
    return false;
  // EnableAutoDepthStencil=TRUE with AutoDepthStencilFormat=UNKNOWN is
  // a spec-permitted "use the runtime default" placeholder per MSDN
  // CreateDevice; wined3d swapchain.c:1455 substitutes D24S8. Without
  // this, the auto-DS allocation path at d3d9_device.cpp::createAutoDS
  // would see UNKNOWN, call D3DFormatToMetal → Invalid, and skip
  // allocating the implicit DS — leaving EnableAutoDepthStencil=TRUE
  // apps with no DS bound on the first frame.
  if (p.EnableAutoDepthStencil && p.AutoDepthStencilFormat == D3DFMT_UNKNOWN)
    p.AutoDepthStencilFormat = D3DFMT_D24S8;

  if (p.Windowed && (p.BackBufferWidth == 0 || p.BackBufferHeight == 0)) {
#ifdef _WIN32
    HWND deriveFrom = p.hDeviceWindow ? p.hDeviceWindow : hwndFallback;
    if (deriveFrom) {
      RECT rc{};
      GetClientRect(deriveFrom, &rc);
      if (p.BackBufferWidth == 0)
        p.BackBufferWidth = rc.right > 0 ? static_cast<UINT>(rc.right) : 8;
      if (p.BackBufferHeight == 0)
        p.BackBufferHeight = rc.bottom > 0 ? static_cast<UINT>(rc.bottom) : 8;
    }
#else
    (void)hwndFallback;
#endif
  }

  if (p.BackBufferWidth == 0 || p.BackBufferHeight == 0)
    return false;
  return true;
}

// Log unhandled D3DPRESENTFLAG_* bits the same way wined3d FIXMEs them
// — so contributors adding handling for LOCKABLE_BACKBUFFER, VIDEO,
// NOAUTOROTATE, etc. have an obvious entry point. Reference:
// dlls/d3d9/device.c:363.
static void
WarnUnhandledPresentFlags(DWORD flags) {
  if (DWORD unhandled = flags & ~kD3DPresentFlagsMask)
    Logger::warn(str::format("Unknown D3DPRESENT_PARAMETERS::Flags bits 0x", std::hex, unhandled));
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPresentationParameters, IDirect3DDevice9 **ppReturnedDeviceInterface
) {
  D9_TRACE("IDirect3D9::CreateDevice");
  if (!ppReturnedDeviceInterface)
    return D3DERR_INVALIDCALL;
  *ppReturnedDeviceInterface = nullptr;
  if (!pPresentationParameters)
    return D3DERR_INVALIDCALL;

  auto devices = WMT::CopyAllDevices();
  if (Adapter >= devices.count())
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL && DeviceType != D3DDEVTYPE_REF && DeviceType != D3DDEVTYPE_SW)
    return D3DERR_INVALIDCALL;
  if (!ValidatePresentParams(*pPresentationParameters, /*isEx=*/false))
    return D3DERR_INVALIDCALL;

  WarnUnhandledPresentFlags(pPresentationParameters->Flags);

  // Don't mutate the caller's struct — apps reuse it for Reset and
  // expect the original shape back. Canonicalise on a private copy.
  D3DPRESENT_PARAMETERS validated = *pPresentationParameters;
  if (!CanonicalisePresentParams(validated, hFocusWindow))
    return D3DERR_INVALIDCALL;

  WMT::Reference<WMT::Device> metalDevice = devices.object(Adapter);
  if (!metalDevice.handle)
    return D3DERR_OUTOFVIDEOMEMORY;

  auto *device = new MTLD3D9Device(
      this, /*isEx=*/false, Adapter, DeviceType, hFocusWindow, BehaviorFlags, validated, std::move(metalDevice)
  );
  device->AddRef();
  *ppReturnedDeviceInterface = static_cast<IDirect3DDevice9 *>(device);
  return D3D_OK;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) {
  D9_TRACE("IDirect3D9Ex::GetAdapterModeCountEx");
  if (!pFilter || Adapter >= WMT::CopyAllDevices().count())
    return 0;
  if (pFilter->Format != D3DFMT_X8R8G8B8)
    return 0;
  if (pFilter->ScanLineOrdering == D3DSCANLINEORDERING_INTERLACED)
    return 0;
  HMONITOR mon = wsi::enumMonitors(Adapter);
  if (!mon)
    return 0;
  UINT n = 0;
  wsi::WsiMode wm{};
  while (wsi::getDisplayMode(mon, n, &wm))
    ++n;
  return n;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::EnumAdapterModesEx(
    UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter, UINT Mode, D3DDISPLAYMODEEX *pMode
) {
  D9_TRACE("IDirect3D9Ex::EnumAdapterModesEx");
  if (!pFilter || !pMode || Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;
  if (pFilter->Format != D3DFMT_X8R8G8B8)
    return D3DERR_INVALIDCALL;
  if (pFilter->ScanLineOrdering == D3DSCANLINEORDERING_INTERLACED)
    return D3DERR_INVALIDCALL;

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getDisplayMode(mon, Mode, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Size = sizeof(*pMode);
  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = wm.refreshRate.denominator ? (wm.refreshRate.numerator / wm.refreshRate.denominator) : 60;
  pMode->Format = D3DFMT_X8R8G8B8;
  pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  D9_TRACE("IDirect3D9Ex::GetAdapterDisplayModeEx");
  if (!pMode || Adapter >= WMT::CopyAllDevices().count())
    return D3DERR_INVALIDCALL;
  if (pMode->Size != sizeof(*pMode))
    return D3DERR_INVALIDCALL;

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getCurrentDisplayMode(mon, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = wm.refreshRate.denominator ? (wm.refreshRate.numerator / wm.refreshRate.denominator) : 60;
  pMode->Format = D3DFMT_X8R8G8B8;
  pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  if (pRotation)
    *pRotation = D3DDISPLAYROTATION_IDENTITY;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CreateDeviceEx(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS *pPresentationParameters, D3DDISPLAYMODEEX *pFullscreenDisplayMode,
    IDirect3DDevice9Ex **ppDevice
) {
  D9_TRACE("IDirect3D9Ex::CreateDeviceEx");
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = nullptr;
  if (!m_isEx)
    return D3DERR_INVALIDCALL;
  if (!pPresentationParameters)
    return D3DERR_INVALIDCALL;
  // The MS contract says pFullscreenDisplayMode must be NULL in
  // windowed mode, but real DX9Ex titles (some WoW expansions, a
  // handful of others) sometimes pass non-null garbage anyway and
  // wined3d tolerates them. Only reject the genuine spec violation
  // (fullscreen request with no mode), match wined3d's silently-
  // tolerate stance for the windowed-with-stale-mode case.
  if (!pPresentationParameters->Windowed && !pFullscreenDisplayMode)
    return D3DERR_INVALIDCALL;

  auto devices = WMT::CopyAllDevices();
  if (Adapter >= devices.count())
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL && DeviceType != D3DDEVTYPE_REF && DeviceType != D3DDEVTYPE_SW)
    return D3DERR_INVALIDCALL;
  if (!ValidatePresentParams(*pPresentationParameters, /*isEx=*/true))
    return D3DERR_INVALIDCALL;

  WarnUnhandledPresentFlags(pPresentationParameters->Flags);

  D3DPRESENT_PARAMETERS validated = *pPresentationParameters;
  if (!CanonicalisePresentParams(validated, hFocusWindow))
    return D3DERR_INVALIDCALL;

  WMT::Reference<WMT::Device> metalDevice = devices.object(Adapter);
  if (!metalDevice.handle)
    return D3DERR_OUTOFVIDEOMEMORY;

  auto *device = new MTLD3D9Device(
      this, /*isEx=*/true, Adapter, DeviceType, hFocusWindow, BehaviorFlags, validated, std::move(metalDevice)
  );
  device->AddRef();
  *ppDevice = static_cast<IDirect3DDevice9Ex *>(device);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterLUID(UINT Adapter, LUID *pLUID) {
  D9_TRACE("IDirect3D9Ex::GetAdapterLUID");
  if (!pLUID)
    return D3DERR_INVALIDCALL;
  auto devices = WMT::CopyAllDevices();
  if (Adapter >= devices.count())
    return D3DERR_INVALIDCALL;
  uint64_t id = devices.object(Adapter).registryID();
  pLUID->LowPart = static_cast<DWORD>(id & 0xFFFFFFFFu);
  pLUID->HighPart = static_cast<LONG>(id >> 32);
  return D3D_OK;
}

} // namespace dxmt
