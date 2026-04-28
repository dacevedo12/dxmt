#include "d3d12_private.h"

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "dxgi_interfaces.h"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <d3d12.h>
#include <dxgi1_6.h>

namespace dxmt {
Logger Logger::s_instance("d3d12.log");
}

namespace dxmt::d3d12 {

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
    return "disabled by DXMT_EXPERIENT_DX12_SUPPORT";
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

} // namespace dxmt::d3d12

extern "C" HRESULT __stdcall
D3D12CreateDevice(IUnknown *adapter, D3D_FEATURE_LEVEL minimum_feature_level, REFIID riid, void **device) {
  dxmt::InitReturnPtr(device);

  dxmt::Com<IDXGIAdapter> dxgi_adapter = nullptr;
  dxmt::Com<IDXGIFactory1> dxgi_factory = nullptr;

  if (adapter) {
    if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter)))) {
      dxmt::Logger::err("D3D12CreateDevice: adapter is not a DXGI adapter");
      return E_INVALIDARG;
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
    return E_INVALIDARG;
  }

  const auto gate = dxmt::d3d12::CheckSupportGate(metal_adapter->GetMTLDevice());
  if (gate.status != dxmt::d3d12::SupportGateStatus::Enabled) {
    dxmt::Logger::err(dxmt::d3d12::FormatSupportGateFailure(gate));
    return DXGI_ERROR_UNSUPPORTED;
  }

  dxmt::Logger::info(dxmt::str::format(
      "D3D12CreateDevice: experimental D3D12 support gate passed, minimum feature level ",
      minimum_feature_level, ", riid ", dxmt::str::format(riid)));

  return device ? E_NOTIMPL : S_FALSE;
}
