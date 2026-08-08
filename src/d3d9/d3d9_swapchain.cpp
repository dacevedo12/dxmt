#include "d3d9_swapchain.hpp"
#include <version.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include "log/log.hpp"
#include "util_string.hpp"

#include "d3d9_debug.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_gamma.hpp"
#include "d3d9_interface.hpp"
#include "d3d9_surface.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "util_env.hpp"
#include "wsi_monitor.hpp"
#include "wsi_platform.hpp"
#include "wsi_window.hpp"

// D3DPRESENT_FORCEIMMEDIATE is a spec-defined Present dwFlags bit
// (value 0x100) introduced with D3D9Ex. Neither mingw's bundled
// d3d9.h nor wine's older d3d9.h define it; the dxmt-native d3d9.h
// has it but isn't used in cross-build. Shim the missing macro.
#ifndef D3DPRESENT_FORCEIMMEDIATE
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100
#endif

// S_PRESENT_OCCLUDED / S_PRESENT_MODE_CHANGED are D3D9Ex success-status
// codes (MAKE_D3DSTATUS(2168) / 2169). mingw's bundled d3d9.h omits
// them; the dxmt-native d3d9.h defines them but isn't visible in the
// cross-build. Shim for build parity: values match wine's d3d9.h.
#ifndef S_PRESENT_OCCLUDED
#define S_PRESENT_OCCLUDED ((HRESULT)0x08760878L)
#endif
#ifndef S_PRESENT_MODE_CHANGED
#define S_PRESENT_MODE_CHANGED ((HRESULT)0x08760877L)
#endif

namespace dxmt {

// Releases a Metal view once the chunk it was registered on retires. The view
// owns the CAMetalLayer a present calls nextDrawable on from the encode thread,
// and Metal does not retain the layer for un-encoded work, so the release has
// to wait for real GPU completion rather than the app-thread Release. Carries
// its own autorelease pool: the finish thread has no outer one.
class D9DeferredViewRelease final : public GpuCompletionTarget {
public:
  explicit D9DeferredViewRelease(WMT::Object view) : view_(view) {}

  void
  CompleteGpuWork(GpuCompletionStatus) noexcept final {
    release();
  }

  ~D9DeferredViewRelease() noexcept final {
    // The queue can drop a target without completing it during teardown; the
    // view still has to go back.
    release();
  }

private:
  void
  release() noexcept {
    if (view_.handle == 0)
      return;
    auto pool = WMT::MakeAutoreleasePool();
    WMT::ReleaseMetalView(view_);
    view_ = {};
  }

  WMT::Object view_;
};

// A lockable backbuffer (D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) is given a host
// mirror like a DEFAULT offscreen-plain surface: LockRect/GetDC read it back
// from the Metal texture and upload on unlock. Non-lockable backbuffers stay
// GPU-only (cpuPtr null, LockRect/GetDC correctly fail).
static void
allocLockableBackBufferMirror(
    bool lockable, const D3DSURFACE_DESC &desc, void *&cpuPtr, uint32_t &pitch, void *&ownedBacking
) {
  cpuPtr = nullptr;
  ownedBacking = nullptr;
  pitch = 0;
  if (!lockable)
    return;
  const uint32_t p = D3DFormatRowPitch(desc.Format, desc.Width);
  if (p == 0)
    return;
  const uint64_t bytes = static_cast<uint64_t>(p) * D3DFormatRowCount(desc.Format, desc.Height);
  void *mirror = wsi::aligned_malloc(bytes, DXMT_PAGE_SIZE);
  if (!mirror)
    return;
  std::memset(mirror, 0, bytes);
  cpuPtr = mirror;
  ownedBacking = mirror;
  pitch = p;
}

// The whole swap configuration, reported wherever the chain is built. Both the
// ctor path and the reset path call this, so a windowed<->fullscreen flip emits
// a line per side and the two can be diffed. Worth carrying because
// BackBufferCount and SwapEffect select behaviour rather than just extent
// (together they arm the content rotation in Present), and an app may request a
// different pair per mode, which makes "it only misbehaves fullscreen" a
// statement about the parameters rather than about the mode.
static void
logSwapParams(const D3DPRESENT_PARAMETERS &params, const char *when) {
  if (!d9PresentDbgEnabled())
    return;
  const UINT count = std::max<UINT>(1u, params.BackBufferCount);
  Logger::warn(
      str::format(
          "d9 swap params (", when, "): ", params.BackBufferWidth, "x", params.BackBufferHeight,
          " fmt=", static_cast<uint32_t>(params.BackBufferFormat), " count=", params.BackBufferCount,
          " swapEffect=", static_cast<uint32_t>(params.SwapEffect), " windowed=", params.Windowed ? 1 : 0,
          " presentInterval=", static_cast<uint32_t>(params.PresentationInterval),
          " flags=", static_cast<uint32_t>(params.Flags), " msaaType=", static_cast<uint32_t>(params.MultiSampleType),
          " msaaQuality=", params.MultiSampleQuality, " refreshHz=", params.FullScreen_RefreshRateInHz,
          " rotation=", (count > 1 && params.SwapEffect != D3DSWAPEFFECT_COPY) ? "on" : "off"
      )
  );
}

// Backbuffer texture descriptor + D3DSURFACE_DESC derived from a
// PRESENT_PARAMETERS. Used identically by buildBackBuffer (ctor
// path) and ResetForDeviceReset (resolution-change path); pull into
// one place so the two stay in lockstep; a future addition like
// MSAA backbuffers (sample_count from params.MultiSampleType) lands
// in one shape, not two.
static void
backBufferDescriptors(
    const D3DPRESENT_PARAMETERS &params, uint32_t sampleCount, WMTTextureInfo &info, D3DSURFACE_DESC &desc
) {
  info = {};
  info.pixel_format = D3DFormatToMetal(params.BackBufferFormat, D3D9FormatUsage::RenderTarget);
  info.width = params.BackBufferWidth;
  info.height = params.BackBufferHeight;
  info.depth = 1;
  info.array_length = 1;
  info.type = sampleCount > 1 ? WMTTextureType2DMultisample : WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = sampleCount;
  info.usage = static_cast<WMTTextureUsage>(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  // PixelFormatView for D3DRS_SRGBWRITEENABLE: the back buffer is the
  // most common RT and games often toggle sRGB-write per draw on it.
  if (Recall_sRGB(info.pixel_format) != info.pixel_format)
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
  info.options = WMTResourceStorageModePrivate;

  desc = {};
  desc.Format = params.BackBufferFormat;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  // GetDesc reads back the realized MSAA mode; a single-sample chain (or a
  // fallback to 1 on an unsupported request) reads back as NONE. The MSAA
  // backbuffer is the app's render target; Present resolves it to a
  // single-sample drawable (see m_resolveTarget).
  desc.MultiSampleType = sampleCount > 1 ? params.MultiSampleType : D3DMULTISAMPLE_NONE;
  desc.MultiSampleQuality = sampleCount > 1 ? params.MultiSampleQuality : 0;
  desc.Width = params.BackBufferWidth;
  desc.Height = params.BackBufferHeight;
}

// Single-sample present source for an MSAA backbuffer (DXVK shape): same
// format/extent/usage as the MSAA backbuffer but one sample, so Present can
// resolve into it and blit it to the single-sample drawable. Null on failure.
// Layer colorspace for the chain's backbuffer format: float16
// backbuffers carry linear (scRGB-shaped) values, everything else is
// presented as sRGB. d3d9 has no colorspace API, so the format is the
// only signal (DXVK keys its pass-through colorspace the same way).
static WMTColorSpace
layerColorSpace(D3DFORMAT backbuffer_format) {
  return backbuffer_format == D3DFMT_A16B16G16R16F ? WMTColorSpaceSRGBLinear : WMTColorSpaceSRGB;
}

// A GPU-private (CpuInvisible) texture allocated exactly as `info` describes,
// including its sample count and type. buildResolveTarget forces single sample
// (it feeds the single-sample drawable); the rotation scratch keeps the
// backbuffers' sample count so a matched-count MSAA copy round-trips.
static Rc<dxmt::Texture>
buildPrivateTarget(MTLD3D9Device *device, const WMTTextureInfo &info) {
  Rc<dxmt::Texture> tex = new dxmt::Texture(info, device->metalDevice());
  dxmt::Flags<dxmt::TextureAllocationFlag> flags;
  flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
  auto allocation = tex->allocate(flags);
  if (!allocation || !allocation->texture())
    return nullptr;
  tex->rename(std::move(allocation));
  return tex;
}

static Rc<dxmt::Texture>
buildResolveTarget(MTLD3D9Device *device, const WMTTextureInfo &bbInfo) {
  WMTTextureInfo info = bbInfo;
  info.type = WMTTextureType2D;
  info.sample_count = 1;
  return buildPrivateTarget(device, info);
}

// Drawable extent = live window client size (not stale layer drawable).
// d3d9 sizes off the window (not backbuffer like d3d11) to avoid upscaling
// during fullscreen<->windowed transitions. Per-Present re-probe converges on
// correct drawable once NSView resize settles.
static void
layerDrawableExtent(
    HWND hWindow, const WMTLayerProps &current, const D3DPRESENT_PARAMETERS &params, double &out_w, double &out_h
) {
  uint32_t win_w = 0, win_h = 0;
  if (hWindow)
    wsi::getWindowSize(hWindow, &win_w, &win_h);
  out_w = win_w > 0 ? (double)win_w * current.contents_scale : (double)params.BackBufferWidth;
  out_h = win_h > 0 ? (double)win_h * current.contents_scale : (double)params.BackBufferHeight;

  // Deduped to one line per distinct extent decision. This runs per Present, so
  // the dedupe is what makes it affordable, and it is also what makes it worth
  // reading: a stable configuration emits a line per mode change, while an
  // extent that disagrees with itself from frame to frame emits a flood. The
  // difference matters because the drawable extent decides both which present
  // pipeline runs (1:1 blit or scaled) and, when it moves, whether the layer
  // rebuilds its drawable pool underneath presents already in flight.
  if (d9PresentDbgEnabled()) {
    static uint32_t last_win_w = 0, last_win_h = 0;
    static uint64_t last_out_w = 0, last_out_h = 0;
    static uint32_t last_bb_w = 0, last_bb_h = 0;
    static uint64_t repeats = 0;
    if (win_w != last_win_w || win_h != last_win_h || (uint64_t)out_w != last_out_w || (uint64_t)out_h != last_out_h ||
        params.BackBufferWidth != last_bb_w || params.BackBufferHeight != last_bb_h) {
      Logger::warn(
          str::format(
              "d9 layer extent: backbuffer ", params.BackBufferWidth, "x", params.BackBufferHeight, " window-client ",
              win_w, "x", win_h, " pinned-drawable ", (uint64_t)current.drawable_width, "x",
              (uint64_t)current.drawable_height, " contents_scale ", current.contents_scale, " -> drawable ",
              (uint64_t)out_w, "x", (uint64_t)out_h, " present=",
              ((uint64_t)out_w == params.BackBufferWidth && (uint64_t)out_h == params.BackBufferHeight) ? "blit"
                                                                                                        : "scale",
              " afterPresents=", repeats
          )
      );
      last_win_w = win_w, last_win_h = win_h;
      last_out_w = (uint64_t)out_w, last_out_h = (uint64_t)out_h;
      last_bb_w = params.BackBufferWidth, last_bb_h = params.BackBufferHeight;
      repeats = 0;
    } else {
      ++repeats;
    }
  }
}

// The monitor a window currently lives on, or null (headless / non-Win32).
static HMONITOR
windowMonitor(HWND hWindow) {
#ifdef _WIN32
  return hWindow ? MonitorFromWindow(hWindow, MONITOR_DEFAULTTOPRIMARY) : nullptr;
#else
  (void)hWindow;
  return nullptr;
#endif
}

// Refresh rate of a monitor, written into *rate_hz and left untouched on
// failure so the caller keeps its prior value (d3d11_swapchain.cpp's
// shape). d3d9 does no display-mode switch; present is always to a
// CAMetalLayer on the host NSView (see ResetEx); so the rate only changes
// when the window is dragged to a different-refresh-rate monitor. The
// per-Present re-probe in Present is the refresh-rate analogue of the
// drawable-extent resize poll a few lines below it.
static void
queryMonitorRefreshRate(HMONITOR mon, double *rate_hz) {
#ifdef _WIN32
  wsi::WsiMode mode{};
  if (mon && wsi::getCurrentDisplayMode(mon, &mode) && mode.refreshRate.denominator != 0 &&
      mode.refreshRate.numerator != 0)
    *rate_hz = static_cast<double>(mode.refreshRate.numerator) / static_cast<double>(mode.refreshRate.denominator);
#else
  (void)mon;
  (void)rate_hz;
#endif
}

MTLD3D9SwapChain::MTLD3D9SwapChain(
    MTLD3D9Device *device, bool isEx, const D3DPRESENT_PARAMETERS &params, HWND hEffectiveWindow, bool isImplicit
) :
    m_device(device),
    m_isEx(isEx),
    m_isImplicit(isImplicit),
    m_params(params) {
  // GetPresentParameters reports the resolved device window: the app's
  // hDeviceWindow when non-null, otherwise the device's focus window. Store
  // the effective window so the reported params match what D3D9 substitutes
  // (wined3d resolves the swapchain's device_window the same way).
  m_params.hDeviceWindow = hEffectiveWindow;
  // CanonicalisePresentParams in d3d9_interface.cpp guarantees the
  // params are concrete and the format lowers to a valid MTLPixelFormat.
  if (!buildBackBuffer()) {
    // Backbuffer allocation failure leaves m_backBuffer null; the
    // chain's GetBackBuffer paths return INVALIDCALL, present is a
    // no-op flush: same outcome as the prior null-texture branch.
    return;
  }

  createPresentTarget(hEffectiveWindow);
}

void
MTLD3D9SwapChain::createPresentTarget(HWND hEffectiveWindow) {
  // CAMetalLayer setup. CreateMetalViewFromHWND returns null when the
  // HWND can't be resolved to an NSView (null HWND, off-screen-only
  // HWND, etc.); in that case the chain stays in headless mode and
  // Present becomes a no-op flush. Real apps always reach this with a
  // valid window.
  m_hWindow = hEffectiveWindow;
  if (hEffectiveWindow != nullptr) {
    // Display refresh rate the window currently lives on (d3d11_swapchain.cpp
    // :191's shape). On failure we keep the 60.0 default; wrong-by-a-frame at
    // 120Hz beats wrong-by-N-frames for apps that request INTERVAL_TWO/THREE/
    // FOUR. m_lastMonitor seeds the per-Present re-probe in Present.
    m_lastMonitor = windowMonitor(hEffectiveWindow);
    queryMonitorRefreshRate(m_lastMonitor, &m_refreshRateHz);
    m_view =
        WMT::CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(hEffectiveWindow), m_device->metalDevice(), m_layer);
    if (m_layer.handle != 0) {
      // The Presenter's ctor reads the layer's current props (contents_scale,
      // framebuffer_only) and stamps in the device handle, opaque, and
      // framebuffer_only=false (the present-blit needs the drawable as a blit
      // destination; framebuffer_only=true would silently no-op in release and
      // assert under MTL_DEBUG_LAYER).
      // changeLayerProperties below then sets pixel_format / colorspace /
      // drawable size and triggers a deferred PSO build at the first Present.
      m_presenter = Rc(new Presenter(
          m_device->dxmtQueue(), m_device->metalDevice(), m_layer, m_device->internalCommandLibrary(),
          /*scale_factor=*/1.0f, /*sample_count=*/1
      ));
      // Display sync is not seeded here: Present decides it from the effective
      // presentation interval every frame, since D3DPRESENT_FORCEIMMEDIATE can
      // change the answer without the chain being recreated.
      // A D3D9 Present is an opaque copy of the backbuffer to the window.
      // the backbuffer alpha is scene scratch, not a window-coverage value.
      // Present it with DXGI_ALPHA_MODE_IGNORE so the present blit doesn't
      // scale the colour by that stray alpha (it darkened the 3D world while
      // leaving the opaque HUD, which writes alpha=1, untouched).
      m_presenter->setPresentAlphaMode(/*DXGI_ALPHA_MODE_IGNORE=*/3);
      // Drawable extent = the host window's live client size (see
      // layerDrawableExtent). When BackBufferWidth/Height differ, pre-
      // modern games hardcode 1024x768 etc., the Presenter's present_scale_
      // PSO does a fit-to-drawable blit. d3d11 sizes off the backbuffer
      // here (d3d11_swapchain.cpp ApplyLayerProps); d3d9 forks to the
      // window because the legacy-resolution problem only really hits dx9
      // apps and a window-sized drawable keeps windowed mode crisp.
      WMTLayerProps current{};
      m_layer.getProps(current);
      double drawable_w, drawable_h;
      layerDrawableExtent(m_hWindow, current, m_params, drawable_w, drawable_h);
      m_presenter->changeLayerProperties(
          D3DFormatToMetal(m_params.BackBufferFormat, D3D9FormatUsage::RenderTarget),
          layerColorSpace(m_params.BackBufferFormat), drawable_w, drawable_h,
          /*sample_count=*/1
      );
      // CAMetalLayer's default maximumDrawableCount is 3, which is too
      // deep for 60Hz games: presents pile up in the queue and Present
      // Delay stretches to 2-3 vsync intervals (the GPU consistently
      // working 2 frames behind the displayed frame). 2 is the standard
      // for low-latency games on Apple Silicon and matches MoltenVK's
      // default. Apps requesting D3DPRESENT_INTERVAL_IMMEDIATE still get
      // the same pacing because nextDrawable is gated by the layer's own
      // queue, not by the present-after-minimum-duration hint.
      WMTLayerProps drawable_cap{};
      m_layer.getProps(drawable_cap);
      drawable_cap.maximum_drawable_count = 2;
      m_layer.setProps(drawable_cap);
    }
  }
}

void
MTLD3D9SwapChain::destroyPresentTarget(bool defer_view_release) {
  // Drop the Presenter before tearing down the NSView; the Presenter holds a
  // non-retaining WMT::MetalLayer copy whose backing CALayer is owned by the
  // view, plus PSOs / a gamma-LUT texture that route their dispose through the
  // device. Sequence the presenter release explicitly so it precedes the view
  // teardown. The present chunk captures its own Rc copy of the presenter, so
  // dropping the map's reference here never frees one an in-flight present needs.
  m_presenter = nullptr;
  if (m_view.handle != 0) {
    if (defer_view_release) {
      // App-owned chain torn down while the device is live: a present chunk may
      // still reference this view from the encode thread and Metal does not
      // retain the layer for un-encoded work. Defer to GPU completion, FIFO
      // after that chunk's encode + retire (the override-target defer shape).
      m_device->dxmtQueue().CurrentChunk()->addCompletionTarget(std::make_shared<D9DeferredViewRelease>(m_view));
    } else {
      // Inline release, safe because the caller drained the GPU first (implicit
      // teardown after the device drain, or the Reset window-change branch).
      // The completion-callback thread has no outer autorelease pool.
      auto pool = WMT::MakeAutoreleasePool();
      WMT::ReleaseMetalView(m_view);
    }
  }
  m_view = {};
  m_layer = {};
}

bool
MTLD3D9SwapChain::buildBackBuffer() {
  m_backBuffers.clear();
  m_resolveTarget = nullptr;
  // Reset drops the COPY front canvas; the first rect present after the
  // new backbuffer exists re-materialises it at the new extent. The
  // content-rotation scratch is extent/format-bound the same way; drop it so
  // the first multi-backbuffer Present rebuilds it against the new backbuffer.
  m_frontCanvas = nullptr;
  m_rotationScratch = nullptr;
  const UINT count = std::max<UINT>(1u, m_params.BackBufferCount);

  logSwapParams(m_params, "create");

  const uint32_t sampleCount = m_device->metalSampleCount(m_params.MultiSampleType, m_params.MultiSampleQuality);
  WMTTextureInfo info = {};
  D3DSURFACE_DESC desc;
  backBufferDescriptors(m_params, sampleCount, info, desc);
  if (sampleCount > 1) {
    m_resolveTarget = buildResolveTarget(m_device, info);
    if (!m_resolveTarget)
      return false;
  }

  // Allocate BackBufferCount surfaces, each over its own dxmt::Texture
  // allocation. Slot 0 is the one the device auto-binds to RT0 and the
  // Presenter blits at Present time. Slots 1..N-1 exist to satisfy
  // GetBackBuffer(i): apps occasionally fetch additional slots for
  // their own pipelining, even though Metal's CAMetalLayer is what
  // does the actual in-flight buffering on this backend.
  m_backBuffers.reserve(count);
  const bool lockable = (m_params.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) != 0;
  for (UINT i = 0; i < count; ++i) {
    Rc<dxmt::Texture> dxmt_bb_texture = new dxmt::Texture(info, m_device->metalDevice());
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
    auto allocation = dxmt_bb_texture->allocate(alloc_flags);
    if (!allocation || !allocation->texture()) {
      m_backBuffers.clear();
      return false;
    }
    WMT::Texture rawTex = allocation->texture();
    dxmt_bb_texture->rename(std::move(allocation));
    // Clear the fresh backbuffer so a draw-less Present right after (re)creation
    // shows a defined frame instead of whatever the Metal allocator recycled
    // from freed process memory (a startup present of a fresh backbuffer would
    // otherwise flash a recognizable prior image for one frame). The RenderTarget
    // usage routes this to the initializer's clear arm.
    m_device->initTextureWithZero(dxmt_bb_texture.ptr());
    if (d9PresentDbgEnabled())
      Logger::warn(str::format("d9 backbuffer built: tex=", (const void *)dxmt_bb_texture.ptr(), " (cleared)"));
    void *bbCpu = nullptr, *bbOwned = nullptr;
    uint32_t bbPitch = 0;
    allocLockableBackBufferMirror(lockable, desc, bbCpu, bbPitch, bbOwned);
    auto *bb = new MTLD3D9Surface(
        m_device, desc, static_cast<IDirect3DSwapChain9 *>(this), WMT::Reference<WMT::Texture>(rawTex),
        /*mipLevel=*/0,
        /*selfPin=*/false,
        /*parentTextureType=*/WMTTextureType2D,
        /*buffer=*/{},
        /*cpuPtr=*/bbCpu,
        /*pitch=*/bbPitch,
        /*arraySlice=*/0,
        /*ownedBacking=*/bbOwned,
        /*dxmtTexture=*/std::move(dxmt_bb_texture)
    );
    // Counts in the device loss gate while the app holds a public ref: native
    // fails a non-Ex Reset with an app-held backbuffer alive.
    bb->markImplicitLosable();
    m_backBuffers.emplace_back(bb);
  }
  return true;
}

HRESULT
MTLD3D9SwapChain::ResetForDeviceReset(const D3DPRESENT_PARAMETERS &params, HWND hEffectiveWindow) {
  // Reset detaches the old backbuffer surfaces and hands out fresh objects:
  // an app-held pre-Reset backbuffer keeps its old desc and contents, loses
  // its swapchain container, and still answers GetContainer for the device
  // (wine's d3d9ex tests pin the contract). Only an Ex app can observe the
  // detach; the non-Ex Reset fails while implicit surfaces are app-held.
  // Allocate textures first; only swap on success to keep chain coherent on OOM.
  logSwapParams(params, "reset");
  const UINT new_count = std::max<UINT>(1u, params.BackBufferCount);
  const uint32_t sampleCount = m_device->metalSampleCount(params.MultiSampleType, params.MultiSampleQuality);
  WMTTextureInfo info = {};
  D3DSURFACE_DESC desc;
  backBufferDescriptors(params, sampleCount, info, desc);
  Rc<dxmt::Texture> new_resolve_target;
  if (sampleCount > 1) {
    new_resolve_target = buildResolveTarget(m_device, info);
    if (!new_resolve_target)
      return D3DERR_DEVICELOST;
  }

  std::vector<Rc<dxmt::Texture>> new_dxmt_textures;
  std::vector<WMT::Texture> new_raw_textures;
  new_dxmt_textures.reserve(new_count);
  new_raw_textures.reserve(new_count);
  for (UINT i = 0; i < new_count; ++i) {
    Rc<dxmt::Texture> dxmt_bb = new dxmt::Texture(info, m_device->metalDevice());
    dxmt::Flags<dxmt::TextureAllocationFlag> alloc_flags;
    alloc_flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
    auto allocation = dxmt_bb->allocate(alloc_flags);
    if (!allocation || !allocation->texture())
      return D3DERR_DEVICELOST;
    new_raw_textures.push_back(allocation->texture());
    dxmt_bb->rename(std::move(allocation));
    // Clear the fresh backbuffer, same as buildBackBuffer: on Reset the just
    // freed pre-Reset surfaces are the likeliest pages this allocation lands on,
    // so a draw-less Present after the reset must not display them.
    m_device->initTextureWithZero(dxmt_bb.ptr());
    if (d9PresentDbgEnabled())
      Logger::warn(str::format("d9 backbuffer reset-rebuilt: tex=", (const void *)dxmt_bb.ptr(), " (cleared)"));
    new_dxmt_textures.push_back(std::move(dxmt_bb));
  }

  // All allocations succeeded; commit the new params and rebind. Report the
  // resolved device window (focus window when the app passed a null
  // hDeviceWindow), matching the ctor and what GetPresentParameters returns.
  m_params = params;
  m_params.hDeviceWindow = hEffectiveWindow;
  m_resolveTarget = std::move(new_resolve_target);

  // Detach every old surface first (a dying surface's detach is harmless),
  // then rebuild the chain's slots as fresh objects. The COPY front canvas
  // is extent-bound; drop it for lazy re-materialisation like buildBackBuffer.
  for (auto &old_bb : m_backBuffers)
    old_bb->detachContainer();
  m_backBuffers.clear();
  m_frontCanvas = nullptr;
  // Same extent/format binding as m_frontCanvas: drop the rotation scratch so a
  // post-Reset multi-backbuffer Present re-materialises it at the new extent.
  m_rotationScratch = nullptr;
  const bool lockable = (params.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) != 0;
  m_backBuffers.reserve(new_count);
  for (UINT i = 0; i < new_count; ++i) {
    void *bbCpu = nullptr, *bbOwned = nullptr;
    uint32_t bbPitch = 0;
    allocLockableBackBufferMirror(lockable, desc, bbCpu, bbPitch, bbOwned);
    auto *bb = new MTLD3D9Surface(
        m_device, desc, static_cast<IDirect3DSwapChain9 *>(this), WMT::Reference<WMT::Texture>(new_raw_textures[i]),
        /*mipLevel=*/0,
        /*selfPin=*/false,
        /*parentTextureType=*/WMTTextureType2D,
        /*buffer=*/{},
        /*cpuPtr=*/bbCpu,
        /*pitch=*/bbPitch,
        /*arraySlice=*/0,
        /*ownedBacking=*/bbOwned,
        /*dxmtTexture=*/std::move(new_dxmt_textures[i])
    );
    bb->markImplicitLosable();
    m_backBuffers.emplace_back(bb);
  }

  if (hEffectiveWindow != nullptr && hEffectiveWindow != m_hWindow) {
    // Reset changed the device window: rebuild the whole present target, not
    // just the layer props. The old same-window changeLayerProperties path
    // sized the drawable off the now-stale m_hWindow and kept presenting into
    // the old window's (possibly destroyed) CAMetalLayer. Inline release is
    // safe: the hpp precondition ("Caller must have drained the GPU first") is
    // met by Reset step 1's full drain, which retired every present chunk and
    // dropped the Rc<Presenter> it captured, so no encode-thread work can still
    // touch this layer. An undrained future caller would reintroduce the
    // encode-thread use-after-free the deferred override-view release guards
    // against. Evict any override entry already keyed on the NEW window first,
    // else the rebuild stacks a second WineMetalView on the same HWND.
    if (auto it = m_overrideTargets.find(hEffectiveWindow); it != m_overrideTargets.end()) {
      releaseOverrideTarget(it->second, /*defer_view_release=*/false);
      m_overrideTargets.erase(it);
    }
    destroyPresentTarget();
    // createPresentTarget re-seeds m_hWindow / m_lastMonitor / m_refreshRateHz;
    // the per-Present resize probe keys off m_lastWindowW/H alone, so clear them
    // to force a re-seed at the new window's client extent on the next Present.
    m_lastWindowW = m_lastWindowH = 0;
    createPresentTarget(hEffectiveWindow);
  } else if (m_presenter != nullptr && m_layer.handle != 0) {
    // Same window (or a null new window, where we keep the old target): update
    // the layer's drawable extent + format to match the new params. The
    // Presenter caches these and rebuilds its present PSO on the next Present if
    // any input changed. Size off the live window client rect, not the layer's
    // pinned (now-stale) drawableSize; see layerDrawableExtent.
    WMTLayerProps current{};
    m_layer.getProps(current);
    double drawable_w, drawable_h;
    layerDrawableExtent(m_hWindow, current, m_params, drawable_w, drawable_h);
    m_presenter->changeLayerProperties(
        D3DFormatToMetal(m_params.BackBufferFormat, D3D9FormatUsage::RenderTarget),
        layerColorSpace(m_params.BackBufferFormat), drawable_w, drawable_h, /*sample_count=*/1
    );
  }
  // Re-evaluate the gamma gate against the new params: a rebuilt Presenter
  // starts with an identity LUT, and a windowed<->fullscreen flip changes
  // whether the stored ramp applies at all (fullscreen-only). reapply
  // re-pushes the stored ramp in fullscreen and clears it when windowed;
  // no-ops on a headless chain or when no ramp was ever set.
  reapplyGammaRamp();
  return D3D_OK;
}

MTLD3D9SwapChain::OverrideTarget *
MTLD3D9SwapChain::resolveOverrideTarget(HWND hwnd) {
  evictStaleOverrideTargets();
  auto it = m_overrideTargets.find(hwnd);
  if (it != m_overrideTargets.end())
    return it->second.presenter != nullptr ? &it->second : nullptr;
  OverrideTarget target{};
  target.view = WMT::CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(hwnd), m_device->metalDevice(), target.layer);
  if (target.layer.handle != 0) {
    target.presenter = Rc(new Presenter(
        m_device->dxmtQueue(), m_device->metalDevice(), target.layer, m_device->internalCommandLibrary(),
        /*scale_factor=*/1.0f, /*sample_count=*/1
    ));
    // Display sync is left to Present, as for the device-window presenter.
    // Same present composite contract as the device window's presenter:
    // the backbuffer alpha is scene scratch, not window coverage.
    target.presenter->setPresentAlphaMode(/*DXGI_ALPHA_MODE_IGNORE=*/3);
    queryMonitorRefreshRate(windowMonitor(hwnd), &target.refresh_hz);
  } else {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      Logger::warn("d3d9: Present hDestWindowOverride window has no presentable view; presenting to the device window");
  }
  auto [ins, added] = m_overrideTargets.emplace(hwnd, std::move(target));
  return ins->second.presenter != nullptr ? &ins->second : nullptr;
}

void
MTLD3D9SwapChain::releaseOverrideTarget(OverrideTarget &target, bool defer_view_release) {
  // Drop the Presenter first: it holds a non-retaining copy of the layer the
  // view owns, so the view must outlive it. The present chunk captures its own
  // Rc copy of the presenter, so releasing the map's reference here never frees
  // a presenter an in-flight present still needs.
  target.presenter = nullptr;
  if (target.view.handle == 0)
    return;
  if (defer_view_release) {
    // The view owns the CAMetalLayer that a present chunk calls nextDrawable on
    // from the encode thread, and presents encode asynchronously: a chunk that
    // captured this target may not be encoded yet, and Metal does not retain
    // the layer for un-encoded work. Defer the release to GPU completion, which
    // is FIFO after that chunk's encode and retire.
    m_device->dxmtQueue().CurrentChunk()->addCompletionTarget(std::make_shared<D9DeferredViewRelease>(target.view));
  } else {
    // Inline teardown. The implicit chain is destroyed only after the device
    // drains its queue, so every present chunk has retired and the view is
    // unreferenced. An additional (app-owned) chain Released while the device
    // is still live shares the eviction path's un-drained hazard, a pre-existing
    // narrow gap tracked separately: deferring here instead would leak, because
    // at implicit teardown the target chunk has already retired and a callback
    // registered on it never fires.
    WMT::ReleaseMetalView(target.view);
  }
  target.view = {};
}

void
MTLD3D9SwapChain::evictStaleOverrideTargets() {
  for (auto it = m_overrideTargets.begin(); it != m_overrideTargets.end();) {
    if (!wsi::isWindow(it->first)) {
      releaseOverrideTarget(it->second, /*defer_view_release=*/true);
      it = m_overrideTargets.erase(it);
    } else {
      ++it;
    }
  }
}

MTLD3D9SwapChain::~MTLD3D9SwapChain() {
  // Teardown order: the override targets first, then the main present target,
  // each presenter-before-view (a presenter holds a non-retaining MetalLayer
  // copy whose CALayer the view owns). The implicit chain is destroyed only
  // after the device drains its queue, so every present chunk has retired and
  // inline view release is safe. An additional (app-owned) chain is Released
  // while the device is live: present chunks may still reference its views
  // un-encoded, so defer the view releases to GPU completion. Hold the device
  // lock across the deferral: the completion target registers on the current
  // un-committed chunk, and under D3DCREATE_MULTITHREADED a concurrent thread
  // could otherwise commit that chunk out from under the registration.
  const bool defer = !m_isImplicit;
  D9DeviceLock lock = defer ? m_device->LockDevice() : D9DeviceLock();
  {
    auto pool = WMT::MakeAutoreleasePool();
    for (auto &entry : m_overrideTargets)
      releaseOverrideTarget(entry.second, /*defer_view_release=*/defer);
  }
  m_overrideTargets.clear();
  destroyPresentTarget(/*defer_view_release=*/defer);
}

ULONG STDMETHODCALLTYPE
MTLD3D9SwapChain::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1) {
    m_device->AddRef();
    // An additional (app-owned) chain counts toward the device-loss gate on its
    // public 0->1 edge: native fails a non-Ex Reset while an additional chain is
    // alive (MSDN: all additional swap chains must be released before Reset).
    // The implicit chain is device-owned and never counts.
    if (!m_isImplicit)
      m_device->onLosableResourceCreated();
  }
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9SwapChain::Release() {
  // Capture the device before the base Release. An app-owned additional
  // chain has no device-held private ref, so ComObject::Release drops the
  // last reference and deletes `this`; reading m_device afterward would be
  // a use-after-free. The implicit chain (device holds an AddRefPrivate)
  // is never deleted here, so capturing the pointer is equally correct for
  // it. The chain's own pin keeps the device alive across the destructor;
  // dropping it last balances the pin taken in AddRef.
  MTLD3D9Device *device = m_device;
  // Capture before the base Release may delete `this`: the losable decrement
  // below must read the flag off a live object.
  const bool is_implicit = m_isImplicit;
  // D3D9 clamps Release-at-0: the implicit chain is handed out at public
  // refcount 0 (the device holds it via a private ref), so an app that
  // Releases a GetSwapChain(0) result past 0 must see 0, not an underflow,
  // and must not re-unpin the device.
  if (m_refCount.load() == 0)
    return 0;
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    // Balance the AddRef-time losable count taken for an additional chain.
    if (!is_implicit)
      device->onLosableResourceDestroyed();
    device->Release();
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DSwapChain9)) {
    *ppvObject = static_cast<IDirect3DSwapChain9 *>(this);
    AddRef();
    return S_OK;
  }
  if (m_isEx && riid == __uuidof(IDirect3DSwapChain9Ex)) {
    *ppvObject = static_cast<IDirect3DSwapChain9Ex *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::Present(
    const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags
) {
  D9DeviceLock lock = m_device->LockDevice();
  (void)pDirtyRegion; // hint only, spec-permitted to ignore
  // Lost-device gate: presentStateGate returns S_PRESENT_OCCLUDED (Ex) or
  // D3DERR_DEVICELOST (non-Ex) when the device is not presentable (the wine
  // spec shape), D3D_OK otherwise.
  if (HRESULT state = m_device->presentStateGate(); state != D3D_OK)
    return state;
  // Foreground-lost probe. d3d11 has the same shape at
  // d3d11_swapchain.cpp. For Ex devices the spec return is
  // S_PRESENT_OCCLUDED; for non-Ex devices MSDN's "Lost Devices" page
  // says the DEVICELOST transition is driven by focus loss, but native
  // doesn't synthesize DEVICELOST on a simple minimize; it just stops
  // updating the front buffer. We follow that: minimized + Ex returns
  // S_PRESENT_OCCLUDED; minimized + non-Ex returns D3D_OK after the
  // draw queue is drained (so resource lifetimes stay coherent across
  // the no-display window). Skip if no hWnd (headless smokes).
  if (m_hWindow && wsi::isMinimized(m_hWindow)) {
    m_device->FlushDrawBatch();
    m_device->flushOpenWork();
    return m_isEx ? S_PRESENT_OCCLUDED : D3D_OK;
  }
  // No in-scene gate: apps Present mid-scene (driver behavior, not spec).
  // D3DPRESENT_FORCEIMMEDIATE is documented FLIPEX-only, but DXVK
  // honors it on any swap effect as an interval-zero present and apps
  // written against that never see the documented INVALIDCALL; follow
  // the reference. The flag already overrides the vsync dwell where
  // the interval multiplier is derived below.
  // D3DPRESENT_DONOTWAIT: MSDN PresentEx parameter table item 3; if
  // the GPU is still busy with the previous frame, return
  // D3DERR_WASSTILLDRAWING instead of blocking. The frame-latency throttle
  // at the end of a normal Present would block until the frame max_latency
  // presents back has retired; peek that value here and short-circuit if it
  // hasn't retired yet. Neither wined3d (swapchain.c FIXMEs flags) nor
  // DXVK honors this; we're strict-spec at trivial cost.
  if (dwFlags & D3DPRESENT_DONOTWAIT) {
    auto &queue = m_device->dxmtQueue();
    // Probe with the same clamp the pre-boundary re-clamp below applies, so a
    // SetMaximumFrameLatency earlier this frame is already reflected here
    // rather than one Present late.
    uint32_t max_latency = std::min(m_device->getFrameLatency(), m_params.BackBufferCount + 1u);
    uint64_t next_seq = queue.CurrentFrameSeq();
    if (next_seq > max_latency && queue.FrameLatencySignaled() < next_seq - max_latency)
      return D3DERR_WASSTILLDRAWING;
  }

  // Headless chain (no layer): no-op success. Existing per-call cmdbufs
  // commit immediately, so there's nothing to flush. Real apps reach
  // here with m_layer non-null.
  //
  // D3DSWAPEFFECT_COPY partial present: the rects copy a backbuffer
  // sub-rect onto the front at a possibly different position, and the
  // uncovered front area keeps its previous contents. The drawable pool
  // cannot preserve history across frames, so the first rect present
  // materialises a persistent front canvas: from then on every present
  // composites into it (full frame without rects, source rect to dest
  // rect with them) and the canvas is what the presenter blits. Chains
  // that never pass rects never allocate it. The rects are COPY-only per
  // the spec; other swap effects ignore them (wined3d swapchain.c honours
  // them through its front-buffer blit, DXVK through its present region).
  const uint32_t bb_w = m_params.BackBufferWidth;
  const uint32_t bb_h = m_params.BackBufferHeight;
  bool canvas_seed = false;
  if (m_params.SwapEffect == D3DSWAPEFFECT_COPY && (pSourceRect || pDestRect) && m_frontCanvas == nullptr &&
      m_resolveTarget == nullptr) {
    WMTTextureInfo cinfo = {};
    D3DSURFACE_DESC cdesc;
    backBufferDescriptors(m_params, /*sampleCount=*/1, cinfo, cdesc);
    m_frontCanvas = buildResolveTarget(m_device, cinfo);
    // Seed the whole canvas from the current backbuffer so the area the
    // first rect leaves uncovered shows the frame the app already drew.
    canvas_seed = m_frontCanvas != nullptr;
  }
  // Clamp the rects to the backbuffer/canvas extent; wined3d clips the
  // same way rather than rejecting. Null means the full extent.
  auto clamp_rect = [](const RECT *r, uint32_t w, uint32_t h, WMTOrigin &origin, WMTSize &size) {
    LONG l = r ? std::max<LONG>(0, r->left) : 0;
    LONG tp = r ? std::max<LONG>(0, r->top) : 0;
    LONG rt = r ? std::min<LONG>(w, r->right) : static_cast<LONG>(w);
    LONG bt = r ? std::min<LONG>(h, r->bottom) : static_cast<LONG>(h);
    origin = {static_cast<uint32_t>(l), static_cast<uint32_t>(tp), 0};
    size = {static_cast<uint32_t>(std::max<LONG>(0, rt - l)), static_cast<uint32_t>(std::max<LONG>(0, bt - tp)), 1};
    return size.width > 0 && size.height > 0;
  };
  WMTOrigin rect_src_origin{}, rect_dst_origin{};
  WMTSize rect_src_size{}, rect_dst_size{};
  bool canvas_rect_blit = false;
  if (m_frontCanvas != nullptr && m_params.SwapEffect == D3DSWAPEFFECT_COPY && (pSourceRect || pDestRect)) {
    canvas_rect_blit = clamp_rect(pSourceRect, bb_w, bb_h, rect_src_origin, rect_src_size) &&
                       clamp_rect(pDestRect, bb_w, bb_h, rect_dst_origin, rect_dst_size);
  }
  auto pool = WMT::MakeAutoreleasePool();
  // Present routes through a chunk so the present-blit's wine_unix_calls
  // (cmdbuf.commit, presentDrawable, encodeCommands) run on the dxmt
  // encode thread instead of the wow64 main thread. Drain queued batched
  // draws onto a chunk first so the Present chunk observes them through
  // Metal queue ordering. flushOpenWork() catches any residual sync cmdbuf
  // work.
  m_device->FlushDrawBatch();
  m_device->flushOpenWork();
  // Present hDestWindowOverride: retarget this frame to the override
  // window's presenter (created on first use), leaving the device
  // window's chain state untouched. wined3d hands the override window to
  // its swapchain present the same way; a failed view resolution falls
  // back to the device window.
  WMT::MetalLayer active_layer = m_layer;
  Rc<Presenter> active_presenter = m_presenter;
  double active_refresh = m_refreshRateHz;
  bool override_present = false;
  if (hDestWindowOverride != nullptr && hDestWindowOverride != m_hWindow) {
    if (OverrideTarget *target = resolveOverrideTarget(hDestWindowOverride)) {
      uint32_t win_w = 0, win_h = 0;
      wsi::getWindowSize(hDestWindowOverride, &win_w, &win_h);
      if (win_w > 0 && win_h > 0 && (win_w != target->last_w || win_h != target->last_h)) {
        target->last_w = win_w;
        target->last_h = win_h;
        WMTLayerProps current{};
        target->layer.getProps(current);
        double drawable_w, drawable_h;
        layerDrawableExtent(hDestWindowOverride, current, m_params, drawable_w, drawable_h);
        target->presenter->changeLayerProperties(
            D3DFormatToMetal(m_params.BackBufferFormat, D3D9FormatUsage::RenderTarget),
            layerColorSpace(m_params.BackBufferFormat), drawable_w, drawable_h, /*sample_count=*/1
        );
      }
      active_layer = target->layer;
      active_presenter = target->presenter;
      active_refresh = target->refresh_hz;
      override_present = true;
    }
  }
  if (active_layer.handle == 0 || active_presenter == nullptr) {
    return D3D_OK;
  }

  // Per-Present window-resize tracking: NSView resizes asynchronously after
  // game window changes, so drawable size goes stale across Reset. Re-probe
  // here so layer snaps to new window once resize settles (cheap: GetClientRect
  // only fires changeLayerProperties on actual size change).
  if (m_hWindow && !override_present) {
    uint32_t win_w = 0, win_h = 0;
    wsi::getWindowSize(m_hWindow, &win_w, &win_h);
    if (win_w > 0 && win_h > 0 && (win_w != m_lastWindowW || win_h != m_lastWindowH)) {
      m_lastWindowW = win_w;
      m_lastWindowH = win_h;
      WMTLayerProps current{};
      m_layer.getProps(current);
      double drawable_w, drawable_h;
      layerDrawableExtent(m_hWindow, current, m_params, drawable_w, drawable_h);
      m_presenter->changeLayerProperties(
          D3DFormatToMetal(m_params.BackBufferFormat, D3D9FormatUsage::RenderTarget),
          layerColorSpace(m_params.BackBufferFormat), drawable_w, drawable_h, /*sample_count=*/1
      );
    }
    // Re-probe the refresh rate when the window moves to a different monitor.
    // m_refreshRateHz drives the INTERVAL_TWO/THREE/FOUR vsync dwell; sampled
    // once at ctor it goes stale exactly like the drawable extent did when the
    // window is dragged to a display with a different rate (e.g. 60Hz panel ->
    // 120Hz external). getCurrentDisplayMode only runs on the rare monitor
    // change. (MonitorFromWindow is the only per-frame add and is cheap.)
    if (HMONITOR mon = windowMonitor(m_hWindow); mon != m_lastMonitor) {
      m_lastMonitor = mon;
      queryMonitorRefreshRate(mon, &m_refreshRateHz);
    }
    // The re-probe above may have just updated the rate; the vsync dwell
    // below must use this frame's value, not the pre-probe copy.
    active_refresh = m_refreshRateHz;
  }

  // synchronizeLayerProperties picks up display-side colorspace / EDR /
  // HDR-metadata changes and (re)builds the present PSO if any input
  // changed. Returns a PresentState whose dtor signals the Presenter's
  // frame_presented_ fence; synchronizeLayerProperties waits on that
  // fence on the next call to detect prior present completion before
  // committing to a PSO rebuild. The state MUST be moved into the
  // chunk lambda (NOT copied out as metadata) so the dtor fires when
  // the lambda is destroyed (post-Metal-retire) rather than when this
  // function returns. d3d11_swapchain.cpp is the literal model.
  auto state = active_presenter->synchronizeLayerProperties();

  // d3d11_swapchain.cpp is the literal model: chunk->emitcc with
  // ctx.present(backbuffer Rc<>, presenter, vsync_duration, metadata).
  // The chunk runs on the encode thread: ArgumentEncodingContext's
  // PresentData encoder allocates a cmdbuf there, calls
  // presenter->encodeCommands + presentDrawable + commit. The heavy
  // present work (drawable acquire, present-blit, commit) is therefore all
  // encode-thread; the one calling-thread wine_unix_call left in this path is
  // the display colorspace/HDR query synchronizeLayerProperties issues above
  // (per-frame, unconditional; gating it on a display-config change is a
  // measured-perf candidate, not done here).
  auto &queue = m_device->dxmtQueue();
  // Per-Present re-clamp of queue max latency. The ctor seeds it from
  // min(getFrameLatency(), BackBufferCount + 1u) but the device's
  // SetMaximumFrameLatency pushes the raw value to the queue without
  // re-clamping, so a post-create SetMaximumFrameLatency(8) on a
  // BackBufferCount=1 chain would otherwise let the queue race 8 frames
  // ahead. DXVK recomputes the same clamp every Present
  // (d3d9_swapchain.cpp GetActualFrameLatency); we mirror that.
  queue.SetMaxLatency(std::min(m_device->getFrameLatency(), m_params.BackBufferCount + 1u));
  auto *chunk = queue.CurrentChunk();
  const uint64_t frame_latency_seq = queue.CurrentFrameSeq();
  chunk->signal_frame_latency_fence_ = frame_latency_seq;
  // Vsync is the layer's hardware display sync, which is what every reference
  // available here uses: the d3d11 frontend in this tree drives the same
  // property off its SyncInterval (d3d11_swapchain.cpp Present1), wined3d's
  // buffer swap IS the vblank wait, and DXVK asks Vulkan for a FIFO present
  // mode. The minimum-duration hint is a frame-rate limiter rather than a vsync
  // mechanism, so it stays zero for a single interval and carries only the
  // multiple an app asks for at INTERVAL_TWO and above, where one vblank is not
  // enough dwell. Pacing a single interval through that hint instead left
  // presentation unlocked from the vblank entirely; a compositor re-imposes its
  // own timing on a windowed layer and hides that, and nothing hides it once
  // the layer reaches the display directly. D3DPRESENT_FORCEIMMEDIATE overrides
  // PresentationInterval per-frame (apps toggle between menus and gameplay
  // without recreating the chain), so both inputs decide it per Present.
  const DWORD pi = m_params.PresentationInterval;
  const bool present_immediate = (pi & D3DPRESENT_INTERVAL_IMMEDIATE) || (dwFlags & D3DPRESENT_FORCEIMMEDIATE);
  int multiplier = 1; // DEFAULT / ONE
  if (pi & D3DPRESENT_INTERVAL_FOUR)
    multiplier = 4;
  else if (pi & D3DPRESENT_INTERVAL_THREE)
    multiplier = 3;
  else if (pi & D3DPRESENT_INTERVAL_TWO)
    multiplier = 2;
  const double vsync_duration =
      (present_immediate || multiplier == 1) ? 0.0 : static_cast<double>(multiplier) / active_refresh;
  // Idempotent inside the presenter, so a per-Present call costs a compare
  // except on the frame an app actually toggles the interval.
  active_presenter->setDisplaySyncEnabled(!present_immediate);
  auto bb_dxmt = m_backBuffers[0]->dxmtTexture();
  Rc<dxmt::Texture> resolve_target = m_resolveTarget;
  Rc<dxmt::Texture> canvas = m_frontCanvas;
  if (d9PresentDbgEnabled()) {
    const char *path = (bb_dxmt->sampleCount() > 1 && resolve_target != nullptr) ? "resolve"
                       : (canvas != nullptr)                                     ? "canvas"
                                                                                 : "direct";
    Logger::warn(
        str::format(
            "d9 present ", m_presentationCount, ": path=", path, " bb=", (const void *)bb_dxmt.ptr(),
            " resolve=", (const void *)resolve_target.ptr(), " canvas=", (const void *)canvas.ptr()
        )
    );
  }
  // Multi-backbuffer content rotation (wined3d swapchain.c wined3d_swapchain_*_
  // rotate, DXVK d3d9_swapchain.cpp Present): a flip chain advances backbuffer
  // CONTENT one slot per Present, new[i] = old[(i+1) % N], so the just-presented
  // slot 0 wraps to the last slot while the app-visible surface pointers stay
  // fixed. wined3d/DXVK do an O(1) backing swap; dxmt keeps the surface backings
  // IMMUTABLE (the encode thread resolves RT0 and any readback to a slot's Metal
  // texture lazily, so a calling-thread backing swap would race those readers,
  // the vsync-rotation bug this replaced) and rotates the CONTENT with ordered
  // matched-sample-count blit-encoder copies through a persistent scratch texture
  // in the present chunk below. COPY keeps its front canvas and never rotates;
  // BackBufferCount == 1 (the DISCARD default and common case) does zero work.
  std::vector<Rc<dxmt::Texture>> rotation_textures;
  Rc<dxmt::Texture> rotation_scratch;
  if (m_backBuffers.size() > 1 && m_params.SwapEffect != D3DSWAPEFFECT_COPY) {
    if (m_rotationScratch == nullptr) {
      // Scratch matches the backbuffers' sample count so the rotation copy is a
      // matched-count blit (single-sample or MSAA alike); an MSAA scratch keeps
      // the multisample content the app renders straight to an MSAA backbuffer,
      // which a single-sample sampler blit could not source. Allocated like the
      // backbuffers themselves in buildBackBuffer (same sampleCount + info).
      const uint32_t sampleCount = m_device->metalSampleCount(m_params.MultiSampleType, m_params.MultiSampleQuality);
      WMTTextureInfo sinfo = {};
      D3DSURFACE_DESC sdesc;
      backBufferDescriptors(m_params, sampleCount, sinfo, sdesc);
      m_rotationScratch = buildPrivateTarget(m_device, sinfo);
    }
    if (m_rotationScratch != nullptr) {
      rotation_textures.reserve(m_backBuffers.size());
      for (auto &bb : m_backBuffers)
        rotation_textures.push_back(bb->dxmtTexture());
      rotation_scratch = m_rotationScratch;
    }
  }
  Rc<Presenter> presenter = std::move(active_presenter);
  chunk->emitcc([backbuffer = std::move(bb_dxmt), resolve_target = std::move(resolve_target),
                 canvas = std::move(canvas), canvas_seed, canvas_rect_blit, rect_src_origin, rect_src_size,
                 rect_dst_origin, rect_dst_size, presenter = std::move(presenter), vsync_duration, bb_w, bb_h,
                 rotation_textures = std::move(rotation_textures), rotation_scratch = std::move(rotation_scratch),
                 state = std::move(state)](ArgumentEncodingContext &ctx) mutable {
    // D3D9 auto-resolves an MSAA backbuffer at Present; the CAMetalLayer
    // drawable is single-sample, so average-resolve into the resolve target
    // and present that (the DXVK/wined3d shape). A single-sample chain
    // presents the backbuffer directly.
    if (backbuffer->sampleCount() > 1 && resolve_target) {
      // Resolve through the full views: the resolve binds its source as a
      // color attachment (needs RenderTarget), but a custom createView on a
      // texture that also carries PixelFormatView usage (for sRGB-write)
      // comes back without the RenderTarget bit, so the encode-side guard
      // would skip the pass. The full view keeps the texture's whole usage.
      ctx.resolve_texture_cmd.resolve(
          backbuffer, backbuffer->fullView, resolve_target, resolve_target->fullView, ResolveTextureMode::Average,
          WMTScissorRect{0, 0, bb_w, bb_h}, WMTOrigin{0, 0, 0}, WMTSize{bb_w, bb_h, 1}
      );
      ctx.present(resolve_target, presenter, vsync_duration, state.metadata);
    } else if (canvas != nullptr) {
      // COPY front canvas: composite this present's contribution, then blit
      // the canvas. A rect-less present (and the one-time seed) contributes
      // the whole backbuffer; a rect present contributes source to dest.
      // Point filtering: a same-size copy must not resample, and a
      // stretching Present rect pair is a copy, not a filtered scale.
      if (canvas_seed || !canvas_rect_blit)
        ctx.stretch_blit_cmd.blit(
            backbuffer, backbuffer->fullView, canvas, canvas->fullView, StretchBlitContext::Filter::Point,
            WMTOrigin{0, 0, 0}, WMTSize{bb_w, bb_h, 1}, WMTOrigin{0, 0, 0}, WMTSize{bb_w, bb_h, 1}
        );
      if (canvas_rect_blit)
        ctx.stretch_blit_cmd.blit(
            backbuffer, backbuffer->fullView, canvas, canvas->fullView, StretchBlitContext::Filter::Point,
            rect_src_origin, rect_src_size, rect_dst_origin, rect_dst_size
        );
      ctx.present(canvas, presenter, vsync_duration, state.metadata);
    } else {
      ctx.present(backbuffer, presenter, vsync_duration, state.metadata);
    }
    // Advance the flip chain's backbuffer CONTENT one slot, after this present has
    // read slot 0 (through the resolve for an MSAA chain, or directly for a single-
    // sample one): new[i] = old[(i+1) % N], the presented slot 0 wrapping to the
    // last slot. Matched-sample-count blit-encoder copies through the persistent
    // scratch, so no surface backing moves: scratch <- slot 0 preserves the just-
    // presented content, slot i <- slot i+1 shifts the rest down, slot N-1 <-
    // scratch closes the ring. copyTexture's access<> tracking serialises the ring
    // the same way the MSAA-resolve path above orders against the present: the
    // write to slot 0 registers after both the present-path read of slot 0 and the
    // scratch read of slot 0 (write-after-read enumerates prior readers), each
    // later slot's write waits on the prior copy that read it as a source, and the
    // closing copy reads scratch after it was written (read-after-write). Empty (a
    // no-op) on the COPY / single-buffer paths. Same 1:1 full-extent copy the d3d9
    // StretchRect Copy path uses; a matched-count MSAA copy round-trips the
    // multisample backbuffers where a sampler stretch could not source them.
    if (!rotation_textures.empty() && rotation_scratch) {
      const size_t n = rotation_textures.size();
      const WMTOrigin origin{0, 0, 0};
      const WMTSize size{bb_w, bb_h, 1};
      ctx.copyTexture(rotation_textures[0], 0, 0, origin, rotation_scratch, 0, 0, origin, size);
      for (size_t i = 0; i + 1 < n; ++i)
        ctx.copyTexture(rotation_textures[i + 1], 0, 0, origin, rotation_textures[i], 0, 0, origin, size);
      ctx.copyTexture(rotation_scratch, 0, 0, origin, rotation_textures[n - 1], 0, 0, origin, size);
    }
  });
  // Backbuffer CONTENT now rotates via the present-chunk blits above (flip chain
  // new[i] = old[(i+1) % N]); the surface backings stay immutable so the encode-
  // thread RT0 resolve and any backbuffer readback keep one stable backing per
  // slot. GetBackBuffer(i) hands back a fixed surface object whose contents
  // advance each frame, matching native's flip chain.
  // Per-cmdbuf tail signal. There is no per-FlushDrawBatch signalEvent (it
  // would block the dxmt_context encoder-list coalescer), so the ring-recycle
  // event must still be advanced once per cmdbuf. The
  // Present chunk is the natural end of a frame's cmdbuf; sync paths
  // (UpdateTexture / GetRenderTargetData) still emit their own signal.
  m_device->emitCmdbufTailSignal();
  m_device->commitCurrentChunkTimed();
  // Charge this thread's CPU to the closing frame before the boundary rolls the
  // ring slot; after PresentBoundary the sink is the next frame.
  m_device->recordPresentThreadCounters();
  queue.PresentBoundary();
  // Throttle the calling thread to the frame-latency depth. Present pacing was
  // split out of PresentBoundary so each back end applies its own; without this
  // the calling thread only stalls on the far deeper command-chunk ring
  // backpressure and can outrun the GPU by many frames, inflating input latency
  // and pinning that many frames of in-flight resources. The chunk stamped
  // above signals the frame-latency fence when it retires.
  queue.WaitFrameLatency(frame_latency_seq);

  m_presentationCount++;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetFrontBufferData(IDirect3DSurface9 *pDestSurface) {
  D9DeviceLock lock = m_device->LockDevice();
  // A lost non-Ex device fails the readback with DEVICELOST (DXVK
  // d3d9_swapchain.cpp GetFrontBufferData); Ex devices never enter Lost.
  if (!m_isEx && m_device->isDeviceLost())
    return D3DERR_DEVICELOST;
  // The device helper owns the whole readback (window-offset placement,
  // X8 -> A8 conversion, intersection copy) against THIS chain's
  // backbuffer, so an additional chain reads its own frame rather
  // than the implicit one's (the DXVK per-chain shape).
  if (m_backBuffers.empty())
    return D3DERR_DRIVERINTERNALERROR;
  return m_device->frontBufferReadback(this, pDestSurface);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) {
  D9DeviceLock lock = m_device->LockDevice();
  // Type is ignored by native (wine dlls/d3d9 swapchain.c: "backbuffer_type is
  // ignored by native"). LEFT/RIGHT exist in the spec for stereo but no
  // real driver implements it; apps probing LEFT for 3D-vision capability
  // expect the MONO surface back, not INVALIDCALL.
  (void)Type;
  // The swapchain method leaves the out-pointer UNMODIFIED on the
  // out-of-range failure path (wine dlls/d3d9 swapchain.c): clearing it is the
  // device-level wrapper's job, which NULLs up front before forwarding.
  if (!ppBackBuffer)
    return D3DERR_INVALIDCALL;
  if (iBackBuffer >= m_backBuffers.size())
    return D3DERR_INVALIDCALL;
  // Every slot's surface object is fixed for the chain's life; Present rotates
  // the backbuffer CONTENT one slot each frame (new[i] = old[(i+1) % N]; see
  // Present), so a slot the app renders into reaches the screen once the presents
  // that follow shift its content down to slot 0, the way a native flip chain
  // does. DISCARD leaves post-Present contents undefined (the common default),
  // but the object identity GetBackBuffer(i) returns is still stable.
  *ppBackBuffer = ::dxmt::ref(static_cast<IDirect3DSurface9 *>(m_backBuffers[iBackBuffer].ptr()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) {
  D9DeviceLock lock = m_device->LockDevice();
  // Chokepoint for raster polling: the device-level GetRasterStatus forwards
  // here, so counting only here attributes each app call once (counting both
  // would double every device-routed call).
  dxmt::perf::addFrameCounter(m_device->frameStats(), &dxmt::FrameStatistics::frame_raster_status_poll_count);
  if (!pRasterStatus)
    return D3DERR_INVALIDCALL;
  // Apple Silicon has no software-readable raster pointer (D3DKMTGetScanLine
  // is Win32-only and Wine doesn't implement it either). DXVK's
  // D3D9SwapChainEx::GetRasterStatus (d3d9_swapchain.cpp)
  // synthesizes a plausible scanline from the refresh rate and the
  // current monotonic time; enough for older games that sync animation
  // / frame-pacing to scanline progress, which a static value strands.
  constexpr uint32_t vblank_line_count = 20; // DXVK constant
  // Scanline range spans the display-mode height, not the backbuffer: in
  // windowed mode the two differ, and DXVK paces GetRasterStatus off the
  // display mode (d3d9_swapchain.cpp, mode.Height). Fall back to the backbuffer
  // if the adapter mode query fails.
  D3DDISPLAYMODE dm{};
  const uint32_t mode_h = SUCCEEDED(m_device->GetDisplayMode(0, &dm)) ? dm.Height : 0u;
  const uint32_t height = mode_h ? mode_h : (m_params.BackBufferHeight ? m_params.BackBufferHeight : 1);
  const double refresh = m_refreshRateHz > 0.0 ? m_refreshRateHz : 60.0;
  const uint32_t scanline_count = height + vblank_line_count;
  const double frame_us = 1'000'000.0 / refresh;
  const double scanline_us = frame_us / scanline_count;
  const auto now_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count();
  const double now_in_frame_us = std::fmod(static_cast<double>(now_us), frame_us);
  uint32_t scan_line = static_cast<uint32_t>(now_in_frame_us / scanline_us);
  const BOOL in_vblank = (scan_line >= height) ? TRUE : FALSE;
  pRasterStatus->InVBlank = in_vblank;
  pRasterStatus->ScanLine = in_vblank ? 0u : scan_line;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetDisplayMode(D3DDISPLAYMODE *pMode) {
  D9DeviceLock lock = m_device->LockDevice();
  // wined3d (swapchain.c -> wined3d_output_get_display_mode) and DXVK
  // (d3d9_swapchain.cpp) both report the monitor's current mode, windowed and
  // fullscreen alike, so there is no fork here.
  if (!pMode)
    return D3DERR_INVALIDCALL;
  return m_device->GetDisplayMode(0, pMode);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *pParameters) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!pParameters)
    return D3DERR_INVALIDCALL;
  *pParameters = m_params;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetLastPresentCount(UINT *pLastPresentCount) {
  D9DeviceLock lock = m_device->LockDevice();
  // Reject a null out-pointer with INVALIDCALL (the native contract). DXVK is
  // lenient here (returns D3D_OK and writes nothing); the strict form lets
  // hr-strict apps distinguish "no buffer passed" from "no presents yet"
  // (both otherwise end at *value=0 in their local).
  if (!pLastPresentCount)
    return D3DERR_INVALIDCALL;
  *pLastPresentCount = m_presentationCount;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) {
  D9DeviceLock lock = m_device->LockDevice();
  // Same null-pointer rejection as GetLastPresentCount (native contract; DXVK
  // is lenient and returns D3D_OK).
  if (!pPresentationStatistics)
    return D3DERR_INVALIDCALL;
  *pPresentationStatistics = D3DPRESENTSTATS{};
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  D9DeviceLock lock = m_device->LockDevice();
  return m_device->GetDisplayModeEx(0, pMode, pRotation);
}

void
MTLD3D9SwapChain::SetGammaRampForChain(DWORD Flags, const D3DGAMMARAMP *pRamp) {
  (void)Flags; // D3DSGR_NO_CALIBRATION / _CALIBRATE: color-management hints,
               // no Metal-side knob, ignored same as wined3d's gamma path.
  if (!pRamp)
    return;
  // Detect a redundant identical re-set before overwriting the stored ramp.
  // Some games call SetGammaRamp every frame with the same WORD ramp (a
  // brightness slider applied once but re-pushed each frame). Forwarding an
  // unchanged ramp bumps the version and forces the presenter to rebuild its
  // present pipeline every frame: a present-path shader compile plus
  // driver-side PSO accumulation. Skip the version bump and the presenter
  // forward when the ramp is byte-identical to the one already in flight.
  const bool ramp_changed = !m_gammaSet || std::memcmp(&m_gammaRamp, pRamp, sizeof(D3DGAMMARAMP)) != 0;
  // Stash the WORD ramp for byte-identical GetGammaRamp round-trip; some
  // apps (calibration tools) re-read the ramp they just set and compare
  // bit-exact. Synthesizing from the float[1024] LUT would re-quantize.
  //
  // Store ALWAYS, even when m_presenter is null and even on an identical
  // re-set; apps that hr-probe gamma readback as a capability test expect
  // Get-after-Set to round-trip whether or not the chain has a live drawable.
  // The presenter LUT push below is gated on an actual change and m_presenter;
  // the WORD storage is unconditional.
  m_gammaRamp = *pRamp;
  m_gammaSet = true;
  if (!ramp_changed)
    return;
  // Push the stored ramp to the presenter. reapplyGammaRamp no-ops on a
  // headless chain (null presenter); the WORD storage above is unconditional so
  // GetGammaRamp still round-trips. ResetForDeviceReset reuses the same path to
  // re-push the stored ramp after a presenter rebuild.
  reapplyGammaRamp();
}

void
MTLD3D9SwapChain::reapplyGammaRamp() {
  if (!m_gammaSet || m_presenter == nullptr)
    return;
  // Gamma is a fullscreen-exclusive display ramp (D3DCAPS2_FULLSCREENGAMMA is
  // the only gamma cap dxmt advertises), and an identity ramp is a no-op. When
  // windowed or identity, clear the presenter LUT so the present blit samples no
  // gamma, matching DXVK's setGammaRamp(0, nullptr) (d3d9_swapchain.cpp)
  // and native (a windowed brightness slider is a no-op). changeGammaRamp(null)
  // no-ops when the presenter already runs LUT-free, so a Reset that stays
  // windowed does not churn the present PSO.
  if (!ShouldApplyGammaRamp(m_gammaRamp, m_params.Windowed)) {
    m_presenter->changeGammaRamp(nullptr);
    return;
  }
  // Upsample 256 -> 1024 with linear interpolation between successive
  // WORD entries. WORD is uint16 in [0, 65535]; the LUT is float in
  // [0, 1]. The 4x oversampling lets the Metal sampler do bilinear
  // interpolation between control points without visible banding. Last
  // 3 samples replicate entry 255 to avoid running off the source
  // array (wrap would map black to bright). Sourced from the stored
  // m_gammaRamp so a re-push after a presenter rebuild uses the app's ramp.
  DXMTGammaRamp gpu_ramp{};
  for (uint32_t lut_i = 0; lut_i < DXMT_GAMMA_CP_COUNT; ++lut_i) {
    uint32_t src_lo = lut_i / 4;
    uint32_t src_hi = src_lo + 1;
    if (src_hi > 255)
      src_hi = 255;
    float t = float(lut_i % 4) / 4.0f;
    auto lerp = [t, src_lo, src_hi](const WORD *ch) {
      float a = float(ch[src_lo]) / 65535.0f;
      float b = float(ch[src_hi]) / 65535.0f;
      return a + (b - a) * t;
    };
    gpu_ramp.red[lut_i] = lerp(m_gammaRamp.red);
    gpu_ramp.green[lut_i] = lerp(m_gammaRamp.green);
    gpu_ramp.blue[lut_i] = lerp(m_gammaRamp.blue);
  }
  gpu_ramp.version = ++m_gammaVersion;
  m_presenter->changeGammaRamp(&gpu_ramp);
}

void
MTLD3D9SwapChain::GetGammaRampForChain(D3DGAMMARAMP *pRamp) {
  if (!pRamp)
    return;
  if (m_gammaSet) {
    *pRamp = m_gammaRamp;
    return;
  }
  // Identity ramp, matching MTLD3D9Device::GetGammaRamp, so a Get before any
  // Set still round-trips a sensible value. Shares IdentityGammaEntry with the
  // identity-Set detection so the two can never disagree.
  SynthesizeIdentityGammaRamp(*pRamp);
}

} // namespace dxmt
