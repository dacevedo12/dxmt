#pragma once

#include "d3d12_device.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_texture.hpp"
#include "com/com_pointer.hpp"
#include "rc/util_rc_ptr.hpp"
#include <d3d12.h>

namespace dxmt::d3d12 {

class Resource {
public:
  virtual ~Resource() = default;

  virtual const D3D12_RESOURCE_DESC &GetResourceDesc() const = 0;
  virtual const D3D12_HEAP_PROPERTIES &GetResourceHeapProperties() const = 0;
  virtual D3D12_HEAP_FLAGS GetResourceHeapFlags() const = 0;
  virtual UINT64 GetHeapOffset() const = 0;
  virtual D3D12_RESOURCE_STATES GetInitialState() const = 0;
  virtual D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const = 0;
  virtual dxmt::Buffer *GetBuffer() const = 0;
  virtual dxmt::BufferAllocation *GetBufferAllocation() const = 0;
  virtual dxmt::Texture *GetTexture() const = 0;
  virtual dxmt::TextureAllocation *GetTextureAllocation() const = 0;
  virtual ID3D12Resource *GetD3D12Resource() = 0;
};

Com<ID3D12Resource>
CreateResource(IMTLD3D12Device *device, const D3D12_HEAP_PROPERTIES *heap_properties,
               D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
               D3D12_RESOURCE_STATES initial_state, UINT64 heap_offset,
               const D3D12_CLEAR_VALUE *optimized_clear_value);

bool IsSupportedResourceDesc(const D3D12_RESOURCE_DESC &desc);

Resource *LookupBufferResourceByGpuVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS address,
                                                  UINT64 *offset = nullptr);

} // namespace dxmt::d3d12
