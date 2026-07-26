#include "d3d12_shader_stage_query.hpp"

#include "airconv_dx12_metal4.h"

#include <cstdint>

namespace dxmt::d3d12 {


bool
NativeShaderUsesBufferSrv(const PipelineDxilShader &shader) {
  const auto *arguments = shader.resourceArgumentInfo();
  if (!arguments)
    return false;
  for (uint32_t i = 0; i < shader.reflection().NumArguments; i++) {
    if (arguments[i].Type == SM50BindingType::SRV &&
        (arguments[i].Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER))
      return true;
  }
  return false;
}

} // namespace dxmt::d3d12
