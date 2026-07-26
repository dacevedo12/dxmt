#include "d3d12_replay_state_clone.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_replay_perf_timers.hpp"
#include "dxmt_statistics.hpp"

#include <chrono>

namespace dxmt::d3d12 {

ReplayState CloneReplayStateWithoutBatch(const ReplayState &state) {
  static const bool rb_clone =
      D3D12DiagEnabledEnv("DXMT_DIAG_REPLAY_BREAKDOWN");
  const auto rb_clone_t0 = rb_clone ? clock::now() : clock::time_point{};
  ReplayState copy = {};
  copy.resource_states = state.resource_states;
  copy.queue_type = state.queue_type;
  copy.touched_resources = state.touched_resources;
  copy.touched_resources_set = state.touched_resources_set;
  copy.pipeline_state = state.pipeline_state;
  copy.graphics_root_signature = state.graphics_root_signature;
  copy.compute_root_signature = state.compute_root_signature;
  copy.graphics_root_signature_impl = state.graphics_root_signature_impl;
  copy.compute_root_signature_impl = state.compute_root_signature_impl;
  copy.topology = state.topology;
  copy.viewports = state.viewports;
  copy.scissors = state.scissors;
  copy.blend_factor = state.blend_factor;
  copy.stencil_ref = state.stencil_ref;
  copy.render_targets = state.render_targets;
  copy.depth_stencil = state.depth_stencil;
  copy.vertex_buffers = state.vertex_buffers;
  copy.index_buffer = state.index_buffer;
  copy.resolved_vertex_buffers = state.resolved_vertex_buffers;
  copy.resolved_index_buffer = state.resolved_index_buffer;
  copy.cbv_srv_uav_heap = state.cbv_srv_uav_heap;
  copy.sampler_heap = state.sampler_heap;
  copy.graphics_tables = state.graphics_tables;
  copy.compute_tables = state.compute_tables;
  copy.graphics_root_constants = state.graphics_root_constants;
  copy.compute_root_constants = state.compute_root_constants;
  copy.graphics_cbv_roots = state.graphics_cbv_roots;
  copy.compute_cbv_roots = state.compute_cbv_roots;
  copy.graphics_srv_roots = state.graphics_srv_roots;
  copy.compute_srv_roots = state.compute_srv_roots;
  copy.graphics_uav_roots = state.graphics_uav_roots;
  copy.compute_uav_roots = state.compute_uav_roots;
  copy.current_record_d3d_sequence = state.current_record_d3d_sequence;
  copy.graphics_binding_generation = state.graphics_binding_generation;
  copy.predication_buffer = state.predication_buffer;
  copy.predication_buffer_offset = state.predication_buffer_offset;
  copy.predication_operation = state.predication_operation;
  if (rb_clone)
    perDrawSubTimers().clone +=
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now() - rb_clone_t0).count();
  return copy;
}

} // namespace dxmt::d3d12
