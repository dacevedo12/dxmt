#include "d3d12_command_allocator.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <mutex>

namespace dxmt::d3d12 {
namespace {

class CommandAllocatorImpl final : public ComObjectWithInitialRef<CommandAllocatorObject> {
public:
  CommandAllocatorImpl(IMTLD3D12Device *device, D3D12_COMMAND_LIST_TYPE type)
      : device_(device), type_(type) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandAllocator)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12CommandAllocator), riid))
      WARN("D3D12CommandAllocator: unknown interface query ", str::format(riid));

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
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE Reset() override {
    std::lock_guard lock(mutex_);
    if (recording_list_)
      return E_FAIL;
    // GPU-side allocator lifetime is the application's fence responsibility.
    // DXMT copies command records on ExecuteCommandLists, so a reset does not
    // invalidate queued submissions that still reference this allocator.
    return S_OK;
  }

  D3D12_COMMAND_LIST_TYPE GetCommandListType() const override {
    return type_;
  }

  bool BeginCommandListRecording(void *command_list) override {
    std::lock_guard lock(mutex_);
    if (recording_list_)
      return false;

    recording_list_ = command_list;
    return true;
  }

  void EndCommandListRecording(void *command_list) override {
    std::lock_guard lock(mutex_);
    if (recording_list_ == command_list)
      recording_list_ = nullptr;
  }

  UINT64 MarkCommandListSubmitted() override {
    std::lock_guard lock(mutex_);
    pending_submission_count_++;
    return ++last_submission_serial_;
  }

  void CompleteCommandListSubmission(UINT64 serial) override {
    std::lock_guard lock(mutex_);
    if (serial && serial <= last_completed_submission_serial_)
      return;
    last_completed_submission_serial_ = serial;
    if (pending_submission_count_)
      pending_submission_count_--;
  }

  void AddRefPrivate() override {
    ComObjectWithInitialRef<CommandAllocatorObject>::AddRefPrivate();
  }

  void ReleasePrivate() override {
    ComObjectWithInitialRef<CommandAllocatorObject>::ReleasePrivate();
  }

private:
  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_COMMAND_LIST_TYPE type_;
  std::mutex mutex_;
  void *recording_list_ = nullptr;
  UINT64 last_submission_serial_ = 0;
  UINT64 last_completed_submission_serial_ = 0;
  UINT pending_submission_count_ = 0;
  std::string name_;
};

} // namespace

Com<ID3D12CommandAllocator>
CreateCommandAllocator(IMTLD3D12Device *device, D3D12_COMMAND_LIST_TYPE type) {
  return Com<ID3D12CommandAllocator>::transfer(new CommandAllocatorImpl(device, type));
}

} // namespace dxmt::d3d12
