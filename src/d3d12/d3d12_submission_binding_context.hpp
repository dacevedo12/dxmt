#pragma once

// The queue-owned state that per-stage binding encode still needs after every
// stateless part of it has been extracted.
//
// Encoding one shader stage reaches for exactly four things the command queue
// happens to own: the Metal device (sampler/texture materialization), the DXMT
// queue that owns the argument-buffer ring, and the two memoized stage-plan
// caches. Bundling them keeps the extracted encode functions at a readable
// parameter count without handing them the command queue itself — every member
// here is a namespace-level type the analyzer can reason about on its own.

#include "Metal.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_stage_plan_cache.hpp"
#include "dxmt_command_queue.hpp"

namespace dxmt::d3d12 {

struct SubmissionBindingContext {
  WMT::Device device;
  ::dxmt::CommandQueue &queue;
  SubmissionStagePlanCache<BindlessMirrorStagePlan> &bindless_stage_plans;
  SubmissionStagePlanCache<NativeRootBaseStagePlan> &native_stage_plans;
};

} // namespace dxmt::d3d12
