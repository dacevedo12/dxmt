#pragma once

// Memoization shells for the per-(root signature, pipeline, stage) stage plans
// built by d3d12_stage_plan_build.hpp.
//
// The builders were extracted first; what was left inside CommandQueueImpl
// (d3d12_command_queue_binding_plans.inc) was the lock/lookup/validate/evict
// shell around them, which only ever touched the cache it was handed. Bundling
// the map with its mutex into a namespace-level type lets that shell compile on
// its own while the cache storage itself stays queue-owned, so plans are still
// reused across submits without being shared between queues.

#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_thread_safety.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace dxmt::d3d12 {

// Cache keyed by HashSubmissionPlanIdentity(). Entries are immutable once
// published, so readers keep their shared_ptr alive past an eviction.
//
// The pairing of the map with its mutex is the whole reason this type exists,
// so state it where the compiler can check it: the type is exported from a
// shared header, and an unguarded `cache.entries.size()` added later for a
// diagnostic would otherwise race the submission worker in silence.
template <typename Plan> struct SubmissionStagePlanCache {
  std::mutex mutex;
  std::unordered_map<uint64_t, std::shared_ptr<const Plan>>
      entries DXMT_GUARDED_BY(mutex);
};

// Returns the cached plan for this (root, pipeline, stage, compute) tuple,
// building and publishing one on miss. The identity fields of a hit are
// re-checked because the key is a hash: a collision falls through to a rebuild
// that overwrites the colliding entry.
//
// Never returns null -- a miss builds the plan unconditionally, and a hit is
// only taken for a non-null entry. Callers may dereference the result without a
// check; the null guards further down (d3d12_native_stage_binding.hpp) exist
// because those functions take a raw pointer, not because this can hand one
// back. Stating it here because the absence of the guarantee is what made the
// dereference look like an oversight next to the call sites that do check.
[[nodiscard]] std::shared_ptr<const BindlessMirrorStagePlan>
GetBindlessMirrorStagePlan(
    SubmissionStagePlanCache<BindlessMirrorStagePlan> &cache,
    const PipelineState &pipeline, const RootSignature &root,
    PipelineStage want_stage, bool compute);

[[nodiscard]] std::shared_ptr<const NativeRootBaseStagePlan>
GetNativeRootBaseStagePlan(
    SubmissionStagePlanCache<NativeRootBaseStagePlan> &cache,
    const PipelineState &pipeline, const RootSignature &root,
    PipelineStage want_stage, bool compute);

} // namespace dxmt::d3d12
