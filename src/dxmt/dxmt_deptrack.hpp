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

  // Collapses the entries that predate the command buffer being encoded down
  // to the newest one.
  //
  // This is what bounds the set. A shared set is only ever emptied by an
  // exclusive access, so a resource that is sampled every frame and never
  // written accumulates one entry per encoder that ever read it. Left alone
  // that reached 15472 entries, and the cost was not searching them but
  // holding them: around 121KB per resource, untouched long enough between
  // frames for the host to compress it, so a single access paid a fault storm
  // and ran for over a second.
  //
  // One entry has to survive rather than none. Such an id can no longer
  // resolve to a fence, since fence bindings are rebuilt per command buffer,
  // but prepareFencePool counts exactly these unresolvable waits, and a blit
  // or explicit-barrier wait among them makes the submission serialize behind
  // the previous one. Ordinary render and compute external edges deliberately
  // do not, so this is not "every external edge serializes". Dropping the
  // whole prefix would retract the cases that do, which for an untracked
  // resource is the only thing ordering it across the boundary. The counts are
  // never compared against anything but zero, so the newest external id
  // preserves every decision the whole prefix would have driven.
  void
  pruneBefore(EncoderId first_live) {
    // Tests the SECOND entry, not the first. Once collapsed the set keeps one
    // entry below the bound, so a front() test would be false on every later
    // access and walk the whole set to discover there is nothing to do, which
    // is the memory traffic this exists to avoid. Entries are sorted, so a
    // second entry at or above the bound means the set is already collapsed.
    if (entries_.size() < 2 || entries_[1] >= first_live)
      return;
    auto newest_external = std::lower_bound(entries_.begin(), entries_.end(), first_live) - 1;
    const size_t remaining = static_cast<size_t>(entries_.end() - newest_external);
    std::move(newest_external, entries_.end(), entries_.begin());
    entries_.resize(remaining);
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
 * Maps exact logical encoder dependency IDs to reusable Metal fence objects
 * for one command buffer. Bindings never survive reset: ordering between
 * command buffers on the same queue already provides that dependency.
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

  // Called before an access rather than at the command buffer boundary: the
  // trackers live on the resources, so there is no point that can walk them
  // all, and a resource nobody touches costs nothing to leave stale.
  void pruneShared(EncoderId first_live) { shared_.pruneBefore(first_live); }

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
};

class FenceLocalityCheck {
public:
  FenceSet collectAndSimplifyWaits(
      FenceSet strong_fences,
      EncoderId id,
      bool implicit_pre_raster_wait = false,
      const char *trace_scope = nullptr);

private:
  std::array<FenceSet, kParityLane> summary_;
  std::array<EncoderId, kParityLane> summary_generation_ = {};
};

} // namespace dxmt
