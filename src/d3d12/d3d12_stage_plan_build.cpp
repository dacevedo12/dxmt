#include "d3d12_stage_plan_build.hpp"

#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_shader_binding.hpp"
#include "dxmt_bindless_buffer_table.hpp"

#include <algorithm>
#include <cstdint>

namespace dxmt::d3d12 {

namespace {

// An unbounded descriptor range reflects to at most this many descriptors; the
// cap keeps a bogus reflection from turning into an unbounded table walk.
constexpr uint64_t kReflectedDescriptorRangeCountLimit = 4096u;

} // namespace

UINT
ReflectedDescriptorRangeCount(const PipelineState &pipeline,
                              const RootSignatureRange &range,
                              D3D12_SHADER_VISIBILITY visibility,
                              bool compute) {
  uint64_t count = 0;
  const auto binding_type = BindingTypeForRange(range.range_type);
  ForEachVisibleStage(
      visibility, compute, [&](PipelineStage stage) {
        const auto *shader = FindShaderForStage(pipeline, stage);
        if (!shader)
          return;
        const auto *arguments =
            binding_type == SM50BindingType::ConstantBuffer
                ? shader->constantBufferInfo()
                : shader->resourceArgumentInfo();
        const auto argument_count =
            binding_type == SM50BindingType::ConstantBuffer
                ? shader->reflection().NumConstantBuffers
                : shader->reflection().NumArguments;
        if (!arguments)
          return;
        for (UINT i = 0; i < argument_count; i++) {
          const auto &argument = arguments[i];
          if (argument.Type != binding_type)
            continue;
          const auto space =
              argument.RegisterCount ? argument.RegisterSpace : 0;
          const auto lower = argument.RegisterCount
                                 ? argument.RegisterLowerBound
                                 : argument.SM50BindingSlot;
          if (space != range.register_space)
            continue;
          const auto size = ShaderArgumentRangeCount(argument);
          const auto overlap = IntersectDescriptorRangeWithShaderArgument(
              lower, size, range.base_shader_register, UINT_MAX);
          if (!overlap)
            continue;
          count = std::max<uint64_t>(
              count, uint64_t(overlap->descriptor_index) + overlap->count);
        }
      });
  return count ? UINT(std::min<uint64_t>(
                     count, kReflectedDescriptorRangeCountLimit))
               : 1u;
}

BindingPlan
BuildBindingPlan(RootSignature *root, PipelineState &pipeline, bool compute) {
  BindingPlan plan;
  plan.compute = compute;
  plan.root_identity = root->GetCacheIdentity();
  plan.pipeline_identity = pipeline.GetCacheIdentity();
  const auto parameters = root->GetParameters();
  for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
    const auto &parameter = parameters[root_index];
    if (parameter.parameter_type ==
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      UINT running_offset = 0;
      for (const auto &range : parameter.ranges) {
        const auto range_offset = DescriptorRangeOffset(range, running_offset);
        const auto count =
            range.descriptor_count == UINT_MAX
                ? ReflectedDescriptorRangeCount(pipeline, range,
                                                parameter.visibility, compute)
                : range.descriptor_count;
        const auto dh_type = DescriptorHeapTypeForRange(range.range_type);
        ForEachVisibleStage(
            parameter.visibility, compute, [&](PipelineStage stage) {
              const auto binding_type = BindingTypeForRange(range.range_type);
              const auto *shader = FindShaderForStage(pipeline, stage);
              if (!shader)
                return;
              const auto *arguments =
                  binding_type == SM50BindingType::ConstantBuffer
                      ? shader->constantBufferInfo()
                      : shader->resourceArgumentInfo();
              const auto argument_count =
                  binding_type == SM50BindingType::ConstantBuffer
                      ? shader->reflection().NumConstantBuffers
                      : shader->reflection().NumArguments;
              if (!arguments)
                return;
              for (UINT arg_index = 0; arg_index < argument_count;
                   arg_index++) {
                const auto &argument = arguments[arg_index];
                if (argument.Type != binding_type)
                  continue;
                const auto space =
                    argument.RegisterCount ? argument.RegisterSpace : 0;
                const auto lower = argument.RegisterCount
                                       ? argument.RegisterLowerBound
                                       : argument.SM50BindingSlot;
                const auto resolved_count =
                    ShaderArgumentRangeCount(argument);
                if (space != range.register_space ||
                    lower + resolved_count < lower)
                  continue;
                for (UINT i = 0; i < resolved_count; i++) {
                  const auto shader_register = lower + i;
                  if (shader_register < range.base_shader_register)
                    continue;
                  const auto descriptor_index =
                      shader_register - range.base_shader_register;
                  if (descriptor_index >= count)
                    continue;
                  plan.entries.push_back(BindingPlanEntry{
                      BindingEntryKind::Table, stage, root_index, range_offset,
                      descriptor_index, count, dh_type, range.range_type,
                      DescriptorRecordType::Empty});
                }
              }
            });
        if (range.descriptor_count != UINT_MAX)
          running_offset = range_offset + range.descriptor_count;
      }
    } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
      ForEachVisibleStage(parameter.visibility, compute,
                          [&](PipelineStage stage) {
                            plan.entries.push_back(BindingPlanEntry{
                                BindingEntryKind::RootBuffer, stage, root_index,
                                0, 0, 0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                D3D12_DESCRIPTOR_RANGE_TYPE_CBV,
                                DescriptorRecordType::ConstantBufferView});
                          });
    } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
      ForEachVisibleStage(parameter.visibility, compute,
                          [&](PipelineStage stage) {
                            plan.entries.push_back(BindingPlanEntry{
                                BindingEntryKind::RootBuffer, stage, root_index,
                                0, 0, 0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                D3D12_DESCRIPTOR_RANGE_TYPE_SRV,
                                DescriptorRecordType::ShaderResourceView});
                          });
    } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
      ForEachVisibleStage(parameter.visibility, compute,
                          [&](PipelineStage stage) {
                            plan.entries.push_back(BindingPlanEntry{
                                BindingEntryKind::RootBuffer, stage, root_index,
                                0, 0, 0, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
                                DescriptorRecordType::UnorderedAccessView});
                          });
    }
  }
  return plan;
}

DescriptorTableBindingRecipe
BuildDescriptorTableBindingRecipe(const PipelineState &pipeline,
                                  const RootSignature &root, bool compute) {
  DescriptorTableBindingRecipe recipe = {};
  const auto parameters = root.GetParameters();
  for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
    const auto &parameter = parameters[root_index];
    if (parameter.parameter_type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
      continue;

    UINT running_offset = 0;
    for (UINT range_index = 0; range_index < parameter.ranges.size();
         range_index++) {
      const auto &range = parameter.ranges[range_index];
      const auto range_offset = DescriptorRangeOffset(range, running_offset);
      const auto count =
          range.descriptor_count == UINT_MAX
              ? ReflectedDescriptorRangeCount(
                    pipeline, range, parameter.visibility, compute)
              : range.descriptor_count;
      ForEachVisibleStage(
          parameter.visibility, compute, [&](PipelineStage stage) {
            const auto binding_type = BindingTypeForRange(range.range_type);
            const auto *shader = FindShaderForStage(pipeline, stage);
            if (!shader)
              return;
            const auto *arguments =
                binding_type == SM50BindingType::ConstantBuffer
                    ? shader->constantBufferInfo()
                    : shader->resourceArgumentInfo();
            const auto argument_count =
                binding_type == SM50BindingType::ConstantBuffer
                    ? shader->reflection().NumConstantBuffers
                    : shader->reflection().NumArguments;
            if (!arguments)
              return;

            for (UINT arg_index = 0; arg_index < argument_count;
                 arg_index++) {
              const auto &argument = arguments[arg_index];
              if (argument.Type != binding_type)
                continue;
              const auto space =
                  argument.RegisterCount ? argument.RegisterSpace : 0;
              const auto lower =
                  argument.RegisterCount ? argument.RegisterLowerBound
                                         : argument.SM50BindingSlot;
              const auto resolved_count =
                  ShaderArgumentRangeCount(argument);
              if (space != range.register_space ||
                  lower + resolved_count < lower)
                continue;

              for (UINT i = 0; i < resolved_count; i++) {
                const auto shader_register = lower + i;
                if (shader_register < range.base_shader_register)
                  continue;
                const auto descriptor_index =
                    shader_register - range.base_shader_register;
                if (descriptor_index >= count)
                  continue;

                DescriptorTableBindingRecipeEntry entry = {};
                entry.root_index = root_index;
                entry.range_index = range_index;
                entry.stage = uint16_t(stage);
                entry.slot = argument.SM50BindingSlot + i;
                entry.range_offset = range_offset;
                entry.descriptor_index = descriptor_index;
                entry.descriptor_count = count;
                entry.range_type = range.range_type;
                entry.shader_register = shader_register;
                entry.register_lower_bound = lower;
                entry.root_offset_key = argument.StructurePtrOffset;
                entry.argument = ShaderArgumentAtRangeOffset(argument, i);
                recipe.entries.push_back(entry);
              }
            }
          });
      if (range.descriptor_count != UINT_MAX)
        running_offset = range_offset + range.descriptor_count;
    }
  }
  return recipe;
}

BindlessMirrorStagePlan
BuildBindlessMirrorStagePlan(const PipelineState &pipeline,
                             const RootSignature &root,
                             PipelineStage want_stage, bool compute) {
  BindlessMirrorStagePlan plan = {};
  plan.root_identity = root.GetCacheIdentity();
  plan.pipeline_identity = pipeline.GetCacheIdentity();
  plan.stage = want_stage;
  plan.compute = compute;

  const auto *shader = FindShaderForStage(pipeline, want_stage);
  if (!shader)
    return plan;
  const auto *arguments = shader->resourceArgumentInfo();
  const auto argument_count = shader->reflection().NumArguments;
  if (!arguments || !argument_count)
    return plan;

  std::vector<uint32_t> texture_bases(argument_count, UINT_MAX);
  std::vector<uint32_t> sampler_bases(argument_count, UINT_MAX);
  std::vector<uint32_t> static_sampler_bases(argument_count, UINT_MAX);
  plan.texture_field_pairs =
      CountBindlessTextureMirrorFieldPairs(arguments, argument_count);

  for (UINT i = 0; i < argument_count; i++) {
    const auto &arg = arguments[i];
    const bool is_sampler = arg.Type == SM50BindingType::Sampler;
    const bool is_texture = IsBindlessTextureMirrorArgument(arg);
    if (!is_sampler && !is_texture)
      continue;

    plan.max_key_plus_one =
        std::max<uint32_t>(plan.max_key_plus_one,
                           arg.StructurePtrOffset + 1);
    const auto count = ShaderArgumentRangeCount(arg);
    if (is_sampler) {
      sampler_bases[i] = plan.sampler_count;
      plan.sampler_count = std::min<uint32_t>(
          dxmt::kBindlessMirrorCapacity, plan.sampler_count + count);
    } else {
      texture_bases[i] = plan.texture_count;
      plan.texture_count = std::min<uint32_t>(
          dxmt::kBindlessMirrorCapacity, plan.texture_count + count);
    }
  }

  for (const auto &sampler_desc : root.GetStaticSamplers()) {
    bool visible = false;
    ForEachVisibleStage(sampler_desc.ShaderVisibility, compute,
                        [&](PipelineStage s) {
                          if (s == want_stage)
                            visible = true;
                        });
    if (!visible)
      continue;

    for (UINT i = 0; i < argument_count; i++) {
      const auto &arg = arguments[i];
      if (arg.Type != SM50BindingType::Sampler)
        continue;
      const auto space = arg.RegisterCount ? arg.RegisterSpace : 0;
      const auto lower =
          arg.RegisterCount ? arg.RegisterLowerBound : arg.SM50BindingSlot;
      if (space != sampler_desc.RegisterSpace ||
          sampler_desc.ShaderRegister < lower)
        continue;
      const auto local = sampler_desc.ShaderRegister - lower;
      const auto count = ShaderArgumentRangeCount(arg);
      if (count != UINT_MAX && local >= count)
        continue;
      if (static_sampler_bases[i] == UINT_MAX) {
        static_sampler_bases[i] = plan.sampler_count;
        plan.sampler_count = std::min<uint32_t>(
            dxmt::kBindlessMirrorCapacity, plan.sampler_count + count);
      }
      const auto slot = static_sampler_bases[i] + local;
      if (slot < dxmt::kBindlessMirrorCapacity) {
        plan.static_samplers.push_back(BindlessMirrorStaticSamplerPlanEntry{
            sampler_desc, arg.StructurePtrOffset, static_sampler_bases[i],
            slot});
        plan.max_key_plus_one =
            std::max<uint32_t>(plan.max_key_plus_one,
                               arg.StructurePtrOffset + 1);
      }
      break;
    }
  }

  const auto parameters = root.GetParameters();
  for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
    const auto &parameter = parameters[root_index];
    bool visible = false;
    ForEachVisibleStage(parameter.visibility, compute, [&](PipelineStage s) {
      if (s == want_stage)
        visible = true;
    });
    if (!visible)
      continue;

    if (parameter.parameter_type !=
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
      continue;

    UINT running_offset = 0;
    for (const auto &range : parameter.ranges) {
      const auto range_offset = DescriptorRangeOffset(range, running_offset);
      const auto count =
          range.descriptor_count == UINT_MAX
              ? ReflectedDescriptorRangeCount(
                    pipeline, range, parameter.visibility, compute)
              : range.descriptor_count;
      const auto binding_type = BindingTypeForRange(range.range_type);
      const auto heap_type = DescriptorHeapTypeForRange(range.range_type);

      for (UINT i = 0; i < argument_count; i++) {
        const auto &argument = arguments[i];
        if (argument.Type != binding_type)
          continue;
        const bool is_sampler = argument.Type == SM50BindingType::Sampler;
        const bool is_texture = IsBindlessTextureMirrorArgument(argument);
        if (!is_sampler && !is_texture)
          continue;
        const auto space = argument.RegisterCount ? argument.RegisterSpace : 0;
        const auto lower = argument.RegisterCount ? argument.RegisterLowerBound
                                                  : argument.SM50BindingSlot;
        const auto argument_range_count = ShaderArgumentRangeCount(argument);
        const auto overlap = IntersectDescriptorRangeWithShaderArgument(
            lower, argument_range_count, range.base_shader_register, count);
        if (space != range.register_space || !overlap)
          continue;
        const auto compact_base =
            is_sampler ? sampler_bases[i] : texture_bases[i];
        if (compact_base == UINT_MAX)
          continue;
        plan.entries.push_back(BindlessMirrorStagePlanEntry{
            root_index,
            range_offset,
            overlap->descriptor_index,
            count,
            lower,
            overlap->argument_local_start,
            argument.StructurePtrOffset,
            compact_base,
            overlap->count,
            uint16_t(i),
            range.range_type,
            heap_type,
            is_sampler,
            is_texture});
      }
      if (range.descriptor_count != UINT_MAX)
        running_offset = range_offset + range.descriptor_count;
    }
  }

  return plan;
}

NativeRootBaseStagePlan
BuildNativeRootBaseStagePlan(const PipelineState &pipeline,
                             const RootSignature &root,
                             PipelineStage want_stage, bool compute) {
  NativeRootBaseStagePlan plan = {};
  plan.root_identity = root.GetCacheIdentity();
  plan.pipeline_identity = pipeline.GetCacheIdentity();
  plan.stage = want_stage;
  plan.compute = compute;

  const auto *shader = FindShaderForStage(pipeline, want_stage);
  if (!shader)
    return plan;
  const auto parameters = root.GetParameters();
  for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
    const auto &parameter = parameters[root_index];
    bool visible = false;
    ForEachVisibleStage(parameter.visibility, compute, [&](PipelineStage s) {
      if (s == want_stage)
        visible = true;
    });
    if (!visible)
      continue;

    if (parameter.parameter_type !=
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
      const bool root_constants =
          parameter.parameter_type ==
          D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      const bool cbuffer =
          root_constants ||
          parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV;
      const auto binding_type =
          cbuffer
              ? SM50BindingType::ConstantBuffer
              : parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV
                    ? SM50BindingType::SRV
                    : parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV
                          ? SM50BindingType::UAV
                          : SM50BindingType::Sampler;
      if (binding_type == SM50BindingType::Sampler)
        continue;
      const UINT shader_register =
          root_constants ? parameter.constants.ShaderRegister
                         : parameter.descriptor.ShaderRegister;
      const UINT register_space =
          root_constants ? parameter.constants.RegisterSpace
                         : parameter.descriptor.RegisterSpace;
      const auto *arguments =
          cbuffer ? shader->constantBufferInfo()
                  : shader->resourceArgumentInfo();
      const auto argument_count =
          cbuffer ? shader->reflection().NumConstantBuffers
                  : shader->reflection().NumArguments;
      for (UINT i = 0; arguments && i < argument_count; ++i) {
        const auto &argument = arguments[i];
        if (argument.Type != binding_type)
          continue;
        const auto space =
            argument.RegisterCount ? argument.RegisterSpace : 0;
        const auto lower = argument.RegisterCount
                               ? argument.RegisterLowerBound
                               : argument.SM50BindingSlot;
        const auto count = ShaderArgumentRangeCount(argument);
        if (space != register_space || shader_register < lower ||
            (count != UINT_MAX && shader_register - lower >= count))
          continue;
        const auto key = argument.StructurePtrOffset;
        if (cbuffer)
          plan.max_cbuffer_key_plus_one =
              std::max<uint32_t>(plan.max_cbuffer_key_plus_one, key + 1);
        else
          plan.max_resource_key_plus_one =
              std::max<uint32_t>(plan.max_resource_key_plus_one, key + 1);
        plan.entries.push_back(NativeRootBaseStagePlanEntry{
            root_index,
            0,
            0,
            1,
            shader_register - lower,
            key,
            1,
            uint16_t(i),
            cbuffer
                ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                : binding_type == SM50BindingType::SRV
                      ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                      : D3D12_DESCRIPTOR_RANGE_TYPE_UAV,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            root_constants
                ? NativeRootBaseStagePlanEntry::Source::RootConstants
                : NativeRootBaseStagePlanEntry::Source::RootDescriptor,
            cbuffer});
        break;
      }
      continue;
    }

    UINT running_offset = 0;
    for (const auto &range : parameter.ranges) {
      const auto range_offset = DescriptorRangeOffset(range, running_offset);
      const auto descriptor_count =
          range.descriptor_count == UINT_MAX
              ? ReflectedDescriptorRangeCount(
                    pipeline, range, parameter.visibility, compute)
              : range.descriptor_count;
      const auto binding_type = BindingTypeForRange(range.range_type);
      const bool cbuffer = binding_type == SM50BindingType::ConstantBuffer;
      const auto *arguments =
          cbuffer ? shader->constantBufferInfo()
                  : shader->resourceArgumentInfo();
      const auto argument_count =
          cbuffer ? shader->reflection().NumConstantBuffers
                  : shader->reflection().NumArguments;
      if (arguments && argument_count) {
        for (UINT i = 0; i < argument_count; i++) {
          const auto &argument = arguments[i];
          if (argument.Type != binding_type)
            continue;
          const auto space =
              argument.RegisterCount ? argument.RegisterSpace : 0;
          const auto lower = argument.RegisterCount
                                 ? argument.RegisterLowerBound
                                 : argument.SM50BindingSlot;
          const auto argument_range_count =
              ShaderArgumentRangeCount(argument);
          const auto overlap = IntersectDescriptorRangeWithShaderArgument(
              lower, argument_range_count, range.base_shader_register,
              descriptor_count);
          if (space != range.register_space || !overlap)
            continue;

          const auto key = argument.StructurePtrOffset;
          if (cbuffer)
            plan.max_cbuffer_key_plus_one =
                std::max<uint32_t>(plan.max_cbuffer_key_plus_one, key + 1);
          else
            plan.max_resource_key_plus_one =
                std::max<uint32_t>(plan.max_resource_key_plus_one, key + 1);

          plan.entries.push_back(NativeRootBaseStagePlanEntry{
              root_index,
              range_offset,
              overlap->descriptor_index,
              descriptor_count,
              overlap->argument_local_start,
              key,
              overlap->count,
              uint16_t(i),
              range.range_type,
              DescriptorHeapTypeForRange(range.range_type),
              NativeRootBaseStagePlanEntry::Source::DescriptorTable,
              cbuffer});
        }
      }

      if (range.descriptor_count != UINT_MAX)
        running_offset = range_offset + range.descriptor_count;
    }
  }
  for (uint32_t i = 0; i < plan.entries.size(); ++i) {
    const auto &entry = plan.entries[i];
    auto group = std::find_if(
        plan.groups.begin(), plan.groups.end(), [&](const auto &candidate) {
          return candidate.cbuffer == entry.cbuffer &&
                 candidate.root_base_key == entry.root_base_key;
        });
    if (group == plan.groups.end()) {
      plan.groups.push_back(NativeRootBaseStagePlanGroup{
          entry.root_base_key, 0, 0, entry.cbuffer, {}});
      group = std::prev(plan.groups.end());
    }
    group->entry_indices.push_back(i);
    group->range_length = std::max<uint32_t>(
        group->range_length,
        entry.argument_local_start + entry.range_count);
    if (entry.descriptor_index < entry.descriptor_count) {
      group->captured_capacity += std::min<uint32_t>(
          entry.range_count,
          entry.descriptor_count - entry.descriptor_index);
    }
  }
  return plan;
}

} // namespace dxmt::d3d12
