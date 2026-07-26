#include "d3d12_swapchain.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "config/config.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_hud_state.hpp"
#include "dxmt_info.hpp"
#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include "wsi_monitor.hpp"
#include "wsi_window.hpp"

#include <atomic>
#include <cfloat>
#include <chrono>

namespace dxmt::d3d12 {
namespace {

using clock = std::chrono::steady_clock;

static uint32_t
GetSwapChainPresentAlphaMode(DXGI_ALPHA_MODE alpha_mode) {
  switch (alpha_mode) {
  case DXGI_ALPHA_MODE_PREMULTIPLIED:
  case DXGI_ALPHA_MODE_STRAIGHT:
  case DXGI_ALPHA_MODE_IGNORE:
    return alpha_mode;
  case DXGI_ALPHA_MODE_UNSPECIFIED:
  default:
    return DXGI_ALPHA_MODE_IGNORE;
  }
}

static WMTPixelFormat
GetSwapChainPixelFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return WMTPixelFormatBGRA8Unorm_sRGB;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return WMTPixelFormatBGRA8Unorm;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTPixelFormatRGB10A2Unorm;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return WMTPixelFormatRGBA16Float;
  default:
    return WMTPixelFormatInvalid;
  }
}

static WMTColorSpace
GetSwapChainColorSpace(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? WMTColorSpaceSRGBLinear
                                                  : WMTColorSpaceSRGB;
}

static WMTColorSpace
GetD3D12SwapChainColorSpace(DXGI_COLOR_SPACE_TYPE color_space) {
  switch (color_space) {
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    return WMTColorSpaceSRGB;
  case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    return WMTColorSpaceSRGBLinear;
  default:
    return WMTColorSpaceInvalid;
  }
}

static bool
IsSupportedD3D12SwapChainColorSpace(DXGI_COLOR_SPACE_TYPE color_space) {
  const WMTColorSpace wmt_color_space =
      GetD3D12SwapChainColorSpace(color_space);
  return wmt_color_space != WMTColorSpaceInvalid &&
         CGColorSpace_checkColorSpaceSupported(wmt_color_space);
}

static WMTColorSpace
GetD3D12SwapChainLayerColorSpace(DXGI_FORMAT format,
                                 DXGI_COLOR_SPACE_TYPE color_space) {
  return color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
             ? GetSwapChainColorSpace(format)
             : GetD3D12SwapChainColorSpace(color_space);
}

static constexpr UINT D3D12SupportedPresentFlags =
    DXGI_PRESENT_TEST | DXGI_PRESENT_ALLOW_TEARING;

static bool
IsSwapChainOcclusionForcedForTesting() {
  return env::getEnvVar("DXMT_TEST_FORCE_SWAPCHAIN_OCCLUDED") == "1";
}

static constexpr UINT D3D12SupportedSwapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

static void
D3D12DiagLogSwapChainBackBuffer(const char *event, UINT index,
                                UINT current_index,
                                ID3D12Resource *backbuffer) {
  static std::atomic<uint32_t> log_count = 0;
  if (!D3D12DiagShouldLog(log_count, D3D12DiagSwapChainEnabled()))
    return;

  auto *resource = dynamic_cast<Resource *>(backbuffer);
  auto *texture = resource ? resource->GetTexture() : nullptr;
  auto *allocation = resource ? resource->GetTextureAllocation() : nullptr;
  WMT::Texture metal_texture =
      texture && texture->current() ? texture->current()->texture()
                                    : WMT::Texture{};
  const auto desc =
      resource ? resource->GetResourceDesc() : D3D12_RESOURCE_DESC{};
  INFO("D3D12 diagnostic: swapchain backbuffer",
       " event=", event,
       " index=", index,
       " current=", current_index,
       " resource=", uint64_t(backbuffer),
       " texture_descriptor=", uint64_t(texture),
       " allocation=", uint64_t(allocation),
       " texture=", uint64_t(metal_texture),
       " resource_size=", resource ? uint64_t(desc.Width) : 0, "x",
       resource ? uint32_t(desc.Height) : 0,
       " resource_format=", resource ? uint32_t(desc.Format) : 0,
       " texture_size=", texture ? texture->width() : 0, "x",
       texture ? texture->height() : 0,
       " texture_format=", texture ? uint32_t(texture->pixelFormat()) : 0);
}

  class SwapChainImpl final : public ComObjectWithInitialRef<IDXGISwapChain4> {
  public:
    SwapChainImpl(D3D12SwapChainHost &host, IDXGIFactory1 *factory, HWND hWnd,
                  const DXGI_SWAP_CHAIN_DESC1 &desc,
                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc)
        : queue_(host.SwapChainQueue()), host_(host), factory_(factory),
          hWnd_(hWnd), desc_(desc),
          fullscreen_desc_(fullscreen_desc ? *fullscreen_desc
                                           : DXGI_SWAP_CHAIN_FULLSCREEN_DESC{}),
          hud_(WMT::DeveloperHUDProperties::instance()) {
      if (!fullscreen_desc)
        fullscreen_desc_.Windowed = TRUE;

      monitor_ = wsi::getWindowMonitor(hWnd_);
      preferred_max_frame_rate_ =
          Config::getInstance().getOption<int>("d3d12.preferredMaxFrameRate", 0);
      InitDisplayRefreshRate();

      native_view_ = WMT::CreateMetalViewFromHWND(
          reinterpret_cast<intptr_t>(hWnd_), host_.SwapChainDevice().GetMTLDevice(),
          layer_);
      if (!native_view_) {
        Logger::err("D3D12SwapChain: failed to create Metal view");
        return;
      }

      presenter_ = Rc(new Presenter(
          host_.SwapChainDevice().GetDXMTDevice().queue(),
          host_.SwapChainDevice().GetMTLDevice(), layer_,
          host_.SwapChainDevice().GetDXMTDevice().queue().cmd_library, 1.0f,
          desc_.SampleDesc.Count ? desc_.SampleDesc.Count : 1));
      hud_.initialize(GetVersionDescriptionText(12, D3D_FEATURE_LEVEL_12_0));
      present_queue_semaphore_ =
          CreateSemaphore(nullptr, frame_latency_,
                          DXGI_MAX_SWAP_CHAIN_BUFFERS, nullptr);
      if (!present_queue_semaphore_)
        WARN("D3D12SwapChain: failed to create present queue semaphore");
      if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        present_semaphore_ =
            CreateSemaphore(nullptr, frame_latency_, DXGI_MAX_SWAP_CHAIN_BUFFERS,
                            nullptr);
      }
      host_.SwapChainDevice().GetDXMTDevice().queue().SetMaxLatency(
          frame_latency_);
      ResizeBuffers(desc_.BufferCount, desc_.Width, desc_.Height, desc_.Format,
                    desc_.Flags);
    }

    ~SwapChainImpl() {
      backbuffers_.clear();
      if (present_semaphore_)
        CloseHandle(present_semaphore_);
      if (present_queue_semaphore_)
        CloseHandle(present_queue_semaphore_);
      if (native_view_)
        WMT::ReleaseMetalView(native_view_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void **object) override {
      if (!object)
        return E_POINTER;
      *object = nullptr;
      if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
          riid == __uuidof(IDXGIDeviceSubObject) ||
          riid == __uuidof(IDXGISwapChain) ||
          riid == __uuidof(IDXGISwapChain1) ||
          riid == __uuidof(IDXGISwapChain2) ||
          riid == __uuidof(IDXGISwapChain3) ||
          riid == __uuidof(IDXGISwapChain4)) {
        *object = ref(this);
        return S_OK;
      }
      if (logQueryInterfaceError(__uuidof(IDXGISwapChain1), riid))
        WARN("D3D12SwapChain: unknown interface query ", str::format(riid));
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override {
      return factory_->QueryInterface(riid, parent);
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                             void *data) override {
      return private_data_.getData(guid, data_size, data);
    }

    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                             const void *data) override {
      return private_data_.setData(guid, data_size, data);
    }

    HRESULT STDMETHODCALLTYPE
    SetPrivateDataInterface(REFGUID guid, const IUnknown *object) override {
      return private_data_.setInterface(guid, object);
    }

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
      return host_.SwapChainDevice().QueryInterface(riid, device);
    }

    HRESULT STDMETHODCALLTYPE Present(UINT sync_interval,
                                      UINT flags) override {
      return Present1(sync_interval, flags, nullptr);
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer_idx, REFIID riid,
                                        void **surface) override {
      if (!surface)
        return E_POINTER;
      *surface = nullptr;
      if (buffer_idx >= backbuffers_.size())
        return DXGI_ERROR_INVALID_CALL;
      D3D12DiagLogSwapChainBackBuffer("GetBuffer", buffer_idx,
                                      current_backbuffer_,
                                      backbuffers_[buffer_idx].ptr());
      HRESULT hr = backbuffers_[buffer_idx]->QueryInterface(riid, surface);
      if (SUCCEEDED(hr) && dxmt::apitrace::d3d_enabled()) {
        // Tag the back buffer with swapchain semantics using the exact pointer
        // the app receives, so it matches the resource later passed to
        // CreateRenderTargetView. Native retrace needs this marker to map the
        // resource onto the real swapchain back buffer and infer window size.
        dxmt::apitrace::record_swapchain_back_buffer(
            &host_.SwapChainDevice(), this,
            static_cast<ID3D12Resource *>(*surface), buffer_idx);
      }
      return hr;
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen,
                                                 IDXGIOutput *target) override {
      fullscreen_desc_.Windowed = !fullscreen;
      target_ = target;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen,
                                                 IDXGIOutput **target) override {
      if (fullscreen)
        *fullscreen = !fullscreen_desc_.Windowed;
      if (target)
        *target = target_.ref();
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override {
      if (!desc)
        return WARN_E_INVALIDARG(__func__);
      desc->BufferDesc.Width = desc_.Width;
      desc->BufferDesc.Height = desc_.Height;
      desc->BufferDesc.RefreshRate = fullscreen_desc_.RefreshRate;
      desc->BufferDesc.Format = desc_.Format;
      desc->BufferDesc.ScanlineOrdering = fullscreen_desc_.ScanlineOrdering;
      desc->BufferDesc.Scaling = fullscreen_desc_.Scaling;
      desc->SampleDesc = desc_.SampleDesc;
      desc->BufferUsage = desc_.BufferUsage;
      desc->BufferCount = desc_.BufferCount;
      desc->OutputWindow = hWnd_;
      desc->Windowed = fullscreen_desc_.Windowed;
      desc->SwapEffect = desc_.SwapEffect;
      desc->Flags = desc_.Flags;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT buffer_count, UINT width,
                                            UINT height, DXGI_FORMAT format,
                                            UINT flags) override {
      UINT old_width = desc_.Width;
      UINT old_height = desc_.Height;
      UINT old_index = current_backbuffer_;
      if (buffer_count == 0)
        buffer_count = desc_.BufferCount ? desc_.BufferCount : 2;
      if (!buffer_count || buffer_count > DXGI_MAX_SWAP_CHAIN_BUFFERS) {
        WARN("D3D12SwapChain::ResizeBuffers: invalid buffer count ",
             buffer_count);
        return DXGI_ERROR_INVALID_CALL;
      }
      UINT new_width = width;
      UINT new_height = height;
      if (new_width == 0 || new_height == 0)
        wsi::getWindowSize(hWnd_, new_width ? nullptr : &new_width,
                           new_height ? nullptr : &new_height);
      new_width = new_width ? new_width : 1;
      new_height = new_height ? new_height : 1;
      const DXGI_FORMAT new_format =
          format == DXGI_FORMAT_UNKNOWN ? desc_.Format : format;
      const WMTPixelFormat new_pixel_format =
          GetSwapChainPixelFormat(new_format);
      if (new_pixel_format == WMTPixelFormatInvalid) {
        WARN("D3D12SwapChain::ResizeBuffers: unsupported format ",
             new_format);
        return DXGI_ERROR_UNSUPPORTED;
      }
      const WMTColorSpace new_color_space =
          GetD3D12SwapChainLayerColorSpace(new_format, color_space_);
      if (flags & ~D3D12SupportedSwapChainFlags) {
        WARN("D3D12SwapChain::ResizeBuffers: unsupported flags ",
             flags & ~D3D12SupportedSwapChainFlags);
        return DXGI_ERROR_UNSUPPORTED;
      }
      if (!host_.PrepareSwapChainResize(backbuffers_)) {
        WARN("D3D12SwapChain::ResizeBuffers: backbuffer references are still "
             "held by the application");
        return DXGI_ERROR_INVALID_CALL;
      }

      desc_.BufferCount = buffer_count;
      desc_.Width = new_width;
      desc_.Height = new_height;
      desc_.Format = new_format;
      desc_.Flags = flags;

      if (!width || !height) {
        WARN("D3D12SwapChain::ResizeBuffers: resolved zero size request to ",
             desc_.Width, "x", desc_.Height);
      }

      presenter_->changeLayerProperties(new_pixel_format, new_color_space,
                                        desc_.Width, desc_.Height,
                                        desc_.SampleDesc.Count
                                            ? desc_.SampleDesc.Count
                                            : 1);

      backbuffers_.clear();
      backbuffers_.reserve(desc_.BufferCount);
      for (UINT i = 0; i < desc_.BufferCount; i++) {
        D3D12_HEAP_PROPERTIES heap_props = {};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap_props.CreationNodeMask = 1;
        heap_props.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resource_desc = {};
        resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_desc.Alignment = 0;
        resource_desc.Width = desc_.Width;
        resource_desc.Height = desc_.Height;
        resource_desc.DepthOrArraySize = 1;
        resource_desc.MipLevels = 1;
        resource_desc.Format = new_format;
        resource_desc.SampleDesc = desc_.SampleDesc;
        if (!resource_desc.SampleDesc.Count)
          resource_desc.SampleDesc.Count = 1;
        resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        backbuffers_.push_back(CreateResource(
            &host_.SwapChainDevice(), &heap_props, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_PRESENT, 0, nullptr));
        if (!backbuffers_.back())
          return E_FAIL;
        D3D12DiagLogSwapChainBackBuffer("ResizeBuffers", i,
                                        current_backbuffer_,
                                        backbuffers_.back().ptr());
      }

      current_backbuffer_ = desc_.BufferCount ? old_index % desc_.BufferCount : 0;
      if (!source_width_ || source_width_ == old_width)
        source_width_ = desc_.Width;
      if (!source_height_ || source_height_ == old_height)
        source_height_ = desc_.Height;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *desc) override {
      return desc ? S_OK : DXGI_ERROR_INVALID_CALL;
    }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override {
      InitReturnPtr(output);
      if (!output)
        return E_POINTER;
      if (!wsi::isWindow(hWnd_))
        return DXGI_ERROR_INVALID_CALL;
      if (target_) {
        *output = target_.ref();
        return S_OK;
      }
      return GetOutputFromMonitor(wsi::getWindowMonitor(hWnd_), output);
    }

    HRESULT STDMETHODCALLTYPE
    GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) override {
      if (!stats)
        return WARN_E_INVALIDARG(__func__);
      stats->PresentCount = presentation_count_;
      stats->SyncRefreshCount = presentation_count_;
      stats->PresentRefreshCount = presentation_count_;
      stats->SyncGPUTime = {};
      stats->SyncQPCTime = {};
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *last_present_count) override {
      if (!last_present_count)
        return E_POINTER;
      *last_present_count = presentation_count_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *desc) override {
      if (!desc)
        return E_POINTER;
      *desc = desc_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc) override {
      if (!desc)
        return E_POINTER;
      *desc = fullscreen_desc_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *hWnd) override {
      if (!hWnd)
        return E_POINTER;
      *hWnd = hWnd_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void **window) override {
      InitReturnPtr(window);
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Present1(
        UINT sync_interval, UINT flags,
        const DXGI_PRESENT_PARAMETERS *present_parameters) override {
      dxmt::perf::ScopedFrameTimer perf_timer(
          dxmt::perf::FrameTimeBucket::Present);
      auto trace_present_return = [this, sync_interval,
                                   flags](HRESULT result) {
        dxmt::apitrace::on_d3d12_present(
            this, sync_interval, flags, static_cast<int32_t>(result), false);
        return result;
      };
      if (sync_interval > 4)
        return trace_present_return(DXGI_ERROR_INVALID_CALL);
      if (flags & ~D3D12SupportedPresentFlags) {
        WARN("D3D12SwapChain::Present1: unsupported flags ",
             flags & ~D3D12SupportedPresentFlags);
        return trace_present_return(DXGI_ERROR_UNSUPPORTED);
      }
      if ((flags & DXGI_PRESENT_ALLOW_TEARING) &&
          !(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
        WARN("D3D12SwapChain::Present1: ALLOW_TEARING used without swapchain "
             "tearing support");
        return trace_present_return(DXGI_ERROR_INVALID_CALL);
      }
      if ((flags & DXGI_PRESENT_ALLOW_TEARING) && sync_interval) {
        WARN("D3D12SwapChain::Present1: ALLOW_TEARING requires sync interval 0");
        return trace_present_return(DXGI_ERROR_INVALID_CALL);
      }
      if (present_parameters &&
          (present_parameters->DirtyRectsCount || present_parameters->pDirtyRects ||
           present_parameters->pScrollRect || present_parameters->pScrollOffset)) {
        WARN("D3D12SwapChain::Present1: dirty rect and scroll parameters are "
             "not supported");
        return trace_present_return(DXGI_ERROR_UNSUPPORTED);
      }

      const bool occluded = IsSwapChainOcclusionForcedForTesting() ||
                            wsi::isMinimized(hWnd_);
      HRESULT hr = occluded ? DXGI_STATUS_OCCLUDED : S_OK;
      if (flags & DXGI_PRESENT_TEST)
        return trace_present_return(hr);
      if (hr == DXGI_STATUS_OCCLUDED)
        return trace_present_return(hr);

      auto *resource = dynamic_cast<Resource *>(
          backbuffers_[current_backbuffer_].ptr());
      if (resource && resource->IsReservedTexture())
        resource->EnsureTextureAllocation("Present");
      if (!resource || !resource->GetTexture())
        return trace_present_return(E_FAIL);
      D3D12DiagLogSwapChainBackBuffer("Present1", current_backbuffer_,
                                      current_backbuffer_,
                                      backbuffers_[current_backbuffer_].ptr());

      const auto apitrace_frame_index =
          dxmt::apitrace::on_d3d12_present(
              this, sync_interval, flags, static_cast<int32_t>(S_OK), true);
      const bool display_sync_enabled =
          sync_interval > 0 && !(flags & DXGI_PRESENT_ALLOW_TEARING);
      const double present_after =
          preferred_max_frame_rate_ > 0 ? 1.0 / preferred_max_frame_rate_ : 0.0;
      // PERF DIAG (DXMT_DIAG_PRESENT_PACING): report the split between
      // CAMetalLayer display sync and explicit Metal frame-rate caps.
      {
        static const bool present_pacing =
            D3D12DiagEnabledEnv("DXMT_DIAG_PRESENT_PACING");
        if (present_pacing) {
          static std::atomic<uint32_t> pc = 0;
          static thread_local clock::time_point last;
          auto now = clock::now();
          const auto idx = pc.fetch_add(1, std::memory_order_relaxed);
          double sinceMs =
              last == clock::time_point{}
                  ? 0.0
                  : std::chrono::duration_cast<std::chrono::microseconds>(
                        now - last).count() / 1000.0;
          last = now;
          if (idx % 60 == 0)
            WARN_FILE_ONLY("DXMT present pacing:"
                 " syncInterval=", sync_interval,
                 " flags=", flags,
                 " displaySyncEnabled=", display_sync_enabled,
                 " presentAfterMs=", present_after * 1000.0,
                 " fpsCap=", present_after > 0 ? 1.0 / present_after : 0.0,
                 " preferredMaxFrameRate=", preferred_max_frame_rate_,
                 " actualPresentIntervalMs=", sinceMs);
        }
      }
      auto &dxmt_queue = host_.SwapChainDevice().GetDXMTDevice().queue();
      const auto present_frame_id = dxmt_queue.CurrentFrameSeq() - 1;
      presenter_->setDisplaySyncEnabled(display_sync_enabled);
      HANDLE present_queue_signal = nullptr;
      HANDLE present_signal = nullptr;
      HANDLE process = GetCurrentProcess();
      if (present_queue_semaphore_) {
        dxmt::perf::ScopedFrameDuration present_queue_wait_timer(
            dxmt::perf::currentFrameStatistics(),
            &dxmt::FrameStatistics::frame_present_queue_wait_interval);
        DWORD wait_result =
            WaitForSingleObject(present_queue_semaphore_, INFINITE);
        present_queue_wait_timer.stop();
        if (wait_result != WAIT_OBJECT_0) {
          WARN("D3D12SwapChain::Present1: present queue wait failed result=",
               wait_result);
        } else if (!DuplicateHandle(process, present_queue_semaphore_, process,
                                    &present_queue_signal, 0, FALSE,
                                    DUPLICATE_SAME_ACCESS)) {
          WARN("D3D12SwapChain::Present1: failed to duplicate present queue "
               "semaphore");
          ReleaseSemaphore(present_queue_semaphore_, 1, nullptr);
        }
      }
      if (present_semaphore_) {
        if (!DuplicateHandle(process, present_semaphore_, process,
                             &present_signal, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
          WARN("D3D12SwapChain::Present1: failed to duplicate frame latency "
               "waitable semaphore");
        }
      }

      auto state_storage = presenter_->synchronizeLayerProperties();
      auto state =
          std::make_shared<Presenter::PresentState>(std::move(state_storage));
      state->metadata.alpha_mode = GetSwapChainPresentAlphaMode(desc_.AlphaMode);
      state->metadata.background_color[0] = background_color_.r;
      state->metadata.background_color[1] = background_color_.g;
      state->metadata.background_color[2] = background_color_.b;
      state->metadata.background_color[3] = background_color_.a;

      auto present_signals = std::make_shared<PresentSemaphoreSignals>(
          present_queue_signal, present_signal);
      Com<ID3D12Resource> present_resource =
          backbuffers_[current_backbuffer_];
      const auto api_thread_present_view = resource->GetPresentSourceView();
      host_.SubmitPresent(
          D3D12PresentSubmission(
              Rc<Texture>(resource->GetTexture()),
              std::move(present_resource), api_thread_present_view, presenter_,
              present_after, apitrace_frame_index, sync_interval, flags,
              std::move(state), std::move(present_signals)),
          present_frame_id);
      perf_timer.stop();
      host_.NotifyPresentBoundary();

      presentation_count_++;
      current_backbuffer_ =
          desc_.BufferCount ? (current_backbuffer_ + 1) % desc_.BufferCount : 0;
      return S_OK;
    }

    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }

    HRESULT STDMETHODCALLTYPE
    GetRestrictToOutput(IDXGIOutput **restrict_to_output) override {
      InitReturnPtr(restrict_to_output);
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *color) override {
      background_color_ = color ? *color : DXGI_RGBA{};
      return color ? S_OK : E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *color) override {
      if (!color)
        return WARN_E_INVALIDARG(__func__);
      *color = background_color_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override {
      rotation_ = rotation;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *rotation) override {
      if (!rotation)
        return WARN_E_INVALIDARG(__func__);
      *rotation = rotation_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override {
      if (!width || !height)
        return WARN_E_INVALIDARG(__func__);
      source_width_ = width;
      source_height_ = height;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *width, UINT *height) override {
      if (width)
        *width = source_width_ ? source_width_ : desc_.Width;
      if (height)
        *height = source_height_ ? source_height_ : desc_.Height;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
      if (!max_latency || max_latency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
        return WARN_E_INVALIDARG(__func__);
      if (present_queue_semaphore_ && max_latency > frame_latency_)
        ReleaseSemaphore(present_queue_semaphore_, max_latency - frame_latency_,
                         nullptr);
      if (present_semaphore_ && max_latency > frame_latency_)
        ReleaseSemaphore(present_semaphore_, max_latency - frame_latency_,
                         nullptr);
      frame_latency_ = max_latency;
      host_.SwapChainDevice().GetDXMTDevice().queue().SetMaxLatency(max_latency);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
      if (max_latency)
        *max_latency = frame_latency_;
      return S_OK;
    }

    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
      if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ||
          !present_semaphore_)
        return nullptr;

      HANDLE result = nullptr;
      HANDLE process = GetCurrentProcess();
      if (!DuplicateHandle(process, present_semaphore_, process, &result, 0,
                           FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
      return result;
    }

    HRESULT STDMETHODCALLTYPE
    SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override {
      if (!matrix)
        return WARN_E_INVALIDARG(__func__);
      matrix_ = *matrix;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override {
      if (!matrix)
        return WARN_E_INVALIDARG(__func__);
      *matrix = matrix_;
      return S_OK;
    }

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
      D3D12DiagLogSwapChainBackBuffer("GetCurrentBackBufferIndex",
                                      current_backbuffer_,
                                      current_backbuffer_,
                                      current_backbuffer_ < backbuffers_.size()
                                          ? backbuffers_[current_backbuffer_].ptr()
                                          : nullptr);
      return current_backbuffer_;
    }

    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(
        DXGI_COLOR_SPACE_TYPE color_space, UINT *color_space_support) override {
      if (!color_space_support)
        return WARN_E_INVALIDARG(__func__);
      *color_space_support =
          IsSupportedD3D12SwapChainColorSpace(color_space)
              ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
              : 0;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetColorSpace1(
        DXGI_COLOR_SPACE_TYPE color_space) override {
      if (!IsSupportedD3D12SwapChainColorSpace(color_space)) {
        WARN("D3D12SwapChain::SetColorSpace1: unsupported color space ",
             color_space);
        return DXGI_ERROR_UNSUPPORTED;
      }
      color_space_ = color_space;
      presenter_->changeLayerColorSpace(
          GetD3D12SwapChainLayerColorSpace(desc_.Format, color_space_));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers1(
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format,
        UINT flags, const UINT *creation_node_mask,
        IUnknown *const *present_queue) override {
      const UINT queue_count =
          buffer_count ? buffer_count : (desc_.BufferCount ? desc_.BufferCount : 2);
      if (!creation_node_mask || !present_queue)
        return DXGI_ERROR_INVALID_CALL;
      if (creation_node_mask) {
        for (UINT i = 0; i < queue_count; i++) {
          if (creation_node_mask[i] > 1) {
            WARN("D3D12SwapChain::ResizeBuffers1: unsupported creation node mask ",
                 creation_node_mask[i]);
            return DXGI_ERROR_INVALID_CALL;
          }
        }
      }

      for (UINT i = 0; i < queue_count; i++) {
        if (!present_queue[i])
          return DXGI_ERROR_INVALID_CALL;

        auto queue = Com<ID3D12CommandQueue>::queryFrom(present_queue[i]);
        if (!queue) {
          WARN("D3D12SwapChain::ResizeBuffers1: present queue is not a D3D12 command queue");
          return DXGI_ERROR_INVALID_CALL;
        }

        if (queue.ptr() != static_cast<ID3D12CommandQueue *>(queue_.ptr())) {
          WARN_FILE_ONLY(
              "D3D12SwapChain::ResizeBuffers1: ignoring non-swapchain present queue");
        }
      }

      if (D3D12DiagSwapChainEnabled()) {
        INFO("D3D12 diagnostic: ResizeBuffers1 bufferCount=", buffer_count,
             " effectiveQueueCount=", queue_count, " size=", width, "x",
             height, " format=", format, " flags=", flags);
      }

      return ResizeBuffers(buffer_count, width, height, format, flags);
    }

    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type,
                                             UINT size, void *metadata) override {
      if (type == DXGI_HDR_METADATA_TYPE_NONE)
        return S_OK;
      WARN("D3D12SwapChain::SetHDRMetaData: HDR metadata is not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }

  private:
    void InitDisplayRefreshRate() {
      if (!monitor_)
        return;

      wsi::WsiMode current_mode = {};
      if (wsi::getCurrentDisplayMode(monitor_, &current_mode) &&
          current_mode.refreshRate.numerator &&
          current_mode.refreshRate.denominator) {
        init_refresh_rate_ =
            double(current_mode.refreshRate.numerator) /
            double(current_mode.refreshRate.denominator);
      }
    }

    HRESULT GetOutputFromMonitor(HMONITOR monitor, IDXGIOutput **output) {
      Com<IDXGIAdapter> adapter;
      Com<IDXGIOutput> candidate;
      if (FAILED(host_.SwapChainDevice().GetAdapter(&adapter)))
        return E_FAIL;

      for (UINT i = 0; SUCCEEDED(adapter->EnumOutputs(i, &candidate)); i++) {
        DXGI_OUTPUT_DESC desc = {};
        if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == monitor)
          return candidate->QueryInterface(IID_PPV_ARGS(output));
        candidate = nullptr;
      }
      return DXGI_ERROR_NOT_FOUND;
    }

    Com<ID3D12CommandQueue> queue_;
    D3D12SwapChainHost &host_;
    Com<IDXGIFactory1> factory_;
    ComPrivateData private_data_;
    HWND hWnd_ = nullptr;
    HMONITOR monitor_ = nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc_ = {};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc_ = {};
    std::vector<Com<ID3D12Resource>> backbuffers_;
    UINT current_backbuffer_ = 0;
    UINT presentation_count_ = 0;
    WMT::Object native_view_;
    WMT::MetalLayer layer_;
    Rc<Presenter> presenter_;
    HUDState hud_;
    Com<IDXGIOutput> target_;
    UINT frame_latency_ = 1;
    UINT source_width_ = 0;
    UINT source_height_ = 0;
    DXGI_RGBA background_color_ = {};
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    DXGI_MATRIX_3X2_F matrix_ = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    DXGI_COLOR_SPACE_TYPE color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    HANDLE present_queue_semaphore_ = nullptr;
    HANDLE present_semaphore_ = nullptr;
    double init_refresh_rate_ = DBL_MAX;
    int preferred_max_frame_rate_ = 0;
  };


} // namespace

PresentSemaphoreSignals::PresentSemaphoreSignals(HANDLE queue_depth,
                                                 HANDLE waitable) noexcept
    : queue_depth_(queue_depth), waitable_(waitable) {}

PresentSemaphoreSignals::~PresentSemaphoreSignals() noexcept {
  Close();
}

void
PresentSemaphoreSignals::Signal() noexcept {
  if (queue_depth_) {
    ReleaseSemaphore(queue_depth_, 1, nullptr);
    CloseHandle(queue_depth_);
    queue_depth_ = nullptr;
  }
  if (waitable_) {
    ReleaseSemaphore(waitable_, 1, nullptr);
    CloseHandle(waitable_);
    waitable_ = nullptr;
  }
}

void
PresentSemaphoreSignals::Close() noexcept {
  if (queue_depth_) {
    CloseHandle(queue_depth_);
    queue_depth_ = nullptr;
  }
  if (waitable_) {
    CloseHandle(waitable_);
    waitable_ = nullptr;
  }
}

HRESULT
CreateD3D12SwapChain(
    D3D12SwapChainHost &host, IDXGIFactory1 *factory, HWND window,
    const DXGI_SWAP_CHAIN_DESC1 &desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
    IDXGISwapChain1 **swap_chain) {
  InitReturnPtr(swap_chain);
  if (!swap_chain || !factory || !window)
    return DXGI_ERROR_INVALID_CALL;

  auto object = Com<IDXGISwapChain1>::transfer(
      new SwapChainImpl(host, factory, window, desc, fullscreen_desc));
  return object->QueryInterface(IID_PPV_ARGS(swap_chain));
}

} // namespace dxmt::d3d12
