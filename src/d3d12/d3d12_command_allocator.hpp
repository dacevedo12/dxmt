#pragma once

#include "d3d12_device.hpp"
#include <d3d12.h>

namespace dxmt::d3d12 {

class CommandAllocator {
public:
  virtual ~CommandAllocator() = default;

  virtual D3D12_COMMAND_LIST_TYPE GetCommandListType() const = 0;
};

Com<ID3D12CommandAllocator>
CreateCommandAllocator(IMTLD3D12Device *device, D3D12_COMMAND_LIST_TYPE type);

} // namespace dxmt::d3d12
