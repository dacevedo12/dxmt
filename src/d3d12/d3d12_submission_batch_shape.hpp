#pragma once

// Unpacking of one Execute batch: which submitted plans carry a compiled
// generation, which descriptor snapshots belong to a given list, and the
// per-list counts the drain trace reports.
//
// Extracted from class CommandQueueImpl
// (d3d12_command_queue_submission.inc). All of it is a pure read of the
// immutable per-Execute payload, so it needs neither the queue instance nor the
// queue mutex.

#include <cstddef>
#include <memory>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct CompiledCommandList;
struct CompiledCommandDescriptorSnapshots;
struct SubmittedCompiledCommandListPlan;

/**
 * Index of the last plan in the batch that still carries a compiled
 * generation, or SIZE_MAX when the batch has none. The replay of that list is
 * the one allowed to keep the batch's trailing resource barriers.
 */
[[nodiscard]] size_t LastCompiledSubmittedListIndex(
    const std::vector<std::shared_ptr<const SubmittedCompiledCommandListPlan>>
        &plans);

/**
 * Compiled generation behind a submitted plan, or null when the plan is absent
 * or was closed without one.
 */
[[nodiscard]] const CompiledCommandList *
SubmittedCompiledGeneration(const SubmittedCompiledCommandListPlan *submitted);

/**
 * Descriptor snapshots captured for list `index`, or null when the capture side
 * produced fewer entries than the batch has command lists.
 */
[[nodiscard]] const CompiledCommandDescriptorSnapshots *
SubmittedDescriptorSnapshotsAt(
    const std::vector<CompiledCommandDescriptorSnapshots> &snapshots,
    size_t index);

/** Per-list counts the drain trace reports; all zero for a null generation. */
struct CompiledCommandListShape final {
  UINT record_count = 0;
  size_t segments = 0;
  size_t graphics_packets = 0;
  size_t compute_packets = 0;
};

[[nodiscard]] CompiledCommandListShape
DescribeCompiledCommandList(const CompiledCommandList *compiled);

} // namespace dxmt::d3d12
