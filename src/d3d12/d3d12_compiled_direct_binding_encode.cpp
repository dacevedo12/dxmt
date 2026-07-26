#include "d3d12_compiled_direct_binding_encode.hpp"

#include "d3d12_bindless_stage_encode.hpp"
#include "d3d12_compiled_binding_encode.hpp"
#include "d3d12_vertex_buffer_encode.hpp"

#include <cassert>

namespace dxmt::d3d12 {

void EncodeCompiledGraphicsBindings(
    WMT::Device device, ::dxmt::CommandQueue &queue,
    ArgumentEncodingContext &enc,
    const CompiledDirectGraphicsBindingPayload &payload,
    PipelineState &pipeline, uint64_t &argbuf_offset,
    const CompiledBindingDelta *binding_delta,
    BindlessMirrorDrawDiag *draw_diag,
    CompiledCommandTestTelemetry *test_telemetry) {
  BindlessMirrorDrawDiag local_diag = {};
  BindlessMirrorDrawDiag &diag = draw_diag ? *draw_diag : local_diag;
  diag.uses_bindless_mirror = pipeline.UsesBindlessMirror();
  diag.bindless_bound = diag.uses_bindless_mirror;
  diag.path = "compiled-direct";
  const bool native = pipeline.GetShaderAbiVersion() ==
                      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  assert(!native || payload.native_binding_recipe);
  const uint32_t dirty_fields = binding_delta
                                    ? binding_delta->dirty_fields
                                    : UINT32_MAX;
  const bool tables_dirty =
      dirty_fields & (CompiledBindingDirtyPipelineLayout |
                      CompiledBindingDirtyResourceHeap |
                      CompiledBindingDirtySamplerHeap |
                      CompiledBindingDirtyRootTables);
  const bool native_recipe_dirty =
      tables_dirty ||
      (dirty_fields & (CompiledBindingDirtyRootConstants |
                       CompiledBindingDirtyRootDescriptors));
  if (native && native_recipe_dirty) {
    assert(payload.native_binding_recipe);
    if (!payload.native_binding_recipe)
      return;
    EncodeCompiledNativeBindingRecipe(
        enc, *payload.native_binding_recipe, dirty_fields,
        binding_delta ? binding_delta->root_table_dirty_mask : UINT64_MAX,
        binding_delta ? binding_delta->root_constant_dirty_mask : UINT64_MAX,
        binding_delta ? binding_delta->root_descriptor_dirty_mask
                      : UINT64_MAX,
        test_telemetry);
  }
  if ((dirty_fields & CompiledBindingDirtyVertexBuffers) &&
      payload.vertex_binding_recipe) {
    EncodeCompiledVertexBindingRecipe(
        enc, *payload.vertex_binding_recipe, argbuf_offset,
        binding_delta ? binding_delta->vertex_buffer_dirty_mask
                      : UINT32_MAX);
  }
  // Native shaders consume descriptor tables and root arguments through the
  // same frozen root-base backing store. The recipe above only switches the
  // stage offset whose field mask changed.
  if (native)
    return;
  const bool shader_bindings_dirty =
      tables_dirty ||
      (dirty_fields & (CompiledBindingDirtyRootConstants |
                       CompiledBindingDirtyRootDescriptors));
  if (!shader_bindings_dirty)
    return;
  assert(payload.bindless_snapshot);
  if (!payload.bindless_snapshot)
    return;
  const auto &key = pipeline.GetShaderCacheKey();
  for (const auto &shader : pipeline.GetDxilShaders()) {
    if (shader.stage == PipelineShaderStage::Vertex) {
      EncodeShaderBindingsForStageBindlessSnapshot<PipelineStage::Vertex>(
          device, queue, enc, shader, key, *payload.bindless_snapshot, &diag);
    } else if (shader.stage == PipelineShaderStage::Pixel) {
      EncodeShaderBindingsForStageBindlessSnapshot<PipelineStage::Pixel>(
          device, queue, enc, shader, key, *payload.bindless_snapshot, &diag);
    }
  }
}

void EncodeCompiledComputeBindings(
    WMT::Device device, ::dxmt::CommandQueue &queue,
    ArgumentEncodingContext &enc,
    const CompiledDirectComputeBindingPayload &payload,
    PipelineState &pipeline,
    const CompiledBindingDelta *binding_delta,
    CompiledCommandTestTelemetry *test_telemetry) {
  const bool native = pipeline.GetShaderAbiVersion() ==
                      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  assert(!native || payload.native_binding_recipe);
  const uint32_t dirty_fields = binding_delta
                                    ? binding_delta->dirty_fields
                                    : UINT32_MAX;
  const bool tables_dirty =
      dirty_fields & (CompiledBindingDirtyPipelineLayout |
                      CompiledBindingDirtyResourceHeap |
                      CompiledBindingDirtySamplerHeap |
                      CompiledBindingDirtyRootTables);
  const bool native_recipe_dirty =
      tables_dirty ||
      (dirty_fields & (CompiledBindingDirtyRootConstants |
                       CompiledBindingDirtyRootDescriptors));
  if (native && native_recipe_dirty) {
    if (!payload.native_binding_recipe)
      return;
    EncodeCompiledNativeBindingRecipe(
        enc, *payload.native_binding_recipe, dirty_fields,
        binding_delta ? binding_delta->root_table_dirty_mask : UINT64_MAX,
        binding_delta ? binding_delta->root_constant_dirty_mask : UINT64_MAX,
        binding_delta ? binding_delta->root_descriptor_dirty_mask
                      : UINT64_MAX,
        test_telemetry);
  }
  RecordBindlessMirrorDiagDispatch();
  if (native)
    return;
  if (!tables_dirty &&
      !(dirty_fields & (CompiledBindingDirtyRootConstants |
                        CompiledBindingDirtyRootDescriptors)))
    return;
  assert(payload.bindless_snapshot);
  if (!payload.bindless_snapshot)
    return;
  const auto &key = pipeline.GetShaderCacheKey();
  for (const auto &shader : pipeline.GetDxilShaders()) {
    if (shader.stage != PipelineShaderStage::Compute)
      continue;
    EncodeShaderBindingsForStageBindlessSnapshot<PipelineStage::Compute>(
        device, queue, enc, shader, key, *payload.bindless_snapshot);
  }
}

} // namespace dxmt::d3d12
