#include "d3d12_live_msaa_demote_query.hpp"

#include "d3d12_descriptor_record_query.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_shader_binding.hpp"
#include "d3d12_stage_plan_build.hpp"

#include "airconv_dx12_metal4.h"

namespace dxmt::d3d12 {

namespace {

// Binding slots covered by the demote mask pair (mask_lo plus mask_hi).
constexpr UINT kPixelShaderMsaaSrvDemoteSlots =
    2 * kPixelShaderMsaaSrvDemoteMaskBits;

} // namespace

std::pair<uint64_t, uint64_t>
PixelShaderSingleSampleMsaaSRVDemoteMask(const ReplayState &state,
                                         const PipelineState &pipeline) {
  const auto *shader = FindShaderForStage(pipeline, PipelineStage::Pixel);
  if (!shader || !shader->resourceArgumentInfo() ||
      !state.graphics_root_signature_impl)
    return {0, 0};

  uint64_t mask_lo = 0;
  uint64_t mask_hi = 0;
  const auto *arguments = shader->resourceArgumentInfo();
  const auto argument_count = shader->reflection().NumArguments;
  const auto parameters = state.graphics_root_signature_impl->GetParameters();

  for (UINT arg_index = 0; arg_index < argument_count; arg_index++) {
    const auto &argument = arguments[arg_index];
    if (argument.Type != SM50BindingType::SRV ||
        !(argument.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED) ||
        argument.SM50BindingSlot >= kPixelShaderMsaaSrvDemoteSlots)
      continue;

    const auto space = argument.RegisterCount ? argument.RegisterSpace : 0;
    const auto lower = argument.RegisterCount ? argument.RegisterLowerBound
                                              : argument.SM50BindingSlot;
    const auto range_count = ShaderArgumentRangeCount(argument);
    bool resolved = false;

    for (UINT root_index = 0; !resolved && root_index < parameters.size();
         root_index++) {
      const auto &parameter = parameters[root_index];
      if (parameter.parameter_type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        continue;

      bool pixel_visible = false;
      ForEachVisibleStage(parameter.visibility, false,
                          [&](PipelineStage stage) {
                            if (stage == PipelineStage::Pixel)
                              pixel_visible = true;
                          });
      if (!pixel_visible)
        continue;

      UINT running_offset = 0;
      for (const auto &range : parameter.ranges) {
        const auto range_offset = DescriptorRangeOffset(range, running_offset);
        const auto descriptor_count =
            range.descriptor_count == UINT_MAX
                ? ReflectedDescriptorRangeCount(pipeline, range,
                                                parameter.visibility, false)
                : range.descriptor_count;
        if (range.range_type != D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
          if (range.descriptor_count != UINT_MAX)
            running_offset = range_offset + range.descriptor_count;
          continue;
        }
        const auto overlap = IntersectDescriptorRangeWithShaderArgument(
            lower, range_count, range.base_shader_register, descriptor_count);
        if (space != range.register_space || !overlap) {
          if (range.descriptor_count != UINT_MAX)
            running_offset = range_offset + range.descriptor_count;
          continue;
        }

        const auto base = GetTableHandle(state, false, root_index);
        if (!base.ptr) {
          resolved = true;
          break;
        }

        for (UINT local = 0;
             local < overlap->count &&
             overlap->descriptor_index + local < descriptor_count;
             local++) {
          const auto arg_local = overlap->argument_local_start + local;
          const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
              GetBoundDescriptorHeap(state,
                                     D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV),
              base, range_offset, overlap->descriptor_index + local,
              descriptor_count, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
          if (!descriptor)
            continue;
          if (!ShaderResourceViewIsMultisampledTexture(*descriptor))
            SetPixelShaderMsaaSrvDemoteBit(
                mask_lo, mask_hi, argument.SM50BindingSlot + arg_local);
        }
        resolved = true;
        break;
      }
    }
  }

  return {mask_lo, mask_hi};
}

} // namespace dxmt::d3d12
