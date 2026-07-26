#pragma once

// Whole-pipeline binding encode driven by live replay state: root descriptor
// tables, vertex buffers and every shader stage of the bound pipeline.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_compiled_encode.inc). It is the non-compiled path, used
// by the indirect draw/dispatch replays that have no compiled binding payload,
// and it needs nothing from the command queue beyond a SubmissionBindingContext.

#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_bindless_stage_encode.hpp"
#include "d3d12_compiled_binding_encode.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_root_parameter_apply.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_submission_binding_context.hpp"
#include "d3d12_vertex_buffer_encode.hpp"
#include "dxmt_context.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

/** Binds everything one live-state graphics draw needs. `out_diag`, when given,
 *  receives the bindless-mirror observations instead of them being folded into
 *  the process counters — the caller then owns reporting them. */
template <typename State>
void EncodeGraphicsBindings(const SubmissionBindingContext &ctx,
                            ArgumentEncodingContext &enc, const State &state,
                            const PipelineState &pipeline, bool use_geometry,
                            bool use_tessellation, uint64_t &argbuf_offset,
                            BindlessMirrorDrawDiag *out_diag = nullptr) {
  const bool native =
      pipeline.GetShaderAbiVersion() ==
      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  if (!native)
    ApplyRootDescriptorTables(ctx.device, ctx.queue, enc, state, pipeline,
                              false);
  const auto pipeline_kind = use_tessellation
                                 ? PipelineKind::Tessellation
                                 : use_geometry ? PipelineKind::Geometry
                                                : PipelineKind::Ordinary;
  EncodeVertexBuffers(enc, state, pipeline.GetGraphicsState(),
                      argbuf_offset, pipeline_kind);
  const auto &shaders = pipeline.GetDxilShaders();
  const auto &key = pipeline.GetShaderCacheKey();
  const bool bindless = pipeline.UsesBindlessMirror();
  const RootSignature *binding_root = state.graphics_root_signature_impl;
  const RootSignature *bindless_root = bindless ? binding_root : nullptr;
  const bool bindless_path = bindless_root != nullptr;
  const bool native_path =
      native && binding_root && !use_geometry && !use_tessellation;
  BindlessMirrorDrawDiag diag = {};
  diag.uses_bindless_mirror = bindless;
  diag.bindless_bound = bindless_path;
  diag.path = BindlessMirrorDiagPathName(bindless_path, false);
  if (native) {
    diag.path = "native";
    if (native_path) {
      EncodeNativeArgumentTables(
          enc, state, false,
          WMTRenderStageVertex | WMTRenderStageFragment);
      for (const auto &shader : shaders) {
        if (shader.stage == PipelineShaderStage::Vertex) {
          EncodeShaderBindingsForStageBindless<PipelineStage::Vertex>(
              ctx, enc, state, pipeline, *binding_root, shader, key, false,
              &diag);
        } else if (shader.stage == PipelineShaderStage::Pixel) {
          EncodeShaderBindingsForStageBindless<PipelineStage::Pixel>(
              ctx, enc, state, pipeline, *binding_root, shader, key, false,
              &diag);
        }
      }
    }
    if (out_diag)
      *out_diag = diag;
    else
      RecordBindlessMirrorDiagDraw(ctx.queue.CurrentFrameSeq(), &pipeline,
                                   diag);
    return;
  }
  if (!bindless_path)
    return;
  for (const auto &shader : shaders) {
    if (use_geometry) {
      if (shader.stage == PipelineShaderStage::Vertex)
        EncodeShaderBindingsForStageBindless<
            PipelineStage::Vertex, PipelineKind::Geometry>(
            ctx, enc, state, pipeline, *bindless_root, shader, key, false,
            &diag);
      else if (shader.stage == PipelineShaderStage::Geometry)
        EncodeShaderBindingsForStageBindless<
            PipelineStage::Geometry, PipelineKind::Geometry>(
            ctx, enc, state, pipeline, *bindless_root, shader, key, false,
            &diag);
      else if (shader.stage == PipelineShaderStage::Pixel)
        EncodeShaderBindingsForStageBindless<
            PipelineStage::Pixel, PipelineKind::Geometry>(
            ctx, enc, state, pipeline, *bindless_root, shader, key, false,
            &diag);
    } else if (use_tessellation) {
      if (shader.stage == PipelineShaderStage::Vertex)
        EncodeShaderBindingsForStageBindless<
            PipelineStage::Vertex, PipelineKind::Tessellation>(
            ctx, enc, state, pipeline, *bindless_root, shader, key, false,
            &diag);
      else if (shader.stage == PipelineShaderStage::Hull)
        EncodeShaderBindingsForStageBindless<
            PipelineStage::Hull, PipelineKind::Tessellation>(
            ctx, enc, state, pipeline, *bindless_root, shader, key, false,
            &diag);
      else if (shader.stage == PipelineShaderStage::Domain)
        EncodeShaderBindingsForStageBindless<
            PipelineStage::Domain, PipelineKind::Tessellation>(
            ctx, enc, state, pipeline, *bindless_root, shader, key, false,
            &diag);
      else if (shader.stage == PipelineShaderStage::Pixel)
        EncodeShaderBindingsForStageBindless<
            PipelineStage::Pixel, PipelineKind::Tessellation>(
            ctx, enc, state, pipeline, *bindless_root, shader, key, false,
            &diag);
    } else if (shader.stage == PipelineShaderStage::Vertex) {
      EncodeShaderBindingsForStageBindless<PipelineStage::Vertex>(
          ctx, enc, state, pipeline, *bindless_root, shader, key, false,
          &diag);
    } else if (shader.stage == PipelineShaderStage::Pixel) {
      EncodeShaderBindingsForStageBindless<PipelineStage::Pixel>(
          ctx, enc, state, pipeline, *bindless_root, shader, key, false,
          &diag);
    }
  }
  if (out_diag)
    *out_diag = diag;
  else
    RecordBindlessMirrorDiagDraw(ctx.queue.CurrentFrameSeq(), &pipeline, diag);
}

/** Compute counterpart of EncodeGraphicsBindings. A non-bindless, non-native
 *  compute pipeline binds nothing beyond its root descriptor tables. */
void EncodeComputeBindings(const SubmissionBindingContext &ctx,
                           ArgumentEncodingContext &enc,
                           const ReplayState &state,
                           const PipelineState &pipeline,
                           uint64_t &argbuf_offset);

} // namespace dxmt::d3d12
