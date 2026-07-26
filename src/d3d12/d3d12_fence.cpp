#include "d3d12_fence.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_noexcept.hpp"
#include "util_string.hpp"
#include "util_win32_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

namespace dxmt::d3d12 {
namespace {

static bool
D3D12FenceDiagEnabled() {
  static const bool enabled = []() {
    auto value = env::getEnvVar("DXMT_DIAG_D3D12_FENCE");
    if (value.empty())
      value = env::getEnvVar("DXMT_DIAG_COMMAND_QUEUE");
    return value == "1" || value == "true" || value == "yes" ||
           value == "trace";
  }();
  return enabled;
}

static bool
D3D12FenceDiagShouldLog(std::atomic<uint32_t> &counter) {
  if (!D3D12FenceDiagEnabled())
    return false;
  counter.fetch_add(1, std::memory_order_relaxed);
  return true;
}

using D3D12FenceDiagClock = std::chrono::high_resolution_clock;

static double
D3D12FenceDiagDurationMs(D3D12FenceDiagClock::duration duration) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

struct FencePendingEvent {
  UINT64 value;
  HANDLE event;
  D3D12FenceDiagClock::time_point registered_time;
};

struct FencePendingQueueWait {
  UINT64 value;
  std::weak_ptr<FenceWaitTarget> target;
  D3D12FenceDiagClock::time_point registered_time;
};

struct FenceCompletionState final : DeviceErrorTarget {
  mutable std::mutex mutex;
  std::vector<FencePendingEvent> pending_events;
  std::vector<FencePendingQueueWait> pending_queue_waits;
  bool device_removed = false;

  void CompleteDeviceError() noexcept override;
};

static void
CompleteFenceForDeviceRemoval(
    FenceCompletionState &state) {
  std::vector<FencePendingEvent> events_to_signal;
  std::vector<std::shared_ptr<FenceWaitTarget>> queue_waits_to_wake;
  {
    std::lock_guard lock(state.mutex);
    if (state.device_removed)
      return;
    state.device_removed = true;
    events_to_signal.swap(state.pending_events);
    for (auto &pending : state.pending_queue_waits) {
      if (auto target = pending.target.lock())
        queue_waits_to_wake.push_back(std::move(target));
    }
    state.pending_queue_waits.clear();
  }
  for (const auto &pending : events_to_signal)
    SetEvent(pending.event);
  for (const auto &target : queue_waits_to_wake)
    target->CompleteFenceWait();
}

void
FenceCompletionState::CompleteDeviceError() noexcept {
  if (!dxmt::invokeNoexcept(
          "fence device error completion",
          [this]() { CompleteFenceForDeviceRemoval(*this); }))
    std::terminate();
}

#ifdef __ID3D12Fence1_INTERFACE_DEFINED__
using FenceComInterface = ID3D12Fence1;
#else
using FenceComInterface = ID3D12Fence;
#endif

class FenceImpl final : public ComObjectWithInitialRef<FenceComInterface>,
                        public Fence {
public:
  FenceImpl(IMTLD3D12Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags)
      : device_(device), event_(device->GetMTLDevice().newSharedEvent()), flags_(flags),
        completed_value_(initial_value), has_manual_completed_value_(false),
        last_signal_was_cpu_(false) {
    event_.signalValue(initial_value);
    device_error_target_id_ =
        device_->GetDXMTDevice().queue().RegisterDeviceErrorTarget(
            completion_state_);
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12FenceDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: CreateFence"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " initial=", initial_value,
           " flags=", flags,
           " sharedEvent=", static_cast<uintptr_t>(event_.handle));
    }
  }

  ~FenceImpl() noexcept override {
    dxmt::invokeNoexcept("fence error target unregister", [this]() {
      device_->GetDXMTDevice().queue().UnregisterDeviceErrorTarget(
          device_error_target_id_);
    });
    std::vector<FencePendingEvent> events_to_signal;
    std::vector<FencePendingQueueWait> queue_waits_to_wake;
    {
      std::lock_guard lock(completion_state_->mutex);
      completed_value_ = UINT64_MAX;
      has_manual_completed_value_ = true;
      events_to_signal.swap(completion_state_->pending_events);
      queue_waits_to_wake.swap(completion_state_->pending_queue_waits);
    }
    for (const auto &pending : events_to_signal)
      SetEvent(pending.event);
    for (const auto &pending : queue_waits_to_wake) {
      if (auto target = pending.target.lock())
        target->CompleteFenceWait();
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12Fence) ||
        riid == __uuidof(FenceComInterface)) {
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
    std::lock_guard lock(completion_state_->mutex);
    const UINT64 value = GetCompletedValueLocked();
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12FenceDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: GetCompletedValue"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " value=", value,
           " manual=", has_manual_completed_value_,
           " pendingEvents=", completion_state_->pending_events.size(),
           " pendingWaits=", completion_state_->pending_queue_waits.size());
    }
    return value;
  }

  UINT64 GetCompletedValue() const override {
    std::lock_guard lock(completion_state_->mutex);
    const UINT64 value = GetCompletedValueLocked();
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12FenceDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: GetCompletedValue internal"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " value=", value,
           " manual=", has_manual_completed_value_,
           " pendingEvents=", completion_state_->pending_events.size(),
           " pendingWaits=", completion_state_->pending_queue_waits.size());
    }
    return value;
  }

  HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64 value, HANDLE event) override {
    const auto register_time = D3D12FenceDiagClock::now();
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12FenceDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: SetEventOnCompletion enter"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " target=", value,
           " event=", reinterpret_cast<uintptr_t>(event));
    }
    if (!event) {
      const auto wait_begin_time = D3D12FenceDiagClock::now();
      while (true) {
        {
          std::lock_guard lock(completion_state_->mutex);
          if (GetCompletedValueLocked() >= value) {
            const auto wait_end_time = D3D12FenceDiagClock::now();
            static std::atomic<uint32_t> wait_log_count = 0;
            if (D3D12FenceDiagShouldLog(wait_log_count)) {
              WARN_FILE_ONLY("D3D12 fence diagnostic: SetEventOnCompletion sync wait"
                   " fence=", reinterpret_cast<uintptr_t>(this),
                   " target=", value,
                   " waitMs=", D3D12FenceDiagDurationMs(wait_end_time - wait_begin_time));
            }
            return S_OK;
          }
        }
        dxmt::this_thread::yield();
      }
    }

    bool signal_now = false;
    {
      std::lock_guard lock(completion_state_->mutex);
      if (GetCompletedValueLocked() >= value) {
        signal_now = true;
      } else {
        completion_state_->pending_events.push_back(
            {value, event, register_time});
      }
    }

    if (signal_now)
      SetEvent(event);

    static std::atomic<uint32_t> result_log_count = 0;
    if (D3D12FenceDiagShouldLog(result_log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: SetEventOnCompletion result"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " target=", value,
           " signalNow=", signal_now,
           " elapsedMs=", D3D12FenceDiagDurationMs(D3D12FenceDiagClock::now() - register_time));
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override {
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12FenceDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: Signal CPU"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " value=", value);
    }
    std::vector<HANDLE> events_to_signal;
    std::vector<std::shared_ptr<FenceWaitTarget>> queue_waits_to_wake;
    {
      std::lock_guard lock(completion_state_->mutex);
      completed_value_ = value;
      has_manual_completed_value_ = true;
      last_signal_was_cpu_ = true;
      pending_gpu_signals_.clear();
      collectCompletedEventsLocked(events_to_signal, queue_waits_to_wake);
    }
    event_.signalValue(value);
    for (HANDLE event : events_to_signal)
      SetEvent(event);
    for (const auto &target : queue_waits_to_wake)
      target->CompleteFenceWait();
    return S_OK;
  }

#ifdef __ID3D12Fence1_INTERFACE_DEFINED__
  D3D12_FENCE_FLAGS STDMETHODCALLTYPE GetCreationFlags() override {
    return flags_;
  }
#endif

  IMTLD3D12Device *GetParentDevice() const override {
    return device_.ptr();
  }

  WMT::Reference<WMT::SharedEvent> GetSharedEvent() const override { return event_; }

  void AddRefPrivate() override {
    ComObjectWithInitialRef<FenceComInterface>::AddRefPrivate();
  }

  void ReleasePrivate() override {
    ComObjectWithInitialRef<FenceComInterface>::ReleasePrivate();
  }

  void SetCompletedValue(UINT64 value) override {
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12FenceDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: SetCompletedValue queue-complete"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " value=", value);
    }
    std::vector<HANDLE> events_to_signal;
    std::vector<std::shared_ptr<FenceWaitTarget>> queue_waits_to_wake;
    {
      std::lock_guard lock(completion_state_->mutex);
      completed_value_ = value;
      has_manual_completed_value_ = true;
      last_signal_was_cpu_ = false;
      pruneGpuSignalsLocked(value);
      collectCompletedEventsLocked(events_to_signal, queue_waits_to_wake);
    }
    event_.signalValue(value);
    for (HANDLE event : events_to_signal)
      SetEvent(event);
    for (const auto &target : queue_waits_to_wake)
      target->CompleteFenceWait();
  }

  void SignalFromQueue(UINT64 value) override {
    static std::atomic<uint32_t> log_count = 0;
    if (D3D12FenceDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: SignalFromQueue"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " value=", value);
    }
    Signal(value);
  }

  void RegisterQueueWait(
      UINT64 value,
      std::weak_ptr<FenceWaitTarget> target) override {
    const auto register_time = D3D12FenceDiagClock::now();
    {
      std::lock_guard lock(completion_state_->mutex);
      if (GetCompletedValueLocked() < value) {
        completion_state_->pending_queue_waits.push_back(
            {value, std::move(target), register_time});
        return;
      }
    }
    if (auto ready = target.lock())
      ready->CompleteFenceWait();
  }

  bool HasReached(UINT64 value) const override {
    return device_->GetDXMTDevice().queue().HasDeviceError() ||
           MTLSharedEvent_signaledValue(event_.handle) >= value;
  }

  void RegisterQueueSignal(const FenceGpuSignal &signal) override {
    std::lock_guard lock(completion_state_->mutex);
    if (flags_ & D3D12_FENCE_FLAG_SHARED)
      return;

    const UINT64 completed_value = GetCompletedValueLocked();
    pruneGpuSignalsLocked(completed_value);
    if (completed_value >= signal.signal_value)
      return;
    last_signal_was_cpu_ = false;

    auto it = std::lower_bound(
        pending_gpu_signals_.begin(), pending_gpu_signals_.end(),
        signal.signal_value,
        [](const FenceGpuSignal &pending, UINT64 value) {
          return pending.signal_value < value;
        });
    if (it != pending_gpu_signals_.end() &&
        it->signal_value == signal.signal_value) {
      *it = signal;
    } else {
      pending_gpu_signals_.insert(it, signal);
    }

    static std::atomic<uint32_t> log_count = 0;
    if (D3D12FenceDiagShouldLog(log_count)) {
      WARN_FILE_ONLY("D3D12 fence diagnostic: RegisterQueueSignal"
           " fence=", reinterpret_cast<uintptr_t>(this),
           " value=", signal.signal_value,
           " queue=", signal.queue,
           " queueType=", signal.queue_type,
           " dxmtChunk=", signal.dxmt_chunk,
           " chunkEvent=", signal.chunk_event,
           " frame=", signal.frame,
           " pendingGpuSignals=", pending_gpu_signals_.size());
    }
  }

  FenceGpuWaitStatus TryResolveGpuWait(UINT64 value, FenceGpuSignal &signal) const override {
    std::lock_guard lock(completion_state_->mutex);
    if (GetCompletedValueLocked() >= value)
      return FenceGpuWaitStatus::Resolved;
    if (last_signal_was_cpu_)
      return FenceGpuWaitStatus::CpuSignal;
    if (flags_ & D3D12_FENCE_FLAG_SHARED)
      return FenceGpuWaitStatus::Shared;

    auto it = std::lower_bound(
        pending_gpu_signals_.begin(), pending_gpu_signals_.end(), value,
        [](const FenceGpuSignal &pending, UINT64 wait_value) {
          return pending.signal_value < wait_value;
        });
    if (it == pending_gpu_signals_.end())
      return pending_gpu_signals_.empty() ? FenceGpuWaitStatus::Unknown
                                          : FenceGpuWaitStatus::Rewind;

    signal = *it;
    return FenceGpuWaitStatus::Resolved;
  }

private:
  UINT64 GetCompletedValueLocked() const {
    if (completion_state_->device_removed ||
        device_->GetDXMTDevice().queue().HasDeviceError())
      return UINT64_MAX;
    if (has_manual_completed_value_)
      return completed_value_;
    return MTLSharedEvent_signaledValue(event_.handle);
  }

  void collectCompletedEventsLocked(
      std::vector<HANDLE> &events,
      std::vector<std::shared_ptr<FenceWaitTarget>> &queue_waits) {
    const UINT64 completed_value = GetCompletedValueLocked();
    const auto completed_time = D3D12FenceDiagClock::now();
    auto it = std::remove_if(completion_state_->pending_events.begin(),
                             completion_state_->pending_events.end(),
                             [&](const FencePendingEvent &pending) {
                               if (completed_value < pending.value)
                                 return false;
                               events.push_back(pending.event);
                               static std::atomic<uint32_t> event_log_count = 0;
                               if (D3D12FenceDiagShouldLog(event_log_count)) {
                                 WARN_FILE_ONLY("D3D12 fence diagnostic: CompletePendingEvent"
                                      " fence=", reinterpret_cast<uintptr_t>(this),
                                      " target=", pending.value,
                                      " completed=", completed_value,
                                      " ageMs=", D3D12FenceDiagDurationMs(completed_time - pending.registered_time));
                               }
                               return true;
                             });
    completion_state_->pending_events.erase(
        it, completion_state_->pending_events.end());
    auto queue_wait_it = std::remove_if(
        completion_state_->pending_queue_waits.begin(),
        completion_state_->pending_queue_waits.end(),
        [&](FencePendingQueueWait &pending) {
          if (completed_value < pending.value)
            return pending.target.expired();
          if (auto target = pending.target.lock())
            queue_waits.push_back(std::move(target));
          return true;
        });
    completion_state_->pending_queue_waits.erase(
        queue_wait_it, completion_state_->pending_queue_waits.end());
  }

  void pruneGpuSignalsLocked(UINT64 completed_value) {
    auto it = std::remove_if(
        pending_gpu_signals_.begin(), pending_gpu_signals_.end(),
        [completed_value](const FenceGpuSignal &signal) {
          return signal.signal_value <= completed_value;
        });
    pending_gpu_signals_.erase(it, pending_gpu_signals_.end());
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  WMT::Reference<WMT::SharedEvent> event_;
  D3D12_FENCE_FLAGS flags_;
  std::shared_ptr<FenceCompletionState> completion_state_ =
      std::make_shared<FenceCompletionState>();
  uint64_t device_error_target_id_ = 0;
  std::vector<FenceGpuSignal> pending_gpu_signals_;
  UINT64 completed_value_;
  bool has_manual_completed_value_;
  bool last_signal_was_cpu_;
  std::string name_;
};

} // namespace

Com<ID3D12Fence>
CreateFence(IMTLD3D12Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags) {
  return Com<ID3D12Fence>::transfer(new FenceImpl(device, initial_value, flags));
}

} // namespace dxmt::d3d12
