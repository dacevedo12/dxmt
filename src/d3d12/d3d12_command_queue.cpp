#include "d3d12_command_queue.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_fence.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstring>
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

      if (SUCCEEDED(state->MarkSubmittedToQueue(desc_.Type)))
        submitted_any = true;
    }

    if (submitted_any)
      submitted_batches_++;
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

    HRESULT hr = state->SignalFromQueue(value);
    if (SUCCEEDED(hr)) {
      signal_count_++;
      last_signal_value_ = value;
    }
    return hr;
  }

  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *fence, UINT64 value) override {
    if (!fence)
      return E_INVALIDARG;

    auto *state = dynamic_cast<Fence *>(fence);
    if (!state)
      return E_INVALIDARG;

    wait_values_.push_back(value);
    if (!state->HasReached(value))
      Logger::info(str::format("D3D12CommandQueue: queued CPU-side wait placeholder for fence value ", value));

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
  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_COMMAND_QUEUE_DESC desc_ = {};
  UINT64 submitted_batches_ = 0;
  UINT64 signal_count_ = 0;
  UINT64 last_signal_value_ = 0;
  std::vector<UINT64> wait_values_;
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
