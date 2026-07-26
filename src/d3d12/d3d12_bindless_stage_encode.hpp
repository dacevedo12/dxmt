#pragma once

// Per-shader-stage binding encode for the two mirror ABIs: the live/compiled
// path that resolves descriptors through the bound heaps, and the snapshot path
// that only uploads what capture already froze.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). They stay templates because the
// live path runs over live replay state, compiled packet state and frozen
// snapshot identities alike; everything queue-owned they reach for now arrives
// through a SubmissionBindingContext (or, for the snapshot path, the Metal
// device and the argument-buffer ring owner alone).

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_argument_upload.hpp"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_bindless_root_offsets.hpp"
#include "d3d12_native_stage_binding.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_shader_binding.hpp"
#include "d3d12_shader_stage_query.hpp"
#include "d3d12_snapshot_binding_query.hpp"
#include "d3d12_snapshot_buffer_table.hpp"
#include "d3d12_snapshot_root_constants.hpp"
#include "d3d12_stage_plan_cache.hpp"
#include "d3d12_submission_binding_context.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <string>
#include <utility>

namespace dxmt::d3d12 {

// The demote masks only exist on a render encoder, so every stage but Pixel
// reports an empty pair rather than reaching for an encoder it has no business
// touching.
template <PipelineStage Stage>
[[nodiscard]] std::pair<uint64_t, uint64_t>
CurrentPixelShaderMsaaSrvDemoteMasks(ArgumentEncodingContext &enc) {
  if constexpr (Stage == PipelineStage::Pixel) {
    auto *render_encoder = enc.currentRenderEncoder();
    return {render_encoder->pixel_shader_demote_msaa_srv_mask_lo,
            render_encoder->pixel_shader_demote_msaa_srv_mask_hi};
  } else {
    return {0, 0};
  }
}

// Bindless live/compute per-stage wiring. The mirror ABI packs a per-stage
// buffer table and binds root offsets plus the persistent sampler/texture
// heaps. Tessellation VS uses a distinct slot range so it can coexist with
// HS in one object function. The native ABI binds the descriptor heap directly.
template <PipelineStage Stage, PipelineKind Kind = PipelineKind::Ordinary,
          typename State>
void EncodeShaderBindingsForStageBindless(
    const SubmissionBindingContext &ctx, ArgumentEncodingContext &enc,
    const State &state, const PipelineState &pipeline,
    const RootSignature &root, const PipelineDxilShader &shader,
    const std::string &shader_key, bool compute,
    BindlessMirrorDrawDiag *draw_diag = nullptr,
    const NativeStageBindingToken *native_token = nullptr) {
  if (pipeline.GetShaderAbiVersion() ==
      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE) {
    {
      const auto plan = GetNativeRootBaseStagePlan(ctx.native_stage_plans,
                                                   pipeline, root, Stage,
                                                   compute);
      TrackNativeStageDescriptorAccesses<Stage>(ctx.device, enc, state,
                                                pipeline, plan.get(), compute);
    }
    constexpr WMTRenderStages render_stages =
        Stage == PipelineStage::Vertex
            ? WMTRenderStageVertex
            : Stage == PipelineStage::Pixel ? WMTRenderStageFragment
                                             : WMTRenderStages{};
    if (shader.reflection().NumConstantBuffers)
      enc.bindNativeNullConstantBuffer(compute, render_stages);
    if (NativeShaderUsesBufferSrv(shader))
      enc.bindNativeNullBuffer(compute, render_stages);
    auto cbuffer_root_bases =
        native_token
            ? UploadNativeRootTableBases(ctx.queue, enc,
                                         native_token->cbuffer_root_bases)
            : BuildNativeRootTableBases(
                  ctx.queue, enc, state,
                  *GetNativeRootBaseStagePlan(ctx.native_stage_plans, pipeline,
                                              root, Stage, compute),
                  compute, /*cbuffer=*/true);
    auto resource_root_bases =
        native_token
            ? UploadNativeRootTableBases(ctx.queue, enc,
                                         native_token->resource_root_bases)
            : BuildNativeRootTableBases(
                  ctx.queue, enc, state,
                  *GetNativeRootBaseStagePlan(ctx.native_stage_plans, pipeline,
                                              root, Stage, compute),
                  compute, /*cbuffer=*/false);
    enc.diagnoseNativeShaderBinding(
        Stage, shader_key, "native-live", cbuffer_root_bases,
        resource_root_bases);
    enc.bindNativeRootTableBases<Stage>(
        cbuffer_root_bases,
        DXMT12_MTL4_NATIVE_CBUFFER_ROOT_TABLE_BASE_BIND_INDEX);
    enc.bindNativeRootTableBases<Stage>(
        resource_root_bases,
        DXMT12_MTL4_NATIVE_ROOT_TABLE_BASE_BIND_INDEX);
    (void)shader;
    (void)shader_key;
    (void)draw_diag;
    return;
  }
  const auto &reflection = shader.reflection();
  const auto [demote_msaa_srv_mask_lo, demote_msaa_srv_mask_hi] =
      CurrentPixelShaderMsaaSrvDemoteMasks<Stage>(enc);
  auto bt = enc.packBindlessStage<Stage, Kind>(
      &reflection, shader.constantBufferInfo(), shader.resourceArgumentInfo(),
      shader_key, DiagCurrentReplayRecordSequence(),
      DiagCurrentReplayRecordSerial(), nullptr,
      demote_msaa_srv_mask_lo, demote_msaa_srv_mask_hi,
      "bindless-live");
  BindlessMirrorWindow window = {};
  auto root_offsets =
      BuildBindlessRootOffsets(ctx, enc, state, pipeline, root, Stage, compute,
                               &window, draw_diag);
  if (draw_diag)
    AddBindlessMirrorDiagBufTable(
        *draw_diag, Stage, shader.constantBufferInfo(),
        reflection.NumConstantBuffers, shader.resourceArgumentInfo(),
        reflection.NumArguments, bt);
  enc.bindBindlessTables<Stage, Kind>(bt, root_offsets, window.texture,
                                      window.sampler);
}

// Bindless-mirror (③.3) snapshot-path per-stage wiring: pack the slot-27 buf_table (encode-time,
// residency via packers) + bind 27/28/29/30. root_offsets is the slice pre-built from the
// snapshot entries. Compact sampler/texture windows are materialized solely
// from the frozen submission recipe; the live descriptor mirror is not part
// of compiled-direct encoding. Only Ordinary Vertex/Pixel.
template <PipelineStage Stage>
void EncodeShaderBindingsForStageBindlessSnapshot(
    WMT::Device device, ::dxmt::CommandQueue &queue,
    ArgumentEncodingContext &enc, const PipelineDxilShader &shader,
    const std::string &shader_key, const GraphicsBindingSnapshot &snapshot,
    BindlessMirrorDrawDiag *draw_diag = nullptr,
    bool pack_live_buffer_table = false) {
  assert(snapshot.bindless);
  assert(!snapshot.native_descriptor_recipe ||
         snapshot.native_descriptor_recipe->entries.size() ==
             snapshot.compiled_bindless_payloads.size());
  // Restore root 32-bit constants into cbuf_ before packBindless so any CBV
  // slot not present as a captured descriptor entry still reads the values
  // that belonged to this submission.
  for (const auto &entry : snapshot.entries) {
    if (entry.kind == GraphicsBindingSnapshotEntry::Kind::RootConstants &&
        entry.stage == Stage)
      BindRootConstantsSnapshot(queue, enc, entry);
  }
  const auto &reflection = shader.reflection();
  // Slot 27 packs buffer descriptors each draw (gpuAddress / suballocation
  // churn). When the caller already ran ApplyRootDescriptorTables into the
  // encoder, pack from live encoder state (nullptr snapshot). Compiled-direct
  // keeps capture-time bindings because it does not re-apply live tables.
  BindlessBufferTableSnapshotStorage bindings_storage;
  BindlessBufferTableSnapshot bindings_view = {};
  const BindlessBufferTableSnapshot *bindings_ptr = nullptr;
  if (!pack_live_buffer_table) {
    bindings_storage =
        BuildSnapshotBindlessBufferTableBindings(device, snapshot, Stage);
    bindings_view = bindings_storage.view();
    bindings_ptr = &bindings_view;
  }
  const auto [demote_msaa_srv_mask_lo, demote_msaa_srv_mask_hi] =
      CurrentPixelShaderMsaaSrvDemoteMasks<Stage>(enc);
  auto bt = enc.packBindlessStage<Stage, PipelineKind::Ordinary>(
      &reflection, shader.constantBufferInfo(), shader.resourceArgumentInfo(),
      shader_key, DiagCurrentReplayRecordSequence(),
      DiagCurrentReplayRecordSerial(), bindings_ptr,
      demote_msaa_srv_mask_lo, demote_msaa_srv_mask_hi,
      pack_live_buffer_table ? "bindless-snapshot-livebuf"
                             : "bindless-snapshot");
  if (draw_diag)
    AddBindlessMirrorDiagBufTable(
        *draw_diag, Stage, shader.constantBufferInfo(),
        reflection.NumConstantBuffers, shader.resourceArgumentInfo(),
        reflection.NumArguments, bt);
  // Slots 28-30: upload pre-frozen tables only (no recipe rebuild).
  const auto &frozen = FrozenBindlessTablesForStage(snapshot, Stage);
  if (!frozen.valid) {
    // Should be rare after capture always marks valid (including empty VS).
    // Rate-limit so a regression cannot flood logs into process OOM.
    static std::atomic<uint32_t> missing_frozen_logs = 0;
    const auto n = missing_frozen_logs.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n < 256 && (n & (n - 1)) == 0)) {
      ERR("DXMT bindless materialize: missing frozen stage tables stage=",
          PipelineStageName(Stage),
          " recipeEntries=",
          snapshot.native_descriptor_recipe
              ? snapshot.native_descriptor_recipe->entries.size()
              : 0,
          " count=", n + 1);
    }
  }
  BindlessMirrorWindow window = {};
  // Invalid freeze → empty upload (null 28-30); slot 27 still carries buffers.
  auto compact_root_offsets =
      frozen.valid
          ? UploadFrozenBindlessStageTables(queue, enc, frozen, &window)
          : AllocatedArgumentBufferSlice{};
  enc.bindBindlessTables<Stage>(bt, compact_root_offsets, window.texture,
                                window.sampler);
}

} // namespace dxmt::d3d12
