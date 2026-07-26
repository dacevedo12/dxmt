#pragma once

#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_present_source_view.hpp"
#include "dxmt_texture.hpp"
#include "rc/util_rc_ptr.hpp"
#include <d3d12.h>
#include <memory>
#include <vector>

namespace dxmt::d3d12 {

enum class ResourceKind {
  Committed,
  Placed,
  ReservedBuffer,
  ReservedTexture,
};

struct SubresourceTiling {
  UINT width_in_tiles = 0;
  UINT16 height_in_tiles = 0;
  UINT16 depth_in_tiles = 0;
  UINT start_tile_index = 0;
  UINT packed_tile_index = 0;
  UINT mip_level = 0;
  UINT array_slice = 0;
  UINT plane = 0;
};

struct ResourceTiling {
  UINT total_tile_count = 0;
  D3D12_TILE_SHAPE tile_shape = {};
  D3D12_PACKED_MIP_INFO packed_mip_info = {};
  std::vector<SubresourceTiling> subresources;
};

struct ResourceTileMapping {
  Com<ID3D12Heap> heap;
  int64_t heap_tile = -1;
};

class Resource;

class CpuQueryResolveTarget {
public:
  CpuQueryResolveTarget() = default;
  CpuQueryResolveTarget(const CpuQueryResolveTarget &) = delete;
  CpuQueryResolveTarget &operator=(const CpuQueryResolveTarget &) = delete;
  virtual ~CpuQueryResolveTarget() noexcept = default;
  virtual void Resolve(Resource &resource) noexcept = 0;
};

class Resource {
public:
  virtual ~Resource() = default;

  virtual ResourceKind GetKind() const = 0;
  virtual bool IsReserved() const = 0;
  virtual bool IsReservedTexture() const = 0;
  virtual bool UsesPlacementSparse() const = 0;
  virtual const ResourceTiling *GetTiling() const = 0;
  virtual bool UpdateTileMapping(UINT subresource, UINT x, UINT y, UINT z,
                                 ID3D12Heap *heap, bool mapped,
                                 UINT64 heap_tile) = 0;
  virtual bool UpdateTileMappingByIndex(UINT tile_index, ID3D12Heap *heap,
                                        bool mapped, UINT64 heap_tile) = 0;
  virtual bool GetTileMapping(UINT subresource, UINT x, UINT y, UINT z,
                              ResourceTileMapping &mapping) const = 0;
  virtual bool GetTileMappingByIndex(UINT tile_index,
                                     ResourceTileMapping &mapping) const = 0;
  virtual const D3D12_RESOURCE_DESC &GetResourceDesc() const = 0;
  virtual IMTLD3D12Device *GetParentDevice() const = 0;
  virtual const D3D12_HEAP_PROPERTIES &GetResourceHeapProperties() const = 0;
  virtual D3D12_HEAP_FLAGS GetResourceHeapFlags() const = 0;
  virtual uint64_t GetDescriptorIdentity() const = 0;
  virtual bool HasLifetimeResidency() const = 0;
  virtual uint64_t GetTileMappingGeneration() const = 0;
  virtual UINT64 GetHeapOffset() const = 0;
  virtual D3D12_RESOURCE_STATES GetInitialState() const = 0;
  virtual D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const = 0;
  virtual dxmt::Buffer *GetBuffer() const = 0;
  virtual dxmt::BufferAllocation *GetBufferAllocation() const = 0;
  virtual dxmt::Texture *GetTexture() const = 0;
  virtual dxmt::Texture *GetTexture(UINT plane) const = 0;
  virtual dxmt::TextureAllocation *GetTextureAllocation() const = 0;
  virtual dxmt::TextureAllocation *GetTextureAllocation(UINT plane) const = 0;
  virtual bool EnsureTextureAllocation(const char *reason) = 0;
  virtual void AddPendingTimestampResolve(UINT64 offset, UINT64 size,
                                          uint64_t seq) = 0;
  virtual bool CanDeferCpuQueryResolve() const = 0;
  [[nodiscard]] virtual bool AddPendingCpuQueryResolve(
      UINT64 offset, UINT64 size, uint64_t seq,
      std::unique_ptr<CpuQueryResolveTarget> target) = 0;
  virtual bool HasPendingCpuQueryResolves(UINT64 offset, UINT64 size) = 0;
  virtual bool MaterializePendingCpuQueryResolves(UINT64 offset, UINT64 size,
                                                  const char *context) = 0;
  virtual void SetPresentSourceView(dxmt::TextureViewKey view) = 0;
  virtual dxmt::TextureViewKey GetPresentSourceView() const = 0;
  virtual ID3D12Resource *GetD3D12Resource() = 0;
};

// Private IID for a non-RTTI downcast ID3D12Resource* -> dxmt Resource*.
// ResourceImpl::QueryInterface answers it by returning static_cast<Resource*>
// WITHOUT AddRef — an internal fast-path that replaces the per-record
// dynamic_cast on the replay hot path (1.33e8 casts/diag-run). It preserves
// dynamic_cast semantics: a non-dxmt ID3D12Resource returns E_NOINTERFACE, so
// GetResource() still yields nullptr. Not for external/COM callers.
inline constexpr GUID IID_DXMTResourceDowncast = {
    0x6b7e9c14, 0x3a8d, 0x4f21,
    {0xb6, 0x05, 0x9e, 0x33, 0x77, 0x12, 0xad, 0x4c}};

Com<ID3D12Resource>
CreateResource(IMTLD3D12Device *device, const D3D12_HEAP_PROPERTIES *heap_properties,
               D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
               D3D12_RESOURCE_STATES initial_state, UINT64 heap_offset,
               const D3D12_CLEAR_VALUE *optimized_clear_value,
               ResourceKind kind = ResourceKind::Committed,
               dxmt::Buffer *placed_buffer = nullptr,
               dxmt::BufferAllocation *placed_buffer_allocation = nullptr,
               WMT::Heap placement_heap = {},
               std::shared_ptr<dxmt::LifetimeResidencyRegistration>
                   placement_heap_residency = {});

bool IsSupportedResourceDesc(const D3D12_RESOURCE_DESC &desc);

bool GetTextureHeapSizeAndAlign(WMT::Device device,
                                const D3D12_RESOURCE_DESC &desc,
                                WMTSizeAndAlign &size_and_align);

Resource *LookupBufferResourceByGpuVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS address,
                                                  UINT64 *offset = nullptr);

void ActivateBufferGpuVirtualAddress(Resource *resource);

} // namespace dxmt::d3d12
