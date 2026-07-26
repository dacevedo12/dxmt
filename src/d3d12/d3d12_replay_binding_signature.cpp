#include "d3d12_replay_binding_signature.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_replay_perf_timers.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

uint64_t HashGraphicsBindingSnapshotState(
    const ReplayState &state, const PipelineState &pipeline,
    dxmt::DescriptorContentRevision descriptor_content_revision) {
  auto mix = [](uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  };
  uint64_t hash = 1469598103934665603ull;
  hash = mix(hash, reinterpret_cast<uintptr_t>(state.pipeline_state.ptr()));
  hash = mix(
      hash, reinterpret_cast<uintptr_t>(state.graphics_root_signature.ptr()));
  hash = mix(hash,
             reinterpret_cast<uintptr_t>(state.cbv_srv_uav_heap.ptr()));
  hash = mix(hash, reinterpret_cast<uintptr_t>(state.sampler_heap.ptr()));
  // Both D3D12 shader ABIs dereference persistent descriptor heaps at
  // execution time. Descriptor writes therefore invalidate the heap journal,
  // not the lightweight token that owns heap/root/table identities.
  const UINT root_count = state.graphics_root_signature_impl
                              ? std::min<UINT>(
                                    state.graphics_root_signature_impl
                                        ->GetParameters()
                                        .size(),
                                    state.graphics_tables.size())
                              : 0;
  for (UINT i = 0; i < root_count; ++i)
    hash = mix(hash, state.graphics_tables[i].ptr);
  auto hash_roots = [&](const auto &roots) {
    for (UINT i = 0; i < root_count; ++i) {
      const auto &root = roots[i];
      hash = mix(hash, root.valid);
      hash = mix(hash, root.valid ? root.address : 0);
    }
  };
  hash_roots(state.graphics_cbv_roots);
  hash_roots(state.graphics_srv_roots);
  hash_roots(state.graphics_uav_roots);
  for (UINT i = 0; i < root_count; ++i) {
    const auto &constants = state.graphics_root_constants[i];
    hash = mix(hash, constants.valid);
    hash = mix(hash, constants.valid ? constants.values.size() : 0);
    if (constants.valid) {
      for (const auto value : constants.values)
        hash = mix(hash, value);
    }
  }
  uint32_t vertex_slot_mask = 0;
  if (const auto *graphics = pipeline.GetGraphicsState()) {
    for (const auto &element : graphics->input_elements) {
      if (element.InputSlot < 32)
        vertex_slot_mask |= 1u << element.InputSlot;
    }
  }
  for (UINT i = 0; i < state.vertex_buffers.size(); ++i) {
    if (!(vertex_slot_mask & (1u << i)))
      continue;
    const auto &view = state.vertex_buffers[i];
    hash = mix(hash, view.has_value());
    if (!view)
      continue;
    hash = mix(hash, view->BufferLocation);
    hash = mix(hash, view->SizeInBytes);
    hash = mix(hash, view->StrideInBytes);
  }
  return hash;
}

bool GraphicsBindingSnapshotMatches(
    const GraphicsBindingSnapshot &snapshot, const ReplayState &state,
    dxmt::DescriptorContentRevision descriptor_content_revision) {
  if (!snapshot.legacy_identity)
    return false;
  const auto &identity = *snapshot.legacy_identity;
  if (snapshot.pipeline_state.ptr() != state.pipeline_state.ptr() ||
      snapshot.root_signature.ptr() != state.graphics_root_signature.ptr() ||
      snapshot.cbv_srv_uav_heap.ptr() != state.cbv_srv_uav_heap.ptr() ||
      snapshot.sampler_heap.ptr() != state.sampler_heap.ptr())
    return false;
  const UINT root_count = state.graphics_root_signature_impl
                              ? std::min<UINT>(
                                    state.graphics_root_signature_impl
                                        ->GetParameters()
                                        .size(),
                                    state.graphics_tables.size())
                              : 0;
  for (UINT i = 0; i < root_count; ++i) {
    if (identity.graphics_tables[i].ptr != state.graphics_tables[i].ptr)
      return false;
  }
  auto roots_match = [root_count](const auto &identity, const auto &roots) {
    for (UINT i = 0; i < root_count; ++i) {
      if (identity[i].valid != roots[i].valid ||
          (roots[i].valid && identity[i].address != roots[i].address))
        return false;
    }
    return true;
  };
  if (!roots_match(identity.graphics_cbv_roots,
                   state.graphics_cbv_roots) ||
      !roots_match(identity.graphics_srv_roots,
                   state.graphics_srv_roots) ||
      !roots_match(identity.graphics_uav_roots,
                   state.graphics_uav_roots))
    return false;
  for (UINT i = 0; i < root_count; ++i) {
    const auto &constants = state.graphics_root_constants[i];
    const auto &captured = identity.graphics_root_constants[i];
    if (captured.valid != constants.valid ||
        (constants.valid && captured.values != constants.values))
      return false;
  }
  for (UINT i = 0; i < state.vertex_buffers.size(); ++i) {
    if (!(snapshot.vertex_slot_mask & (1u << i)))
      continue;
    const auto &vertex_identity = identity.vertex_buffer_views[i];
    const auto &view = state.vertex_buffers[i];
    if (vertex_identity.valid != view.has_value())
      return false;
    if (view &&
        (vertex_identity.view.BufferLocation != view->BufferLocation ||
         vertex_identity.view.SizeInBytes != view->SizeInBytes ||
         vertex_identity.view.StrideInBytes != view->StrideInBytes))
      return false;
  }
  return true;
}

std::shared_ptr<GraphicsBindingSnapshot> FindCachedGraphicsBindingSnapshot(
    const ReplayState &state, const PipelineState &pipeline,
    dxmt::DescriptorContentRevision descriptor_content_revision) {
  const auto key = HashGraphicsBindingSnapshotState(
      state, pipeline, descriptor_content_revision);
  const auto &cache = state.graphics_pass_batch.captured_binding_snapshots;
  const auto [begin, end] = cache.equal_range(key);
  for (auto it = begin; it != end; ++it) {
    if (GraphicsBindingSnapshotMatches(
            *it->second, state, descriptor_content_revision))
      return it->second;
  }
  return {};
}

void RecordReplayBindingSignaturePerf(const ReplayState &state) {
  if (!ReplayPerfEnabled())
    return;
  static thread_local const void *last_pso = nullptr;
  static thread_local const void *last_rootsig = nullptr;
  static thread_local uint64_t last_full = 0;
  auto &timers = perDrawSubTimers();
  timers.drawCount++;
  const void *pso = state.pipeline_state.ptr();
  const void *rs = state.graphics_root_signature.ptr();
  if (pso == last_pso && rs == last_rootsig)
    timers.psoRootUnchanged++;
  const uint64_t full = HashGraphicsBindingFull(state);
  if (timers.drawCount > 1 && full == last_full)
    timers.fullBindUnchanged++;
  last_pso = pso;
  last_rootsig = rs;
  last_full = full;
}

} // namespace dxmt::d3d12
