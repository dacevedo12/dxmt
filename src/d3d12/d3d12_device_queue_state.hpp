#pragma once

#include "d3d12_submission_service.hpp"
#include <d3d12.h>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dxmt::d3d12 {

struct ReplaySubresourceState {
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES pending_before = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES pending_after = D3D12_RESOURCE_STATE_COMMON;
  D3D12_RESOURCE_STATES mismatch_barrier_synced_state =
      D3D12_RESOURCE_STATE_COMMON;
  bool implicitly_promoted = false;
  bool has_pending_split = false;
};

struct ReplayResourceStateEntry {
  D3D12_RESOURCE_DESC desc = {};
  D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
  UINT64 heap_offset = 0;
  std::vector<ReplaySubresourceState> subresources;
};

using ReplayResourceStateMap =
    std::unordered_map<ID3D12Resource *, ReplayResourceStateEntry>;

class D3D12DeviceQueueState final {
public:
  D3D12DeviceQueueState();
  D3D12DeviceQueueState(const D3D12DeviceQueueState &) = delete;
  D3D12DeviceQueueState &operator=(const D3D12DeviceQueueState &) = delete;
  ~D3D12DeviceQueueState() noexcept = default;

  [[nodiscard]] const std::shared_ptr<DeviceSubmissionService> &
  SubmissionService() const noexcept {
    return submission_service_;
  }

  [[nodiscard]] ReplayResourceStateMap &BackendResourceStates() noexcept {
    return resources_;
  }

  void EraseResource(ID3D12Resource *resource) noexcept {
    resources_.erase(resource);
  }

private:
  ReplayResourceStateMap resources_;
  std::shared_ptr<DeviceSubmissionService> submission_service_;
};

[[nodiscard]] std::shared_ptr<D3D12DeviceQueueState>
AcquireD3D12DeviceQueueState(IMTLD3D12Device &device);

} // namespace dxmt::d3d12
