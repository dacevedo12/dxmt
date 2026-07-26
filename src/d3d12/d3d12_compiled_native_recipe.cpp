#include "d3d12_compiled_native_recipe.hpp"

#include "d3d12_command_list.hpp"
#include "d3d12_shader_binding.hpp"
#include "d3d12_shader_stage_query.hpp"

#include "airconv_dx12_metal4.h"

namespace dxmt::d3d12 {

std::shared_ptr<const CompiledNativeBindingRecipe>
BuildCompiledNativeBindingRecipe(
    const PipelineState &pipeline, bool compute,
    const SubmittedFrozenNativeDescriptorStore &frozen,
    std::initializer_list<
        std::pair<PipelineStage, const CompiledNativeStageBinding *>>
        stages) {
  if (!frozen.ready)
    return {};
  auto recipe = std::make_shared<CompiledNativeBindingRecipe>();
  auto add_argument_buffer = [&](WMT::Buffer buffer, uint64_t offset,
                                 uint32_t index, WMTRenderStages render_stages,
                                 uint32_t required_dirty_fields,
                                 uint64_t root_table_mask = 0,
                                 uint64_t root_constant_mask = 0,
                                 uint64_t root_descriptor_mask = 0) {
    if (!buffer)
      return;
    recipe->ops.push_back(CompiledNativeBindingRecipe::Op{
        CompiledNativeBindingOpKind::ArgumentBuffer,
        WMT::Reference<WMT::Buffer>(buffer), offset, index,
        required_dirty_fields, root_table_mask, root_constant_mask,
        root_descriptor_mask, compute, render_stages});
  };

  const WMTRenderStages all_render_stages =
      compute ? WMTRenderStages{}
              : WMTRenderStageVertex | WMTRenderStageFragment;
  add_argument_buffer(frozen.descriptor_table_buffer,
                      frozen.descriptor_table_buffer_offset,
                      DXMT12_MTL4_NATIVE_DESCRIPTOR_HEAP_BIND_INDEX,
                      all_render_stages, CompiledBindingDirtyResourceHeap);
  add_argument_buffer(frozen.descriptor_table_buffer,
                      frozen.descriptor_table_buffer_offset,
                      DXMT12_MTL4_NATIVE_SAMPLER_HEAP_BIND_INDEX,
                      all_render_stages, CompiledBindingDirtySamplerHeap);
  add_argument_buffer(frozen.buffer_resource_table_buffer,
                      frozen.buffer_resource_table_buffer_offset,
                      DXMT12_MTL4_NATIVE_BUFFER_RESOURCE_TABLE_BIND_INDEX,
                      all_render_stages, CompiledBindingDirtyResourceHeap);
  add_argument_buffer(frozen.buffer_record_buffer,
                      frozen.buffer_record_buffer_offset,
                      DXMT12_MTL4_NATIVE_BUFFER_DESCRIPTOR_RECORD_BIND_INDEX,
                      all_render_stages, CompiledBindingDirtyResourceHeap);

  for (const auto &[stage, binding] : stages) {
    if (!binding || !binding->ready)
      continue;
    const auto *shader = FindShaderForStage(pipeline, stage);
    if (!shader)
      continue;
    const WMTRenderStages render_stages =
        compute ? WMTRenderStages{}
                : stage == PipelineStage::Vertex
                      ? WMTRenderStageVertex
                      : stage == PipelineStage::Pixel ? WMTRenderStageFragment
                                                      : WMTRenderStages{};
    if (shader->reflection().NumConstantBuffers) {
      recipe->ops.push_back(CompiledNativeBindingRecipe::Op{
          CompiledNativeBindingOpKind::NullConstantBuffer, {}, 0, 0,
          CompiledBindingDirtyPipelineLayout, 0, 0, 0, compute,
          render_stages});
    }
    if (NativeShaderUsesBufferSrv(*shader)) {
      recipe->ops.push_back(CompiledNativeBindingRecipe::Op{
          CompiledNativeBindingOpKind::NullBuffer, {}, 0, 0,
          CompiledBindingDirtyPipelineLayout, 0, 0, 0, compute,
          render_stages});
    }
    if (binding->cbuffer_root_base_count) {
      add_argument_buffer(
          frozen.root_base_buffer, binding->cbuffer_root_base_offset,
          DXMT12_MTL4_NATIVE_CBUFFER_ROOT_TABLE_BASE_BIND_INDEX, render_stages,
          CompiledBindingDirtyPipelineLayout |
              CompiledBindingDirtyRootTables |
              CompiledBindingDirtyRootConstants |
              CompiledBindingDirtyRootDescriptors,
          binding->cbuffer_root_table_mask,
          binding->cbuffer_root_constant_mask,
          binding->cbuffer_root_descriptor_mask);
    }
    if (binding->resource_root_base_count) {
      add_argument_buffer(
          frozen.root_base_buffer, binding->resource_root_base_offset,
          DXMT12_MTL4_NATIVE_ROOT_TABLE_BASE_BIND_INDEX, render_stages,
          CompiledBindingDirtyPipelineLayout |
              CompiledBindingDirtyRootTables |
              CompiledBindingDirtyRootConstants |
              CompiledBindingDirtyRootDescriptors,
          binding->resource_root_table_mask,
          binding->resource_root_constant_mask,
          binding->resource_root_descriptor_mask);
    }
  }
  return recipe;
}

} // namespace dxmt::d3d12
