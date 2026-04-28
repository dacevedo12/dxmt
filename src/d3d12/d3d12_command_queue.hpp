#pragma once

#include "d3d12_device.hpp"
#include <d3d12.h>

namespace dxmt::d3d12 {

class CommandQueue;

HRESULT
CreateCommandQueue(IMTLD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc,
                   REFIID riid, void **command_queue);

} // namespace dxmt::d3d12
