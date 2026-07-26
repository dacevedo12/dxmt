#include "d3d12_queue_config.hpp"

#include "log/log.hpp"
#include "util_string.hpp"

namespace dxmt::d3d12 {
namespace {

bool IsSupportedQueueType(D3D12_COMMAND_LIST_TYPE type) {
  return type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
         type == D3D12_COMMAND_LIST_TYPE_COPY;
}

bool IsSupportedQueuePriority(INT priority) {
  return priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL ||
         priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
}

bool IsSupportedQueueFlags(D3D12_COMMAND_QUEUE_FLAGS flags) {
  return (flags & ~D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT) == 0;
}

} // namespace

HRESULT NormalizeQueueDesc(const D3D12_COMMAND_QUEUE_DESC *desc,
                           D3D12_COMMAND_QUEUE_DESC &normalized) {
  if (!desc)
    return WARN_E_INVALIDARG(__func__);

  normalized = *desc;
  if (!normalized.NodeMask)
    normalized.NodeMask = 1;

  if (!IsSupportedQueueType(normalized.Type)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported queue type ",
                            normalized.Type));
    return WARN_E_INVALIDARG(__func__);
  }

  if (!IsSupportedQueuePriority(normalized.Priority)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported priority ",
                            normalized.Priority));
    return WARN_E_INVALIDARG(__func__);
  }

  if (!IsSupportedQueueFlags(normalized.Flags)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported flags ",
                            normalized.Flags));
    return WARN_E_INVALIDARG(__func__);
  }

  if (normalized.NodeMask > 1) {
    Logger::err(str::format("D3D12CommandQueue: unsupported node mask ",
                            normalized.NodeMask));
    return WARN_E_INVALIDARG(__func__);
  }

  return S_OK;
}

} // namespace dxmt::d3d12
