#include "d3d9_interface.hpp"

#include "Metal.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "dxmt_format.hpp"
#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "wsi_monitor.hpp"

#include <cstdio>
#include <cstring>

namespace dxmt {

// Pin Metal's own shader and pipeline cache to the per-executable
// directory the disk shader cache uses (the d3d11 backend does the
// same at provider init). Without it the OS cache keys off the wine
// loader's identity and pipeline links never persist across runs.
static void
initializeMetalCachePath() {
  static std::once_flag once;
  std::call_once(once, []() {
    if (env::getEnvVar("DXMT_USE_DEFAULT_METAL_CACHE") == "1")
      return;
    auto path = GetDXMTShaderCacheDirectory() + "com.apple.metal";
    if (path.size() >= 2 && path[1] == ':')
      path = env::getUnixPath(path);
    if (!WMTSetMetalShaderCachePath(path.c_str()))
      Logger::info("Failed to set Metal cache path, fallback to system default");
  });
}

MTLD3D9Interface::MTLD3D9Interface(UINT SDKVersion, bool isEx) :
    m_sdkVersion(SDKVersion),
    m_isEx(isEx),
    m_adapters(WMT::CopyAllDevices()),
    m_adapterCount(m_adapters ? static_cast<UINT>(m_adapters.count()) : 0u) {
  initializeMetalCachePath();
}

// wsi reports refresh as a rational (numerator/denominator). Either
// can be zero on a display where the OS doesn't know the real rate
// (sleep/wake transitions, headless monitors). Match d3d11_swapchain
// and d3d9_swapchain: only divide when both are non-zero; otherwise
// fall back to 60 so apps that gate animation on the rate don't
// start dividing by zero or scheduling at 0 Hz.
static UINT
refreshRateHzOr60(const wsi::WsiMode &wm) {
  if (wm.refreshRate.denominator == 0 || wm.refreshRate.numerator == 0)
    return 60;
  return wm.refreshRate.numerator / wm.refreshRate.denominator;
}

MTLD3D9Interface::~MTLD3D9Interface() = default;

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::QueryInterface(REFIID riid, void **ppvObject) {
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
  if (!pInitializeFunction)
    return D3DERR_INVALIDCALL;
  return D3DERR_NOTAVAILABLE;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterCount() {
  return m_adapterCount;
}

// Canonical D3D9 device UID. wined3d hands this back from
// wined3d_adapter_get_identifier (directx.c, :1805). Some 2005-era
// titles compare DeviceIdentifier against this exact GUID and treat
// any other value as a "non-standard" adapter, falling into a SetupAPI/
// EnumDisplayDevices fallback that walks the registry every frame.
static const GUID kD3DDeviceD3DUID = {0xaeb2cdd4, 0x6e41, 0x43ea, {0x94, 0x1c, 0x83, 0x61, 0xcc, 0x76, 0x07, 0x81}};

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) {
  if (!pIdentifier)
    return D3DERR_INVALIDCALL;

  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  auto device = m_adapters.object(Adapter);

  std::memset(pIdentifier, 0, sizeof(*pIdentifier));
  std::snprintf(pIdentifier->Driver, MAX_DEVICE_IDENTIFIER_STRING, "%s", "DXMT (Metal)");
  device.name().getCString(pIdentifier->Description, MAX_DEVICE_IDENTIFIER_STRING, WMTUTF8StringEncoding);
  std::snprintf(pIdentifier->DeviceName, sizeof(pIdentifier->DeviceName), "%s", "\\\\.\\DISPLAY1");

  // 0x106B is Apple's PCI vendor ID, the same value dxgi_adapter.cpp
  // hands back to D3D11 callers. DeviceId / SubSysId / Revision aren't
  // meaningful for Metal but keep DeviceId non-zero; some titles
  // treat 0 as "no adapter" and fall through to a different code path.
  pIdentifier->VendorId = 0x106B;
  pIdentifier->DeviceId = 1;
  // SubSysId / Revision intentionally 0; wined3d does the same.

  // Apps gate driver-bug workarounds on this version; a low value
  // re-enables ancient workaround paths. DXVK reports INT64_MAX
  // ("newest possible driver", d3d9_adapter.cpp) after games
  // misbehaved on small values; mirror it.
  pIdentifier->DriverVersion.QuadPart = INT64_MAX;

  pIdentifier->DeviceIdentifier = kD3DDeviceD3DUID;
  pIdentifier->WHQLLevel = (Flags & D3DENUM_WHQL_LEVEL) ? 1 : 0;

  return D3D_OK;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) {
  if (Adapter >= m_adapterCount)
    return 0;
  // wined3d filters: D3D9 only enumerates X8R8G8B8 / R5G6B5 here, and
  // Apple displays don't expose 16-bit modes any more. Reject everything
  // except X8R8G8B8. wine dlls/d3d9/directx.c (Ex variant).
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
  if (!pMode || Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (Format != D3DFMT_X8R8G8B8)
    return D3DERR_INVALIDCALL;

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getDisplayMode(mon, Mode, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = refreshRateHzOr60(wm);
  pMode->Format = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) {
  if (!pMode || Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getCurrentDisplayMode(mon, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = refreshRateHzOr60(wm);
  // D3D9 only ever advertises X8R8G8B8 / R5G6B5 here. Pick X8R8G8B8.
  // matches what wined3d does on a 32-bit GL desktop and what every
  // modern Windows desktop reports.
  pMode->Format = D3DFMT_X8R8G8B8;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceType(
    UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT DisplayFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DevType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // There are only four display formats, and an alpha format is never one of
  // them. wined3d directx.c wined3d_check_device_type.
  if (DisplayFormat != D3DFMT_X8R8G8B8 && DisplayFormat != D3DFMT_R5G6B5 &&
      DisplayFormat != D3DFMT_X1R5G5B5 && DisplayFormat != D3DFMT_A2R10G10B10)
    return D3DERR_NOTAVAILABLE;

  if (!bWindowed) {
    // Fullscreen requires the display format to actually have enumerable modes.
    if (GetAdapterModeCount(Adapter, DisplayFormat) == 0)
      return D3DERR_NOTAVAILABLE;
  } else if (DisplayFormat == D3DFMT_A2R10G10B10) {
    // A2R10G10B10 is a fullscreen-only display format.
    return D3DERR_NOTAVAILABLE;
  }

  if (bWindowed) {
    // Windowed mode permits the driver to convert the backbuffer to the display
    // format at present. Backbuffer == UNKNOWN means "use the display format",
    // but only here: fullscreen keeps UNKNOWN so the strict match below rejects
    // it (wined3d substitutes inside the conversion branch only).
    if (BackBufferFormat == D3DFMT_UNKNOWN)
      BackBufferFormat = DisplayFormat;
    if (FAILED(CheckDeviceFormatConversion(Adapter, DevType, BackBufferFormat, DisplayFormat)))
      return D3DERR_NOTAVAILABLE;
  } else {
    // Fullscreen: ignoring alpha, the display and backbuffer formats must match
    // exactly, so each display format pairs only with itself or its alpha twin.
    bool match = DisplayFormat == BackBufferFormat ||
                 (DisplayFormat == D3DFMT_X1R5G5B5 && BackBufferFormat == D3DFMT_A1R5G5B5) ||
                 (DisplayFormat == D3DFMT_X8R8G8B8 && BackBufferFormat == D3DFMT_A8R8G8B8);
    if (!match)
      return D3DERR_NOTAVAILABLE;
  }

  // Finally the backbuffer format must be usable as a render target.
  if (FAILED(
          CheckDeviceFormat(Adapter, DevType, DisplayFormat, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, BackBufferFormat)
      ))
    return D3DERR_NOTAVAILABLE;

  return D3D_OK;
}

namespace {

// D16_LOCKABLE and D32_LOCKABLE are NOT advertised: a lockable depth surface
// lets the app read depth back via LockRect, which needs a host-visible
// backing dxmt's GPU-private depth textures do not have, so the lock would
// fail. D16_LOCKABLE is AMD-only and D32_LOCKABLE is unsupported everywhere
// (DXVK d3d9_format.cpp). Plain D32 (32-bit unorm depth) is likewise dropped:
// no hardware driver exposes it, so wined3d intentionally refuses it too
// (wined3d utils.c); apps fall back to D24X8 / D16. D32F_LOCKABLE stays: it
// is the only float depth format, used as a plain depth buffer (the unlikely
// lock path is the separate lockable-depth feature).
bool
isDSFormat(D3DFORMAT f) {
  switch (f) {
  case D3DFMT_D16:
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D32F_LOCKABLE:
  case D3DFMT_D24FS8:
  case D3DFMT_D15S1:
  // FOURCC sampleable-depth aliases (D3DFormatToMetal maps them).
  // Games probe via CheckDeviceFormat(... DEPTHSTENCIL ...,
  // INTZ) before allocating a shadow-map intermediate; NOTAVAILABLE
  // here would force them off the fast PCF path even though creation
  // would succeed. wined3d's directx.c exposes INTZ whenever the matching
  // native depth format is available; DF16 / DF24 are ATI-specific FOURCCs
  // with no wined3d support at all (they come from d9vk / DXVK), so this set
  // advertises INTZ on the wined3d precedent and DF16 / DF24 on the d9vk one.
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
  case D3DFMT_DF16:
    return true;
  default:
    return false;
  }
}

} // namespace

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceFormat(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType,
    D3DFORMAT CheckFormat
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;

  // The adapter-format validation precedes the device-type gate: a
  // D3DFMT_UNKNOWN adapter format is D3DERR_INVALIDCALL for every device
  // type, since the d3d9 runtime validates the adapter format before the
  // device type is ever consumed. wined3d takes the same X8R8G8B8 / R5G6B5
  // / X1R5G5B5 set; ddraw advertises others but D3D9 doesn't.
  // wine dlls/d3d9/directx.c.
  if (AdapterFormat != D3DFMT_X8R8G8B8 && AdapterFormat != D3DFMT_R5G6B5 && AdapterFormat != D3DFMT_X1R5G5B5)
    return AdapterFormat ? D3DERR_NOTAVAILABLE : D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  const bool wantRT = (Usage & D3DUSAGE_RENDERTARGET) != 0;
  const bool wantDS = (Usage & D3DUSAGE_DEPTHSTENCIL) != 0;
  if (wantRT && wantDS)
    return D3DERR_NOTAVAILABLE;

  // A depth-stencil format on a cube texture is never creatable
  // (validate_texture_create rejects it: no native driver backs a depth cube).
  // Deny the format probe too so CheckDeviceFormat and the create stay in step;
  // a split probe/create trips an app that gates a code path on the query.
  if (RType == D3DRTYPE_CUBETEXTURE && IsDepthStencilFormat(CheckFormat))
    return D3DERR_NOTAVAILABLE;

  // D3DUSAGE_AUTOGENMIPMAP asks whether the runtime can auto-build a texture's
  // mip chain. It is a texture-only query (a standalone surface or a volume
  // slice has no mip chain to generate), so it is an invalid usage on any other
  // resource type. wined3d allows the bit only in the 2D-texture allowed_usage
  // set (wined3d directx.c).
  const bool wantAutoGen = (Usage & D3DUSAGE_AUTOGENMIPMAP) != 0;
  if (wantAutoGen && RType != D3DRTYPE_TEXTURE && RType != D3DRTYPE_CUBETEXTURE)
    return D3DERR_NOTAVAILABLE;

  // D3DUSAGE_QUERY_SRGBREAD / SRGBWRITE: apps probe sRGB sampling/writing.
  // dxmt aliases to *_sRGB Metal format; return OK for formats with sRGB.
  // Apps getting NOTAVAILABLE fall back to non-sRGB shader path.
  const bool querySrgbRead = (Usage & D3DUSAGE_QUERY_SRGBREAD) != 0;
  const bool querySrgbWrite = (Usage & D3DUSAGE_QUERY_SRGBWRITE) != 0;
  if (querySrgbRead || querySrgbWrite) {
    // sRGB read is a sampler capability, sRGB write a render-target one.
    // A plain surface (no shader-resource role) carries SRGBWRITE only
    // when RENDERTARGET is also requested, and never SRGBREAD; the sampled
    // resource types carry both. wined3d gates the query bits the same way
    // through its per-resource allowed_usage table (wined3d directx.c).
    const bool sampledType = RType == D3DRTYPE_TEXTURE || RType == D3DRTYPE_CUBETEXTURE ||
                             RType == D3DRTYPE_VOLUMETEXTURE || RType == D3DRTYPE_VOLUME;
    if (querySrgbRead && !sampledType)
      return D3DERR_NOTAVAILABLE;
    if (querySrgbWrite && !sampledType && !wantRT)
      return D3DERR_NOTAVAILABLE;

    auto metal_fmt = D3DFormatToMetal(CheckFormat, D3D9FormatUsage::SampleableTexture);
    // D3DFMT_A8L8 lowers to RG8Unorm, which HAS an sRGB Metal sibling, but that
    // sibling gamma-decodes the alpha lane stored in G; native D3D9 and DXVK
    // give A8L8 no sRGB pair (D3D9FormatSuppressSRGBRead), so deny both
    // SRGBREAD and SRGBWRITE for it here just as the sample-bind alias skips it.
    if (metal_fmt == WMTPixelFormatInvalid || Recall_sRGB(metal_fmt) == metal_fmt ||
        D3D9FormatSuppressSRGBRead(CheckFormat))
      return D3DERR_NOTAVAILABLE;
  }

  // D3DUSAGE_QUERY_FILTER asks whether the format can be linearly filtered.
  // Apple-silicon Metal cannot filter the 32-bit-float colour formats, so deny
  // it for them (native ATI / pre-G80 denied it too, and wined3d gates on the
  // per-format FILTERING cap); the sampler translation degrades a LINEAR
  // request over these formats to point anyway. Every other format falls
  // through to the per-type probe below and reports filterable as before.
  if ((Usage & D3DUSAGE_QUERY_FILTER) && IsMetalNonFilterableFormat(CheckFormat))
    return D3DERR_NOTAVAILABLE;

  auto isSamplable2D = [&](D3DFORMAT f) {
    if (isColorRTFormat(f))
      return true;
    switch (f) {
    case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4:
    case D3DFMT_A8:
    case D3DFMT_A8L8:
    case D3DFMT_L8:
    case D3DFMT_L16:
    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
    case D3DFMT_ATI1:
    case D3DFMT_ATI2:
    case D3DFMT_V8U8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16:
    // 4-channel signed-norm bump, mapped to RGBA16Snorm (sampler-only, like the
    // other bump formats); advertise it so the TEXTURE probe agrees with the
    // create path (d9vk / DXVK / wined3d all support it).
    case D3DFMT_Q16W16V16U16:
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
      // slot the app binds but never writes; accepted as RT-capable
      // here so apps gate creation on the standard ENABLE pattern.
      // dxmt drops the slot at render-pass + PSO build time.
      if (IsNullFormat(CheckFormat))
        return D3D_OK;
      return isColorRTFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    // 3Dc (ATI1N/ATI2N) is a texture-only vendor FOURCC: sampleable as a
    // texture but with no offscreen-plain surface form. CreateOffscreenPlainSurface
    // rejects it in every pool but SCRATCH, so deny the surface cap here to keep
    // the advertisement consistent with the create; the texture arm below still
    // grants it. Native advertises the pair for D3DRTYPE_TEXTURE only.
    if (Is3DcFormat(CheckFormat))
      return D3DERR_NOTAVAILABLE;
    return (isSamplable2D(CheckFormat) || isDSFormat(CheckFormat)) ? D3D_OK : D3DERR_NOTAVAILABLE;

  case D3DRTYPE_TEXTURE:
  case D3DRTYPE_CUBETEXTURE:
    if (wantDS) {
      // Depth-stencil CUBE maps: Metal can allocate one and CreateCubeTexture
      // lowers a DS format fine, but neither reference grants the cap (DXVK
      // restricts cube to the color aspect, wined3d strips the DEPTH_STENCIL
      // bind for cubemaps) and 2005-era hardware had no cube depth. Advertising
      // it would send a point-light-shadow app down a Windows-untested path, so
      // the probe denies it. The create path stays permissive (a blind create
      // still works) - a deliberate probe-conservative / create-permissive split.
      if (RType == D3DRTYPE_CUBETEXTURE)
        return D3DERR_NOTAVAILABLE;
      return isDSFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;
    }
    if (wantRT)
      // The 'NULL' FOURCC is RT-capable as a render-target texture too, not
      // just a standalone surface (see the SURFACE branch); apps query it
      // before binding a write-skipped colour slot backed by a texture.
      return (isColorRTFormat(CheckFormat) || IsNullFormat(CheckFormat)) ? D3D_OK : D3DERR_NOTAVAILABLE;
    // Depth formats are sampleable textures, not just depth attachments:
    // wined3d marks every depth format FORMAT_CAP_TEXTURE (ARB_depth_texture),
    // and 2005-era titles create a plain depth texture (D24X8 / D24S8) to read
    // back as a hardware shadow map. dxmt lowers them to a sampleable Metal
    // depth texture, so advertise them texturable here too (not only via the
    // DEPTHSTENCIL probe above), keeping CheckDeviceFormat consistent with the
    // create path.
    if (!isSamplable2D(CheckFormat) && !isDSFormat(CheckFormat))
      return D3DERR_NOTAVAILABLE;
    // Hardware mip generation renders each level down, so it needs a colour-
    // renderable format. dxmt ties autogen-capability to RT-capability: a
    // renderable format autogens (a RENDERTARGET | AUTOGENMIPMAP probe already
    // returned D3D_OK in the wantRT branch above), a non-renderable one is still
    // a valid texture but reports D3DOK_NOAUTOGEN, the success-with-caveat code
    // apps fall back on by building the mips themselves. wined3d gates this on
    // the per-format GEN_MIPMAP cap (wined3d directx.c).
    if (wantAutoGen && !isColorRTFormat(CheckFormat))
      return D3DOK_NOAUTOGEN;
    return D3D_OK;

  case D3DRTYPE_VOLUMETEXTURE:
  case D3DRTYPE_VOLUME:
    // 3D textures: no DS, no RT. The mappable-color set is derived from the
    // same predicate CreateVolumeTexture gates on (IsVolumeTextureFormat), so
    // the probe and the create can never drift; both refs advertise every
    // mapped color format for TEXTURE_3D.
    if (wantDS || wantRT)
      return D3DERR_NOTAVAILABLE;
    return IsVolumeTextureFormat(CheckFormat) ? D3D_OK : D3DERR_NOTAVAILABLE;

  case D3DRTYPE_VERTEXBUFFER:
  case D3DRTYPE_INDEXBUFFER:
    // Buffer resource types are not valid CheckDeviceFormat queries: native
    // returns D3DERR_INVALIDCALL for VERTEXDATA / INDEX16 / INDEX32 regardless
    // of format (wine d3d9 test device.c; DXVK rejects buffer rtypes up front
    // in d3d9_adapter.cpp). wined3d itself gets this wrong (todo_wine), so
    // match native/DXVK, not wined3d.
    return D3DERR_INVALIDCALL;

  default:
    return D3DERR_INVALIDCALL;
  }
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceMultiSampleType(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL, D3DMULTISAMPLE_TYPE MultiSampleType,
    DWORD *pQualityLevels
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // wined3d caps the request at 16; anything beyond is invalid input.
  // wine dlls/d3d9/directx.c.
  if (MultiSampleType > D3DMULTISAMPLE_16_SAMPLES)
    return D3DERR_INVALIDCALL;

  // D3DFMT_UNKNOWN is rejected outright, even for D3DMULTISAMPLE_NONE.
  // wined3d_check_device_multisample_type does this before any type or
  // capability check (wined3d directx.c).
  if (SurfaceFormat == D3DFMT_UNKNOWN)
    return D3DERR_INVALIDCALL;

  // The format must be one the create path can actually back with a
  // Metal texture. Deriving the check from D3DFormatToMetal (the same
  // call CreateRenderTarget / CreateDepthStencilSurface make) keeps the
  // probe from rejecting formats the create path accepts, e.g. G16R16,
  // A2B10G10R10, R32F, G32R32F. DXVK gates on ConvertFormatUnfixed for
  // the same reason.
  auto device = m_adapters.object(Adapter);
  const bool is_ds = IsDepthStencilFormat(SurfaceFormat);
  WMTPixelFormat metal_fmt =
      D3DFormatToMetal(SurfaceFormat, is_ds ? D3D9FormatUsage::DepthStencil : D3D9FormatUsage::RenderTarget);
  const bool format_ok = metal_fmt != WMTPixelFormatInvalid || IsNullFormat(SurfaceFormat);

  HRESULT hr = D3D_OK;
  DWORD quality_levels = 1;
  if (MultiSampleType == D3DMULTISAMPLE_NONE) {
    hr = D3D_OK;
  } else if (!format_ok) {
    hr = D3DERR_NOTAVAILABLE;
  } else if (MultiSampleType == D3DMULTISAMPLE_NONMASKABLE) {
    // NONMASKABLE reports how many quality levels exist; level N selects
    // 1 << N samples at create time (the DXVK GetSampleCount mapping).
    // Count the supported power-of-two sample counts so an app's
    // quality-level loop sees exactly the counts the create path honours.
    // Apple's supportsTextureSampleCount: is the device capability Metal
    // exposes (no per-format sample-count table), matching what
    // newTextureDescriptor with sampleCount=N will allow.
    quality_levels = 1; // level 0 = 1 sample, always available
    for (uint8_t n = 2; n <= 16; n <<= 1) {
      if (device.supportsTextureSampleCount(n))
        ++quality_levels;
      else
        break;
    }
    hr = D3D_OK;
  } else {
    // Masked request: the count is the enum value itself. Probe the
    // device for that exact sample count; M-series GPUs vary in 8x
    // support by family. wined3d issues an equivalent
    // vkGetPhysicalDeviceImageFormatProperties probe.
    hr = device.supportsTextureSampleCount(static_cast<uint8_t>(MultiSampleType)) ? D3D_OK : D3DERR_NOTAVAILABLE;
  }

  // On D3DERR_NOTAVAILABLE the d3d9 runtime still writes a quality count of
  // 1 (wine directx.c: `if (hr == WINED3DERR_NOTAVAILABLE && levels) *levels
  // = 1`). Only D3DERR_INVALIDCALL leaves the out-pointer untouched.
  if (pQualityLevels) {
    if (hr == D3D_OK)
      *pQualityLevels = quality_levels;
    else if (hr == D3DERR_NOTAVAILABLE)
      *pQualityLevels = 1;
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDepthStencilMatch(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat,
    D3DFORMAT DepthStencilFormat
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;
  if (AdapterFormat != D3DFMT_X8R8G8B8 && AdapterFormat != D3DFMT_R5G6B5 && AdapterFormat != D3DFMT_X1R5G5B5)
    return D3DERR_NOTAVAILABLE;

  // Shares CheckDeviceFormat's format predicates so the two probes cannot
  // diverge; apps query one then the other and expect consistent answers.
  // wined3d does real bit-depth pairing; on Metal the depth/stencil
  // attachment is independent of the colour attachment, so any legal
  // (RT, DS) pair is compatible.
  // A 'NULL' colour target pairs with any real depth/stencil: a depth-only
  // pass binds NULL as the colour slot and a genuine DS attachment.
  return ((isColorRTFormat(RenderTargetFormat) || IsNullFormat(RenderTargetFormat)) && isDSFormat(DepthStencilFormat))
             ? D3D_OK
             : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::CheckDeviceFormatConversion(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat
) {
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL)
    return D3DERR_NOTAVAILABLE;

  // wined3d short-circuits the same-format case before consulting its
  // table. wine dlls/d3d9/directx.c.
  if (SourceFormat == TargetFormat)
    return D3D_OK;

  // Models wined3d_check_device_format_conversion (wined3d directx.c): the
  // source must be a blittable non-depth format and the destination a simple
  // RGB format with a real red channel. Packed YUV is a valid source (StretchRect
  // YUV->RGB, which real hardware performs though WARP does not); the float-color
  // formats carry no fixed-function blit cap and are rejected as a source, while
  // remaining valid RGB destinations. isColorRTFormat is exactly the simple-RGB
  // set for the destination: it excludes luminance, YUV, A8 and the compressed
  // formats.
  const bool src_blittable = SourceFormat == D3DFMT_YUY2 || SourceFormat == D3DFMT_UYVY ||
                             (isColorRTFormat(SourceFormat) && !IsFloatColorFormat(SourceFormat));
  if (!src_blittable || !isColorRTFormat(TargetFormat))
    return D3DERR_NOTAVAILABLE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) {
  if (!pCaps)
    return D3DERR_INVALIDCALL;

  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;

  std::memset(pCaps, 0, sizeof(*pCaps));

  // wined3d_caps_from_wined3dcaps masks each field down to the bits
  // D3D9 actually defines. We start from those masks directly; they
  // describe a hardware-T&L SM 3.0 GPU, which is what most D3D9 games
  // expect. wine dlls/d3d9/device.c d3d9_caps_from_wined3dcaps.
  pCaps->DeviceType = DeviceType;
  pCaps->AdapterOrdinal = Adapter;

  pCaps->Caps = D3DCAPS_READ_SCANLINE;
  // D3DCAPS2_CANCALIBRATEGAMMA is deliberately omitted (as wined3d and DXVK do):
  // the SetGammaRamp D3DSGR_CALIBRATE flag is ignored, so advertising it would
  // be a caps lie. D3DCAPS2_CANMANAGERESOURCE and D3DCAPS2_RESERVED are dropped
  // for the same reason: neither reference raises them, and CANMANAGERESOURCE
  // claims driver-side management of the MANAGED pool while dxmt mirrors MANAGED
  // resources in the runtime (the DXVK model), so it is not earned.
  pCaps->Caps2 = D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_DYNAMICTEXTURES | D3DCAPS2_CANAUTOGENMIPMAP;
  // D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION is not advertised: the Present path
  // consumes only D3DPRESENTFLAG_LOCKABLE_BACKBUFFER and never
  // D3DPRESENTFLAG_LINEAR_CONTENT, so a title that renders linear content and
  // relies on a present-time linear->sRGB encode would present washed out.
  // wined3d never sets the bit. The honest upgrade is an sRGB drawable view at
  // the Present blit when LINEAR_CONTENT is set; deferred.
  pCaps->Caps3 =
      D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD | D3DCAPS3_COPY_TO_VIDMEM | D3DCAPS3_COPY_TO_SYSTEMMEM;
  // Per DXVK d3d9_adapter.cpp; claim the full set the Present
  // path honours. swapchain.cpp already maps INTERVAL_TWO/
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
                             D3DPMISCCAPS_CLIPTLVERTS | D3DPMISCCAPS_TSSARGTEMP | D3DPMISCCAPS_BLENDOP |
                             D3DPMISCCAPS_INDEPENDENTWRITEMASKS | D3DPMISCCAPS_SEPARATEALPHABLEND |
                             D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS | D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING |
                             D3DPMISCCAPS_FOGVERTEXCLAMPED | D3DPMISCCAPS_FOGANDSPECULARALPHA |
                             D3DPMISCCAPS_POSTBLENDSRGBCONVERT | D3DPMISCCAPS_PERSTAGECONSTANT;
  // CLIPTLVERTS: the generated fixed-function VS clips pre-transformed
  // (POSITIONT) draws against the user clip planes, so advertising the
  // "clips post-transform vertices" cap is honest. Both refs set it.
  // PERSTAGECONSTANT: the fixed-function combiner reads D3DTSS_CONSTANT
  // (D3DTA_CONSTANT) per stage, so the cap must be advertised or a
  // cap-checking app skips per-stage constants and falls back to TFACTOR.
  // wined3d and DXVK both advertise it.
  // FOGANDSPECULARALPHA: the fog factor rides its own FOG0 varying (VS oFog),
  // separate from the specular-alpha channel, in both the FFP generator and the
  // DXSO path, so fog and specular alpha never share storage. DXVK advertises
  // it; wined3d leaves it inside a TODO alongside FOGVERTEXCLAMPED.

  // Both WFOG and ZFOG are honored: table fog picks its coordinate from the
  // projection like wined3d (orthographic projection, 4th column 0,0,0,1 ->
  // device z; otherwise eye w), which is stronger than DXVK's single
  // z-shaped path. FOGRANGE is honored too (D3DRS_RANGEFOGENABLE drives the
  // FFP vertex-fog radial-distance path), so this whole mask is now backed.
  pCaps->RasterCaps = D3DPRASTERCAPS_DITHER | D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX |
                      D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_FOGRANGE |
                      D3DPRASTERCAPS_ANISOTROPY | D3DPRASTERCAPS_WFOG | D3DPRASTERCAPS_ZFOG |
                      D3DPRASTERCAPS_COLORPERSPECTIVE | D3DPRASTERCAPS_SCISSORTEST |
                      D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS | D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_MULTISAMPLE_TOGGLE;

  // Compare ops, blend factors, alpha-test ops; claim the full set.
  pCaps->ZCmpCaps = D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESS | D3DPCMPCAPS_EQUAL | D3DPCMPCAPS_LESSEQUAL |
                    D3DPCMPCAPS_GREATER | D3DPCMPCAPS_NOTEQUAL | D3DPCMPCAPS_GREATEREQUAL | D3DPCMPCAPS_ALWAYS;
  pCaps->AlphaCmpCaps = pCaps->ZCmpCaps;
  pCaps->SrcBlendCaps = D3DPBLENDCAPS_ZERO | D3DPBLENDCAPS_ONE | D3DPBLENDCAPS_SRCCOLOR | D3DPBLENDCAPS_INVSRCCOLOR |
                        D3DPBLENDCAPS_SRCALPHA | D3DPBLENDCAPS_INVSRCALPHA | D3DPBLENDCAPS_DESTALPHA |
                        D3DPBLENDCAPS_INVDESTALPHA | D3DPBLENDCAPS_DESTCOLOR | D3DPBLENDCAPS_INVDESTCOLOR |
                        D3DPBLENDCAPS_SRCALPHASAT | D3DPBLENDCAPS_BOTHSRCALPHA | D3DPBLENDCAPS_BOTHINVSRCALPHA |
                        D3DPBLENDCAPS_BLENDFACTOR;
  // Dual-source factors are a 9Ex-only advertisement; non-Ex devices
  // never see SRCCOLOR2/INVSRCCOLOR2. d3d9_adapter.cpp gates them on
  // IsExtended() and so do we.
  if (m_isEx)
    pCaps->SrcBlendCaps |= D3DPBLENDCAPS_SRCCOLOR2 | D3DPBLENDCAPS_INVSRCCOLOR2;
  pCaps->DestBlendCaps = pCaps->SrcBlendCaps;

  pCaps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                     D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;

  // D3DPTEXTURECAPS_NOPROJECTEDBUMPENV is not advertised: that restriction bit
  // claims projected bump-env lookups are unsupported, but the per-stage
  // PROJECTED divide runs before every stage's sample including the bump-env
  // stages, so the restriction is not real. Neither reference sets it.
  pCaps->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_ALPHAPALETTE |
                       D3DPTEXTURECAPS_TEXREPEATNOTSCALEDBYSIZE | D3DPTEXTURECAPS_PROJECTED |
                       D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_VOLUMEMAP | D3DPTEXTURECAPS_MIPMAP |
                       D3DPTEXTURECAPS_MIPVOLUMEMAP | D3DPTEXTURECAPS_MIPCUBEMAP;

  // Regular texture filter caps: point/linear/anisotropic min+mag, point/linear
  // mip. This mask mirrors DXVK's d3d9_adapter set (wined3d omits the aniso bits
  // from its own TextureFilterCaps).
  const DWORD textureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR |
                                  D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MIPFPOINT |
                                  D3DPTFILTERCAPS_MIPFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR |
                                  D3DPTFILTERCAPS_MAGFANISOTROPIC;
  pCaps->TextureFilterCaps = textureFilterCaps;
  pCaps->CubeTextureFilterCaps = textureFilterCaps;
  pCaps->VolumeTextureFilterCaps = textureFilterCaps;
  // Classic D3D9 vertex texture fetch is point-sampled only in wined3d, which
  // reports just point min/mag here (directx.c SM3 arm); DXVK actually adds the
  // LINEAR bits (d3d9_adapter.cpp). Follow wined3d, the primary reference: a
  // filtering vertex sampler is a Metal capability the SM3-era reference
  // hardware lacked. Advertising less than we honor is the safe direction (the
  // VTF sample path filters if asked, like both refs).
  pCaps->VertexTextureFilterCaps = D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MAGFPOINT;
  // StretchRect uses Metal's blit encoder which supports only point and
  // linear filtering; anisotropic is sampler-only. DXVK d3d9_adapter.cpp
  // reports only POINT|LINEAR. Pre-port dxmt reused the full
  // textureFilterCaps mask, so apps that requested D3DTEXF_ANISOTROPIC
  // on StretchRect silently degraded to LINEAR.
  pCaps->StretchRectFilterCaps =
      D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT | D3DPTFILTERCAPS_MAGFLINEAR;

  pCaps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                              D3DPTADDRESSCAPS_BORDER | D3DPTADDRESSCAPS_INDEPENDENTUV | D3DPTADDRESSCAPS_MIRRORONCE;
  pCaps->VolumeTextureAddressCaps = pCaps->TextureAddressCaps;

  // No D3DLINECAPS_ANTIALIAS: Metal has no line smoothing and
  // D3DRS_ANTIALIASEDLINEENABLE is inert, so the bit would be a lie. Native
  // Windows drivers do not set it either (wined3d directx.c).
  pCaps->LineCaps =
      D3DLINECAPS_TEXTURE | D3DLINECAPS_ZTEST | D3DLINECAPS_BLEND | D3DLINECAPS_ALPHACMP | D3DLINECAPS_FOG;

  pCaps->StencilCaps = D3DSTENCILCAPS_KEEP | D3DSTENCILCAPS_ZERO | D3DSTENCILCAPS_REPLACE | D3DSTENCILCAPS_INCRSAT |
                       D3DSTENCILCAPS_DECRSAT | D3DSTENCILCAPS_INVERT | D3DSTENCILCAPS_INCR | D3DSTENCILCAPS_DECR |
                       D3DSTENCILCAPS_TWOSIDED;

  pCaps->FVFCaps = 8 | D3DFVFCAPS_PSIZE; // 8 = max texture coord count

  // Every advertised op has a real combiner arm in ffp_compile.cpp. The two
  // MODULATEINV* ops are the exact MSDN formulas (arg1 + (1-arg1.a)*arg2 and
  // (1-arg1)*arg2 + arg1.a), and both refs advertise them, so hiding them would
  // send a cap-checking combiner engine down a needless fallback.
  // D3DTEXOPCAPS_PREMODULATE is NOT advertised: the combiner no-ops op 17 (a DX6
  // relic with no well-defined semantics anywhere), and wined3d neither
  // advertises nor implements it; advertising an inert op would be a caps lie.
  pCaps->TextureOpCaps =
      D3DTEXOPCAPS_DISABLE | D3DTEXOPCAPS_SELECTARG1 | D3DTEXOPCAPS_SELECTARG2 | D3DTEXOPCAPS_MODULATE |
      D3DTEXOPCAPS_MODULATE2X | D3DTEXOPCAPS_MODULATE4X | D3DTEXOPCAPS_ADD | D3DTEXOPCAPS_ADDSIGNED |
      D3DTEXOPCAPS_ADDSIGNED2X | D3DTEXOPCAPS_SUBTRACT | D3DTEXOPCAPS_ADDSMOOTH | D3DTEXOPCAPS_BLENDDIFFUSEALPHA |
      D3DTEXOPCAPS_BLENDTEXTUREALPHA | D3DTEXOPCAPS_BLENDFACTORALPHA | D3DTEXOPCAPS_BLENDTEXTUREALPHAPM |
      D3DTEXOPCAPS_BLENDCURRENTALPHA | D3DTEXOPCAPS_MODULATEALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATECOLOR_ADDALPHA |
      D3DTEXOPCAPS_MODULATEINVALPHA_ADDCOLOR | D3DTEXOPCAPS_MODULATEINVCOLOR_ADDALPHA | D3DTEXOPCAPS_DOTPRODUCT3 |
      D3DTEXOPCAPS_MULTIPLYADD | D3DTEXOPCAPS_LERP | D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE;
  pCaps->MaxTextureBlendStages = 8;
  pCaps->MaxSimultaneousTextures = 8;

  // No D3DVTXPCAPS_TWEENING: the generated vertex pipe treats
  // D3DVBF_TWEENING as disabled, and wined3d leaves the bit clear too.
  // No D3DVTXPCAPS_NO_TEXGEN_NONLOCALVIEWER: that restriction bit claims texgen
  // breaks when D3DRS_LOCALVIEWER is FALSE, but texgen is shader-generated and
  // independent of the render state (LOCALVIEWER only steers the specular
  // half-vector), so the restriction is not real. Neither reference sets it.
  pCaps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_MATERIALSOURCE7 | D3DVTXPCAPS_DIRECTIONALLIGHTS |
                                D3DVTXPCAPS_POSITIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER | D3DVTXPCAPS_TEXGEN_SPHEREMAP;
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
  // Apple Silicon max point size 511.0 per Metal Feature Set Tables.
  // Vulkan pointSizeRange[1] equivalent.
  pCaps->MaxPointSize = 511.0f;
  // wined3d and DXVK both report this (taken from an AMD Evergreen GPU).
  pCaps->MaxPrimitiveCount = 0x00555555;
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
  if (Adapter >= m_adapterCount)
    return nullptr;
  return wsi::enumMonitors(Adapter);
}

// All D3DPRESENTFLAG_* bits the D3D9 runtime knows about. Anything
// outside this mask is a higher-version or vendor extension we don't
// understand. Mirrors dlls/d3d9/d3d9_private.h D3DPRESENTFLAGS_MASK.
static constexpr DWORD kD3DPresentFlagsMask = 0x00000fffu;

// ValidatePresentParams (the pure accept/reject matrix mirroring
// wined3d_swapchain_desc_from_d3d9) moved to d3d9_present_validation.hpp so the
// host unit tier can pin it; it is in scope through d3d9_interface.hpp.

// A fullscreen (Windowed=FALSE) present must name a mode the adapter actually
// enumerates. dxmt has no real display-mode switch on macOS (wined3d and DXVK
// let the OS ChangeDisplaySettings reject a bogus mode); validate against the
// same wsi mode list GetAdapterModeCount / EnumAdapterModes walk so the
// enumeration and the gate can never disagree. D3D9 advertises only X8R8G8B8
// modes, so the width/height comparison spans the enumerable set; the refresh
// rate is matched only when the app pins a nonzero FullScreen_RefreshRateInHz.
static bool
fullscreenModeSupported(UINT adapter, UINT width, UINT height, UINT refreshRate) {
  HMONITOR mon = wsi::enumMonitors(adapter);
  if (!mon)
    return false;
  wsi::WsiMode wm{};
  for (UINT i = 0; wsi::getDisplayMode(mon, i, &wm); ++i) {
    if (wm.width != width || wm.height != height)
      continue;
    if (refreshRate != 0 && refreshRateHzOr60(wm) != refreshRate)
      continue;
    return true;
  }
  return false;
}

// Resolve spec "use runtime default" placeholders in params; return false on
// invalid format, zero extent on a hidden window, or a fullscreen extent that
// is not an enumerable adapter mode. hwndFallback is hFocusWindow; spec:
// hDeviceWindow takes precedence (wined3d swapchain.c).
bool
CanonicalisePresentParams(D3DPRESENT_PARAMETERS &p, HWND hwndFallback, UINT adapter) {
  if (p.BackBufferCount == 0)
    p.BackBufferCount = 1;
  if (p.BackBufferFormat == D3DFMT_UNKNOWN)
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
  if (D3DFormatToMetal(p.BackBufferFormat, D3D9FormatUsage::RenderTarget) == WMTPixelFormatInvalid)
    return false;
  // EnableAutoDepthStencil=TRUE with AutoDepthStencilFormat=UNKNOWN is
  // a spec-permitted "use the runtime default" placeholder per MSDN
  // CreateDevice; wined3d swapchain.c substitutes D24S8. Without
  // this, the auto-DS allocation path at d3d9_device.cpp::createAutoDS
  // would see UNKNOWN, call D3DFormatToMetal → Invalid, and skip
  // allocating the implicit DS; leaving EnableAutoDepthStencil=TRUE
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
  // Fullscreen present: the extent must be a real adapter display mode; native
  // fails the fullscreen create / reset otherwise. Windowed present composites
  // into a window and accepts any size.
  if (!p.Windowed &&
      !fullscreenModeSupported(adapter, p.BackBufferWidth, p.BackBufferHeight, p.FullScreen_RefreshRateInHz))
    return false;
  return true;
}

// Log unhandled D3DPRESENTFLAG_* bits the same way wined3d FIXMEs them;
// so contributors adding handling for LOCKABLE_BACKBUFFER, VIDEO,
// NOAUTOROTATE, etc. have an obvious entry point. Reference:
// dlls/d3d9/device.c.
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
  if (!ppReturnedDeviceInterface)
    return D3DERR_INVALIDCALL;
  *ppReturnedDeviceInterface = nullptr;
  if (!pPresentationParameters)
    return D3DERR_INVALIDCALL;

  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL && DeviceType != D3DDEVTYPE_REF && DeviceType != D3DDEVTYPE_SW)
    return D3DERR_INVALIDCALL;
  // A device's extended-ness follows the parent interface, not the Create
  // method: IDirect3D9Ex::CreateDevice makes an extended device (DXVK routes
  // it straight through CreateDeviceEx), so it validates with the extended
  // backbuffer-count / swap-effect limits and QIs to IDirect3DDevice9Ex. A
  // plain IDirect3D9 stays non-extended.
  if (!ValidatePresentParams(*pPresentationParameters, /*isEx=*/m_isEx))
    return D3DERR_INVALIDCALL;

  WarnUnhandledPresentFlags(pPresentationParameters->Flags);

  // Canonicalise the placeholders (zero extent, UNKNOWN format, zero
  // backbuffer count) in place. D3D9 writes the realized values back into the
  // caller's struct, the same as Reset and CreateAdditionalSwapChain, and that
  // is what apps read after CreateDevice.
  if (!CanonicalisePresentParams(*pPresentationParameters, hFocusWindow, Adapter))
    return D3DERR_INVALIDCALL;

  WMT::Reference<WMT::Device> metalDevice = m_adapters.object(Adapter);
  if (!metalDevice.handle)
    return D3DERR_OUTOFVIDEOMEMORY;

  auto *device = new MTLD3D9Device(
      this, /*isEx=*/m_isEx, Adapter, DeviceType, hFocusWindow, BehaviorFlags, *pPresentationParameters,
      std::move(metalDevice)
  );
  device->AddRef();
  *ppReturnedDeviceInterface = static_cast<IDirect3DDevice9 *>(device);
  return D3D_OK;
}

UINT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterModeCountEx(UINT Adapter, const D3DDISPLAYMODEFILTER *pFilter) {
  if (!pFilter || Adapter >= m_adapterCount)
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
  if (!pFilter || !pMode || Adapter >= m_adapterCount)
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
  pMode->RefreshRate = refreshRateHzOr60(wm);
  pMode->Format = D3DFMT_X8R8G8B8;
  pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterDisplayModeEx(UINT Adapter, D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  if (!pMode || Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (pMode->Size != sizeof(*pMode))
    return D3DERR_INVALIDCALL;

  HMONITOR mon = wsi::enumMonitors(Adapter);
  wsi::WsiMode wm{};
  if (!mon || !wsi::getCurrentDisplayMode(mon, &wm))
    return D3DERR_INVALIDCALL;

  pMode->Width = wm.width;
  pMode->Height = wm.height;
  pMode->RefreshRate = refreshRateHzOr60(wm);
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

  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  if (DeviceType != D3DDEVTYPE_HAL && DeviceType != D3DDEVTYPE_REF && DeviceType != D3DDEVTYPE_SW)
    return D3DERR_INVALIDCALL;
  if (!ValidatePresentParams(*pPresentationParameters, /*isEx=*/true))
    return D3DERR_INVALIDCALL;

  WarnUnhandledPresentFlags(pPresentationParameters->Flags);

  // Canonicalise in place so the caller reads back the realized extent /
  // format / count, matching CreateDevice and the Reset path.
  if (!CanonicalisePresentParams(*pPresentationParameters, hFocusWindow, Adapter))
    return D3DERR_INVALIDCALL;

  WMT::Reference<WMT::Device> metalDevice = m_adapters.object(Adapter);
  if (!metalDevice.handle)
    return D3DERR_OUTOFVIDEOMEMORY;

  auto *device = new MTLD3D9Device(
      this, /*isEx=*/true, Adapter, DeviceType, hFocusWindow, BehaviorFlags, *pPresentationParameters,
      std::move(metalDevice)
  );
  device->AddRef();
  *ppDevice = static_cast<IDirect3DDevice9Ex *>(device);
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Interface::GetAdapterLUID(UINT Adapter, LUID *pLUID) {
  if (!pLUID)
    return D3DERR_INVALIDCALL;
  if (Adapter >= m_adapterCount)
    return D3DERR_INVALIDCALL;
  uint64_t id = m_adapters.object(Adapter).registryID();
  pLUID->LowPart = static_cast<DWORD>(id & 0xFFFFFFFFu);
  pLUID->HighPart = static_cast<LONG>(id >> 32);
  return D3D_OK;
}

} // namespace dxmt
