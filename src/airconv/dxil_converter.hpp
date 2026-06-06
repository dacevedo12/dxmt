#pragma once

#include "DXILParser/DXILParser.hpp"
#include "airconv_dx12_metal4.h"

namespace dxmt::airconv {

const dxil::DxilTranslationInfo *
GetDxmt12DxilTranslationInfo(dxmt12_airconv_shader_t pShader);

} // namespace dxmt::airconv
