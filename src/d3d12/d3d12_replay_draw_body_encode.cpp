#include "d3d12_replay_draw_body_encode.hpp"

#include "d3d12_argument_buffer_layout.hpp"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_draw_visibility_scope.hpp"
#include "d3d12_indirect_topology.hpp"

#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"

#include <utility>

namespace dxmt::d3d12 {

void EncodeReplayDrawInstancedBody(ArgumentEncodingContext &enc,
                                   dxmt::CommandQueue &queue,
                                   ReplayDrawInstancedPacket &packet,
                                   uint64_t &argbuf_offset,
                                   dxmt::FrameStatistics *perf_stats) {
  auto &common = packet.common;
  Rc<VisibilityResultQuery> active_visibility_query;
  {
    dxmt::perf::ScopedFrameDuration visibility_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_visibility_interval);
    active_visibility_query =
        BeginReplayDrawVisibilityQuery(enc, common.visibility_query);
  }

  {
    dxmt::perf::ScopedFrameDuration body_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_body_interval);
    if (common.use_tessellation) {
      auto *render_encoder = enc.currentRenderEncoder();
      render_encoder->use_tessellation = 1;
      enc.tess_num_output_control_point_element =
          common.tess_num_output_control_point_element;
      enc.tess_threads_per_patch = common.tess_threads_per_patch;
      if (!common.tess_threads_per_patch || !common.control_point_count) {
        WARN("D3D12CommandQueue: tessellation draw skipped because tessellation metadata is invalid");
        return;
      }

      const auto patch_count_per_instance =
          packet.vertex_count / *common.control_point_count;
      if (!patch_count_per_instance)
        return;
      const auto patch_per_group = 32u / common.tess_threads_per_patch;
      if (!patch_per_group) {
        WARN("D3D12CommandQueue: tessellation draw skipped because threads-per-patch is unsupported value=",
             common.tess_threads_per_patch);
        return;
      }
      const auto patch_per_mesh_instance =
          (patch_count_per_instance - 1u) / patch_per_group + 1u;
      if (uint64_t(patch_per_mesh_instance) * packet.instance_count >
          common.max_object_threadgroups) {
        WARN("D3D12CommandQueue: omitted tessellation draw because of too many object threadgroups patch_groups=",
             patch_per_mesh_instance, " instance_count=", packet.instance_count);
        return;
      }

      const auto draw_arguments_offset =
          AllocateArgumentBuffer(argbuf_offset, sizeof(DXMT_DRAW_ARGUMENTS));
      auto *draw_argument =
          enc.getMappedArgumentBuffer<DXMT_DRAW_ARGUMENTS>(
              draw_arguments_offset);
      if (!draw_argument)
        return;
      draw_argument->StartVertex = packet.vertex_start;
      draw_argument->VertexCount = packet.vertex_count;
      draw_argument->InstanceCount = packet.instance_count;
      draw_argument->StartInstance = packet.base_instance;

      enc.resolveRenderPassBarrier();
      auto &draw =
          enc.encodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw>();
      draw.type = WMTRenderCommandDXMTTessellationMeshDraw;
      draw.draw_arguments_offset =
          enc.getFinalArgumentBufferOffset(draw_arguments_offset);
      draw.instance_count = packet.instance_count;
      draw.threads_per_patch = common.tess_threads_per_patch;
      draw.patch_per_group = patch_per_group;
      draw.patch_per_mesh_instance = patch_per_mesh_instance;
    } else if (common.use_geometry) {
      auto *render_encoder = enc.currentRenderEncoder();
      render_encoder->use_geometry = 1;
      if (!common.geometry_counts) {
        WARN("D3D12CommandQueue: geometry draw skipped because geometry metadata is invalid");
        return;
      }
      const auto [vertex_per_warp, vertex_increment_per_warp] =
          *common.geometry_counts;
      const auto draw_arguments_offset =
          AllocateArgumentBuffer(argbuf_offset, sizeof(DXMT_DRAW_ARGUMENTS));
      auto *draw_argument =
          enc.getMappedArgumentBuffer<DXMT_DRAW_ARGUMENTS>(
              draw_arguments_offset);
      if (!draw_argument)
        return;
      draw_argument->StartVertex = packet.vertex_start;
      draw_argument->VertexCount = packet.vertex_count;
      draw_argument->InstanceCount = packet.instance_count;
      draw_argument->StartInstance = packet.base_instance;

      const auto warp_count =
          (packet.vertex_count - 1) / vertex_increment_per_warp + 1;
      if (uint64_t(warp_count) * packet.instance_count >
          common.max_object_threadgroups) {
        WARN("D3D12CommandQueue: omitted geometry draw because of too many object threadgroups warp_count=",
             warp_count, " instance_count=", packet.instance_count);
      } else {
        enc.resolveRenderPassBarrier();
        auto &draw =
            enc.encodeRenderCommand<wmtcmd_render_dxmt_geometry_draw>();
        draw.type = WMTRenderCommandDXMTGeometryDraw;
        draw.draw_arguments_offset =
            enc.getFinalArgumentBufferOffset(draw_arguments_offset);
        draw.instance_count = packet.instance_count;
        draw.warp_count = warp_count;
        draw.vertex_per_warp = vertex_per_warp;
      }
    } else {
      if (!common.primitive) {
        WARN("D3D12CommandQueue: draw skipped because primitive topology is unavailable");
        return;
      }
      const auto primitive_type = *common.primitive;
      enc.resolveRenderPassBarrier();
      auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw>();
      draw.type = WMTRenderCommandDraw;
      draw.primitive_type = primitive_type;
      draw.vertex_start = packet.vertex_start;
      draw.vertex_count = packet.vertex_count;
      draw.instance_count = packet.instance_count;
      draw.base_instance = packet.base_instance;
    }
    common.bindless_diag.binding_generation = common.binding_generation;
    common.bindless_diag.descriptor_revision =
        common.descriptor_content_revision;
    common.bindless_diag.binding_fingerprint =
        common.binding_content_fingerprint;
    RecordBindlessMirrorDiagDraw(queue.CurrentFrameSeq(), common.pipeline,
                                 common.bindless_diag);
  }

  {
    dxmt::perf::ScopedFrameDuration visibility_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_visibility_interval);
    EndReplayDrawVisibilityQuery(enc, active_visibility_query);
  }
}

void EncodeReplayDrawIndexedInstancedBody(
    ArgumentEncodingContext &enc, dxmt::CommandQueue &queue,
    ReplayDrawIndexedInstancedPacket &packet, uint64_t &argbuf_offset,
    dxmt::FrameStatistics *perf_stats) {
  auto &common = packet.common;
  Rc<VisibilityResultQuery> active_visibility_query;
  {
    dxmt::perf::ScopedFrameDuration visibility_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_visibility_interval);
    active_visibility_query =
        BeginReplayDrawVisibilityQuery(enc, common.visibility_query);
  }

  {
    dxmt::perf::ScopedFrameDuration body_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_body_interval);
    if (common.use_tessellation) {
      auto *render_encoder = enc.currentRenderEncoder();
      render_encoder->use_tessellation = 1;
      enc.tess_num_output_control_point_element =
          common.tess_num_output_control_point_element;
      enc.tess_threads_per_patch = common.tess_threads_per_patch;
      if (!common.tess_threads_per_patch || !common.control_point_count) {
        WARN("D3D12CommandQueue: tessellation indexed draw skipped because tessellation metadata is invalid");
        return;
      }

      const auto patch_count_per_instance =
          packet.index_count / *common.control_point_count;
      if (!patch_count_per_instance)
        return;
      const auto patch_per_group = 32u / common.tess_threads_per_patch;
      if (!patch_per_group) {
        WARN("D3D12CommandQueue: tessellation indexed draw skipped because threads-per-patch is unsupported value=",
             common.tess_threads_per_patch);
        return;
      }
      const auto patch_per_mesh_instance =
          (patch_count_per_instance - 1u) / patch_per_group + 1u;
      if (uint64_t(patch_per_mesh_instance) * packet.instance_count >
          common.max_object_threadgroups) {
        WARN("D3D12CommandQueue: omitted tessellation indexed draw because of too many object threadgroups patch_groups=",
             patch_per_mesh_instance, " instance_count=", packet.instance_count);
        return;
      }

      const auto draw_arguments_offset = AllocateArgumentBuffer(
          argbuf_offset, sizeof(DXMT_DRAW_INDEXED_ARGUMENTS));
      auto *draw_argument =
          enc.getMappedArgumentBuffer<DXMT_DRAW_INDEXED_ARGUMENTS>(
              draw_arguments_offset);
      if (!draw_argument)
        return;
      draw_argument->BaseVertex = packet.base_vertex;
      draw_argument->IndexCount = packet.index_count;
      draw_argument->StartIndex = packet.start_index;
      draw_argument->InstanceCount = packet.instance_count;
      draw_argument->StartInstance = packet.base_instance;

      enc.resolveRenderPassBarrier();
      auto &draw = enc.encodeRenderCommand<
          wmtcmd_render_dxmt_tessellation_mesh_draw_indexed>();
      draw.type = WMTRenderCommandDXMTTessellationMeshDrawIndexed;
      draw.draw_arguments_offset =
          enc.getFinalArgumentBufferOffset(draw_arguments_offset);
      draw.index_buffer = packet.index_allocation->buffer();
      draw.index_buffer_offset = packet.index_binding_offset;
      draw.instance_count = packet.instance_count;
      draw.threads_per_patch = common.tess_threads_per_patch;
      draw.patch_per_group = patch_per_group;
      draw.patch_per_mesh_instance = patch_per_mesh_instance;
    } else if (common.use_geometry) {
      auto *render_encoder = enc.currentRenderEncoder();
      render_encoder->use_geometry = 1;
      if (!common.geometry_counts) {
        WARN("D3D12CommandQueue: geometry indexed draw skipped because geometry metadata is invalid");
        return;
      }
      const auto [vertex_per_warp, vertex_increment_per_warp] =
          *common.geometry_counts;
      const auto draw_arguments_offset = AllocateArgumentBuffer(
          argbuf_offset, sizeof(DXMT_DRAW_INDEXED_ARGUMENTS));
      auto *draw_argument =
          enc.getMappedArgumentBuffer<DXMT_DRAW_INDEXED_ARGUMENTS>(
              draw_arguments_offset);
      if (!draw_argument)
        return;
      draw_argument->BaseVertex = packet.base_vertex;
      draw_argument->IndexCount = packet.index_count;
      draw_argument->StartIndex = packet.start_index;
      draw_argument->InstanceCount = packet.instance_count;
      draw_argument->StartInstance = packet.base_instance;

      const auto warp_count =
          (packet.index_count - 1) / vertex_increment_per_warp + 1;
      if (uint64_t(warp_count) * packet.instance_count >
          common.max_object_threadgroups) {
        WARN("D3D12CommandQueue: omitted geometry indexed draw because of too many object threadgroups warp_count=",
             warp_count, " instance_count=", packet.instance_count);
      } else {
        enc.resolveRenderPassBarrier();
        auto &draw =
            enc.encodeRenderCommand<wmtcmd_render_dxmt_geometry_draw_indexed>();
        draw.type = WMTRenderCommandDXMTGeometryDrawIndexed;
        draw.draw_arguments_offset =
            enc.getFinalArgumentBufferOffset(draw_arguments_offset);
        draw.instance_count = packet.instance_count;
        draw.warp_count = warp_count;
        draw.vertex_per_warp = vertex_per_warp;
        draw.index_buffer = packet.index_allocation->buffer();
        draw.index_buffer_offset = packet.index_binding_offset;
      }
    } else {
      if (!common.primitive) {
        WARN("D3D12CommandQueue: indexed draw skipped because primitive topology is unavailable");
        return;
      }
      const auto primitive_type = *common.primitive;
      enc.resolveRenderPassBarrier();
      auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw_indexed>();
      draw.type = WMTRenderCommandDrawIndexed;
      draw.primitive_type = primitive_type;
      draw.index_type = packet.index_type;
      draw.index_count = packet.index_count;
      draw.index_buffer = packet.index_allocation->buffer();
      draw.index_buffer_offset = packet.index_offset;
      draw.instance_count = packet.instance_count;
      draw.base_vertex = packet.base_vertex;
      draw.base_instance = packet.base_instance;
    }
    common.bindless_diag.binding_generation = common.binding_generation;
    common.bindless_diag.descriptor_revision =
        common.descriptor_content_revision;
    common.bindless_diag.binding_fingerprint =
        common.binding_content_fingerprint;
    RecordBindlessMirrorDiagDraw(queue.CurrentFrameSeq(), common.pipeline,
                                 common.bindless_diag);
  }

  {
    dxmt::perf::ScopedFrameDuration visibility_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_visibility_interval);
    EndReplayDrawVisibilityQuery(enc, active_visibility_query);
  }
}

void EncodeReplayDrawIndirectCompiledBody(
    ArgumentEncodingContext &enc, ReplayDrawIndirectCompiledPacket &packet) {
  // ResolveCompiledGraphicsMetalPipeline only reports
  // CompiledCommandFallbackReason::None once it has engaged `plan.primitive`
  // (d3d12_compiled_graphics_emit_plan.cpp:100-102), and the compiled replay
  // bails out on any other reason before it ever queues an indirect command
  // (d3d12_command_queue_execute.inc:738-741 into
  // QueueCompiledGraphicsIndirectCommands, which copies the plan value at
  // d3d12_command_queue_execute.inc:399). None of that is visible from here:
  // the guarantee is expressed as an enum return value in a different
  // translation unit, and the packet is parked in a queued command before this
  // body runs. Re-check it once, exactly like the direct compiled draw bodies
  // above already do.
  if (!packet.common.primitive) {
    WARN("D3D12CommandQueue: compiled indirect draw skipped because primitive topology is unavailable");
    return;
  }
  const auto primitive_type = *packet.common.primitive;
  auto [argument_allocation, argument_sub_offset] =
      enc.access<PipelineStage::Vertex>(
          packet.argument_buffer, packet.argument_offset,
          packet.argument_size, ResourceAccess::Read);
  const auto metal_argument_offset =
      argument_sub_offset + packet.argument_offset;
  enc.resolveRenderPassBarrier();
  if (packet.indexed) {
    if (!packet.index_allocation)
      return;
    enc.retainResource(packet.index_allocation->buffer());
    auto &draw = enc.encodeRenderCommand<
        wmtcmd_render_draw_indexed_indirect>();
    draw.type = WMTRenderCommandDrawIndexedIndirect;
    draw.primitive_type = primitive_type;
    draw.index_type = packet.index_type;
    draw.index_buffer = packet.index_allocation->buffer();
    draw.index_buffer_offset = packet.index_buffer_offset;
    draw.indirect_args_buffer = argument_allocation->buffer();
    draw.indirect_args_offset = metal_argument_offset;
  } else {
    auto &draw =
        enc.encodeRenderCommand<wmtcmd_render_draw_indirect>();
    draw.type = WMTRenderCommandDrawIndirect;
    draw.primitive_type = primitive_type;
    draw.indirect_args_buffer = argument_allocation->buffer();
    draw.indirect_args_offset = metal_argument_offset;
  }
}

} // namespace dxmt::d3d12
