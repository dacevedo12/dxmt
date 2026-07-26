#include "d3d12_bindless_mirror_plan.hpp"

#include <climits>

namespace dxmt::d3d12 {

bool
IsBindlessTextureMirrorArgument(const MTL_SM50_SHADER_ARGUMENT &arg) {
  return (arg.Type == SM50BindingType::SRV || arg.Type == SM50BindingType::UAV) &&
         (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE);
}

uint32_t
CountBindlessTextureMirrorFieldPairs(const MTL_SM50_SHADER_ARGUMENT *arguments,
                                     uint32_t argument_count) {
  uint32_t count = 0;
  if (!arguments)
    return count;
  for (uint32_t i = 0; i < argument_count; i++)
    if (IsBindlessTextureMirrorArgument(arguments[i]))
      count++;
  return count;
}

const MTL_SM50_SHADER_ARGUMENT *
FindBindlessMirrorArgument(const MTL_SM50_SHADER_ARGUMENT *arguments,
                           uint32_t argument_count, SM50BindingType type,
                           UINT shader_register, UINT register_space) {
  if (!arguments)
    return nullptr;

  for (UINT i = 0; i < argument_count; i++) {
    const auto &argument = arguments[i];
    if (argument.Type != type)
      continue;
    const auto space = argument.RegisterCount ? argument.RegisterSpace : 0;
    const auto lower =
        argument.RegisterCount ? argument.RegisterLowerBound
                               : argument.SM50BindingSlot;
    const auto count = ShaderArgumentRangeCount(argument);
    if (space != register_space || shader_register < lower)
      continue;
    const auto local = shader_register - lower;
    if (count != UINT_MAX && local >= count)
      continue;
    return &argument;
  }
  return nullptr;
}

} // namespace dxmt::d3d12
