#pragma once

// Payload value types carried by the command queue's deferred-work and
// retirement pipelines. They used to be nested inside CommandQueueImpl, which
// made them unnameable outside that class body -- and therefore pinned every
// piece of queue-work / tile-mapping / debug-dump logic into the same
// translation unit. Hoisting them to namespace scope is what lets those
// fragments become independent TUs.

#include "d3d12_command_allocator.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_replay_diagnostics.hpp"
#include "d3d12_swapchain.hpp"
#include "d3d12_tile_mapping.hpp"

#include <atomic>
#include <cstdint>
#include <d3d12.h>
#include <memory>
#include <variant>
#include <vector>

namespace dxmt::d3d12 {

struct SparseMappingGroup final {
  WMT::Texture texture;
  WMT::Heap placement_heap;
  std::vector<WMTSparseTextureMappingOperation> operations;
  Com<ID3D12Resource> resource;
  Com<ID3D12Heap> heap;
  Rc<Texture> barrier_texture;
  std::vector<SparseTileBarrierSubresource> barrier_subresources;
};

struct SparseUpdateQueueWork final {
  SparseMappingGroup group;
  bool has_map = false;
  uint64_t resource_identity = 0;
  uint64_t gpu_resource_id = 0;
  uint64_t mapping_generation = 0;
  uint32_t operation_count = 0;
  uint32_t map_count = 0;
  uint32_t unmap_count = 0;
};

struct SparseCopyQueueWork final {
  std::vector<SparseMappingGroup> groups;
};

struct SwapChainResizeResult final {
  std::atomic_bool ready{false};
};

struct SwapChainResizeQueueWork final {
  std::vector<Com<ID3D12Resource>> backbuffers;
  std::shared_ptr<SwapChainResizeResult> result;
};

// Coarse discriminator for a PendingOperation, of which QueueWorkPayload below
// is one arm. It sits here rather than in the queue class so the pure mapping
// from packet to name/kind can be compiled outside the queue TU.
enum class PendingOperationType {
  Execute,
  QueueWork,
  Signal,
  Wait,
  Stop,
};

using QueueWorkPayload =
    std::variant<std::monostate, SparseUpdateQueueWork, SparseCopyQueueWork,
                 D3D12PresentSubmission, SwapChainResizeQueueWork>;

/**
 * Owns one private reference on a Fence for as long as the queue work that
 * needs the fence alive is in flight. Move-only; a moved-from instance holds
 * nullptr and releases nothing.
 */
class FencePrivateReference final {
public:
  explicit FencePrivateReference(Fence *fence) noexcept
      : fence_(fence) {
    if (fence_)
      fence_->AddRefPrivate();
  }

  FencePrivateReference(const FencePrivateReference &) = delete;
  FencePrivateReference &
  operator=(const FencePrivateReference &) = delete;

  FencePrivateReference(FencePrivateReference &&other) noexcept
      : fence_(other.fence_) {
    other.fence_ = nullptr;
  }

  FencePrivateReference &
  operator=(FencePrivateReference &&other) noexcept {
    if (this == &other)
      return *this;
    Reset();
    fence_ = other.fence_;
    other.fence_ = nullptr;
    return *this;
  }

  ~FencePrivateReference() noexcept {
    Reset();
  }

  [[nodiscard]] Fence *get() const noexcept {
    return fence_;
  }

private:
  void Reset() noexcept {
    if (!fence_)
      return;
    auto *fence = fence_;
    fence_ = nullptr;
    fence->ReleasePrivate();
  }

  Fence *fence_ = nullptr;
};

struct AllocatorRetirementWork final {
  std::vector<SubmittedCommandAllocatorUse> uses;
  uintptr_t queue_identity = 0;
  D3D12_COMMAND_LIST_TYPE queue_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  uint64_t lifecycle_pair_id = 0;
  uint64_t queue_lifecycle_id = 0;
  uint64_t frame = 0;
  uint64_t chunk = 0;
};

struct FenceRetirementWork final {
  FencePrivateReference fence;
  UINT64 value = 0;
  uintptr_t queue_identity = 0;
  D3D12_COMMAND_LIST_TYPE queue_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  uint64_t chunk = 0;
  uint64_t chunk_event = 0;
  uint64_t frame = 0;
  uint64_t lifecycle_pair_id = 0;
  uint64_t queue_lifecycle_id = 0;
};

struct PresentRetirementWork final {
  std::shared_ptr<Presenter::PresentState> state;
  std::shared_ptr<PresentSemaphoreSignals> signals;
};

using RetirementPayload =
    std::variant<AllocatorRetirementWork, FenceRetirementWork,
                 PresentRetirementWork, DrawVisibilityRetirementWork,
                 IndexReadbackRetirementWork,
                 VertexReadbackRetirementWork,
                 ConstantBufferReadbackRetirementWork>;

} // namespace dxmt::d3d12
