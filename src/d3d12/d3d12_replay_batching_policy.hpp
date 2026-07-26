#pragma once

// Replay batching toggles and the record classification that defines an
// intra-pass parallel run.
//
// These used to be private members of class CommandQueueImpl
// (d3d12_command_queue_pass_queue.inc). None of them reads a CommandQueueImpl
// instance member or names `this`: the toggles only consult environment /
// diagnostic switches through free functions, and the predicates only inspect
// the CommandRecord payload variant. The two `D3D12Replay*BatchingEnabled()`
// entry points were plain non-static members for historical reasons only.
//
// The function-local `static` env caches are process-level, exactly as before:
// the member functions were implicitly inline, so each already had a single
// program-wide definition; the definitions now live in
// d3d12_replay_batching_policy.cpp.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_command_list.hpp"

namespace dxmt::d3d12 {

[[nodiscard]] bool D3D12ReplayComputeBatchingEnabled();

[[nodiscard]] bool D3D12ReplayGraphicsBatchingEnabled();

[[nodiscard]] bool IntraPassParallelEnabled();

// Intra-pass parallel (DXMT_INTRAPASS_PARALLEL): a record that stays inside an
// active graphics render pass — a draw or a graphics state-setter — and does
// NOT force a FlushPassBatches. A maximal run of these between two flush points
// (barrier / clear / copy / dispatch / resolve / render-target change / query)
// is the unit replayable by parallel workers: within it the resource state is
// frozen (barrier-free) so per-draw hazard records are read-mostly. RootSig /
// descriptor-table / root-constant setters stay in the run (they only mutate
// per-list binding state, which each worker clones); RenderTargets does NOT
// (it flushes the batch and starts a new pass).
[[nodiscard]] bool IsGraphicsPassRunRecord(const CommandRecord &record);

[[nodiscard]] bool IsGraphicsStateSetterRecord(const CommandRecord &record);

[[nodiscard]] bool IsDeferrableCompiledStateRecord(const CommandRecord &record);

[[nodiscard]] bool FallbackRecordNeedsBindingState(const CommandRecord &record);

} // namespace dxmt::d3d12
