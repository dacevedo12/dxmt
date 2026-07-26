#pragma once

// Admission rules for the replay pass batcher: whether an incoming pass can
// keep riding the encoder boundary carried over from the previous submission,
// whether the open graphics batch has to be flushed first, and the arithmetic
// that reserves a slot for the new command inside that batch.
//
// These were the open-coded prologues of QueueGraphicsPassCommand() /
// QueueCompiledGraphicsPassCommand() / QueueComputePassCommand() in
// d3d12_command_queue_pass_queue.inc. They only read and mutate the promoted
// ReplayState / pass-batch value types, so hoisting them to dxmt::d3d12 lets
// the merge decision be analyzed without the queue translation unit. The
// queueing functions themselves stay behind: they own the flush calls and the
// encoder allocation, both of which need CommandQueueImpl.

#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

/** True when a graphics pass with `attachments` continues the encoder that the
 *  previous submission left open, i.e. the boundary is a merge and not a
 *  flush. Requires the open batch to be graphics-only and attachment-
 *  compatible. */
[[nodiscard]] bool ReplayGraphicsPassContinuesSubmissionBoundary(
    const ReplayState &state, const ReplayRenderPassAttachments &attachments);

/** Compute counterpart of ReplayGraphicsPassContinuesSubmissionBoundary().
 *  Compute passes carry no attachments, so only encoder exclusivity matters. */
[[nodiscard]] bool
ReplayComputePassContinuesSubmissionBoundary(const ReplayState &state);

/** True when the open graphics batch describes a different render pass than
 *  `attachments` and therefore has to be flushed before the new command can
 *  join it. An inactive batch never rejects. */
[[nodiscard]] bool ReplayGraphicsBatchRejectsAttachments(
    const ReplayGraphicsPassBatch &batch,
    const ReplayRenderPassAttachments &attachments);

/** Batch-relative placement handed back by ReserveReplayGraphicsBatchSlot(). */
struct ReplayGraphicsBatchSlot {
  uint64_t argument_buffer_offset = 0;
  uint32_t command_index = 0;
  bool parallel_candidate = false;
};

/** Claims the next command slot in an already-activated graphics batch: takes
 *  the current argument-buffer cursor and command index, then advances the
 *  cursor and folds the command's geometry/tessellation usage into the batch.
 *  Must be called before the command is pushed. */
[[nodiscard]] ReplayGraphicsBatchSlot ReserveReplayGraphicsBatchSlot(
    ReplayGraphicsPassBatch &batch, uint64_t argument_buffer_size,
    ReplayGraphicsCommandKind kind, bool use_geometry, bool use_tessellation);

/** Copies a reserved slot plus the command's own classification into the
 *  queued command record. The encoder and the D3D sequence number are the
 *  caller's business. */
void FillReplayGraphicsPassCommandSlot(ReplayGraphicsPassCommand &command,
                                       const ReplayGraphicsBatchSlot &slot,
                                       uint64_t argument_buffer_size,
                                       ReplayGraphicsCommandKind kind,
                                       bool use_geometry,
                                       bool use_tessellation,
                                       bool bindless_compiled_candidate);

} // namespace dxmt::d3d12
