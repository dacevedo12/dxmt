#include "d3d12_replay_api_state_reset.hpp"

#include "d3d12_replay_pass_batch_ops.hpp"
#include "dxmt_statistics.hpp"

namespace dxmt::d3d12 {

void ResetReplayApiState(ReplayState &state,
                         ID3D12PipelineState *pipeline_state) {
  state.pipeline_state = pipeline_state;
  state.graphics_root_signature = nullptr;
  state.compute_root_signature = nullptr;
  state.graphics_root_signature_impl = nullptr;
  state.compute_root_signature_impl = nullptr;
  state.topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  state.viewports.clear();
  state.scissors.clear();
  state.blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
  state.stencil_ref = 0;
  state.render_targets.clear();
  state.depth_stencil.reset();
  state.vertex_buffers = {};
  state.index_buffer.reset();
  state.resolved_vertex_buffers = {};
  state.resolved_index_buffer = {};
  state.cbv_srv_uav_heap = nullptr;
  state.sampler_heap = nullptr;
  state.graphics_tables = {};
  state.compute_tables = {};
  state.graphics_root_constants = {};
  state.compute_root_constants = {};
  state.graphics_cbv_roots = {};
  state.compute_cbv_roots = {};
  state.graphics_srv_roots = {};
  state.compute_srv_roots = {};
  state.graphics_uav_roots = {};
  state.compute_uav_roots = {};
  state.has_last_snapshot_request_generation = false;
  state.last_snapshot_request_graphics_generation = 0;
  state.last_snapshot_request_descriptor_revision = {};
  state.da_cache.clear();
  state.descriptor_journal_access_cache = {};
  state.predication_buffer = nullptr;
  state.predication_buffer_offset = 0;
  state.predication_operation = D3D12_PREDICATION_OP_EQUAL_ZERO;
  BumpGraphicsBindingGeneration(
      state, GraphicsBindingGenerationBumpSource::PipelineState);
}

void ResetReplayCommandListSemanticState(ReplayState &state) {
  // Command-list state does not carry across D3D12 command-list boundaries,
  // but submission-owned batches, descriptor snapshots, resource state and
  // deferred barriers do. Reset only the API-visible recording state so
  // compatible work from adjacent lists can remain in the same Metal
  // encoder without allowing a fallback record to inherit bindings.
  ResetReplayApiState(state, nullptr);
  state.current_record_d3d_sequence = 0;
  state.compiled_fallback_reason =
      dxmt::CompiledFallbackReason::Unknown;
}

} // namespace dxmt::d3d12
