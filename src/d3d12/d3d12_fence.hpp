#pragma once

#include "d3d12_device.hpp"
#include "Metal.hpp"
#include <d3d12.h>

namespace dxmt::d3d12 {

class Fence {
public:
  virtual ~Fence() = default;

  virtual HRESULT SignalFromQueue(UINT64 value) = 0;
  virtual bool HasReached(UINT64 value) const = 0;
};

Com<ID3D12Fence>
CreateFence(IMTLD3D12Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags);

} // namespace dxmt::d3d12
