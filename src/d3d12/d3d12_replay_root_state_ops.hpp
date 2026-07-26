#pragma once

// Queue-independent root-binding and predication state operations.
//
// These used to be private members of class CommandQueueImpl
// (d3d12_command_queue_replay_state_ops.inc). None of them reads a
// CommandQueueImpl instance member or names `this`: they only mutate the
// promoted ReplayState value type and call free helpers
// (ApplyRootConstants(), GetPipelineState()). Hoisting them into dxmt::d3d12
// lets them be compiled and analyzed independently of the ~20k-line queue
// translation unit.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_queue_state_types.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

struct RootConstantsRecord;
struct RootDescriptorRecord;
struct PredicationRecord;

// Installs a root CBV/SRV/UAV descriptor into the graphics or compute root
// slot array. Out-of-range root parameter indices are ignored.
void StoreRootDescriptor(ReplayState &state,
                         const RootDescriptorRecord &record);

// Installs (part of) a root constant slot. Out-of-range root parameter
// indices are ignored.
void StoreRootConstants(ReplayState &state,
                        const RootConstantsRecord &record);

// True when the pipeline state currently bound in the replay state is a
// compute pipeline.
[[nodiscard]] bool CurrentPipelineIsCompute(const ReplayState &state);

// Installs the predication buffer/offset/operation of a SetPredication record.
void ReplaySetPredication(ReplayState &state, const PredicationRecord &record);

} // namespace dxmt::d3d12
