#pragma once

// Reset of the API-visible replay recording state.
//
// ResetReplayApiState() and ResetReplayCommandListSemanticState() used to be
// private members of class CommandQueueImpl
// (d3d12_command_queue_replay_state_ops.inc and
// d3d12_command_queue_replay_types.inc). Neither reads a CommandQueueImpl
// instance member or names `this`: both only write fields of the ReplayState
// they are handed. Their last queue-side dependency was
// BumpGraphicsBindingGeneration(), which is now the namespace-level free
// function in d3d12_replay_pass_batch_ops.hpp.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_queue_state_types.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Restores every API-visible binding/raster field of `state` to its
// pipeline-reset value and invalidates the cached graphics binding
// generation.
void ResetReplayApiState(ReplayState &state,
                         ID3D12PipelineState *pipeline_state);

// Command-list state does not carry across D3D12 command-list boundaries, but
// submission-owned batches, descriptor snapshots, resource state and deferred
// barriers do. Resets only the API-visible recording state so compatible work
// from adjacent lists can remain in the same Metal encoder without allowing a
// fallback record to inherit bindings.
void ResetReplayCommandListSemanticState(ReplayState &state);

} // namespace dxmt::d3d12
