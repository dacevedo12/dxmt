#pragma once

#include <d3d12.h>

namespace dxmt::d3d12 {

[[nodiscard]] HRESULT
NormalizeQueueDesc(const D3D12_COMMAND_QUEUE_DESC *desc,
                   D3D12_COMMAND_QUEUE_DESC &normalized);

} // namespace dxmt::d3d12
