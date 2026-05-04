#include "d3d9_swapchain.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include "log/log.hpp"
#include "util_string.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_interface.hpp"
#include "d3d9_surface.hpp"
#include "d3d9_trace.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "wsi_monitor.hpp"
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
// cross-build. Shim for build parity — values match wine's d3d9.h.
#ifndef S_PRESENT_OCCLUDED
#define S_PRESENT_OCCLUDED ((HRESULT)0x08760878L)
#endif
#ifndef S_PRESENT_MODE_CHANGED
#define S_PRESENT_MODE_CHANGED ((HRESULT)0x08760879L)
#endif

namespace dxmt {

MTLD3D9SwapChain::MTLD3D9SwapChain(
    MTLD3D9Device *device, bool isEx, const D3DPRESENT_PARAMETERS &params, HWND hEffectiveWindow
) :
    m_device(device),
    m_isEx(isEx),
    m_params(params) {
  // CanonicalisePresentParams in d3d9_interface.cpp guarantees the
  // params are concrete and the format lowers to a valid MTLPixelFormat.
  if (!buildBackBuffer()) {
    // Backbuffer allocation failure leaves m_backBuffer null; the
    // chain's GetBackBuffer paths return INVALIDCALL, present is a
    // no-op flush — same outcome as the prior null-texture branch.
    return;
  }

  // CAMetalLayer setup. CreateMetalViewFromHWND returns null when the
  // HWND can't be resolved to an NSView (null HWND, off-screen-only
  // HWND, etc.); in that case the chain stays in headless mode and
  // Present becomes a no-op flush. Real apps always reach this with a
  // valid window.
  m_hWindow = hEffectiveWindow;
  if (hEffectiveWindow != nullptr) {
#ifdef _WIN32
    // Read the display refresh rate the window currently lives on.
    // Matches d3d11_swapchain.cpp:191's shape. On failure we keep the
    // 60.0 default — wrong-by-a-frame at 120Hz beats wrong-by-N-frames
    // for apps that request INTERVAL_TWO/THREE/FOUR.
    HMONITOR mon = MonitorFromWindow(hEffectiveWindow, MONITOR_DEFAULTTOPRIMARY);
    wsi::WsiMode mode{};
    if (mon && wsi::getCurrentDisplayMode(mon, &mode) && mode.refreshRate.denominator != 0 &&
        mode.refreshRate.numerator != 0) {
      m_refreshRateHz =
          static_cast<double>(mode.refreshRate.numerator) / static_cast<double>(mode.refreshRate.denominator);
    }
#endif
    m_view =
        WMT::CreateMetalViewFromHWND(reinterpret_cast<intptr_t>(hEffectiveWindow), m_device->metalDevice(), m_layer);
    if (m_layer.handle != 0) {
      // The Presenter's ctor reads the layer's current props (display_sync,
      // contents_scale, framebuffer_only) and stamps in the device handle,
      // opaque, and framebuffer_only=false (the present-blit needs the
      // drawable as a render target — see project_d3d9_swapchain_framebufferonly).
      // changeLayerProperties below then sets pixel_format / colorspace /
      // drawable size and triggers a deferred PSO build at the first Present.
      m_presenter = Rc(new Presenter(
          m_device->metalDevice(), m_layer, m_device->internalCommandLibrary(),
          /*scale_factor=*/1.0f, /*sample_count=*/1
      ));
      // Drawable size = NSView's natural pixel size, which Presenter's
      // ctor has already cached via getProps (CreateMetalViewFromHWND
      // sized the CAMetalLayer to view.bounds * contentsScale). When
      // BackBufferWidth/Height differ — pre-modern games hardcode
      // 1024×768 etc. — the Presenter's present_scale_ PSO does an
      // aspect-correct fit-to-drawable blit with black letterbox bars.
      // d3d11 currently uses backbuffer-size here (d3d11_swapchain.cpp
      // ApplyLayerProps); d3d9 forks because the legacy-resolution
      // problem only really hits dx9 apps.
      WMTLayerProps current{};
      m_layer.getProps(current);
      double drawable_w = current.drawable_width > 0 ? current.drawable_width : (double)m_params.BackBufferWidth;
      double drawable_h = current.drawable_height > 0 ? current.drawable_height : (double)m_params.BackBufferHeight;
      m_presenter->changeLayerProperties(
          D3DFormatToMetal(m_params.BackBufferFormat, D3D9FormatUsage::RenderTarget), WMTColorSpaceSRGB, drawable_w,
          drawable_h,
          /*sample_count=*/1
      );
      // CAMetalLayer's default maximumDrawableCount is 3, which is too
      // deep for 60Hz games: presents pile up in the queue and Present
      // Delay stretches to 2-3 vsync intervals (HUD on NFS:MW: 31ms avg,
      // 49ms max — i.e., the GPU is consistently working 2 frames
      // behind the displayed frame). 2 is the standard for low-latency
      // games on Apple Silicon and matches MoltenVK's default. Apps
      // requesting D3DPRESENT_INTERVAL_IMMEDIATE still get the same
      // pacing because nextDrawable is gated by the layer's own queue,
      // not by the present-after-minimum-duration hint.
      WMTLayerProps drawable_cap{};
      m_layer.getProps(drawable_cap);
      drawable_cap.maximum_drawable_count = 2;
      m_layer.setProps(drawable_cap);
    }
  }

  // Frame-latency clamp per DXVK d3d9_swapchain.cpp:1145
  // (GetActualFrameLatency): the queue's PresentBoundary wait is
  // min(device->m_frameLatency, BackBufferCount + 1). Pre-port,
  // dxmt-d3d9 left the queue at the hardcoded max_latency_=3 default
  // regardless of m_frameLatency or BackBufferCount, so NFS:MW (which
  // doesn't call SetMaximumFrameLatency and has BackBufferCount=1)
  // ran at frame-latency 3 instead of the DXVK-equivalent 2. HUD
  // libMTLHud_speed_2026_05_16_16_18_07 showed Present Delay avg
  // 30.24ms ≈ 2 vsync intervals at 60Hz; tighter pacing should drop
  // it to ~1 vsync without losing throughput (per-frame work is
  // 5.7ms GPU + 2.9ms CPU avg, well under the 16.67ms budget).
  UINT actual = std::min(m_device->getFrameLatency(), m_params.BackBufferCount + 1u);
  m_device->dxmtQueue().SetMaxLatency(actual);
}

bool
MTLD3D9SwapChain::buildBackBuffer() {
  m_backBuffers.clear();
  const UINT count = std::max<UINT>(1u, m_params.BackBufferCount);

  WMTTextureInfo info{};
  info.pixel_format = D3DFormatToMetal(m_params.BackBufferFormat, D3D9FormatUsage::RenderTarget);
  info.width = m_params.BackBufferWidth;
  info.height = m_params.BackBufferHeight;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = static_cast<WMTTextureUsage>(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  // PixelFormatView for D3DRS_SRGBWRITEENABLE — the back buffer is the
  // most common RT and games often toggle sRGB-write per draw on it.
  if (Recall_sRGB(info.pixel_format) != info.pixel_format)
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
  info.options = WMTResourceStorageModePrivate;

  D3DSURFACE_DESC desc{};
  desc.Format = m_params.BackBufferFormat;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = m_params.MultiSampleType;
  desc.MultiSampleQuality = m_params.MultiSampleQuality;
  desc.Width = m_params.BackBufferWidth;
  desc.Height = m_params.BackBufferHeight;

  // Allocate BackBufferCount surfaces, each over its own dxmt::Texture
  // allocation. Slot 0 is the one the device auto-binds to RT0 and the
  // Presenter blits at Present time. Slots 1..N-1 exist to satisfy
  // GetBackBuffer(i) — apps occasionally fetch additional slots for
  // their own pipelining, even though Metal's CAMetalLayer is what
  // does the actual in-flight buffering on this backend.
  m_backBuffers.reserve(count);
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
    auto *bb = new MTLD3D9Surface(
        m_device, desc, static_cast<IDirect3DSwapChain9 *>(this), WMT::Reference<WMT::Texture>(rawTex),
        /*mipLevel=*/0,
        /*selfPin=*/false,
        /*parentTextureType=*/WMTTextureType2D,
        /*buffer=*/{},
        /*cpuPtr=*/nullptr,
        /*pitch=*/0,
        /*arraySlice=*/0,
        /*ownedBacking=*/nullptr,
        /*dxmtTexture=*/std::move(dxmt_bb_texture)
    );
    m_backBuffers.emplace_back(bb);
  }
  return true;
}

HRESULT
MTLD3D9SwapChain::ResetForDeviceReset(const D3DPRESENT_PARAMETERS &params) {
  // Caller (MTLD3D9Device::Reset) is responsible for draining the GPU
  // before we re-back the backbuffer surfaces.
  //
  // Identity-preserving rebuild — wined3d swapchain.c shape and DXVK
  // D3D9SwapChainEx::Reset both reuse the MTLD3D9Surface objects and
  // just swap their Metal backing. Apps that called GetBackBuffer
  // before Reset and held the IDirect3DSurface9* get the *new*
  // backbuffer contents after Reset, not a stale frozen snapshot
  // pointing at the destroyed pre-Reset texture. The old "clear +
  // buildBackBuffer" path was documented as out-of-spec for exactly
  // this reason — fixed here.
  //
  // Allocate the new Metal textures first; only swap them in once
  // every allocation succeeded. If any one fails the chain stays on
  // the previous backing and the caller gets DEVICELOST. Read from
  // `params` until success — mutating m_params before allocation
  // would leave the chain in a half-update state on OOM, with the
  // app seeing the new dimensions from GetPresentParameters but
  // the actual backbuffer still at the old size.
  const UINT new_count = std::max<UINT>(1u, params.BackBufferCount);
  WMTTextureInfo info{};
  info.pixel_format = D3DFormatToMetal(params.BackBufferFormat, D3D9FormatUsage::RenderTarget);
  info.width = params.BackBufferWidth;
  info.height = params.BackBufferHeight;
  info.depth = 1;
  info.array_length = 1;
  info.type = WMTTextureType2D;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = static_cast<WMTTextureUsage>(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  if (Recall_sRGB(info.pixel_format) != info.pixel_format)
    info.usage = (WMTTextureUsage)(info.usage | WMTTextureUsagePixelFormatView);
  info.options = WMTResourceStorageModePrivate;

  D3DSURFACE_DESC desc{};
  desc.Format = params.BackBufferFormat;
  desc.Type = D3DRTYPE_SURFACE;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.MultiSampleType = params.MultiSampleType;
  desc.MultiSampleQuality = params.MultiSampleQuality;
  desc.Width = params.BackBufferWidth;
  desc.Height = params.BackBufferHeight;

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
    new_dxmt_textures.push_back(std::move(dxmt_bb));
  }

  // All allocations succeeded — commit the new params and rebind.
  m_params = params;

  // BackBufferCount shrunk: trim trailing slots. Surfaces with an
  // outstanding app-held public ref get destroyed when the app drops
  // the ref; identity is only preserved for the slots that still
  // exist on both sides of the Reset.
  if (new_count < m_backBuffers.size())
    m_backBuffers.resize(new_count);

  // Slots 0..min(old, new)-1: keep the same MTLD3D9Surface object and
  // swap its inner texture/format/desc in place.
  const UINT keep = std::min<UINT>(static_cast<UINT>(m_backBuffers.size()), new_count);
  for (UINT i = 0; i < keep; ++i) {
    m_backBuffers[i]->resetBacking(
        desc, WMT::Reference<WMT::Texture>(new_raw_textures[i]), std::move(new_dxmt_textures[i]), WMTTextureType2D
    );
  }
  // BackBufferCount grew: append fresh surfaces for the new tail.
  for (UINT i = keep; i < new_count; ++i) {
    auto *bb = new MTLD3D9Surface(
        m_device, desc, static_cast<IDirect3DSwapChain9 *>(this), WMT::Reference<WMT::Texture>(new_raw_textures[i]),
        /*mipLevel=*/0,
        /*selfPin=*/false,
        /*parentTextureType=*/WMTTextureType2D,
        /*buffer=*/{},
        /*cpuPtr=*/nullptr,
        /*pitch=*/0,
        /*arraySlice=*/0,
        /*ownedBacking=*/nullptr,
        /*dxmtTexture=*/std::move(new_dxmt_textures[i])
    );
    m_backBuffers.emplace_back(bb);
  }

  // Update the layer's drawable extent + format to match the new
  // params. The Presenter caches these and rebuilds its present PSO
  // on the next Present if any input changed.
  if (m_presenter != nullptr && m_layer.handle != 0) {
    WMTLayerProps current{};
    m_layer.getProps(current);
    double drawable_w = current.drawable_width > 0 ? current.drawable_width : (double)m_params.BackBufferWidth;
    double drawable_h = current.drawable_height > 0 ? current.drawable_height : (double)m_params.BackBufferHeight;
    m_presenter->changeLayerProperties(
        D3DFormatToMetal(m_params.BackBufferFormat, D3D9FormatUsage::RenderTarget), WMTColorSpaceSRGB, drawable_w,
        drawable_h, /*sample_count=*/1
    );
  }
  return D3D_OK;
}

MTLD3D9SwapChain::~MTLD3D9SwapChain() {
  // Drop the Presenter before tearing down the NSView — the Presenter
  // holds a non-retaining WMT::MetalLayer copy whose backing CALayer is
  // owned by the view, plus PSOs / a gamma-LUT texture that route their
  // dispose through the device. The Rc<>'s destructor would run as part
  // of normal field destruction, but ReleaseMetalView happens explicitly
  // above any field destruction, so we sequence it here.
  m_presenter = nullptr;
  if (m_view.handle != 0) {
    auto pool = WMT::MakeAutoreleasePool();
    WMT::ReleaseMetalView(m_view);
  }
}

ULONG STDMETHODCALLTYPE
MTLD3D9SwapChain::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9SwapChain::Release() {
  // The device holds an AddRefPrivate on us, so private refcount stays
  // at 1 across this call and ComObject::Release will not delete the
  // chain — `this` is safe to dereference for m_device->Release. The
  // chain only ever destructs from the device dtor's ReleasePrivate.
  ULONG ref = ComObject::Release();
  if (ref == 0)
    m_device->Release();
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::QueryInterface(REFIID riid, void **ppvObject) {
  D9_TRACE("IDirect3DSwapChain9::QueryInterface");
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
  D9_TRACE("IDirect3DSwapChain9::Present");
  (void)pDirtyRegion; // hint only, spec-permitted to ignore
  // Lost-device gate. MSDN IDirect3DDevice9::Present return codes:
  //   D3D_OK             — happy path.
  //   D3DERR_DEVICELOST  — device lost; app must call TestCooperativeLevel
  //                        and Reset.
  //   D3DERR_DRIVERINTERNALERROR — driver error; app should release the
  //                        device and re-create.
  //   D3DERR_INVALIDCALL — caller bug (only via PresentEx's dwFlags
  //                        bad-combination cases per Wine's commentary;
  //                        Microsoft docs don't explicitly list it here).
  //
  // Wine wired in two reference deltas:
  //   1. For Direct3DDevice9Ex (`d3d_parent->extended`), the device-lost
  //      gate returns S_PRESENT_OCCLUDED instead of D3DERR_DEVICELOST when
  //      the device's render target is not visible (occluded). The non-Ex
  //      path still returns D3DERR_DEVICELOST. wine/dlls/d3d9/device.c:1336
  //      and swapchain.c:147 enforce this.
  //   2. DXVK (d3d9_device.cpp:586 + d3d9_swapchain.cpp:116) returns
  //      D3DERR_DEVICELOST unconditionally — no Ex distinction. Wine's
  //      shape is the strict-spec one; we follow it.
  //
  // `presentStateGate()` currently returns D3D_OK unconditionally — the
  // state machine is audit G1; once that lands it must honor the Ex vs
  // non-Ex distinction above. Cross-ref m_isEx on the device when wiring
  // the state machine.
  if (HRESULT state = m_device->presentStateGate(); state != D3D_OK)
    return state;
  // Foreground-lost probe. d3d11 has the same shape at
  // d3d11_swapchain.cpp:854-862. For Ex devices the spec return is
  // S_PRESENT_OCCLUDED; for non-Ex devices MSDN's "Lost Devices" page
  // says the DEVICELOST transition is driven by focus loss, but native
  // doesn't synthesize DEVICELOST on a simple minimize — it just stops
  // updating the front buffer. We follow that: minimized + Ex returns
  // S_PRESENT_OCCLUDED; minimized + non-Ex returns D3D_OK after the
  // draw queue is drained (so resource lifetimes stay coherent across
  // the no-display window). Skip if no hWnd (headless smokes).
  if (m_hWindow && wsi::isMinimized(m_hWindow)) {
    m_device->FlushDrawBatch();
    m_device->flushOpenWork();
    return m_isEx ? S_PRESENT_OCCLUDED : D3D_OK;
  }
  // No in-scene gate: neither wined3d (swapchain.c:136-155) nor DXVK
  // (d3d9_swapchain.cpp:106-201) rejects Present between BeginScene /
  // EndScene. Real apps Present mid-scene (some engines call
  // BeginScene at frame start, Present in the middle, EndScene at
  // the next frame); the previously-cited spec contract was
  // descriptive, not a runtime gate. Driver behavior is to accept.
  //
  // pSourceRect / pDestRect: passed through regardless of SwapEffect.
  // Both references accept them on all SwapEffect values (wined3d
  // wined3d_swapchain_present, DXVK d3d9_swapchain.cpp:1325-1362).
  // dxmt currently doesn't honor the rects (partial-region present
  // needs Presenter API extensions); accepting + ignoring matches
  // pre-Vista driver behavior more closely than INVALIDCALL.
  // hDestWindowOverride: present to an HWND other than hDeviceWindow.
  // dxmt's Presenter is bound to a single CAMetalLayer at swapchain
  // create time and has no current path to retarget mid-frame.
  // Returning INVALIDCALL would break the common case where apps pass
  // their main hWnd as the override "just to be safe"; ignoring with
  // a one-shot warn matches DXVK's approach until a proper Presenter
  // retarget lands.
  if (hDestWindowOverride != nullptr) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      Logger::warn("d3d9: Present hDestWindowOverride is currently ignored");
  }
  // D3DPRESENT_FORCEIMMEDIATE is documented to be legal only when the
  // swapchain was created with D3DSWAPEFFECT_FLIPEX; "improperly
  // specified" is INVALIDCALL per the D3DPRESENT remarks table. Apps
  // probe-and-fall-back on this — they call Present with the flag,
  // observe INVALIDCALL, and then drop the flag for subsequent frames.
  if ((dwFlags & D3DPRESENT_FORCEIMMEDIATE) && m_params.SwapEffect != D3DSWAPEFFECT_FLIPEX)
    return D3DERR_INVALIDCALL;
  // D3DPRESENT_DONOTWAIT: MSDN PresentEx parameter table item 3 — if
  // the GPU is still busy with the previous frame, return
  // D3DERR_WASSTILLDRAWING instead of blocking. The next PresentBoundary
  // would block on frame_latency_fence_.wait(CurrentFrameSeq() -
  // max_latency_); peek that value here and short-circuit if it hasn't
  // retired yet. Neither wined3d (swapchain.c:225-226 FIXMEs flags) nor
  // DXVK honors this — we're strict-spec at trivial cost.
  if (dwFlags & D3DPRESENT_DONOTWAIT) {
    auto &queue = m_device->dxmtQueue();
    uint32_t max_latency = queue.GetMaxLatency();
    uint64_t next_seq = queue.CurrentFrameSeq();
    if (next_seq > max_latency && queue.FrameLatencySignaled() < next_seq - max_latency)
      return D3DERR_WASSTILLDRAWING;
  }

  // Diagnostic: skip ALL Metal-side Present work when DXMT_D9_SKIP_PRESENT
  // is set. Pairs with the audio/input-responsiveness bisect — if the cap
  // lifts here, the cause is somewhere downstream (drawable acquire,
  // commit, encoder build). Drawn frame is silently dropped (black screen).
  static const bool skip_present = std::getenv("DXMT_D9_SKIP_PRESENT") != nullptr;
  if (skip_present) {
    D9_HOT_BUMP(presentCalls);
    return D3D_OK;
  }
  // Headless chain (no layer): no-op success. Existing per-call cmdbufs
  // commit immediately, so there's nothing to flush. Real apps reach
  // here with m_layer non-null.
  // pSourceRect / pDestRect honour-under-COPY is still a TODO (the
  // Presenter would need a sub-region blit path); apps that pass them
  // under COPY today get a full-frame blit until that lands.
  (void)pSourceRect;
  (void)pDestRect;
  D9_HOT_BUMP(presentCalls);
  auto t_present_start = std::chrono::steady_clock::now();
  // interPresent: gap between this Present's entry and the previous
  // Present's return. Skips the first call (sentinel time_point ==
  // epoch) so process startup doesn't get billed as a Present gap.
  if (m_lastPresentReturn.time_since_epoch().count() != 0) {
    uint64_t gap_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t_present_start - m_lastPresentReturn).count()
    );
    D9_HOT_ADD(interPresentMicros, gap_us);
    // Frame-scale stutter detection. interPresent is the work the
    // game's render thread did between Present return and the next
    // Present entry — at 60 fps that's ~16 ms steady-state. Anything
    // > 30 ms means we missed at least one vsync; > 100 ms is a
    // visible stutter. Per-second aggregates hide both, so we record
    // max and bucket counts separately. Atomic CAS keeps the racy
    // max-store correct under multiple swapchains (rare).
    if (D3D9HotCounters::enabled()) {
      auto &c = D3D9HotCounters::instance();
      uint64_t prev_max = c.maxFrameMicros.load(std::memory_order_relaxed);
      while (gap_us > prev_max &&
             !c.maxFrameMicros.compare_exchange_weak(prev_max, gap_us, std::memory_order_relaxed)) {
      }
      if (gap_us > 30000)
        c.slowFrames30ms.fetch_add(1, std::memory_order_relaxed);
      if (gap_us > 100000)
        c.slowFrames100ms.fetch_add(1, std::memory_order_relaxed);
    }
  }
  auto pool = WMT::MakeAutoreleasePool();
  // Phase 3.6: Present routes through a chunk so the present-blit's
  // wine_unix_calls (cmdbuf.commit, presentDrawable, encodeCommands)
  // run on the dxmt encode thread instead of the wow64 main thread.
  // Drain queued batched draws onto a chunk first so the Present chunk
  // observes them through Metal queue ordering. flushOpenWork() catches
  // any residual sync cmdbuf work.
  auto t_flush_start = std::chrono::steady_clock::now();
  m_device->FlushDrawBatch();
  m_device->flushOpenWork();
  auto t_flush_end = std::chrono::steady_clock::now();
  D9_HOT_ADD(
      flushOpenWorkMicros, std::chrono::duration_cast<std::chrono::microseconds>(t_flush_end - t_flush_start).count()
  );
  if (m_layer.handle == 0 || m_presenter == nullptr) {
    D9_HOT_ADD(
        presentTotalMicros,
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_present_start)
            .count()
    );
    m_lastPresentReturn = std::chrono::steady_clock::now();
    return D3D_OK;
  }

  // synchronizeLayerProperties picks up display-side colorspace / EDR /
  // HDR-metadata changes and (re)builds the present PSO if any input
  // changed. Returns a PresentState whose dtor signals the Presenter's
  // frame_presented_ fence — synchronizeLayerProperties waits on that
  // fence on the next call to detect prior present completion before
  // committing to a PSO rebuild. The state MUST be moved into the
  // chunk lambda (NOT copied out as metadata) so the dtor fires when
  // the lambda is destroyed (post-Metal-retire) rather than when this
  // function returns. d3d11_swapchain.cpp:925-934 is the literal model.
  auto state = m_presenter->synchronizeLayerProperties();

  // d3d11_swapchain.cpp:925-934 is the literal model: chunk->emitcc with
  // ctx.present(backbuffer Rc<>, presenter, vsync_duration, metadata).
  // The chunk runs on the encode thread: ArgumentEncodingContext's
  // PresentData encoder allocates a cmdbuf there, calls
  // presenter->encodeCommands + presentDrawable + commit. Nothing in
  // this Present path now touches a wine_unix_call on the calling
  // thread except the chunk-emit memcpy itself.
  auto &queue = m_device->dxmtQueue();
  // Per-Present re-clamp of queue max latency. The ctor seeds it from
  // min(getFrameLatency(), BackBufferCount + 1u) but the device's
  // SetMaximumFrameLatency pushes the raw value to the queue without
  // re-clamping, so a post-create SetMaximumFrameLatency(8) on a
  // BackBufferCount=1 chain would otherwise let the queue race 8 frames
  // ahead. DXVK recomputes the same clamp every Present
  // (d3d9_swapchain.cpp:1145 GetActualFrameLatency); we mirror that.
  queue.SetMaxLatency(std::min(m_device->getFrameLatency(), m_params.BackBufferCount + 1u));
  auto *chunk = queue.CurrentChunk();
  chunk->signal_frame_latency_fence_ = queue.CurrentFrameSeq();
  // Present has no SyncInterval input on d3d9 (the value sits on
  // m_params.PresentationInterval, captured at swapchain create time).
  // Map IMMEDIATE → 0.0 (no minimum dwell). For the four sync-multiple
  // values, dwell is N * refresh_period using the actual display rate
  // m_refreshRateHz captured at swapchain ctor — matches wined3d / DXVK.
  // D3DPRESENT_FORCEIMMEDIATE is a per-Present override that drops the
  // sync interval to 0 for this frame regardless of the swapchain's
  // configured PresentationInterval (DXVK d3d9_swapchain.cpp:134-135).
  // Apps toggle this between menus / gameplay / cutscenes without
  // recreating the swapchain.
  double vsync_duration;
  const DWORD pi = m_params.PresentationInterval;
  if ((pi & D3DPRESENT_INTERVAL_IMMEDIATE) || (dwFlags & D3DPRESENT_FORCEIMMEDIATE)) {
    vsync_duration = 0.0;
  } else {
    int multiplier = 1; // DEFAULT / ONE
    if (pi & D3DPRESENT_INTERVAL_FOUR)
      multiplier = 4;
    else if (pi & D3DPRESENT_INTERVAL_THREE)
      multiplier = 3;
    else if (pi & D3DPRESENT_INTERVAL_TWO)
      multiplier = 2;
    vsync_duration = static_cast<double>(multiplier) / m_refreshRateHz;
  }
  auto bb_dxmt = m_backBuffers[0]->dxmtTexture();
  Rc<Presenter> presenter = m_presenter;
  auto t_encode_start = std::chrono::steady_clock::now();
  chunk->emitcc([backbuffer = std::move(bb_dxmt), presenter = std::move(presenter), vsync_duration,
                 state = std::move(state)](ArgumentEncodingContext &ctx) mutable {
    ctx.present(backbuffer, presenter, vsync_duration, state.metadata);
  });
  // SwapEffect FLIP / FLIPEX / DISCARD: rotate the backing of the
  // implicit chain so the next frame's "slot 0" is what was the
  // last-rendered slot 1, and the just-presented slot 0 cycles to the
  // tail. DXVK D3D9SwapChainEx::PresentImage (d3d9_swapchain.cpp:938-953)
  // is the literal model — pairwise Swap rather than std::rotate so
  // app-held GetBackBuffer(i) pointers stay bound to the same vector
  // slot, with the slot's *contents* (m_texture / m_dxmtTexture /
  // m_viewCache) shifting one position per Present.
  //
  // COPY: contents persist across Present per D3D9 spec ("Present
  // copies the back buffer to the front" rather than swapping pointers).
  // No rotation — m_backBuffers[0] stays the source for both the next
  // frame's draws AND the next Present blit.
  //
  // The Rc<dxmt::Texture> captured into the chunk lambda above retains
  // the just-presented texture across the swap, so the Presenter still
  // blits the right contents even though slot 0 now points elsewhere.
  //
  // INVARIANT: FlushDrawBatch above (line ~453) MUST run before this
  // swap. ResolveBatchedDrawForChunk captures each queued draw's RT
  // backing into a per-draw Rc<dxmt::Texture> by value; the flush
  // forces that capture to happen against the *pre-rotation* slot 0
  // texture. Any future refactor that moves the rotate before the
  // flush would regress to a UAF on queued-but-unresolved draws whose
  // RT0 backing was swapped out from under them.
  if (m_params.SwapEffect != D3DSWAPEFFECT_COPY && m_backBuffers.size() > 1) {
    for (size_t i = 1; i < m_backBuffers.size(); ++i)
      m_backBuffers[i]->SwapBacking(m_backBuffers[i - 1].ptr());
  }
  // Per-cmdbuf tail signal — the per-FlushDrawBatch signalEvents were
  // removed to unblock the dxmt_context encoder-list coalescer, so the
  // ring-recycle event must still get advanced once per cmdbuf. The
  // Present chunk is the natural end of a frame's cmdbuf; sync paths
  // (UpdateTexture / GetRenderTargetData) still emit their own signal.
  m_device->emitCmdbufTailSignal();
  auto t_encode_end = std::chrono::steady_clock::now();
  D9_HOT_ADD(
      presentEncodeMicros, std::chrono::duration_cast<std::chrono::microseconds>(t_encode_end - t_encode_start).count()
  );
  auto t_commit_start = std::chrono::steady_clock::now();
  {
    // T0.3 latency histogram: per-Present chunk-commit distribution
    // (n / mean / max / min per second). presentCmdbufCommitMicros above
    // accumulates the sum; this captures the per-call shape so a one-off
    // long commit stops hiding inside the steady-state mean.
    D9_HOT_SCOPE(cmdbufSubmit);
    queue.CommitCurrentChunk();
  }
  auto t_commit_mid = std::chrono::steady_clock::now();
  queue.PresentBoundary();
  auto t_commit_end = std::chrono::steady_clock::now();
  D9_HOT_ADD(
      presentCmdbufCommitMicros,
      std::chrono::duration_cast<std::chrono::microseconds>(t_commit_mid - t_commit_start).count()
  );
  D9_HOT_ADD(
      presentBoundaryMicros, std::chrono::duration_cast<std::chrono::microseconds>(t_commit_end - t_commit_mid).count()
  );

  uint64_t present_total_us =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_present_start).count();
  D9_HOT_ADD(presentTotalMicros, present_total_us);
  m_presentationCount++;
  m_lastPresentReturn = std::chrono::steady_clock::now();
  // DXMT_D9_DIAG_SWAPCHAIN=1: per-Present breakdown line — mirrors the
  // d3d11_swapchain.cpp:946-968 INFO log. The aggregate hotcounters
  // hide single-frame spikes inside per-second means; this surfaces
  // each Present's encode / commit / boundary split so an intermittent
  // stall stops looking like steady-state. Off by default; opt-in.
  static const bool diag_swapchain = std::getenv("DXMT_D9_DIAG_SWAPCHAIN") != nullptr;
  if (diag_swapchain) {
    uint64_t encode_us = std::chrono::duration_cast<std::chrono::microseconds>(t_encode_end - t_encode_start).count();
    uint64_t commit_us = std::chrono::duration_cast<std::chrono::microseconds>(t_commit_mid - t_commit_start).count();
    uint64_t boundary_us = std::chrono::duration_cast<std::chrono::microseconds>(t_commit_end - t_commit_mid).count();
    Logger::info(
        str::format(
            "d3d9: Present frame=", m_presentationCount, " totalUs=", present_total_us, " encodeUs=", encode_us,
            " commitUs=", commit_us, " boundaryUs=", boundary_us
        )
    );
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetFrontBufferData(IDirect3DSurface9 *pDestSurface) {
  D9_TRACE("IDirect3DSwapChain9::GetFrontBufferData");
  // DXVK d3d9_swapchain.cpp:243-260 notes that "for SWAPEFFECT_COPY and
  // windowed SWAPEFFECT_DISCARD with 1 backbuffer, we just copy the
  // backbuffer data instead" — dxmt's situation is exactly that case:
  // BackBufferCount == 1 means the front buffer IS the most-recently-
  // rendered backbuffer (the Presenter has just blitted it to the
  // drawable). Forward to MTLD3D9Device::GetRenderTargetData with
  // m_backBuffers[0] as the source — that already has the validation
  // gates (pool != DEFAULT, format match, dim match, src MSAA NONE) and
  // the blit infra (copyFromTexture:toBuffer: + sync wait).
  //
  // Multi-back-buffer apps with SwapEffect=FLIP would expect the prior
  // frame's content here once rotation is wired up; current dxmt
  // behavior is that backBuffer()[0] is always the most-recently-
  // rendered slot which functionally matches "front buffer" for the
  // single-back-buffer case. Format-converting copy (DXVK's full path
  // for cross-format readback) is deferred — apps that need a format
  // conversion fall through to GRTD's INVALIDCALL.
  if (!pDestSurface)
    return D3DERR_INVALIDCALL;
  if (m_backBuffers.empty())
    return D3DERR_DRIVERINTERNALERROR;
  return m_device->GetRenderTargetData(static_cast<IDirect3DSurface9 *>(m_backBuffers[0].ptr()), pDestSurface);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) {
  D9_TRACE("IDirect3DSwapChain9::GetBackBuffer");
  // Mirror wined3d swapchain.c d3d9_swapchain_GetBackBuffer (and the
  // same shape MTLD3D9Device::GetSwapChain follows above): validate
  // BEFORE touching the out-pointer. Some game engines plant a
  // sentinel in their out-ptr that they expect to survive an
  // out-of-range probe, so the pre-validation null-clobber the prior
  // code did was observable behaviour, even though the end state on
  // failure (out-ptr == NULL) matched wined3d's internal-failure
  // path.
  if (!ppBackBuffer)
    return D3DERR_INVALIDCALL;
  // Type is ignored by native — wined3d swapchain.c:183 comment is
  // explicit: "backbuffer_type is ignored by native." LEFT/RIGHT exist
  // in the spec for stereo but no real driver implements it; apps that
  // probe LEFT for 3D-vision capability expect the MONO surface back,
  // not INVALIDCALL.
  (void)Type;
  if (iBackBuffer >= m_backBuffers.size())
    return D3DERR_INVALIDCALL;
  *ppBackBuffer = ::dxmt::ref(static_cast<IDirect3DSurface9 *>(m_backBuffers[iBackBuffer].ptr()));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) {
  D9_TRACE("IDirect3DSwapChain9::GetRasterStatus");
  if (!pRasterStatus)
    return D3DERR_INVALIDCALL;
  // Apple Silicon has no software-readable raster pointer (D3DKMTGetScanLine
  // is Win32-only and Wine doesn't implement it either). DXVK's
  // D3D9SwapChainEx::GetRasterStatus (d3d9_swapchain.cpp:499-531)
  // synthesizes a plausible scanline from the refresh rate and the
  // current monotonic time — enough for older games that sync animation
  // / frame-pacing to scanline progress. Earlier dxmt returned
  // InVBlank=FALSE / ScanLine=0 statically, which broke that pattern.
  constexpr uint32_t vblank_line_count = 20; // DXVK constant
  const uint32_t height = m_params.BackBufferHeight ? m_params.BackBufferHeight : 1;
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
  D9_TRACE("IDirect3DSwapChain9::GetDisplayMode");
  // Both wined3d (swapchain.c:223-244 → wined3d_output_get_display_mode)
  // and DXVK (d3d9_swapchain.cpp:594-616) return the monitor's current
  // mode unconditionally — windowed and fullscreen alike. The earlier
  // windowed/fullscreen fork was a dxmt invention and misattributed to
  // DXVK in the inline comment. Forward to the adapter for both cases.
  if (!pMode)
    return D3DERR_INVALIDCALL;
  return m_device->GetDisplayMode(0, pMode);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9_TRACE("IDirect3DSwapChain9::GetDevice");
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS *pParameters) {
  D9_TRACE("IDirect3DSwapChain9::GetPresentParameters");
  if (!pParameters)
    return D3DERR_INVALIDCALL;
  *pParameters = m_params;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetLastPresentCount(UINT *pLastPresentCount) {
  D9_TRACE("IDirect3DSwapChain9Ex::GetLastPresentCount");
  if (pLastPresentCount)
    *pLastPresentCount = m_presentationCount;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) {
  D9_TRACE("IDirect3DSwapChain9Ex::GetPresentStats");
  if (pPresentationStatistics)
    *pPresentationStatistics = D3DPRESENTSTATS{};
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9SwapChain::GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) {
  D9_TRACE("IDirect3DSwapChain9Ex::GetDisplayModeEx");
  return m_device->GetDisplayModeEx(0, pMode, pRotation);
}

void
MTLD3D9SwapChain::SetGammaRampForChain(DWORD Flags, const D3DGAMMARAMP *pRamp) {
  (void)Flags; // D3DSGR_NO_CALIBRATION / _CALIBRATE — color-management hints,
               // no Metal-side knob, ignored same as wined3d's gamma path.
  if (!pRamp)
    return;
  // Stash the WORD ramp for byte-identical GetGammaRamp round-trip; some
  // apps (calibration tools) re-read the ramp they just set and compare
  // bit-exact. Synthesizing from the float[1024] LUT would re-quantize.
  //
  // Store ALWAYS, even when m_presenter is null — apps that hr-probe
  // gamma readback as a capability test (calibration tooling, several
  // launch-time checks) expect Get-after-Set to round-trip whether or
  // not the chain has a live drawable. The presenter-side LUT push
  // below is gated on m_presenter; the WORD storage is unconditional.
  m_gammaRamp = *pRamp;
  m_gammaSet = true;
  if (!m_presenter)
    return;
  // Upsample 256 → 1024 with linear interpolation between successive
  // WORD entries. WORD is uint16 in [0, 65535]; the LUT is float in
  // [0, 1]. The 4x oversampling lets the Metal sampler do bilinear
  // interpolation between control points without visible banding. Last
  // 3 samples replicate entry 255 to avoid running off the source
  // array (wrap would map black to bright).
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
    gpu_ramp.red[lut_i] = lerp(pRamp->red);
    gpu_ramp.green[lut_i] = lerp(pRamp->green);
    gpu_ramp.blue[lut_i] = lerp(pRamp->blue);
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
  // Identity ramp — same shape as MTLD3D9Device::GetGammaRamp returned
  // before this method existed, kept here so Get-before-Set still
  // round-trips a sensible value.
  for (uint32_t i = 0; i < 256; ++i) {
    WORD v = static_cast<WORD>(i * 257);
    pRamp->red[i] = v;
    pRamp->green[i] = v;
    pRamp->blue[i] = v;
  }
}

} // namespace dxmt
