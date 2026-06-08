#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_backend.hpp"
#include "dxgi_options.hpp"
#include "util_string.hpp"
#include "log/log.hpp"
#include "wsi_monitor.hpp"
#include "dxgi_interfaces.h"
#include "dxgi_object.hpp"
#include "d3d10_1.h"
#include "util_fh4_bypass.hpp"

#include <cstring>

namespace dxmt {

Com<IDXGIOutput> CreateOutput(IMTLDXGIAdapter *pAadapter, HMONITOR monitor, DxgiOptions &options);

LUID GetAdapterLuid(uint64_t registry_id) {
    // NOTE: use big-endian registryID, be consistent with MVK
  return std::bit_cast<LUID>(__builtin_bswap64(registry_id));
}

class MTLDXGIAdapter : public MTLDXGIObject<IMTLDXGIAdapter> {
public:
  MTLDXGIAdapter(const DxgiBackendProvider &provider,
                 const DxgiBackendAdapterInfo &info,
                 IDXGIFactory *factory, Config &config)
      : provider_(provider), info_(info), factory_(factory), options_(config) {
    if (info_.device_handle && provider_.retain_device)
      provider_.retain_device(info_.device_handle);
    if (!info_.device_handle || !info_.registry_id)
      return;
    D3DKMT_OPENADAPTERFROMLUID open = {};
    open.AdapterLuid = GetAdapterLuid(info_.registry_id);
    if (D3DKMTOpenAdapterFromLuid(&open))
      WARN("Failed to open D3DKMT adapter");
    else
      local_kmt_ = open.hAdapter;
  };

  ~MTLDXGIAdapter() {
    if (local_kmt_) {
      D3DKMT_CLOSEADAPTER close = {};
      close.hAdapter = local_kmt_;
      if (D3DKMTCloseAdapter(&close))
        WARN("Failed to close D3DKMT adapter");
    }
    if (info_.device_handle && provider_.release_device)
      provider_.release_device(info_.device_handle);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    fh4bypass::ApplyBadFiberDataBypass();

    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
        riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1) ||
        riid == __uuidof(IDXGIAdapter2) || riid == __uuidof(IDXGIAdapter3) ||
        riid == __uuidof(IDXGIAdapter4) || riid == __uuidof(IMTLDXGIAdapter)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(IDXGIAdapter), riid)) {
      WARN("DXGIAdapter: Unknown interface query ", str::format(riid));
    }

    return E_NOINTERFACE;
  };

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) final {
    return factory_->QueryInterface(riid, ppParent);
  }
  HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC *pDesc) final {
    if (pDesc == nullptr)
      return ERR_E_INVALIDARG(__func__);

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
    }

    return hr;
  }
  HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1 *pDesc) final {
    if (pDesc == nullptr)
      return ERR_E_INVALIDARG(__func__);

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc2(DXGI_ADAPTER_DESC2 *pDesc) final {
    if (pDesc == nullptr)
      return ERR_E_INVALIDARG(__func__);

    DXGI_ADAPTER_DESC3 desc;
    HRESULT hr = GetDesc3(&desc);

    if (SUCCEEDED(hr)) {
      std::memcpy(pDesc->Description, desc.Description, sizeof(pDesc->Description));
      pDesc->VendorId = desc.VendorId;
      pDesc->DeviceId = desc.DeviceId;
      pDesc->SubSysId = desc.SubSysId;
      pDesc->Revision = desc.Revision;
      pDesc->DedicatedVideoMemory = desc.DedicatedVideoMemory;
      pDesc->DedicatedSystemMemory = desc.DedicatedSystemMemory;
      pDesc->SharedSystemMemory = desc.SharedSystemMemory;
      pDesc->AdapterLuid = desc.AdapterLuid;
      pDesc->Flags = desc.Flags & 0b11;
      pDesc->GraphicsPreemptionGranularity = desc.GraphicsPreemptionGranularity;
      pDesc->ComputePreemptionGranularity = desc.ComputePreemptionGranularity;
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE GetDesc3(DXGI_ADAPTER_DESC3 *pDesc) final {
    if (pDesc == nullptr)
      return ERR_E_INVALIDARG(__func__);

    std::memset(pDesc->Description, 0, sizeof(pDesc->Description));

    if (!options_.customDeviceDesc.empty()) {
      str::transcodeString(
          pDesc->Description,
          sizeof(pDesc->Description) / sizeof(pDesc->Description[0]) - 1,
          options_.customDeviceDesc.c_str(), options_.customDeviceDesc.size());
    } else {
      std::memcpy(pDesc->Description, info_.name, sizeof(pDesc->Description));
    }

    if (options_.customVendorId >= 0) {
      pDesc->VendorId = options_.customVendorId;
    } else {
      pDesc->VendorId = 0x106B;
      if (g_extension_enabled == VendorExtension::Nvidia) {
        pDesc->VendorId = 0x10DE;
      }
    }

    if (options_.customDeviceId >= 0) {
      pDesc->DeviceId = options_.customDeviceId;
    } else {
      pDesc->DeviceId = 0;
    }

    pDesc->SubSysId = 0;
    pDesc->Revision = 0;
    if (info_.has_unified_memory)
      pDesc->DedicatedVideoMemory = info_.recommended_max_working_set_size / 2; // FIXME: use a more appropriate value
    else
      pDesc->DedicatedVideoMemory = info_.recommended_max_working_set_size;
    pDesc->DedicatedSystemMemory = 0;
    pDesc->SharedSystemMemory = 0;
    pDesc->AdapterLuid = GetAdapterLuid(info_.registry_id);
    pDesc->Flags = DXGI_ADAPTER_FLAG3_NONE;
    pDesc->GraphicsPreemptionGranularity = DXGI_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
    pDesc->ComputePreemptionGranularity = DXGI_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumOutputs(UINT Output,
                                        IDXGIOutput **ppOutput) final {
    fh4bypass::ApplyBadFiberDataBypass();
    InitReturnPtr(ppOutput);

    if (ppOutput == nullptr) {
      fh4bypass::ApplyBadFiberDataBypass();
      return ERR_E_INVALIDARG(__func__);
    }

    HMONITOR monitor = wsi::enumMonitors(Output);
    if (monitor == nullptr) {
      fh4bypass::ApplyBadFiberDataBypass();
      return DXGI_ERROR_NOT_FOUND;
    }

    *ppOutput = CreateOutput(this, monitor, options_);
    fh4bypass::ApplyBadFiberDataBypass();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE
  CheckInterfaceSupport(const GUID &guid, LARGE_INTEGER *umd_version) final {
    HRESULT hr = DXGI_ERROR_UNSUPPORTED;

    if (guid == __uuidof(IDXGIDevice) || guid == __uuidof(ID3D10Device) ||
        guid == __uuidof(ID3D10Device1))
      hr = S_OK;

    // We can't really reconstruct the version numbers
    // returned by Windows drivers from Metal
    if (SUCCEEDED(hr) && umd_version)
      umd_version->QuadPart = ~0ull;

    if (FAILED(hr)) {
      Logger::err("DXGI: CheckInterfaceSupport: Unsupported interface");
      Logger::err(str::format(guid));
    }

    return hr;
  }

  HRESULT STDMETHODCALLTYPE
  RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE event,
                                                       DWORD *cookie) override {
    if (cookie)
      *cookie = 0;
    WARN("DXGIAdapter: hardware content protection teardown notifications are unsupported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  void STDMETHODCALLTYPE
  UnregisterHardwareContentProtectionTeardownStatus(DWORD cookie) override {
    WARN("DXGIAdapter: hardware content protection teardown notifications are unsupported");
  }

  HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      DXGI_QUERY_VIDEO_MEMORY_INFO *pVideoMemoryInfo) override {
    if (NodeIndex > 0 || !pVideoMemoryInfo)
      return ERR_E_INVALIDARG(__func__);

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return ERR_E_INVALIDARG(__func__);

    // we don't actually care about MemorySegmentGroup
    pVideoMemoryInfo->Budget = info_.recommended_max_working_set_size;
    pVideoMemoryInfo->CurrentUsage = info_.current_allocated_size;
    pVideoMemoryInfo->AvailableForReservation = 0;
    pVideoMemoryInfo->CurrentReservation =
        mem_reserved_[uint32_t(MemorySegmentGroup)];
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetVideoMemoryReservation(
      UINT NodeIndex, DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
      UINT64 Reservation) override {
    if (NodeIndex > 0)
      return ERR_E_INVALIDARG(__func__);

    if (MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_LOCAL &&
        MemorySegmentGroup != DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL)
      return ERR_E_INVALIDARG(__func__);

    mem_reserved_[uint32_t(MemorySegmentGroup)] = Reservation;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE RegisterVideoMemoryBudgetChangeNotificationEvent(
      HANDLE event, DWORD *cookie) override {
    if (cookie)
      *cookie = 0;
    WARN("DXGIAdapter: video memory budget change notifications are unsupported");
    return DXGI_ERROR_UNSUPPORTED;
  }

  void STDMETHODCALLTYPE
  UnregisterVideoMemoryBudgetChangeNotification(DWORD cookie) override {
    WARN("DXGIAdapter: video memory budget change notifications are unsupported");
  }

  DxgiBackendKind STDMETHODCALLTYPE GetBackendKind() final {
    return provider_.kind;
  }

  uint64_t STDMETHODCALLTYPE GetMetalDeviceHandle() final {
    return info_.device_handle;
  }

  uint32_t STDMETHODCALLTYPE GetBackendAdapterIndex() final {
    return info_.backend_index;
  }

  bool STDMETHODCALLTYPE
  ResolveBackendAdapter(const DxgiBackendProvider *provider) final {
    if (!provider || !provider->get_adapter_info) {
      WARN("DXGIAdapter: backend resolve failed: provider unavailable");
      return false;
    }
    if (provider_.kind != DxgiBackendKind::Unknown &&
        provider->kind != provider_.kind) {
      WARN("DXGIAdapter: backend resolve failed: kind mismatch current=",
           uint32_t(provider_.kind), " requested=", uint32_t(provider->kind));
      return false;
    }
    if (info_.device_handle)
      return true;

    DxgiBackendAdapterInfo resolved = {};
    if (!provider->get_adapter_info(info_.backend_index, &resolved) ||
        !resolved.device_handle) {
      WARN("DXGIAdapter: backend resolve failed: no device for provider=",
           provider->name ? provider->name : "<unnamed>",
           " index=", info_.backend_index);
      return false;
    }
    resolved.backend_index = info_.backend_index;

    provider_ = *provider;
    info_ = resolved;
    if (provider_.retain_device)
      provider_.retain_device(info_.device_handle);

    if (info_.registry_id && !local_kmt_) {
      D3DKMT_OPENADAPTERFROMLUID open = {};
      open.AdapterLuid = GetAdapterLuid(info_.registry_id);
      if (D3DKMTOpenAdapterFromLuid(&open))
        WARN("Failed to open D3DKMT adapter");
      else
        local_kmt_ = open.hAdapter;
    }

    WARN("DXGIAdapter: resolved backend provider=",
         provider_.name ? provider_.name : "<unnamed>",
         " index=", info_.backend_index,
         " registryID=", info_.registry_id);
    return true;
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() final { return local_kmt_; }

  bool STDMETHODCALLTYPE IsBackBufferFormatSupported(DXGI_FORMAT Format) final {
    if (!info_.device_handle)
      return true;
    return provider_.is_backbuffer_format_supported
               ? provider_.is_backbuffer_format_supported(info_.device_handle, Format)
               : true;
  }

private:
  DxgiBackendProvider provider_;
  DxgiBackendAdapterInfo info_;
  D3DKMT_HANDLE local_kmt_ = 0;
  Com<IDXGIFactory> factory_;
  DxgiOptions options_;
  uint64_t mem_reserved_[2] = {0, 0};
};

Com<IMTLDXGIAdapter> CreateAdapter(const DxgiBackendProvider &provider,
                                   const DxgiBackendAdapterInfo &info,
                                   IDXGIFactory2 *pFactory, Config &config) {
  return Com<IMTLDXGIAdapter>::transfer(
      new MTLDXGIAdapter(provider, info, pFactory, config));
}

} // namespace dxmt
