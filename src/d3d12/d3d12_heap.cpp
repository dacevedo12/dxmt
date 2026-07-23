#include "d3d12_heap.hpp"

#include "dxmt_command_queue.hpp"
#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

#include <atomic>
#include <mutex>

namespace dxmt::d3d12 {
namespace {

static bool
ShouldLogExternalCpuHeapDiag() {
  static std::atomic<uint32_t> count = 0;
  return count.fetch_add(1, std::memory_order_relaxed) < 8;
}

static UINT64
GetBackendHeapSize(const D3D12_HEAP_DESC &desc) {
  const UINT64 alignment = desc.Alignment
                               ? desc.Alignment
                               : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  if (!alignment || (alignment & (alignment - 1)) ||
      desc.SizeInBytes > UINT64_MAX - (alignment - 1))
    return 0;
  return (desc.SizeInBytes + alignment - 1) & ~(alignment - 1);
}

#ifdef __ID3D12Heap1_INTERFACE_DEFINED__
using HeapComInterface = ID3D12Heap1;
#else
using HeapComInterface = ID3D12Heap;
#endif

class HeapImpl final : public ComObjectWithInitialRef<HeapComInterface>,
                       public Heap {
public:
  HeapImpl(IMTLD3D12Device *device, const D3D12_HEAP_DESC &desc)
      : device_(device), desc_(desc),
        heap_type_(d3d12::GetHeapType(desc.Properties)),
        cpu_visible_(d3d12::IsCpuVisibleHeap(desc.Properties)) {
    if (!desc_.Alignment)
      desc_.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    backend_size_ = GetBackendHeapSize(desc_);
    if (!desc_.Properties.CreationNodeMask)
      desc_.Properties.CreationNodeMask = 1;
    if (!desc_.Properties.VisibleNodeMask)
      desc_.Properties.VisibleNodeMask = 1;
  }

  HeapImpl(IMTLD3D12Device *device, const D3D12_HEAP_DESC &desc,
           const void *external_address)
      : HeapImpl(device, desc) {
    external_address_ = external_address;
  }

  ~HeapImpl() {
    auto &queue = device_->GetDXMTDevice().queue();
    if (placement_heap_)
      queue.RemovePersistentResidencyAfterCompletion(placement_heap_);
    if (allocation_)
      queue.RemovePersistentResidencyAfterCompletion(allocation_->buffer());
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Heap) ||
        riid == __uuidof(HeapComInterface)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Heap), riid))
      WARN("D3D12Heap: unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                   const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  IMTLD3D12Device *GetParentDevice() const override {
    return device_.ptr();
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_HEAP_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }
#else
  D3D12_HEAP_DESC STDMETHODCALLTYPE GetDesc() override {
    return desc_;
  }
#endif

#ifdef __ID3D12Heap1_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(
      REFIID riid, void **protected_session) override {
    InitReturnPtr(protected_session);
    if (!protected_session)
      return E_POINTER;
    return DXGI_ERROR_NOT_FOUND;
  }
#endif

  const D3D12_HEAP_DESC &GetHeapDesc() const override {
    return desc_;
  }

  D3D12_HEAP_TYPE GetHeapType() const override {
    return heap_type_;
  }

  bool IsCpuVisible() const override {
    return cpu_visible_;
  }

  dxmt::Buffer *GetBuffer() const override {
    EnsureBufferAllocation();
    return buffer_.ptr();
  }

  dxmt::BufferAllocation *GetAllocation() const override {
    EnsureBufferAllocation();
    return allocation_.ptr();
  }

  void EnsureBufferAllocation() const {
    std::lock_guard lock(allocation_mutex_);
    if (!allocation_) {
      buffer_ = new dxmt::Buffer(backend_size_,
                                 device_->GetDXMTDevice().device());
      if (external_address_) {
        if (ShouldLogExternalCpuHeapDiag()) {
          WARN("D3D12Heap: OpenExistingHeapFromAddress using external CPU backing"
           " address=", external_address_,
           " size=", desc_.SizeInBytes,
           " backendSize=", backend_size_,
           " flags=", desc_.Flags);
        }
        allocation_ = buffer_->allocateExternalCpu(
            GetHeapBufferAllocationFlags(desc_.Properties),
            const_cast<void *>(external_address_));
      } else {
        allocation_ = buffer_->allocate(GetHeapBufferAllocationFlags(desc_.Properties));
      }
      buffer_->rename(Rc<dxmt::BufferAllocation>(allocation_));
      if (allocation_)
        device_->GetDXMTDevice().queue().AddPersistentResidency(
            allocation_->buffer());
    }
  }

  WMT::Reference<WMT::Heap> GetPlacementHeap() override {
    std::lock_guard lock(allocation_mutex_);
    if (placement_heap_)
      return placement_heap_;
    if (external_address_)
      return {};

    WMTPlacementHeapInfo info = {};
    info.size = backend_size_;
    info.options = WMTResourceHazardTrackingModeUntracked;
    if (heap_type_ == D3D12_HEAP_TYPE_DEFAULT)
      info.options |= WMTResourceStorageModePrivate;
    else if (heap_type_ == D3D12_HEAP_TYPE_UPLOAD)
      info.options |= WMTResourceOptionCPUCacheModeWriteCombined;
    // READBACK / other: leave Shared (no Private bit).
    placement_heap_ =
        device_->GetDXMTDevice().device().newPlacementHeap(info);
    if (!placement_heap_) {
      WARN("D3D12Heap: TODO failed to create Metal4 placement heap"
           " size=", desc_.SizeInBytes,
           " backendSize=", backend_size_,
           " heapType=", heap_type_,
           " flags=", desc_.Flags,
           " options=", info.options);
    } else {
      static std::atomic<uint32_t> placement_heap_diag = 0;
      const uint32_t n =
          placement_heap_diag.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 16 || (n % 64) == 0) {
        WARN("D3D12Heap: placement heap created"
             " size=", desc_.SizeInBytes,
             " backendSize=", backend_size_,
             " heapType=", heap_type_,
             " cpuVisible=", cpu_visible_,
             " flags=", desc_.Flags,
             " options=", info.options,
             " metalHeap=", placement_heap_.handle,
             " count=", n);
      }
      device_->GetDXMTDevice().queue().AddPersistentResidency(
          placement_heap_);
    }
    return placement_heap_;
  }

private:
  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_HEAP_DESC desc_ = {};
  D3D12_HEAP_TYPE heap_type_ = D3D12_HEAP_TYPE_DEFAULT;
  bool cpu_visible_ = false;
  UINT64 backend_size_ = 0;
  mutable Rc<dxmt::Buffer> buffer_;
  mutable Rc<dxmt::BufferAllocation> allocation_;
  WMT::Reference<WMT::Heap> placement_heap_;
  mutable std::mutex allocation_mutex_;
  const void *external_address_ = nullptr;
  std::string name_;
};

} // namespace

D3D12_HEAP_TYPE
GetHeapType(const D3D12_HEAP_PROPERTIES &properties) {
  if (properties.Type != D3D12_HEAP_TYPE_CUSTOM)
    return properties.Type;

  if (properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
    return D3D12_HEAP_TYPE_DEFAULT;

  if (properties.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE)
    return D3D12_HEAP_TYPE_UPLOAD;

  return D3D12_HEAP_TYPE_READBACK;
}

bool
IsCpuVisibleHeap(const D3D12_HEAP_PROPERTIES &properties) {
  const auto type = GetHeapType(properties);
  return type == D3D12_HEAP_TYPE_UPLOAD || type == D3D12_HEAP_TYPE_READBACK;
}

Flags<dxmt::BufferAllocationFlag>
GetHeapBufferAllocationFlags(const D3D12_HEAP_PROPERTIES &properties) {
  Flags<dxmt::BufferAllocationFlag> flags;
  switch (GetHeapType(properties)) {
  case D3D12_HEAP_TYPE_UPLOAD:
    flags.set(dxmt::BufferAllocationFlag::CpuWriteCombined);
    break;
  case D3D12_HEAP_TYPE_READBACK:
    break;
  case D3D12_HEAP_TYPE_DEFAULT:
  default:
    flags.set(dxmt::BufferAllocationFlag::CpuInvisible);
    flags.set(dxmt::BufferAllocationFlag::GpuPrivate);
    break;
  }
  return flags;
}

Com<ID3D12Heap>
CreateHeap(IMTLD3D12Device *device, const D3D12_HEAP_DESC *desc) {
  return Com<ID3D12Heap>::transfer(new HeapImpl(device, *desc));
}

Com<ID3D12Heap>
CreateExternalCpuHeap(IMTLD3D12Device *device, const D3D12_HEAP_DESC *desc,
                      const void *address) {
  return Com<ID3D12Heap>::transfer(new HeapImpl(device, *desc, address));
}

} // namespace dxmt::d3d12
