#include "d3d12_replay_binding_encode.hpp"

#include "d3d12_compiled_binding_encode.hpp"

namespace dxmt::d3d12 {

void EncodeComputeBindings(const SubmissionBindingContext &ctx,
                           ArgumentEncodingContext &enc,
                           const ReplayState &state,
                           const PipelineState &pipeline,
                           uint64_t &argbuf_offset) {
  const bool native =
      pipeline.GetShaderAbiVersion() ==
      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  if (!native)
    ApplyRootDescriptorTables(ctx.device, ctx.queue, enc, state, pipeline,
                              true);
  const auto &shaders = pipeline.GetDxilShaders();
  const auto &key = pipeline.GetShaderCacheKey();
  const bool bindless = pipeline.UsesBindlessMirror();
  const RootSignature *bindless_root = state.compute_root_signature_impl;
  if (native) {
    if (!bindless_root)
      return;
    EncodeNativeArgumentTables(enc, state, true, {});
    for (const auto &shader : shaders) {
      if (shader.stage == PipelineShaderStage::Compute)
        EncodeShaderBindingsForStageBindless<PipelineStage::Compute>(
            ctx, enc, state, pipeline, *bindless_root, shader, key, true);
    }
    return;
  }
  if (!bindless || !bindless_root)
    return;
  for (const auto &shader : shaders) {
    if (shader.stage == PipelineShaderStage::Compute)
      EncodeShaderBindingsForStageBindless<PipelineStage::Compute>(
          ctx, enc, state, pipeline, *bindless_root, shader, key, true);
  }
}

} // namespace dxmt::d3d12
