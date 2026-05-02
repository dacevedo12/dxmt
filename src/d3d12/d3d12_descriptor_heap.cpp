#include "d3d12_descriptor_heap.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

namespace dxmt::d3d12 {
namespace {

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
    }
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
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                   const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

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
    auto *record = reinterpret_cast<DescriptorRecord *>(handle.ptr);
    if (!record || records_.empty())
      return nullptr;
    if (record < records_.data() || record >= records_.data() + records_.size())
      return nullptr;
    return record;
  }

  const DescriptorRecord *
  DescriptorRecordFromHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
    auto *record = reinterpret_cast<const DescriptorRecord *>(handle.ptr);
    if (!record || records_.empty())
      return nullptr;
    if (record < records_.data() || record >= records_.data() + records_.size())
      return nullptr;
    return record;
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_DESCRIPTOR_HEAP_DESC desc_ = {};
  std::vector<DescriptorRecord> records_;
  std::string name_;
};

} // namespace

Com<ID3D12DescriptorHeap>
CreateDescriptorHeap(IMTLD3D12Device *device,
                     const D3D12_DESCRIPTOR_HEAP_DESC *desc) {
  return Com<ID3D12DescriptorHeap>::transfer(new DescriptorHeapImpl(device, *desc));
}

DescriptorRecord *
GetDescriptorRecordFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
  auto *record = reinterpret_cast<DescriptorRecord *>(handle.ptr);
  if (!record || record->magic != DescriptorRecord::kMagic)
    return nullptr;
  return record;
}

DescriptorRecord *
GetDescriptorRecordFromCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                                 D3D12_DESCRIPTOR_HEAP_TYPE expected_type) {
  auto *record = GetDescriptorRecordFromCpuHandle(handle);
  if (!record)
    return nullptr;
  if (record->heap_type != expected_type) {
    WARN("D3D12DescriptorHeap: descriptor heap type mismatch expected=",
         uint32_t(expected_type), " actual=", uint32_t(record->heap_type));
    return nullptr;
  }
  return record;
}

const DescriptorRecord *
GetDescriptorRecordFromGpuHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle,
                                 D3D12_DESCRIPTOR_HEAP_TYPE expected_type) {
  auto *record =
      GetDescriptorRecordFromCpuHandle({static_cast<SIZE_T>(handle.ptr)},
                                       expected_type);
  if (!record || !record->shader_visible) {
    if (record)
      WARN("D3D12DescriptorHeap: non shader-visible descriptor used as GPU handle");
    return nullptr;
  }
  return record;
}

} // namespace dxmt::d3d12
