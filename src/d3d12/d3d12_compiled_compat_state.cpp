#include "d3d12_compiled_compat_state.hpp"

#include "d3d12_command_list.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_replay_pass_batch_ops.hpp"

namespace dxmt::d3d12 {

void ApplyCompiledGraphicsCompatibilityState(
    ReplayState &state, const CompiledGraphicsPacket &packet) {
  state.pipeline_state = packet.pipeline.pipeline_state;
  state.graphics_root_signature = packet.pipeline.root_signature;
  state.graphics_root_signature_impl =
      GetRootSignature(packet.pipeline.root_signature.ptr());
  state.cbv_srv_uav_heap = packet.descriptor_heaps.cbv_srv_uav;
  state.sampler_heap = packet.descriptor_heaps.sampler;
  ApplyCompiledRootBindings(state, packet, false);
  state.resolved_vertex_buffers.fill({});
  state.resolved_index_buffer = {};
  state.resolved_vertex_buffer_cache.clear();
  state.resolved_index_buffer_cache.clear();
  state.vertex_buffers.fill({});
  for (const auto &binding : packet.input_assembler.vertex_buffers) {
    if (binding.slot < state.vertex_buffers.size()) {
      state.vertex_buffers[binding.slot] = binding.view;
      state.resolved_vertex_buffers[binding.slot] =
          ResolveReplayVertexBuffer(state, binding.view);
    }
  }
  state.index_buffer = packet.input_assembler.index_buffer;
  if (state.index_buffer)
    state.resolved_index_buffer =
        ResolveReplayIndexBuffer(state, *state.index_buffer);
  state.render_targets = packet.render_state.render_targets.copy();
  state.depth_stencil = packet.render_state.depth_stencil;
  state.viewports = packet.render_state.viewports.copy();
  state.scissors = packet.render_state.scissors.copy();
  state.blend_factor = packet.render_state.blend_factor;
  state.stencil_ref = packet.render_state.stencil_ref;
  state.topology = packet.render_state.topology;
  state.graphics_binding_generation++;
}

void ApplyCompiledComputeCompatibilityState(
    ReplayState &state, const CompiledComputePacket &packet) {
  state.pipeline_state = packet.pipeline.pipeline_state;
  state.compute_root_signature = packet.pipeline.root_signature;
  state.compute_root_signature_impl =
      GetRootSignature(packet.pipeline.root_signature.ptr());
  state.cbv_srv_uav_heap = packet.descriptor_heaps.cbv_srv_uav;
  state.sampler_heap = packet.descriptor_heaps.sampler;
  ApplyCompiledRootBindings(state, packet, true);
}

} // namespace dxmt::d3d12
