#pragma once

// Read/write hazard tracking for the replay blit batch.
//
// These helpers used to be private members of the anonymous-namespace class
// CommandQueueImpl (d3d12_command_queue_pass_batching.inc). They only read and
// mutate the promoted ReplayBlitBatch access sets, so hoisting them to
// dxmt::d3d12 lets them be compiled and analyzed independently.

#include "d3d12_replay_pass_types.hpp"

#include <initializer_list>

#include <d3d12.h>

namespace dxmt::d3d12 {

// True when queueing a command that reads `reads` and writes `writes` would
// conflict with the accesses already recorded in the open blit batch. Null
// resources are ignored.
[[nodiscard]] bool
ReplayBlitBatchHasHazard(const ReplayBlitBatch &batch,
                         std::initializer_list<ID3D12Resource *> reads,
                         std::initializer_list<ID3D12Resource *> writes);

// Records the accesses of a newly queued blit command. Null resources are
// ignored.
void ReplayBlitBatchTrackAccess(ReplayBlitBatch &batch,
                                std::initializer_list<ID3D12Resource *> reads,
                                std::initializer_list<ID3D12Resource *> writes);

} // namespace dxmt::d3d12
