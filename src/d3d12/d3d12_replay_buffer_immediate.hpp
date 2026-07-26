#pragma once

// WriteBufferImmediate() replay: destination validation, last-writer collapse
// of overlapping destinations and the staged blit that carries the values.
//
// This used to be a private member of CommandQueueImpl
// (d3d12_command_queue_replay_records.inc). The one piece of queue identity it
// reached for through `this` — device_->GetMTLDevice(), needed to allocate the
// staging buffer — is now an explicit parameter.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "dxmt_command_queue.hpp"

namespace dxmt::d3d12 {

void ReplayWriteBufferImmediate(WMT::Device device, CommandChunk *chunk,
                                const WriteBufferImmediateRecord &record);

} // namespace dxmt::d3d12
