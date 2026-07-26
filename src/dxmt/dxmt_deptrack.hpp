#pragma once
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "util_bit.hpp"
#include "util_svector.hpp"

namespace dxmt {

namespace ResourceAccess {
constexpr int Read = 1 << 0;
constexpr int Write = 1 << 1;
constexpr int ReadWrite = Read | Write;
constexpr int UAV = 1 << 2;
constexpr int All = ReadWrite | UAV;
}; // namespace ResourceAccess

constexpr auto kLog2Lane = 6ull;
constexpr auto kLane = 1 << kLog2Lane;
constexpr auto kLaneMask = kLane - 1;
constexpr auto kAllLaneMask = ~0ull >> (64 /* uint64_t */ - kLane);
constexpr auto kParity = 4; // can also use 3, although power of 2 is nice
constexpr auto kParityLane = kParity * kLane;

static_assert(kLog2Lane <= 6);
static_assert(kLane > 1);

using LaneStorage = uint64_t;
using EncoderId = uint64_t;

constexpr auto
PARITY(EncoderId id) {
  return (id >> kLog2Lane) % kParity;
}

constexpr auto
LANE(EncoderId id) {
  return id & kLaneMask;
}

class FenceSet {
public:
  FenceSet() = default;

  FenceSet(EncoderId id) {
    set(id);
  }

  bool
  set(EncoderId id) {
    auto entry = std::lower_bound(entries_.begin(), entries_.end(), id);
    if (entry != entries_.end() && *entry == id)
      return false;
    entries_.insert(entry, id);
    return true;
  }

  void
  unset(EncoderId id) {
    auto entry = std::lower_bound(entries_.begin(), entries_.end(), id);
    if (entry != entries_.end() && *entry == id)
      entries_.erase(entry);
  }

  bool
  test(EncoderId id) const {
    return std::binary_search(entries_.begin(), entries_.end(), id);
  }

  bool
  testAndSet(EncoderId id) {
    return !set(id);
  }

  bool
  add(EncoderId id) {
    return set(id);
  }

  bool
  isLastAccess(EncoderId id) const {
    return test(id);
  }

  template <typename Fn>
  size_t
  enumerate(EncoderId id_before, Fn &&fn) {
    size_t count = 0;
    for (auto id : entries_) {
      if (id >= id_before)
        continue;
      fn(id);
      count++;
    }
    return count;
  }

  bool
  intersectedWith(const FenceSet &set) const {
    for (auto id : entries_) {
      if (set.test(id))
        return true;
    }
    return false;
  }

  bool
  contains(const FenceSet &set) const {
    for (auto id : set.entries_) {
      if (!test(id))
        return false;
    }
    return true;
  }

  FenceSet &
  merge(const FenceSet &set) {
    for (auto id : set.entries_)
      this->set(id);
    return *this;
  }

  FenceSet
  unionOf(const FenceSet &set) const {
    FenceSet ret(*this);
    ret.merge(set);
    return ret;
  }

  FenceSet &
  subtract(const FenceSet &set) {
    for (auto id : set.entries_)
      unset(id);
    return *this;
  }

  LaneStorage
  laneMask() const {
    LaneStorage ret = 0;
    for (auto id : entries_)
      ret |= 1ull << LANE(id);
    return ret;
  }

  LaneStorage
  storage(int parity) const {
    LaneStorage ret = 0;
    for (auto id : entries_) {
      if (PARITY(id) == static_cast<uint64_t>(parity))
        ret |= 1ull << LANE(id);
    }
    return ret;
  }

  uint32_t
  count() const {
    return static_cast<uint32_t>(entries_.size());
  }

  bool
  empty() const {
    return entries_.empty();
  }

  void
  clear() {
    entries_.clear();
  }

  template <typename Fn>
  void
  forEach(Fn &&fn) const {
    for (auto id : entries_)
      fn(id);
  }

  template <typename Fn, typename FnPrior>
  void
  forEach(const FenceSet &prior, FnPrior &&fnPrior, Fn &&fn) const {
    for (auto id : prior.entries_)
      fnPrior(id);
    for (auto id : entries_) {
      if (!prior.test(id))
        fn(id);
    }
  }

private:
  small_vector<EncoderId, 4> entries_;
};

struct RenderFenceMergePlan {
  FenceSet fragment_waits;
  FenceSet pre_raster_waits;
  FenceSet fragment_updates;
  FenceSet pre_raster_updates;
  bool mergeable = true;

  bool valid() const { return mergeable; }
};

inline RenderFenceMergePlan
BuildRenderResolveFenceMergePlan(
    const FenceSet &render_waits, const FenceSet &render_updates,
    const FenceSet &resolve_waits, const FenceSet &resolve_updates) {
  RenderFenceMergePlan plan;
  plan.fragment_waits = render_waits.unionOf(resolve_waits);
  plan.fragment_updates = render_updates.unionOf(resolve_updates);
  plan.fragment_waits.subtract(plan.fragment_updates);
  return plan;
}

inline RenderFenceMergePlan
BuildRenderFenceMergePlan(
    const FenceSet &latter_fragment_waits,
    const FenceSet &latter_pre_raster_waits,
    const FenceSet &latter_fragment_updates,
    const FenceSet &latter_pre_raster_updates,
    const FenceSet &former_fragment_waits,
    const FenceSet &former_pre_raster_waits,
    const FenceSet &former_fragment_updates,
    const FenceSet &former_pre_raster_updates) {
  RenderFenceMergePlan plan;

  const auto former_waits =
      former_fragment_waits.unionOf(former_pre_raster_waits);
  const auto latter_updates =
      latter_fragment_updates.unionOf(latter_pre_raster_updates);
  if (former_waits.intersectedWith(latter_updates)) {
    plan.mergeable = false;
    return plan;
  }

  plan.fragment_waits =
      former_fragment_waits.unionOf(latter_fragment_waits);
  plan.pre_raster_waits =
      former_pre_raster_waits.unionOf(latter_pre_raster_waits);
  plan.fragment_updates =
      former_fragment_updates.unionOf(latter_fragment_updates);
  plan.pre_raster_updates =
      former_pre_raster_updates.unionOf(latter_pre_raster_updates);

  // The encoder emits duplicate waits at pre-raster and duplicate updates at
  // fragment. Normalize the sets before classifying internal dependencies.
  plan.fragment_waits.subtract(plan.pre_raster_waits);
  plan.pre_raster_updates.subtract(plan.fragment_updates);

  const auto all_updates =
      plan.fragment_updates.unionOf(plan.pre_raster_updates);
  if (plan.pre_raster_waits.intersectedWith(all_updates) ||
      plan.fragment_waits.intersectedWith(plan.fragment_updates)) {
    plan.mergeable = false;
    return plan;
  }

  // A fragment wait on a pre-raster update is already ordered by the render
  // pass. Keeping the fence edge would turn it into a self-wait after merging.
  plan.fragment_waits.subtract(plan.pre_raster_updates);
  return plan;
}

/**
 * Maps exact command-buffer-local dependency IDs to Metal fence objects.
 *
 * A fence is only required for an update that has a later consumer. Metal 4
 * keeps every stage-domain fence unique for the lifetime of the command
 * buffer; completed generations return their fence objects to the pool.
 */
class CommandBufferFenceBindingTable {
public:
  void reset(uint32_t reusable_slot_count) {
    bindings_.clear();
    slot_last_use_.assign(reusable_slot_count, -1);
  }

  uint32_t bind(EncoderId id, uint32_t first, uint32_t last) {
    uint32_t slot = 0;
    while (slot < slot_last_use_.size() &&
           slot_last_use_[slot] >= static_cast<int64_t>(first))
      slot++;
    if (slot == slot_last_use_.size())
      slot_last_use_.push_back(-1);
    slot_last_use_[slot] = last;
    bindings_.insert_or_assign(id, slot);
    return slot;
  }

  uint32_t bindUnique(EncoderId id, uint32_t last) {
    const auto slot = static_cast<uint32_t>(bindings_.size());
    if (slot == slot_last_use_.size())
      slot_last_use_.push_back(-1);
    slot_last_use_[slot] = last;
    bindings_.insert_or_assign(id, slot);
    return slot;
  }

  std::optional<uint32_t> find(EncoderId id) const {
    const auto binding = bindings_.find(id);
    if (binding == bindings_.end())
      return std::nullopt;
    return binding->second;
  }

  uint32_t slotCount() const {
    return static_cast<uint32_t>(slot_last_use_.size());
  }

private:
  std::unordered_map<EncoderId, uint32_t> bindings_;
  std::vector<int64_t> slot_last_use_;
};

struct FenceDependencyOrderAnalysis {
  uint32_t prior_local_waits = 0;
  uint32_t future_local_waits = 0;
  uint32_t same_encoder_waits = 0;
  uint32_t external_waits = 0;
  uint32_t repeated_updates = 0;
};

class FenceDependencyOrderTracker {
public:
  void recordUpdates(const FenceSet &updates) {
    updates.forEach([&](EncoderId id) {
      analysis_.repeated_updates += !total_updates_.insert(id).second;
    });
  }

  void analyzeEncoder(const FenceSet &waits, const FenceSet &updates) {
    waits.forEach([&](EncoderId id) {
      if (seen_updates_.contains(id))
        analysis_.prior_local_waits++;
      else if (updates.test(id))
        analysis_.same_encoder_waits++;
      else if (total_updates_.contains(id))
        analysis_.future_local_waits++;
      else
        analysis_.external_waits++;
    });
    updates.forEach([&](EncoderId id) { seen_updates_.insert(id); });
  }

  const FenceDependencyOrderAnalysis &analysis() const {
    return analysis_;
  }

private:
  std::unordered_set<EncoderId> total_updates_;
  std::unordered_set<EncoderId> seen_updates_;
  FenceDependencyOrderAnalysis analysis_;
};

template <size_t Sz = kLane, size_t Forward = 1> class TrackingSet {
public:
  TrackingSet() {
    cursor = 0;
    clear();
  };

  bool
  add(EncoderId id) {
    assert(storage_[cursor] <= id);
    if (storage_[cursor] == id)
      return false;
    {
      cursor++;
      cursor = cursor % Sz;
    }
    storage_[cursor] = id;
    return true;
  };

  bool
  isLastAccess(EncoderId id) {
    return storage_[cursor] == id;
  }

  void
  clear() {
    storage_[cursor] = 0;
  };

  template <typename Fn>
  size_t
  enumerate(EncoderId id_before, Fn &&fn) {
    size_t count = 0;
    assert(id_before > Sz);
    for (size_t i = 0; i < Sz; i++) {
      auto c = storage_[(cursor + Sz - i) % Sz];
      if (c >= id_before) {
        assert(c - id_before <= Forward);
        continue;
      }
      if (c > (id_before - Sz)) {
        fn(c);
        count++;
        continue;
      }
      break;
    }
    return count;
  }

private:
  EncoderId storage_[Sz + Forward];
  uint32_t cursor;
};

constexpr auto kBarrierTypeRW = 1 << 0;
constexpr auto kBarrierTypeWaW = 1 << 1;

struct EncoderBarrierState {
  uint64_t barrierSet                       : 2 = 0;
  uint64_t barrierPreRasterSet              : 2 = 0;
  uint64_t barrierFragmentAfterPreRasterSet : 2 = 0;
  uint64_t barrierPreRasterAfterFragmentSet : 2 = 0;
  uint64_t reserved                         : 56;
};

class GenericAccessTracker {
public:
  void accessShared(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state);
  void accessExclusive(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav);

  void accessSharedPreRaster(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state);
  void accessExclusivePreRaster(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav);
  void accessSharedFragment(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state);
  void accessExclusiveFragment(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav);

  /**
   * Probe a pre-raster access without mutating dependency state. Metal 4 cannot
   * encode Fragment -> PreRaster as an in-render-pass barrier, so callers must
   * close the render encoder before publishing an access that returns true.
   */
  bool wouldRequireFragmentToPreRasterBoundary(EncoderId id,
                                               int flags) const;

  /**
   * GPTK resource trackers are command-buffer scoped. Cross-submit order is the
   * queue timeline only — residual exclusive_/shared_ EncoderIds from earlier
   * CBs must not seed wait sets (would become stripped external waits).
   */
  void reset() {
    shared_.clear();
    exclusive_ = {};
    isShared = 0;
    isSharedPreRaster = 0;
    lastWriteFromPreRaster = 0;
    generation_ = 0;
  }

  /** Lazy epoch reset when first touched in a new command-buffer generation. */
  void ensureGeneration(uint64_t generation) {
    if (generation_ == generation)
      return;
    shared_.clear();
    exclusive_ = {};
    isShared = 0;
    isSharedPreRaster = 0;
    lastWriteFromPreRaster = 0;
    generation_ = generation;
  }

private:
  /**
   * Previous shared access
   */
  FenceSet shared_;
  /**
   * Last exclusive access
   */
  EncoderId exclusive_{};
  uint64_t isShared               : 1 = 0;
  uint64_t isSharedPreRaster      : 1 = 0;
  uint64_t lastWriteFromPreRaster : 1 = 0;
  uint64_t generation_ = 0;
};

class FenceLocalityCheck {
public:
  FenceSet collectAndSimplifyWaits(
      FenceSet strong_fences,
      EncoderId id,
      bool implicit_pre_raster_wait = false,
      const char *trace_scope = nullptr);

  /** Clear rolling summaries at command-buffer boundaries (GPTK CB-local). */
  void reset() {
    for (auto &summary : summary_)
      summary.clear();
    summary_generation_.fill(0);
  }

private:
  std::array<FenceSet, kParityLane> summary_;
  std::array<EncoderId, kParityLane> summary_generation_ = {};
};

} // namespace dxmt
