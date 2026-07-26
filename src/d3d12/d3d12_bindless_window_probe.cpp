#include "d3d12_bindless_window_probe.hpp"

#include "d3d12_descriptor_diagnostics.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_shader_binding.hpp"
#include "dxmt_bindless_buffer_table.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <utility>

namespace dxmt::d3d12 {

namespace {

// Shortened pipeline-state cache key used as the `pso=` field of every probe
// log line.
std::string
ProbePsoKeyPrefix(const PipelineState &pipeline) {
  const auto &cache_key = pipeline.GetShaderCacheKey();
  return std::string(cache_key.c_str(),
                     cache_key.c_str() +
                         std::min<size_t>(cache_key.size(), 16));
}

} // namespace

std::optional<DescriptorTextureSlotPayload>
BuildBindlessTextureWindowPayload(WMT::Device device,
                                  const DescriptorRecord &record,
                                  const DXMT12_MTL4_SHADER_ARGUMENT &argument) {
  if (!(argument.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE))
    return std::nullopt;

  auto *resource = GetResource(record.resource.ptr());
  if (!resource || resource->GetBuffer() || !resource->GetTexture())
    return std::nullopt;
  if (resource->IsReservedTexture() &&
      !resource->EnsureTextureAllocation(
          "BuildBindlessTextureWindowPayload"))
    return std::nullopt;

  TextureViewBinding view = {};
  if (record.type == DescriptorRecordType::ShaderResourceView) {
    view = CreateShaderResourceTextureView(device, *resource, record);
  } else if (record.type == DescriptorRecordType::UnorderedAccessView) {
    view = CreateUnorderedAccessTextureView(device, *resource, record);
  }
  if (!view || !view.texture.ptr())
    return std::nullopt;

  const auto adapted_view = view.texture->checkViewUseArray(
      view.view,
      argument.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
  auto *allocation = view.texture->current();
  if (!allocation)
    return std::nullopt;

  uint64_t encoded[dxmt::kMirrorTextureQwords] = {};
  EncodeMirrorTextureSlot(
      encoded, view.texture->view(adapted_view, allocation).gpuResourceID,
      view.texture->arrayLength(adapted_view), 0.0f);
  return DescriptorTextureSlotPayload{encoded[0], encoded[1]};
}

void
DiagnoseBindlessRootOffsetGap(uint64_t frame_seq, const char *path,
                              const PipelineState *pipeline,
                              PipelineStage stage,
                              const DXMT12_MTL4_SHADER_ARGUMENT &arg,
                              uint32_t issue_flags,
                              BindlessMirrorDrawDiag *draw_diag) {
  if (!BindlessMirrorDiagEnabled())
    return;
  if (draw_diag)
    draw_diag->root_offset_missing++;

  static std::atomic<uint32_t> occurrence_count = 0;
  const auto occurrence =
      occurrence_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (occurrence > 64 && (occurrence & (occurrence - 1)) != 0)
    return;

  std::string pso;
  std::string shader_hash;
  if (pipeline) {
    pso = ProbePsoKeyPrefix(*pipeline);
    if (const auto *shader = FindShaderForStage(*pipeline, stage))
      shader_hash = BindlessMirrorShaderSha1(*shader);
  }
  WARN("DXMT bindless-window DIAG root-offset-gap",
       " occurrence=", occurrence,
       " frame=", frame_seq,
       " recordSerial=", DiagCurrentReplayRecordSerial(),
       " path=", path ? path : "unknown",
       " pso=", pso,
       " shader=", shader_hash,
       " stage=", PipelineStageName(stage),
       " argKey=", arg.StructurePtrOffset,
       " type=", uint32_t(arg.Type),
       " slot=", arg.SM50BindingSlot,
       " lower=", arg.RegisterLowerBound,
       " count=", arg.RegisterCount,
       " flags=0x", std::hex, uint32_t(arg.Flags),
       " issueFlags=0x", issue_flags, std::dec);
}

bool
ProbeBindlessMirrorTextureBinding(WMT::Device device, uint64_t frame_seq,
                                  const BindlessMirrorDiagProbe &probe,
                                  BindlessMirrorDrawDiag *draw_diag) {
  if (!BindlessMirrorDiagEnabled() || !probe.arg || !probe.descriptor)
    return false;

  auto *mirror = probe.descriptor->mirror;
  auto source_payload =
      BuildBindlessTextureWindowPayload(device, *probe.descriptor, *probe.arg);
  if (!source_payload && mirror && !mirror->isSamplerHeap())
    source_payload = mirror->textureSlotPayload(
        probe.descriptor->heap_index, probe.descriptor->slot_version);

  std::optional<DescriptorTextureSlotPayload> window_payload;
  uint32_t pair_mismatches = 0;
  bool window_in_bounds = false;
  if (probe.window && probe.window->texture.mapped &&
      probe.window->texture_field_pairs &&
      probe.absolute_slot < dxmt::kBindlessMirrorCapacity) {
    const auto *qwords =
        static_cast<const uint64_t *>(probe.window->texture.mapped);
    const auto qword_count =
        probe.window->texture.length / sizeof(uint64_t);
    for (uint32_t pair = 0; pair < probe.window->texture_field_pairs;
         pair++) {
      const uint64_t pair_base =
          uint64_t(pair) * dxmt::kMirrorTextureQwords *
          dxmt::kBindlessMirrorCapacity;
      const uint64_t handle_index = pair_base + probe.absolute_slot;
      const uint64_t meta_index =
          pair_base + dxmt::kBindlessMirrorCapacity + probe.absolute_slot;
      if (meta_index >= qword_count)
        break;
      window_in_bounds = true;
      const DescriptorTextureSlotPayload payload{
          qwords[handle_index], qwords[meta_index]};
      if (!window_payload)
        window_payload = payload;
      else if (payload.handle != window_payload->handle ||
               payload.metadata != window_payload->metadata)
        pair_mismatches++;
    }
  }

  const auto filled_version =
      mirror ? mirror->slotFilledVersion(probe.descriptor->heap_index)
             : dxmt::DescriptorSlotVersion{};
  const auto stale_version =
      mirror ? mirror->slotStaleVersion(probe.descriptor->heap_index)
             : dxmt::DescriptorSlotVersion{};
  auto *resource = GetResource(probe.descriptor->resource.ptr());
  const auto resource_desc =
      resource ? resource->GetResourceDesc() : D3D12_RESOURCE_DESC{};
  auto *texture = resource ? resource->GetTexture() : nullptr;
  auto *allocation = resource ? resource->GetTextureAllocation() : nullptr;
  const bool source_missing =
      !source_payload && resource && texture && probe.descriptor->has_desc;
  const bool window_missing =
      source_payload && (!window_in_bounds || !window_payload);
  const bool payload_mismatch =
      source_payload && window_payload &&
      (source_payload->handle != window_payload->handle ||
       source_payload->metadata != window_payload->metadata ||
       pair_mismatches != 0);
  const bool invalid = source_missing || window_missing || payload_mismatch;
  UINT view_dimension = 0;
  if (probe.descriptor->has_desc &&
      probe.descriptor->type == DescriptorRecordType::ShaderResourceView)
    view_dimension = UINT(probe.descriptor->desc.srv.ViewDimension);
  else if (probe.descriptor->has_desc &&
           probe.descriptor->type ==
               DescriptorRecordType::UnorderedAccessView)
    view_dimension = UINT(probe.descriptor->desc.uav.ViewDimension);
  if (draw_diag) {
    if (probe.stage == PipelineStage::Pixel)
      draw_diag->ps_tex++;
    else if (probe.stage == PipelineStage::Vertex)
      draw_diag->vs_tex++;
    if (invalid) {
      draw_diag->tex_null++;
      if (probe.stage == PipelineStage::Pixel)
        draw_diag->ps_tex_null++;
      else if (probe.stage == PipelineStage::Vertex)
        draw_diag->vs_tex_null++;
    }
    draw_diag->texture_payload_mismatch += payload_mismatch;
    draw_diag->texture_source_missing += source_missing;
    draw_diag->texture_window_missing += window_missing;
  }
  static std::atomic<uint32_t> invalid_count = 0;
  const auto invalid_occurrence =
      invalid ? invalid_count.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
  const bool log_invalid =
      invalid_occurrence &&
      (invalid_occurrence <= 64 ||
       (invalid_occurrence & (invalid_occurrence - 1)) == 0);
  const bool sample = !invalid && BindlessMirrorDiagShouldLog();
  if (!log_invalid && !sample)
    return false;
  const char *fill_state = filled_version == stale_version
                               ? (filled_version ? "filled" : "never-filled")
                               : "stale";

  std::string pso;
  std::string shader_hash;
  if (probe.pipeline) {
    pso = ProbePsoKeyPrefix(*probe.pipeline);
    if (const auto *shader =
            FindShaderForStage(*probe.pipeline, probe.stage))
      shader_hash = BindlessMirrorShaderSha1(*shader);
  }

  INFO("DXMT bindless-window DIAG"
       " invalidOccurrence=", invalid_occurrence,
       " frame=", frame_seq,
       " draw=", DiagCurrentReplayRecordSequence(),
       " drawSerial=", DiagCurrentReplayRecordSerial(),
       " pso=", pso,
       " shader=", shader_hash,
       " stage=", PipelineStageName(probe.stage),
       " argKey=", probe.arg->StructurePtrOffset,
       " register=", probe.shader_register,
       " lower=", probe.lower_bound,
       " rootOffset=", probe.root_offset,
       " absoluteSlot=", probe.absolute_slot,
       " path=", probe.path ? probe.path : "unknown",
       " fill=", fill_state,
       " filledEpoch=", filled_version.epoch,
       " filledSequence=", filled_version.sequence,
       " staleEpoch=", stale_version.epoch,
       " staleSequence=", stale_version.sequence,
       " sourceHandle=", source_payload ? source_payload->handle : 0,
       " sourceMeta=", source_payload ? source_payload->metadata : 0,
       " windowHandle=", window_payload ? window_payload->handle : 0,
       " windowMeta=", window_payload ? window_payload->metadata : 0,
       " windowInBounds=", window_in_bounds ? 1 : 0,
       " windowPairs=",
       probe.window ? probe.window->texture_field_pairs : 0,
       " pairMismatches=", pair_mismatches,
       " descriptorSlot=", probe.descriptor->heap_index,
       " descriptorVersion=", probe.descriptor->slot_version.epoch, ":",
       probe.descriptor->slot_version.sequence,
       " descriptorType=", uint32_t(probe.descriptor->type),
       " hasDesc=", probe.descriptor->has_desc ? 1 : 0,
       " viewDimension=", view_dimension,
       " descriptorFormat=",
       uint32_t(D3D12DiagDescriptorFormat(*probe.descriptor)),
       " d3dResource=", uint64_t(probe.descriptor->resource.ptr()),
       " resource=",
       uint64_t(resource ? resource->GetD3D12Resource() : nullptr),
       " resourceDimension=", uint32_t(resource_desc.Dimension),
       " resourceSize=", uint64_t(resource_desc.Width), "x",
       uint32_t(resource_desc.Height), "x",
       uint32_t(resource_desc.DepthOrArraySize),
       " resourceMips=", uint32_t(resource_desc.MipLevels),
       " resourceFormat=", uint32_t(resource_desc.Format),
       " resourceSamples=", uint32_t(resource_desc.SampleDesc.Count),
       " textureDescriptor=", uint64_t(texture),
       " textureIdentity=", texture ? texture->diagnosticIdentity() : 0,
       " allocation=", uint64_t(allocation),
       " metalTexture=",
       texture && texture->current()
           ? uint64_t(texture->current()->texture())
           : 0,
       " reason=", source_missing
                       ? "source-payload-missing"
                       : window_missing
                             ? "window-payload-missing"
                             : payload_mismatch ? "payload-mismatch"
                                                : "sample-valid");
  return invalid;
}

bool
ProbeBindlessMirrorSamplerBinding(uint64_t frame_seq,
                                  const BindlessMirrorDiagProbe &probe,
                                  BindlessMirrorDrawDiag *draw_diag) {
  if (!BindlessMirrorDiagEnabled() || !probe.arg || !probe.descriptor)
    return false;

  auto *mirror = probe.descriptor->mirror;
  const auto source_payload =
      mirror && mirror->isSamplerHeap()
          ? mirror->samplerSlotPayload(probe.descriptor->heap_index,
                                       probe.descriptor->slot_version)
          : std::nullopt;
  std::optional<DescriptorSamplerSlotPayload> window_payload;
  bool window_in_bounds = false;
  if (probe.window && probe.window->sampler.mapped &&
      probe.absolute_slot < dxmt::kBindlessMirrorCapacity) {
    const auto *qwords =
        static_cast<const uint64_t *>(probe.window->sampler.mapped);
    const auto qword_count = probe.window->sampler.length / sizeof(uint64_t);
    const uint64_t handle_index = probe.absolute_slot;
    const uint64_t cube_index =
        uint64_t(dxmt::kBindlessMirrorCapacity) + probe.absolute_slot;
    const uint64_t lod_index =
        uint64_t(dxmt::kBindlessMirrorCapacity) * 2 + probe.absolute_slot;
    if (lod_index < qword_count) {
      window_in_bounds = true;
      window_payload = DescriptorSamplerSlotPayload{
          qwords[handle_index], qwords[cube_index], qwords[lod_index]};
    }
  }

  const bool source_missing =
      !source_payload && mirror && probe.descriptor->has_desc;
  const bool window_missing =
      source_payload && (!window_in_bounds || !window_payload);
  const bool payload_mismatch =
      source_payload && window_payload &&
      (source_payload->handle != window_payload->handle ||
       source_payload->cube_handle != window_payload->cube_handle ||
       source_payload->lod_bias != window_payload->lod_bias);
  const bool invalid = source_missing || window_missing || payload_mismatch;
  if (draw_diag) {
    draw_diag->sampler_payload_mismatch += payload_mismatch;
    draw_diag->sampler_source_missing += source_missing;
    draw_diag->sampler_window_missing += window_missing;
  }

  static std::atomic<uint32_t> invalid_count = 0;
  const auto invalid_occurrence =
      invalid ? invalid_count.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
  const bool log_invalid =
      invalid_occurrence &&
      (invalid_occurrence <= 64 ||
       (invalid_occurrence & (invalid_occurrence - 1)) == 0);
  const bool sample = !invalid && BindlessMirrorDiagShouldLog();
  if (!log_invalid && !sample)
    return false;

  std::string pso;
  std::string shader_hash;
  if (probe.pipeline) {
    pso = ProbePsoKeyPrefix(*probe.pipeline);
    if (const auto *shader = FindShaderForStage(*probe.pipeline, probe.stage))
      shader_hash = BindlessMirrorShaderSha1(*shader);
  }
  INFO("DXMT bindless-sampler-window DIAG",
       " invalidOccurrence=", invalid_occurrence,
       " frame=", frame_seq,
       " draw=", DiagCurrentReplayRecordSequence(),
       " drawSerial=", DiagCurrentReplayRecordSerial(),
       " pso=", pso,
       " shader=", shader_hash,
       " stage=", PipelineStageName(probe.stage),
       " argKey=", probe.arg->StructurePtrOffset,
       " register=", probe.shader_register,
       " lower=", probe.lower_bound,
       " rootOffset=", probe.root_offset,
       " absoluteSlot=", probe.absolute_slot,
       " path=", probe.path ? probe.path : "unknown",
       " sourceHandle=", source_payload ? source_payload->handle : 0,
       " sourceCube=", source_payload ? source_payload->cube_handle : 0,
       " sourceLodBias=", source_payload ? source_payload->lod_bias : 0,
       " windowHandle=", window_payload ? window_payload->handle : 0,
       " windowCube=", window_payload ? window_payload->cube_handle : 0,
       " windowLodBias=", window_payload ? window_payload->lod_bias : 0,
       " windowInBounds=", window_in_bounds ? 1 : 0,
       " descriptorSlot=", probe.descriptor->heap_index,
       " descriptorVersion=", probe.descriptor->slot_version.epoch, ":",
       probe.descriptor->slot_version.sequence,
       " descriptorType=", uint32_t(probe.descriptor->type),
       " hasDesc=", probe.descriptor->has_desc ? 1 : 0,
       " reason=", source_missing
                       ? "source-payload-missing"
                       : window_missing
                             ? "window-payload-missing"
                             : payload_mismatch ? "payload-mismatch"
                                                : "sample-valid");
  return invalid;
}

} // namespace dxmt::d3d12
