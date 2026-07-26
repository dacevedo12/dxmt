#include "dxmt_deptrack.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include <cassert>

namespace dxmt {
namespace {

void
copyFenceMasks(const FenceSet &set, LaneStorage (&masks)[kParity]) {
  for (int i = 0; i < kParity; i++) {
    masks[i] = set.storage(i);
  }
}

void
emitFenceDependencyTrace(
    const char *scope,
    EncoderId id,
    bool implicit_pre_raster_wait,
    const FenceSet &strong_fences,
    const FenceSet &full_fences,
    const FenceSet &minimal_fences) {
  if (!apitrace::d3d_enabled())
    return;

  LaneStorage strong_masks[kParity] = {};
  LaneStorage full_masks[kParity] = {};
  LaneStorage minimal_masks[kParity] = {};
  copyFenceMasks(strong_fences, strong_masks);
  copyFenceMasks(full_fences, full_masks);
  copyFenceMasks(minimal_fences, minimal_masks);

  apitrace::on_fence_dependency(
      scope ? scope : "unknown",
      apitrace::current_d3d_sequence(),
      id,
      implicit_pre_raster_wait,
      strong_masks,
      strong_fences.count(),
      full_masks,
      full_fences.count(),
      minimal_masks,
      minimal_fences.count(),
      kParity);
}

} // namespace

void
GenericAccessTracker::accessShared(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state) {
  if (exclusive_ == id) {
    if (isShared)
      return;
    isShared = 1;
    barrier_state.barrierSet |= kBarrierTypeRW;
    return;
  }
  assert(exclusive_ < id);
  if (shared_.isLastAccess(id))
    return;
  shared_.add(id);
  if (exclusive_) {
    wait_fences.set(exclusive_);
  }
}

void
GenericAccessTracker::accessExclusive(
    EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav
) {
  isShared = 0;
  if (exclusive_ == id) {
    barrier_state.barrierSet |= (1 << uav);
    return;
  }
  if (shared_.isLastAccess(id)) {
    barrier_state.barrierSet |= kBarrierTypeRW;
  }
  shared_.enumerate(id, [&](EncoderId id) { wait_fences.set(id); });
  shared_.clear();
  if (exclusive_)
    wait_fences.set(exclusive_);
  exclusive_ = id;
}

void
GenericAccessTracker::accessSharedPreRaster(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state) {
  if (exclusive_ == id + 1) {
    if (isSharedPreRaster)
      return;
    isSharedPreRaster = 1;
    if (lastWriteFromPreRaster)
      barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    else
      barrier_state.barrierPreRasterAfterFragmentSet |= kBarrierTypeRW;
    return;
  }
  if (exclusive_ == id) {
    if (isSharedPreRaster)
      return;
    isSharedPreRaster = 1;
    barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    return;
  }
  assert(exclusive_ < id);
  if (shared_.isLastAccess(id + 1)) {
    if (isSharedPreRaster)
      return;
    isSharedPreRaster = 1;
    if (exclusive_)
      wait_fences.set(exclusive_);
    return;
  } else if (shared_.isLastAccess(id)) {
    // NOP
    return;
  }
  shared_.add(id);
  if (exclusive_)
    wait_fences.set(exclusive_);
  isSharedPreRaster = 1;
  isShared = 0;
}

void
GenericAccessTracker::accessExclusivePreRaster(
    EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav
) {
  auto last_exclusive = exclusive_;
  exclusive_ = id;

  if (last_exclusive == id + 1) {
    auto last = lastWriteFromPreRaster;
    lastWriteFromPreRaster = 1;
    if (!isShared && !isSharedPreRaster) {
      if (last)
        barrier_state.barrierPreRasterSet |= (1 << uav);
      else
        barrier_state.barrierPreRasterAfterFragmentSet |= (1 << uav);
      return;
    }
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierPreRasterAfterFragmentSet |= kBarrierTypeRW;
    }
    return;
  }
  if (last_exclusive == id) {
    if (!isShared && !isSharedPreRaster) {
      barrier_state.barrierPreRasterSet |= (1 << uav);
    }
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierPreRasterAfterFragmentSet |= kBarrierTypeRW;
    }
    return;
  }
  shared_.enumerate(id, [&](EncoderId id) { wait_fences.set(id); });
  if (last_exclusive)
    wait_fences.set(last_exclusive);

  if (shared_.isLastAccess(id + 1) || shared_.isLastAccess(id)) {
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierPreRasterAfterFragmentSet |= kBarrierTypeRW;
    }
    shared_.clear();
    return;
  }
  shared_.clear();
  isShared = 0;
  isSharedPreRaster = 0;
}

void
GenericAccessTracker::accessSharedFragment(EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state) {
  if (exclusive_ == id) {
    if (isShared)
      return;
    isShared = 1;
    if (isSharedPreRaster)
      return; // IMPLICIT BARRIER
    if (lastWriteFromPreRaster)
      barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    else
      barrier_state.barrierSet |= kBarrierTypeRW;
    return;
  }
  if (exclusive_ == id - 1) {
    if (isShared)
      return;
    isShared = 1;
    if (isSharedPreRaster)
      return; // IMPLICIT BARRIER
    barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    return;
  }
  assert(exclusive_ < id - 1);
  if (shared_.isLastAccess(id))
    return;
  bool isVertexLastAccess = shared_.isLastAccess(id - 1);
  if (exclusive_)
    wait_fences.set(exclusive_);
  shared_.add(id);
  isShared = 1;
  if (!isVertexLastAccess)
    isSharedPreRaster = 0;
}

void
GenericAccessTracker::accessExclusiveFragment(
    EncoderId id, FenceSet &wait_fences, EncoderBarrierState &barrier_state, bool uav
) {
  auto last_exclusive = exclusive_;
  exclusive_ = id;
  if (last_exclusive == id) {
    auto last = lastWriteFromPreRaster;
    lastWriteFromPreRaster = 0;
    if (!isShared && !isSharedPreRaster) {
      if (last)
        barrier_state.barrierFragmentAfterPreRasterSet |= (1 << uav);
      else
        barrier_state.barrierSet |= (1 << uav);
      return;
    }
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierSet |= kBarrierTypeRW;
    }
    return;
  }
  lastWriteFromPreRaster = 0;
  if (last_exclusive == id - 1) {
    if (!isShared && !isSharedPreRaster) {
      barrier_state.barrierFragmentAfterPreRasterSet |= (1 << uav);
      return;
    }
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierSet |= kBarrierTypeRW;
    }
    return;
  }
  shared_.enumerate(id, [&](EncoderId id) { wait_fences.set(id); });
  if (last_exclusive)
    wait_fences.set(last_exclusive);
  if (shared_.isLastAccess(id) || shared_.isLastAccess(id - 1)) {
    if (isSharedPreRaster) {
      isSharedPreRaster = 0;
      barrier_state.barrierFragmentAfterPreRasterSet |= kBarrierTypeRW;
    }
    if (isShared) {
      isShared = 0;
      barrier_state.barrierSet |= kBarrierTypeRW;
    }
    shared_.clear();
    return;
  }
  shared_.clear();
  isShared = 0;
  isSharedPreRaster = 0;
}

bool
GenericAccessTracker::wouldRequireFragmentToPreRasterBoundary(
    EncoderId id, int flags) const {
  // Reuse the production state machine on a value snapshot. This keeps the
  // probe exactly aligned with accessShared/ExclusivePreRaster as those rules
  // evolve, without consuming the real Fragment producer identity.
  auto probe = *this;
  FenceSet ignored_waits;
  EncoderBarrierState probe_barrier = {};
  if (flags & ResourceAccess::Write) {
    probe.accessExclusivePreRaster(
        id, ignored_waits, probe_barrier,
        (flags & ResourceAccess::UAV) != 0);
  } else {
    probe.accessSharedPreRaster(id, ignored_waits, probe_barrier);
  }
  return probe_barrier.barrierPreRasterAfterFragmentSet != 0;
}

FenceSet
FenceLocalityCheck::collectAndSimplifyWaits(
    FenceSet strong_fences,
    EncoderId id,
    bool implicit_pre_raster_wait,
    const char *trace_scope) {
  if (implicit_pre_raster_wait)
    strong_fences.set(id - 1);

  FenceSet full_fences(strong_fences);

  FenceSet minimal_fences;
  FenceSet accessible_fences;

  // The rolling summaries below can only prove transitive coverage inside
  // their generation window. A strong dependency outside that window must be
  // kept explicitly; otherwise a correctly tracked distant producer silently
  // disappears before Metal fence encoding.
  strong_fences.forEach([&](EncoderId producer_id) {
    if (producer_id < id && id - producer_id >= kParityLane)
      minimal_fences.set(producer_id);
  });

  for (EncoderId offset = 1; offset < kParityLane && offset <= id; offset++) {
    EncoderId prev_encoder_id = id - offset;

    if (full_fences.test(prev_encoder_id) && !accessible_fences.testAndSet(prev_encoder_id))
      minimal_fences.set(prev_encoder_id);
    if (accessible_fences.test(prev_encoder_id) &&
        summary_generation_[prev_encoder_id % kParityLane] ==
            prev_encoder_id)
      accessible_fences.merge(summary_[prev_encoder_id % kParityLane]);
    if (accessible_fences.contains(full_fences))
      break;
  }

  summary_[id % kParityLane] = full_fences;
  summary_generation_[id % kParityLane] = id;

  if (implicit_pre_raster_wait)
    minimal_fences.unset(id - 1);

  emitFenceDependencyTrace(
      trace_scope,
      id,
      implicit_pre_raster_wait,
      strong_fences,
      full_fences,
      minimal_fences);

  return minimal_fences;
}

} // namespace dxmt
