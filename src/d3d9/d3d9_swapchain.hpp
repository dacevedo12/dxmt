#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "rc/util_rc_ptr.hpp"

#include <chrono>
#include <unordered_map>
#include <vector>

namespace dxmt {

class Presenter;
class MTLD3D9Device;
class MTLD3D9Surface;
class Texture;

class MTLD3D9SwapChain final : public ComObject<IDirect3DSwapChain9Ex> {
public:
  // hEffectiveWindow is the hDeviceWindow per spec: params.hDeviceWindow
  // if non-null, else the device's hFocusWindow. The interface layer
  // resolves it before calling us. Null is allowed: the chain simply
  // skips layer creation and Present becomes a no-op (smokes use this
  // path; real apps always have a window).
  MTLD3D9SwapChain(
      MTLD3D9Device *device, bool isEx, const D3DPRESENT_PARAMETERS &params, HWND hEffectiveWindow, bool isImplicit
  );
  ~MTLD3D9SwapChain();

  // Raw accessor used by the device ctor to auto-bind the backbuffer to
  // RT slot 0. Doesn't bump the public refcount: the caller treats the
  // surface as device-owned via the implicit-chain priv pin.
  // With BackBufferCount > 1 the device's RT0 always tracks slot 0 (the
  // draw + present buffer); Present rotates the backbuffer CONTENT one slot
  // afterwards (new[i] = old[(i+1) % N]) via GPU blits, keeping the surface
  // backings immutable. Apps that fetch GetBackBuffer(i>0) get a distinct
  // surface object whose contents advance with the flip chain.
  MTLD3D9Surface *
  backBuffer() const {
    return m_backBuffers.empty() ? nullptr : m_backBuffers[0].ptr();
  }

  // The window the chain blits to, as resolved by the interface layer
  // from hDeviceWindow / hFocusWindow at create time. Null on headless
  // chains. Used by CheckDeviceState to mirror the Present-path
  // occlusion probe when the caller didn't pass an HWND.
  HWND
  hWindow() const {
    return m_hWindow;
  }

  bool
  windowed() const {
    return m_params.Windowed;
  }

  // The presented front image's backing when a COPY front canvas is
  // active (see m_frontCanvas); null when the backbuffer is the front.
  Rc<dxmt::Texture>
  frontCanvas() const {
    return m_frontCanvas;
  }

  // The single-sample twin an MSAA backbuffer resolves into at Present;
  // null on single-sample chains. Front-buffer readback resolves into
  // and reads from it, since the MSAA backbuffer itself cannot feed a
  // texture-to-buffer copy.
  Rc<dxmt::Texture>
  resolveTarget() const {
    return m_resolveTarget;
  }

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;

  HRESULT STDMETHODCALLTYPE Present(
      const RECT *pSourceRect, const RECT *pDestRect, HWND hDestWindowOverride, const RGNDATA *pDirtyRegion,
      DWORD dwFlags
  ) override;
  HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *pDestSurface) override;
  HRESULT STDMETHODCALLTYPE
  GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) override;
  HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) override;
  HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *pMode) override;
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *pParameters) override;
  HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *pLastPresentCount) override;
  HRESULT STDMETHODCALLTYPE GetPresentStats(D3DPRESENTSTATS *pPresentationStatistics) override;
  HRESULT STDMETHODCALLTYPE GetDisplayModeEx(D3DDISPLAYMODEEX *pMode, D3DDISPLAYROTATION *pRotation) override;

  // Internal: drives the device's Reset path. Drops the current
  // backbuffer surface and rebuilds at the new dimensions/format,
  // updates the Presenter's layer properties. Caller must have drained
  // the GPU first. Returns D3DERR_DEVICELOST if the new backbuffer
  // can't be allocated (out-of-memory / invalid format).
  HRESULT ResetForDeviceReset(const D3DPRESENT_PARAMETERS &params, HWND hEffectiveWindow);

  // D3D9 SetGammaRamp / GetGammaRamp targets the swapchain's Presenter
  // gamma_lut_texture_ via Presenter::changeGammaRamp. The ramp is
  // stored per-chain (each fullscreen swapchain has its own gamma) and
  // round-tripped through GetGammaRamp; the Presenter's GammaEnabled
  // function-constant picks up the LUT at the next present.
  void SetGammaRampForChain(DWORD Flags, const D3DGAMMARAMP *pRamp);
  void GetGammaRampForChain(D3DGAMMARAMP *pRamp);

private:
  // Allocates the backbuffer surface from m_params. Used by both the
  // ctor and ResetForDeviceReset. Returns true on success.
  bool buildBackBuffer();
  // Builds the presentation target for hEffectiveWindow: resolves the NSView /
  // CAMetalLayer, seeds m_hWindow / m_lastMonitor / m_refreshRateHz, and stands
  // up the Presenter. Degrades to headless (null layer + presenter) when the
  // window can't be resolved. Shared by the ctor and the window-change branch
  // of ResetForDeviceReset. Reads m_params, which the ctor sets first.
  void createPresentTarget(HWND hEffectiveWindow);
  // Tears the presentation target back down, the Presenter before the view that
  // owns its layer (the Presenter holds a non-retaining WMT::MetalLayer copy).
  // Shared by the dtor and the window-change branch of ResetForDeviceReset. The
  // view release is inline (caller drained the GPU first) unless
  // defer_view_release, which hands it to GPU completion: an app-owned chain
  // torn down while the device is live may still have un-encoded present chunks
  // referencing the view.
  void destroyPresentTarget(bool defer_view_release = false);
  // Re-pushes the stored gamma ramp to the current Presenter (upsampling the
  // WORD ramp to the float LUT). No-op when no ramp was set or the chain is
  // headless. Shared by SetGammaRampForChain and by ResetForDeviceReset, where a
  // rebuilt Presenter otherwise silently drops a non-identity ramp.
  void reapplyGammaRamp();
  // Lifetime contract (wined3d dlls/d3d9/swapchain.c): device holds chain
  // via raw pointer + AddRefPrivate/ReleasePrivate (no Com<> cycle). Public
  // Add/Release pins device; private refcount bumped by device, destroyed by
  // device dtor. Survives GetSwapChain(0); dev->Release() without UAF.
  MTLD3D9Device *m_device;
  const bool m_isEx;
  // Implicit (device-owned) vs additional (app-owned via CreateAdditionalSwapChain).
  // The implicit chain is torn down only after the device drains its queue and
  // does not count toward the losable-resource Reset gate; an additional chain is
  // Released while the device is live (so its view teardown must defer to GPU
  // completion) and counts toward the gate (native fails a non-Ex Reset while an
  // additional chain is alive).
  const bool m_isImplicit;
  D3DPRESENT_PARAMETERS m_params;
  // Implicit backbuffer chain: m_backBuffers[0] = device RT0 = the draw +
  // present buffer. The surface backings stay IMMUTABLE (the encode thread
  // resolves RT0 and any readback to a slot's Metal texture lazily, so moving a
  // backing on the calling thread would race those readers). Present instead
  // rotates the backbuffer CONTENT one slot with ordered GPU blits (flip chain
  // new[i] = old[(i+1) % N]; see Present + m_rotationScratch), so GetBackBuffer(i)
  // hands back a fixed surface object whose contents advance each frame the way a
  // native flip chain does. BackBufferCount == 1 (the DISCARD default) rotates
  // nothing. Surfaces are Com<,false> (priv ref only).
  std::vector<Com<MTLD3D9Surface, false>> m_backBuffers;
  // Single-sample present source for an MSAA backbuffer. D3D9 lets the app
  // render the scene straight to an MSAA backbuffer and auto-resolves it at
  // Present; the CAMetalLayer drawable is single-sample, so Present resolves
  // m_backBuffers[0] into this and presents it (the DXVK/wined3d shape). Null
  // for a single-sample chain, where Present blits the backbuffer directly.
  Rc<dxmt::Texture> m_resolveTarget;
  // Persistent front canvas for D3DSWAPEFFECT_COPY partial presents,
  // materialised by the first Present that passes rects: presents
  // composite into it (full frame or source-to-dest rect) and the
  // presenter blits it, so the area a rect leaves uncovered keeps its
  // previous contents the way the native front buffer does. Null until
  // an app passes rects; dropped on Reset.
  Rc<dxmt::Texture> m_frontCanvas;
  // Persistent scratch for multi-backbuffer content rotation. Present saves slot
  // 0's just-presented content here, shifts slots 1..N-1 down, then restores it
  // into slot N-1, advancing the flip chain (new[i] = old[(i+1) % N]) without
  // moving any surface backing, via matched-sample-count blit-encoder copies.
  // Allocated lazily on the first multi-backbuffer Present at the backbuffers'
  // own sample count (so an MSAA chain's multisample content round-trips); null
  // on single-buffer and COPY chains; dropped on Reset.
  Rc<dxmt::Texture> m_rotationScratch;
  // CAMetalLayer the chain blits to at Present, plus the NSView wrapper
  // returned by CreateMetalViewFromHWND that owns its lifetime. Null
  // when no HWND was provided (smokes and headless test harnesses);
  // Present then commits a no-op cmdbuf and returns S_OK.
  WMT::Object m_view;
  // Present hDestWindowOverride retarget: a per-window (view, layer,
  // presenter) created on first use; the overridden frame presents through
  // it, the device window's chain state stays untouched. DXVK keeps the
  // same per-window presenter map (d3d9_swapchain.cpp). A window whose
  // view creation failed caches a null presenter so the failure is not
  // retried every frame. Stale entries whose window has been destroyed are
  // evicted on the next override present (DXVK drives the same eviction off
  // an explicit window-teardown hook; d3d11's swapchain polls wsi::isWindow
  // the same way).
  struct OverrideTarget {
    WMT::Object view{};
    WMT::MetalLayer layer{};
    Rc<Presenter> presenter;
    uint32_t last_w = 0;
    uint32_t last_h = 0;
    double refresh_hz = 60.0;
  };
  std::unordered_map<HWND, OverrideTarget> m_overrideTargets;
  OverrideTarget *resolveOverrideTarget(HWND hwnd);
  // Drop override entries whose window no longer exists so the map cannot grow
  // for the swapchain's whole life; called on the override present path.
  void evictStaleOverrideTargets();
  // Tear down one override entry, the Presenter before the view that owns its
  // layer. Shared by the evictor and the dtor; defer_view_release routes the
  // view release through GPU completion (mid-run eviction, where a present
  // chunk may still encode on the layer) instead of inline (drained teardown).
  void releaseOverrideTarget(OverrideTarget &target, bool defer_view_release);
  WMT::MetalLayer m_layer;
  // Resolved hWnd the Presenter is bound to: used by Present's
  // wsi::isMinimized probe. Stashed from the ctor's hEffectiveWindow
  // argument, which the interface layer resolves from
  // params.hDeviceWindow / hFocusWindow before construction.
  // Null on headless chains.
  HWND m_hWindow = nullptr;
  // Shared presenter that owns the present_blit / present_scale PSOs and
  // does the scale-with-letterbox blit at Present time. Null on headless
  // chains (no m_layer). The dtor explicitly resets this before
  // ReleaseMetalView(m_view); the Presenter holds a non-retaining
  // WMT::MetalLayer copy whose backing CALayer is owned by the NSView.
  Rc<Presenter> m_presenter;
  // Client-rect extent Present last observed. NSView resizes
  // asynchronously (frames after Reset), so Present re-probes the
  // rect each frame (DXVK's per-Present model) and compares here.
  uint32_t m_lastWindowW = 0;
  uint32_t m_lastWindowH = 0;
  // Monitor the window was last seen on. Present re-probes m_refreshRateHz
  // (the INTERVAL_TWO/THREE/FOUR vsync dwell rate) when this changes; the
  // rate is otherwise sampled once at ctor and goes stale when the window is
  // dragged to a display with a different refresh rate.
  HMONITOR m_lastMonitor = nullptr;
  // Display refresh rate the chain is on, queried at ctor via
  // wsi::getCurrentDisplayMode(MonitorFromWindow) and re-probed by Present
  // when m_lastMonitor changes. Drives the PresentationInterval=TWO/THREE/
  // FOUR dwell math: apps that ask for 2× vsync on a 120Hz display want a
  // 16.67 ms dwell (60 fps), not the 33.33 ms (30 fps) the prior
  // 60Hz-hardcoded value gave them. Falls back to 60.0 on detection failure
  // (headless smokes, monitor query returning zero, etc).
  double m_refreshRateHz = 60.0;
  // Per-swapchain gamma ramp storage. Lazily initialized to identity
  // on first GetGammaRamp / SetGammaRamp. m_gammaSet tracks whether
  // the app has set a non-identity ramp; when false, GetGammaRamp
  // synthesizes identity on demand and the Presenter sees a null
  // gamma_ramp pointer (gamma path disabled, slightly faster). The
  // D3DGAMMARAMP storage holds the raw WORD ramp the app handed us so
  // GetGammaRamp returns it byte-identical; the upsample to
  // DXMTGammaRamp's float[1024] happens at SetGammaRamp time.
  D3DGAMMARAMP m_gammaRamp{};
  bool m_gammaSet = false;
  uint64_t m_gammaVersion = 0;
  // Monotonic Present counter. Incremented once per successful Present
  // (D3D_OK return path) and surfaced through GetLastPresentCount.
  // wined3d / DXVK both stub this; MSDN says it must increase per
  // Present, so we track it cheaply. Wraps at UINT_MAX per spec.
  UINT m_presentationCount = 0;
};

} // namespace dxmt
