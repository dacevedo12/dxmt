#pragma once

#include "d3d12_submission_service.hpp"
#include <d3d12.h>
#include <cstdint>
#include <memory>

namespace dxmt::d3d12 {

class CommandQueue;

[[nodiscard]] bool SubmitDxmtQueueWork(
    IMTLD3D12Device *device,
    std::unique_ptr<DxmtQueueSubmissionTarget> target,
    uint64_t &submitted_sequence) noexcept;

HRESULT
CreateCommandQueue(IMTLD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc,
                   REFIID riid, void **command_queue);

} // namespace dxmt::d3d12
