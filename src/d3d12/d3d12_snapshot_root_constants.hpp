#pragma once

// Replay of a captured root 32-bit constants entry as a direct constant-buffer
// binding.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_compiled_encode.inc). The only piece of the command
// queue it ever used was the DXMT command queue that owns the argument-buffer
// ring, which is now passed in, so it can be compiled and analysed on its own.

#include "d3d12_replay_binding_types.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"

namespace dxmt::d3d12 {

// Uploads the snapshot entry's root constants into an argument-buffer slice and
// binds it to the entry's constant-buffer slot for the entry's stage. No-op for
// an entry that declares no constants or when the allocation failed.
void BindRootConstantsSnapshot(::dxmt::CommandQueue &queue,
                               ArgumentEncodingContext &enc,
                               const GraphicsBindingSnapshotEntry &entry);

} // namespace dxmt::d3d12
