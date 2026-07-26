#include "d3d12_indirect_command_encode.hpp"

#include "d3d12_indirect_encoding.hpp"
#include "d3d12_indirect_topology.hpp"
#include "d3d12_render_encoder_state.hpp"
#include "d3d12_replay_binding_encode.hpp"
#include "log/log.hpp"

namespace dxmt::d3d12 {

void
EncodeIndirectDraw(const SubmissionBindingContext &ctx,
                   ArgumentEncodingContext &enc,
                   IndirectDrawEncodeParams &p, uint64_t &argbuf_offset) {
  // Bound to the params by reference so the body below stays exactly what it
  // was as a by-value lambda capture list.
  auto &metal_pso = p.metal_pso;
  const bool use_geometry = p.use_geometry;
  const bool use_tessellation = p.use_tessellation;
  const uint64_t demote_msaa_srv_mask_lo = p.demote_msaa_srv_mask_lo;
  const uint64_t demote_msaa_srv_mask_hi = p.demote_msaa_srv_mask_hi;
  auto &depth_stencil = p.depth_stencil;
  auto &rasterizer = p.rasterizer;
  const uint32_t tess_threads_per_patch = p.tess_threads_per_patch;
  const uint32_t tess_num_output_control_point_element =
      p.tess_num_output_control_point_element;
  auto *pipeline = p.pipeline;
  auto &replay_state = p.replay_state;
  auto &primitive = p.primitive;
  auto &geometry_counts = p.geometry_counts;
  auto &control_point_count = p.control_point_count;
  auto &blend_factor = p.blend_factor;
  const UINT stencil_ref = p.stencil_ref;
  auto &arg_buffer = p.arg_buffer;
  const UINT64 arg_offset = p.arg_offset;
  auto &count_buffer = p.count_buffer;
  auto &counted_args = p.counted_args;
  const UINT64 counted_offset = p.counted_offset;
  const UINT argument_size = p.argument_size;
  const uint32_t max_object_threadgroups = p.max_object_threadgroups;
  auto &viewports = p.viewports;
  auto &scissors = p.scissors;

  EncodeRenderPipelineStateIfChanged(enc, metal_pso);
  auto *render_encoder = enc.currentRenderEncoder();
  render_encoder->pixel_shader_demote_msaa_srv_mask_lo =
      demote_msaa_srv_mask_lo;
  render_encoder->pixel_shader_demote_msaa_srv_mask_hi =
      demote_msaa_srv_mask_hi;
  if (depth_stencil) {
    auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
    cmd.type = WMTRenderCommandSetDSSO;
    cmd.dsso = depth_stencil;
    cmd.stencil_ref = static_cast<uint8_t>(stencil_ref);
  }
  auto &rs = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
  rs = rasterizer;

  EncodeGraphicsBindings(ctx, enc, replay_state, *pipeline, use_geometry,
                         use_tessellation, argbuf_offset);
  if (enc.argumentBufferOverflowed())
    return;
  EncodeDynamicRenderState(enc, viewports, scissors, blend_factor,
                           stencil_ref);

  if (use_tessellation) {
    if (count_buffer.ptr()) {
      // TODO(d3d12): marshal counted tessellation indirect arguments.
      WARN("D3D12CommandQueue: counted tessellation indirect draw is unsupported");
      return;
    }
    if (!tess_threads_per_patch || !control_point_count) {
      WARN("D3D12CommandQueue: tessellation indirect draw skipped because tessellation metadata is invalid");
      return;
    }
    const auto patch_per_group = 32u / tess_threads_per_patch;
    if (!patch_per_group) {
      WARN("D3D12CommandQueue: tessellation indirect draw skipped because threads-per-patch is unsupported value=",
           tess_threads_per_patch);
      return;
    }
    auto *render_encoder = enc.currentRenderEncoder();
    render_encoder->use_tessellation = 1;
    enc.tess_num_output_control_point_element =
        tess_num_output_control_point_element;
    enc.tess_threads_per_patch = tess_threads_per_patch;
    auto [arg_allocation, arg_sub_offset] =
        enc.access<PipelineStage::Vertex>(arg_buffer, arg_offset,
                                          sizeof(DXMT_DRAW_ARGUMENTS),
                                          ResourceAccess::Read);
    auto dispatch_arg =
        enc.allocateTempBuffer1(sizeof(DXMT_DISPATCH_ARGUMENTS), 4);
    enc.encodeTSDispatchArgumentsMarshal(
        arg_allocation->buffer(),
        arg_allocation->gpuAddress() + arg_sub_offset + arg_offset, 0,
        *control_point_count, patch_per_group, dispatch_arg.gpu_buffer,
        dispatch_arg.gpu_address, dispatch_arg.offset,
        max_object_threadgroups);
    enc.resolveRenderPassBarrier();
    auto &draw = enc.encodeRenderCommand<
        wmtcmd_render_dxmt_tessellation_mesh_draw_indirect>();
    draw.type = WMTRenderCommandDXMTTessellationMeshDrawIndirect;
    draw.dispatch_args_buffer = dispatch_arg.gpu_buffer;
    draw.dispatch_args_offset = dispatch_arg.offset;
    draw.patch_per_group = patch_per_group;
    draw.threads_per_patch = tess_threads_per_patch;
    draw.indirect_args_buffer = arg_allocation->buffer();
    draw.indirect_args_offset = arg_sub_offset + arg_offset;
    draw.imm_draw_arguments = enc.getFinalArgumentBuffer();
  } else if (use_geometry) {
    if (count_buffer.ptr()) {
      WARN("D3D12CommandQueue: counted geometry indirect draw is unsupported");
      return;
    }
    // The queue only reaches this branch after LowerIndirectDrawTopology
    // engaged `geometry_counts` for a geometry PSO
    // (d3d12_indirect_encoding.cpp:233-241) and the caller dropped the draw
    // when it could not (d3d12_command_queue_indirect.inc:249-250). Naming
    // that chain in a comment is not enough: the correlation between
    // `use_geometry` and which of the three lowering optionals is engaged is
    // established in another translation unit, is carried across a
    // `IndirectDrawEncodeParams` copy, a lambda capture and a pass batch, and
    // is never re-expressed here, so a flow-sensitive reader of this function
    // -- human or clang-tidy -- has nothing to derive it from. Re-check it the
    // way the tessellation branch above already re-checks
    // `control_point_count`.
    if (!geometry_counts) {
      WARN("D3D12CommandQueue: geometry indirect draw skipped because geometry metadata is invalid");
      return;
    }
    auto *render_encoder = enc.currentRenderEncoder();
    render_encoder->use_geometry = 1;
    auto [arg_allocation, arg_sub_offset] =
        enc.access<PipelineStage::Vertex>(arg_buffer, arg_offset,
                                          sizeof(DXMT_DRAW_ARGUMENTS),
                                          ResourceAccess::Read);
    auto dispatch_arg =
        enc.allocateTempBuffer1(sizeof(DXMT_DISPATCH_ARGUMENTS), 4);
    auto [vertex_per_warp, vertex_increment_per_warp] = *geometry_counts;
    enc.encodeGSDispatchArgumentsMarshal(
        arg_allocation->buffer(),
        arg_allocation->gpuAddress() + arg_sub_offset + arg_offset, 0,
        vertex_increment_per_warp, dispatch_arg.gpu_buffer,
        dispatch_arg.gpu_address, dispatch_arg.offset,
        max_object_threadgroups);
    enc.resolveRenderPassBarrier();
    auto &draw =
        enc.encodeRenderCommand<wmtcmd_render_dxmt_geometry_draw_indirect>();
    draw.type = WMTRenderCommandDXMTGeometryDrawIndirect;
    draw.dispatch_args_buffer = dispatch_arg.gpu_buffer;
    draw.dispatch_args_offset = dispatch_arg.offset;
    draw.vertex_per_warp = vertex_per_warp;
    draw.indirect_args_buffer = arg_allocation->buffer();
    draw.indirect_args_offset = arg_sub_offset + arg_offset;
    draw.imm_draw_arguments = enc.getFinalArgumentBuffer();
  } else {
    // Same reasoning as the geometry branch above: `primitive` is the lowering
    // result reserved for the plain (non-geometry, non-tessellation) PSO
    // variant (d3d12_indirect_encoding.cpp:242-251), but the bools that select
    // this branch travel here separately from the optional they constrain.
    if (!primitive) {
      WARN("D3D12CommandQueue: indirect draw skipped because primitive topology is unavailable");
      return;
    }
    WMT::Buffer indirect_buffer;
    UINT64 indirect_offset;
    if (count_buffer.ptr()) {
      auto [counted_allocation, counted_sub_offset] =
          enc.access<PipelineStage::Vertex>(
              counted_args, counted_offset, argument_size,
              ResourceAccess::Read);
      indirect_buffer = counted_allocation->buffer();
      indirect_offset = counted_sub_offset + counted_offset;
    } else {
      auto [arg_allocation, arg_sub_offset] =
          enc.access<PipelineStage::Vertex>(arg_buffer, arg_offset,
                                            argument_size,
                                            ResourceAccess::Read);
      indirect_buffer = arg_allocation->buffer();
      indirect_offset = arg_sub_offset + arg_offset;
    }

    auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw_indirect>();
    draw.type = WMTRenderCommandDrawIndirect;
    draw.primitive_type = *primitive;
    draw.indirect_args_buffer = indirect_buffer;
    draw.indirect_args_offset = indirect_offset;
  }
}

void
EncodeIndirectDrawIndexed(const SubmissionBindingContext &ctx,
                          ArgumentEncodingContext &enc,
                          IndirectDrawEncodeParams &p,
                          uint64_t &argbuf_offset) {
  auto &metal_pso = p.metal_pso;
  const bool use_geometry = p.use_geometry;
  const bool use_tessellation = p.use_tessellation;
  const uint64_t demote_msaa_srv_mask_lo = p.demote_msaa_srv_mask_lo;
  const uint64_t demote_msaa_srv_mask_hi = p.demote_msaa_srv_mask_hi;
  auto &depth_stencil = p.depth_stencil;
  auto &rasterizer = p.rasterizer;
  const uint32_t tess_threads_per_patch = p.tess_threads_per_patch;
  const uint32_t tess_num_output_control_point_element =
      p.tess_num_output_control_point_element;
  auto *pipeline = p.pipeline;
  auto &replay_state = p.replay_state;
  auto &index_allocation = p.index_allocation;
  auto &primitive = p.primitive;
  auto &geometry_counts = p.geometry_counts;
  auto &control_point_count = p.control_point_count;
  const auto index_type = p.index_type;
  const UINT64 index_offset = p.index_offset;
  auto &blend_factor = p.blend_factor;
  const UINT stencil_ref = p.stencil_ref;
  auto &arg_buffer = p.arg_buffer;
  const UINT64 arg_offset = p.arg_offset;
  auto &count_buffer = p.count_buffer;
  auto &counted_args = p.counted_args;
  const UINT64 counted_offset = p.counted_offset;
  const UINT argument_size = p.argument_size;
  const uint32_t max_object_threadgroups = p.max_object_threadgroups;
  auto &viewports = p.viewports;
  auto &scissors = p.scissors;

  enc.retainAllocation(index_allocation.ptr());
  enc.retainResource(index_allocation->buffer());
  EncodeRenderPipelineStateIfChanged(enc, metal_pso);
  auto *render_encoder = enc.currentRenderEncoder();
  render_encoder->pixel_shader_demote_msaa_srv_mask_lo =
      demote_msaa_srv_mask_lo;
  render_encoder->pixel_shader_demote_msaa_srv_mask_hi =
      demote_msaa_srv_mask_hi;
  if (depth_stencil) {
    auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
    cmd.type = WMTRenderCommandSetDSSO;
    cmd.dsso = depth_stencil;
    cmd.stencil_ref = static_cast<uint8_t>(stencil_ref);
  }
  auto &rs = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
  rs = rasterizer;

  EncodeGraphicsBindings(ctx, enc, replay_state, *pipeline, use_geometry,
                         use_tessellation, argbuf_offset);
  if (enc.argumentBufferOverflowed())
    return;
  EncodeDynamicRenderState(enc, viewports, scissors, blend_factor,
                           stencil_ref);

  if (use_tessellation) {
    if (count_buffer.ptr()) {
      // TODO(d3d12): marshal counted tessellation indirect indexed arguments.
      WARN("D3D12CommandQueue: counted tessellation indirect indexed draw is unsupported");
      return;
    }
    if (!tess_threads_per_patch || !control_point_count) {
      WARN("D3D12CommandQueue: tessellation indirect indexed draw skipped because tessellation metadata is invalid");
      return;
    }
    const auto patch_per_group = 32u / tess_threads_per_patch;
    if (!patch_per_group) {
      WARN("D3D12CommandQueue: tessellation indirect indexed draw skipped because threads-per-patch is unsupported value=",
           tess_threads_per_patch);
      return;
    }
    auto *render_encoder = enc.currentRenderEncoder();
    render_encoder->use_tessellation = 1;
    enc.tess_num_output_control_point_element =
        tess_num_output_control_point_element;
    enc.tess_threads_per_patch = tess_threads_per_patch;
    auto [arg_allocation, arg_sub_offset] =
        enc.access<PipelineStage::Vertex>(
            arg_buffer, arg_offset, sizeof(DXMT_DRAW_INDEXED_ARGUMENTS),
            ResourceAccess::Read);
    auto dispatch_arg =
        enc.allocateTempBuffer1(sizeof(DXMT_DISPATCH_ARGUMENTS), 4);
    enc.encodeTSDispatchArgumentsMarshal(
        arg_allocation->buffer(),
        arg_allocation->gpuAddress() + arg_sub_offset + arg_offset, 0,
        *control_point_count, patch_per_group, dispatch_arg.gpu_buffer,
        dispatch_arg.gpu_address, dispatch_arg.offset,
        max_object_threadgroups);
    enc.resolveRenderPassBarrier();
    auto &draw = enc.encodeRenderCommand<
        wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect>();
    draw.type = WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect;
    draw.dispatch_args_buffer = dispatch_arg.gpu_buffer;
    draw.dispatch_args_offset = dispatch_arg.offset;
    draw.patch_per_group = patch_per_group;
    draw.threads_per_patch = tess_threads_per_patch;
    draw.indirect_args_buffer = arg_allocation->buffer();
    draw.indirect_args_offset = arg_sub_offset + arg_offset;
    draw.index_buffer = index_allocation->buffer();
    draw.index_buffer_offset = index_offset;
    draw.imm_draw_arguments = enc.getFinalArgumentBuffer();
  } else if (use_geometry) {
    if (count_buffer.ptr()) {
      WARN("D3D12CommandQueue: counted geometry indirect indexed draw is unsupported");
      return;
    }
    // See EncodeIndirectDraw for why the `use_geometry` bool alone does not
    // make `geometry_counts` observably engaged at this point.
    if (!geometry_counts) {
      WARN("D3D12CommandQueue: geometry indirect indexed draw skipped because geometry metadata is invalid");
      return;
    }
    auto *render_encoder = enc.currentRenderEncoder();
    render_encoder->use_geometry = 1;
    auto [arg_allocation, arg_sub_offset] =
        enc.access<PipelineStage::Vertex>(
            arg_buffer, arg_offset, sizeof(DXMT_DRAW_INDEXED_ARGUMENTS),
            ResourceAccess::Read);
    auto dispatch_arg =
        enc.allocateTempBuffer1(sizeof(DXMT_DISPATCH_ARGUMENTS), 4);
    auto [vertex_per_warp, vertex_increment_per_warp] = *geometry_counts;
    enc.encodeGSDispatchArgumentsMarshal(
        arg_allocation->buffer(),
        arg_allocation->gpuAddress() + arg_sub_offset + arg_offset, 0,
        vertex_increment_per_warp, dispatch_arg.gpu_buffer,
        dispatch_arg.gpu_address, dispatch_arg.offset,
        max_object_threadgroups);
    enc.resolveRenderPassBarrier();
    auto &draw = enc.encodeRenderCommand<
        wmtcmd_render_dxmt_geometry_draw_indexed_indirect>();
    draw.type = WMTRenderCommandDXMTGeometryDrawIndexedIndirect;
    draw.dispatch_args_buffer = dispatch_arg.gpu_buffer;
    draw.dispatch_args_offset = dispatch_arg.offset;
    draw.vertex_per_warp = vertex_per_warp;
    draw.indirect_args_buffer = arg_allocation->buffer();
    draw.indirect_args_offset = arg_sub_offset + arg_offset;
    draw.index_buffer = index_allocation->buffer();
    draw.index_buffer_offset = index_offset;
    draw.imm_draw_arguments = enc.getFinalArgumentBuffer();
  } else {
    // See EncodeIndirectDraw for why the branch condition alone does not make
    // `primitive` observably engaged at this point.
    if (!primitive) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped because primitive topology is unavailable");
      return;
    }
    WMT::Buffer indirect_buffer;
    UINT64 indirect_offset;
    if (count_buffer.ptr()) {
      auto [counted_allocation, counted_sub_offset] =
          enc.access<PipelineStage::Vertex>(
              counted_args, counted_offset, argument_size,
              ResourceAccess::Read);
      indirect_buffer = counted_allocation->buffer();
      indirect_offset = counted_sub_offset + counted_offset;
    } else {
      auto [arg_allocation, arg_sub_offset] =
          enc.access<PipelineStage::Vertex>(arg_buffer, arg_offset,
                                            argument_size,
                                            ResourceAccess::Read);
      indirect_buffer = arg_allocation->buffer();
      indirect_offset = arg_sub_offset + arg_offset;
    }

    auto &draw =
        enc.encodeRenderCommand<wmtcmd_render_draw_indexed_indirect>();
    draw.type = WMTRenderCommandDrawIndexedIndirect;
    draw.primitive_type = *primitive;
    draw.index_type = index_type;
    draw.index_buffer = index_allocation->buffer();
    draw.index_buffer_offset = index_offset;
    draw.indirect_args_buffer = indirect_buffer;
    draw.indirect_args_offset = indirect_offset;
  }
}

void
EncodeIndirectDispatch(const SubmissionBindingContext &ctx,
                       ArgumentEncodingContext &enc,
                       IndirectDispatchEncodeParams &p,
                       uint64_t &argbuf_offset) {
  auto &metal_pso = p.metal_pso;
  const auto threadgroup_size = p.threadgroup_size;
  auto *pipeline = p.pipeline;
  auto &replay_state = p.replay_state;
  const uint64_t argument_buffer_size = p.argument_buffer_size;
  auto &arg_buffer = p.arg_buffer;
  const UINT64 arg_offset = p.arg_offset;
  auto &count_buffer = p.count_buffer;
  auto &counted_args = p.counted_args;
  const UINT64 counted_offset = p.counted_offset;
  const UINT argument_size = p.argument_size;
  const UINT command_index = p.command_index;

  auto &set_pso = enc.encodeComputeCommand<wmtcmd_compute_setpso>();
  set_pso.type = WMTComputeCommandSetPSO;
  set_pso.pso = metal_pso;
  set_pso.threadgroup_size = threadgroup_size;

  const uint64_t argbuf_base = argbuf_offset;
  EncodeComputeBindings(ctx, enc, replay_state, *pipeline, argbuf_offset);
  if (enc.argumentBufferOverflowed())
    return;
  if (argbuf_offset - argbuf_base > argument_buffer_size) {
    WARN("D3D12CommandQueue: compute argument buffer estimate was too small estimated=",
         argument_buffer_size, " actual=", argbuf_offset - argbuf_base);
  }

  WMT::Buffer indirect_buffer;
  UINT64 indirect_offset;
  if (count_buffer.ptr()) {
    auto [counted_allocation, counted_sub_offset] =
        enc.access<PipelineStage::Compute>(
            counted_args, counted_offset, argument_size,
            ResourceAccess::Read);
    indirect_buffer = counted_allocation->buffer();
    indirect_offset = counted_sub_offset + counted_offset;
  } else {
    auto [arg_allocation, arg_sub_offset] =
        enc.access<PipelineStage::Compute>(arg_buffer, arg_offset,
                                           argument_size,
                                           ResourceAccess::Read);
    indirect_buffer = arg_allocation->buffer();
    indirect_offset = arg_sub_offset + arg_offset;
  }

  LogDispatchIndirectEncode(
      command_index, count_buffer.ptr() != nullptr, arg_offset,
      argument_size, counted_offset,
      static_cast<obj_handle_t>(indirect_buffer), indirect_offset,
      static_cast<obj_handle_t>(metal_pso), threadgroup_size);

  auto &dispatch =
      enc.encodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
  dispatch.type = WMTComputeCommandDispatchIndirect;
  dispatch.indirect_args_buffer = indirect_buffer;
  dispatch.indirect_args_offset = indirect_offset;
}

} // namespace dxmt::d3d12
