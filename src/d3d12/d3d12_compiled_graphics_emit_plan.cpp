#include "d3d12_compiled_graphics_emit_plan.hpp"

#include "d3d12_argument_buffer_layout.hpp"
#include "d3d12_compiled_binding_encode.hpp"
#include "d3d12_compiled_binding_tables.hpp"
#include "d3d12_compiled_native_recipe.hpp"
#include "d3d12_compiled_snapshot_access.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_native_stage_descriptor_diag.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_render_pass_attachments.hpp"
#include "d3d12_snapshot_binding_query.hpp"

#include "dxmt_perf_stats.hpp"

#include <atomic>
#include <span>
#include <utility>

namespace dxmt::d3d12 {

namespace {

// The CommandQueueImpl overload this diagnostic used to call repeated the
// dense-diagnostic gate ahead of the memoized plan lookup so the lookup stays
// off the hot path when dense correctness checking is disabled. Keep both the
// gate and its placement here.
void DiagnoseStageDescriptorsCached(
    SubmissionStagePlanCache<NativeRootBaseStagePlan> &cache,
    const std::vector<CompiledCommandRootDescriptorTable> &tables,
    const PipelineState &pipeline, const RootSignature &root,
    PipelineStage stage, bool compute, const char *kind, uint64_t frame,
    uint64_t sequence, uint64_t record_serial, obj_handle_t metal_pso) {
  if (!D3D12DiagCorrectnessDenseEnabled())
    return;

  const auto plan =
      GetNativeRootBaseStagePlan(cache, pipeline, root, stage, compute);
  DiagnoseCompiledNativeStageDescriptors(tables, pipeline, plan.get(), stage,
                                         kind, frame, sequence, record_serial,
                                         metal_pso);
}

} // namespace

CompiledCommandFallbackReason ResolveCompiledGraphicsMetalPipeline(
    const CompiledGraphicsPacket &packet,
    const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
    DirectIndirectOperation indirect_operation,
    CompiledGraphicsPacketPlan &plan) {
  const bool native_packet = plan.native_packet;
  auto *pipeline = plan.pipeline;
  auto *&metal = plan.metal;
  auto &metal_pso = plan.metal_pso;
  auto &primitive = plan.primitive;
  auto &indexed = plan.indexed;
  const auto [compiled_demote_msaa_srv_mask_lo,
              compiled_demote_msaa_srv_mask_hi] =
      submitted_snapshot
          ? PixelShaderSingleSampleMsaaSRVDemoteMask(
                *submitted_snapshot, *pipeline)
          : std::pair<uint64_t, uint64_t>{0, 0};
  plan.compiled_demote_msaa_srv_mask_lo = compiled_demote_msaa_srv_mask_lo;
  plan.compiled_demote_msaa_srv_mask_hi = compiled_demote_msaa_srv_mask_hi;
  metal = packet.pipeline.metadata.metal_graphics;
  if (!metal ||
      metal->pixel_shader_demote_msaa_srv_mask_lo !=
          compiled_demote_msaa_srv_mask_lo ||
      metal->pixel_shader_demote_msaa_srv_mask_hi !=
          compiled_demote_msaa_srv_mask_hi) {
    metal = pipeline->GetMetalGraphicsState(
        compiled_demote_msaa_srv_mask_lo,
        compiled_demote_msaa_srv_mask_hi);
  }
  if (!metal || !metal->pso)
    return CompiledCommandFallbackReason::MissingPipelineState;
  if (metal->use_geometry)
    return native_packet
               ? CompiledCommandFallbackReason::NativeUnsupportedGeometryPipeline
               : CompiledCommandFallbackReason::GeometryPipeline;
  if (metal->use_tessellation)
    return native_packet
               ? CompiledCommandFallbackReason::NativeUnsupportedTessellationPipeline
               : CompiledCommandFallbackReason::TessellationPipeline;
  if (packet.render_state.topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED)
    return CompiledCommandFallbackReason::UnsupportedVertexIndexState;
  indexed =
      packet.draw_indexed ||
      indirect_operation == DirectIndirectOperation::DrawIndexed;
  metal_pso =
      indexed && packet.input_assembler.index_buffer
          ? SelectGraphicsPipelineState(
                *metal, packet.render_state.topology,
                packet.input_assembler.index_buffer->Format)
          : SelectGraphicsPipelineState(*metal,
                                        packet.render_state.topology);
  if (!metal_pso)
    return CompiledCommandFallbackReason::MissingPipelineState;
  primitive = GetPrimitiveType(packet.render_state.topology);
  if (!primitive)
    return CompiledCommandFallbackReason::UnsupportedVertexIndexState;
  return CompiledCommandFallbackReason::None;
}

void DiagnoseCompiledGraphicsPacket(
    CompiledReplayContext &ctx,
    SubmissionStagePlanCache<NativeRootBaseStagePlan> &native_stage_plan_cache,
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
    const std::vector<CompiledCommandRootDescriptorTable>
        &submitted_root_tables,
    const ExecuteIndirectRecord *indirect,
    const CompiledGraphicsPacketPlan &plan) {
  auto &queue = ctx.queue;
  auto &replay_record_serial = ctx.replay_record_serial;
  const bool native_packet = plan.native_packet;
  const bool indexed = plan.indexed;
  auto *pipeline = plan.pipeline;
  auto *root = plan.root;
  const auto &metal_pso = plan.metal_pso;
  D3D12DiagLogCompiledTargetInputs(
      packet, prepared, *pipeline, queue.CurrentFrameSeq(),
      replay_record_serial);

  if (native_packet) {
    D3D12DiagLogNativePacket(
        indirect ? (indexed ? "draw-indexed-indirect"
                            : "draw-indirect")
                 : packet.draw ? "draw" : "draw-indexed",
        queue.CurrentFrameSeq(), packet.d3d_sequence,
        replay_record_serial, *pipeline, metal_pso.handle,
        submitted_root_tables,
        {{"vertex", &submitted_snapshot->frozen_native_vertex},
         {"pixel", &submitted_snapshot->frozen_native_pixel}});
    DiagnoseStageDescriptorsCached(
        native_stage_plan_cache, submitted_root_tables, *pipeline, *root,
        PipelineStage::Vertex, false,
        indirect ? (indexed ? "draw-indexed-indirect"
                            : "draw-indirect")
                 : packet.draw ? "draw" : "draw-indexed",
        queue.CurrentFrameSeq(), packet.d3d_sequence,
        replay_record_serial, metal_pso.handle);
    DiagnoseStageDescriptorsCached(
        native_stage_plan_cache, submitted_root_tables, *pipeline, *root,
        PipelineStage::Pixel, false,
        indirect ? (indexed ? "draw-indexed-indirect"
                            : "draw-indirect")
                 : packet.draw ? "draw" : "draw-indexed",
        queue.CurrentFrameSeq(), packet.d3d_sequence,
        replay_record_serial, metal_pso.handle);
  }
}

void BuildCompiledGraphicsPacketBindingPlan(
    CompiledReplayContext &ctx, WMT::Device device,
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
    const std::vector<CompiledCommandRootDescriptorTable>
        &submitted_root_tables,
    CompiledGraphicsPacketPlan &plan) {
  auto &queue = ctx.queue;
  auto *test_telemetry = ctx.test_telemetry;
  auto &graphics_native_binding_recipes = ctx.graphics_native_binding_recipes;
  const bool native_packet = plan.native_packet;
  auto *pipeline = plan.pipeline;
  auto *root = plan.root;
  const auto &frozen_native = plan.frozen_native;
  const auto &vertex_binding_recipe = packet.vertex_binding_recipe;
  auto &binding_state = plan.binding_state;
  auto &direct_access = plan.direct_access;
  auto &argument_buffer_size = plan.argument_buffer_size;
  auto &bindless_snapshot = plan.bindless_snapshot;
  auto &native_binding_recipe = plan.native_binding_recipe;
  auto &binding_fingerprint = plan.binding_fingerprint;
  auto &submitted_descriptor_revision = plan.submitted_descriptor_revision;
  FillCompiledBindingState(binding_state, packet.root_constants,
                           packet.root_descriptors,
                           submitted_snapshot.get());
  RecordCompiledDescriptorBackendStats(
      dxmt::perf::enabled() ? &queue.CurrentFrameStatistics() : nullptr,
      submitted_root_tables);
  direct_access.static_buffer_allocations =
      packet.direct_buffer_allocations;
  AddCompiledSnapshotEncoderAccesses(device, direct_access,
                                     submitted_snapshot.get());
  argument_buffer_size =
      EstimateGraphicsArgumentBufferSize(*pipeline, false, false);
  bindless_snapshot =
      native_packet
          ? std::shared_ptr<GraphicsBindingSnapshot>{}
          : submitted_snapshot;
  if (native_packet) {
    const auto &native_vertex =
        submitted_snapshot->frozen_native_vertex;
    const auto &native_pixel =
        submitted_snapshot->frozen_native_pixel;
    const NativeBindingRecipeKey recipe_key = {
        packet.binding_program.get(), submitted_snapshot.get()};
    if (const auto cached =
            graphics_native_binding_recipes.find(recipe_key);
        cached != graphics_native_binding_recipes.end()) {
      native_binding_recipe = cached->second;
      if (test_telemetry)
        test_telemetry->submitted_native_binding_recipe_reuses
            .fetch_add(1, std::memory_order_relaxed);
    } else {
      native_binding_recipe = BuildCompiledNativeBindingRecipe(
          *pipeline, false, *frozen_native,
          {{PipelineStage::Vertex, &native_vertex},
           {PipelineStage::Pixel, &native_pixel}});
      graphics_native_binding_recipes.emplace(
          recipe_key, native_binding_recipe);
    }
  }
  binding_fingerprint =
      BuildCompiledDirectGraphicsBindingFingerprint(
          packet, prepared, pipeline, root,
          vertex_binding_recipe.get(), bindless_snapshot.get());
  submitted_descriptor_revision =
      submitted_snapshot
          ? submitted_snapshot->descriptor_content_revision
          : dxmt::DescriptorContentRevision{
                binding_fingerprint, binding_fingerprint};
}

void MaterializeActiveEncoderAttachments(
    CompiledReplayContext &ctx, WMT::Device device,
    const CompiledCommandSegment &segment,
    std::optional<ReplayRenderPassAttachments> &active_encoder_attachments) {
  const auto *compiled = ctx.compiled;
  auto *test_telemetry = ctx.test_telemetry;
  const CompiledCommandRenderState *first_render_state = nullptr;
  if (segment.graphics_packet_count) {
    first_render_state =
        &compiled->graphics_packets[segment.first_graphics_packet]
             .render_state;
  } else if (segment.kind == CompiledCommandSegmentKind::Indirect &&
             segment.indirect_packet_count) {
    const auto &indirect = compiled->indirect_packets[
        segment.first_indirect_packet];
    if (!indirect.compute &&
        indirect.state_packet_index <
            compiled->graphics_packets.size())
      first_render_state =
          &compiled->graphics_packets[indirect.state_packet_index]
               .render_state;
  }
  // Reserved-resource allocation and present-source tracking are
  // submission-dynamic, so materialize the immutable Close-time
  // attachment identity once per active encoder rather than once per
  // draw packet.
  if (first_render_state) {
    active_encoder_attachments.emplace(BuildRenderPassAttachments(
        device,
        std::span<const DescriptorRecord>{
            first_render_state->render_targets.data(),
            first_render_state->render_targets.size()},
        first_render_state->depth_stencil
            ? &*first_render_state->depth_stencil
            : nullptr,
        nullptr));
    if (test_telemetry)
      test_telemetry->encoder_attachment_materializations.fetch_add(
          1, std::memory_order_relaxed);
  }
}

} // namespace dxmt::d3d12
