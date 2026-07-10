#pragma once

#include "dxmt_statistics.hpp"
#include <algorithm>
#include <cstdint>

namespace dxmt::perf {

bool enabled();

enum class PsoArtifactKind : uint32_t {
  Graphics,
  Compute,
  GraphicsVariant,
};

enum class PsoArtifactCacheResult : uint32_t {
  Hit,
  Miss,
  Wait,
};

enum class FrameTimeBucket : uint32_t {
  ExecuteCommandLists,
  Present,
  QueueSignal,
  QueueWait,
  CreateResource,
  CreateReservedResource,
  CreateHeap,
  CreatePipeline,
  TileMapping,
  // Time spent in AddRecord recording CommandRecords on the calling thread. If
  // this accumulates on the present thread, it is part of the measured frame
  // wall and must be optimized as DXMT recording work. If it stays zero,
  // recording runs outside the measured frame boundary.
  CommandListRecord,
};

enum class ExecuteTimeBucket : uint32_t {
  Validate,
  Collect,
  Enqueue,
  Drain,
  DrainLock,
  Replay,
  Decay,
  Signal,
  Commit,
  WaitArm,
};

class ScopedFrameTimer {
public:
  explicit ScopedFrameTimer(FrameTimeBucket bucket);
  ~ScopedFrameTimer();
  void stop();

  ScopedFrameTimer(const ScopedFrameTimer &) = delete;
  ScopedFrameTimer &operator=(const ScopedFrameTimer &) = delete;

private:
  FrameTimeBucket bucket_;
  FrameStatistics *stats_;
  dxmt::clock::time_point begin_;
  bool active_;
};

class ScopedCodeTimer {
public:
  explicit ScopedCodeTimer(PerfCodePath path);
  ~ScopedCodeTimer();
  void stop();

  ScopedCodeTimer(const ScopedCodeTimer &) = delete;
  ScopedCodeTimer &operator=(const ScopedCodeTimer &) = delete;

private:
  PerfCodePath path_;
  FrameStatistics *stats_;
  ScopedCodeTimer *parent_;
  dxmt::clock::time_point begin_;
  dxmt::clock::duration child_interval_{};
  bool active_;
};

// Temporarily binds a statistics sink to the calling thread. Submission
// workers use this to collect replay timings into worker-owned storage instead
// of writing through the Present thread's frame-ring pointer.
class ScopedFrameStatisticsBinding {
public:
  explicit ScopedFrameStatisticsBinding(FrameStatistics *stats);
  ~ScopedFrameStatisticsBinding();

  ScopedFrameStatisticsBinding(const ScopedFrameStatisticsBinding &) = delete;
  ScopedFrameStatisticsBinding &
  operator=(const ScopedFrameStatisticsBinding &) = delete;

private:
  FrameStatistics *previous_;
};

struct ReplayWorkerSummary {
  uint64_t execute_replay_us = 0;
  uint64_t replay_timed_us = 0;
  uint64_t record_loop_us = 0;
  uint64_t superseded_mask_us = 0;
  uint64_t compiled_graphics_us = 0;
  uint64_t compiled_compute_us = 0;
  uint64_t fallback_classification_us = 0;
  uint64_t typed_record_us = 0;
  uint64_t native_command_body_us = 0;
  uint64_t record_control_us = 0;
  uint64_t classified_record_us = 0;
  uint64_t replay_coverage_permille = 0;
  uint64_t record_coverage_permille = 0;
};

void setCurrentFrameStatistics(FrameStatistics *stats);
FrameStatistics *currentFrameStatistics();

using FrameDurationMember = dxmt::clock::duration FrameStatistics::*;
using FrameCounterMember = uint64_t FrameStatistics::*;

inline dxmt::clock::duration exclusiveCodeDuration(
    dxmt::clock::duration elapsed, dxmt::clock::duration child_interval) {
  return elapsed > child_interval ? elapsed - child_interval
                                  : dxmt::clock::duration{};
}

inline void addExclusiveCodeTiming(FrameStatistics *stats, PerfCodePath path,
                                   dxmt::clock::duration elapsed,
                                   dxmt::clock::duration child_interval) {
  if (!stats)
    return;
  const auto index = static_cast<size_t>(path);
  if (index >= stats->frame_code_path_intervals.size())
    return;
  const auto exclusive = exclusiveCodeDuration(elapsed, child_interval);
  stats->frame_code_path_intervals[index] += exclusive;
  stats->frame_code_path_max_intervals[index] = std::max(
      stats->frame_code_path_max_intervals[index], exclusive);
  stats->frame_code_path_counts[index]++;
}

class ScopedFrameDuration {
public:
  ScopedFrameDuration(FrameStatistics *stats, FrameDurationMember target)
      : stats_(stats && target ? stats : nullptr),
        target_(target),
        begin_(stats_ ? dxmt::clock::now() : dxmt::clock::time_point{}) {
  }

  ~ScopedFrameDuration() {
    stop();
  }

  void stop() {
    if (!stats_)
      return;
    (stats_->*target_) += dxmt::clock::now() - begin_;
    stats_ = nullptr;
  }

  ScopedFrameDuration(const ScopedFrameDuration &) = delete;
  ScopedFrameDuration &operator=(const ScopedFrameDuration &) = delete;

private:
  FrameStatistics *stats_;
  FrameDurationMember target_;
  dxmt::clock::time_point begin_;
};

template <typename Context>
inline FrameStatistics *frameStatisticsForContext(Context &context) {
  return enabled() ? &context.currentFrameStatistics() : nullptr;
}

inline void addFrameCounter(FrameStatistics *stats, FrameCounterMember target,
                            uint64_t count = 1) {
  if (stats && target)
    (stats->*target) += count;
}

// Times a scope into a duration field and counts the entry. An interval on its
// own cannot tell one expensive call from a churn of cheap ones, and that is
// the distinction that decides whether to optimize the operation or the call
// rate, so an entry point measuring both should not have to spell out the pair.
// Inert when the sink is null, like the two primitives it composes.
class ScopedFrameDurationCounted {
public:
  ScopedFrameDurationCounted(FrameStatistics *stats, FrameDurationMember target,
                             FrameCounterMember counter)
      : duration_(stats, target) {
    addFrameCounter(stats, counter);
  }

  ScopedFrameDurationCounted(const ScopedFrameDurationCounted &) = delete;
  ScopedFrameDurationCounted &
  operator=(const ScopedFrameDurationCounted &) = delete;

private:
  ScopedFrameDuration duration_;
};

void recordExecuteTime(FrameStatistics *stats, ExecuteTimeBucket bucket,
                       dxmt::clock::duration duration);
void recordReplayBreakdown(FrameStatistics *stats,
                           dxmt::clock::duration record_loop,
                           dxmt::clock::duration flush_pass,
                           dxmt::clock::duration timestamp_resolve,
                           dxmt::clock::duration cpu_query_resolve);
void recordCompiledPassBuildTime(FrameStatistics *stats,
                                 dxmt::clock::duration duration);
void recordCompiledDrawEncodeTime(FrameStatistics *stats,
                                  dxmt::clock::duration duration);
void recordCompiledDispatchEncodeTime(FrameStatistics *stats,
                                      dxmt::clock::duration duration);
void recordArgumentTableUpdateTime(FrameStatistics *stats,
                                   dxmt::clock::duration duration);
void recordArgumentTableBindTime(FrameStatistics *stats,
                                 dxmt::clock::duration duration);
void recordResidencySubmitTime(FrameStatistics *stats,
                               dxmt::clock::duration duration);
void recordPsoCacheLookupTime(FrameStatistics *stats,
                              dxmt::clock::duration duration);
void recordPsoMaterializeTime(FrameStatistics *stats,
                              dxmt::clock::duration duration);
void recordPsoCompileWaitTime(FrameStatistics *stats,
                              dxmt::clock::duration duration);
void recordCompiledGraphicsPackets(FrameStatistics *stats, uint64_t count);
void recordFallbackGraphicsPackets(FrameStatistics *stats, uint64_t count,
                                   CompiledFallbackReason reason);
void recordCompiledComputePackets(FrameStatistics *stats, uint64_t count);
void recordFallbackComputePackets(FrameStatistics *stats, uint64_t count,
                                  CompiledFallbackReason reason);
void recordStateRecordsElided(FrameStatistics *stats, uint64_t count);
void recordFrameBoundary(uint64_t frame);
void recordFrameBoundary(uint64_t frame, const FrameStatistics &frame_stats,
                         const FrameStatistics &average_stats,
                         uint64_t frame_wall_us);
ReplayWorkerSummary
summarizeReplayWorkerFrame(const FrameStatistics &stats);
void recordReplayWorkerFrame(uint64_t frame, uintptr_t queue,
                             uint32_t queue_type, uint64_t execute_batches,
                             const FrameStatistics &stats);
void flushFinal(uint64_t frame);
void recordWaitCpuFence(uint64_t wait_us);

void recordTimestampGpuResolve(uint64_t queries);
void recordTimestampCpuFallback(uint64_t queries);
void recordTimestampCpuDeferred(uint64_t queries);
void recordTimestampCpuImmediate(bool unsafe);
void recordTimestampCpuMaterialized();
void recordTimestampCpuWait(uint64_t wait_us);
void recordQueryBatchWait(uint64_t batches, uint64_t queries,
                          uint64_t wait_us);

void recordTileMapping(uint64_t standard_ops, uint64_t packed_ops,
                       uint64_t map_ops, uint64_t unmap_ops,
                       uint64_t invalid, uint64_t metal_failures,
                       uint64_t barriers);

void recordGraphicsPipelineCreate(uint64_t duration_us, bool success);
void recordComputePipelineCreate(uint64_t duration_us, bool success);
void recordPsoArtifactCacheLookup(PsoArtifactKind kind,
                                  PsoArtifactCacheResult result,
                                  uint64_t wait_us = 0);
void recordPsoArtifactCompile(PsoArtifactKind kind, uint64_t duration_us,
                              bool success);
void recordMetalCommandBufferCommit(uint64_t duration_us);
void recordGpuFrameCompletion(uint64_t frame, uint64_t command_buffers,
                              uint64_t gpu_start_ns, uint64_t gpu_end_ns,
                              uint64_t gpu_busy_sum_ns,
                              uint64_t gpu_active_union_ns,
                              uint64_t gpu_idle_ns,
                              uint64_t commit_to_complete_sum_us,
                              uint64_t commit_to_complete_max_us,
                              uint64_t boundary_to_first_publish_us,
                              uint64_t publish_span_us,
                              uint64_t publish_gap_max_us,
                              uint64_t publish_to_encode_sum_us,
                              uint64_t publish_to_encode_max_us,
                              uint64_t encode_sum_us,
                              uint64_t encode_max_us,
                              uint64_t encode_prepare_sum_us,
                              uint64_t encode_prepare_max_us,
                              uint64_t encode_flush_sum_us,
                              uint64_t encode_flush_max_us,
                              uint64_t encode_to_commit_sum_us,
                              uint64_t encode_to_commit_max_us,
                              uint64_t first_publish_to_last_commit_us,
                              uint64_t max_encode_chunk,
                              uint32_t max_encode_input_encoders,
                              uint32_t max_encode_encoded_encoders,
                              uint32_t max_encode_render_encoders,
                              uint32_t max_encode_compute_encoders,
                              uint32_t max_encode_blit_encoders);
void recordGpuFrameEncodeBreakdown(uint64_t frame,
                                   const FrameStatistics &stats);
void recordDrawableAcquire(uint64_t duration_us);
void recordDescriptorContentWrite(uint32_t kind);
void recordNativeDescriptorBufferRecord(uint32_t kind);
void recordNativeDescriptorBufferRecordCounter();
void recordNativeDescriptorBufferRecordMissingResource();
void recordNativeDescriptorResourceTableEntry();

} // namespace dxmt::perf
