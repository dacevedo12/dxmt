#pragma once

#include "Metal.hpp"
#include "dxgi_interfaces.h"

namespace dxmt::d3d12 {

HRESULT EnsureD3D12DxgiBackendRegistered();
WMT::Device GetD3D12AdapterDevice(IMTLDXGIAdapter *adapter);
WMT::Device GetD3D12DeviceMetalDevice(IMTLDXGIDevice *device);

} // namespace dxmt::d3d12
