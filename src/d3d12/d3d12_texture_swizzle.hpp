#pragma once

#include "winemetal.h"
#include <d3d12.h>

namespace dxmt::d3d12 {

[[nodiscard]] WMTTextureSwizzleChannels
ShaderResourceViewSwizzle(WMTPixelFormat format, UINT component_mapping);

} // namespace dxmt::d3d12
