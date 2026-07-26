#include "d3d12_frozen_bindless_stage_tables.hpp"

#include "d3d12_bindless_window_probe.hpp"
#include "d3d12_sampler.hpp"
#include "d3d12_shader_binding.hpp"
#include "d3d12_snapshot_binding_query.hpp"
#include "dxmt_bindless_buffer_table.hpp"
#include "dxmt_descriptor_mirror.hpp"
#include "dxmt_gpu_lifetime.hpp"

#include <cassert>
#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

void FreezeBindlessStageTables(
    WMT::Device device,
    SubmissionStagePlanCache<BindlessMirrorStagePlan> &plan_cache,
    GraphicsBindingSnapshot &snapshot, const PipelineState &pipeline,
    PipelineStage want_stage, bool compute) {
  auto &out = MutableFrozenBindlessTablesForStage(snapshot, want_stage);
  out = {};
  if (!snapshot.bindless) {
    out.valid = true;
    return;
  }

  const auto *root = snapshot.root_signature_impl;
  if (!root) {
    // Capture without a root signature cannot build a stage plan; encode will
    // bind empty 28-30 tables (slot 27 still packs buffers).
    out.valid = true;
    return;
  }

  const auto *shader = FindShaderForStage(pipeline, want_stage);
  const auto *arguments = shader ? shader->resourceArgumentInfo() : nullptr;
  const auto argument_count =
      shader ? shader->reflection().NumArguments : 0u;
  // Vertex (and other) stages often have zero texture/sampler mirror args.
  // That is a valid empty freeze, not a failure.
  if (!shader || !arguments || !argument_count) {
    out.valid = true;
    return;
  }

  const auto plan = GetBindlessMirrorStagePlan(plan_cache, pipeline, *root,
                                               want_stage, compute);
  if (!plan || !plan->max_key_plus_one) {
    out.valid = true;
    return;
  }

  out.root_offsets.assign(plan->max_key_plus_one, 0);
  out.texture_field_pairs = plan->texture_field_pairs;
  if (plan->texture_count && plan->texture_field_pairs) {
    out.texture_window.assign(
        uint64_t(dxmt::kBindlessMirrorCapacity) *
            dxmt::kMirrorTextureQwords * plan->texture_field_pairs,
        0);
  }
  if (plan->sampler_count) {
    out.sampler_window.assign(
        uint64_t(dxmt::kBindlessMirrorCapacity) * dxmt::kMirrorSamplerQwords,
        0);
  }

  // Seed layout bases from the stage plan so empty ranges keep unique windows.
  for (const auto &entry : plan->entries) {
    if (entry.root_offset_key < out.root_offsets.size() &&
        entry.compact_base < dxmt::kBindlessMirrorCapacity)
      out.root_offsets[entry.root_offset_key] = entry.compact_base;
  }
  for (const auto &entry : plan->static_samplers) {
    if (entry.root_offset_key < out.root_offsets.size() &&
        entry.compact_base < dxmt::kBindlessMirrorCapacity)
      out.root_offsets[entry.root_offset_key] = entry.compact_base;
  }

  auto write_sampler = [&](uint32_t compact_base, uint32_t local,
                           const FrozenBindlessDescriptorPayload &payload) {
    if (out.sampler_window.empty())
      return;
    if (!dxmt::MirrorSlotInCapacity(compact_base, local,
                                    dxmt::kBindlessMirrorCapacity))
      return;
    uint64_t encoded[dxmt::kMirrorSamplerQwords] = {};
    if (payload.kind == FrozenBindlessDescriptorPayload::Kind::Sampler &&
        payload.sampler)
      EncodeMirrorSamplerSlot(encoded, *payload.sampler);
    else
      EncodeMirrorSamplerSlotNull(encoded, 0);
    dxmt::MirrorWriteSamplerSlot(
        out.sampler_window.data(), dxmt::kBindlessMirrorCapacity,
        compact_base, local, encoded[0], encoded[1], encoded[2]);
  };

  auto write_texture = [&](uint32_t compact_base, uint32_t local,
                           const DescriptorRecord &descriptor,
                           const DXMT12_MTL4_SHADER_ARGUMENT &arg) {
    if (out.texture_window.empty() || !out.texture_field_pairs)
      return;
    if (!dxmt::MirrorSlotInCapacity(compact_base, local,
                                    dxmt::kBindlessMirrorCapacity))
      return;
    // Capture-time freeze: resolve ordinary texture SRV/UAV only. Buffer
    // textures that need encode-time access stay zero; slot-27 still packs
    // buffer descriptors.
    auto payload = BuildBindlessTextureWindowPayload(device, descriptor, arg);
    if (!payload)
      return;
    dxmt::MirrorWriteTextureSlot(
        out.texture_window.data(), dxmt::kBindlessMirrorCapacity,
        out.texture_field_pairs, compact_base, local, payload->handle,
        payload->metadata);
  };

  auto fill_from_recipe =
      [&](const DXMT12_MTL4_SHADER_ARGUMENT &arg, uint32_t root_offset_key,
          uint32_t shader_register, uint32_t register_lower_bound,
          const DescriptorRecord &descriptor,
          const FrozenBindlessDescriptorPayload &frozen_payload) {
        const bool is_tex_or_sampler =
            arg.Type == SM50BindingType::Sampler ||
            (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE);
        if (!is_tex_or_sampler)
          return;
        if (root_offset_key >= out.root_offsets.size())
          return;
        if (shader_register < register_lower_bound)
          return;
        const auto argument_lower =
            arg.RegisterCount ? arg.RegisterLowerBound : arg.SM50BindingSlot;
        // Reject unsigned underflows that wrap into huge locals and can pass
        // a later capacity check after compact_base + local wraps.
        if (shader_register < argument_lower &&
            shader_register < register_lower_bound)
          return;
        const uint32_t local =
            shader_register >= argument_lower
                ? shader_register - argument_lower
                : shader_register - register_lower_bound;
        uint32_t compact_base = out.root_offsets[root_offset_key];
        // Prefer plan compact base for this StructurePtrOffset when available.
        for (const auto &plan_entry : plan->entries) {
          if (plan_entry.root_offset_key == root_offset_key) {
            compact_base = plan_entry.compact_base;
            break;
          }
        }
        if (compact_base >= dxmt::kBindlessMirrorCapacity)
          return;
        if (uint64_t(compact_base) + uint64_t(local) >=
            dxmt::kBindlessMirrorCapacity)
          return;
        out.root_offsets[root_offset_key] = compact_base;
        if (arg.Type == SM50BindingType::Sampler)
          write_sampler(compact_base, local, frozen_payload);
        else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE)
          write_texture(compact_base, local, descriptor, arg);
      };

  if (snapshot.native_descriptor_recipe) {
    const auto &recipe_entries = snapshot.native_descriptor_recipe->entries;
    assert(recipe_entries.size() ==
           snapshot.native_descriptor_indices.size());
    assert(recipe_entries.size() ==
           snapshot.compiled_bindless_payloads.size());
    for (size_t i = 0; i < recipe_entries.size(); ++i) {
      if (snapshot.native_descriptor_indices[i] == UINT32_MAX)
        continue;
      const auto &entry = recipe_entries[i];
      if (static_cast<PipelineStage>(entry.stage) != want_stage)
        continue;
      fill_from_recipe(entry.argument, entry.root_offset_key,
                       entry.shader_register, entry.register_lower_bound,
                       SnapshotNativeDescriptor(snapshot, i),
                       snapshot.compiled_bindless_payloads[i]);
    }
  }
  for (const auto &entry : snapshot.entries) {
    if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor ||
        !entry.has_descriptor || entry.stage != want_stage)
      continue;
    fill_from_recipe(*entry.argument, entry.root_offset_key,
                     entry.shader_register, entry.register_lower_bound,
                     SnapshotDescriptor(snapshot, entry),
                     entry.bindless_payload);
  }

  for (const auto &entry : plan->static_samplers) {
    if (entry.root_offset_key >= out.root_offsets.size() ||
        !dxmt::MirrorSlotInCapacity(entry.compact_slot, 0,
                                    dxmt::kBindlessMirrorCapacity))
      continue;
    auto sampler = CreateD3D12StaticSampler(device, entry.desc);
    if (!sampler || out.sampler_window.empty())
      continue;
    uint64_t encoded[dxmt::kMirrorSamplerQwords] = {};
    EncodeMirrorSamplerSlot(encoded, *sampler);
    out.root_offsets[entry.root_offset_key] = entry.compact_base;
    dxmt::MirrorWriteSamplerSlot(
        out.sampler_window.data(), dxmt::kBindlessMirrorCapacity,
        entry.compact_slot, 0, encoded[0], encoded[1], encoded[2]);
  }

  out.valid = true;
}

void FreezeAllBindlessStageTables(
    WMT::Device device,
    SubmissionStagePlanCache<BindlessMirrorStagePlan> &plan_cache,
    GraphicsBindingSnapshot &snapshot, const PipelineState &pipeline,
    bool compute) {
  if (!snapshot.bindless) {
    snapshot.frozen_bindless_vertex.valid = true;
    snapshot.frozen_bindless_pixel.valid = true;
    snapshot.frozen_bindless_compute.valid = true;
    return;
  }
  if (compute) {
    FreezeBindlessStageTables(device, plan_cache, snapshot, pipeline,
                              PipelineStage::Compute, true);
    return;
  }
  FreezeBindlessStageTables(device, plan_cache, snapshot, pipeline,
                            PipelineStage::Vertex, false);
  FreezeBindlessStageTables(device, plan_cache, snapshot, pipeline,
                            PipelineStage::Pixel, false);
}

} // namespace dxmt::d3d12
