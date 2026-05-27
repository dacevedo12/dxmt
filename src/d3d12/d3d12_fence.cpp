#include "d3d12_fence.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "util_win32_compat.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dxmt::d3d12 {
namespace {

static void
RunCompletionCallbacksAsync(std::vector<std::function<void()>> callbacks) {
  for (auto &callback : callbacks)
    callback();
}

class FenceImpl final : public ComObjectWithInitialRef<ID3D12Fence>, public Fence {
public:
  FenceImpl(IMTLD3D12Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags)
      : device_(device), event_(device->GetMTLDevice().newSharedEvent()), flags_(flags),
        completed_value_(initial_value), has_manual_completed_value_(false) {
    event_.signalValue(initial_value);
  }

  ~FenceImpl() override {
    std::vector<HANDLE> events_to_signal;
    std::vector<std::function<void()>> callbacks_to_run;
    {
      std::lock_guard lock(mutex_);
      completed_value_ = UINT64_MAX;
      has_manual_completed_value_ = true;
      for (const auto &pending : pending_events_)
        events_to_signal.push_back(pending.event);
      pending_events_.clear();
      for (auto &pending : pending_callbacks_) {
        callbacks_to_run.push_back(std::move(pending.callback));
      }
      pending_callbacks_.clear();
    }
    for (HANDLE event : events_to_signal)
      SetEvent(event);
    for (auto &callback : callbacks_to_run)
      callback();
  }

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
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
    std::lock_guard lock(mutex_);
    return GetCompletedValueLocked();
  }

  UINT64 GetCompletedValue() const override {
    std::lock_guard lock(mutex_);
    return GetCompletedValueLocked();
  }

  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override {
    if (!event) {
      while (true) {
        {
          std::lock_guard lock(mutex_);
          if (GetCompletedValueLocked() >= value)
            return S_OK;
        }
        dxmt::this_thread::yield();
      }
    }

    bool signal_now = false;
    {
      std::lock_guard lock(mutex_);
      if (GetCompletedValueLocked() >= value) {
        signal_now = true;
      } else {
        pending_events_.push_back({value, event});
      }
    }

    if (signal_now)
      SetEvent(event);

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {
    std::vector<HANDLE> events_to_signal;
    std::vector<std::function<void()>> callbacks_to_run;
    {
      std::lock_guard lock(mutex_);
      completed_value_ = value;
      has_manual_completed_value_ = true;
      collectCompletedEventsLocked(events_to_signal, callbacks_to_run);
    }
    event_.signalValue(value);
    for (HANDLE event : events_to_signal)
      SetEvent(event);
    for (auto &callback : callbacks_to_run)
      callback();
    return S_OK;
  }

  WMT::Reference<WMT::SharedEvent> GetSharedEvent() const override { return event_; }

  void AddRefPrivate() override {
    ComObjectWithInitialRef<ID3D12Fence>::AddRefPrivate();
  }

  void ReleasePrivate() override {
    ComObjectWithInitialRef<ID3D12Fence>::ReleasePrivate();
  }

  void SetCompletedValue(UINT64 value) override {
    std::vector<HANDLE> events_to_signal;
    std::vector<std::function<void()>> callbacks_to_run;
    {
      std::lock_guard lock(mutex_);
      completed_value_ = value;
      has_manual_completed_value_ = true;
      collectCompletedEventsLocked(events_to_signal, callbacks_to_run);
    }
    event_.signalValue(value);
    for (HANDLE event : events_to_signal)
      SetEvent(event);
    RunCompletionCallbacksAsync(std::move(callbacks_to_run));
  }

  void SignalFromQueue(UINT64 value) override {
    Signal(value);
  }

  void AddCompletionCallback(UINT64 value, std::function<void()> callback) override {
    bool run_now = false;
    {
      std::lock_guard lock(mutex_);
      if (GetCompletedValueLocked() >= value) {
        run_now = true;
      } else {
        pending_callbacks_.push_back({value, std::move(callback)});
      }
    }

    if (run_now) {
      callback();
      return;
    }
  }

  bool HasReached(UINT64 value) const override {
    return MTLSharedEvent_signaledValue(event_.handle) >= value;
  }

private:
  struct PendingEvent {
    UINT64 value;
    HANDLE event;
  };

  struct PendingCallback {
    UINT64 value;
    std::function<void()> callback;
  };

  UINT64 GetCompletedValueLocked() const {
    if (has_manual_completed_value_)
      return completed_value_;
    return MTLSharedEvent_signaledValue(event_.handle);
  }

  void collectCompletedEventsLocked(
      std::vector<HANDLE> &events,
      std::vector<std::function<void()>> &callbacks) {
    const UINT64 completed_value = GetCompletedValueLocked();
    auto it = std::remove_if(pending_events_.begin(), pending_events_.end(),
                             [&](const PendingEvent &pending) {
                               if (completed_value < pending.value)
                                 return false;
                               events.push_back(pending.event);
                               return true;
                             });
    pending_events_.erase(it, pending_events_.end());
    auto callback_it = std::remove_if(
        pending_callbacks_.begin(), pending_callbacks_.end(),
        [&](PendingCallback &pending) {
          if (completed_value < pending.value)
            return false;
          callbacks.push_back(std::move(pending.callback));
          return true;
        });
    pending_callbacks_.erase(callback_it, pending_callbacks_.end());
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  WMT::Reference<WMT::SharedEvent> event_;
  D3D12_FENCE_FLAGS flags_;
  mutable std::mutex mutex_;
  std::vector<PendingEvent> pending_events_;
  std::vector<PendingCallback> pending_callbacks_;
  UINT64 completed_value_;
  bool has_manual_completed_value_;
  std::string name_;
};

} // namespace

Com<ID3D12Fence>
CreateFence(IMTLD3D12Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags) {
  return Com<ID3D12Fence>::transfer(new FenceImpl(device, initial_value, flags));
}

} // namespace dxmt::d3d12
