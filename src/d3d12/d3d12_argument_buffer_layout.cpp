#include "d3d12_argument_buffer_layout.hpp"

#include "airconv_dx12_metal4.h"
#include "d3d12_indirect_topology.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

namespace {

constexpr uint64_t kArgumentBufferAlignment = 32ull;
constexpr uint64_t kMinimumArgumentAllocation = 8ull;

bool PipelineStageParticipates(PipelineShaderStage stage, bool use_geometry,
                               bool use_tessellation) {
  switch (stage) {
  case PipelineShaderStage::Vertex:
  case PipelineShaderStage::Pixel:
    return true;
  case PipelineShaderStage::Hull:
  case PipelineShaderStage::Domain:
    return use_tessellation;
  case PipelineShaderStage::Geometry:
    return use_geometry;
  default:
    return false;
  }
}

uint64_t GraphicsVertexBufferArgumentSize(PipelineState &pipeline) {
  const auto *graphics = pipeline.GetGraphicsState();
  if (!graphics)
    return 0;
  uint32_t slot_mask = 0;
  for (const auto &element : graphics->input_elements) {
    if (element.InputSlot < 32)
      slot_mask |= 1u << element.InputSlot;
  }
  if (!slot_mask)
    return 0;
  return uint64_t(__builtin_popcount(slot_mask)) * 16u;
}

} // namespace

uint64_t AllocateArgumentBuffer(uint64_t &cursor, uint64_t size) {
  if (cursor > UINT64_MAX - (kArgumentBufferAlignment - 1)) {
    cursor = UINT64_MAX;
    return UINT64_MAX;
  }
  const auto aligned = (cursor + kArgumentBufferAlignment - 1) &
                       ~(kArgumentBufferAlignment - 1);
  const auto allocation_size =
      std::max<uint64_t>(size, kMinimumArgumentAllocation);
  if (allocation_size > UINT64_MAX - aligned) {
    cursor = UINT64_MAX;
    return UINT64_MAX;
  }
  cursor = aligned + allocation_size;
  return aligned;
}

uint64_t AlignArgumentBufferSize(uint64_t size) {
  if (size > UINT64_MAX - (kArgumentBufferAlignment - 1))
    return UINT64_MAX;
  return (size + kArgumentBufferAlignment - 1) &
         ~(kArgumentBufferAlignment - 1);
}

uint64_t AdvanceArgumentBufferEstimate(uint64_t cursor, uint64_t size) {
  AllocateArgumentBuffer(cursor, size);
  return cursor;
}

uint64_t
EstimateShaderArgumentBufferSize(const PipelineDxilShader &shader) {
  uint64_t size = 0;
  if (shader.reflection().NumConstantBuffers)
    size = AdvanceArgumentBufferEstimate(
        size, uint64_t(shader.reflection().NumConstantBuffers) << 3);
  if (shader.reflection().NumArguments)
    size = AdvanceArgumentBufferEstimate(
        size, uint64_t(shader.reflection().ArgumentTableQwords) << 3);
  return AlignArgumentBufferSize(size);
}

uint64_t EstimateGraphicsArgumentBufferSize(PipelineState &pipeline,
                                           bool use_geometry,
                                           bool use_tessellation) {
  uint64_t size = 0;
  if (const auto vertex_size = GraphicsVertexBufferArgumentSize(pipeline))
    size = AdvanceArgumentBufferEstimate(size, vertex_size);

  const bool native = pipeline.GetShaderAbiVersion() ==
                      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  if (native)
    return AlignArgumentBufferSize(size);

  for (const auto &shader : pipeline.GetDxilShaders()) {
    if (!PipelineStageParticipates(shader.stage, use_geometry,
                                   use_tessellation))
      continue;
    if (const auto shader_size = EstimateShaderArgumentBufferSize(shader))
      size = AdvanceArgumentBufferEstimate(size, shader_size);
  }
  return AlignArgumentBufferSize(size);
}

uint64_t EstimateComputeArgumentBufferSize(PipelineState &pipeline) {
  if (pipeline.GetShaderAbiVersion() ==
      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE)
    return 0;
  uint64_t size = 0;
  for (const auto &shader : pipeline.GetDxilShaders()) {
    if (shader.stage == PipelineShaderStage::Compute)
      if (const auto shader_size = EstimateShaderArgumentBufferSize(shader))
        size = AdvanceArgumentBufferEstimate(size, shader_size);
  }
  return AlignArgumentBufferSize(size);
}

uint64_t EstimateDrawArgumentBufferSize(bool indexed) {
  return indexed
             ? AlignArgumentBufferSize(sizeof(DXMT_DRAW_INDEXED_ARGUMENTS))
             : AlignArgumentBufferSize(sizeof(DXMT_DRAW_ARGUMENTS));
}

} // namespace dxmt::d3d12
