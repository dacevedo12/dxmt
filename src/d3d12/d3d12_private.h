#pragma once

#include "Metal.hpp"
#include <string>

// Private declarations for the standalone D3D12 frontend.
// This layer must not depend on D3D11 device/context/state objects.

namespace dxmt::d3d12 {

constexpr const char *kExperimentD3D12SupportEnv = "DXMT_EXPERIENT_DX12_SUPPORT";

enum class SupportGateStatus {
  Enabled,
  DisabledByEnv,
  DisabledByAppleGpuFamily,
  DisabledByMetal4,
  DisabledByMacOS,
};

struct SupportGateResult {
  SupportGateStatus status;
  std::string env_value;
  bool supports_apple_gpu_family_7;
  bool supports_metal_4;
  WMT::OperatingSystemVersion os_version;
};

SupportGateResult CheckSupportGate(WMT::Device device);

} // namespace dxmt::d3d12
