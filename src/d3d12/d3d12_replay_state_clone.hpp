#pragma once

// Replay-state cloning for deferred/indirect encode closures.
//
// CloneReplayStateWithoutBatch() used to be a private static member of class
// CommandQueueImpl (d3d12_command_queue_replay_types.inc). Its only queue-side
// dependency was the diagnostic ledger perDrawSubTimers(), which is now the
// namespace-level free function in d3d12_replay_perf_timers.hpp, so the body no
// longer names CommandQueueImpl at all.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_queue_state_types.hpp"

namespace dxmt::d3d12 {

// Field-by-field copy of the API-visible replay state, deliberately leaving out
// the submission-owned pass batches, barrier batches, pending query/marker
// work, resolver caches and descriptor-access caches: an encode closure that
// captures the clone must not inherit or extend the recording queue's batching.
[[nodiscard]] ReplayState
CloneReplayStateWithoutBatch(const ReplayState &state);

} // namespace dxmt::d3d12
