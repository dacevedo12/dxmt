#pragma once

// Queue-independent replay pass-batching operations.
//
// These used to be private members of class CommandQueueImpl
// (d3d12_command_queue_pass_batching.inc). None of them reads a
// CommandQueueImpl instance member or names `this`: they only inspect and
// mutate the promoted ReplayState / pass-batch / barrier-batch value types,
// plus the process-wide replay diagnostic ledger (perDrawSubTimers()) and the
// frame statistics singletons. Hoisting them into dxmt::d3d12 lets them be
// compiled and analyzed independently of the ~20k-line queue translation unit.
//
// The two resolvers (vertex/index buffer) were plain non-static members but
// only ever called the free function LookupBufferResourceByGpuVirtualAddress()
// from d3d12_resource.hpp, so they hoist unchanged.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_command_list.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_replay_state_types.hpp"
#include "d3d12_resource_barrier_batch.hpp"

#include <cstdint>
#include <unordered_set>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Which record kind invalidated the cached graphics binding generation. Purely
// a diagnostic attribution tag for BumpGraphicsBindingGeneration().
enum class GraphicsBindingGenerationBumpSource : uint8_t {
  PipelineState,
  DescriptorHeaps,
  VertexBuffers,
  RootSignature,
  RootDescriptorTable,
  RootDescriptor,
  RootConstants,
  IndirectVertexBuffer,
  IndirectRootConstants,
  IndirectRootDescriptor,
};

// Invalidates the cached graphics binding generation (skipping 0, which is the
// "never captured" sentinel) and attributes the bump to `source` in the replay
// diagnostic ledger.
void BumpGraphicsBindingGeneration(ReplayState &state,
                                   GraphicsBindingGenerationBumpSource source);

// Installs the D3D sequence number of the record being replayed for the
// lifetime of the scope, restoring the previous one on exit.
struct ReplayStateRecordScope {
  ReplayState &state;
  uint64_t old_sequence = 0;

  ReplayStateRecordScope(ReplayState &state, uint64_t sequence)
      : state(state), old_sequence(state.current_record_d3d_sequence) {
    state.current_record_d3d_sequence = sequence;
  }

  ~ReplayStateRecordScope() {
    state.current_record_d3d_sequence = old_sequence;
  }
};

// Resolves (and memoizes in the replay state) the buffer backing a vertex
// buffer view. An invalid/unbacked address yields a `valid == false` result.
ResolvedReplayVertexBuffer
ResolveReplayVertexBuffer(ReplayState &state,
                          const D3D12_VERTEX_BUFFER_VIEW &view);

// Index-buffer counterpart of ResolveReplayVertexBuffer().
ResolvedReplayIndexBuffer
ResolveReplayIndexBuffer(ReplayState &state,
                         const D3D12_INDEX_BUFFER_VIEW &view);

// True when the open graphics batch holds at least one command that is not a
// pure barrier, i.e. the pass would do observable GPU work.
[[nodiscard]] bool
ReplayGraphicsPassBatchHasRealWork(const ReplayGraphicsPassBatch &batch);

// Compute counterpart of ReplayGraphicsPassBatchHasRealWork().
[[nodiscard]] bool
ReplayComputePassBatchHasRealWork(const ReplayComputePassBatch &batch);

// True when the barrier batch can be folded into the already-open graphics
// pass instead of forcing a flush.
[[nodiscard]] bool CanDeferResourceBarriersIntoGraphicsBatch(
    const ReplayState &state, const ResourceAccessBarrierBatch &batch);

// Compute counterpart of CanDeferResourceBarriersIntoGraphicsBatch().
[[nodiscard]] bool CanDeferResourceBarriersIntoComputeBatch(
    const ReplayState &state, const ResourceAccessBarrierBatch &batch);

[[nodiscard]] bool HasPendingGraphicsPass(const ReplayState &state);

[[nodiscard]] bool HasPendingComputePass(const ReplayState &state);

[[nodiscard]] bool HasPendingBlitBatch(const ReplayState &state);

// True when anything at all is buffered that a flush would emit.
[[nodiscard]] bool HasPendingReplayWork(const ReplayState &state);

// Summarizes a graphics pass command list into the encode plan / telemetry
// counters consumed when the pass is emitted.
[[nodiscard]] ReplayGraphicsPassPlan BuildReplayGraphicsPassPlan(
    const std::vector<ReplayGraphicsPassCommand> &commands,
    uint64_t argument_buffer_size);

// Accounts an encoder-kind boundary carried over from the previous submission
// as either a merge or a flush, then clears the boundary.
void ResolveSubmissionEncoderBoundary(ReplayState &state,
                                      CompiledEncoderKind incoming_kind,
                                      bool compatible);

// Detaches the whole pending barrier batch from the replay state.
[[nodiscard]] ResourceAccessBarrierBatch
TakePendingResourceBarrierBatch(ReplayState &state);

// Detaches only the pending barriers that touch the given read/write sets,
// leaving the rest pending. A pending separator forces taking everything.
[[nodiscard]] ResourceAccessBarrierBatch TakeMatchingPendingResourceBarriers(
    ReplayState &state, const std::unordered_set<ID3D12Resource *> &reads,
    const std::unordered_set<ID3D12Resource *> &writes);

} // namespace dxmt::d3d12
