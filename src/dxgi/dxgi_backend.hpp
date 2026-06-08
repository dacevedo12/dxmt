#pragma once

#include "dxgi_interfaces.h"

#include <vector>

namespace dxmt {

std::vector<DxgiBackendProvider> CopyRegisteredBackends();

} // namespace dxmt
