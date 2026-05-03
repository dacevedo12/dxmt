#include "d3d12_device.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_command_allocator.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_command_queue.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_query.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace dxmt::d3d12 {

namespace {

constexpr D3D_FEATURE_LEVEL kSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;

static UINT64
Align(UINT64 value, UINT64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static UINT
SubresourceMipSlice(UINT sub_resource, UINT mip_levels) {
  return mip_levels ? sub_resource % mip_levels : 0;
}

static UINT64
MipSize(UINT64 value, UINT mip_slice) {
  return std::max<UINT64>(1, value >> mip_slice);
}

static bool
IsCpuVisibleHeap(D3D12_HEAP_TYPE heap_type) {
  return heap_type == D3D12_HEAP_TYPE_UPLOAD ||
         heap_type == D3D12_HEAP_TYPE_READBACK;
}

static UINT
IndirectArgumentByteSize(const D3D12_INDIRECT_ARGUMENT_DESC &argument) {
  switch (argument.Type) {
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
    return sizeof(D3D12_DRAW_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
    return sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
    return sizeof(D3D12_DISPATCH_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
    return sizeof(D3D12_VERTEX_BUFFER_VIEW);
  case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
    return sizeof(D3D12_INDEX_BUFFER_VIEW);
  case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
    return sizeof(UINT) * argument.Constant.Num32BitValuesToSet;
  case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
  case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
  case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
    return sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
  default:
    return 0;
  }
}

static bool
IsIndirectOperationArgument(D3D12_INDIRECT_ARGUMENT_TYPE type) {
  return type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ||
         type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ||
         type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
}

static bool
HasFormatCapability(FormatCapability caps, FormatCapability cap) {
  return (static_cast<int>(caps) & static_cast<int>(cap)) != 0;
}

static DXGI_FORMAT
ResolveDepthTypelessFormatForD3D12Caps(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R16_TYPELESS:
    return DXGI_FORMAT_D16_UNORM;
  case DXGI_FORMAT_R32_TYPELESS:
    return DXGI_FORMAT_D32_FLOAT;
  default:
    return format;
  }
}

static FormatCapability
GetD3D12FormatCapability(WMT::Device device,
                         const MTL_DXGI_FORMAT_DESC &format) {
  FormatCapabilityInspector inspector;
  inspector.Inspect(device);
  auto entry = inspector.textureCapabilities.find(format.PixelFormat);
  return entry == inspector.textureCapabilities.end()
             ? FormatCapability::None
             : entry->second;
}

static D3D12_FORMAT_SUPPORT1
GetD3D12FormatSupport1(FormatCapability caps,
                       const MTL_DXGI_FORMAT_DESC &format) {
  D3D12_FORMAT_SUPPORT1 support = D3D12_FORMAT_SUPPORT1_NONE;
  if (HasFormatCapability(caps, FormatCapability::TextureBufferRead) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferWrite) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT1_BUFFER;

  support |= D3D12_FORMAT_SUPPORT1_TEXTURE1D |
             D3D12_FORMAT_SUPPORT1_TEXTURE2D |
             D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
             D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;

  if (!(format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER |
                       MTL_DXGI_FORMAT_STENCIL_PLANER))) {
    support |= D3D12_FORMAT_SUPPORT1_TEXTURE3D |
               D3D12_FORMAT_SUPPORT1_TEXTURECUBE;
  }
  if (HasFormatCapability(caps, FormatCapability::Filter))
    support |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
               D3D12_FORMAT_SUPPORT1_SHADER_GATHER;
  if (HasFormatCapability(caps, FormatCapability::Color))
    support |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
  if (HasFormatCapability(caps, FormatCapability::Blend))
    support |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
  if (HasFormatCapability(caps, FormatCapability::DepthStencil))
    support |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
  if (HasFormatCapability(caps, FormatCapability::MSAA))
    support |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
               D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
  if (HasFormatCapability(caps, FormatCapability::Resolve))
    support |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
  if (HasFormatCapability(caps, FormatCapability::Write))
    support |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
  if (format.Flag & MTL_DXGI_FORMAT_BACKBUFFER)
    support |= D3D12_FORMAT_SUPPORT1_DISPLAY |
               D3D12_FORMAT_SUPPORT1_BACK_BUFFER_CAST;

  return support;
}

static D3D12_FORMAT_SUPPORT2
GetD3D12FormatSupport2(FormatCapability caps) {
  D3D12_FORMAT_SUPPORT2 support = D3D12_FORMAT_SUPPORT2_NONE;
  if (HasFormatCapability(caps, FormatCapability::TextureBufferRead) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
  if (HasFormatCapability(caps, FormatCapability::Write) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferWrite) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
  if (HasFormatCapability(caps, FormatCapability::Atomic))
    support |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
  if (HasFormatCapability(caps, FormatCapability::Sparse))
    support |= D3D12_FORMAT_SUPPORT2_TILED;
  return support;
}

static UINT8
GetD3D12FormatPlaneCount(const MTL_DXGI_FORMAT_DESC &format) {
  const uint32_t planes = DepthStencilPlanarFlags(format.PixelFormat);
  return planes == 3 ? 2 : 1;
}

static bool
IsSupportedD3D12SampleCount(UINT sample_count) {
  return sample_count == 1 || sample_count == 2 || sample_count == 4 ||
         sample_count == 8;
}

static bool
IsSupportedCommandListType(D3D12_COMMAND_LIST_TYPE type) {
  return type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         type == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
         type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
         type == D3D12_COMMAND_LIST_TYPE_COPY;
}

static bool
IsSupportedCommandQueuePriority(UINT priority) {
  return priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL ||
         priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
}

static DescriptorRecord *
GetDescriptorRecordForWrite(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                            D3D12_DESCRIPTOR_HEAP_TYPE expected_type,
                            const char *context) {
  auto *record = d3d12::GetDescriptorRecordRangeFromCpuHandle(
      handle, expected_type, 1, context);
  if (!record)
    WARN("D3D12Device: invalid descriptor handle for ", context);
  return record;
}

static void
ResetDescriptorRecord(DescriptorRecord &record) {
  const auto magic = record.magic;
  const auto heap_type = record.heap_type;
  const auto shader_visible = record.shader_visible;
  const auto cpu_handle = record.cpu_handle;
  const auto heap_index = record.heap_index;
  const auto heap_count = record.heap_count;
  record = {};
  record.magic = magic;
  record.heap_type = heap_type;
  record.shader_visible = shader_visible;
  record.cpu_handle = cpu_handle;
  record.heap_index = heap_index;
  record.heap_count = heap_count;
}

static bool
IsBufferResource(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  return d3d12_resource &&
         d3d12_resource->GetResourceDesc().Dimension ==
             D3D12_RESOURCE_DIMENSION_BUFFER;
}

static bool
IsTextureResource(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  if (!d3d12_resource)
    return false;
  const auto dimension = d3d12_resource->GetResourceDesc().Dimension;
  return dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
         dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
         dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
}

static const D3D12_RESOURCE_DESC *
GetResourceDesc(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  return d3d12_resource ? &d3d12_resource->GetResourceDesc() : nullptr;
}

static UINT
GetTextureMipDepth(const D3D12_RESOURCE_DESC &resource_desc, UINT mip_slice) {
  if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 1;
  return static_cast<UINT>(
      std::max<UINT64>(1, resource_desc.DepthOrArraySize >> mip_slice));
}

static bool
IsSupportedSrvDimension(D3D12_SRV_DIMENSION dimension) {
  switch (dimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
  case D3D12_SRV_DIMENSION_TEXTURE1D:
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE2D:
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE2DMS:
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE3D:
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    return true;
  default:
    return false;
  }
}

static bool
IsSupportedUavDimension(D3D12_UAV_DIMENSION dimension) {
  switch (dimension) {
  case D3D12_UAV_DIMENSION_BUFFER:
  case D3D12_UAV_DIMENSION_TEXTURE1D:
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
  case D3D12_UAV_DIMENSION_TEXTURE2D:
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    return true;
  default:
    return false;
  }
}

static bool
ValidateShaderResourceView(ID3D12Resource *resource,
                           const D3D12_SHADER_RESOURCE_VIEW_DESC &desc) {
  if (!IsSupportedSrvDimension(desc.ViewDimension)) {
    WARN("D3D12Device: unsupported SRV dimension ",
         uint32_t(desc.ViewDimension));
    return false;
  }
  if (desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
    if (resource && !IsBufferResource(resource)) {
      WARN("D3D12Device: buffer SRV created for non-buffer resource");
      return false;
    }
    const auto raw = (desc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
    const auto structured = desc.Buffer.StructureByteStride != 0;
    const auto typed =
        desc.Format != DXGI_FORMAT_UNKNOWN &&
        !(raw && desc.Format == DXGI_FORMAT_R32_TYPELESS);
    if ((raw && structured) || (raw && typed) || (structured && typed)) {
      WARN("D3D12Device: ambiguous buffer SRV typed/raw/structured descriptor");
      return false;
    }
    if (raw && desc.Format != DXGI_FORMAT_R32_TYPELESS &&
        desc.Format != DXGI_FORMAT_UNKNOWN) {
      WARN("D3D12Device: raw buffer SRV should use R32_TYPELESS/UNKNOWN format");
      return false;
    }
    return true;
  }
  if (resource && !IsTextureResource(resource)) {
    WARN("D3D12Device: texture SRV created for non-texture resource");
    return false;
  }
  return true;
}

static bool
ValidateUnorderedAccessView(ID3D12Resource *resource,
                            ID3D12Resource *counter_resource,
                            const D3D12_UNORDERED_ACCESS_VIEW_DESC &desc) {
  if (!IsSupportedUavDimension(desc.ViewDimension)) {
    WARN("D3D12Device: unsupported UAV dimension ",
         uint32_t(desc.ViewDimension));
    return false;
  }
  if (desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
    if (resource && !IsBufferResource(resource)) {
      WARN("D3D12Device: buffer UAV created for non-buffer resource");
      return false;
    }
    const auto raw = (desc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
    const auto structured = desc.Buffer.StructureByteStride != 0;
    const auto typed =
        desc.Format != DXGI_FORMAT_UNKNOWN &&
        !(raw && desc.Format == DXGI_FORMAT_R32_TYPELESS);
    if ((raw && structured) || (raw && typed) || (structured && typed)) {
      WARN("D3D12Device: ambiguous buffer UAV typed/raw/structured descriptor");
      return false;
    }
    if (counter_resource && !structured) {
      WARN("D3D12Device: UAV counter resource is only valid for structured buffer UAVs");
      return false;
    }
    return true;
  }
  if (resource && !IsTextureResource(resource)) {
    WARN("D3D12Device: texture UAV created for non-texture resource");
    return false;
  }
  if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D) {
    const auto *resource_desc = GetResourceDesc(resource);
    if (resource_desc && resource_desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
      WARN("D3D12Device: 3D texture UAV created for non-3D texture resource");
      return false;
    }
    if (resource_desc && desc.Texture3D.MipSlice >= resource_desc->MipLevels) {
      WARN("D3D12Device: 3D texture UAV mip slice out of range ",
           desc.Texture3D.MipSlice);
      return false;
    }
    if (resource_desc) {
      const UINT mip_depth = GetTextureMipDepth(*resource_desc, desc.Texture3D.MipSlice);
      const UINT first_w = desc.Texture3D.FirstWSlice;
      const UINT w_size = desc.Texture3D.WSize == UINT_MAX
                              ? (first_w < mip_depth ? mip_depth - first_w : 0)
                              : desc.Texture3D.WSize;
      if (first_w >= mip_depth || w_size == 0 || w_size > mip_depth - first_w) {
        WARN("D3D12Device: invalid 3D texture UAV W slice range first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return false;
      }
      if (first_w != 0 || w_size != mip_depth) {
        // TODO(d3d12): support 3D texture UAV W-slice subranges when the
        // Metal texture view layer can preserve D3D12 depth-slice semantics.
        WARN("D3D12Device: unsupported 3D texture UAV W slice subrange first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return false;
      }
    }
  }
  if (counter_resource) {
    WARN("D3D12Device: UAV counter resource is ignored for texture UAVs");
    return false;
  }
  return true;
}

static void
CopyDescriptorRecord(DescriptorRecord &dst, const DescriptorRecord &src) {
  const auto magic = dst.magic;
  const auto heap_type = dst.heap_type;
  const auto shader_visible = dst.shader_visible;
  const auto cpu_handle = dst.cpu_handle;
  const auto heap_index = dst.heap_index;
  const auto heap_count = dst.heap_count;
  dst = src;
  dst.magic = magic;
  dst.heap_type = heap_type;
  dst.shader_visible = shader_visible;
  dst.cpu_handle = cpu_handle;
  dst.heap_index = heap_index;
  dst.heap_count = heap_count;
}

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
using DeviceComBase = ID3D12Device2;
#else
using DeviceComBase = ID3D12Device1;
#endif

class DeviceImpl final : public ComObjectWithInitialRef<IMTLD3D12Device, DeviceComBase> {
public:
  DeviceImpl(std::unique_ptr<dxmt::Device> &&device, IMTLDXGIAdapter *adapter)
      : adapter_(adapter), device_(std::move(device)) {
    enqueue_set_event_signal_ = device_->device().newSharedEvent();
    multiple_fence_wait_signal_ = device_->device().newSharedEvent();
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
      *ppvObject = ref(AsD3D12Device());
      return S_OK;
    }

    if (riid == __uuidof(ID3D12Device1)) {
      *ppvObject = ref(static_cast<ID3D12Device1 *>(
          static_cast<DeviceComBase *>(this)));
      return S_OK;
    }

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device2)) {
      *ppvObject = ref(static_cast<ID3D12Device2 *>(this));
      return S_OK;
    }
#endif

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
    if (!event)
      return E_INVALIDARG;
    auto &queue = device_->queue();
    auto signal = enqueue_set_event_signal_;
    auto value = ++enqueue_set_event_value_;
    MTLSharedEvent_setWin32EventAtValue(
        signal.handle, queue.GetSharedEventListener(), event, value);
    queue.CurrentChunk()->emitcc([signal = std::move(signal), value](
                                    ArgumentEncodingContext &enc) mutable {
      enc.signalEvent(std::move(signal), value);
    });
    queue.CommitCurrentChunk();
    return S_OK;
  }

  void STDMETHODCALLTYPE Trim() override {}

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return adapter_->GetMTLDevice();
  }

  dxmt::Device &GetDXMTDevice() override {
    return *device_;
  }

  uint64_t NextTimestampQueryValue() override {
    return ++timestamp_query_value_;
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
    return d3d12::CreateCommandQueue(static_cast<IMTLD3D12Device *>(this), desc, riid, command_queue);
  }

  HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                   REFIID riid, void **command_allocator) override {
    InitReturnPtr(command_allocator);
    if (!command_allocator)
      return E_POINTER;
    if (!IsSupportedCommandListType(type)) {
      WARN("D3D12Device: unsupported command allocator type ", type);
      return E_INVALIDARG;
    }

    auto allocator = d3d12::CreateCommandAllocator(static_cast<IMTLD3D12Device *>(this), type);
    return allocator->QueryInterface(riid, command_allocator);
  }

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                              REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    HRESULT status = S_OK;
    auto state = d3d12::CreateGraphicsPipelineState(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    return state->QueryInterface(riid, pipeline_state);
  }

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                             REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    HRESULT status = S_OK;
    auto state = d3d12::CreateComputePipelineState(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    return state->QueryInterface(riid, pipeline_state);
  }

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE
  CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                      REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;

    HRESULT status = S_OK;
    auto state = d3d12::CreatePipelineStateFromStream(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    return state->QueryInterface(riid, pipeline_state);
  }
#endif

  HRESULT STDMETHODCALLTYPE CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                              ID3D12CommandAllocator *command_allocator,
                                              ID3D12PipelineState *initial_pipeline_state,
                                              REFIID riid, void **command_list) override {
    InitReturnPtr(command_list);

    if (node_mask > 1 || !command_allocator)
      return E_INVALIDARG;
    if (!IsSupportedCommandListType(type)) {
      WARN("D3D12Device: unsupported command list type ", type);
      return E_INVALIDARG;
    }

    auto allocator_state = dynamic_cast<d3d12::CommandAllocator *>(command_allocator);
    if (!allocator_state || allocator_state->GetCommandListType() != type)
      return E_INVALIDARG;

    auto list = d3d12::CreateGraphicsCommandList(
        static_cast<IMTLD3D12Device *>(this), node_mask, type,
        command_allocator, initial_pipeline_state);
    return list->QueryInterface(riid, command_list);
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
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(feature_data);
      data->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
      data->Support2 = D3D12_FORMAT_SUPPORT2_NONE;

      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device_->device(), data->Format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(FORMAT_SUPPORT) unsupported format ",
             data->Format);
        return S_OK;
      }

      const auto caps = GetD3D12FormatCapability(device_->device(), format);
      data->Support1 = GetD3D12FormatSupport1(caps, format);
      data->Support2 = GetD3D12FormatSupport2(caps);
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(feature_data);
      data->NumQualityLevels = 0;
      if (data->Flags & ~D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE)
        return E_INVALIDARG;

      MTL_DXGI_FORMAT_DESC format = {};
      const auto query_format =
          ResolveDepthTypelessFormatForD3D12Caps(data->Format);
      if (FAILED(MTLQueryDXGIFormat(device_->device(), query_format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(MSAA) unsupported format ",
             data->Format);
        return S_OK;
      }

      const auto caps = GetD3D12FormatCapability(device_->device(), format);
      if (IsSupportedD3D12SampleCount(data->SampleCount) &&
          (data->SampleCount == 1 || HasFormatCapability(caps, FormatCapability::MSAA)))
        data->NumQualityLevels = 1;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_INFO: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_FORMAT_INFO))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_FORMAT_INFO *>(feature_data);
      data->PlaneCount = 0;

      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device_->device(), data->Format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(FORMAT_INFO) unsupported format ",
             data->Format);
        return S_OK;
      }

      data->PlaneCount = GetD3D12FormatPlaneCount(format);
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *>(feature_data);
      data->MaxGPUVirtualAddressBitsPerResource = 40;
      data->MaxGPUVirtualAddressBitsPerProcess = 40;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(feature_data);
      if (data->HighestShaderModel >= D3D_SHADER_MODEL_6_0)
        data->HighestShaderModel = D3D_SHADER_MODEL_6_0;
      else
        data->HighestShaderModel = D3D_SHADER_MODEL_5_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ExpandedComputeResourceStates = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(feature_data);
      data->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1 *>(feature_data);
      if (data->NodeIndex != 0)
        return E_INVALIDARG;

      const bool unified_memory = GetMTLDevice().hasUnifiedMemory();
      data->TileBasedRenderer = TRUE;
      data->UMA = unified_memory;
      data->CacheCoherentUMA = unified_memory;
      data->IsolatedMMU = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ProgrammableSamplePositionsTier =
          D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_SHADER_CACHE))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(feature_data);
      data->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY *>(feature_data);
      data->PriorityForTypeIsSupported =
          IsSupportedCommandListType(data->CommandListType) &&
          IsSupportedCommandQueuePriority(data->Priority);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->CopyQueueTimestampQueriesSupported = TRUE;
      data->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_EXISTING_HEAPS: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_EXISTING_HEAPS *>(feature_data);
      data->Supported = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->SharedResourceCompatibilityTier =
          D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_SERIALIZATION: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_SERIALIZATION))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_SERIALIZATION *>(feature_data);
      if (data->NodeIndex != 0)
        return E_INVALIDARG;
      data->HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_CROSS_NODE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_CROSS_NODE))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_CROSS_NODE *>(feature_data);
      data->SharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      data->AtomicShaderInstructions = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      data->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
    case D3D12_FEATURE_D3D12_OPTIONS6: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS6))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS6 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->VariableShadingRateTier =
          D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
      data->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS8: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS8))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS8 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS9: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS9))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS9 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->WaveMMATier = D3D12_WAVE_MMA_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS10: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS10))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS10 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS11: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS11))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS11 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS12: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->MSPrimitivesPipelineStatisticIncludesCulledPrimitives =
          D3D12_TRI_STATE_UNKNOWN;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS13: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS13))
        return E_INVALIDARG;

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS13 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
#endif
    default:
      WARN("D3D12Device: CheckFeatureSupport unsupported feature ", feature);
      return DXGI_ERROR_UNSUPPORTED;
    }
  }

  HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                                                 REFIID riid, void **descriptor_heap) override {
    InitReturnPtr(descriptor_heap);
    if (!descriptor_heap)
      return E_POINTER;
    if (!desc || desc->NumDescriptors == 0)
      return E_INVALIDARG;
    if (desc->NodeMask > 1)
      return E_INVALIDARG;

    switch (desc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      break;
    default:
      return E_INVALIDARG;
    }

    auto heap = d3d12::CreateDescriptorHeap(
        static_cast<IMTLD3D12Device *>(this), desc);
    return heap->QueryInterface(riid, descriptor_heap);
  }

  UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    switch (descriptor_heap_type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      return sizeof(DescriptorRecord);
    default:
      return 0;
    }
  }

  HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT node_mask, const void *bytecode,
                                                SIZE_T bytecode_length, REFIID riid,
                                                void **root_signature) override {
    InitReturnPtr(root_signature);
    if (!root_signature)
      return E_POINTER;
    if (node_mask > 1 || (!bytecode && bytecode_length))
      return E_INVALIDARG;

    auto object = d3d12::CreateRootSignatureFromBlob(
        static_cast<IMTLD3D12Device *>(this),
        std::span<const std::byte>(static_cast<const std::byte *>(bytecode),
                                   bytecode_length));
    if (!object)
      return E_INVALIDARG;

    return object->QueryInterface(riid, root_signature);
  }

  void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateConstantBufferView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::ConstantBufferView;
    if (desc) {
      if (desc->BufferLocation & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
        WARN("D3D12Device: CBV BufferLocation is not 256-byte aligned");
      if (desc->SizeInBytes & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
        WARN("D3D12Device: CBV SizeInBytes is not 256-byte aligned");
      UINT64 offset = 0;
      auto *resource =
          LookupBufferResourceByGpuVirtualAddress(desc->BufferLocation, &offset);
      if (!resource || !resource->GetBuffer()) {
        WARN("D3D12Device: CBV BufferLocation does not resolve to a buffer resource");
      } else if (desc->SizeInBytes >
                 resource->GetResourceDesc().Width - offset) {
        WARN("D3D12Device: CBV range exceeds buffer resource size");
      }
      record->desc.cbv = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *resource,
                                                  const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateShaderResourceView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::ShaderResourceView;
    record->resource = resource;
    if (desc) {
      if (!ValidateShaderResourceView(resource, *desc))
        return;
      record->desc.srv = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *resource,
                                                   ID3D12Resource *counter_resource,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateUnorderedAccessView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::UnorderedAccessView;
    record->resource = resource;
    record->counter_resource = counter_resource;
    if (desc) {
      if (!ValidateUnorderedAccessView(resource, counter_resource, *desc))
        return;
      record->desc.uav = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *resource,
                                                const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        "CreateRenderTargetView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::RenderTargetView;
    record->resource = resource;
    if (desc) {
      record->desc.rtv = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *resource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        "CreateDepthStencilView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::DepthStencilView;
    record->resource = resource;
    if (desc) {
      record->desc.dsv = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, "CreateSampler");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::Sampler;
    if (desc) {
      record->desc.sampler = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CopyDescriptors(UINT dst_descriptor_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
                                         const UINT *dst_descriptor_range_sizes,
                                         UINT src_descriptor_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
                                         const UINT *src_descriptor_range_sizes,
                                         D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    if (!dst_descriptor_range_count)
      return;
    if ((dst_descriptor_range_count && !dst_descriptor_range_offsets) ||
        (src_descriptor_range_count && !src_descriptor_range_offsets)) {
      WARN("D3D12Device: CopyDescriptors called with null range offsets");
      return;
    }

    std::vector<DescriptorRecord *> destinations;
    UINT64 dst_total = 0;
    for (UINT dst_range = 0; dst_range < dst_descriptor_range_count; dst_range++) {
      const UINT dst_count = dst_descriptor_range_sizes
                                 ? dst_descriptor_range_sizes[dst_range]
                                 : 1;
      if (!dst_count)
        continue;
      auto *dst = d3d12::GetDescriptorRecordRangeFromCpuHandle(
          dst_descriptor_range_offsets[dst_range], descriptor_heap_type,
          dst_count, "CopyDescriptors destination");
      if (!dst)
        return;
      if (dst_total > UINT_MAX - dst_count) {
        WARN("D3D12Device: CopyDescriptors destination descriptor count overflow");
        return;
      }
      dst_total += dst_count;
      destinations.reserve(static_cast<size_t>(dst_total));
      for (UINT i = 0; i < dst_count; i++)
        destinations.push_back(dst + i);
    }
    if (destinations.empty())
      return;

    std::vector<DescriptorRecord> copied;
    copied.reserve(destinations.size());
    for (UINT src_range = 0; src_range < src_descriptor_range_count; src_range++) {
      const UINT src_count = src_descriptor_range_sizes
                                 ? src_descriptor_range_sizes[src_range]
                                 : 1;
      if (!src_count)
        continue;
      auto *src = d3d12::GetDescriptorRecordRangeFromCpuHandle(
          src_descriptor_range_offsets[src_range], descriptor_heap_type,
          src_count, "CopyDescriptors source");
      if (!src)
        return;
      for (UINT i = 0; i < src_count && copied.size() < destinations.size(); i++)
        copied.push_back(src[i]);
      if (copied.size() == destinations.size())
        break;
    }
    if (copied.size() != destinations.size()) {
      WARN("D3D12Device: CopyDescriptors source descriptor count is smaller "
           "than destination descriptor count");
      return;
    }

    for (size_t i = 0; i < copied.size(); i++)
      CopyDescriptorRecord(*destinations[i], copied[i]);
  }

  void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT descriptor_count,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
                                               D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    if (!descriptor_count)
      return;
    auto *dst = d3d12::GetDescriptorRecordRangeFromCpuHandle(
        dst_descriptor_range_offset, descriptor_heap_type, descriptor_count,
        "CopyDescriptorsSimple destination");
    auto *src = d3d12::GetDescriptorRecordRangeFromCpuHandle(
        src_descriptor_range_offset, descriptor_heap_type, descriptor_count,
        "CopyDescriptorsSimple source");
    if (!dst || !src)
      return;
    std::vector<DescriptorRecord> copied(src, src + descriptor_count);
    for (UINT i = 0; i < copied.size(); i++)
      CopyDescriptorRecord(dst[i], copied[i]);
  }

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
    if (!resource)
      return E_POINTER;
    if (!IsValidCommittedResourceDesc(heap_properties, heap_flags, desc,
                                      initial_state))
      return E_INVALIDARG;

    auto resource_object = d3d12::CreateResource(
        static_cast<IMTLD3D12Device *>(this), heap_properties, heap_flags,
        desc, initial_state, 0);
    return resource_object->QueryInterface(riid, resource);
  }

  HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid,
                                       void **heap) override {
    InitReturnPtr(heap);
    if (!heap)
      return E_POINTER;
    if (!IsValidHeapDesc(desc))
      return E_INVALIDARG;

    auto heap_object = d3d12::CreateHeap(static_cast<IMTLD3D12Device *>(this),
                                         desc);
    return heap_object->QueryInterface(riid, heap);
  }

  HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *heap, UINT64 heap_offset,
                                                 const D3D12_RESOURCE_DESC *desc,
                                                 D3D12_RESOURCE_STATES initial_state,
                                                 const D3D12_CLEAR_VALUE *optimized_clear_value,
                                                 REFIID riid, void **resource) override {
    InitReturnPtr(resource);
    if (!resource)
      return E_POINTER;
    if (!heap || !desc)
      return E_INVALIDARG;

    auto *heap_object = dynamic_cast<d3d12::Heap *>(heap);
    if (!heap_object)
      return E_INVALIDARG;

    if (!IsValidPlacedResourceDesc(*heap_object, heap_offset, desc,
                                   initial_state))
      return E_INVALIDARG;

    const auto &heap_desc = heap_object->GetHeapDesc();
    auto resource_object = d3d12::CreateResource(
        static_cast<IMTLD3D12Device *>(this), &heap_desc.Properties,
        heap_desc.Flags, desc, initial_state, heap_offset);
    return resource_object->QueryInterface(riid, resource);
  }

  HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES initial_state,
                                                   const D3D12_CLEAR_VALUE *optimized_clear_value,
                                                   REFIID riid, void **resource) override {
    InitReturnPtr(resource);
    // TODO(d3d12): implement reserved resources together with tile mappings.
    WARN("D3D12Device: reserved resources are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *object,
                                               const SECURITY_ATTRIBUTES *attributes,
                                               DWORD access, const WCHAR *name,
                                               HANDLE *handle) override {
    if (handle)
      *handle = nullptr;
    // TODO(d3d12): export shareable NT handles once resource/process ownership
    // is represented outside the local COM object lifetime.
    WARN("D3D12Device: shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE handle, REFIID riid, void **object) override {
    InitReturnPtr(object);
    WARN("D3D12Device: shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(const WCHAR *name, DWORD access,
                                                   HANDLE *handle) override {
    if (handle)
      *handle = nullptr;
    WARN("D3D12Device: named shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE MakeResident(UINT object_count, ID3D12Pageable *const *objects) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Evict(UINT object_count, ID3D12Pageable *const *objects) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void *blob,
                                                  SIZE_T blob_size,
                                                  REFIID iid,
                                                  void **lib) override {
    InitReturnPtr(lib);
    if (!lib)
      return E_POINTER;
    if (!blob && blob_size)
      return E_INVALIDARG;

    if (blob_size)
      WARN("D3D12Device: ignoring serialized pipeline library blob");

#ifdef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
    auto library = d3d12::CreatePipelineLibrary(
        static_cast<IMTLD3D12Device *>(this));
    return library->QueryInterface(iid, lib);
#else
    return E_NOINTERFACE;
#endif
  }

  HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count,
      D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) override {
    if (!fence_count || !fences || !values)
      return E_INVALIDARG;
    if (static_cast<UINT>(flags) &
        ~static_cast<UINT>(D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY))
      return E_INVALIDARG;

    std::vector<Com<ID3D12Fence>> wait_fences;
    wait_fences.reserve(fence_count);
    std::vector<UINT64> wait_values(values, values + fence_count);
    for (UINT i = 0; i < fence_count; i++) {
      if (!fences[i])
        return E_INVALIDARG;
      wait_fences.emplace_back(fences[i]);
    }

    auto completed = [wait_fences = wait_fences,
                      wait_values = wait_values, flags]() {
      if (flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY) {
        for (size_t i = 0; i < wait_fences.size(); i++) {
          if (wait_fences[i]->GetCompletedValue() >= wait_values[i])
            return true;
        }
        return false;
      }

      for (size_t i = 0; i < wait_fences.size(); i++) {
        if (wait_fences[i]->GetCompletedValue() < wait_values[i])
          return false;
      }
      return true;
    };

    if (completed()) {
      if (event) {
        auto signal = device_->device().newSharedEvent();
        MTLSharedEvent_setWin32EventAtValue(
            signal.handle, device_->queue().GetSharedEventListener(), event, 1);
        signal.signalValue(1);
      }
      return S_OK;
    }

    if (!event) {
      while (!completed())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      return S_OK;
    }

    auto signal = multiple_fence_wait_signal_;
    auto signal_value = ++multiple_fence_wait_value_;
    MTLSharedEvent_setWin32EventAtValue(
        signal.handle, device_->queue().GetSharedEventListener(), event,
        signal_value);

    dxmt::thread([wait_fences = std::move(wait_fences),
                  wait_values = std::move(wait_values), flags,
                  signal = std::move(signal), signal_value]() mutable {
      auto completed = [&]() {
        if (flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY) {
          for (size_t i = 0; i < wait_fences.size(); i++) {
            if (wait_fences[i]->GetCompletedValue() >= wait_values[i])
              return true;
          }
          return false;
        }

        for (size_t i = 0; i < wait_fences.size(); i++) {
          if (wait_fences[i]->GetCompletedValue() < wait_values[i])
            return false;
        }
        return true;
      };

      while (!completed())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      signal.signalValue(signal_value);
    }).detach();

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  SetResidencyPriority(UINT object_count, ID3D12Pageable *const *objects,
                       const D3D12_RESIDENCY_PRIORITY *priorities) override {
    if (object_count && (!objects || !priorities))
      return E_INVALIDARG;
    for (UINT i = 0; i < object_count; i++) {
      if (!objects[i])
        return E_INVALIDARG;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS flags,
                                        REFIID riid, void **fence) override {
    InitReturnPtr(fence);
    if (flags != D3D12_FENCE_FLAG_NONE) {
      WARN("D3D12Device::CreateFence: unsupported fence flags ", flags);
      return E_NOTIMPL;
    }

    auto fence_object = d3d12::CreateFence(static_cast<IMTLD3D12Device *>(this), initial_value, flags);
    return fence_object->QueryInterface(riid, fence);
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
    GetCopyableFootprintsImpl(desc, first_sub_resource, sub_resource_count,
                              base_offset, layouts, row_count, row_size,
                              total_bytes);
  }

    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc,
                                              REFIID riid, void **heap) override {
      InitReturnPtr(heap);
      if (!heap)
        return E_POINTER;
      if (!desc || desc->Count == 0 || desc->NodeMask > 1)
        return E_INVALIDARG;

      switch (desc->Type) {
      case D3D12_QUERY_HEAP_TYPE_OCCLUSION:
      case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
      case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
      case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS:
        break;
      default:
        return E_INVALIDARG;
      }

      auto query_heap = d3d12::CreateQueryHeap(
          static_cast<IMTLD3D12Device *>(this), desc);
      return query_heap->QueryInterface(riid, heap);
    }

  HRESULT STDMETHODCALLTYPE SetStablePowerState(WINBOOL enable) override {
    if (enable)
      WARN("D3D12Device: stable power state is unsupported");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                                   ID3D12RootSignature *root_signature,
                                                   REFIID riid, void **command_signature) override {
    InitReturnPtr(command_signature);
    if (!command_signature)
      return E_POINTER;
    if (!desc || !desc->NumArgumentDescs || !desc->pArgumentDescs ||
        !desc->ByteStride || desc->NodeMask > 1)
      return E_INVALIDARG;

    UINT min_stride = 0;
    UINT operation_count = 0;
    for (UINT i = 0; i < desc->NumArgumentDescs; i++) {
      const auto &argument = desc->pArgumentDescs[i];
      const auto argument_size = IndirectArgumentByteSize(argument);
      if (!argument_size) {
        WARN("D3D12Device::CreateCommandSignature: unsupported indirect "
             "argument type ",
             argument.Type);
        return E_NOTIMPL;
      }
      min_stride += argument_size;
      if (IsIndirectOperationArgument(argument.Type)) {
        if (++operation_count > 1 || i + 1 != desc->NumArgumentDescs) {
          WARN("D3D12Device::CreateCommandSignature: unsupported indirect "
               "operation layout");
          return E_INVALIDARG;
        }
      }
      switch (argument.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        if (!root_signature)
          return E_INVALIDARG;
        break;
      default:
        break;
      }
    }
    if (!operation_count) {
      WARN("D3D12Device::CreateCommandSignature: missing indirect operation");
      return E_INVALIDARG;
    }
    if (desc->ByteStride < min_stride)
      return E_INVALIDARG;

    auto signature = d3d12::CreateCommandSignature(
        static_cast<IMTLD3D12Device *>(this), desc, root_signature);
    return signature->QueryInterface(riid, command_signature);
  }

  void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *resource, UINT *total_tile_count,
                                           D3D12_PACKED_MIP_INFO *packed_mip_info,
                                           D3D12_TILE_SHAPE *standard_tile_shape,
                                           UINT *sub_resource_tiling_count,
                                           UINT first_sub_resource_tiling,
                                           D3D12_SUBRESOURCE_TILING *sub_resource_tilings) override {
    // TODO(d3d12): return real tiling once reserved/tiled resources are
    // implemented. The device advertises TiledResourcesTier NOT_SUPPORTED.
    if (resource)
      WARN("D3D12Device: resource tiling is unsupported");
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
  ID3D12Device *AsD3D12Device() {
    return static_cast<ID3D12Device *>(static_cast<DeviceComBase *>(this));
  }

  D3D12_RESOURCE_ALLOCATION_INFO
  GetResourceAllocationInfoImpl(UINT visible_mask, UINT resource_desc_count,
                                const D3D12_RESOURCE_DESC *resource_descs) const {
    D3D12_RESOURCE_ALLOCATION_INFO info = {};
    info.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

    if (!resource_descs)
      return info;

    for (UINT i = 0; i < resource_desc_count; i++) {
      const auto &desc = resource_descs[i];
      UINT64 size = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        size = desc.Width;
      } else if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                 desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR) {
        const UINT mip_levels = desc.MipLevels ? desc.MipLevels : 1;
        const UINT subresources = mip_levels * desc.DepthOrArraySize;
        UINT64 total_bytes = 0;
        GetCopyableFootprintsImpl(&desc, 0, subresources, 0, nullptr, nullptr,
                                  nullptr, &total_bytes);
        size = total_bytes;
      }
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

  bool IsValidHeapDesc(const D3D12_HEAP_DESC *desc) const {
    if (!desc || desc->SizeInBytes == 0 || desc->Alignment > D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
      return false;
    if (desc->Alignment && desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
        desc->Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
      return false;
    if (desc->Properties.CreationNodeMask > 1 || desc->Properties.VisibleNodeMask > 1)
      return false;
    if (desc->Properties.Type == D3D12_HEAP_TYPE_CUSTOM) {
      switch (desc->Properties.CPUPageProperty) {
      case D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE:
      case D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE:
      case D3D12_CPU_PAGE_PROPERTY_WRITE_BACK:
        break;
      default:
        return false;
      }
      switch (desc->Properties.MemoryPoolPreference) {
      case D3D12_MEMORY_POOL_UNKNOWN:
      case D3D12_MEMORY_POOL_L0:
      case D3D12_MEMORY_POOL_L1:
        break;
      default:
        return false;
      }
      return true;
    }
    if (desc->Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN ||
        desc->Properties.MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN)
      return false;
    switch (desc->Properties.Type) {
    case D3D12_HEAP_TYPE_DEFAULT:
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
      return true;
    default:
      return false;
    }
  }

  bool IsValidHeapProperties(const D3D12_HEAP_PROPERTIES *properties) const {
    if (!properties)
      return false;

    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Properties = *properties;
    return IsValidHeapDesc(&heap_desc);
  }

  bool IsValidInitialState(const D3D12_HEAP_PROPERTIES &properties,
                           D3D12_RESOURCE_STATES initial_state) const {
    const auto heap_type = d3d12::GetHeapType(properties);
    if (heap_type == D3D12_HEAP_TYPE_UPLOAD)
      return initial_state == D3D12_RESOURCE_STATE_GENERIC_READ;
    if (heap_type == D3D12_HEAP_TYPE_READBACK)
      return initial_state == D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
  }

  bool IsValidCommittedResourceDesc(
      const D3D12_HEAP_PROPERTIES *heap_properties,
      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
      D3D12_RESOURCE_STATES initial_state) const {
    if (!IsValidHeapProperties(heap_properties) || !desc)
      return false;
    if (!d3d12::IsSupportedResourceDesc(*desc))
      return false;
    if (!IsValidInitialState(*heap_properties, initial_state))
      return false;

    const auto heap_type = d3d12::GetHeapType(*heap_properties);
    if ((heap_type == D3D12_HEAP_TYPE_UPLOAD ||
         heap_type == D3D12_HEAP_TYPE_READBACK) &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
      return false;

    return true;
  }

  bool IsValidPlacedResourceDesc(const d3d12::Heap &heap, UINT64 heap_offset,
                                 const D3D12_RESOURCE_DESC *desc,
                                 D3D12_RESOURCE_STATES initial_state) const {
    if (!desc || !d3d12::IsSupportedResourceDesc(*desc))
      return false;
    if (!IsValidInitialState(heap.GetHeapDesc().Properties, initial_state))
      return false;

    const auto allocation_info = GetResourceAllocationInfoImpl(1, 1, desc);
    if (allocation_info.SizeInBytes == 0)
      return false;
    if (heap_offset % allocation_info.Alignment)
      return false;
    if (heap_offset > heap.GetHeapDesc().SizeInBytes ||
        allocation_info.SizeInBytes > heap.GetHeapDesc().SizeInBytes - heap_offset)
      return false;

    const auto heap_type = heap.GetHeapType();
    if ((heap_type == D3D12_HEAP_TYPE_UPLOAD ||
         heap_type == D3D12_HEAP_TYPE_READBACK) &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
      return false;

    const auto heap_flags = heap.GetHeapDesc().Flags;
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_BUFFERS) &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return false;
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES) &&
        (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
      return false;
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES) &&
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        !(desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
      return false;

    return true;
  }

  void GetCopyableFootprintsImpl(
      const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource,
      UINT sub_resource_count, UINT64 base_offset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count,
      UINT64 *row_size, UINT64 *total_bytes) const {
    UINT64 offset = base_offset;

    if (!desc || sub_resource_count == 0) {
      if (total_bytes)
        *total_bytes = 0;
      return;
    }

    MTL_DXGI_FORMAT_DESC format = {};
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        FAILED(MTLQueryDXGIFormat(device_->device(), desc->Format, format))) {
      if (total_bytes)
        *total_bytes = 0;
      return;
    }

    const UINT mip_levels = desc->MipLevels ? desc->MipLevels : 1;
    for (UINT i = 0; i < sub_resource_count; i++) {
      const UINT subresource = first_sub_resource + i;
      D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
      UINT rows = 1;
      UINT64 unpadded_row_size = desc->Width;
      UINT64 subresource_size = desc->Width;

      footprint.Format = desc->Format;

      if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        footprint.Width = static_cast<UINT>(desc->Width);
        footprint.Height = 1;
        footprint.Depth = 1;
        footprint.RowPitch =
            static_cast<UINT>(Align(desc->Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
      } else {
        const UINT mip_slice = SubresourceMipSlice(subresource, mip_levels);
        const UINT64 width = MipSize(desc->Width, mip_slice);
        const UINT height = static_cast<UINT>(MipSize(desc->Height, mip_slice));
        const UINT depth = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               ? static_cast<UINT>(MipSize(desc->DepthOrArraySize, mip_slice))
                               : 1;

        footprint.Width = static_cast<UINT>(width);
        footprint.Height = height;
        footprint.Depth = depth;

        if (format.Flag & MTL_DXGI_FORMAT_BC) {
          const UINT block_width = static_cast<UINT>((width + 3) / 4);
          const UINT block_height = (height + 3) / 4;
          rows = std::max(1u, block_height);
          unpadded_row_size = block_width * format.BlockSize;
        } else {
          rows = height;
          unpadded_row_size = width * format.BytesPerTexel;
        }

        footprint.RowPitch = static_cast<UINT>(
            Align(unpadded_row_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        subresource_size = footprint.RowPitch * rows * depth;
      }

      offset = Align(offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      if (layouts) {
        layouts[i].Offset = offset;
        layouts[i].Footprint = footprint;
      }
      if (row_count)
        row_count[i] = rows;
      if (row_size)
        row_size[i] = unpadded_row_size;

      offset += subresource_size;
    }

    if (total_bytes)
      *total_bytes = offset - base_offset;
  }

  Com<IMTLDXGIAdapter> adapter_;
  std::unique_ptr<dxmt::Device> device_;
  ComPrivateData private_data_;
  WMT::Reference<WMT::SharedEvent> enqueue_set_event_signal_;
  WMT::Reference<WMT::SharedEvent> multiple_fence_wait_signal_;
  D3DKMT_HANDLE local_kmt_ = 0;
  LUID adapter_luid_ = {};
  UINT maximum_frame_latency_ = 3;
  INT gpu_thread_priority_ = 0;
  uint64_t timestamp_query_value_ = 0;
  uint64_t enqueue_set_event_value_ = 0;
  uint64_t multiple_fence_wait_value_ = 0;
  std::string name_;
};

} // namespace

Com<IMTLD3D12Device>
CreateD3D12Device(std::unique_ptr<dxmt::Device> &&device, IMTLDXGIAdapter *adapter) {
  return Com<IMTLD3D12Device>::transfer(new DeviceImpl(std::move(device), adapter));
}

} // namespace dxmt::d3d12
