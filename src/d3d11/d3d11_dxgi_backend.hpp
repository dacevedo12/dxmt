#pragma once

#include "Metal.hpp"
#include "dxgi_interfaces.h"

namespace dxmt {

HRESULT RegisterD3D11DxgiBackend();
WMT::Device GetD3D11AdapterDevice(IMTLDXGIAdapter *adapter);
WMT::Device GetD3D11DeviceMetalDevice(IMTLDXGIDevice *device);

} // namespace dxmt
