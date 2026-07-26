#pragma once

// Pure value types describing the per-stage bindless-mirror / native root-base
// binding plans, plus the stateless shader-argument queries used to build them.
// Nothing here touches the command queue instance, so it can be compiled and
// analysed on its own.

#include "airconv_dx12_metal4.h"
#include "d3d12_shader_binding.hpp"
#include "dxmt_context.hpp"
#include "dxmt_gpu_lifetime.hpp"

#include <cstdint>
#include <d3d12.h>
#include <vector>

namespace dxmt::d3d12 {

/** Argument-buffer slices holding one stage's mirrored texture/sampler tables. */
struct BindlessMirrorWindow {
  AllocatedArgumentBufferSlice texture;
  AllocatedArgumentBufferSlice sampler;
  uint32_t texture_field_pairs = 0;
};

struct BindlessMirrorStagePlanEntry {
  uint32_t root_index = 0;
  uint32_t range_offset = 0;
  uint32_t descriptor_index = 0;
  uint32_t descriptor_count = 0;
  uint32_t shader_register_lower_bound = 0;
  uint32_t argument_local_start = 0;
  uint32_t root_offset_key = 0;
  uint32_t compact_base = 0;
  uint32_t range_count = 0;
  uint16_t argument_index = 0;
  D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  bool sampler = false;
  bool texture = false;
};

struct BindlessMirrorStaticSamplerPlanEntry {
  D3D12_STATIC_SAMPLER_DESC desc = {};
  uint32_t root_offset_key = 0;
  uint32_t compact_base = 0;
  uint32_t compact_slot = 0;
};

struct BindlessMirrorStagePlan {
  uint64_t root_identity = 0;
  uint64_t pipeline_identity = 0;
  PipelineStage stage = PipelineStage::Vertex;
  bool compute = false;
  uint32_t max_key_plus_one = 0;
  uint32_t texture_count = 0;
  uint32_t sampler_count = 0;
  uint32_t texture_field_pairs = 0;
  std::vector<BindlessMirrorStagePlanEntry> entries;
  std::vector<BindlessMirrorStaticSamplerPlanEntry> static_samplers;
};

struct NativeRootBaseStagePlanEntry {
  enum class Source : uint8_t {
    DescriptorTable,
    RootConstants,
    RootDescriptor,
  };

  uint32_t root_index = 0;
  uint32_t range_offset = 0;
  uint32_t descriptor_index = 0;
  uint32_t descriptor_count = 0;
  uint32_t argument_local_start = 0;
  uint32_t root_base_key = 0;
  uint32_t range_count = 0;
  uint16_t argument_index = 0;
  D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  Source source = Source::DescriptorTable;
  bool cbuffer = false;
};

struct NativeRootBaseStagePlanGroup {
  uint32_t root_base_key = 0;
  uint32_t range_length = 0;
  uint32_t captured_capacity = 0;
  bool cbuffer = false;
  std::vector<uint32_t> entry_indices;
};

struct NativeRootBaseStagePlan {
  uint64_t root_identity = 0;
  uint64_t pipeline_identity = 0;
  PipelineStage stage = PipelineStage::Vertex;
  bool compute = false;
  uint32_t max_resource_key_plus_one = 0;
  uint32_t max_cbuffer_key_plus_one = 0;
  std::vector<NativeRootBaseStagePlanEntry> entries;
  std::vector<NativeRootBaseStagePlanGroup> groups;
};

/** SRV/UAV arguments that carry a texture handle participate in the mirror. */
[[nodiscard]] bool
IsBindlessTextureMirrorArgument(const MTL_SM50_SHADER_ARGUMENT &arg);

/** Number of (handle, metadata) qword pairs the texture mirror window needs. */
[[nodiscard]] uint32_t
CountBindlessTextureMirrorFieldPairs(const MTL_SM50_SHADER_ARGUMENT *arguments,
                                     uint32_t argument_count);

/** First reflected argument of `type` covering (space, register), or null. */
[[nodiscard]] const MTL_SM50_SHADER_ARGUMENT *
FindBindlessMirrorArgument(const MTL_SM50_SHADER_ARGUMENT *arguments,
                           uint32_t argument_count, SM50BindingType type,
                           UINT shader_register, UINT register_space);

} // namespace dxmt::d3d12
