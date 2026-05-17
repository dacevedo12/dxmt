#include "d3d12_agility.hpp"

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "log/log.hpp"
#include "util_error.hpp"
#include "util_fh4_bypass.hpp"
#include "util_string.hpp"
#include <windows.h>
#include <d3d12.h>

dxmt::Logger dxmt::Logger::s_instance("d3d12core.log");

#ifdef _WIN32
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason,
                               LPVOID reserved) {
  if (reason != DLL_PROCESS_ATTACH)
    return TRUE;

  dxmt::fh4bypass::ApplyBadFiberDataBypass();
  DisableThreadLibraryCalls(instance);
  return TRUE;
}
#endif

namespace dxmt::d3d12 {

class DredSettingsImpl final
    : public ComObjectWithInitialRef<ID3D12DeviceRemovedExtendedDataSettings1> {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    InitReturnPtr(object);
    if (!object)
      return E_POINTER;

    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(ID3D12DeviceRemovedExtendedDataSettings) ||
        riid == __uuidof(ID3D12DeviceRemovedExtendedDataSettings1)) {
      *object = ref(static_cast<ID3D12DeviceRemovedExtendedDataSettings1 *>(this));
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  void STDMETHODCALLTYPE SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT enablement) override {
    breadcrumbs_ = enablement;
  }

  void STDMETHODCALLTYPE SetPageFaultEnablement(D3D12_DRED_ENABLEMENT enablement) override {
    page_fault_ = enablement;
  }

  void STDMETHODCALLTYPE SetWatsonDumpEnablement(D3D12_DRED_ENABLEMENT enablement) override {
    watson_ = enablement;
  }

  void STDMETHODCALLTYPE SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT enablement) override {
    breadcrumb_context_ = enablement;
  }

private:
  D3D12_DRED_ENABLEMENT breadcrumbs_ = D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED;
  D3D12_DRED_ENABLEMENT page_fault_ = D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED;
  D3D12_DRED_ENABLEMENT watson_ = D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED;
  D3D12_DRED_ENABLEMENT breadcrumb_context_ = D3D12_DRED_ENABLEMENT_SYSTEM_CONTROLLED;
};

using PFN_D3D12CreateDevice = HRESULT (__stdcall *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
using PFN_D3D12SerializeVersionedRootSignature = HRESULT (__stdcall *)(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *, ID3DBlob **, ID3DBlob **);
using PFN_D3D12CreateVersionedRootSignatureDeserializer = HRESULT (__stdcall *)(
    const void *, SIZE_T, REFIID, void **);
using PFN_D3D12CreateRootSignatureDeserializerFromSubobjectInLibrary = HRESULT (__stdcall *)(
    const void *, SIZE_T, LPCWSTR, REFIID, void **);

template <typename Proc>
static Proc
GetD3D12Proc(const char *name) {
#if defined(__MINGW32__) || defined(_WIN32)
  HMODULE module = LoadLibraryA("d3d12.dll");
  if (!module) {
    Logger::err(str::format("D3D12Core: failed to load d3d12.dll for ", name));
    return nullptr;
  }

  auto proc = reinterpret_cast<Proc>(GetProcAddress(module, name));
  if (!proc)
    Logger::err(str::format("D3D12Core: d3d12.dll does not export ", name));
  return proc;
#else
  Logger::warn(str::format("D3D12Core: ", name,
                           " is only available in the Wine DLL build"));
  return nullptr;
#endif
}

class DeviceFactoryImpl final
    : public ComObjectWithInitialRef<ID3D12DeviceFactory,
                                     ID3D12DeviceConfiguration1> {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    InitReturnPtr(object);
    if (!object)
      return E_POINTER;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12DeviceFactory)) {
      *object = ref(static_cast<ID3D12DeviceFactory *>(this));
      return S_OK;
    }

    if (riid == __uuidof(ID3D12DeviceConfiguration) ||
        riid == __uuidof(ID3D12DeviceConfiguration1)) {
      *object = ref(static_cast<ID3D12DeviceConfiguration1 *>(this));
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE InitializeFromGlobalState() override {
    flags_ = D3D12_DEVICE_FACTORY_FLAG_NONE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ApplyToGlobalState() override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetFlags(D3D12_DEVICE_FACTORY_FLAGS flags) override {
    flags_ = flags;
    return S_OK;
  }

  D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE GetFlags() override {
    return flags_;
  }

  HRESULT STDMETHODCALLTYPE GetConfigurationInterface(REFCLSID clsid,
                                                      REFIID riid,
                                                      void **object) override {
    InitReturnPtr(object);
    if (!object)
      return S_FALSE;

    if (clsid == kCLSID_D3D12DeviceRemovedExtendedData) {
      auto dred = Com<ID3D12DeviceRemovedExtendedDataSettings1>::transfer(
          new DredSettingsImpl());
      return dred->QueryInterface(riid, object);
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE EnableExperimentalFeatures(UINT feature_count,
                                                       const IID *iids,
                                                       void *configs,
                                                       UINT *config_sizes) override {
    if (feature_count && !iids)
      return WARN_E_INVALIDARG(__func__);
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE CreateDevice(IUnknown *adapter,
                                         D3D_FEATURE_LEVEL feature_level,
                                         REFIID riid,
                                         void **device) override {
    auto create_device =
        GetD3D12Proc<PFN_D3D12CreateDevice>("DXMTCreateD3D12DeviceFromFactory");
    if (!create_device)
      return E_NOINTERFACE;
    return create_device(adapter, feature_level, riid, device);
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_DEVICE_CONFIGURATION_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DEVICE_CONFIGURATION_DESC *__ret) override {
    *__ret = GetDescImpl();
    return __ret;
  }
#else
  D3D12_DEVICE_CONFIGURATION_DESC STDMETHODCALLTYPE GetDesc() override {
    return GetDescImpl();
  }
#endif

  HRESULT STDMETHODCALLTYPE GetEnabledExperimentalFeatures(GUID *guids,
                                                           UINT guid_count) override {
    if (guid_count && !guids)
      return WARN_E_INVALIDARG(__func__);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SerializeVersionedRootSignature(
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob,
      ID3DBlob **error_blob) override {
    auto serialize = GetD3D12Proc<PFN_D3D12SerializeVersionedRootSignature>(
        "D3D12SerializeVersionedRootSignature");
    if (!serialize)
      return E_NOINTERFACE;
    return serialize(desc, blob, error_blob);
  }

  HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializer(
      const void *blob, SIZE_T size, REFIID riid, void **deserializer) override {
    auto create_deserializer =
        GetD3D12Proc<PFN_D3D12CreateVersionedRootSignatureDeserializer>(
            "D3D12CreateVersionedRootSignatureDeserializer");
    if (!create_deserializer)
      return E_NOINTERFACE;
    return create_deserializer(blob, size, riid, deserializer);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *library_blob, SIZE_T size, LPCWSTR subobject_name,
      REFIID riid, void **deserializer) override {
    auto create_deserializer =
        GetD3D12Proc<PFN_D3D12CreateRootSignatureDeserializerFromSubobjectInLibrary>(
            "DXMTCreateRootSignatureDeserializerFromSubobjectInLibrary");
    if (!create_deserializer)
      return E_NOINTERFACE;
    return create_deserializer(library_blob, size, subobject_name, riid,
                               deserializer);
  }

private:
  static D3D12_DEVICE_CONFIGURATION_DESC GetDescImpl() {
    D3D12_DEVICE_CONFIGURATION_DESC desc = {};
    desc.Flags = D3D12_DEVICE_FLAG_NONE;
    desc.SDKVersion = kAgilitySdkVersion;
    return desc;
  }

  D3D12_DEVICE_FACTORY_FLAGS flags_ = D3D12_DEVICE_FACTORY_FLAG_NONE;
};

} // namespace dxmt::d3d12

extern "C" HRESULT __stdcall
DXMTCreateD3D12DeviceFactory(REFIID riid, void **factory) {
  dxmt::InitReturnPtr(factory);
  if (!factory)
    return E_POINTER;

  auto object = dxmt::Com<ID3D12DeviceFactory>::transfer(
      new dxmt::d3d12::DeviceFactoryImpl());
  return object->QueryInterface(riid, factory);
}
