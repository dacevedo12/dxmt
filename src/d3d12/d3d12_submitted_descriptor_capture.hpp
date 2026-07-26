#pragma once

// Descriptor snapshot capture for one Execute submission.
//
// ExecuteCommandLists is the last synchronous boundary at which a
// descriptor-table generation belongs unambiguously to a submission, so the
// compiled packet list is walked here and frozen into snapshots that Metal
// encode consumes much later. This used to be four members of CommandQueueImpl
// (d3d12_command_queue_pass_queue.inc). The only queue-owned state the walk
// ever reached for was the Metal device and the two stage-plan memos, which
// travel together in a SubmissionBindingContext now; the recipe memo, the heap
// mirrors, the descriptor journal and the telemetry are all namespace-level
// already, so the whole capture is analysable without the command queue.

#include "d3d12_command_list.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_submission_binding_context.hpp"

#include <memory>

namespace dxmt::d3d12 {

/** Captures one binding snapshot per compiled graphics/compute packet that
 *  actually reaches the bindless-mirror or native descriptor-table path;
 *  packets that already fell back are left null. Packets with identical
 *  bindings share a snapshot through a submission-local cache, and every
 *  participating heap mirror stays locked for the whole walk — that lock window
 *  is what makes the captured contents belong to this submission.
 *
 *  `descriptor_records` and `native_span_store` may be shared across the
 *  submissions of one Execute; empty handles get fresh stores. */
[[nodiscard]] CompiledCommandDescriptorSnapshots
CaptureSubmittedDescriptorSnapshots(
    const SubmissionBindingContext &binding,
    const CompiledCommandList &compiled,
    std::shared_ptr<SubmittedDescriptorRecordStore> descriptor_records = {},
    std::shared_ptr<SubmittedNativeDescriptorSpanStore> native_span_store = {});

} // namespace dxmt::d3d12
