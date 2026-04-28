#pragma once

#include "dxgi_interfaces.h"
#include "com/com_guid.hpp"
#include <d3d12.h>

// Internal DXGI bridge implemented by the D3D12 device so existing DXGI
// swapchain/presenter code can recognize DXMT-backed D3D12 devices.
DEFINE_COM_INTERFACE("7f7f9293-9c67-4c3f-865b-30c92e1a7d12", IMTLD3D12Device)
    : public IMTLDXGIDevice {
};
