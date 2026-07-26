#include "d3d12_replay_compiled_execute_stats.hpp"

#include "d3d12_compiled_replay_ledger.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace dxmt::d3d12 {

void AccumulateCompiledReplayStageIntervals(
    dxmt::FrameStatistics *stats, const ReplayBreakdownAccumulators &rb) {
  const auto &rb_superseded_mask_interval = rb.superseded_mask_interval;
  const auto &rb_compiled_graphics_interval = rb.compiled_graphics_interval;
  const auto &rb_compiled_compute_interval = rb.compiled_compute_interval;
  const auto &rb_fallback_classification_interval =
      rb.fallback_classification_interval;
  const auto &rb_typed_record_interval = rb.typed_record_interval;
  const auto &rb_compiled_graphics_packets = rb.compiled_graphics_packets;
  const auto &rb_compiled_compute_packets = rb.compiled_compute_packets;
  const auto &rb_fallback_classification_ranges =
      rb.fallback_classification_ranges;
  stats->frame_replay_superseded_mask_interval +=
      rb_superseded_mask_interval;
  stats->frame_replay_compiled_graphics_interval +=
      rb_compiled_graphics_interval;
  stats->frame_replay_compiled_compute_interval +=
      rb_compiled_compute_interval;
  stats->frame_replay_fallback_classification_interval +=
      rb_fallback_classification_interval;
  stats->frame_replay_typed_record_interval +=
      rb_typed_record_interval;
  stats->frame_replay_compiled_graphics_packet_count +=
      rb_compiled_graphics_packets;
  stats->frame_replay_compiled_compute_packet_count +=
      rb_compiled_compute_packets;
  stats->frame_replay_fallback_classification_count +=
      rb_fallback_classification_ranges;
}

void AccumulateCompiledReplaySubPhaseIntervals(dxmt::FrameStatistics *stats,
                                               const PerDrawSubTimers &timers,
                                               const StallProbe &stall) {
  stats->frame_replay_get_pipeline_interval +=
      std::chrono::microseconds(stall.getPipelineUs);
  stats->frame_replay_get_metal_pso_interval +=
      std::chrono::microseconds(stall.psoSelectUs);
  stats->frame_replay_select_pso_interval +=
      std::chrono::microseconds(stall.selectPsoUs);
  stats->frame_replay_desc_access_interval +=
      std::chrono::microseconds(timers.desc);
  stats->frame_replay_state_update_interval +=
      std::chrono::microseconds(timers.stateUpd);
  stats->frame_replay_attach_interval +=
      std::chrono::microseconds(timers.attach);
  stats->frame_replay_bind_snapshot_interval +=
      std::chrono::microseconds(stall.bindSnapUs);
  stats->frame_replay_estimate_interval +=
      std::chrono::microseconds(stall.estimateUs);
  stats->frame_replay_packet_interval +=
      std::chrono::microseconds(stall.packetUs);
  stats->frame_replay_queue_interval +=
      std::chrono::microseconds(stall.queueUs);
  stats->frame_replay_emit_interval +=
      std::chrono::microseconds(stall.emitUs);
  stats->frame_replay_flush_blit_interval +=
      std::chrono::microseconds(timers.flushBlitUs);
  stats->frame_replay_flush_compute_interval +=
      std::chrono::microseconds(timers.flushComputeUs);
  stats->frame_replay_flush_graphics_interval +=
      std::chrono::microseconds(timers.flushGraphicsUs);
  stats->frame_replay_flush_barrier_interval +=
      std::chrono::microseconds(timers.flushBarrierUs);
  stats->frame_replay_build_plan_interval +=
      std::chrono::microseconds(timers.buildPlanUs);
  stats->frame_replay_emit_timestamp_interval +=
      std::chrono::microseconds(timers.emitTsMarkersUs);
}

void RecordCompiledFallbackRangeStats(CompiledReplayContext &ctx, UINT begin,
                                      UINT count,
                                      dxmt::CompiledFallbackReason reason) {
  auto &queue = ctx.queue;
  auto *test_telemetry = ctx.test_telemetry;
  const auto &records = ctx.records;
  auto &rb_fallback_classification_ranges =
      ctx.rb.fallback_classification_ranges;
  auto &rb_fallback_classification_interval =
      ctx.rb.fallback_classification_interval;
  if (test_telemetry && count) {
    test_telemetry->replayed_fallback_ranges.fetch_add(
        1, std::memory_order_relaxed);
    test_telemetry->replayed_fallback_records.fetch_add(
        count, std::memory_order_relaxed);
  }
  if (!dxmt::perf::enabled())
    return;
  const auto classification_begin = clock::now();
  rb_fallback_classification_ranges++;
  uint64_t graphics_packets = 0;
  uint64_t compute_packets = 0;
  const UINT end = std::min<UINT>(
      begin + count, static_cast<UINT>(records.size()));
  for (UINT i = begin; i < end; i++) {
    const auto &payload = records[i].payload;
    if (std::holds_alternative<DrawInstancedRecord>(payload) ||
        std::holds_alternative<DrawIndexedInstancedRecord>(payload)) {
      graphics_packets++;
      continue;
    }
    if (std::holds_alternative<DispatchRecord>(payload)) {
      compute_packets++;
      continue;
    }
    const auto *indirect =
        std::get_if<ExecuteIndirectRecord>(&payload);
    if (!indirect)
      continue;
    auto *signature = dynamic_cast<CommandSignature *>(
        indirect->command_signature.ptr());
    bool compute = false;
    if (signature) {
      for (const auto &argument : signature->GetArguments()) {
        if (argument.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH) {
          compute = true;
          break;
        }
      }
    }
    if (compute)
      compute_packets++;
    else
      graphics_packets++;
  }

  auto *stats = &queue.CurrentFrameStatistics();
  if (graphics_packets)
    dxmt::perf::recordFallbackGraphicsPackets(
        stats, graphics_packets, reason);
  if (compute_packets)
    dxmt::perf::recordFallbackComputePackets(
        stats, compute_packets, reason);
  rb_fallback_classification_interval +=
      clock::now() - classification_begin;
}

void RecordCompiledReplayFrameStatistics(CompiledReplayContext &ctx,
                                         uintptr_t queue_identity,
                                         D3D12_COMMAND_LIST_TYPE queue_type,
                                         clock::time_point rb_t0,
                                         clock::time_point rb_t1,
                                         clock::time_point rb_t2,
                                         clock::time_point rb_t3) {
  const auto &records = ctx.records;
  const auto &rb = ctx.rb;
  const auto &rb_stall_accum = rb.stall_accum;
  const auto rb_t4 = clock::now();
  auto *stats = dxmt::perf::currentFrameStatistics();
  dxmt::perf::recordReplayBreakdown(
      stats, rb_t1 - rb_t0, rb_t2 - rb_t1, rb_t3 - rb_t2, rb_t4 - rb_t3);
  if (stats) {
    AccumulateCompiledReplayStageIntervals(stats, rb);
    const auto &timers = perDrawSubTimers();
    const auto &stall = rb_stall_accum;
    AccumulateCompiledReplaySubPhaseIntervals(stats, timers, stall);
    AccumulateCompiledReplayRecordTypeStatistics(stats, timers);
    AccumulateCompiledReplayCounterStatistics(stats, timers);
  }
  const auto replay_total_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          rb_t4 - rb_t0).count();
  const auto &timers = perDrawSubTimers();
  if (replay_total_us >= 100000 || timers.graphicsPsoUnavailable ||
      timers.computePsoUnavailable) {
    static std::atomic<uint32_t> context_log_count = 0;
    if (context_log_count.fetch_add(1, std::memory_order_relaxed) < 512) {
      WARN_FILE_ONLY("DXMT replay context:"
           " queue=", queue_identity,
           " queueType=", queue_type,
           " records=", records.size(),
           " totalUs=", replay_total_us,
           " recordLoopUs=",
           std::chrono::duration_cast<std::chrono::microseconds>(
               rb_t1 - rb_t0).count(),
           " flushPassUs=",
           std::chrono::duration_cast<std::chrono::microseconds>(
               rb_t2 - rb_t1).count(),
           " lastD3DSeq=", dxmt::apitrace::current_d3d_sequence(),
           " drawCount=", timers.drawCount,
           " indexedDrawRecords=", timers.recordDrawIndexedCount,
           " dispatchRecords=", timers.recordDispatchCount,
           " rootTableRecords=", timers.recordRootTableCount,
           " vertexStateRecords=", timers.recordVertexIndexStateCount,
           " descAccessPassthrough=", timers.descAccessPassthrough,
           " bindlessStateCacheHit=", timers.bindlessReplayStateCacheHit,
           " bindlessStateCacheMiss=", timers.bindlessReplayStateCacheMiss,
           " graphicsPsoUnavailable=", timers.graphicsPsoUnavailable,
           " computePsoUnavailable=", timers.computePsoUnavailable,
           " firstComputePsoUnavailableKey=",
           timers.firstComputePsoUnavailableKey);
    }
  }
}

void LogCompiledReplayBreakdown(CompiledReplayContext &ctx,
                                clock::time_point rb_t0,
                                clock::time_point rb_t1,
                                clock::time_point rb_t2,
                                clock::time_point rb_t3) {
  const auto &records = ctx.records;
  const auto &rb_by_type = ctx.rb.by_type;
  const auto &rb_stall_total_us = ctx.rb.stall_total_us;
  const auto &rb_stall_accum = ctx.rb.stall_accum;
  const auto rb_t4 = clock::now();
  const auto us = [](auto d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
  };
  const auto total = us(rb_t4 - rb_t0);
  const uint64_t record_loop_us = us(rb_t1 - rb_t0);
  const uint64_t flush_pass_us = us(rb_t2 - rb_t1);
  const uint64_t timestamp_resolve_us = us(rb_t3 - rb_t2);
  const uint64_t cpu_query_resolve_us = us(rb_t4 - rb_t3);
  static const uint64_t rb_threshold_us = []() {
    auto v = env::getEnvVar("DXMT_DIAG_REPLAY_BREAKDOWN_US");
    return v.empty() ? uint64_t(50000) : strtoull(v.c_str(), nullptr, 10);
  }();
  // This threshold controls verbose per-type diagnostics only. The worker
  // frame ledger above records every replay batch regardless of duration.
  if (uint64_t(total) > rb_threshold_us) { // env-tunable; default 50ms explosion frames
    // Find the top-3 record types by accumulated time.
    std::vector<std::pair<const char *, std::pair<uint64_t, uint64_t>>>
        sorted(rb_by_type.begin(), rb_by_type.end());
    std::sort(sorted.begin(), sorted.end(), [](auto &a, auto &b) {
      return a.second.second > b.second.second;
    });
    std::string top;
    for (size_t i = 0; i < sorted.size() && i < 12; i++) {
      top += " [";
      top += sorted[i].first;
      top += " n=" + std::to_string(sorted[i].second.first) +
             " us=" + std::to_string(sorted[i].second.second) + "]";
    }
    WARN_FILE_ONLY("DXMT replay breakdown:"
         " records=", records.size(),
         " totalUs=", total,
         " recordLoopUs=", record_loop_us,
         " flushPassUs=", flush_pass_us,
         " timestampResolveUs=", timestamp_resolve_us,
         " cpuQueryResolveUs=", cpu_query_resolve_us,
         // recordLoop sub-phase split. DXMT_DIAG_STALL adds slow-record logs;
         // DXMT_PERF_STATS records the aggregate fields without per-record log spam.
         " stallTotalUs=", rb_stall_total_us,
         " stallGetPipelineUs=", rb_stall_accum.getPipelineUs,
         " stallPsoSelectUs=", rb_stall_accum.psoSelectUs,
         " stallSelectPsoUs=", rb_stall_accum.selectPsoUs,
         " stallDescAccessUs=", rb_stall_accum.descAccessUs,
         " stallAttachUs=", rb_stall_accum.attachUs,
         " stallBindSnapUs=", rb_stall_accum.bindSnapUs,
         " stallEstimateUs=", rb_stall_accum.estimateUs,
         " stallPacketUs=", rb_stall_accum.packetUs,
         " stallQueueUs=", rb_stall_accum.queueUs,
         " stallEmitUs=", rb_stall_accum.emitUs,
         " drawCount=", perDrawSubTimers().drawCount,
         " psoRootUnchanged=", perDrawSubTimers().psoRootUnchanged,
         " fullBindUnchanged=", perDrawSubTimers().fullBindUnchanged,
         " drawDescAccessUs=", perDrawSubTimers().desc,
         " drawStateUpdUs=", perDrawSubTimers().stateUpd,
         " drawAttachUs=", perDrawSubTimers().attach,
         " drawCloneUs=", perDrawSubTimers().clone,
         " flushBlitUs=", perDrawSubTimers().flushBlitUs,
         " flushBlitCalls=", perDrawSubTimers().flushBlitCalls,
         " blitBarrierTakeUs=", perDrawSubTimers().blitBarrierTakeUs,
         " blitBarrierLookupUs=", perDrawSubTimers().blitBarrierLookupUs,
         " blitBarrierScanUs=", perDrawSubTimers().blitBarrierScanUs,
         " blitBarrierRebuildMatchedUs=",
         perDrawSubTimers().blitBarrierRebuildMatchedUs,
         " blitBarrierRebuildRemainingUs=",
         perDrawSubTimers().blitBarrierRebuildRemainingUs,
         " blitBarrierAssignUs=", perDrawSubTimers().blitBarrierAssignUs,
         " blitSeparatorUs=", perDrawSubTimers().blitSeparatorUs,
         " blitEmitCommandUs=", perDrawSubTimers().blitEmitCommandUs,
         " blitBatchResetUs=", perDrawSubTimers().blitBatchResetUs,
         " blitPendingEntries=",
         perDrawSubTimers().blitBarrierPendingEntries,
         " blitMatchedEntries=",
         perDrawSubTimers().blitBarrierMatchedEntries,
         " blitRemainingEntries=",
         perDrawSubTimers().blitBarrierRemainingEntries,
         " blitPendingEntriesMax=",
         perDrawSubTimers().blitBarrierPendingEntriesMax,
         " blitBatchCommands=", perDrawSubTimers().blitBatchCommands,
         " blitBatchCommandsMax=",
         perDrawSubTimers().blitBatchCommandsMax,
         " blitBatchReads=", perDrawSubTimers().blitBatchReads,
         " blitBatchWrites=", perDrawSubTimers().blitBatchWrites,
         " queueBlitCalls=", perDrawSubTimers().queueBlitCalls,
         " queueBlitHazardFlushes=",
         perDrawSubTimers().queueBlitHazardFlushes,
         " queueBlitHazardUs=", perDrawSubTimers().queueBlitHazardUs,
         " queueBlitTrackUs=", perDrawSubTimers().queueBlitTrackUs,
         " queueBlitAppendUs=", perDrawSubTimers().queueBlitAppendUs,
         " copyTextureCalls=", perDrawSubTimers().copyTextureCalls,
         " copyTextureQueued=", perDrawSubTimers().copyTextureQueued,
         " copyTextureLookupUs=",
         perDrawSubTimers().copyTextureLookupUs,
         " copyTextureEnsureAllocationUs=",
         perDrawSubTimers().copyTextureEnsureAllocationUs,
         " copyTexturePrepareUs=", perDrawSubTimers().copyTexturePrepareUs,
         " copyTextureQueueUs=", perDrawSubTimers().copyTextureQueueUs,
         " flushComputeUs=", perDrawSubTimers().flushComputeUs,
         " flushComputeCalls=", perDrawSubTimers().flushComputeCalls,
         " flushGraphicsUs=", perDrawSubTimers().flushGraphicsUs,
         " flushGraphicsCalls=", perDrawSubTimers().flushGraphicsCalls,
         " buildPlanUs=", perDrawSubTimers().buildPlanUs,
         " flushBarrierUs=", perDrawSubTimers().flushBarrierUs,
         " flushBarrierCalls=", perDrawSubTimers().flushBarrierCalls,
         " emitTsMarkersUs=", perDrawSubTimers().emitTsMarkersUs,
         " emitTsMarkersCalls=", perDrawSubTimers().emitTsMarkersCalls,
         " flushPassBatchesCalls=", perDrawSubTimers().flushPassBatchesCalls,
         " flushPassBatchesEmpty=", perDrawSubTimers().flushPassBatchesEmpty,
         " resAccessCalls=", perDrawSubTimers().resAccessCalls,
         " resAccessSteadyNoop=", perDrawSubTimers().resAccessSteadyNoop,
         " descAccessHits=", perDrawSubTimers().descAccessHits,
         " descAccessMiss=", perDrawSubTimers().descAccessMiss,
         " descAccessPassthrough=", perDrawSubTimers().descAccessPassthrough,
         " mismatchBarriers=", perDrawSubTimers().mismatchBarriers,
         " appBarrierTransitions=", perDrawSubTimers().appBarrierTransitions,
         " getResourceCasts=", replayRttiCounters().getResource,
         " getPipelineCasts=", replayRttiCounters().getPipeline,
         " topTypes=", top);
  }
}

} // namespace dxmt::d3d12
