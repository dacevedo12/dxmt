#include "d3d12_command_queue.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

namespace dxmt::d3d12 {
namespace {

static bool
IsSupportedQueueType(D3D12_COMMAND_LIST_TYPE type) {
  return type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
         type == D3D12_COMMAND_LIST_TYPE_COPY;
}

static bool
IsSupportedQueuePriority(INT priority) {
  return priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL ||
         priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
}

static bool
IsSupportedQueueFlags(D3D12_COMMAND_QUEUE_FLAGS flags) {
  return (flags & ~D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT) == 0;
}

static Resource *
GetResource(ID3D12Resource *resource) {
  return dynamic_cast<Resource *>(resource);
}

static PipelineState *
GetPipelineState(ID3D12PipelineState *pipeline_state) {
  return dynamic_cast<PipelineState *>(pipeline_state);
}

static WMTPrimitiveType
GetPrimitiveType(D3D12_PRIMITIVE_TOPOLOGY topology) {
  switch (topology) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    return WMTPrimitiveTypePoint;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    return WMTPrimitiveTypeLine;
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    return WMTPrimitiveTypeLineStrip;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    return WMTPrimitiveTypeTriangleStrip;
  default:
    return WMTPrimitiveTypeTriangle;
  }
}

static WMTIndexType
GetIndexType(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32
                                        : WMTIndexTypeUInt16;
}

static UINT
GetIndexSize(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R32_UINT ? 4 : 2;
}

static UINT
GetMipLevel(const Resource &resource, UINT subresource) {
  const auto &desc = resource.GetResourceDesc();
  const UINT mip_levels = desc.MipLevels ? desc.MipLevels : 1;
  return mip_levels ? subresource % mip_levels : 0;
}

static UINT
GetArraySlice(const Resource &resource, UINT subresource) {
  const auto &desc = resource.GetResourceDesc();
  const UINT mip_levels = desc.MipLevels ? desc.MipLevels : 1;
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
             ? 0
             : subresource / mip_levels;
}

static WMTSize
GetSubresourceSize(const Resource &resource, UINT subresource,
                   const D3D12_BOX *box) {
  if (box) {
    return {box->right - box->left, box->bottom - box->top,
            box->back - box->front};
  }

  const auto &desc = resource.GetResourceDesc();
  const auto mip = GetMipLevel(resource, subresource);
  return {std::max<UINT64>(1, desc.Width >> mip),
          std::max<UINT64>(1, desc.Height >> mip),
          desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
              ? std::max<UINT64>(1, desc.DepthOrArraySize >> mip)
              : 1};
}

static TextureViewKey
CreateRenderTargetView(Resource &resource, const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = 1;
  view.firstArraySlice = 0;
  view.arraySize = 1;
  view.intendedUsage = WMTTextureUsageRenderTarget;

  if (descriptor.has_desc) {
    const auto &rtv = descriptor.desc.rtv;
    switch (rtv.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE2D:
      view.firstMiplevel = rtv.Texture2D.MipSlice;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = rtv.Texture2DArray.MipSlice;
      view.firstArraySlice = rtv.Texture2DArray.FirstArraySlice;
      view.arraySize = rtv.Texture2DArray.ArraySize;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
      view.type = WMTTextureType2DMultisampleArray;
      view.firstArraySlice = rtv.Texture2DMSArray.FirstArraySlice;
      view.arraySize = rtv.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
    }
  }

  return texture->createView(view);
}

static TextureViewKey
CreateDepthStencilView(Resource &resource, const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = 1;
  view.firstArraySlice = 0;
  view.arraySize = 1;
  view.intendedUsage = WMTTextureUsageRenderTarget;

  if (descriptor.has_desc) {
    const auto &dsv = descriptor.desc.dsv;
    switch (dsv.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE2D:
      view.firstMiplevel = dsv.Texture2D.MipSlice;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = dsv.Texture2DArray.MipSlice;
      view.firstArraySlice = dsv.Texture2DArray.FirstArraySlice;
      view.arraySize = dsv.Texture2DArray.ArraySize;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
      view.type = WMTTextureType2DMultisampleArray;
      view.firstArraySlice = dsv.Texture2DMSArray.FirstArraySlice;
      view.arraySize = dsv.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
    }
  }

  return texture->createView(view);
}

static UINT
GetRenderTargetArrayLength(const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc)
    return 1;

  switch (descriptor.desc.rtv.ViewDimension) {
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    return descriptor.desc.rtv.Texture2DArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    return descriptor.desc.rtv.Texture2DMSArray.ArraySize;
  default:
    return 1;
  }
}

static UINT
GetDepthStencilArrayLength(const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc)
    return 1;

  switch (descriptor.desc.dsv.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return descriptor.desc.dsv.Texture2DArray.ArraySize;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    return descriptor.desc.dsv.Texture2DMSArray.ArraySize;
  default:
    return 1;
  }
}

static HRESULT
NormalizeQueueDesc(const D3D12_COMMAND_QUEUE_DESC *desc,
                   D3D12_COMMAND_QUEUE_DESC &normalized) {
  normalized = {};
  normalized.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  normalized.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  normalized.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  normalized.NodeMask = 0;

  if (desc)
    normalized = *desc;

  if (!IsSupportedQueueType(normalized.Type)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported queue type ", normalized.Type));
    return E_INVALIDARG;
  }

  if (!IsSupportedQueuePriority(normalized.Priority)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported priority ", normalized.Priority));
    return E_INVALIDARG;
  }

  if (!IsSupportedQueueFlags(normalized.Flags)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported flags ", normalized.Flags));
    return E_INVALIDARG;
  }

  if (normalized.NodeMask > 1) {
    Logger::err(str::format("D3D12CommandQueue: unsupported node mask ", normalized.NodeMask));
    return E_INVALIDARG;
  }

  return S_OK;
}

class CommandQueueImpl final : public ComObjectWithInitialRef<ID3D12CommandQueue, IMTLDXGIDevice> {
public:
  CommandQueueImpl(IMTLD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC &desc)
      : device_(device), desc_(desc) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = ref(static_cast<ID3D12CommandQueue *>(this));
      return S_OK;
    }

    if (riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) ||
        riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IMTLDXGIDevice)) {
      *ppvObject = ref(static_cast<IMTLDXGIDevice *>(this));
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid))
      WARN("D3D12CommandQueue: unknown interface query ", str::format(riid));

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

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

#ifdef __MINGW32__
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *resource, UINT region_count,
                                            const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
                                            const D3D12_TILE_REGION_SIZE *region_sizes,
                                            ID3D12Heap *heap,
                                            UINT range_count,
                                            const D3D12_TILE_RANGE_FLAGS *range_flags,
                                            const UINT *heap_range_offsets,
                                            const UINT *range_tile_counts,
                                            D3D12_TILE_MAPPING_FLAGS flags) override {}
#else
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *resource, UINT region_count,
                                            const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
                                            const D3D12_TILE_REGION_SIZE *region_sizes,
                                            UINT range_count,
                                            const D3D12_TILE_RANGE_FLAGS *range_flags,
                                            UINT *heap_range_offsets,
                                            UINT *range_tile_counts,
                                            D3D12_TILE_MAPPING_FLAGS flags) override {}
#endif

  void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *dst_resource,
                                          const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
                                          ID3D12Resource *src_resource,
                                          const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
                                          const D3D12_TILE_REGION_SIZE *region_size,
                                          D3D12_TILE_MAPPING_FLAGS flags) override {}

  void STDMETHODCALLTYPE ExecuteCommandLists(UINT command_list_count,
                                             ID3D12CommandList *const *command_lists) override {
    if (!command_list_count)
      return;

    if (!command_lists) {
      Logger::err("D3D12CommandQueue: ExecuteCommandLists called with null command list array");
      return;
    }

    bool submitted_any = false;
    for (UINT i = 0; i < command_list_count; i++) {
      auto *command_list = command_lists[i];
      if (!command_list) {
        Logger::err(str::format("D3D12CommandQueue: null command list at index ", i));
        continue;
      }

      auto *state = dynamic_cast<GraphicsCommandList *>(command_list);
      if (!state) {
        Logger::err(str::format("D3D12CommandQueue: foreign command list at index ", i));
        continue;
      }

      if (state->GetCommandListType() != desc_.Type) {
        Logger::err(str::format("D3D12CommandQueue: command list type ", state->GetCommandListType(),
                                " does not match queue type ", desc_.Type));
        continue;
      }

      if (!state->IsClosed()) {
        Logger::err(str::format("D3D12CommandQueue: command list at index ", i, " is not closed"));
        continue;
      }

      if (SUCCEEDED(state->MarkSubmittedToQueue(desc_.Type))) {
        ReplayCommandRecords(state->GetCommandRecords());
        submitted_any = true;
      }
    }

    if (submitted_any) {
      device_->GetDXMTDevice().queue().CommitCurrentChunk();
      submitted_batches_++;
    }
  }

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) override {}

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) override {}

  void STDMETHODCALLTYPE EndEvent() override {}

  HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *fence, UINT64 value) override {
    if (!fence)
      return E_INVALIDARG;

    auto *state = dynamic_cast<Fence *>(fence);
    if (!state)
      return E_INVALIDARG;

    auto event = state->GetSharedEvent();
    {
      std::lock_guard lock(mutex_);
      auto &queue = device_->GetDXMTDevice().queue();
      queue.CurrentChunk()->emitcc([event = std::move(event), value](ArgumentEncodingContext &enc) mutable {
        enc.signalEvent(std::move(event), value);
      });
      queue.CommitCurrentChunk();
      signal_count_++;
      last_signal_value_ = value;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *fence, UINT64 value) override {
    if (!fence)
      return E_INVALIDARG;

    auto *state = dynamic_cast<Fence *>(fence);
    if (!state)
      return E_INVALIDARG;

    auto event = state->GetSharedEvent();
    {
      std::lock_guard lock(mutex_);
      auto &queue = device_->GetDXMTDevice().queue();
      queue.CurrentChunk()->emitcc([event = std::move(event), value](ArgumentEncodingContext &enc) mutable {
        enc.waitEvent(std::move(event), value);
      });
      queue.CommitCurrentChunk();
      wait_values_.push_back(value);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *frequency) override {
    if (!frequency)
      return E_INVALIDARG;

    *frequency = 1'000'000'000ull;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) override {
    if (!gpu_timestamp || !cpu_timestamp)
      return E_INVALIDARG;

    *gpu_timestamp = 0;
    *cpu_timestamp = 0;
    return S_OK;
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }
#else
  D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override {
    return desc_;
  }
#endif

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return device_->GetParent(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    return device_->GetAdapter(pAdapter);
  }

  HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count,
                                          DXGI_USAGE usage,
                                          const DXGI_SHARED_RESOURCE *shared_resource,
                                          IDXGISurface **surface) override {
    InitReturnPtr(surface);
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown *const *resources,
                                                   DXGI_RESIDENCY *residency,
                                                   UINT resource_count) override {
    return device_->QueryResourceResidency(resources, residency, resource_count);
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override {
    return device_->SetGPUThreadPriority(priority);
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *priority) override {
    return device_->GetGPUThreadPriority(priority);
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    return device_->SetMaximumFrameLatency(max_latency);
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    return device_->GetMaximumFrameLatency(max_latency);
  }

  HRESULT STDMETHODCALLTYPE OfferResources(UINT resource_count, IDXGIResource *const *resources,
                                           DXGI_OFFER_RESOURCE_PRIORITY priority) override {
    return device_->OfferResources(resource_count, resources, priority);
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT resource_count, IDXGIResource *const *resources,
                                             WINBOOL *discarded) override {
    return device_->ReclaimResources(resource_count, resources, discarded);
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
    return device_->EnqueueSetEvent(event);
  }

  void STDMETHODCALLTYPE Trim() override {
    device_->Trim();
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return device_->GetMTLDevice();
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return device_->GetLocalD3DKMT();
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory1 *factory, HWND hWnd,
                                            const DXGI_SWAP_CHAIN_DESC1 *desc,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
                                            IDXGISwapChain1 **swap_chain) override {
    InitReturnPtr(swap_chain);
    Logger::err("D3D12CommandQueue: DXGI swapchain bridge is not implemented yet");
    return DXGI_ERROR_UNSUPPORTED;
  }

private:
  struct ReplayState {
    Com<ID3D12PipelineState> pipeline_state;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::vector<D3D12_VIEWPORT> viewports;
    std::vector<D3D12_RECT> scissors;
    std::vector<DescriptorRecord> render_targets;
    std::optional<DescriptorRecord> depth_stencil;
    std::array<std::optional<D3D12_VERTEX_BUFFER_VIEW>, 32> vertex_buffers = {};
    std::optional<D3D12_INDEX_BUFFER_VIEW> index_buffer;
  };

  struct ReplayRenderTargetAttachment {
    Rc<Texture> texture;
    TextureViewKey view = {};
    UINT slot = 0;
    UINT array_length = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    WMTPixelFormat format = WMTPixelFormatInvalid;
  };

  struct ReplayDepthStencilAttachment {
    Rc<Texture> texture;
    TextureViewKey view = {};
    UINT array_length = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    WMTPixelFormat format = WMTPixelFormatInvalid;
  };

  struct ReplayRenderPassAttachments {
    std::vector<ReplayRenderTargetAttachment> colors;
    std::optional<ReplayDepthStencilAttachment> depth_stencil;
  };

  void ReplayCommandRecords(const std::vector<CommandRecord> &records) {
    auto *chunk = device_->GetDXMTDevice().queue().CurrentChunk();
    ReplayState state = {};
    for (const auto &record : records) {
      std::visit([&](const auto &payload) { ReplayRecord(chunk, state, payload); },
                 record.payload);
    }
  }

  template <typename T>
  void ReplayRecord(CommandChunk *chunk, ReplayState &state, const T &record) {
    if constexpr (std::is_same_v<T, CopyBufferRegionRecord>) {
      ReplayCopyBufferRegion(chunk, record);
    } else if constexpr (std::is_same_v<T, CopyTextureRegionRecord>) {
      ReplayCopyTextureRegion(chunk, record);
    } else if constexpr (std::is_same_v<T, CopyResourceRecord>) {
      ReplayCopyResource(chunk, record);
    } else if constexpr (std::is_same_v<T, ClearRenderTargetRecord>) {
      ReplayClearRenderTarget(chunk, record);
    } else if constexpr (std::is_same_v<T, ClearDepthStencilRecord>) {
      ReplayClearDepthStencil(chunk, record);
    } else if constexpr (std::is_same_v<T, PipelineStateRecord>) {
      state.pipeline_state = record.pipeline_state;
    } else if constexpr (std::is_same_v<T, PrimitiveTopologyRecord>) {
      state.topology = record.topology;
    } else if constexpr (std::is_same_v<T, ViewportRecord>) {
      state.viewports = record.viewports;
    } else if constexpr (std::is_same_v<T, ScissorRecord>) {
      state.scissors = record.rects;
    } else if constexpr (std::is_same_v<T, RenderTargetsRecord>) {
      state.render_targets = record.render_targets;
      state.depth_stencil = record.depth_stencil;
    } else if constexpr (std::is_same_v<T, VertexBuffersRecord>) {
      for (UINT i = 0; i < record.views.size() &&
                       record.start_slot + i < state.vertex_buffers.size();
           i++)
        state.vertex_buffers[record.start_slot + i] = record.views[i];
    } else if constexpr (std::is_same_v<T, IndexBufferRecord>) {
      state.index_buffer = record.view;
    } else if constexpr (std::is_same_v<T, ResourceBarrierRecord>) {
      ReplayResourceBarrier(chunk, record);
    } else if constexpr (std::is_same_v<T, DrawInstancedRecord>) {
      ReplayDrawInstanced(chunk, state, record);
    } else if constexpr (std::is_same_v<T, DrawIndexedInstancedRecord>) {
      ReplayDrawIndexedInstanced(chunk, state, record);
    } else if constexpr (std::is_same_v<T, DispatchRecord>) {
      ReplayDispatch(chunk, state, record);
    }
  }

  void ReplayCopyBufferRegion(CommandChunk *chunk,
                              const CopyBufferRegionRecord &record) {
    auto *dst = GetResource(record.dst.ptr());
    auto *src = GetResource(record.src.ptr());
    if (!dst || !src || !dst->GetBufferAllocation() ||
        !src->GetBufferAllocation())
      return;

    Rc<BufferAllocation> dst_allocation = dst->GetBufferAllocation();
    Rc<BufferAllocation> src_allocation = src->GetBufferAllocation();
    const UINT64 src_offset = src->GetHeapOffset() + record.src_offset;
    const UINT64 dst_offset = dst->GetHeapOffset() + record.dst_offset;
    chunk->emitcc([dst_allocation, src_allocation, src_offset, dst_offset,
                   byte_count = record.byte_count](ArgumentEncodingContext &enc) {
      enc.retainAllocation(dst_allocation.ptr());
      enc.retainAllocation(src_allocation.ptr());
      enc.startBlitPass();
      auto &copy =
          enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = src_allocation->buffer();
      copy.src_offset = src_offset;
      copy.dst = dst_allocation->buffer();
      copy.dst_offset = dst_offset;
      copy.copy_length = byte_count;
      enc.endPass();
    });
  }

  void ReplayResourceBarrier(CommandChunk *chunk,
                             const ResourceBarrierRecord &record) {
    if (record.barriers.empty())
      return;

    chunk->emitcc([](ArgumentEncodingContext &enc) {
      enc.endPass();
    });
  }

  ReplayRenderPassAttachments BuildRenderPassAttachments(
      const ReplayState &state) {
    ReplayRenderPassAttachments attachments = {};
    attachments.colors.reserve(state.render_targets.size());

    for (UINT i = 0; i < state.render_targets.size(); i++) {
      const auto &descriptor = state.render_targets[i];
      auto *resource = GetResource(descriptor.resource.ptr());
      if (!resource || !resource->GetTexture())
        continue;

      auto view = CreateRenderTargetView(*resource, descriptor);
      auto *texture = resource->GetTexture();
      attachments.colors.push_back({
          .texture = texture,
          .view = view,
          .slot = i,
          .array_length = GetRenderTargetArrayLength(descriptor),
          .width = texture->width(),
          .height = texture->height(),
          .format = texture->pixelFormat(),
      });
    }

    if (state.depth_stencil) {
      auto *resource = GetResource(state.depth_stencil->resource.ptr());
      if (resource && resource->GetTexture()) {
        auto view = CreateDepthStencilView(*resource, *state.depth_stencil);
        auto *texture = resource->GetTexture();
        attachments.depth_stencil = ReplayDepthStencilAttachment{
            .texture = texture,
            .view = view,
            .array_length = GetDepthStencilArrayLength(*state.depth_stencil),
            .width = texture->width(),
            .height = texture->height(),
            .format = texture->pixelFormat(),
        };
      }
    }

    return attachments;
  }

  static bool BeginRenderPass(ArgumentEncodingContext &enc,
                              ReplayRenderPassAttachments &attachments) {
    if (attachments.colors.empty() && !attachments.depth_stencil)
      return false;

    UINT render_target_count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t array_length = 1;
    for (const auto &color : attachments.colors) {
      render_target_count = std::max(render_target_count, color.slot + 1);
      width = width ? width : color.width;
      height = height ? height : color.height;
      array_length = std::max<uint32_t>(array_length, color.array_length);
    }
    if (attachments.depth_stencil) {
      width = width ? width : attachments.depth_stencil->width;
      height = height ? height : attachments.depth_stencil->height;
      array_length =
          std::max<uint32_t>(array_length, attachments.depth_stencil->array_length);
    }

    const auto dsv_format = attachments.depth_stencil
                                ? attachments.depth_stencil->format
                                : WMTPixelFormatInvalid;
    auto &info = *enc.startRenderPass(DepthStencilPlanarFlags(dsv_format), 0,
                                      render_target_count, 0);
    for (auto &rtv : attachments.colors) {
      auto &color = info.colors[rtv.slot];
      color.attachment = enc.access<PipelineStage::Pixel>(
          rtv.texture, rtv.view, ResourceAccess::ReadWrite);
      color.load_action = WMTLoadActionLoad;
      color.store_action = WMTStoreActionStore;
      color.depth_plane = 0;
      info.tile_barrier_pso_key.color_formats[rtv.slot] = rtv.format;
    }

    if (attachments.depth_stencil) {
      const auto planar_flags =
          DepthStencilPlanarFlags(attachments.depth_stencil->format);
      if (planar_flags & 1) {
        auto &depth = info.depth;
        depth.attachment = enc.access<PipelineStage::Pixel>(
            attachments.depth_stencil->texture, attachments.depth_stencil->view,
            ResourceAccess::ReadWrite);
        depth.load_action = WMTLoadActionLoad;
        depth.store_action = WMTStoreActionStore;
      }
      if (planar_flags & 2) {
        auto &stencil = info.stencil;
        stencil.attachment = enc.access<PipelineStage::Pixel>(
            attachments.depth_stencil->texture, attachments.depth_stencil->view,
            ResourceAccess::ReadWrite);
        stencil.load_action = WMTLoadActionLoad;
        stencil.store_action = WMTStoreActionStore;
      }
    }

    info.render_target_width = width;
    info.render_target_height = height;
    info.render_target_array_length = array_length;
    info.tile_barrier_pso_key.raster_sample_count = 1;
    return true;
  }

  void ReplayDrawInstanced(CommandChunk *chunk, ReplayState &state,
                           const DrawInstancedRecord &record) {
    if (!record.vertex_count_per_instance || !record.instance_count)
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: draw skipped without graphics pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalGraphicsState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    const auto primitive = GetPrimitiveType(state.topology);
    auto viewports = state.viewports;
    auto scissors = state.scissors;
    auto attachments = BuildRenderPassAttachments(state);
    chunk->emitcc([metal_pso = metal->pso, primitive,
                   vertex_start = record.start_vertex_location,
                   vertex_count = record.vertex_count_per_instance,
                   instance_count = record.instance_count,
                   base_instance = record.start_instance_location,
                   viewports = std::move(viewports),
                   scissors = std::move(scissors),
                   attachments = std::move(attachments)](ArgumentEncodingContext &enc) mutable {
      if (!BeginRenderPass(enc, attachments))
        return;

      auto &set_pso = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      set_pso.type = WMTRenderCommandSetPSO;
      set_pso.pso = metal_pso;

      if (!viewports.empty()) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setviewports>();
        cmd.type = WMTRenderCommandSetViewports;
        auto *data = static_cast<WMTViewport *>(
            enc.allocate_cpu_heap(sizeof(WMTViewport) * viewports.size(),
                                  alignof(WMTViewport)));
        for (size_t i = 0; i < viewports.size(); i++) {
          const auto &viewport = viewports[i];
          data[i] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                     viewport.Height, viewport.MinDepth, viewport.MaxDepth};
        }
        cmd.viewports.set(data);
        cmd.viewport_count = viewports.size();
      }

      if (!scissors.empty()) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setscissorrects>();
        cmd.type = WMTRenderCommandSetScissorRects;
        auto *data = static_cast<WMTScissorRect *>(
            enc.allocate_cpu_heap(sizeof(WMTScissorRect) * scissors.size(),
                                  alignof(WMTScissorRect)));
        for (size_t i = 0; i < scissors.size(); i++) {
          const auto &rect = scissors[i];
          data[i] = {uint32_t(std::max<LONG>(0, rect.left)),
                     uint32_t(std::max<LONG>(0, rect.top)),
                     uint32_t(std::max<LONG>(0, rect.right - rect.left)),
                     uint32_t(std::max<LONG>(0, rect.bottom - rect.top))};
        }
        cmd.scissor_rects.set(data);
        cmd.rect_count = scissors.size();
      }

      auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw>();
      draw.type = WMTRenderCommandDraw;
      draw.primitive_type = primitive;
      draw.vertex_start = vertex_start;
      draw.vertex_count = vertex_count;
      draw.instance_count = instance_count;
      draw.base_instance = base_instance;
      enc.endPass();
    });
  }

  void ReplayDrawIndexedInstanced(CommandChunk *chunk, ReplayState &state,
                                  const DrawIndexedInstancedRecord &record) {
    if (!record.index_count_per_instance || !record.instance_count ||
        !state.index_buffer)
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: indexed draw skipped without graphics pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalGraphicsState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: indexed draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    UINT64 index_resource_offset = 0;
    auto *index_resource = LookupBufferResourceByGpuVirtualAddress(
        state.index_buffer->BufferLocation, &index_resource_offset);
    if (!index_resource || !index_resource->GetBufferAllocation()) {
      WARN("D3D12CommandQueue: indexed draw skipped because index buffer binding is unavailable");
      return;
    }

    Rc<BufferAllocation> index_allocation = index_resource->GetBufferAllocation();
    const auto primitive = GetPrimitiveType(state.topology);
    const auto index_type = GetIndexType(state.index_buffer->Format);
    const UINT64 index_offset = index_resource->GetHeapOffset() +
                                index_resource_offset +
                                record.start_index_location *
                                    GetIndexSize(state.index_buffer->Format);
    auto attachments = BuildRenderPassAttachments(state);
    chunk->emitcc([metal_pso = metal->pso, index_allocation, primitive,
                   index_type, index_offset,
                   index_count = record.index_count_per_instance,
                   instance_count = record.instance_count,
                   base_vertex = record.base_vertex_location,
                   base_instance = record.start_instance_location,
                   attachments = std::move(attachments)](ArgumentEncodingContext &enc) mutable {
      enc.retainAllocation(index_allocation.ptr());
      if (!BeginRenderPass(enc, attachments))
        return;
      auto &set_pso = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      set_pso.type = WMTRenderCommandSetPSO;
      set_pso.pso = metal_pso;

      auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw_indexed>();
      draw.type = WMTRenderCommandDrawIndexed;
      draw.primitive_type = primitive;
      draw.index_type = index_type;
      draw.index_count = index_count;
      draw.index_buffer = index_allocation->buffer();
      draw.index_buffer_offset = index_offset;
      draw.instance_count = instance_count;
      draw.base_vertex = base_vertex;
      draw.base_instance = base_instance;
      enc.endPass();
    });
  }

  void ReplayDispatch(CommandChunk *chunk, ReplayState &state,
                      const DispatchRecord &record) {
    if (!record.x || !record.y || !record.z)
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: dispatch skipped without compute pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalComputeState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: dispatch skipped because Metal compute PSO is unavailable");
      return;
    }

    chunk->emitcc([metal_pso = metal->pso,
                   threadgroup_size = metal->threadgroup_size,
                   x = record.x, y = record.y, z = record.z](ArgumentEncodingContext &enc) {
      enc.startComputePass(0);
      auto &set_pso = enc.encodeComputeCommand<wmtcmd_compute_setpso>();
      set_pso.type = WMTComputeCommandSetPSO;
      set_pso.pso = metal_pso;
      set_pso.threadgroup_size = threadgroup_size;

      auto &dispatch = enc.encodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = {x, y, z};
      enc.endPass();
    });
  }

  void ReplayCopyResource(CommandChunk *chunk, const CopyResourceRecord &record) {
    auto *dst = GetResource(record.dst.ptr());
    auto *src = GetResource(record.src.ptr());
    if (!dst || !src)
      return;

    if (dst->GetBufferAllocation() && src->GetBufferAllocation()) {
      const UINT64 size = std::min(dst->GetResourceDesc().Width,
                                   src->GetResourceDesc().Width);
      CopyBufferRegionRecord copy = {};
      copy.dst = record.dst;
      copy.src = record.src;
      copy.byte_count = size;
      ReplayCopyBufferRegion(chunk, copy);
      return;
    }

    if (dst->GetTextureAllocation() && src->GetTextureAllocation()) {
      CopyTextureRegionRecord copy = {};
      copy.dst.resource = record.dst;
      copy.dst.type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      copy.src.resource = record.src;
      copy.src.type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      const UINT dst_subresources =
          (dst->GetResourceDesc().MipLevels ? dst->GetResourceDesc().MipLevels : 1) *
          (dst->GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
               ? 1
               : dst->GetResourceDesc().DepthOrArraySize);
      const UINT src_subresources =
          (src->GetResourceDesc().MipLevels ? src->GetResourceDesc().MipLevels : 1) *
          (src->GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
               ? 1
               : src->GetResourceDesc().DepthOrArraySize);
      const UINT count = std::min(dst_subresources, src_subresources);
      for (UINT i = 0; i < count; i++) {
        copy.dst.subresource_index = i;
        copy.src.subresource_index = i;
        ReplayCopyTextureRegion(chunk, copy);
      }
    }
  }

  void ReplayCopyTextureRegion(CommandChunk *chunk,
                               const CopyTextureRegionRecord &record) {
    auto *dst = GetResource(record.dst.resource.ptr());
    auto *src = GetResource(record.src.resource.ptr());
    if (!dst || !src)
      return;

    if (dst->GetTextureAllocation() && src->GetTextureAllocation() &&
        dst->GetTexture() && src->GetTexture()) {
      const UINT dst_subresource =
          record.dst.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
              ? record.dst.subresource_index
              : 0;
      const UINT src_subresource =
          record.src.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
              ? record.src.subresource_index
              : 0;
      const auto size =
          GetSubresourceSize(*src, src_subresource,
                             record.src_box ? &*record.src_box : nullptr);
      const auto src_origin = record.src_box
                                  ? WMTOrigin{record.src_box->left,
                                              record.src_box->top,
                                              record.src_box->front}
                                  : WMTOrigin{0, 0, 0};
      const auto dst_origin = WMTOrigin{record.dst_x, record.dst_y, record.dst_z};
      Rc<TextureAllocation> dst_allocation = dst->GetTextureAllocation();
      Rc<TextureAllocation> src_allocation = src->GetTextureAllocation();
      const UINT dst_slice = GetArraySlice(*dst, dst_subresource);
      const UINT dst_level = GetMipLevel(*dst, dst_subresource);
      const UINT src_slice = GetArraySlice(*src, src_subresource);
      const UINT src_level = GetMipLevel(*src, src_subresource);
      chunk->emitcc([dst_allocation, src_allocation, dst_slice, dst_level,
                     src_slice, src_level, src_origin, dst_origin,
                     size](ArgumentEncodingContext &enc) {
        enc.retainAllocation(dst_allocation.ptr());
        enc.retainAllocation(src_allocation.ptr());
        enc.startBlitPass();
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
        copy.type = WMTBlitCommandCopyFromTextureToTexture;
        copy.src = src_allocation->texture();
        copy.src_slice = src_slice;
        copy.src_level = src_level;
        copy.src_origin = src_origin;
        copy.src_size = size;
        copy.dst = dst_allocation->texture();
        copy.dst_slice = dst_slice;
        copy.dst_level = dst_level;
        copy.dst_origin = dst_origin;
        enc.endPass();
      });
      return;
    }

    ReplayBufferTextureCopy(chunk, record, *dst, *src);
  }

  void ReplayBufferTextureCopy(CommandChunk *chunk,
                               const CopyTextureRegionRecord &record,
                               Resource &dst, Resource &src) {
    const bool dst_is_buffer = dst.GetBufferAllocation() != nullptr;
    const bool src_is_buffer = src.GetBufferAllocation() != nullptr;
    if (dst_is_buffer == src_is_buffer)
      return;

    auto &buffer_resource = dst_is_buffer ? dst : src;
    auto &texture_resource = dst_is_buffer ? src : dst;
    Rc<BufferAllocation> buffer_allocation = buffer_resource.GetBufferAllocation();
    Rc<TextureAllocation> texture_allocation = texture_resource.GetTextureAllocation();
    if (!buffer_allocation || !texture_allocation)
      return;

    const auto &buffer_location = dst_is_buffer ? record.dst : record.src;
    const auto &texture_location = dst_is_buffer ? record.src : record.dst;
    if (buffer_location.type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
      return;
    const UINT subresource =
        texture_location.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
            ? texture_location.subresource_index
            : 0;
    const UINT slice = GetArraySlice(texture_resource, subresource);
    const UINT level = GetMipLevel(texture_resource, subresource);
    const auto size =
        GetSubresourceSize(texture_resource, subresource,
                           record.src_box ? &*record.src_box : nullptr);
    const auto origin = record.src_box
                            ? WMTOrigin{record.src_box->left,
                                        record.src_box->top,
                                        record.src_box->front}
                            : WMTOrigin{dst_is_buffer ? 0u : record.dst_x,
                                        dst_is_buffer ? 0u : record.dst_y,
                                        dst_is_buffer ? 0u : record.dst_z};
    const auto footprint = buffer_location.placed_footprint.Footprint;
    const UINT64 buffer_offset =
        buffer_resource.GetHeapOffset() + buffer_location.placed_footprint.Offset;
    const UINT row_pitch = footprint.RowPitch;
    const UINT image_pitch = footprint.RowPitch * footprint.Height;

    chunk->emitcc([dst_is_buffer, buffer_allocation, texture_allocation,
                   buffer_offset, row_pitch, image_pitch, size, origin, slice,
                   level](ArgumentEncodingContext &enc) {
      enc.retainAllocation(buffer_allocation.ptr());
      enc.retainAllocation(texture_allocation.ptr());
      enc.startBlitPass();
      if (dst_is_buffer) {
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
        copy.type = WMTBlitCommandCopyFromTextureToBuffer;
        copy.src = texture_allocation->texture();
        copy.slice = slice;
        copy.level = level;
        copy.origin = origin;
        copy.size = size;
        copy.dst = buffer_allocation->buffer();
        copy.offset = buffer_offset;
        copy.bytes_per_row = row_pitch;
        copy.bytes_per_image = image_pitch;
      } else {
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        copy.type = WMTBlitCommandCopyFromBufferToTexture;
        copy.src = buffer_allocation->buffer();
        copy.src_offset = buffer_offset;
        copy.bytes_per_row = row_pitch;
        copy.bytes_per_image = image_pitch;
        copy.size = size;
        copy.dst = texture_allocation->texture();
        copy.slice = slice;
        copy.level = level;
        copy.origin = origin;
      }
      enc.endPass();
    });
  }

  void ReplayClearRenderTarget(CommandChunk *chunk,
                               const ClearRenderTargetRecord &record) {
    auto *resource = GetResource(record.descriptor.resource.ptr());
    if (!resource || !resource->GetTexture() || !resource->GetTextureAllocation())
      return;

    Rc<Texture> texture = resource->GetTexture();
    auto view = CreateRenderTargetView(*resource, record.descriptor);
    const UINT array_length = GetRenderTargetArrayLength(record.descriptor);
    WMTClearColor color = {record.color[0], record.color[1], record.color[2],
                           record.color[3]};
    chunk->emitcc([texture = std::move(texture), view, array_length,
                   color](ArgumentEncodingContext &enc) mutable {
      enc.clearColor(std::move(texture), view, array_length, color);
    });
  }

  void ReplayClearDepthStencil(CommandChunk *chunk,
                               const ClearDepthStencilRecord &record) {
    auto *resource = GetResource(record.descriptor.resource.ptr());
    if (!resource || !resource->GetTexture() || !resource->GetTextureAllocation())
      return;

    Rc<Texture> texture = resource->GetTexture();
    auto view = CreateDepthStencilView(*resource, record.descriptor);
    const UINT array_length = GetDepthStencilArrayLength(record.descriptor);
    unsigned flags = 0;
    if (record.flags & D3D12_CLEAR_FLAG_DEPTH)
      flags |= 1;
    if (record.flags & D3D12_CLEAR_FLAG_STENCIL)
      flags |= 2;
    chunk->emitcc([texture = std::move(texture), view, array_length, flags,
                   depth = record.depth,
                   stencil = record.stencil](ArgumentEncodingContext &enc) mutable {
      enc.clearDepthStencil(std::move(texture), view, array_length, flags,
                            depth, stencil);
    });
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_COMMAND_QUEUE_DESC desc_ = {};
  UINT64 submitted_batches_ = 0;
  UINT64 signal_count_ = 0;
  UINT64 last_signal_value_ = 0;
  std::vector<UINT64> wait_values_;
  std::mutex mutex_;
  std::string name_;
};

} // namespace

HRESULT
CreateCommandQueue(IMTLD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc,
                   REFIID riid, void **command_queue) {
  InitReturnPtr(command_queue);
  if (!command_queue)
    return E_INVALIDARG;

  D3D12_COMMAND_QUEUE_DESC normalized = {};
  HRESULT hr = NormalizeQueueDesc(desc, normalized);
  if (FAILED(hr))
    return hr;

  auto queue = Com<ID3D12CommandQueue>::transfer(new CommandQueueImpl(device, normalized));
  return queue->QueryInterface(riid, command_queue);
}

} // namespace dxmt::d3d12
