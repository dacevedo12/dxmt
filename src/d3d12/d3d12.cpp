#include "d3d12_private.h"

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d12_agility.hpp"
#include "d3d12_device.hpp"
#include "d3d12_root_signature.hpp"
#include "dxgi_interfaces.h"
#include "dxmt_device.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_error.hpp"
#include "util_fh4_bypass.hpp"
#include "util_string.hpp"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <mutex>

namespace dxmt {
Logger Logger::s_instance("d3d12.log");
}

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

static HRESULT
CreateD3D12DeviceInstance(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level,
                          REFIID riid, void **device,
                          bool use_global_singleton = false);

using PFN_DXMTCreateD3D12DeviceFactory = HRESULT (__stdcall *)(REFIID, void **);

static HRESULT
CreateCoreDeviceFactory(REFIID riid, void **factory) {
  InitReturnPtr(factory);
  if (!factory)
    return E_POINTER;

#if defined(__MINGW32__) || defined(_WIN32)
  HMODULE core = LoadLibraryA("d3d12core.dll");
  if (!core) {
    Logger::err("D3D12SDKConfiguration: failed to load d3d12core.dll");
    return HRESULT_FROM_WIN32(GetLastError());
  }

  auto create_factory = reinterpret_cast<PFN_DXMTCreateD3D12DeviceFactory>(
      GetProcAddress(core, "DXMTCreateD3D12DeviceFactory"));
  if (!create_factory) {
    Logger::err("D3D12SDKConfiguration: d3d12core.dll does not export DXMTCreateD3D12DeviceFactory");
    return E_NOINTERFACE;
  }

  return create_factory(riid, factory);
#else
  Logger::warn("D3D12SDKConfiguration: device factory is only available in the Wine DLL build");
  return E_NOINTERFACE;
#endif
}

class SDKConfigurationImpl final : public ComObjectWithInitialRef<ID3D12SDKConfiguration1> {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    InitReturnPtr(object);
    if (!object)
      return E_POINTER;

    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(ID3D12SDKConfiguration) ||
        riid == __uuidof(ID3D12SDKConfiguration1)) {
      *object = ref(static_cast<ID3D12SDKConfiguration1 *>(this));
      return S_OK;
    }

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE SetSDKVersion(UINT version, const char *path) override {
    Logger::info(str::format(
        "D3D12SDKConfiguration: accepting SDK version request version=",
        version, ", path=", path ? path : "<null>"));
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT version, LPCSTR path,
                                                REFIID riid,
                                                void **factory) override {
    Logger::info(str::format(
        "D3D12SDKConfiguration: creating device factory for SDK version ",
        version, ", path=", path ? path : "<null>"));
    return CreateCoreDeviceFactory(riid, factory);
  }

  void STDMETHODCALLTYPE FreeUnusedSDKs() override {}
};

SupportGateResult
CheckSupportGate(WMT::Device device) {
  SupportGateResult result = {
      .status = SupportGateStatus::Enabled,
      .env_value = env::getEnvVar(kExperimentD3D12SupportEnv),
      .supports_apple_gpu_family_7 = device.supportsFamily(WMTGPUFamilyApple7),
      .supports_metal_4 = device.supportsFamily(WMTGPUFamilyMetal4),
      .os_version = WMT::GetOperatingSystemVersion(),
  };

  if (result.env_value != "1")
    result.status = SupportGateStatus::DisabledByEnv;
  else if (!result.supports_apple_gpu_family_7)
    result.status = SupportGateStatus::DisabledByAppleGpuFamily;
  else if (!result.supports_metal_4)
    result.status = SupportGateStatus::DisabledByMetal4;
  else if (result.os_version.major < 26)
    result.status = SupportGateStatus::DisabledByMacOS;

  return result;
}

static const char *
SupportGateStatusName(SupportGateStatus status) {
  switch (status) {
  case SupportGateStatus::Enabled:
    return "enabled";
  case SupportGateStatus::DisabledByEnv:
    return "disabled by DXMT_EXPERIMENT_DX12_SUPPORT";
  case SupportGateStatus::DisabledByAppleGpuFamily:
    return "requires Apple GPU family 7";
  case SupportGateStatus::DisabledByMetal4:
    return "requires Metal 4";
  case SupportGateStatus::DisabledByMacOS:
    return "requires macOS 26";
  }
  return "unknown";
}

static const char *
BoolString(bool value) {
  return value ? "true" : "false";
}

static std::string
EnvValueForLog(const std::string &value) {
  return value.empty() ? "<unset>" : value;
}

static std::string
FormatSupportGateFailure(const SupportGateResult &gate) {
  return str::format(
      "D3D12CreateDevice: experimental D3D12 support unavailable: ",
      SupportGateStatusName(gate.status), "; requirements: ",
      kExperimentD3D12SupportEnv, "=1, Apple GPU family 7=true, Metal 4=true, macOS>=26.0.0",
      "; actual: ", kExperimentD3D12SupportEnv, "=", EnvValueForLog(gate.env_value),
      ", Apple GPU family 7=", BoolString(gate.supports_apple_gpu_family_7),
      ", Metal 4=", BoolString(gate.supports_metal_4),
      ", macOS=", gate.os_version.major, ".", gate.os_version.minor, ".", gate.os_version.patch);
}

static HRESULT
CreateD3D12DeviceInstance(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level,
                          REFIID riid, void **device,
                          bool use_global_singleton) {
  dxmt::InitReturnPtr(device);

  dxmt::Com<IDXGIAdapter> dxgi_adapter = nullptr;
  dxmt::Com<IDXGIFactory1> dxgi_factory = nullptr;

  if (adapter) {
    if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter)))) {
      dxmt::Logger::err("D3D12CreateDevice: adapter is not a DXGI adapter");
      return WARN_E_INVALIDARG(__func__);
    }
  } else {
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr))
      return hr;

    hr = dxgi_factory->EnumAdapters(0, &dxgi_adapter);
    if (FAILED(hr))
      return hr;
  }

  dxmt::Com<IMTLDXGIAdapter> metal_adapter = nullptr;
  if (FAILED(dxgi_adapter->QueryInterface(IID_PPV_ARGS(&metal_adapter)))) {
    dxmt::Logger::err("D3D12CreateDevice: not a DXMT adapter");
    return WARN_E_INVALIDARG(__func__);
  }

  const auto gate = dxmt::d3d12::CheckSupportGate(metal_adapter->GetMTLDevice());
  if (gate.status != dxmt::d3d12::SupportGateStatus::Enabled) {
    dxmt::Logger::err(dxmt::d3d12::FormatSupportGateFailure(gate));
    return DXGI_ERROR_UNSUPPORTED;
  }

  constexpr D3D_FEATURE_LEVEL supported_feature_level = D3D_FEATURE_LEVEL_12_0;
  if (minimum_feature_level < D3D_FEATURE_LEVEL_11_0 ||
      minimum_feature_level > supported_feature_level) {
    dxmt::Logger::err(dxmt::str::format(
        "D3D12CreateDevice: requested feature level ", minimum_feature_level,
        " is outside supported D3D12 range [", D3D_FEATURE_LEVEL_11_0,
        ", ", supported_feature_level, "]"));
    return WARN_E_INVALIDARG(__func__);
  }

  if (!device)
    return S_FALSE;

  dxmt::Logger::info(dxmt::str::format(
      "D3D12CreateDevice: experimental D3D12 support gate passed, minimum feature level ",
      minimum_feature_level, ", riid ", dxmt::str::format(riid)));

  try {
    static std::mutex singleton_mutex;
    static Com<IMTLD3D12Device, false> singleton_device;

    if (use_global_singleton) {
      std::lock_guard lock(singleton_mutex);
      if (singleton_device)
        return singleton_device->QueryInterface(riid, device);

      singleton_device = dxmt::d3d12::CreateD3D12Device(
          dxmt::CreateDXMTDevice({.device = metal_adapter->GetMTLDevice()}),
          metal_adapter.ptr()).prvRef();
      return singleton_device->QueryInterface(riid, device);
    }

    auto d3d12_device = dxmt::d3d12::CreateD3D12Device(
        dxmt::CreateDXMTDevice({.device = metal_adapter->GetMTLDevice()}),
        metal_adapter.ptr());

    return d3d12_device->QueryInterface(riid, device);
  } catch (const dxmt::MTLD3DError &e) {
    dxmt::Logger::err(dxmt::str::format("D3D12CreateDevice: failed to create device: ", e.message()));
    return E_FAIL;
  }
}

} // namespace dxmt::d3d12

extern "C" HRESULT __stdcall
D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level, REFIID riid, void **device) {
  return dxmt::d3d12::CreateD3D12DeviceInstance(
      adapter, minimum_feature_level, riid, device, true);
}

extern "C" HRESULT __stdcall
DXMTCreateD3D12DeviceFromFactory(IUnknown *adapter,
                                 D3D_FEATURE_LEVEL minimum_feature_level,
                                 REFIID riid, void **device) {
  return dxmt::d3d12::CreateD3D12DeviceInstance(
      adapter, minimum_feature_level, riid, device, false);
}

extern "C" HRESULT __stdcall
D3D12GetDebugInterface(REFIID riid, void **debug) {
  dxmt::InitReturnPtr(debug);
  dxmt::Logger::warn(dxmt::str::format(
      "D3D12GetDebugInterface: debug layer is not supported for riid ",
      dxmt::str::format(riid)));
  return E_NOINTERFACE;
}

extern "C" HRESULT __stdcall
D3D12GetInterface(REFCLSID clsid, REFIID riid, void **object) {
  dxmt::InitReturnPtr(object);
  if (!object)
    return E_POINTER;

  if (clsid == dxmt::d3d12::kCLSID_D3D12SDKConfiguration) {
    auto configuration =
        dxmt::Com<ID3D12SDKConfiguration1>::transfer(new dxmt::d3d12::SDKConfigurationImpl());
    return configuration->QueryInterface(riid, object);
  }

  if (clsid == dxmt::d3d12::kCLSID_D3D12DeviceFactory) {
    return dxmt::d3d12::CreateCoreDeviceFactory(riid, object);
  }

  dxmt::Logger::warn(dxmt::str::format(
      "D3D12GetInterface: unsupported class ", dxmt::str::format(clsid),
      ", riid ", dxmt::str::format(riid)));
  return E_NOINTERFACE;
}

extern "C" HRESULT __stdcall
D3D12EnableExperimentalFeatures(UINT feature_count, const IID *iids,
                                void *configurations,
                                UINT *configuration_sizes) {
  if (feature_count && !iids)
    return WARN_E_INVALIDARG(__func__);

  dxmt::Logger::warn(dxmt::str::format(
      "D3D12EnableExperimentalFeatures: experimental features are not "
      "supported, requested count ", feature_count));
  return DXGI_ERROR_UNSUPPORTED;
}
