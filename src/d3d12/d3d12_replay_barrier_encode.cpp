#include "d3d12_replay_barrier_encode.hpp"

#include "dxmt_perf_stats.hpp"

#include <algorithm>
#include <utility>

namespace dxmt::d3d12 {

void
EncodeResourceAccessBarrierEntries(
    ArgumentEncodingContext &enc,
    std::vector<ResourceAccessBarrierEntry> &entries) {
  EncodeResourceAccessBarrierEntriesForStage<PipelineStage::Compute>(
      enc, entries);
}

void
EncodeRenderResourceAccessBarrierEntries(
    ArgumentEncodingContext &enc,
    std::vector<ResourceAccessBarrierEntry> &entries) {
  EncodeResourceAccessBarrierEntriesForStage<PipelineStage::Vertex>(
      enc, entries);
}

bool
RenderResourceAccessBarrierEntriesRequireReverseBoundary(
    ArgumentEncodingContext &enc,
    const std::vector<ResourceAccessBarrierEntry> &entries) {
  for (const auto &entry : entries) {
    if (entry.buffer &&
        enc.requiresReverseRenderPassBoundary(entry.buffer, entry.access))
      return true;
    if (entry.texture &&
        enc.requiresReverseRenderPassBoundary(
            entry.texture, entry.level, entry.slice, entry.access))
      return true;
  }
  return false;
}

void
EncodeInlineRenderResourceBarrierEntries(
    ArgumentEncodingContext &enc,
    std::vector<ResourceAccessBarrierEntry> &entries) {
  // Probe before mutating GenericAccessTracker. If a Fragment producer would
  // feed a PreRaster barrier in this pass, closing first preserves the real
  // producer id; publishing after reopen turns it into a stage-correct local
  // fence edge. Mutate-then-split would overwrite that id with the old
  // vertex stage and silently make the transition visible too early.
  if (RenderResourceAccessBarrierEntriesRequireReverseBoundary(enc, entries))
    enc.splitRenderPassForReverseStage();
  EncodeRenderResourceAccessBarrierEntries(enc, entries);
  enc.resolveRenderPassBarrier();
}

void
RecordSeparatorOnlyBarrier(const ResourceAccessBarrierBatch &batch) {
  if (batch.entries.empty() && batch.needs_separator) {
    if (auto *stats = dxmt::perf::currentFrameStatistics())
      stats->blit_separator_pass_count++;
  }
}

void
MergeResourceAccessBarrierBatch(ResourceAccessBarrierBatch &dst,
                                ResourceAccessBarrierBatch src) {
  if (dst.entry_index.empty() && src.entry_index.empty() &&
      dst.entries.size() + src.entries.size() <=
          kResourceAccessBarrierLinearEntryLimit) {
    for (auto &entry : src.entries) {
      auto existing =
          std::find_if(dst.entries.begin(), dst.entries.end(),
                       [&entry](const ResourceAccessBarrierEntry &candidate) {
                         return ResourceAccessBarrierEntriesMatch(candidate,
                                                                  entry);
                       });
      if (existing != dst.entries.end()) {
        MergeResourceAccessBarrierEntry(*existing, std::move(entry));
        continue;
      }
      dst.entries.push_back(std::move(entry));
    }
    dst.needs_separator = dst.needs_separator || src.needs_separator;
    return;
  }

  if (dst.entry_index.empty())
    RebuildResourceAccessBarrierIndex(dst);

  for (auto &entry : src.entries) {
    auto key = MakeResourceAccessBarrierKey(entry);

    auto found = dst.entry_index.find(key);
    if (found != dst.entry_index.end()) {
      MergeResourceAccessBarrierEntry(dst.entries[found->second],
                                      std::move(entry));
      continue;
    }

    dst.entry_index.emplace(key, dst.entries.size());
    if (auto *resource = entry.d3d_resource.ptr())
      dst.resource_entry_keys[resource].push_back(key);
    dst.entries.push_back(std::move(entry));
  }
  dst.needs_separator = dst.needs_separator || src.needs_separator;
}

} // namespace dxmt::d3d12
