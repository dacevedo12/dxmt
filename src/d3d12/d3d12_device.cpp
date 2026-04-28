#include "d3d12_device.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstring>

namespace dxmt::d3d12 {

namespace {

constexpr D3D_FEATURE_LEVEL kSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;

static UINT64
Align(UINT64 value, UINT64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static bool
IsCpuVisibleHeap(D3D12_HEAP_TYPE heap_type) {
  return heap_type == D3D12_HEAP_TYPE_UPLOAD ||
         heap_type == D3D12_HEAP_TYPE_READBACK;
}

class DeviceImpl final : public ComObjectWithInitialRef<IMTLD3D12Device, ID3D12Device> {
public:
  DeviceImpl(std::unique_ptr<dxmt::Device> &&device, IMTLDXGIAdapter *adapter)
      : adapter_(adapter), device_(std::move(device)) {
    DXGI_ADAPTER_DESC adapter_desc = {};
    if (SUCCEEDED(adapter_->GetDesc(&adapter_desc)))
      adapter_luid_ = adapter_desc.AdapterLuid;

    if (adapter_->GetLocalD3DKMT()) {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = adapter_->GetLocalD3DKMT();
      if (D3DKMTCreateDevice(&create))
        WARN("D3D12Device: failed to create D3DKMT device");
      else
        local_kmt_ = create.hDevice;
    }
  }

  ~DeviceImpl() {
    if (local_kmt_) {
      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = local_kmt_;
      D3DKMTDestroyDevice(&destroy);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12Device)) {
      *ppvObject = ref(static_cast<ID3D12Device *>(this));
      return S_OK;
    }

    if (riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) ||
        riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IMTLDXGIDevice) ||
        riid == __uuidof(IMTLD3D12Device)) {
      *ppvObject = ref(static_cast<IMTLD3D12Device *>(this));
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Device), riid))
      WARN("D3D12Device: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return adapter_->QueryInterface(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    if (!pAdapter)
      return DXGI_ERROR_INVALID_CALL;

    return adapter_->QueryInterface(IID_PPV_ARGS(pAdapter));
  }

  HRESULT STDMETHODCALLTYPE
  CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count, DXGI_USAGE usage,
                const DXGI_SHARED_RESOURCE *shared_resource, IDXGISurface **surface) override {
    InitReturnPtr(surface);
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  QueryResourceResidency(IUnknown *const *resources, DXGI_RESIDENCY *residency, UINT resource_count) override {
    if (!resources || !residency)
      return E_INVALIDARG;

    for (UINT i = 0; i < resource_count; i++)
      residency[i] = DXGI_RESIDENCY_FULLY_RESIDENT;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override {
    if (priority < -7 || priority > 7)
      return E_INVALIDARG;

    gpu_thread_priority_ = priority;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *priority) override {
    if (!priority)
      return E_INVALIDARG;

    *priority = gpu_thread_priority_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    maximum_frame_latency_ = max_latency;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    if (!max_latency)
      return E_INVALIDARG;

    *max_latency = maximum_frame_latency_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  OfferResources(UINT resource_count, IDXGIResource *const *resources,
                 DXGI_OFFER_RESOURCE_PRIORITY priority) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT resource_count, IDXGIResource *const *resources,
                                             WINBOOL *discarded) override {
    if (discarded) {
      for (UINT i = 0; i < resource_count; i++)
        discarded[i] = FALSE;
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
    return E_FAIL;
  }

  void STDMETHODCALLTYPE Trim() override {}

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return adapter_->GetMTLDevice();
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return local_kmt_;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 *factory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
      IDXGISwapChain1 **swap_chain) override {
    InitReturnPtr(swap_chain);
    return DXGI_ERROR_UNSUPPORTED;
  }

  UINT STDMETHODCALLTYPE GetNodeCount() override {
    return 1;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *desc,
                                               REFIID riid, void **command_queue) override {
    InitReturnPtr(command_queue);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                   REFIID riid, void **command_allocator) override {
    InitReturnPtr(command_allocator);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                              REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                             REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                              ID3D12CommandAllocator *command_allocator,
                                              ID3D12PipelineState *initial_pipeline_state,
                                              REFIID riid, void **command_list) override {
    InitReturnPtr(command_list);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void *feature_data,
                                                UINT feature_data_size) override {
    if (!feature_data)
      return E_INVALIDARG;

    switch (feature) {
    case D3D12_FEATURE_FEATURE_LEVELS: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(feature_data);
      data->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL(0);

      if (!data->pFeatureLevelsRequested || !data->NumFeatureLevels) {
        data->MaxSupportedFeatureLevel = kSupportedFeatureLevel;
        return S_OK;
      }

      for (UINT i = 0; i < data->NumFeatureLevels; i++) {
        const auto requested = data->pFeatureLevelsRequested[i];
        if (requested <= kSupportedFeatureLevel)
          data->MaxSupportedFeatureLevel = std::max(data->MaxSupportedFeatureLevel, requested);
      }

      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(feature_data);
      if (data->NodeIndex != 0)
        return E_INVALIDARG;

      const bool unified_memory = GetMTLDevice().hasUnifiedMemory();
      data->TileBasedRenderer = TRUE;
      data->UMA = unified_memory;
      data->CacheCoherentUMA = unified_memory;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
      data->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_1;
      data->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
      data->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      data->MaxGPUVirtualAddressBitsPerResource = 40;
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(feature_data);
      data->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
      return S_OK;
    }
    default:
      return E_NOTIMPL;
    }
  }

  HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                                                 REFIID riid, void **descriptor_heap) override {
    InitReturnPtr(descriptor_heap);
    return E_NOTIMPL;
  }

  UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    return 1;
  }

  HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT node_mask, const void *bytecode,
                                                SIZE_T bytecode_length, REFIID riid,
                                                void **root_signature) override {
    InitReturnPtr(root_signature);
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {}

  void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *resource,
                                                  const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {}

  void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *resource,
                                                   ID3D12Resource *counter_resource,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {}

  void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *resource,
                                                const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {}

  void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *resource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {}

  void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {}

  void STDMETHODCALLTYPE CopyDescriptors(UINT dst_descriptor_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
                                         const UINT *dst_descriptor_range_sizes,
                                         UINT src_descriptor_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
                                         const UINT *src_descriptor_range_sizes,
                                         D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {}

  void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT descriptor_count,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
                                               D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {}

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
                            UINT resource_desc_count, const D3D12_RESOURCE_DESC *resource_descs) override {
    *__ret = GetResourceAllocationInfoImpl(visible_mask, resource_desc_count, resource_descs);
    return __ret;
  }
#else
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
  GetResourceAllocationInfo(UINT visible_mask, UINT resource_desc_count,
                            const D3D12_RESOURCE_DESC *resource_descs) override {
    return GetResourceAllocationInfoImpl(visible_mask, resource_desc_count, resource_descs);
  }
#endif

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE
  GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *__ret, UINT node_mask,
                          D3D12_HEAP_TYPE heap_type) override {
    *__ret = GetCustomHeapPropertiesImpl(node_mask, heap_type);
    return __ret;
  }
#else
  D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE
  GetCustomHeapProperties(UINT node_mask, D3D12_HEAP_TYPE heap_type) override {
    return GetCustomHeapPropertiesImpl(node_mask, heap_type);
  }
#endif

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource(const D3D12_HEAP_PROPERTIES *heap_properties,
                          D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
                          D3D12_RESOURCE_STATES initial_state,
                          const D3D12_CLEAR_VALUE *optimized_clear_value,
                          REFIID riid, void **resource) override {
    InitReturnPtr(resource);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid,
                                       void **heap) override {
    InitReturnPtr(heap);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *heap, UINT64 heap_offset,
                                                 const D3D12_RESOURCE_DESC *desc,
                                                 D3D12_RESOURCE_STATES initial_state,
                                                 const D3D12_CLEAR_VALUE *optimized_clear_value,
                                                 REFIID riid, void **resource) override {
    InitReturnPtr(resource);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES initial_state,
                                                   const D3D12_CLEAR_VALUE *optimized_clear_value,
                                                   REFIID riid, void **resource) override {
    InitReturnPtr(resource);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *object,
                                               const SECURITY_ATTRIBUTES *attributes,
                                               DWORD access, const WCHAR *name,
                                               HANDLE *handle) override {
    if (handle)
      *handle = nullptr;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE handle, REFIID riid, void **object) override {
    InitReturnPtr(object);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(const WCHAR *name, DWORD access,
                                                   HANDLE *handle) override {
    if (handle)
      *handle = nullptr;
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE MakeResident(UINT object_count, ID3D12Pageable *const *objects) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Evict(UINT object_count, ID3D12Pageable *const *objects) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS flags,
                                        REFIID riid, void **fence) override {
    InitReturnPtr(fence);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
    return S_OK;
  }

  void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc,
                                               UINT first_sub_resource,
                                               UINT sub_resource_count, UINT64 base_offset,
                                               D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
                                               UINT *row_count, UINT64 *row_size,
                                               UINT64 *total_bytes) override {
    if (row_count) {
      for (UINT i = 0; i < sub_resource_count; i++)
        row_count[i] = 0;
    }
    if (row_size) {
      for (UINT i = 0; i < sub_resource_count; i++)
        row_size[i] = 0;
    }
    if (total_bytes)
      *total_bytes = 0;
  }

  HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc,
                                            REFIID riid, void **heap) override {
    InitReturnPtr(heap);
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE SetStablePowerState(WINBOOL enable) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                                   ID3D12RootSignature *root_signature,
                                                   REFIID riid, void **command_signature) override {
    InitReturnPtr(command_signature);
    return E_NOTIMPL;
  }

  void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *resource, UINT *total_tile_count,
                                           D3D12_PACKED_MIP_INFO *packed_mip_info,
                                           D3D12_TILE_SHAPE *standard_tile_shape,
                                           UINT *sub_resource_tiling_count,
                                           UINT first_sub_resource_tiling,
                                           D3D12_SUBRESOURCE_TILING *sub_resource_tilings) override {
    if (total_tile_count)
      *total_tile_count = 0;
    if (sub_resource_tiling_count)
      *sub_resource_tiling_count = 0;
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  LUID *STDMETHODCALLTYPE GetAdapterLuid(LUID *__ret) override {
    *__ret = adapter_luid_;
    return __ret;
  }
#else
  LUID STDMETHODCALLTYPE GetAdapterLuid() override {
    return adapter_luid_;
  }
#endif

private:
  D3D12_RESOURCE_ALLOCATION_INFO
  GetResourceAllocationInfoImpl(UINT visible_mask, UINT resource_desc_count,
                                const D3D12_RESOURCE_DESC *resource_descs) const {
    D3D12_RESOURCE_ALLOCATION_INFO info = {};
    info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    if (!resource_descs)
      return info;

    for (UINT i = 0; i < resource_desc_count; i++) {
      const auto &desc = resource_descs[i];
      const UINT64 size = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                              ? desc.Width
                              : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      info.SizeInBytes += Align(size, info.Alignment);
    }

    return info;
  }

  D3D12_HEAP_PROPERTIES
  GetCustomHeapPropertiesImpl(UINT node_mask, D3D12_HEAP_TYPE heap_type) const {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = heap_type;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    properties.CPUPageProperty = IsCpuVisibleHeap(heap_type) ? D3D12_CPU_PAGE_PROPERTY_WRITE_BACK
                                                             : D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    properties.MemoryPoolPreference = IsCpuVisibleHeap(heap_type) ? D3D12_MEMORY_POOL_L0
                                                                  : D3D12_MEMORY_POOL_L1;
    return properties;
  }

  Com<IMTLDXGIAdapter> adapter_;
  std::unique_ptr<dxmt::Device> device_;
  ComPrivateData private_data_;
  D3DKMT_HANDLE local_kmt_ = 0;
  LUID adapter_luid_ = {};
  UINT maximum_frame_latency_ = 3;
  INT gpu_thread_priority_ = 0;
  std::string name_;
};

} // namespace

Com<IMTLD3D12Device>
CreateD3D12Device(std::unique_ptr<dxmt::Device> &&device, IMTLDXGIAdapter *adapter) {
  return Com<IMTLD3D12Device>::transfer(new DeviceImpl(std::move(device), adapter));
}

} // namespace dxmt::d3d12
