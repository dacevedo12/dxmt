#pragma once

// ClearRenderTargetView / ClearDepthStencilView / ClearUnorderedAccessView /
// DiscardResource replay.
//
// These helpers used to be private members of CommandQueueImpl
// (d3d12_command_queue_copy_clear.inc). They only touch namespace-level
// resource / view helpers and the command chunk; the one piece of queue
// identity they used to reach for through `this` — device_->GetMTLDevice(),
// needed to create the Metal views — is now an explicit parameter.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "dxmt_command_queue.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

void ReplayClearRenderTarget(WMT::Device device, CommandChunk *chunk,
                             const ClearRenderTargetRecord &record);

void ReplayClearDepthStencil(WMT::Device device, CommandChunk *chunk,
                             const ClearDepthStencilRecord &record);

void ReplayClearUnorderedAccess(WMT::Device device, CommandChunk *chunk,
                                const ClearUnorderedAccessRecord &record);

void ReplayDiscardResource(CommandChunk *chunk,
                           const DiscardResourceRecord &record);

} // namespace dxmt::d3d12
