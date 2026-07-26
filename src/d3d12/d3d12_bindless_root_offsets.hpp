#pragma once

// Live per-draw construction of the bindless mirror slot-28 root_offsets, plus
// the compact texture/sampler windows (slots 29/30) that go with them.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_native_binding.inc). It stays a template because it runs
// over live replay state as well as compiled packet state; the queue-owned
// pieces it needs (argument-buffer ring, Metal device, memoized stage plans)
// now arrive in a SubmissionBindingContext.

#include "airconv_dx12_metal4.h"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_bindless_mirror_slot_fill.hpp"
#include "d3d12_bindless_window_probe.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_sampler.hpp"
#include "d3d12_shader_binding.hpp"
#include "d3d12_stage_plan_cache.hpp"
#include "d3d12_submission_binding_context.hpp"
#include "dxmt_bindless_buffer_table.hpp"
#include "dxmt_context.hpp"
#include "dxmt_descriptor_mirror.hpp"
#include "dxmt_gpu_lifetime.hpp"

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Bindless-mirror: build the per-draw slot-28 root_offsets for ONE shader
// stage. root_offsets[arg.StructurePtrOffset] is the compact mirror-window
// base assigned by BindlessMirrorStagePlan; descriptor heap slots are copied
// into that compact window below. Per the hybrid ABI, only TEXTURE/SAMPLER
// args get a root_offsets entry. BUFFER args use the slot-27 buf_table.
//
// Returns the ring slice (slot-28 buffer) holding uint32 root_offsets[N], or an empty
// slice if the stage has no texture/sampler args. The compute/graphics distinction is
// carried by `compute` (selects state.compute_tables vs graphics_tables / heap).
template <typename State>
[[nodiscard]] AllocatedArgumentBufferSlice
BuildBindlessRootOffsets(const SubmissionBindingContext &ctx,
                         ArgumentEncodingContext &enc, const State &state,
                         const PipelineState &pipeline,
                         const RootSignature &root, PipelineStage want_stage,
                         bool compute,
                         BindlessMirrorWindow *window = nullptr,
                         BindlessMirrorDrawDiag *draw_diag = nullptr) {
  const auto *shader = FindShaderForStage(pipeline, want_stage);
  if (!shader)
    return {};
  const auto *arguments = shader->resourceArgumentInfo();
  const auto argument_count = shader->reflection().NumArguments;
  if (!arguments || !argument_count)
    return {};

  const auto plan = GetBindlessMirrorStagePlan(ctx.bindless_stage_plans,
                                               pipeline, root, want_stage,
                                               compute);
  if (!plan || !plan->max_key_plus_one)
    return {};
  auto slice = ctx.queue.AllocateArgumentBuffer(
      enc.currentSeqId(), uint64_t(plan->max_key_plus_one) * sizeof(uint32_t));
  if (!slice.valid() ||
      slice.length < uint64_t(plan->max_key_plus_one) * sizeof(uint32_t) ||
      !slice.fill_zero()) {
    ERR("DXMT bindless live: root_offsets allocation failed keys=",
        plan->max_key_plus_one, " mapped=", slice.mapped != nullptr,
        " length=", slice.length);
    return {};
  }
  auto *root_offsets =
      slice.map<uint32_t>(0, plan->max_key_plus_one);
  if (!root_offsets)
    return {};
  const bool bindless_diag_enabled = BindlessMirrorDiagEnabled();
  std::vector<uint8_t> root_offset_assigned(
      bindless_diag_enabled ? plan->max_key_plus_one : 0, 0);
  std::vector<uint32_t> root_offset_issues(
      bindless_diag_enabled ? plan->max_key_plus_one : 0, 0);
  if (window) {
    if (plan->texture_count) {
      const uint64_t qwords =
          uint64_t(dxmt::kBindlessMirrorCapacity) *
          dxmt::kMirrorTextureQwords * plan->texture_field_pairs;
      window->texture_field_pairs = plan->texture_field_pairs;
      window->texture = ctx.queue.AllocateArgumentBuffer(
          enc.currentSeqId(), qwords * sizeof(uint64_t));
      if (window->texture.valid() &&
          window->texture.length >= qwords * sizeof(uint64_t)) {
        if (!window->texture.fill_zero())
          window->texture = {};
      } else if (qwords)
        ERR("DXMT bindless live: texture window allocation failed qwords=",
            qwords);
    }
    if (plan->sampler_count) {
      const uint64_t qwords =
          uint64_t(dxmt::kBindlessMirrorCapacity) * dxmt::kMirrorSamplerQwords;
      window->sampler = ctx.queue.AllocateArgumentBuffer(
          enc.currentSeqId(), qwords * sizeof(uint64_t));
      if (window->sampler.valid() &&
          window->sampler.length >= qwords * sizeof(uint64_t)) {
        if (!window->sampler.fill_zero())
          window->sampler = {};
      } else if (qwords)
        ERR("DXMT bindless live: sampler window allocation failed qwords=",
            qwords);
    }
  }
  std::vector<BindlessMirrorDiagProbe> bindless_diag_probes;

  for (const auto &entry : plan->entries) {
    if (entry.argument_index >= argument_count ||
        entry.root_offset_key >= plan->max_key_plus_one)
      continue;
    const auto &argument = arguments[entry.argument_index];
    const auto base_handle = GetTableHandle(state, compute, entry.root_index);
    if (!base_handle.ptr) {
      if (bindless_diag_enabled)
        root_offset_issues[entry.root_offset_key] |=
            BindlessRootOffsetIssueMissingTable;
      continue;
    }

    auto *heap = GetBoundDescriptorHeap(state, entry.heap_type);
    const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
        heap, base_handle, entry.range_offset, entry.descriptor_index,
        entry.descriptor_count, entry.heap_type);
    if (!descriptor) {
      if (bindless_diag_enabled)
        root_offset_issues[entry.root_offset_key] |=
            BindlessRootOffsetIssueMissingDescriptor;
      continue;
    }
    root_offsets[entry.root_offset_key] = entry.compact_base;
    if (bindless_diag_enabled)
      root_offset_assigned[entry.root_offset_key] = 1;
    if (window) {
      for (UINT local = 0;
           local < entry.range_count &&
           entry.descriptor_index + local < entry.descriptor_count;
           local++) {
        // Fail-closed uint64 capacity check (no uint32 wrap).
        if (!dxmt::MirrorSlotInCapacity(entry.compact_base, local,
                                        dxmt::kBindlessMirrorCapacity))
          break;
        // Reject argument_local_start overflow when forming local index.
        if (uint64_t(entry.argument_local_start) + uint64_t(local) >
            uint64_t(UINT32_MAX))
          break;
        const uint32_t dst_local = entry.argument_local_start + local;
        if (!dxmt::MirrorSlotInCapacity(entry.compact_base, dst_local,
                                        dxmt::kBindlessMirrorCapacity))
          break;
        const auto slot_descriptor = GetBoundDescriptorRecordInRangeFromHeap(
            heap, base_handle, entry.range_offset,
            entry.descriptor_index + local, entry.descriptor_count,
            entry.heap_type);
        if (!slot_descriptor || !slot_descriptor->mirror)
          continue;
        const auto source_slot = slot_descriptor->heap_index;
        if (entry.sampler && window->sampler.valid()) {
          auto *dst = window->sampler.map<uint64_t>(
              0, window->sampler.length / sizeof(uint64_t));
          const auto payload = slot_descriptor->mirror->samplerSlotPayload(
              source_slot, slot_descriptor->slot_version);
          if (payload && dst)
            dxmt::MirrorWriteSamplerSlot(
                dst, dxmt::kBindlessMirrorCapacity, entry.compact_base,
                dst_local, payload->handle, payload->cube_handle,
                payload->lod_bias);
        } else if (entry.texture && window->texture.valid()) {
          auto *dst = window->texture.map<uint64_t>(
              0, window->texture.length / sizeof(uint64_t));
          auto payload = BuildBindlessTextureWindowPayload(
              ctx.device, *slot_descriptor, argument);
          if (!payload)
            payload = slot_descriptor->mirror->textureSlotPayload(
                source_slot, slot_descriptor->slot_version);
          if (payload && dst)
            dxmt::MirrorWriteTextureSlot(
                dst, dxmt::kBindlessMirrorCapacity,
                window->texture_field_pairs, entry.compact_base, dst_local,
                payload->handle, payload->metadata);
        }
      }
    }
    if (entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER &&
        BindlessMirrorVerifyEnabled()) {
      for (UINT local = 0;
           local < entry.range_count &&
           entry.descriptor_index + local < entry.descriptor_count;
           local++) {
        const auto slot_descriptor = GetBoundDescriptorRecordInRangeFromHeap(
            heap, base_handle, entry.range_offset,
            entry.descriptor_index + local, entry.descriptor_count,
            entry.heap_type);
        if (slot_descriptor)
          VerifyBindlessMirrorSamplerDescriptor(ctx.device, enc,
                                                *slot_descriptor, want_stage,
                                                &argument);
      }
    }
    if (bindless_diag_enabled && (entry.texture || entry.sampler)) {
      for (UINT local = 0;
           local < entry.range_count &&
           entry.descriptor_index + local < entry.descriptor_count;
           local++) {
        const auto slot_descriptor = GetBoundDescriptorRecordInRangeFromHeap(
            heap, base_handle, entry.range_offset,
            entry.descriptor_index + local, entry.descriptor_count,
            entry.heap_type);
        if (!slot_descriptor)
          continue;
        const auto dst_local = entry.argument_local_start + local;
        bindless_diag_probes.push_back(BindlessMirrorDiagProbe{
            "live", &pipeline, window, want_stage, &argument,
            entry.shader_register_lower_bound + dst_local,
            entry.shader_register_lower_bound, 0, 0,
            slot_descriptor});
      }
    }
  }
  if (window && window->sampler.valid()) {
    auto *dst = window->sampler.map<uint64_t>(
        0, window->sampler.length / sizeof(uint64_t));
    for (const auto &entry : plan->static_samplers) {
      if (entry.root_offset_key >= plan->max_key_plus_one ||
          !dxmt::MirrorSlotInCapacity(entry.compact_slot, 0,
                                      dxmt::kBindlessMirrorCapacity))
        continue;
      auto sampler = CreateD3D12StaticSampler(ctx.device, entry.desc);
      if (!sampler)
        continue;
      uint64_t encoded[dxmt::kMirrorSamplerQwords] = {};
      EncodeMirrorSamplerSlot(encoded, *sampler);
      root_offsets[entry.root_offset_key] = entry.compact_base;
      if (bindless_diag_enabled)
        root_offset_assigned[entry.root_offset_key] = 1;
      if (dst)
        dxmt::MirrorWriteSamplerSlot(
            dst, dxmt::kBindlessMirrorCapacity, entry.compact_slot, 0,
            encoded[0], encoded[1], encoded[2]);
    }
  }
  for (auto probe : bindless_diag_probes) {
    if (!probe.arg || probe.arg->StructurePtrOffset >= plan->max_key_plus_one)
      continue;
    const auto local = probe.shader_register - probe.lower_bound;
    probe.root_offset = root_offsets[probe.arg->StructurePtrOffset];
    probe.absolute_slot = probe.root_offset + local;
    // The probes report through `draw_diag` and the process counters; the
    // bool is only interesting to a caller probing a single slot.
    if (probe.arg->Type == SM50BindingType::Sampler)
      (void)ProbeBindlessMirrorSamplerBinding(ctx.queue.CurrentFrameSeq(),
                                              probe, draw_diag);
    else
      (void)ProbeBindlessMirrorTextureBinding(
          ctx.device, ctx.queue.CurrentFrameSeq(), probe, draw_diag);
  }
  if (bindless_diag_enabled) {
    std::vector<uint8_t> diagnosed(plan->max_key_plus_one, 0);
    for (UINT i = 0; i < argument_count; i++) {
      const auto &argument = arguments[i];
      const bool tracked = argument.Type == SM50BindingType::Sampler ||
                           (argument.Flags &
                            MTL_SM50_SHADER_ARGUMENT_TEXTURE);
      const auto key = argument.StructurePtrOffset;
      if (!tracked || key >= plan->max_key_plus_one || diagnosed[key] ||
          root_offset_assigned[key])
        continue;
      diagnosed[key] = 1;
      DiagnoseBindlessRootOffsetGap(
          ctx.queue.CurrentFrameSeq(), "live", &pipeline, want_stage, argument,
          root_offset_issues[key] |
              BindlessRootOffsetIssueMissingPlanCoverage,
          draw_diag);
    }
  }
  slice.flush_if_needed();
  if (window) {
    window->texture.flush_if_needed();
    window->sampler.flush_if_needed();
  }
  return slice;
}

} // namespace dxmt::d3d12
