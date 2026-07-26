#pragma once

// When a queued fence wait counts as satisfied, and the worker bookkeeping that
// records which wait the submission worker is parked on.
//
// Extracted from class CommandQueueImpl
// (d3d12_command_queue_submission.inc). The drain loop and the destruction path
// applied the same "already completed?" test through two differently spelled
// expressions; they share one predicate here so the two cannot drift apart.
//
// The dependency accessors exist for their DXMT_REQUIRES contract: the flag
// they clear or publish is DXMT_GUARDED_BY the queue mutex, and the triple must
// be written together or the worker parks on a wait it no longer tracks. The
// mutex parameter carries no data -- it is the capability the analyser checks.

#include "thread.hpp"

#include <atomic>
#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

class Fence;

/**
 * True when the wait needs no further blocking: either its CPU completion
 * callback already fired, or the fence has reached the awaited value. The
 * caller passes the callback observation separately because it reads a
 * completion object whose lifetime it owns.
 */
[[nodiscard]] bool QueuedFenceWaitIsSatisfied(const Fence *fence, UINT64 value,
                                              bool completion_observed);

/**
 * True when the wait cannot be retired during queue destruction: it is
 * unsatisfied and does not resolve against a producer queue's signal, so no
 * operation ordered behind it can make progress. Everything from such a wait
 * onwards is cancelled instead of drained.
 */
[[nodiscard]] bool QueuedFenceWaitBlocksShutdown(const Fence *fence,
                                                 UINT64 value,
                                                 bool completion_observed);

/** Forgets the wait the worker was parked on, unblocking the drain loop. */
void ClearSubmissionWaitDependency(
    dxmt::mutex &queue_mutex, bool &waiting_for_wait,
    std::atomic<uint64_t> &dependency_pair,
    std::atomic<uint64_t> &dependency_value) noexcept
    DXMT_REQUIRES(queue_mutex);

/**
 * Publishes the wait the worker is parked on. The pair id and value are read
 * without the mutex by the hang diagnostics, hence the relaxed atomics.
 */
void SetSubmissionWaitDependency(dxmt::mutex &queue_mutex,
                                 bool &waiting_for_wait,
                                 std::atomic<uint64_t> &dependency_pair,
                                 std::atomic<uint64_t> &dependency_value,
                                 uint64_t lifecycle_pair_id,
                                 UINT64 value) noexcept
    DXMT_REQUIRES(queue_mutex);

} // namespace dxmt::d3d12
