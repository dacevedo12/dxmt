#include "d3d12_fence.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <atomic>
#include <mutex>
#include <vector>

namespace dxmt::d3d12 {
namespace {

struct PendingEvent {
  UINT64 value;
  HANDLE event;
};

class FenceImpl final : public ComObjectWithInitialRef<ID3D12Fence>, public Fence {
public:
  FenceImpl(IMTLD3D12Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags)
      : device_(device), value_(initial_value), flags_(flags) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12Fence)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Fence), riid))
      WARN("D3D12Fence: unknown interface query ", str::format(riid));

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

  UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
    return value_.load(std::memory_order_acquire);
  }

  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override {
    if (!event)
      return E_INVALIDARG;

    std::lock_guard lock(mutex_);
    if (GetCompletedValue() >= value) {
      ::SetEvent(event);
      return S_OK;
    }

    pending_events_.push_back({value, event});

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {
    return SignalFromQueue(value);
  }

  HRESULT SignalFromQueue(UINT64 value) override {
    value_.store(value, std::memory_order_release);
    FlushPendingEvents();
    return S_OK;
  }

  bool HasReached(UINT64 value) const override {
    return value_.load(std::memory_order_acquire) >= value;
  }

private:
  void FlushPendingEvents() {
    std::lock_guard lock(mutex_);
    const auto completed = GetCompletedValue();

    for (auto it = pending_events_.begin(); it != pending_events_.end();) {
      if (completed >= it->value) {
        ::SetEvent(it->event);
        it = pending_events_.erase(it);
      } else {
        ++it;
      }
    }
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  std::atomic<UINT64> value_;
  D3D12_FENCE_FLAGS flags_;
  std::mutex mutex_;
  std::vector<PendingEvent> pending_events_;
  std::string name_;
};

} // namespace

Com<ID3D12Fence>
CreateFence(IMTLD3D12Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags) {
  return Com<ID3D12Fence>::transfer(new FenceImpl(device, initial_value, flags));
}

} // namespace dxmt::d3d12
