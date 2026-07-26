#pragma once

// Encode of one replayed compute dispatch: pipeline state, bindings (compiled
// or live) and the dispatch command itself.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_compiled_encode.inc). Everything it needs now travels in
// the packet or in the SubmissionBindingContext.

#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_submission_binding_context.hpp"
#include "dxmt_context.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

/** Encodes `packet` into the compute encoder currently open on `enc`. Compiled
 *  packets reuse the encoder's binding-program cache and only re-bind on a
 *  miss; non-compiled packets rebuild bindings from the packet's replay state
 *  and are skipped outright when that state is missing. */
void EncodeReplayDispatchPacket(const SubmissionBindingContext &ctx,
                                ArgumentEncodingContext &enc,
                                ReplayDispatchPacket &packet,
                                uint64_t &argbuf_offset);

} // namespace dxmt::d3d12
