#include "d3d12_compiled_replay_ledger.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "dxmt_perf_stats.hpp"
#include "dxmt_statistics.hpp"

#include <atomic>
#include <chrono>

namespace dxmt::d3d12 {

bool ShouldLogCompiledReplayDiagnostic() {
  static std::atomic<uint32_t> replay_log_count = 0;
  return D3D12DiagShouldLog(replay_log_count, D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE"));
}

void AccumulateCompiledReplayRecordTypeStatistics(
    dxmt::FrameStatistics *stats, const PerDrawSubTimers &timers) {
  stats->frame_replay_draw_count += timers.drawCount;
  stats->frame_replay_record_draw_interval +=
      std::chrono::microseconds(timers.recordDrawUs);
  stats->frame_replay_record_draw_count += timers.recordDrawCount;
  stats->frame_replay_record_draw_indexed_interval +=
      std::chrono::microseconds(timers.recordDrawIndexedUs);
  stats->frame_replay_record_draw_indexed_count +=
      timers.recordDrawIndexedCount;
  stats->frame_replay_record_dispatch_interval +=
      std::chrono::microseconds(timers.recordDispatchUs);
  stats->frame_replay_record_dispatch_count += timers.recordDispatchCount;
  stats->frame_replay_record_pipeline_state_interval +=
      std::chrono::microseconds(timers.recordPipelineStateUs);
  stats->frame_replay_record_pipeline_state_count +=
      timers.recordPipelineStateCount;
  stats->frame_replay_record_descriptor_heaps_interval +=
      std::chrono::microseconds(timers.recordDescriptorHeapsUs);
  stats->frame_replay_record_descriptor_heaps_count +=
      timers.recordDescriptorHeapsCount;
  stats->frame_replay_record_root_signature_interval +=
      std::chrono::microseconds(timers.recordRootSignatureUs);
  stats->frame_replay_record_root_signature_count +=
      timers.recordRootSignatureCount;
  stats->frame_replay_record_root_table_interval +=
      std::chrono::microseconds(timers.recordRootTableUs);
  stats->frame_replay_record_root_table_count +=
      timers.recordRootTableCount;
  stats->frame_replay_record_root_descriptor_interval +=
      std::chrono::microseconds(timers.recordRootDescriptorUs);
  stats->frame_replay_record_root_descriptor_count +=
      timers.recordRootDescriptorCount;
  stats->frame_replay_record_root_constants_interval +=
      std::chrono::microseconds(timers.recordRootConstantsUs);
  stats->frame_replay_record_root_constants_count +=
      timers.recordRootConstantsCount;
  stats->frame_replay_record_vertex_index_state_interval +=
      std::chrono::microseconds(timers.recordVertexIndexStateUs);
  stats->frame_replay_record_vertex_index_state_count +=
      timers.recordVertexIndexStateCount;
  stats->frame_replay_record_render_targets_interval +=
      std::chrono::microseconds(timers.recordRenderTargetsUs);
  stats->frame_replay_record_render_targets_count +=
      timers.recordRenderTargetsCount;
  stats->frame_replay_record_resource_barrier_interval +=
      std::chrono::microseconds(timers.recordResourceBarrierUs);
  stats->frame_replay_record_resource_barrier_count +=
      timers.recordResourceBarrierCount;
  stats->frame_replay_record_copy_clear_resolve_interval +=
      std::chrono::microseconds(timers.recordCopyClearResolveUs);
  stats->frame_replay_record_copy_clear_resolve_count +=
      timers.recordCopyClearResolveCount;
  stats->frame_replay_record_query_interval +=
      std::chrono::microseconds(timers.recordQueryUs);
  stats->frame_replay_record_query_count += timers.recordQueryCount;
  stats->frame_replay_record_execute_indirect_interval +=
      std::chrono::microseconds(timers.recordExecuteIndirectUs);
  stats->frame_replay_record_execute_indirect_count +=
      timers.recordExecuteIndirectCount;
  stats->frame_replay_record_temporal_upscale_interval +=
      std::chrono::microseconds(timers.recordTemporalUpscaleUs);
  stats->frame_replay_record_temporal_upscale_count +=
      timers.recordTemporalUpscaleCount;
}

void AccumulateCompiledReplayCounterStatistics(
    dxmt::FrameStatistics *stats, const PerDrawSubTimers &timers) {
  stats->frame_replay_pso_root_unchanged += timers.psoRootUnchanged;
  stats->frame_replay_full_bind_unchanged += timers.fullBindUnchanged;
  stats->frame_replay_flush_blit_count += timers.flushBlitCalls;
  stats->frame_replay_flush_compute_count += timers.flushComputeCalls;
  stats->frame_replay_flush_graphics_count += timers.flushGraphicsCalls;
  stats->frame_replay_flush_barrier_count += timers.flushBarrierCalls;
  stats->frame_replay_emit_timestamp_count += timers.emitTsMarkersCalls;
  stats->frame_replay_flush_pass_batches_count +=
      timers.flushPassBatchesCalls;
  stats->frame_replay_flush_pass_batches_empty +=
      timers.flushPassBatchesEmpty;
  stats->frame_replay_resource_access_count += timers.resAccessCalls;
  stats->frame_replay_resource_access_steady_noop +=
      timers.resAccessSteadyNoop;
  stats->frame_replay_desc_access_hit += timers.descAccessHits;
  stats->frame_replay_desc_access_miss += timers.descAccessMiss;
  stats->frame_replay_desc_access_passthrough +=
      timers.descAccessPassthrough;
  stats->frame_replay_superseded_state_records_skipped +=
      timers.supersededStateRecordsSkipped;
  dxmt::perf::recordStateRecordsElided(
      stats, timers.supersededStateRecordsSkipped);
  stats->frame_replay_mismatch_barriers += timers.mismatchBarriers;
  stats->frame_replay_app_barrier_transitions +=
      timers.appBarrierTransitions;
  stats->frame_replay_binding_gen_bumps += timers.bindingGenBumps;
  stats->frame_replay_binding_gen_pipeline += timers.bindingGenPipeline;
  stats->frame_replay_binding_gen_descriptor_heaps +=
      timers.bindingGenDescriptorHeaps;
  stats->frame_replay_binding_gen_vertex_buffers +=
      timers.bindingGenVertexBuffers;
  stats->frame_replay_binding_gen_root_signature +=
      timers.bindingGenRootSignature;
  stats->frame_replay_binding_gen_root_descriptor_table +=
      timers.bindingGenRootDescriptorTable;
  stats->frame_replay_binding_gen_root_descriptor +=
      timers.bindingGenRootDescriptor;
  stats->frame_replay_binding_gen_root_constants +=
      timers.bindingGenRootConstants;
  stats->frame_replay_binding_gen_indirect_vertex_buffer +=
      timers.bindingGenIndirectVertexBuffer;
  stats->frame_replay_binding_gen_indirect_root_constants +=
      timers.bindingGenIndirectRootConstants;
  stats->frame_replay_binding_gen_indirect_root_descriptor +=
      timers.bindingGenIndirectRootDescriptor;
  stats->frame_replay_snapshot_requests += timers.snapshotRequests;
  stats->frame_replay_snapshot_cache_hits += timers.snapshotCacheHits;
  stats->frame_replay_snapshot_cache_misses += timers.snapshotCacheMisses;
  stats->frame_replay_snapshot_passthrough += timers.snapshotPassthrough;
  stats->frame_replay_snapshot_graphics_gen_changes +=
      timers.snapshotGraphicsGenChanges;
  stats->frame_replay_snapshot_descriptor_gen_changes +=
      timers.snapshotDescriptorGenChanges;
  stats->frame_replay_snapshot_both_gen_changes +=
      timers.snapshotBothGenChanges;
  stats->frame_replay_snapshot_no_gen_changes +=
      timers.snapshotNoGenChanges;
  stats->frame_replay_snapshot_captured_entries +=
      timers.snapshotCapturedEntries;
  stats->frame_replay_snapshot_captured_descriptors +=
      timers.snapshotCapturedDescriptors;
  stats->frame_replay_snapshot_captured_missing_descriptors +=
      timers.snapshotCapturedMissingDescriptors;
  stats->frame_replay_snapshot_captured_root_descriptors +=
      timers.snapshotCapturedRootDescriptors;
  stats->frame_replay_snapshot_captured_root_constants +=
      timers.snapshotCapturedRootConstants;
  stats->frame_replay_snapshot_captured_vertex_buffers +=
      timers.snapshotCapturedVertexBuffers;
  stats->frame_replay_snapshot_captured_bindless +=
      timers.snapshotCapturedBindless;
}

} // namespace dxmt::d3d12
