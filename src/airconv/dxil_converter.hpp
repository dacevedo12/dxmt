#pragma once

#include "DXILParser/DXILParser.hpp"
#include "airconv_public.h"

namespace dxmt::airconv {

const dxil::DxilTranslationInfo *GetDxilTranslationInfo(dxil_shader_t pShader);

} // namespace dxmt::airconv
