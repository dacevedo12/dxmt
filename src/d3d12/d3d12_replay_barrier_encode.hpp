#pragma once

// Encoding of pending resource-access barrier batches into an
// ArgumentEncodingContext, plus the batch merge used to accumulate them.
//
// These helpers used to be private static members of the anonymous-namespace
// class CommandQueueImpl (d3d12_command_queue_pass_batching.inc /
// d3d12_command_queue_replay_records.inc). They only touch the barrier batch
// value types and the encoder, never the queue instance, so hoisting them to
// dxmt::d3d12 lets them be compiled and analyzed independently.

#include "d3d12_resource_barrier_batch.hpp"
#include "dxmt_context.hpp"

#include <vector>

namespace dxmt::d3d12 {

// Replays barrier entries as encoder accesses for one pipeline stage. A
// cross-submit wait is requested before the access it belongs to.
template <PipelineStage stage>
void EncodeResourceAccessBarrierEntriesForStage(
    ArgumentEncodingContext &enc,
    std::vector<ResourceAccessBarrierEntry> &entries) {
  for (auto &entry : entries) {
    if (entry.requires_cross_submit_wait)
      enc.requireCrossSubmitWait();
    if (entry.buffer) {
      enc.access<stage>(entry.buffer, 0, entry.buffer_length, entry.access);
    } else if (entry.texture) {
      enc.access<stage>(entry.texture, entry.level, entry.slice, entry.access);
    }
  }
}

// Compute/blit-pass flavor of the barrier replay.
void EncodeResourceAccessBarrierEntries(
    ArgumentEncodingContext &enc,
    std::vector<ResourceAccessBarrierEntry> &entries);

// Render-pass flavor of the barrier replay.
void EncodeRenderResourceAccessBarrierEntries(
    ArgumentEncodingContext &enc,
    std::vector<ResourceAccessBarrierEntry> &entries);

// True when any entry would need the render pass split so a Fragment producer
// can feed a PreRaster consumer.
[[nodiscard]] bool RenderResourceAccessBarrierEntriesRequireReverseBoundary(
    ArgumentEncodingContext &enc,
    const std::vector<ResourceAccessBarrierEntry> &entries);

// Replays barrier entries inside an already open render pass, splitting it
// first when a reverse-stage boundary is required.
void EncodeInlineRenderResourceBarrierEntries(
    ArgumentEncodingContext &enc,
    std::vector<ResourceAccessBarrierEntry> &entries);

// Counts a batch that carries only a separator (no entries) as a blit
// separator pass in the frame statistics.
void RecordSeparatorOnlyBarrier(const ResourceAccessBarrierBatch &batch);

// Folds `src` into `dst`, merging entries that address the same subresource.
void MergeResourceAccessBarrierBatch(ResourceAccessBarrierBatch &dst,
                                     ResourceAccessBarrierBatch src);

} // namespace dxmt::d3d12
