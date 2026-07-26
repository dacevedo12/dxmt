#include "d3d12_shader_binding.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>

namespace dxmt::d3d12 {

namespace {

PipelineShaderStage DxilShaderStageForPipelineStage(PipelineStage stage) {
  switch (stage) {
  case PipelineStage::Compute:
    return PipelineShaderStage::Compute;
  case PipelineStage::Pixel:
    return PipelineShaderStage::Pixel;
  case PipelineStage::Geometry:
    return PipelineShaderStage::Geometry;
  case PipelineStage::Hull:
    return PipelineShaderStage::Hull;
  case PipelineStage::Domain:
    return PipelineShaderStage::Domain;
  case PipelineStage::Vertex:
  default:
    return PipelineShaderStage::Vertex;
  }
}

struct ShaderArgumentTable {
  const DXMT12_MTL4_SHADER_ARGUMENT *arguments = nullptr;
  UINT count = 0;
};

ShaderArgumentTable
ShaderArgumentTableForBindingType(const PipelineDxilShader &shader,
                                  SM50BindingType binding_type) {
  if (binding_type == SM50BindingType::ConstantBuffer)
    return {shader.constantBufferInfo(),
            shader.reflection().NumConstantBuffers};
  return {shader.resourceArgumentInfo(), shader.reflection().NumArguments};
}

bool ShaderArgumentCoversRegister(const DXMT12_MTL4_SHADER_ARGUMENT &argument,
                                  UINT shader_register, UINT register_space,
                                  UINT &index) {
  const auto lower = argument.RegisterCount ? argument.RegisterLowerBound
                                            : argument.SM50BindingSlot;
  const auto space = argument.RegisterCount ? argument.RegisterSpace : 0;
  const auto count = argument.RegisterCount ? argument.RegisterCount : 1;
  if (space != register_space || shader_register < lower)
    return false;
  const auto local_index = shader_register - lower;
  if (count != UINT_MAX && local_index >= count)
    return false;
  index = local_index;
  return true;
}

} // namespace

const PipelineDxilShader *
FindShaderForStage(const PipelineState &pipeline, PipelineStage stage) {
  const auto shader_stage = DxilShaderStageForPipelineStage(stage);
  for (const auto &shader : pipeline.GetDxilShaders()) {
    if (shader.stage == shader_stage)
      return &shader;
  }
  return nullptr;
}

SM50BindingType
BindingTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
  switch (range_type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
    return SM50BindingType::ConstantBuffer;
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    return SM50BindingType::Sampler;
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
    return SM50BindingType::UAV;
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
  default:
    return SM50BindingType::SRV;
  }
}

UINT ShaderBindingSlotCapacity(SM50BindingType binding_type) {
  switch (binding_type) {
  case SM50BindingType::ConstantBuffer:
    return 14u;
  case SM50BindingType::Sampler:
    return 16u;
  case SM50BindingType::UAV:
    return kUAVBindings;
  case SM50BindingType::SRV:
  default:
    return kSRVBindings;
  }
}

UINT
ShaderArgumentQwordStride(const DXMT12_MTL4_SHADER_ARGUMENT &argument) {
  if (argument.Type == SM50BindingType::Sampler)
    return 3u;

  UINT stride = 1u;
  if (argument.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE)
    stride = 2u;
  else if (argument.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER)
    stride = 2u;

  if (argument.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER)
    stride = std::max(stride, 3u);
  return stride;
}

UINT
ShaderArgumentRangeCount(const DXMT12_MTL4_SHADER_ARGUMENT &argument) {
  const auto count = argument.RegisterCount ? argument.RegisterCount : 1u;
  if (count != UINT_MAX)
    return std::max<UINT>(count, 1u);

  const auto capacity = ShaderBindingSlotCapacity(argument.Type);
  if (argument.SM50BindingSlot >= capacity)
    return 0u;
  return capacity - argument.SM50BindingSlot;
}

std::optional<DescriptorShaderRangeOverlap>
IntersectDescriptorRangeWithShaderArgument(UINT shader_lower_bound,
                                           UINT shader_range_count,
                                           UINT descriptor_lower_bound,
                                           UINT descriptor_count) {
  if (!shader_range_count || !descriptor_count)
    return std::nullopt;

  const uint64_t shader_first = shader_lower_bound;
  const uint64_t shader_last = shader_first + uint64_t(shader_range_count);
  const uint64_t descriptor_first = descriptor_lower_bound;
  const uint64_t descriptor_last =
      descriptor_count == UINT_MAX
          ? UINT64_MAX
          : descriptor_first + uint64_t(descriptor_count);
  const uint64_t first = std::max(shader_first, descriptor_first);
  const uint64_t last = std::min(shader_last, descriptor_last);
  if (first >= last)
    return std::nullopt;

  const uint64_t descriptor_index = first - descriptor_first;
  const uint64_t argument_local_start = first - shader_first;
  const uint64_t count = last - first;
  if (descriptor_index > UINT_MAX || argument_local_start > UINT_MAX ||
      count > UINT_MAX)
    return std::nullopt;

  return DescriptorShaderRangeOverlap{UINT(descriptor_index),
                                      UINT(argument_local_start),
                                      UINT(count)};
}

DXMT12_MTL4_SHADER_ARGUMENT
ShaderArgumentAtRangeOffset(const DXMT12_MTL4_SHADER_ARGUMENT &argument,
                            UINT offset) {
  auto result = argument;
  result.SM50BindingSlot += offset;
  result.RegisterLowerBound += offset;
  result.StructurePtrOffset += offset * ShaderArgumentQwordStride(argument);
  result.RegisterCount = 1u;
  return result;
}

std::optional<UINT>
ResolveShaderBindingSlot(const PipelineState &pipeline, PipelineStage stage,
                         SM50BindingType binding_type, UINT shader_register,
                         UINT register_space) {
  const auto *shader = FindShaderForStage(pipeline, stage);
  if (!shader)
    return std::nullopt;

  const auto table = ShaderArgumentTableForBindingType(*shader, binding_type);
  if (!table.arguments)
    return std::nullopt;

  for (UINT i = 0; i < table.count; i++) {
    const auto &argument = table.arguments[i];
    if (argument.Type != binding_type)
      continue;
    UINT index = 0;
    if (!ShaderArgumentCoversRegister(argument, shader_register,
                                      register_space, index))
      continue;
    return argument.SM50BindingSlot + index;
  }
  return std::nullopt;
}

const DXMT12_MTL4_SHADER_ARGUMENT *
ResolveShaderBindingArgument(const PipelineState &pipeline,
                             PipelineStage stage,
                             SM50BindingType binding_type,
                             UINT shader_register, UINT register_space) {
  const auto *shader = FindShaderForStage(pipeline, stage);
  if (!shader)
    return nullptr;

  const auto table = ShaderArgumentTableForBindingType(*shader, binding_type);
  if (!table.arguments)
    return nullptr;

  for (UINT i = 0; i < table.count; i++) {
    const auto &argument = table.arguments[i];
    if (argument.Type != binding_type)
      continue;
    UINT index = 0;
    if (!ShaderArgumentCoversRegister(argument, shader_register,
                                      register_space, index))
      continue;
    return &argument;
  }
  return nullptr;
}

const DXMT12_MTL4_SHADER_ARGUMENT *
ResolveShaderBindingArgumentBySlot(const PipelineState &pipeline,
                                   PipelineStage stage,
                                   SM50BindingType binding_type,
                                   UINT binding_slot) {
  const auto *shader = FindShaderForStage(pipeline, stage);
  if (!shader)
    return nullptr;

  const auto table = ShaderArgumentTableForBindingType(*shader, binding_type);
  if (!table.arguments)
    return nullptr;

  for (UINT i = 0; i < table.count; i++) {
    const auto &argument = table.arguments[i];
    if (argument.Type == binding_type &&
        argument.SM50BindingSlot == binding_slot)
      return &argument;
  }
  return nullptr;
}

} // namespace dxmt::d3d12
