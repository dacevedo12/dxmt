#pragma once

#include "airconv_dx12_metal4.h"
#include "d3d12_pipeline.hpp"
#include "dxmt_context.hpp"

#include <d3d12.h>
#include <optional>

namespace dxmt::d3d12 {

struct DescriptorShaderRangeOverlap {
  UINT descriptor_index = 0;
  UINT argument_local_start = 0;
  UINT count = 0;
};

template <typename Fn>
void
ForEachVisibleStage(D3D12_SHADER_VISIBILITY visibility, bool compute,
                    const Fn &fn) {
  if (compute) {
    fn(PipelineStage::Compute);
    return;
  }

  switch (visibility) {
  case D3D12_SHADER_VISIBILITY_VERTEX:
    fn(PipelineStage::Vertex);
    break;
  case D3D12_SHADER_VISIBILITY_PIXEL:
    fn(PipelineStage::Pixel);
    break;
  case D3D12_SHADER_VISIBILITY_GEOMETRY:
    fn(PipelineStage::Geometry);
    break;
  case D3D12_SHADER_VISIBILITY_HULL:
    fn(PipelineStage::Hull);
    break;
  case D3D12_SHADER_VISIBILITY_DOMAIN:
    fn(PipelineStage::Domain);
    break;
  case D3D12_SHADER_VISIBILITY_ALL:
  default:
    fn(PipelineStage::Vertex);
    fn(PipelineStage::Pixel);
    fn(PipelineStage::Geometry);
    fn(PipelineStage::Hull);
    fn(PipelineStage::Domain);
    break;
  }
}

[[nodiscard]] const PipelineDxilShader *
FindShaderForStage(const PipelineState &pipeline, PipelineStage stage);

[[nodiscard]] SM50BindingType
BindingTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type);

[[nodiscard]] UINT ShaderBindingSlotCapacity(SM50BindingType binding_type);

[[nodiscard]] UINT
ShaderArgumentQwordStride(const DXMT12_MTL4_SHADER_ARGUMENT &argument);

[[nodiscard]] UINT
ShaderArgumentRangeCount(const DXMT12_MTL4_SHADER_ARGUMENT &argument);

[[nodiscard]] std::optional<DescriptorShaderRangeOverlap>
IntersectDescriptorRangeWithShaderArgument(UINT shader_lower_bound,
                                           UINT shader_range_count,
                                           UINT descriptor_lower_bound,
                                           UINT descriptor_count);

[[nodiscard]] DXMT12_MTL4_SHADER_ARGUMENT
ShaderArgumentAtRangeOffset(const DXMT12_MTL4_SHADER_ARGUMENT &argument,
                            UINT offset);

[[nodiscard]] std::optional<UINT>
ResolveShaderBindingSlot(const PipelineState &pipeline, PipelineStage stage,
                         SM50BindingType binding_type, UINT shader_register,
                         UINT register_space);

[[nodiscard]] const DXMT12_MTL4_SHADER_ARGUMENT *
ResolveShaderBindingArgument(const PipelineState &pipeline,
                             PipelineStage stage,
                             SM50BindingType binding_type,
                             UINT shader_register, UINT register_space);

[[nodiscard]] const DXMT12_MTL4_SHADER_ARGUMENT *
ResolveShaderBindingArgumentBySlot(const PipelineState &pipeline,
                                   PipelineStage stage,
                                   SM50BindingType binding_type,
                                   UINT binding_slot);

} // namespace dxmt::d3d12
