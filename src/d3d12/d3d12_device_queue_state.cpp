#include "d3d12_device_queue_state.hpp"

#include <mutex>

namespace dxmt::d3d12 {
namespace {

class D3D12DeviceQueueStateRegistry final {
public:
  [[nodiscard]] std::shared_ptr<D3D12DeviceQueueState>
  Acquire(IMTLD3D12Device &device) {
    std::lock_guard lock(mutex_);
    for (auto entry = states_.begin(); entry != states_.end();) {
      if (entry->second.expired())
        entry = states_.erase(entry);
      else
        ++entry;
    }

    auto &weak = states_[&device];
    auto state = weak.lock();
    if (!state) {
      state = std::make_shared<D3D12DeviceQueueState>();
      weak = state;
    }
    return state;
  }

private:
  std::mutex mutex_;
  std::unordered_map<IMTLD3D12Device *,
                     std::weak_ptr<D3D12DeviceQueueState>>
      states_;
};

D3D12DeviceQueueStateRegistry &DeviceQueueStateRegistry() {
  static D3D12DeviceQueueStateRegistry registry;
  return registry;
}

} // namespace

D3D12DeviceQueueState::D3D12DeviceQueueState()
    : submission_service_(std::make_shared<DeviceSubmissionService>()) {}

std::shared_ptr<D3D12DeviceQueueState>
AcquireD3D12DeviceQueueState(IMTLD3D12Device &device) {
  return DeviceQueueStateRegistry().Acquire(device);
}

} // namespace dxmt::d3d12
