#pragma once

// Binding-signature hashing and legacy binding-snapshot cache lookup.
//
// These used to be private (mostly static) members of class CommandQueueImpl
// (d3d12_command_queue_pass_queue.inc). None of them reads a CommandQueueImpl
// instance member or names `this`: they only hash / compare the promoted
// ReplayState and GraphicsBindingSnapshot value types, and the one perf
// reporter additionally pokes the process-wide diagnostic ledger
// perDrawSubTimers(). FindCachedGraphicsBindingSnapshot() was a plain
// non-static member but only ever called the two statics below.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_queue_state_types.hpp"

#include <cstdint>
#include <memory>

namespace dxmt::d3d12 {

// PERF DIAG: compute a binding signature for the GRAPHICS draw state, split
// into (a) structure key = (PSO, root_sig) which determines the descAccess
// traversal STRUCTURE (binding-plan cache hit rate), and (b) full value hash
// = structure + heaps + bound tables/roots/root-constants which determines
// whether the whole descAccess can be skipped vs the previous draw.
template <typename GraphicsState>
[[nodiscard]] uint64_t HashGraphicsBindingFull(const GraphicsState &s) {
  auto mix = [](uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  };
  uint64_t h = 1469598103934665603ull;
  h = mix(h, (uint64_t)s.pipeline_state.ptr());
  h = mix(h, (uint64_t)s.graphics_root_signature.ptr());
  h = mix(h, (uint64_t)s.cbv_srv_uav_heap.ptr());
  h = mix(h, (uint64_t)s.sampler_heap.ptr());
  for (UINT i = 0; i < s.graphics_tables.size(); i++)
    if (s.graphics_tables[i].ptr)
      h = mix(mix(h, i), s.graphics_tables[i].ptr);
  for (UINT i = 0; i < s.graphics_cbv_roots.size(); i++)
    if (s.graphics_cbv_roots[i].valid)
      h = mix(mix(h, i), s.graphics_cbv_roots[i].address);
  for (UINT i = 0; i < s.graphics_srv_roots.size(); i++)
    if (s.graphics_srv_roots[i].valid)
      h = mix(mix(h, i), s.graphics_srv_roots[i].address);
  for (UINT i = 0; i < s.graphics_uav_roots.size(); i++)
    if (s.graphics_uav_roots[i].valid)
      h = mix(mix(h, i), s.graphics_uav_roots[i].address);
  for (UINT i = 0; i < s.graphics_root_constants.size(); i++) {
    const auto &slot = s.graphics_root_constants[i];
    if (!slot.valid)
      continue;
    h = mix(h, i);
    for (auto v : slot.values) h = mix(h, v);
  }
  return h;
}

// Cache key for the legacy (non-compiled) graphics binding snapshot: heap and
// root identities only, deliberately excluding descriptor *contents*.
[[nodiscard]] uint64_t HashGraphicsBindingSnapshotState(
    const ReplayState &state, const PipelineState &pipeline,
    dxmt::DescriptorContentRevision descriptor_content_revision);

// Exact comparison behind the HashGraphicsBindingSnapshotState() bucket.
[[nodiscard]] bool GraphicsBindingSnapshotMatches(
    const GraphicsBindingSnapshot &snapshot, const ReplayState &state,
    dxmt::DescriptorContentRevision descriptor_content_revision);

// Looks up an equivalent snapshot already captured for the open graphics pass
// batch. Returns an empty pointer on a miss.
[[nodiscard]] std::shared_ptr<GraphicsBindingSnapshot>
FindCachedGraphicsBindingSnapshot(
    const ReplayState &state, const PipelineState &pipeline,
    dxmt::DescriptorContentRevision descriptor_content_revision);

// PERF DIAG: counts how often consecutive draws reuse the same (PSO, root-sig)
// structure and the same full binding value hash.
void RecordReplayBindingSignaturePerf(const ReplayState &state);

} // namespace dxmt::d3d12
