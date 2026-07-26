#pragma once

// Content fingerprint of the descriptor bindings a binding plan resolves to,
// used to elide the per-draw resource hazard pass.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_descriptor_binding.inc). It resolves descriptors purely
// from the replay state, the plan and the heaps handed to it, so it never
// touches the command queue instance and can be compiled and analysed on its
// own.

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_stage_plan_build.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace dxmt::d3d12 {

// Hashes each plan entry's resolved CONTENT (resource pointer, record type,
// counter resource, root buffer address) rather than its descriptor handle, so
// ring-buffer handle churn that re-binds the same resources keeps the same
// fingerprint. `root` and `pipeline` only contribute their object identity.
//
// Also fills `entries_by_slot`: for every CBV/SRV/UAV heap slot that a plan
// entry resolved to, the indices of the entries reading it. The journal cache
// replays only those entries when the heap reports writes since the last draw.
[[nodiscard]] uint64_t HashDescriptorAccessBindingFingerprint(
    const ReplayState &state, bool compute, const RootSignature *root,
    const PipelineState *pipeline, const BindingPlan &plan,
    DescriptorHeap *cbv_heap, DescriptorHeap *sampler_heap,
    std::unordered_map<uint32_t, std::vector<uint32_t>> &entries_by_slot);

} // namespace dxmt::d3d12
