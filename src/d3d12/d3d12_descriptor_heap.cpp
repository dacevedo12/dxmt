#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"

#include "dxmt_lease_range_registry.hpp"
#include "dxmt_d3d12_test_path.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

#include <memory>

namespace dxmt::d3d12 {
namespace {

dxmt::DescriptorRevisionClock g_descriptor_content_revision;

dxmt::LeaseRangeRegistry<DescriptorRecord, DescriptorHeap>
    g_descriptor_heap_ranges;

bool RegisterDescriptorHeapRange(DescriptorRecord *records, UINT count,
                                 DescriptorHeap *owner) {
  if (g_descriptor_heap_ranges.Register(records, count, owner))
    return true;
  ERR("D3D12DescriptorHeap: failed to register descriptor handle range");
  return false;
}

void UnregisterDescriptorHeapRange(DescriptorRecord *records,
                                   DescriptorHeap *owner) {
  g_descriptor_heap_ranges.Unregister(records, owner);
}

DescriptorRecordLease LookupDescriptorRecord(uintptr_t address) {
  return g_descriptor_heap_ranges.Lookup(
      address,
      [](const DescriptorRecord *record) {
        return record->magic == DescriptorRecord::kMagic;
      },
      [](DescriptorRecord *record, DescriptorHeap *owner) {
        // The registry's shared lock remains held during Acquire. Its owner
        // reference therefore cannot be released by final public Release
        // until this private lease has been retained.
        return DescriptorRecordLease::Acquire(record, owner);
      });
}

bool DescriptorHeapSupportsMirror(const D3D12_DESCRIPTOR_HEAP_DESC &desc) {
  if (!(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
    return false;

  return desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
         desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
}

class DescriptorHeapImpl final
    : public ComObjectWithInitialRef<ID3D12DescriptorHeap>,
      public DescriptorHeap {
public:
  DescriptorHeapImpl(IMTLD3D12Device *device,
                     const D3D12_DESCRIPTOR_HEAP_DESC &desc)
      : device_(device), desc_(desc), records_(desc.NumDescriptors) {
    for (UINT i = 0; i < desc.NumDescriptors; i++) {
      records_[i].magic = DescriptorRecord::kMagic;
      records_[i].heap_type = desc.Type;
      records_[i].shader_visible =
          (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0;
      records_[i].cpu_handle.ptr =
          reinterpret_cast<SIZE_T>(&records_[i]);
      records_[i].heap_index = i;
      records_[i].heap_count = desc.NumDescriptors;
    }
    // Eagerly allocate the unified descriptor owner for shader-visible
    // CBV/SRV/UAV and SAMPLER heaps, and back-fill the per-record back-pointer.
    // The owner preserves the fallback mirrors and owns the MSC descriptor
    // table/resource-record buffers. Native packets bind those buffers into the
    // command encoder's per-stage argument table at fixed ABI slots. Eager
    // construction avoids cross-thread first-touch ordering between descriptor
    // writes and replay.
    const bool sampler_heap = desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    if (DescriptorHeapSupportsMirror(desc_)) {
      mirror_ = std::make_unique<DescriptorHeapMirror>(
          device_->GetMTLDevice(), desc.NumDescriptors, sampler_heap);
      auto &queue = device_->GetDXMTDevice().queue();
      auto register_descriptor_allocation =
          [&](WMT::Object allocation, uint32_t component) {
            residency_registrations_.push_back(
                queue.RegisterLifetimeResidency(
                    dxmt::ResidencyOwnership::Lifetime(
                        allocation,
                        dxmt::ResidencyProvenance{
                            .kind =
                                dxmt::ResidencyProvenanceKind::DescriptorHeap,
                            .owner = reinterpret_cast<uintptr_t>(this),
                            .identity = static_cast<uint64_t>(desc_.Type),
                            .size = desc_.NumDescriptors,
                            .component = component,
                        })));
          };
      register_descriptor_allocation(mirror_->buffer(), 0);
      register_descriptor_allocation(mirror_->descriptorTableBuffer(), 1);
      if (!sampler_heap) {
        register_descriptor_allocation(
            mirror_->bufferDescriptorRecordBuffer(), 2);
        register_descriptor_allocation(
            mirror_->bufferResourceTableBuffer(), 3);
      }
      for (UINT i = 0; i < desc.NumDescriptors; i++)
        records_[i].mirror = mirror_.get();
    }
    // Keep one private reference on behalf of the global handle registry. The
    // final public Release unregisters the range before dropping this owner,
    // making it safe for a concurrent lookup to take its own private lease.
    AddRefPrivate();
    if (RegisterDescriptorHeapRange(records_.data(), desc.NumDescriptors,
                                    this)) {
      registry_registered_.store(true, std::memory_order_release);
    } else {
      ReleasePrivate();
    }
  }

  ~DescriptorHeapImpl() {
    // Normally final public Release has already removed the range. Keep this
    // defensive erase owner-qualified so an address-reused heap can never be
    // removed by a stale destructor.
    UnregisterDescriptorHeapRange(records_.data(), this);
    if (!mirror_)
      return;
    auto &queue = device_->GetDXMTDevice().queue();
    for (auto &target : mirror_->DrainResidencyTargets()) {
      if (target.sampler)
        queue.RetainGpuOwner(std::move(target.sampler));
    }
    residency_registrations_.clear();
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const uint32_t ref_count = --m_refCount;
    if (!ref_count) {
      if (registry_registered_.exchange(false, std::memory_order_acq_rel)) {
        UnregisterDescriptorHeapRange(records_.data(), this);
        // Drop registry ownership first, while the public-reference private
        // count still guarantees that `this` remains valid for the final drop.
        ReleasePrivate();
      }
      // This may delete the object. Do not access members after this call.
      ReleasePrivate();
    }
    return ref_count;
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12DescriptorHeap)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12DescriptorHeap), riid))
      WARN("D3D12DescriptorHeap: unknown interface query ", str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    if (guid == dxmt::d3d12::test::kDescriptorHeapSlotRepairGuid) {
      using dxmt::d3d12::test::DescriptorHeapSlotRepairConfig;
      if (!data || data_size != sizeof(DescriptorHeapSlotRepairConfig) ||
          !mirror_)
        return E_INVALIDARG;
      const auto &config =
          *static_cast<const DescriptorHeapSlotRepairConfig *>(data);
      if (config.struct_size != sizeof(config) ||
          config.slot >= records_.size())
        return E_INVALIDARG;
      auto lock = mirror_->AcquireLock();
      records_[config.slot].slot_version =
          mirror_->BeginSlotWrite(lock, config.slot);
      return records_[config.slot].slot_version ? S_OK : E_FAIL;
    }
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

  IMTLD3D12Device *GetParentDevice() const override { return device_.ptr(); }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_DESCRIPTOR_HEAP_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DESCRIPTOR_HEAP_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart(D3D12_CPU_DESCRIPTOR_HANDLE *__ret) override {
    *__ret = GetCPUDescriptorHandleForHeapStartImpl();
    return __ret;
  }

  D3D12_GPU_DESCRIPTOR_HANDLE *STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart(D3D12_GPU_DESCRIPTOR_HANDLE *__ret) override {
    *__ret = GetGPUDescriptorHandleForHeapStartImpl();
    return __ret;
  }
#else
  D3D12_DESCRIPTOR_HEAP_DESC STDMETHODCALLTYPE GetDesc() override {
    return desc_;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE
  GetCPUDescriptorHandleForHeapStart() override {
    return GetCPUDescriptorHandleForHeapStartImpl();
  }

  D3D12_GPU_DESCRIPTOR_HANDLE STDMETHODCALLTYPE
  GetGPUDescriptorHandleForHeapStart() override {
    return GetGPUDescriptorHandleForHeapStartImpl();
  }
#endif

  const D3D12_DESCRIPTOR_HEAP_DESC &GetDescriptorHeapDesc() const override {
    return desc_;
  }

  DescriptorRecord *
  GetDescriptorRecord(D3D12_CPU_DESCRIPTOR_HANDLE handle) override {
    return DescriptorRecordFromHandle(handle);
  }

  const DescriptorRecord *
  GetDescriptorRecord(D3D12_CPU_DESCRIPTOR_HANDLE handle) const override {
    return DescriptorRecordFromHandle(handle);
  }

  const DescriptorRecord *
  GetDescriptorRecord(D3D12_GPU_DESCRIPTOR_HANDLE handle) const override {
    return DescriptorRecordFromHandle({static_cast<SIZE_T>(handle.ptr)});
  }

  DescriptorHeapMirror *GetMirror() override { return mirror_.get(); }

  void AcquireDescriptorRecordLease() override { AddRefPrivate(); }
  void ReleaseDescriptorRecordLease() override { ReleasePrivate(); }

private:
  D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandleForHeapStartImpl() const {
    if (records_.empty())
      return {};
    return records_[0].cpu_handle;
  }

  D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandleForHeapStartImpl() const {
    if (!(desc_.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) ||
        records_.empty())
      return {};

    D3D12_GPU_DESCRIPTOR_HANDLE handle = {};
    handle.ptr = records_[0].cpu_handle.ptr;
    return handle;
  }

  DescriptorRecord *DescriptorRecordFromHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const uintptr_t address = static_cast<uintptr_t>(handle.ptr);
    if (!address || records_.empty())
      return nullptr;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(records_.data());
    const uintptr_t size = records_.size() * sizeof(DescriptorRecord);
    if (address < begin || address - begin >= size ||
        (address - begin) % sizeof(DescriptorRecord))
      return nullptr;
    const size_t record_index =
        static_cast<size_t>((address - begin) / sizeof(DescriptorRecord));
    return &records_[record_index];
  }

  const DescriptorRecord *
  DescriptorRecordFromHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
    const uintptr_t address = static_cast<uintptr_t>(handle.ptr);
    if (!address || records_.empty())
      return nullptr;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(records_.data());
    const uintptr_t size = records_.size() * sizeof(DescriptorRecord);
    if (address < begin || address - begin >= size ||
        (address - begin) % sizeof(DescriptorRecord))
      return nullptr;
    const size_t record_index =
        static_cast<size_t>((address - begin) / sizeof(DescriptorRecord));
    return &records_[record_index];
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_DESCRIPTOR_HEAP_DESC desc_ = {};
  std::vector<DescriptorRecord> records_;
  std::string name_;
  std::unique_ptr<DescriptorHeapMirror> mirror_;
  std::vector<std::shared_ptr<dxmt::LifetimeResidencyRegistration>>
      residency_registrations_;
  std::atomic<bool> registry_registered_{false};
};

} // namespace

DescriptorRecordLease::DescriptorRecordLease(
    DescriptorRecordLease &&other) noexcept
    : record_(other.record_), owner_(other.owner_) {
  other.record_ = nullptr;
  other.owner_ = nullptr;
}

DescriptorRecordLease &
DescriptorRecordLease::operator=(DescriptorRecordLease &&other) noexcept {
  if (this == &other)
    return *this;
  reset();
  record_ = other.record_;
  owner_ = other.owner_;
  other.record_ = nullptr;
  other.owner_ = nullptr;
  return *this;
}

DescriptorRecordLease::~DescriptorRecordLease() {
  reset();
}

DescriptorRecordLease
DescriptorRecordLease::Acquire(DescriptorRecord *record,
                               DescriptorHeap *owner) {
  if (!record || !owner)
    return {};
  owner->AcquireDescriptorRecordLease();
  return DescriptorRecordLease(record, owner);
}

void DescriptorRecordLease::reset() {
  auto *owner = owner_;
  record_ = nullptr;
  owner_ = nullptr;
  if (owner)
    owner->ReleaseDescriptorRecordLease();
}

dxmt::DescriptorContentRevision BumpDescriptorContentRevision() {
  return g_descriptor_content_revision.Bump();
}

dxmt::DescriptorContentRevision GetDescriptorContentRevision() {
  return g_descriptor_content_revision.Load();
}

Com<ID3D12DescriptorHeap>
CreateDescriptorHeap(IMTLD3D12Device *device,
                     const D3D12_DESCRIPTOR_HEAP_DESC *desc) {
  return Com<ID3D12DescriptorHeap>::transfer(new DescriptorHeapImpl(device, *desc));
}

DescriptorRecordLease
GetDescriptorRecordFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  return LookupDescriptorRecord(static_cast<uintptr_t>(handle.ptr));
}

DescriptorRecordLease
GetDescriptorRecordFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                 D3D12_DESCRIPTOR_HEAP_TYPE expected_type) {
  auto record = GetDescriptorRecordFromCpuHandle(handle);
  if (!record)
    return {};
  if (record->heap_type != expected_type) {
    WARN("D3D12DescriptorHeap: descriptor heap type mismatch expected=",
         uint32_t(expected_type), " actual=", uint32_t(record->heap_type));
    return {};
  }
  return record;
}

DescriptorRecordLease
GetDescriptorRecordFromGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle,
                                 D3D12_DESCRIPTOR_HEAP_TYPE expected_type) {
  auto record =
      GetDescriptorRecordFromCpuHandle({static_cast<SIZE_T>(handle.ptr)},
                                       expected_type);
  if (!record || !record->shader_visible) {
    if (record)
      WARN("D3D12DescriptorHeap: non shader-visible descriptor used as GPU handle");
    return {};
  }
  return record;
}

DescriptorRecordLease
GetDescriptorRecordRangeFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                      D3D12_DESCRIPTOR_HEAP_TYPE expected_type,
                                      UINT descriptor_count,
                                      const char *context) {
  auto record = GetDescriptorRecordFromCpuHandle(handle, expected_type);
  if (!record)
    return {};
  if (descriptor_count &&
      (record->heap_index >= record->heap_count ||
       descriptor_count > record->heap_count - record->heap_index)) {
    WARN("D3D12DescriptorHeap: descriptor range exceeds heap for ",
         context ? context : "<unknown>", " start=", record->heap_index,
         " count=", descriptor_count, " heap_count=", record->heap_count);
    return {};
  }
  return record;
}

} // namespace dxmt::d3d12
