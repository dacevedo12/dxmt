#pragma once

// Builders for the per-(root signature, pipeline, stage) binding plans: the
// reflected descriptor-range size, the descriptor-table binding recipe, the
// bindless-mirror stage plan and the native root-base stage plan.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_plans.inc). Each one is a pure function of the
// pipeline reflection and the root signature layout, so none of them touch the
// command queue instance and the whole set can be compiled and analysed on its
// own. The caches that memoize the results stay on the queue.

#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_root_signature.hpp"

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// PERF P1: binding-plan cache. The descAccess traversal structure (which
// register -> which descriptor slot/heap/stage) is a pure function of
// (root_sig, PSO); only descriptor VALUES vary per draw. Cache the structure
// and iterate it per draw instead of re-walking root sig + reflection.
enum class BindingEntryKind : uint8_t { Table, RootBuffer };

struct BindingPlanEntry {
  BindingEntryKind kind;
  PipelineStage stage;
  UINT root_index;
  UINT range_offset;        // Table only
  UINT descriptor_index;    // Table only
  UINT count;               // Table only
  D3D12_DESCRIPTOR_HEAP_TYPE heap_type;     // Table only
  D3D12_DESCRIPTOR_RANGE_TYPE range_type;   // Table only
  DescriptorRecordType buffer_type;         // RootBuffer only
};

struct BindingPlan {
  bool compute = false;
  uint64_t root_identity = 0;
  uint64_t pipeline_identity = 0;
  std::vector<BindingPlanEntry> entries;
};

// P1: build the binding plan (cache-miss path). It pushes entries instead of
// accessing descriptors; base handles, heaps and descriptor records remain
// per-draw values and are resolved by the consume path.
[[nodiscard]] BindingPlan BuildBindingPlan(RootSignature *root,
                                           PipelineState &pipeline,
                                           bool compute);

// Descriptor count of an unbounded (UINT_MAX) range, derived from how far the
// shader reflection actually indexes past the range's base register. Returns 1
// when no visible stage references the range.
[[nodiscard]] UINT
ReflectedDescriptorRangeCount(const PipelineState &pipeline,
                              const RootSignatureRange &range,
                              D3D12_SHADER_VISIBILITY visibility, bool compute);

// Flattens every (root parameter, range, stage, shader register) descriptor
// table binding of `pipeline` under `root` into a replayable recipe.
[[nodiscard]] DescriptorTableBindingRecipe
BuildDescriptorTableBindingRecipe(const PipelineState &pipeline,
                                  const RootSignature &root, bool compute);

// Bindless-mirror texture/sampler window layout plus the descriptor-table
// sources that feed it, for one shader stage.
[[nodiscard]] BindlessMirrorStagePlan
BuildBindlessMirrorStagePlan(const PipelineState &pipeline,
                             const RootSignature &root,
                             PipelineStage want_stage, bool compute);

// Native root-base descriptor layout for one shader stage: which root
// parameters and descriptor ranges feed which argument-buffer root base key.
[[nodiscard]] NativeRootBaseStagePlan
BuildNativeRootBaseStagePlan(const PipelineState &pipeline,
                             const RootSignature &root,
                             PipelineStage want_stage, bool compute);

} // namespace dxmt::d3d12
