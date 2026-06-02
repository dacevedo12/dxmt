#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "rc/util_rc_ptr.hpp"

#include <chrono>
#include <vector>

namespace dxmt {

class Presenter;
class MTLD3D9Device;
class MTLD3D9Surface;

class MTLD3D9SwapChain final : public ComObject<IDirect3DSwapChain9Ex> {
public:
  // hEffectiveWindow is the hDeviceWindow per spec: params.hDeviceWindow
  // if non-null, else the device's hFocusWindow. The interface layer
  // resolves it before calling us. Null is allowed: the chain simply
  // skips layer creation and Present becomes a no-op (smokes use this
  // path; real apps always have a window).
  MTLD3D9SwapChain(MTLD3D9Device *device, bool isEx, const D3DPRESENT_PARAMETERS &params, HWND hEffectiveWindow);
  ~MTLD3D9SwapChain();

  // Raw accessor used by the device ctor to auto-bind the backbuffer to
  // RT slot 0. Doesn't bump the public refcount: the caller treats the
  // surface as device-owned via the implicit-chain priv pin.
  // With BackBufferCount > 1 the device's RT0 still tracks slot 0;
  // Metal's CAMetalLayer handles in-flight buffering, so a single
  // app-visible surface is sufficient for the GPU rotation. Apps that
  // explicitly fetch GetBackBuffer(i>0) get a distinct surface object
  // they can manage themselves.
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
  HRESULT ResetForDeviceReset(const D3DPRESENT_PARAMETERS &params);

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
  // Lifetime contract (wined3d dlls/d3d9/swapchain.c): device holds chain
  // via raw pointer + AddRefPrivate/ReleasePrivate (no Com<> cycle). Public
  // Add/Release pins device; private refcount bumped by device, destroyed by
  // device dtor. Survives GetSwapChain(0); dev->Release() without UAF.
  MTLD3D9Device *m_device;
  const bool m_isEx;
  D3DPRESENT_PARAMETERS m_params;
  // Implicit backbuffer chain: m_backBuffers[0] = device RT0. With
  // BackBufferCount > 1, Present rotates backing per SwapEffect=FLIP/DISCARD;
  // COPY skips rotation. Surfaces are Com<,false> (priv ref only).
  std::vector<Com<MTLD3D9Surface, false>> m_backBuffers;
  // CAMetalLayer the chain blits to at Present, plus the NSView wrapper
  // returned by CreateMetalViewFromHWND that owns its lifetime. Null
  // when no HWND was provided (smokes and headless test harnesses);
  // Present then commits a no-op cmdbuf and returns S_OK.
  WMT::Object m_view;
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
  // Wall-clock at previous Present's return; used to bump
  // interPresentMicros. NSView resizes asynchronously (frames after Reset);
  // Present re-probes client rect each frame (DXVK per-Present model).
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
