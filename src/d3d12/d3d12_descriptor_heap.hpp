#pragma once

#include "d3d12_resource.hpp"
#include "d3d12_device.hpp"
#include "com/com_pointer.hpp"
#include <d3d12.h>
#include <cstddef>
#include <vector>

namespace dxmt::d3d12 {

class DescriptorHeap;

enum class DescriptorRecordType {
  Empty,
  ConstantBufferView,
  ShaderResourceView,
  UnorderedAccessView,
  RenderTargetView,
  DepthStencilView,
  Sampler,
};

struct DescriptorRecord {
  static constexpr uint32_t kMagic = 0x44584d54;

  uint32_t magic = kMagic;
  D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
  bool shader_visible = false;
  DescriptorRecordType type = DescriptorRecordType::Empty;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = {};
  Com<ID3D12Resource> resource;
  Com<ID3D12Resource> counter_resource;
  union {
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav;
    D3D12_RENDER_TARGET_VIEW_DESC rtv;
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv;
    D3D12_SAMPLER_DESC sampler;
  } desc = {};
  bool has_desc = false;
};

class DescriptorHeap {
public:
  virtual const D3D12_DESCRIPTOR_HEAP_DESC &GetDescriptorHeapDesc() const = 0;
  virtual DescriptorRecord *GetDescriptorRecord(D3D12_CPU_DESCRIPTOR_HANDLE handle) = 0;
  virtual const DescriptorRecord *GetDescriptorRecord(D3D12_CPU_DESCRIPTOR_HANDLE handle) const = 0;
  virtual const DescriptorRecord *GetDescriptorRecord(D3D12_GPU_DESCRIPTOR_HANDLE handle) const = 0;
};

Com<ID3D12DescriptorHeap>
CreateDescriptorHeap(IMTLD3D12Device *device,
                     const D3D12_DESCRIPTOR_HEAP_DESC *desc);

DescriptorRecord *GetDescriptorRecordFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle);
DescriptorRecord *GetDescriptorRecordFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                                   D3D12_DESCRIPTOR_HEAP_TYPE expected_type);
const DescriptorRecord *GetDescriptorRecordFromGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle,
                                                         D3D12_DESCRIPTOR_HEAP_TYPE expected_type);

} // namespace dxmt::d3d12
