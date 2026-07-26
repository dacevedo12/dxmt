#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace WMT {
class Object;
}

namespace dxmt {

class CommandQueue;
class LifetimeResidencyRegistration;

enum class ResidencyProvenanceKind : uint8_t {
  Unknown,
  CommittedResource,
  PlacedResourceChild,
  ReservedResource,
  HeapBacking,
  PlacementHeap,
  DescriptorHeap,
  InternalResource,
  ReplayAllocator,
  ReplayTemporary,
};

/*
 * Diagnostic-only allocation provenance. Every field is a copied value because
 * a deferred residency mutation may outlive the originating D3D object.
 */
struct ResidencyProvenance {
  ResidencyProvenanceKind kind = ResidencyProvenanceKind::Unknown;
  uint64_t owner = 0;
  uint64_t identity = 0;
  uint64_t parent = 0;
  uint64_t heap_offset = 0;
  uint64_t size = 0;
  uint32_t dimension = 0;
  uint32_t component = 0;
};

class Allocation {
public:
  virtual ~Allocation(){};

  void incRef();
  void decRef();

  bool
  checkRetained(uint64_t seq_id) {
    return last_retained_seq_id.exchange(seq_id, std::memory_order_relaxed) ==
           seq_id;
  }

  void ensureLifetimeResidency(
      CommandQueue &queue, WMT::Object allocation,
      ResidencyProvenance provenance = {});

  bool hasLifetimeResidency() const;

private:
  std::atomic<uint32_t> refcount_ = {0u};
  std::atomic<uint64_t> last_retained_seq_id = {0};
  mutable std::mutex lifetime_residency_mutex_;
  std::shared_ptr<LifetimeResidencyRegistration>
      lifetime_residency_registration_;
};

class AllocationRefTracking {
public:
  AllocationRefTracking();
  ~AllocationRefTracking();

  AllocationRefTracking(const AllocationRefTracking &) = delete;
  AllocationRefTracking &operator=(const AllocationRefTracking &) = delete;

  bool track(Allocation *allocation);

  void addStorage(void *ptr, size_t length);

  void transferTo(std::vector<Allocation *> &allocations);

  void clear();

private:
  template <size_t Size = 1> struct RefAddChunk {
    RefAddChunk *next_chunk;
    size_t capacity;
    size_t size;
    Allocation *allocations[Size];
  };
  RefAddChunk<29> chunk_placed;
  RefAddChunk<> *chunk_last = nullptr;
};

} // namespace dxmt
