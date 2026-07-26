#pragma once

#include "d3d12_device.hpp"
#include "dxmt_buffer.hpp"
#include "Metal.hpp"
#include "rc/util_rc_ptr.hpp"
#include <d3d12.h>
#include <memory>

namespace dxmt {
class LifetimeResidencyRegistration;
}

namespace dxmt::d3d12 {

class Heap {
public:
  virtual ~Heap() = default;

  virtual IMTLD3D12Device *GetParentDevice() const = 0;
  virtual const D3D12_HEAP_DESC &GetHeapDesc() const = 0;
  virtual D3D12_HEAP_TYPE GetHeapType() const = 0;
  virtual bool IsCpuVisible() const = 0;
  virtual dxmt::Buffer *GetBuffer() const = 0;
  virtual dxmt::BufferAllocation *GetAllocation() const = 0;
  virtual WMT::Reference<WMT::Heap> GetPlacementHeap() = 0;
  virtual std::shared_ptr<dxmt::LifetimeResidencyRegistration>
  GetPlacementHeapResidency() = 0;
};

Com<ID3D12Heap> CreateHeap(IMTLD3D12Device *device,
                           const D3D12_HEAP_DESC *desc);

Com<ID3D12Heap> CreateExternalCpuHeap(IMTLD3D12Device *device,
                                      const D3D12_HEAP_DESC *desc,
                                      const void *address);

D3D12_HEAP_TYPE GetHeapType(const D3D12_HEAP_PROPERTIES &properties);
bool IsCpuVisibleHeap(const D3D12_HEAP_PROPERTIES &properties);
Flags<dxmt::BufferAllocationFlag>
GetHeapBufferAllocationFlags(const D3D12_HEAP_PROPERTIES &properties);

} // namespace dxmt::d3d12
