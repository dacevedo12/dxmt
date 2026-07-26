#include "dxmt_allocation.hpp"
#include "dxmt_command_queue.hpp"
#include "util_likely.hpp"
#include <iterator>
#include <new>

namespace dxmt {

void
Allocation::incRef() {
  refcount_.fetch_add(1u, std::memory_order_relaxed);
};

void
Allocation::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
    delete this;
};

void
Allocation::ensureLifetimeResidency(CommandQueue &queue,
                                    WMT::Object allocation,
                                    ResidencyProvenance provenance) {
  if (!allocation)
    return;
  std::lock_guard lock(lifetime_residency_mutex_);
  if (!lifetime_residency_registration_)
    lifetime_residency_registration_ =
        queue.RegisterLifetimeResidency(
            ResidencyOwnership::Lifetime(allocation, provenance));
}

bool
Allocation::hasLifetimeResidency() const {
  std::lock_guard lock(lifetime_residency_mutex_);
  return bool(lifetime_residency_registration_);
}

AllocationRefTracking::AllocationRefTracking() {
  chunk_placed.next_chunk = nullptr;
  chunk_placed.size = 0;
  chunk_placed.capacity = std::size(chunk_placed.allocations);
  chunk_last = reinterpret_cast<RefAddChunk<> *>(&chunk_placed);
}

AllocationRefTracking::~AllocationRefTracking() {
  clear();
}

bool
AllocationRefTracking::track(Allocation *allocation) {
  if (unlikely(chunk_last->capacity <= chunk_last->size))
    return false;
  dxmt::Allocation **list = std::launder(chunk_last->allocations);
  allocation->incRef();
  list[chunk_last->size++] = allocation;
  return true;
};

void
AllocationRefTracking::addStorage(void *ptr, size_t length) {
  constexpr size_t header_size = offsetof(RefAddChunk<>, allocations);
  if (!ptr || reinterpret_cast<uintptr_t>(ptr) % alignof(RefAddChunk<>) ||
      length < header_size + sizeof(Allocation *))
    return;

  RefAddChunk<> *new_chunk = std::launder(reinterpret_cast<RefAddChunk<> *>(ptr));
  new_chunk->next_chunk = nullptr;
  new_chunk->size = 0;
  new_chunk->capacity = (length - header_size) / sizeof(Allocation *);
  chunk_last->next_chunk = new_chunk;
  chunk_last = new_chunk;
}

void
AllocationRefTracking::transferTo(std::vector<Allocation *> &allocations) {
  RefAddChunk<> *chunk = reinterpret_cast<RefAddChunk<> *>(&chunk_placed);
  while (chunk) {
    dxmt::Allocation **list = std::launder(chunk->allocations);
    allocations.insert(allocations.end(), list, list + chunk->size);
    chunk->size = 0;
    chunk = chunk->next_chunk;
  }

  chunk_last = reinterpret_cast<RefAddChunk<> *>(&chunk_placed);
  chunk_placed.next_chunk = nullptr;
}

void
AllocationRefTracking::clear() {
  RefAddChunk<> *chunk = reinterpret_cast<RefAddChunk<> *>(&chunk_placed);
  while (chunk) {
    dxmt::Allocation **list = std::launder(chunk->allocations);
    for (unsigned i = 0; i < chunk->size; i++)
      list[i]->decRef();
    chunk->size = 0;
    chunk = chunk->next_chunk;
  }
  // reset state
  chunk_last = reinterpret_cast<RefAddChunk<> *>(&chunk_placed);
  chunk_placed.next_chunk = nullptr;
};

} // namespace dxmt
