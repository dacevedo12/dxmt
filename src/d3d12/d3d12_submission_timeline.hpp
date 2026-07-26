#pragma once

// Sequence-number arithmetic of the submission path: the "not bound to a frame
// yet" sentinel a pending operation carries, and the chunk coordinates a fence
// signal or a resolved GPU wait is stamped with.
//
// Extracted from class CommandQueueImpl
// (d3d12_command_queue_submission.inc). These are the conventions the drain
// loop and the enqueue path have to agree on; none of them reads queue state,
// so they compile and analyse on their own.
//
// The callers deliberately keep the sentinel test outside these functions:
// CommandQueue::CurrentFrameSeq() reads a plain non-atomic counter owned by the
// device queue, so it must stay behind the `== kUnresolvedSubmissionFrameId`
// guard it was already behind rather than becoming an unconditional read.

#include <cstdint>

namespace dxmt::d3d12 {

/**
 * Frame tag of a pending operation that ExecuteCommandLists enqueued without
 * knowing which frame it would land in.
 */
inline constexpr uint64_t kUnresolvedSubmissionFrameId = ~0ull;

/**
 * Frame such an operation belongs to: CurrentFrameSeq() names the frame being
 * opened, and work enqueued now still belongs to the one in flight.
 */
[[nodiscard]] uint64_t PreviousFrameSeq(uint64_t current_frame_seq) noexcept;

/** Ring slot a committed chunk occupies, i.e. which chunk it will recycle. */
[[nodiscard]] uint64_t CommandChunkSlot(uint64_t chunk_id) noexcept;

/**
 * Event sequence id the next signal/wait encoded into the current chunk will
 * consume. GetCurrentEventSeqId() reports the last id handed out, so the
 * encoder predicts its own by adding one.
 */
[[nodiscard]] uint64_t
NextChunkEventSeqId(uint64_t current_event_seq_id) noexcept;

} // namespace dxmt::d3d12
