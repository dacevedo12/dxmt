#include "com/com_pointer.hpp"
#include "config/config.hpp"
#include "dxgi_backend.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "com/com_guid.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_fh4_bypass.hpp"
#include "util_string.hpp"
#include "wsi_window.hpp"

#include <cstring>

namespace dxmt {

Com<IMTLDXGIAdapter> CreateAdapter(const DxgiBackendProvider &provider,
                                   const DxgiBackendAdapterInfo &info,
                                   IDXGIFactory2 *pFactory, Config &config);
LUID GetAdapterLuid(uint64_t registry_id);

static bool IsEqualLuid(LUID a, LUID b) {
  return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

struct DxgiAdapterCandidate {
  DxgiBackendProvider provider = {};
  DxgiBackendAdapterInfo info = {};
};

static constexpr uint64_t DeferredMetal4RegistryId = 0x44584d544d344445ull;

static DxgiAdapterCandidate
MakeDeferredAdapterCandidate() {
  DxgiAdapterCandidate candidate = {};
  candidate.provider.kind = DxgiBackendKind::Unknown;
  candidate.provider.name = "deferred";
  candidate.provider.priority = 100;
  candidate.info.registry_id = DeferredMetal4RegistryId;
  candidate.info.backend_index = 0;
  candidate.info.has_unified_memory = true;
  candidate.info.recommended_max_working_set_size = 8ull * 1024ull * 1024ull * 1024ull;
  static constexpr WCHAR name[] = L"Apple Metal Deferred Adapter";
  std::memcpy(candidate.info.name, name, sizeof(name));
  return candidate;
}

static std::vector<DxgiAdapterCandidate>
EnumerateAdapterCandidates() {
  std::vector<DxgiAdapterCandidate> candidates;
  for (const auto &provider : CopyRegisteredBackends()) {
    const uint32_t count = provider.adapter_count ? provider.adapter_count() : 0;
    for (uint32_t i = 0; i < count; ++i) {
      DxgiBackendAdapterInfo info = {};
      if (!provider.get_adapter_info || !provider.get_adapter_info(i, &info) ||
          !info.device_handle)
        continue;
      info.backend_index = i;
      candidates.push_back({provider, info});
    }
  }
  if (candidates.empty())
    candidates.push_back(MakeDeferredAdapterCandidate());
  return candidates;
}

static constexpr UINT SupportedSwapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_NONPREROTATED |
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
    DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE |
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

static bool IsKnownSwapEffect(DXGI_SWAP_EFFECT swap_effect) {
  switch (swap_effect) {
  case DXGI_SWAP_EFFECT_DISCARD:
  case DXGI_SWAP_EFFECT_SEQUENTIAL:
  case DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL:
  case DXGI_SWAP_EFFECT_FLIP_DISCARD:
    return true;
  default:
    return false;
  }
}

static bool DxgiBootstrapDiagEnabled() {
  auto enabled = env::getEnvVar("DXMT_DIAG_DXGI");
  if (enabled.empty())
    enabled = env::getEnvVar("DXMT_DIAG_D3D12_DEVICE");
  if (enabled.empty())
    enabled = env::getEnvVar("DXMT_DIAG_COMMAND_QUEUE");
  return enabled == "1" || enabled == "true" || enabled == "yes" ||
         enabled == "trace";
}

class MTLDXGIFactory : public MTLDXGIObject<IDXGIFactory6> {

public:
  MTLDXGIFactory(UINT Flags) : flags_(Flags) {};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    fh4bypass::ApplyBadFiberDataBypass();

    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIFactory) || riid == __uuidof(IDXGIFactory1) ||
        riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory2) ||
        riid == __uuidof(IDXGIFactory3) || riid == __uuidof(IDXGIFactory4) ||
        riid == __uuidof(IDXGIFactory5) || riid == __uuidof(IDXGIFactory6)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIFactory2), riid)) {
      WARN("DXGIFactory: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    InitReturnPtr(ppParent);

    WARN("DXGIFactory::GetParent: Unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() final {
    // We don't support Stereo 3D at the moment
    return FALSE;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter **ppAdapter) final {
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    ERR("Software adapters not supported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  CreateSwapChain(IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
                  IDXGISwapChain **ppSwapChain) final {
    if (ppSwapChain == nullptr || pDesc == nullptr || pDevice == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    DXGI_SWAP_CHAIN_DESC1 desc;
    desc.Width = pDesc->BufferDesc.Width;
    desc.Height = pDesc->BufferDesc.Height;
    desc.Format = pDesc->BufferDesc.Format;
    desc.Stereo = FALSE;
    desc.SampleDesc = pDesc->SampleDesc;
    desc.BufferUsage = pDesc->BufferUsage;
    desc.BufferCount = pDesc->BufferCount;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = pDesc->SwapEffect;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = pDesc->Flags;

    DXGI_SWAP_CHAIN_FULLSCREEN_DESC descFs;
    descFs.RefreshRate = pDesc->BufferDesc.RefreshRate;
    descFs.ScanlineOrdering = pDesc->BufferDesc.ScanlineOrdering;
    descFs.Scaling = pDesc->BufferDesc.Scaling;
    descFs.Windowed = pDesc->Windowed;

    IDXGISwapChain1 *swapChain = nullptr;
    HRESULT hr = CreateSwapChainForHwnd(pDevice, pDesc->OutputWindow, &desc,
                                        &descFs, nullptr, &swapChain);

    *ppSwapChain = swapChain;
    return hr;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(
      IUnknown *pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    if (!ppSwapChain || !pDesc || !hWnd || !pDevice)
      return DXGI_ERROR_INVALID_CALL;

    Com<IMTLDXGIDevice> metal_dxgi_device;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&metal_dxgi_device)))) {
      ERR("Unsupported device type");
      return DXGI_ERROR_UNSUPPORTED;
    }

    DXGI_SWAP_CHAIN_DESC1 desc = *pDesc;

    wsi::getWindowSize(hWnd, desc.Width ? nullptr : &desc.Width,
                       desc.Height ? nullptr : &desc.Height);
    if (!desc.Width || !desc.Height) {
      WARN("CreateSwapChainForHwnd: using fallback swapchain size ",
           desc.Width ? desc.Width : 1, "x", desc.Height ? desc.Height : 1);
      desc.Width = desc.Width ? desc.Width : 1;
      desc.Height = desc.Height ? desc.Height : 1;
    }
    if (desc.Format == DXGI_FORMAT_UNKNOWN) {
      WARN("CreateSwapChainForHwnd: swapchain format must not be UNKNOWN");
      return DXGI_ERROR_INVALID_CALL;
    }
    if (!desc.BufferCount) {
      WARN("CreateSwapChainForHwnd: defaulting zero buffer count to 2");
      desc.BufferCount = 2;
    }
    if (desc.BufferCount > DXGI_MAX_SWAP_CHAIN_BUFFERS) {
      WARN("CreateSwapChainForHwnd: invalid buffer count ", desc.BufferCount);
      return DXGI_ERROR_INVALID_CALL;
    }
    if (!desc.SampleDesc.Count)
      desc.SampleDesc.Count = 1;
    if (desc.Flags & ~SupportedSwapChainFlags) {
      WARN("CreateSwapChainForHwnd: unsupported swapchain flags ",
           desc.Flags & ~SupportedSwapChainFlags);
      return DXGI_ERROR_UNSUPPORTED;
    }
    if (!IsKnownSwapEffect(desc.SwapEffect)) {
      WARN("CreateSwapChainForHwnd: unknown swap effect ", desc.SwapEffect);
      return DXGI_ERROR_INVALID_CALL;
    }
    if ((desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
         desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL) &&
        desc.BufferCount < 2) {
      WARN("CreateSwapChainForHwnd: flip swapchains usually require at least "
           "two buffers");
    }

    // If necessary, set up a default set of
    // fullscreen parameters for the swap chain
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc;

    if (pFullscreenDesc) {
      fsDesc = *pFullscreenDesc;
    } else {
      fsDesc.RefreshRate = {0, 0};
      fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
      fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
      fsDesc.Windowed = TRUE;
    }

    return metal_dxgi_device->CreateSwapChain(this, hWnd, &desc, &fsDesc,
                                              ppSwapChain);
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(
      IUnknown *pDevice, IUnknown *pWindow, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    WARN("CreateSwapChainForCoreWindow is unsupported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(
      IUnknown *pDevice, const DXGI_SWAP_CHAIN_DESC1 *pDesc,
      IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) final {
    InitReturnPtr(ppSwapChain);

    WARN("CreateSwapChainForComposition is unsupported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters(UINT Adapter,
                                         IDXGIAdapter **ppAdapter) final {
    fh4bypass::ApplyBadFiberDataBypass();
    InitReturnPtr(ppAdapter);

    if (ppAdapter == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    IDXGIAdapter1 *handle = nullptr;
    HRESULT hr = this->EnumAdapters1(Adapter, &handle);
    *ppAdapter = handle;
    fh4bypass::ApplyBadFiberDataBypass();
    return hr;
  }

  HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT Adapter,
                                          IDXGIAdapter1 **ppAdapter) final {
    fh4bypass::ApplyBadFiberDataBypass();
    InitReturnPtr(ppAdapter);

    auto candidates = EnumerateAdapterCandidates();
    UINT adapter_count = UINT(candidates.size());

    if (DxgiBootstrapDiagEnabled()) {
      WARN("DXGI bootstrap diagnostic: EnumAdapters1 request adapter=", Adapter,
           " deviceCount=", adapter_count);
    }

    if (Adapter >= adapter_count) {
      if (DxgiBootstrapDiagEnabled()) {
        WARN("DXGI bootstrap diagnostic: EnumAdapters1 returning "
             "DXGI_ERROR_NOT_FOUND for adapter=",
             Adapter, " deviceCount=", adapter_count);
      }
      fh4bypass::ApplyBadFiberDataBypass();
      return DXGI_ERROR_NOT_FOUND;
    }

    UINT adjusted_adapter = Adapter;
    if (adapter_count > 1) {
      UINT preferred_adapter = 0;
      for (unsigned i = 0; i < adapter_count; i++) {
        if (!candidates[i].info.has_unified_memory)
          preferred_adapter = i;
      }
      if (Adapter == 0)
        adjusted_adapter = preferred_adapter;
      else
        adjusted_adapter = Adapter <= preferred_adapter ? Adapter - 1 : Adapter;
    }

    if (DxgiBootstrapDiagEnabled()) {
      WARN("DXGI bootstrap diagnostic: EnumAdapters1 selected adjustedAdapter=",
           adjusted_adapter, " backend=", candidates[adjusted_adapter].provider.name
               ? candidates[adjusted_adapter].provider.name
               : "<unnamed>",
           " unifiedMemory=", candidates[adjusted_adapter].info.has_unified_memory,
           " registryID=", candidates[adjusted_adapter].info.registry_id);
    }

    *ppAdapter = CreateAdapter(candidates[adjusted_adapter].provider,
                               candidates[adjusted_adapter].info, this,
                               Config::getInstance());
    fh4bypass::ApplyBadFiberDataBypass();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND *pWindowHandle) final {
    if (pWindowHandle == nullptr)
      return DXGI_ERROR_INVALID_CALL;

    *pWindowHandle = associated_window_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE hResource,
                                                         LUID *pLuid) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND WindowHandle,
                                                  UINT Flags) final {
    if (Flags & ~DXGI_MWA_VALID) {
      WARN("MakeWindowAssociation: unsupported flags ",
           Flags & ~DXGI_MWA_VALID);
      return DXGI_ERROR_INVALID_CALL;
    }
    associated_window_ = WindowHandle;
    return S_OK;
  }

  BOOL STDMETHODCALLTYPE IsCurrent() final { return TRUE; }

  HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(
      HWND WindowHandle, UINT wMsg, DWORD *pdwCookie) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE hEvent,
                                                      DWORD *pdwCookie) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND WindowHandle,
                                                       UINT wMsg,
                                                       DWORD *pdwCookie) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD *pdwCookie) final {
    ERR("Not implemented");
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD dwCookie) final {
    ERR("Not implemented");
  }

  void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD dwCookie) final {
    ERR("Not implemented");
  }

  UINT STDMETHODCALLTYPE GetCreationFlags() override { return flags_; }

  HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID luid, REFIID iid,
                                              void **adapter) override {
    InitReturnPtr(adapter);
    if (!adapter)
      return DXGI_ERROR_INVALID_CALL;

    auto candidates = EnumerateAdapterCandidates();
    for (const auto &candidate : candidates) {
      if (!IsEqualLuid(GetAdapterLuid(candidate.info.registry_id), luid))
        continue;

      auto dxgi_adapter = CreateAdapter(candidate.provider, candidate.info,
                                        this, Config::getInstance());
      return dxgi_adapter->QueryInterface(iid, adapter);
    }

    return DXGI_ERROR_NOT_FOUND;
  }

  HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID iid,
                                            void **adapter) override {
    ERR("DXGIFactory::EnumWrapAdapter: not implemented");
    return DXGI_ERROR_NOT_FOUND;
  };

  HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(DXGI_FEATURE Feature, void *pFeatureSupportData,
                      UINT FeatureSupportDataSize) override {
    switch (Feature) {
    case DXGI_FEATURE_PRESENT_ALLOW_TEARING: {
      auto info = static_cast<BOOL *>(pFeatureSupportData);

      if (FeatureSupportDataSize != sizeof(*info))
        return ERR_E_INVALIDARG(__func__);

      *info = TRUE;
      return S_OK;
    }
    default: {
      ERR("DXGIFactory::CheckFeatureSupport: unknown feature ", Feature);
      return ERR_E_INVALIDARG(__func__);
    }
    }
  };

  HRESULT STDMETHODCALLTYPE
  EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                             REFIID riid, void **ppvAdapter) override {
    // GpuPreference ignored, since Apple Silicon has only 1 GPU anyway
    // FIXME: support Intel Mac with dedicated GPU
    Com<IDXGIAdapter1> adapter;
    HRESULT hr = this->EnumAdapters1(Adapter, &adapter);

    if (FAILED(hr))
      return hr;
    return adapter->QueryInterface(riid, ppvAdapter);
  };

private:
  UINT flags_;

  HWND associated_window_ = nullptr;
};

extern "C" HRESULT __stdcall CreateDXGIFactory2(UINT Flags, REFIID riid,
                                                void **ppFactory) {
  try {
    if (DxgiBootstrapDiagEnabled()) {
      WARN("DXGI bootstrap diagnostic: CreateDXGIFactory2 flags=", Flags,
           " riid=", str::format(riid));
    }

    MTLDXGIFactory* factory = new MTLDXGIFactory(Flags);
    HRESULT hr = factory->QueryInterface(riid, ppFactory);
    factory->Release();

    if (FAILED(hr)) {
      if (DxgiBootstrapDiagEnabled()) {
        WARN("DXGI bootstrap diagnostic: CreateDXGIFactory2 QueryInterface "
             "failed hr=",
             hr);
      }
      return hr;
    }

    return S_OK;
  } catch (const MTLD3DError &e) {
    Logger::err(e.message());
    return E_FAIL;
  }
}

extern "C" HRESULT __stdcall CreateDXGIFactory1(REFIID riid, void **ppFactory) {
  return CreateDXGIFactory2(0, riid, ppFactory);
}

extern "C" HRESULT __stdcall CreateDXGIFactory(REFIID riid, void **factory) {
  return CreateDXGIFactory2(0, riid, factory);
}

} // namespace dxmt
