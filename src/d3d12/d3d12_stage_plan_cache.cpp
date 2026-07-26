#include "d3d12_stage_plan_cache.hpp"

#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_stage_plan_build.hpp"

#include <utility>

namespace dxmt::d3d12 {
namespace {

// Both stage plans expose the same identity quadruple, so the shell is written
// once and instantiated per plan type inside this translation unit.
template <typename Plan, typename Build>
std::shared_ptr<const Plan>
GetOrBuildStagePlan(SubmissionStagePlanCache<Plan> &cache,
                    const PipelineState &pipeline, const RootSignature &root,
                    PipelineStage want_stage, bool compute, Build build) {
  const auto root_identity = root.GetCacheIdentity();
  const auto pipeline_identity = pipeline.GetCacheIdentity();
  const uint64_t key = HashSubmissionPlanIdentity(
      root_identity, pipeline_identity, want_stage, compute);

  std::lock_guard lock(cache.mutex);
  auto it = cache.entries.find(key);
  if (it != cache.entries.end() && it->second &&
      it->second->root_identity == root_identity &&
      it->second->pipeline_identity == pipeline_identity &&
      it->second->stage == want_stage && it->second->compute == compute)
    return it->second;

  auto plan = std::make_shared<Plan>(
      build(pipeline, root, want_stage, compute));
  if (it == cache.entries.end() &&
      cache.entries.size() >= kSubmissionPlanCacheLimit)
    cache.entries.erase(cache.entries.begin());
  cache.entries[key] = plan;
  return plan;
}

} // namespace

std::shared_ptr<const BindlessMirrorStagePlan>
GetBindlessMirrorStagePlan(
    SubmissionStagePlanCache<BindlessMirrorStagePlan> &cache,
    const PipelineState &pipeline, const RootSignature &root,
    PipelineStage want_stage, bool compute) {
  return GetOrBuildStagePlan(cache, pipeline, root, want_stage, compute,
                             BuildBindlessMirrorStagePlan);
}

std::shared_ptr<const NativeRootBaseStagePlan>
GetNativeRootBaseStagePlan(
    SubmissionStagePlanCache<NativeRootBaseStagePlan> &cache,
    const PipelineState &pipeline, const RootSignature &root,
    PipelineStage want_stage, bool compute) {
  return GetOrBuildStagePlan(cache, pipeline, root, want_stage, compute,
                             BuildNativeRootBaseStagePlan);
}

} // namespace dxmt::d3d12
