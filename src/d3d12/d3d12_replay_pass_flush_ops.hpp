#pragma once

// Queue-independent replay flush operations that emit into a CommandChunk.
//
// These used to be private members of class CommandQueueImpl
// (d3d12_command_queue_pass_batching.inc). Neither reads a CommandQueueImpl
// instance member nor names `this`: their emitcc closures capture only locals
// (the moved-out command storage / barrier entries / timestamp query), and the
// bodies otherwise touch just the promoted ReplayState value types, the
// namespace-level pass-batch / barrier / hazard helpers, and the process-wide
// diagnostic ledgers perDrawSubTimers() and stallProbe(). Their last blocker
// was the queue-nested ScopeAccum guard, now in d3d12_replay_stall_probe.hpp.
//
// The graphics and compute pass flushes stay in the .inc: their emitcc
// closures capture `this` and forward it to
// ReplayPassEncodeCommand::Encode(CommandQueueImpl &, ...), and
// FlushGraphicsPassBatch additionally reads the queue's device_ member.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_queue_state_types.hpp"
#include "dxmt_command_queue.hpp"

namespace dxmt::d3d12 {

// Emits the timestamp sample markers buffered by the replay state, if any, and
// clears them.
void EmitTimestampMarkers(CommandChunk *chunk, ReplayState &state);

// Emits the open blit batch as one Metal blit pass, taking with it only the
// pending barriers that touch the batch's read/write sets, then resets it.
void FlushBlitBatch(CommandChunk *chunk, ReplayState &state);

} // namespace dxmt::d3d12
