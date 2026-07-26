#pragma once

#include "d3d12_device.hpp"
#include "Metal.hpp"
#include <cstdint>
#include <memory>
#include <d3d12.h>

namespace dxmt::d3d12 {

struct FenceGpuSignal {
  WMT::Reference<WMT::SharedEvent> event;
  UINT64 signal_value = 0;
  uintptr_t queue = 0;
  D3D12_COMMAND_LIST_TYPE queue_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  uint64_t dxmt_chunk = 0;
  uint64_t chunk_event = 0;
  uint64_t frame = 0;
};

enum class FenceGpuWaitStatus {
  Resolved,
  External,
  Shared,
  CpuSignal,
  Unknown,
  Rewind,
};

class FenceWaitTarget {
public:
  FenceWaitTarget() = default;
  FenceWaitTarget(const FenceWaitTarget &) = delete;
  FenceWaitTarget &operator=(const FenceWaitTarget &) = delete;
  virtual ~FenceWaitTarget() noexcept = default;
  virtual void CompleteFenceWait() noexcept = 0;
};

class Fence {
public:
  virtual ~Fence() = default;

  virtual IMTLD3D12Device *GetParentDevice() const = 0;
  virtual WMT::Reference<WMT::SharedEvent> GetSharedEvent() const = 0;
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
  virtual UINT64 GetCompletedValue() const = 0;
  virtual void SetCompletedValue(UINT64 value) = 0;
  virtual void SignalFromQueue(UINT64 value) = 0;
  virtual void RegisterQueueWait(
      UINT64 value, std::weak_ptr<FenceWaitTarget> target) = 0;
  virtual bool HasReached(UINT64 value) const = 0;
  virtual void RegisterQueueSignal(const FenceGpuSignal &signal) = 0;
  virtual FenceGpuWaitStatus TryResolveGpuWait(UINT64 value, FenceGpuSignal &signal) const = 0;
};

Com<ID3D12Fence>
CreateFence(IMTLD3D12Device *device, UINT64 initial_value, D3D12_FENCE_FLAGS flags);

} // namespace dxmt::d3d12
