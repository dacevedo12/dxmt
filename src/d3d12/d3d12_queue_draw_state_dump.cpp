#include "d3d12_queue_draw_state_dump.hpp"

#include "d3d12_binding_debug_log.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_descriptor_diagnostics.hpp"
#include "d3d12_draw_state_audit.hpp"
#include "d3d12_indirect_topology.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_render_state.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>
#include <string>

namespace dxmt::d3d12 {

namespace {

// A D3D12 graphics pipeline description carries at most eight render targets
// and the input assembler at most 32 vertex buffer slots; the report walks
// both spaces exhaustively.
constexpr UINT kDrawStateMaxRenderTargets = 8;
constexpr UINT kDrawStateInputSlotCount = 32;

} // namespace

void
LogReplayDrawState(WMT::Device device, const char *kind,
                   const ReplayState &state, PipelineState &pipeline,
                   const PipelineMetalGraphicsState &metal,
                   const ReplayRenderPassAttachments &attachments,
                   const std::vector<D3D12_VIEWPORT> &viewports,
                   const std::vector<D3D12_RECT> &scissors,
                   const DrawInstancedRecord *draw,
                   const DrawIndexedInstancedRecord *indexed_draw,
                   UINT64 index_resource_offset, UINT64 index_offset) {
  static std::atomic<uint32_t> log_count = 0;
  static std::atomic<uint32_t> target_log_count = 0;

  const auto *graphics = pipeline.GetGraphicsState();
  const auto &desc = graphics->desc;
  const auto slot_mask = InputSlotMask(graphics);
  const auto &cache_key = pipeline.GetShaderCacheKey();
  const bool target_shader =
      D3D12DiagShaderFilterConfigured() &&
      (D3D12DiagPipelineStageSelected(pipeline, PipelineStage::Vertex) ||
       D3D12DiagPipelineStageSelected(pipeline, PipelineStage::Pixel) ||
       D3D12DiagPipelineStageSelected(pipeline, PipelineStage::Geometry) ||
       D3D12DiagPipelineStageSelected(pipeline, PipelineStage::Hull) ||
       D3D12DiagPipelineStageSelected(pipeline, PipelineStage::Domain));
  const bool target_pso =
      DiagIsTargetCompositePso(cache_key) || target_shader;
  const auto target_occurrence =
      target_pso && D3D12DiagDrawStateEnabled()
          ? target_log_count.fetch_add(1, std::memory_order_relaxed) + 1
          : 0;
  const bool sample_target = IsSampledTargetOccurrence(target_occurrence);
  if (target_pso) {
    if (!sample_target)
      return;
  } else if (!D3D12DiagShouldLog(log_count,
                                 D3D12DiagDrawStateEnabled())) {
    return;
  }

  const std::string key_prefix = ShaderCacheKeyPrefix(cache_key);
  const auto d3d_sequence = DiagCurrentReplayRecordSequence();
  const auto record_serial = DiagCurrentReplayRecordSerial();
  const bool color0_write =
      desc.NumRenderTargets &&
      desc.BlendState.RenderTarget[0].RenderTargetWriteMask != 0;

  DrawStateAnomaly anomaly;

  for (UINT slot = 0;
       slot < desc.NumRenderTargets && slot < kDrawStateMaxRenderTargets;
       slot++) {
    const auto &blend = desc.BlendState.RenderTarget[
        desc.BlendState.IndependentBlendEnable ? slot : 0];
    if (!blend.RenderTargetWriteMask)
      continue;
    const auto attachment = std::find_if(
        attachments.colors.begin(), attachments.colors.end(),
        [slot](const auto &color) { return color.slot == slot; });
    if (attachment == attachments.colors.end()) {
      anomaly.Add(DrawStateAnomaly::kMissingColorAttachment,
                  "writable-render-target-not-attached");
    } else {
      MTL_DXGI_FORMAT_DESC expected_format = {};
      if (FAILED(MTLQueryDXGIFormat(device, desc.RTVFormats[slot],
                                    expected_format)) ||
          attachment->format != expected_format.PixelFormat) {
        anomaly.Add(DrawStateAnomaly::kColorFormatMismatch,
                    "pipeline-rtv-format-mismatch");
      }
    }
  }
  if (desc.DepthStencilState.DepthEnable &&
      desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
    MTL_DXGI_FORMAT_DESC expected_format = {};
    if (!attachments.depth_stencil ||
        FAILED(MTLQueryDXGIFormat(device, desc.DSVFormat, expected_format)) ||
        attachments.depth_stencil->format != expected_format.PixelFormat) {
      anomaly.Add(DrawStateAnomaly::kDepthFormatMismatch,
                  "pipeline-dsv-format-mismatch");
    }
  }

  for (UINT slot = 0; slot < kDrawStateInputSlotCount; slot++) {
    if (!(slot_mask & (1u << slot)))
      continue;
    if (!state.vertex_buffers[slot]) {
      anomaly.Add(DrawStateAnomaly::kMissingVertexView,
                  "required-vertex-view-missing");
      continue;
    }
    const auto &view = *state.vertex_buffers[slot];
    UINT64 resource_offset = 0;
    auto *resource = LookupBufferResourceByGpuVirtualAddress(
        view.BufferLocation, &resource_offset);
    if (!resource || !resource->GetBuffer()) {
      anomaly.Add(DrawStateAnomaly::kUnresolvedVertexView,
                  "required-vertex-view-unresolved");
      continue;
    }
    const auto resource_width = resource->GetResourceDesc().Width;
    if (VertexViewExceedsResource(resource_offset, view.SizeInBytes,
                                  resource_width)) {
      anomaly.Add(DrawStateAnomaly::kVertexViewOutOfBounds,
                  "vertex-view-exceeds-resource");
    }

    if (draw && NonIndexedVertexFetchExceedsView(*draw, view.StrideInBytes,
                                                 view.SizeInBytes)) {
      anomaly.Add(DrawStateAnomaly::kVertexFetchOutOfBounds,
                  "nonindexed-vertex-fetch-exceeds-view");
    }
  }

  if (indexed_draw) {
    if (!state.index_buffer) {
      anomaly.Add(DrawStateAnomaly::kMissingIndexView,
                  "indexed-draw-index-view-missing");
    } else {
      UINT64 resolved_offset = 0;
      auto *resource = LookupBufferResourceByGpuVirtualAddress(
          state.index_buffer->BufferLocation, &resolved_offset);
      if (!resource || !resource->GetBuffer()) {
        anomaly.Add(DrawStateAnomaly::kUnresolvedIndexView,
                    "indexed-draw-index-view-unresolved");
      }
      const auto index_size = GetIndexSize(state.index_buffer->Format);
      if (IndexedFetchExceedsView(*indexed_draw, index_size,
                                  state.index_buffer->SizeInBytes))
        anomaly.Add(DrawStateAnomaly::kIndexFetchOutOfBounds,
                    "indexed-draw-fetch-exceeds-view");
    }
  }

  if (target_pso && anomaly.flags) {
    static std::atomic<uint32_t> anomaly_log_count = 0;
    if (D3D12DiagShouldLog(anomaly_log_count, true)) {
      WARN("D3D12 diagnostic: selected draw state anomaly",
           " sequence=", DiagCurrentReplayRecordSequence(),
           " recordSerial=", DiagCurrentReplayRecordSerial(),
           " pso=", cache_key,
           " flags=0x", std::hex, anomaly.flags, std::dec,
           " reasons=", anomaly.reasons,
           " drawVertexCount=", draw ? draw->vertex_count_per_instance : 0,
           " drawStartVertex=", draw ? draw->start_vertex_location : 0,
           " indexCount=",
           indexed_draw ? indexed_draw->index_count_per_instance : 0,
           " startIndex=",
           indexed_draw ? indexed_draw->start_index_location : 0,
           " indexViewSize=",
           state.index_buffer ? state.index_buffer->SizeInBytes : 0,
           " inputSlotMask=0x", std::hex, slot_mask, std::dec,
           " colorAttachments=", attachments.colors.size(),
           " hasDepthStencil=", attachments.depth_stencil.has_value());
    }
  }

  INFO("D3D12 diagnostic: draw state",
       " kind=", kind,
       " sequence=", d3d_sequence,
       " recordSerial=", record_serial,
       " pso=", key_prefix,
       " topology=", uint32_t(state.topology),
       " primitiveTopologyType=", uint32_t(desc.PrimitiveTopologyType),
       " sampleMask=", uint32_t(desc.SampleMask),
       " sampleCount=", uint32_t(desc.SampleDesc.Count),
       " rtvCount=", uint32_t(desc.NumRenderTargets),
       " dsvFormat=", uint32_t(desc.DSVFormat),
       " inputElements=", uint32_t(graphics->input_elements.size()),
       " inputSlotMask=0x", std::hex, slot_mask, std::dec,
       " viewportCount=", uint32_t(viewports.size()),
       " scissorCount=", uint32_t(scissors.size()),
       " colorAttachments=", uint32_t(attachments.colors.size()),
       " hasDepthStencil=", attachments.depth_stencil.has_value(),
       " fill=", D3D12FillModeName(desc.RasterizerState.FillMode),
       " cull=", D3D12CullModeName(desc.RasterizerState.CullMode),
       " frontCCW=", uint32_t(desc.RasterizerState.FrontCounterClockwise),
       " depthClip=", uint32_t(desc.RasterizerState.DepthClipEnable),
       " metalCull=", uint32_t(metal.rasterizer.cull_mode),
       " metalWinding=", uint32_t(metal.rasterizer.winding),
       " depthEnable=", uint32_t(desc.DepthStencilState.DepthEnable),
       " depthWrite=", uint32_t(desc.DepthStencilState.DepthWriteMask),
       " depthFunc=", uint32_t(desc.DepthStencilState.DepthFunc),
       " stencilEnable=", uint32_t(desc.DepthStencilState.StencilEnable),
       " alphaToCoverage=", uint32_t(desc.BlendState.AlphaToCoverageEnable),
       " independentBlend=", uint32_t(desc.BlendState.IndependentBlendEnable),
       " color0WriteMask=", desc.NumRenderTargets
                               ? uint32_t(desc.BlendState.RenderTarget[0].RenderTargetWriteMask)
                               : 0u,
       " color0Write=", color0_write,
       " color0Blend=", desc.NumRenderTargets
                            ? uint32_t(desc.BlendState.RenderTarget[0].BlendEnable)
                            : 0u,
       " drawVertexCount=", draw ? draw->vertex_count_per_instance : 0,
       " drawStartVertex=", draw ? draw->start_vertex_location : 0,
       " indexedIndexCount=", indexed_draw ? indexed_draw->index_count_per_instance : 0,
       " indexedStartIndex=", indexed_draw ? indexed_draw->start_index_location : 0,
       " indexedBaseVertex=", indexed_draw ? indexed_draw->base_vertex_location : 0,
       " instanceCount=", draw ? draw->instance_count
                                : indexed_draw ? indexed_draw->instance_count : 0,
       " baseInstance=", draw ? draw->start_instance_location
                               : indexed_draw ? indexed_draw->start_instance_location : 0,
       " indexFormat=", state.index_buffer ? uint32_t(state.index_buffer->Format) : 0u,
       " indexSize=", state.index_buffer ? uint32_t(state.index_buffer->SizeInBytes) : 0u,
       " indexViewOffset=", uint64_t(index_resource_offset),
       " indexMetalOffset=", uint64_t(index_offset));

  for (UINT i = 0;
       i < desc.NumRenderTargets && i < kDrawStateMaxRenderTargets; i++) {
    const auto &blend = desc.BlendState.RenderTarget[
        desc.BlendState.IndependentBlendEnable ? i : 0];
    INFO("D3D12 diagnostic: draw render target state",
         " pso=", key_prefix,
         " slot=", i,
         " descFormat=", uint32_t(desc.RTVFormats[i]),
         " writeMask=", uint32_t(blend.RenderTargetWriteMask),
         " blend=", uint32_t(blend.BlendEnable),
         " src=", uint32_t(blend.SrcBlend),
         " dst=", uint32_t(blend.DestBlend),
         " op=", uint32_t(blend.BlendOp),
         " srcAlpha=", uint32_t(blend.SrcBlendAlpha),
         " dstAlpha=", uint32_t(blend.DestBlendAlpha),
         " opAlpha=", uint32_t(blend.BlendOpAlpha));
  }

  for (const auto &color : attachments.colors) {
    INFO("D3D12 diagnostic: draw attachment state",
         " pso=", key_prefix,
         " sequence=", d3d_sequence,
         " recordSerial=", record_serial,
         " slot=", uint32_t(color.slot),
         " view=", uint64_t(color.view),
         " format=", uint32_t(color.format),
         " size=", color.width, "x", color.height,
         " array=", uint32_t(color.array_length));
  }
  for (UINT i = 0; i < state.render_targets.size(); i++) {
    const auto &descriptor = state.render_targets[i];
    Resource *resource = GetResource(descriptor.resource.ptr());
    INFO("D3D12 diagnostic: draw rtv binding",
         " pso=", key_prefix,
         " sequence=", d3d_sequence,
         " recordSerial=", record_serial,
         " slot=", i,
         " descriptorType=", DescriptorRecordTypeName(descriptor.type),
         " descriptorHeapIndex=", descriptor.heap_index,
         " descriptorHeapCount=", descriptor.heap_count,
         " descriptorResource=", uint64_t(descriptor.resource.ptr()),
         " resource=", uint64_t(resource ? resource->GetD3D12Resource() : nullptr),
         " hasTexture=", uint32_t(resource && resource->GetTexture()),
         " descFormat=", uint32_t(D3D12DiagDescriptorFormat(descriptor)));
  }
  if (attachments.depth_stencil) {
    const auto &depth = *attachments.depth_stencil;
    INFO("D3D12 diagnostic: draw depth state",
         " pso=", key_prefix,
         " sequence=", d3d_sequence,
         " recordSerial=", record_serial,
         " view=", uint64_t(depth.view),
         " format=", uint32_t(depth.format),
         " size=", depth.width, "x", depth.height,
         " array=", uint32_t(depth.array_length));
  }

  for (size_t i = 0; i < viewports.size(); i++) {
    const auto &viewport = viewports[i];
    INFO("D3D12 diagnostic: draw viewport",
         " pso=", key_prefix,
         " index=", uint32_t(i),
         " rect=", viewport.TopLeftX, ",", viewport.TopLeftY, ",",
         viewport.Width, ",", viewport.Height,
         " depth=", viewport.MinDepth, ",", viewport.MaxDepth);
  }
  for (size_t i = 0; i < scissors.size(); i++) {
    const auto &rect = scissors[i];
    INFO("D3D12 diagnostic: draw scissor",
         " pso=", key_prefix,
         " index=", uint32_t(i),
         " rect=", rect.left, ",", rect.top, ",", rect.right, ",",
         rect.bottom);
  }

  const auto max_slot = InputSlotMaskWidth(slot_mask);
  for (UINT slot = 0; slot < max_slot; slot++) {
    if (!(slot_mask & (1u << slot)))
      continue;
    const auto has_view = state.vertex_buffers[slot].has_value();
    UINT64 resource_offset = 0;
    Resource *resource = nullptr;
    if (has_view)
      resource = LookupBufferResourceByGpuVirtualAddress(
          state.vertex_buffers[slot]->BufferLocation, &resource_offset);
    INFO("D3D12 diagnostic: draw vertex buffer",
         " pso=", key_prefix,
         " slot=", slot,
         " hasView=", has_view,
         " resolved=", resource && resource->GetBuffer(),
         " stride=", has_view ? state.vertex_buffers[slot]->StrideInBytes : 0,
         " viewSize=", has_view ? state.vertex_buffers[slot]->SizeInBytes : 0,
         " resourceOffset=", uint64_t(resource_offset),
         " resourceWidth=", resource ? uint64_t(resource->GetResourceDesc().Width) : 0,
         " heapOffset=", resource ? uint64_t(resource->GetHeapOffset()) : 0);
  }
}

} // namespace dxmt::d3d12
