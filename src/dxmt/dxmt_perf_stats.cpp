#include "dxmt_perf_stats.hpp"

#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <version.h>
#include <algorithm>
#include <atomic>
#include <chrono>

namespace dxmt::perf {
namespace {

struct PsoArtifactCounters {
  std::atomic<uint64_t> cache_hits = {0};
  std::atomic<uint64_t> cache_misses = {0};
  std::atomic<uint64_t> cache_waits = {0};
  std::atomic<uint64_t> cache_wait_us = {0};
  std::atomic<uint64_t> compiles = {0};
  std::atomic<uint64_t> compile_us = {0};
  std::atomic<uint64_t> compile_failures = {0};
};

struct Counters {
  std::atomic<uint64_t> frames = {0};
  std::atomic<uint64_t> wait_cpu_fence_count = {0};
  std::atomic<uint64_t> wait_cpu_fence_us = {0};
  std::atomic<uint64_t> wait_cpu_fence_max_us = {0};
  std::atomic<uint64_t> timestamp_gpu_runs = {0};
  std::atomic<uint64_t> timestamp_gpu_queries = {0};
  std::atomic<uint64_t> timestamp_cpu_fallbacks = {0};
  std::atomic<uint64_t> timestamp_cpu_fallback_queries = {0};
  std::atomic<uint64_t> timestamp_cpu_deferred = {0};
  std::atomic<uint64_t> timestamp_cpu_deferred_queries = {0};
  std::atomic<uint64_t> timestamp_cpu_immediate = {0};
  std::atomic<uint64_t> timestamp_cpu_unsafe = {0};
  std::atomic<uint64_t> timestamp_cpu_materialized = {0};
  std::atomic<uint64_t> timestamp_cpu_wait_us = {0};
  std::atomic<uint64_t> timestamp_cpu_wait_max_us = {0};
  std::atomic<uint64_t> query_batch_waits = {0};
  std::atomic<uint64_t> query_batch_wait_queries = {0};
  std::atomic<uint64_t> query_batch_wait_us = {0};
  std::atomic<uint64_t> query_batch_wait_max_us = {0};
  std::atomic<uint64_t> tile_mapping_calls = {0};
  std::atomic<uint64_t> tile_mapping_standard_ops = {0};
  std::atomic<uint64_t> tile_mapping_packed_ops = {0};
  std::atomic<uint64_t> tile_mapping_map_ops = {0};
  std::atomic<uint64_t> tile_mapping_unmap_ops = {0};
  std::atomic<uint64_t> tile_mapping_invalid = {0};
  std::atomic<uint64_t> tile_mapping_metal_failures = {0};
  std::atomic<uint64_t> tile_mapping_barriers = {0};
  std::atomic<uint64_t> graphics_pso_creates = {0};
  std::atomic<uint64_t> graphics_pso_create_us = {0};
  std::atomic<uint64_t> graphics_pso_create_max_us = {0};
  std::atomic<uint64_t> graphics_pso_create_failures = {0};
  std::atomic<uint64_t> compute_pso_creates = {0};
  std::atomic<uint64_t> compute_pso_create_us = {0};
  std::atomic<uint64_t> compute_pso_create_max_us = {0};
  std::atomic<uint64_t> compute_pso_create_failures = {0};
  PsoArtifactCounters graphics_pso_artifacts;
  PsoArtifactCounters compute_pso_artifacts;
  PsoArtifactCounters graphics_pso_variant_artifacts;
  std::atomic<uint64_t> frame_wall_us = {0};
  std::atomic<uint64_t> frame_wall_max_us = {0};
  std::atomic<uint64_t> frame_command_buffers = {0};
  std::atomic<uint64_t> frame_command_buffers_max = {0};
  std::atomic<uint64_t> frame_render_passes = {0};
  std::atomic<uint64_t> frame_render_passes_max = {0};
  std::atomic<uint64_t> frame_render_commands = {0};
  std::atomic<uint64_t> frame_render_commands_max = {0};
  std::atomic<uint64_t> frame_render_draws = {0};
  std::atomic<uint64_t> frame_render_draws_max = {0};
  std::atomic<uint64_t> frame_render_indexed = {0};
  std::atomic<uint64_t> frame_render_indexed_max = {0};
  std::atomic<uint64_t> frame_render_pso_binds = {0};
  std::atomic<uint64_t> frame_render_pso_binds_max = {0};
  std::atomic<uint64_t> frame_compute_passes = {0};
  std::atomic<uint64_t> frame_compute_passes_max = {0};
  std::atomic<uint64_t> frame_compute_commands = {0};
  std::atomic<uint64_t> frame_compute_commands_max = {0};
  std::atomic<uint64_t> frame_compute_dispatches = {0};
  std::atomic<uint64_t> frame_compute_dispatches_max = {0};
  std::atomic<uint64_t> frame_blit_passes = {0};
  std::atomic<uint64_t> frame_blit_passes_max = {0};
  std::atomic<uint64_t> frame_blit_empty_passes = {0};
  std::atomic<uint64_t> frame_blit_deferred_fence_only_passes = {0};
  std::atomic<uint64_t> frame_blit_merged_fence_only_passes = {0};
  std::atomic<uint64_t> frame_blit_barrier_only_passes = {0};
  std::atomic<uint64_t> frame_blit_separator_passes = {0};
  std::atomic<uint64_t> frame_resource_barrier_batches_merged = {0};
  std::atomic<uint64_t> frame_resource_barrier_batches_graphics_inlined = {0};
  std::atomic<uint64_t> frame_resource_barrier_batches_compute_inlined = {0};
  std::atomic<uint64_t> frame_flush_chunks = {0};
  std::atomic<uint64_t> frame_flush_chunks_max = {0};
  std::atomic<uint64_t> frame_flush_encoders = {0};
  std::atomic<uint64_t> frame_flush_encoders_max = {0};
  std::atomic<uint64_t> frame_encoded_encoders = {0};
  std::atomic<uint64_t> frame_encoded_encoders_max = {0};
  std::atomic<uint64_t> frame_flush_render = {0};
  std::atomic<uint64_t> frame_flush_render_max = {0};
  std::atomic<uint64_t> frame_flush_compute = {0};
  std::atomic<uint64_t> frame_flush_compute_max = {0};
  std::atomic<uint64_t> frame_flush_blit = {0};
  std::atomic<uint64_t> frame_flush_blit_max = {0};
  std::atomic<uint64_t> frame_encoder_switches = {0};
  std::atomic<uint64_t> frame_encoder_switches_max = {0};
  std::atomic<uint64_t> frame_flush_us = {0};
  std::atomic<uint64_t> frame_flush_max_us = {0};
  std::atomic<uint64_t> frame_commit_us = {0};
  std::atomic<uint64_t> frame_commit_max_us = {0};
  std::atomic<uint64_t> metal_commit_count = {0};
  std::atomic<uint64_t> metal_commit_wall_us = {0};
  std::atomic<uint64_t> metal_commit_wall_max_us = {0};
  std::atomic<uint64_t> drawable_acquire_count = {0};
  std::atomic<uint64_t> drawable_acquire_us = {0};
  std::atomic<uint64_t> drawable_acquire_max_us = {0};
  std::atomic<uint64_t> descriptor_content_writes = {0};
  std::atomic<uint64_t> descriptor_content_write_cbv = {0};
  std::atomic<uint64_t> descriptor_content_write_srv = {0};
  std::atomic<uint64_t> descriptor_content_write_uav = {0};
  std::atomic<uint64_t> descriptor_content_write_sampler = {0};
  std::atomic<uint64_t> descriptor_content_write_copy = {0};
  std::atomic<uint64_t> descriptor_content_write_feedback_uav = {0};
  std::atomic<uint64_t> native_descriptor_buffer_records = {0};
  std::atomic<uint64_t> native_descriptor_buffer_record_cbv = {0};
  std::atomic<uint64_t> native_descriptor_buffer_record_srv = {0};
  std::atomic<uint64_t> native_descriptor_buffer_record_uav = {0};
  std::atomic<uint64_t> native_descriptor_buffer_record_counters = {0};
  std::atomic<uint64_t> native_descriptor_buffer_record_missing_resource = {0};
  std::atomic<uint64_t> native_descriptor_resource_table_entries = {0};
  std::atomic<uint64_t> avg_command_buffers = {0};
  std::atomic<uint64_t> avg_render_passes = {0};
  std::atomic<uint64_t> avg_render_draws = {0};
  std::atomic<uint64_t> avg_render_indexed = {0};
  std::atomic<uint64_t> avg_compute_passes = {0};
  std::atomic<uint64_t> avg_compute_dispatches = {0};
  std::atomic<uint64_t> avg_blit_passes = {0};
  std::atomic<uint64_t> avg_flush_chunks = {0};
  std::atomic<uint64_t> avg_flush_encoders = {0};
  std::atomic<uint64_t> avg_encoded_encoders = {0};
  std::atomic<uint64_t> avg_flush_render = {0};
  std::atomic<uint64_t> avg_flush_compute = {0};
  std::atomic<uint64_t> avg_flush_blit = {0};
  std::atomic<uint64_t> avg_encoder_switches = {0};
  std::atomic<uint64_t> avg_flush_us = {0};
  std::atomic<uint64_t> avg_commit_us = {0};
  std::atomic<uint64_t> avg_frame_wall_us = {0};
  std::atomic<uint64_t> avg_drawable_block_us = {0};
  std::atomic<uint64_t> avg_present_latency_us = {0};
};

Counters g_counters;
dxmt::mutex g_flush_mutex;
std::atomic<uint64_t> g_last_flush_frame = {0};
const dxmt::clock::time_point g_perf_start_time = dxmt::clock::now();
std::atomic<uint64_t> g_last_frame_log_monotonic_us = {0};
thread_local FrameStatistics *t_current_frame_stats = nullptr;
thread_local ScopedCodeTimer *t_active_code_timer = nullptr;

bool parseEnabledEnv(const char *name) {
  const auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" ||
         value == "on";
}

uint64_t sample(std::atomic<uint64_t> &value) {
  return value.load(std::memory_order_relaxed);
}

uint64_t durationUs(dxmt::clock::duration value) {
  return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
}

PsoArtifactCounters &psoArtifactCounters(PsoArtifactKind kind) {
  switch (kind) {
  case PsoArtifactKind::Graphics:
    return g_counters.graphics_pso_artifacts;
  case PsoArtifactKind::Compute:
    return g_counters.compute_pso_artifacts;
  case PsoArtifactKind::GraphicsVariant:
    return g_counters.graphics_pso_variant_artifacts;
  }
  return g_counters.graphics_pso_artifacts;
}

constexpr std::array<const char *, kPerfCodePathCount> kPerfCodePathNames = {
    "commandListCloseControl",
    "commandListCloseBuildCall",
    "commandListCloseAllocatorRelease",
    "commandListCloseApitrace",
    "commandListResetControl",
    "commandListResetAllocator",
    "commandListResetStateClear",
    "commandListResetInitialPipeline",
    "commandListResetApitrace",
    "commandListReferenceCount",
    "commandListObjectApi",
    "commandListFeatureSupport",
    "commandListDrawInstanced",
    "commandListDrawIndexedInstanced",
    "commandListDispatch",
    "commandListCopyBuffer",
    "commandListCopyTexture",
    "commandListCopyResource",
    "commandListCopyTiles",
    "commandListResolve",
    "commandListClearState",
    "commandListPipelineState",
    "commandListResourceBarrier",
    "commandListDescriptorHeaps",
    "commandListRootSignature",
    "commandListRootDescriptorTable",
    "commandListRootConstants",
    "commandListRootDescriptor",
    "commandListPrimitiveTopology",
    "commandListViewports",
    "commandListScissors",
    "commandListBlendFactor",
    "commandListStencilRef",
    "commandListExecuteBundle",
    "commandListIndexBuffer",
    "commandListVertexBuffers",
    "commandListStreamOutput",
    "commandListRenderTargets",
    "commandListClearDepthStencil",
    "commandListClearRenderTarget",
    "commandListClearUav",
    "commandListDiscard",
    "commandListQueryBeginEnd",
    "commandListQueryResolve",
    "commandListPredication",
    "commandListExecuteIndirect",
    "commandListMarkerEvent",
    "commandListDepthBounds",
    "commandListSamplePositions",
    "commandListViewInstanceMask",
    "commandListWriteBufferImmediate",
    "commandListProtectedSession",
    "commandListRenderPassBegin",
    "commandListRenderPassEnd",
    "commandListTemporalUpscale",
    "commandListUnsupportedFeature",
    "commandListSnapshotCapture",
    "commandListRecordVectorAppend",
    "deviceCreateDescriptorHeap",
    "deviceGetDescriptorHandleIncrementSize",
    "deviceCreateConstantBufferView",
    "deviceCreateShaderResourceView",
    "deviceCreateUnorderedAccessView",
    "deviceCreateRenderTargetView",
    "deviceCreateDepthStencilView",
    "deviceCreateSampler",
    "deviceCopyDescriptors",
    "deviceCopyDescriptorsSimple",
    "deviceCreateSamplerFeedbackUav",
    "descriptorHandleResolve",
    "descriptorLockAcquire",
    "descriptorRecordReset",
    "descriptorRecordCopy",
    "descriptorSlotBeginWrite",
    "descriptorRevisionCommit",
    "descriptorSamplerMirrorMaterialize",
    "descriptorTableMaterializeControl",
    "descriptorTableMirrorMutation",
    "descriptorTableTextureViewCreate",
    "descriptorTableBufferTextureView",
    "descriptorTableNativeBufferRecord",
    "descriptorTableResidencyUpdate",
    "descriptorTableDenseDiagnostic",
    "queueExecuteControl",
    "queueExecuteValidation",
    "queueExecuteApitrace",
    "queueExecuteCollect",
    "queueExecutePreparePlan",
    "queueExecuteCaptureDescriptors",
    "queueCaptureDescriptorCacheLookup",
    "queueCaptureDescriptorSnapshotBuild",
    "queueCaptureDescriptorJournalFinalize",
    "queueEnqueueControl",
    "queueEnqueueFrameTag",
    "queueEnqueueLock",
    "queueEnqueueNotify",
    "compiledBuildLoopDispatch",
    "compiledBuildPipelineMetadata",
    "compiledBuildPacketStateCopy",
    "compiledBuildRootTableMaterialize",
    "compiledBuildNativeBindingDispatch",
    "compiledBuildNativeStageCacheKey",
    "compiledBuildNativeStageCacheLookup",
    "compiledBuildNativeShaderLookup",
    "compiledBuildNativeRootBaseLayout",
    "compiledBuildNativeRootBaseRangeScan",
    "compiledBuildNativePayloadAppend",
    "compiledBuildNativeDiagnosticCopy",
    "compiledBuildNativeStageCacheStore",
    "compiledBuildSegmentAppend",
    "compiledBuildStateUpdate",
    "compiledBuildPayloadBufferCreate",
    "compiledBuildPayloadBufferCopy",
    "compiledBuildFallbackRewrite",
};
static_assert(kPerfCodePathNames.size() == kPerfCodePathCount);

void addCompiledFallbackReason(FrameStatistics &stats,
                               CompiledFallbackReason reason,
                               uint64_t count) {
  const auto index = static_cast<size_t>(reason);
  if (index < stats.frame_compiled_fallback_reasons.size())
    stats.frame_compiled_fallback_reasons[index] += count;
}

void addDurationToBucket(FrameStatistics &stats, FrameTimeBucket bucket,
                         dxmt::clock::duration duration) {
  switch (bucket) {
  case FrameTimeBucket::ExecuteCommandLists:
    stats.frame_execute_command_lists_interval += duration;
    break;
  case FrameTimeBucket::Present:
    stats.frame_present_interval += duration;
    break;
  case FrameTimeBucket::QueueSignal:
    stats.frame_queue_signal_interval += duration;
    break;
  case FrameTimeBucket::QueueWait:
    stats.frame_queue_wait_interval += duration;
    break;
  case FrameTimeBucket::CreateResource:
    stats.frame_create_resource_interval += duration;
    break;
  case FrameTimeBucket::CreateReservedResource:
    stats.frame_create_reserved_resource_interval += duration;
    break;
  case FrameTimeBucket::CreateHeap:
    stats.frame_create_heap_interval += duration;
    break;
  case FrameTimeBucket::CreatePipeline:
    stats.frame_create_pipeline_interval += duration;
    break;
  case FrameTimeBucket::TileMapping:
    stats.frame_tile_mapping_interval += duration;
    break;
  case FrameTimeBucket::CommandListRecord:
    stats.frame_cmdlist_record_interval += duration;
    break;
  }
}

void addDurationToBucket(FrameStatistics &stats, ExecuteTimeBucket bucket,
                         dxmt::clock::duration duration) {
  switch (bucket) {
  case ExecuteTimeBucket::Validate:
    stats.frame_execute_validate_interval += duration;
    break;
  case ExecuteTimeBucket::Collect:
    stats.frame_execute_collect_interval += duration;
    break;
  case ExecuteTimeBucket::Enqueue:
    stats.frame_execute_enqueue_interval += duration;
    break;
  case ExecuteTimeBucket::Drain:
    stats.frame_execute_drain_interval += duration;
    break;
  case ExecuteTimeBucket::DrainLock:
    stats.frame_execute_drain_lock_interval += duration;
    break;
  case ExecuteTimeBucket::Replay:
    stats.frame_execute_replay_interval += duration;
    break;
  case ExecuteTimeBucket::Decay:
    stats.frame_execute_decay_interval += duration;
    break;
  case ExecuteTimeBucket::Signal:
    stats.frame_execute_signal_interval += duration;
    break;
  case ExecuteTimeBucket::Commit:
    stats.frame_execute_commit_interval += duration;
    break;
  case ExecuteTimeBucket::WaitArm:
    stats.frame_execute_wait_arm_interval += duration;
    break;
  }
}

void updateMax(std::atomic<uint64_t> &target, uint64_t value) {
  auto current = target.load(std::memory_order_relaxed);
  while (current < value &&
         !target.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed)) {
  }
}

void flushCounters(uint64_t frame, bool final) {
  if (!enabled())
    return;

  if (frame == 0)
    return;

  std::lock_guard lock(g_flush_mutex);
  const auto last = g_last_flush_frame.load(std::memory_order_relaxed);
  if (last == frame && !final)
    return;
  g_last_flush_frame.store(frame, std::memory_order_relaxed);
  Logger::logFileOnly(
      LogLevel::Info,
      str::format("DXMT perf stats:"
                  " frame=", frame,
                  " final=", final ? 1 : 0,
                  " totalFrames=", sample(g_counters.frames),
                  " waitCpuFenceCount=",
                  sample(g_counters.wait_cpu_fence_count),
                  " waitCpuFenceUs=", sample(g_counters.wait_cpu_fence_us),
                  " waitCpuFenceMaxUs=",
                  sample(g_counters.wait_cpu_fence_max_us),
                  " tsGpuRuns=", sample(g_counters.timestamp_gpu_runs),
                  " tsGpuQueries=", sample(g_counters.timestamp_gpu_queries),
                  " tsCpuFallbacks=",
                  sample(g_counters.timestamp_cpu_fallbacks),
                  " tsCpuFallbackQueries=",
                  sample(g_counters.timestamp_cpu_fallback_queries),
                  " tsCpuDeferred=",
                  sample(g_counters.timestamp_cpu_deferred),
                  " tsCpuDeferredQueries=",
                  sample(g_counters.timestamp_cpu_deferred_queries),
                  " tsCpuImmediate=",
                  sample(g_counters.timestamp_cpu_immediate),
                  " tsCpuUnsafe=", sample(g_counters.timestamp_cpu_unsafe),
                  " tsCpuMaterialized=",
                  sample(g_counters.timestamp_cpu_materialized),
                  " tsCpuWaitUs=", sample(g_counters.timestamp_cpu_wait_us),
                  " tsCpuWaitMaxUs=",
                  sample(g_counters.timestamp_cpu_wait_max_us),
                  " queryBatchWaits=", sample(g_counters.query_batch_waits),
                  " queryBatchWaitQueries=",
                  sample(g_counters.query_batch_wait_queries),
                  " queryBatchWaitUs=", sample(g_counters.query_batch_wait_us),
                  " queryBatchWaitMaxUs=",
                  sample(g_counters.query_batch_wait_max_us),
                  " tileMappingCalls=",
                  sample(g_counters.tile_mapping_calls),
                  " tileMappingStandardOps=",
                  sample(g_counters.tile_mapping_standard_ops),
                  " tileMappingPackedOps=",
                  sample(g_counters.tile_mapping_packed_ops),
                  " tileMappingMapOps=",
                  sample(g_counters.tile_mapping_map_ops),
                  " tileMappingUnmapOps=",
                  sample(g_counters.tile_mapping_unmap_ops),
                  " tileMappingInvalid=",
                  sample(g_counters.tile_mapping_invalid),
                  " tileMappingMetalFailures=",
                  sample(g_counters.tile_mapping_metal_failures),
                  " tileMappingBarriers=",
                  sample(g_counters.tile_mapping_barriers),
                  " graphicsPsoCreates=",
                  sample(g_counters.graphics_pso_creates),
                  " graphicsPsoCreateUs=",
                  sample(g_counters.graphics_pso_create_us),
                  " graphicsPsoCreateMaxUs=",
                  sample(g_counters.graphics_pso_create_max_us),
                  " graphicsPsoCreateFailures=",
                  sample(g_counters.graphics_pso_create_failures),
                  " computePsoCreates=",
                  sample(g_counters.compute_pso_creates),
                  " computePsoCreateUs=",
                  sample(g_counters.compute_pso_create_us),
                  " computePsoCreateMaxUs=",
                  sample(g_counters.compute_pso_create_max_us),
                  " computePsoCreateFailures=",
                  sample(g_counters.compute_pso_create_failures),
                  " graphicsPsoCacheHits=",
                  sample(g_counters.graphics_pso_artifacts.cache_hits),
                  " graphicsPsoCacheMisses=",
                  sample(g_counters.graphics_pso_artifacts.cache_misses),
                  " graphicsPsoCacheWaits=",
                  sample(g_counters.graphics_pso_artifacts.cache_waits),
                  " graphicsPsoCacheWaitUs=",
                  sample(g_counters.graphics_pso_artifacts.cache_wait_us),
                  " graphicsPsoArtifactCompiles=",
                  sample(g_counters.graphics_pso_artifacts.compiles),
                  " graphicsPsoArtifactCompileUs=",
                  sample(g_counters.graphics_pso_artifacts.compile_us),
                  " graphicsPsoArtifactCompileFailures=",
                  sample(g_counters.graphics_pso_artifacts.compile_failures),
                  " computePsoCacheHits=",
                  sample(g_counters.compute_pso_artifacts.cache_hits),
                  " computePsoCacheMisses=",
                  sample(g_counters.compute_pso_artifacts.cache_misses),
                  " computePsoCacheWaits=",
                  sample(g_counters.compute_pso_artifacts.cache_waits),
                  " computePsoCacheWaitUs=",
                  sample(g_counters.compute_pso_artifacts.cache_wait_us),
                  " computePsoArtifactCompiles=",
                  sample(g_counters.compute_pso_artifacts.compiles),
                  " computePsoArtifactCompileUs=",
                  sample(g_counters.compute_pso_artifacts.compile_us),
                  " computePsoArtifactCompileFailures=",
                  sample(g_counters.compute_pso_artifacts.compile_failures),
                  " graphicsPsoVariantCacheHits=",
                  sample(g_counters.graphics_pso_variant_artifacts.cache_hits),
                  " graphicsPsoVariantCacheMisses=",
                  sample(g_counters.graphics_pso_variant_artifacts.cache_misses),
                  " graphicsPsoVariantCacheWaits=",
                  sample(g_counters.graphics_pso_variant_artifacts.cache_waits),
                  " graphicsPsoVariantCacheWaitUs=",
                  sample(g_counters.graphics_pso_variant_artifacts.cache_wait_us),
                  " graphicsPsoVariantArtifactCompiles=",
                  sample(g_counters.graphics_pso_variant_artifacts.compiles),
                  " graphicsPsoVariantArtifactCompileUs=",
                  sample(g_counters.graphics_pso_variant_artifacts.compile_us),
                  " graphicsPsoVariantArtifactCompileFailures=",
                  sample(g_counters.graphics_pso_variant_artifacts.compile_failures),
                  " frameWallUs=", sample(g_counters.frame_wall_us),
                  " frameWallMaxUs=",
                  sample(g_counters.frame_wall_max_us),
                  " frameWallAvgUs=",
                  sample(g_counters.avg_frame_wall_us),
                  " frameCmdBufs=",
                  sample(g_counters.frame_command_buffers),
                  " frameCmdBufsMax=",
                  sample(g_counters.frame_command_buffers_max),
                  " frameRenderPasses=",
                  sample(g_counters.frame_render_passes),
                  " frameRenderPassesMax=",
                  sample(g_counters.frame_render_passes_max),
                  " frameRenderCmds=",
                  sample(g_counters.frame_render_commands),
                  " frameRenderCmdsMax=",
                  sample(g_counters.frame_render_commands_max),
                  " frameRenderDraws=",
                  sample(g_counters.frame_render_draws),
                  " frameRenderDrawsMax=",
                  sample(g_counters.frame_render_draws_max),
                  " frameRenderIndexed=",
                  sample(g_counters.frame_render_indexed),
                  " frameRenderIndexedMax=",
                  sample(g_counters.frame_render_indexed_max),
                  " frameRenderPsoBinds=",
                  sample(g_counters.frame_render_pso_binds),
                  " frameRenderPsoBindsMax=",
                  sample(g_counters.frame_render_pso_binds_max),
                  " frameComputePasses=",
                  sample(g_counters.frame_compute_passes),
                  " frameComputePassesMax=",
                  sample(g_counters.frame_compute_passes_max),
                  " frameComputeCmds=",
                  sample(g_counters.frame_compute_commands),
                  " frameComputeCmdsMax=",
                  sample(g_counters.frame_compute_commands_max),
                  " frameComputeDispatches=",
                  sample(g_counters.frame_compute_dispatches),
                  " frameComputeDispatchesMax=",
                  sample(g_counters.frame_compute_dispatches_max),
                  " frameBlitPasses=", sample(g_counters.frame_blit_passes),
                  " frameBlitPassesMax=",
                  sample(g_counters.frame_blit_passes_max),
                  " frameBlitEmptyPasses=",
                  sample(g_counters.frame_blit_empty_passes),
                  " frameBlitDeferredFenceOnlyPasses=",
                  sample(g_counters.frame_blit_deferred_fence_only_passes),
                  " frameBlitMergedFenceOnlyPasses=",
                  sample(g_counters.frame_blit_merged_fence_only_passes),
                  " frameBlitBarrierOnlyPasses=",
                  sample(g_counters.frame_blit_barrier_only_passes),
                  " frameBlitSeparatorPasses=",
                  sample(g_counters.frame_blit_separator_passes),
                  " frameResourceBarrierBatchesMerged=",
                  sample(g_counters.frame_resource_barrier_batches_merged),
                  " frameResourceBarrierBatchesGraphicsInlined=",
                  sample(g_counters.frame_resource_barrier_batches_graphics_inlined),
                  " frameResourceBarrierBatchesComputeInlined=",
                  sample(g_counters.frame_resource_barrier_batches_compute_inlined),
                  " frameFlushChunks=",
                  sample(g_counters.frame_flush_chunks),
                  " frameFlushChunksMax=",
                  sample(g_counters.frame_flush_chunks_max),
                  " frameFlushEncoders=",
                  sample(g_counters.frame_flush_encoders),
                  " frameFlushEncodersMax=",
                  sample(g_counters.frame_flush_encoders_max),
                  " frameEncodedEncoders=",
                  sample(g_counters.frame_encoded_encoders),
                  " frameEncodedEncodersMax=",
                  sample(g_counters.frame_encoded_encoders_max),
                  " frameFlushRender=",
                  sample(g_counters.frame_flush_render),
                  " frameFlushRenderMax=",
                  sample(g_counters.frame_flush_render_max),
                  " frameFlushCompute=",
                  sample(g_counters.frame_flush_compute),
                  " frameFlushComputeMax=",
                  sample(g_counters.frame_flush_compute_max),
                  " frameFlushBlit=", sample(g_counters.frame_flush_blit),
                  " frameFlushBlitMax=",
                  sample(g_counters.frame_flush_blit_max),
                  " frameEncoderSwitches=",
                  sample(g_counters.frame_encoder_switches),
                  " frameEncoderSwitchesMax=",
                  sample(g_counters.frame_encoder_switches_max),
                  " frameFlushUs=", sample(g_counters.frame_flush_us),
                  " frameFlushMaxUs=",
                  sample(g_counters.frame_flush_max_us),
                  " frameCommitUs=", sample(g_counters.frame_commit_us),
                  " frameCommitMaxUs=",
                  sample(g_counters.frame_commit_max_us),
                  " metalCommitCount=",
                  sample(g_counters.metal_commit_count),
                  " metalCommitWallUs=",
                  sample(g_counters.metal_commit_wall_us),
                  " metalCommitWallMaxUs=",
                  sample(g_counters.metal_commit_wall_max_us),
                  " drawableAcquireCount=",
                  sample(g_counters.drawable_acquire_count),
                  " drawableAcquireUs=",
                  sample(g_counters.drawable_acquire_us),
                  " drawableAcquireMaxUs=",
                  sample(g_counters.drawable_acquire_max_us),
                  " avgCmdBufs=", sample(g_counters.avg_command_buffers),
                  " avgRenderPasses=", sample(g_counters.avg_render_passes),
                  " avgRenderDraws=", sample(g_counters.avg_render_draws),
                  " avgRenderIndexed=",
                  sample(g_counters.avg_render_indexed),
                  " avgComputePasses=",
                  sample(g_counters.avg_compute_passes),
                  " avgComputeDispatches=",
                  sample(g_counters.avg_compute_dispatches),
                  " avgBlitPasses=", sample(g_counters.avg_blit_passes),
                  " avgFlushChunks=", sample(g_counters.avg_flush_chunks),
                  " avgFlushEncoders=",
                  sample(g_counters.avg_flush_encoders),
                  " avgEncodedEncoders=",
                  sample(g_counters.avg_encoded_encoders),
                  " avgFlushRender=", sample(g_counters.avg_flush_render),
                  " avgFlushCompute=", sample(g_counters.avg_flush_compute),
                  " avgFlushBlit=", sample(g_counters.avg_flush_blit),
                  " avgEncoderSwitches=",
                  sample(g_counters.avg_encoder_switches),
                  " avgFlushUs=", sample(g_counters.avg_flush_us),
                  " avgCommitUs=", sample(g_counters.avg_commit_us),
                  " avgDrawableBlockUs=",
                  sample(g_counters.avg_drawable_block_us),
                  " avgPresentLatencyUs=",
                  sample(g_counters.avg_present_latency_us)));
}

void maybeFlush(uint64_t frame) {
  flushCounters(frame, false);
}

} // namespace

bool enabled() {
  static const bool result = parseEnabledEnv("DXMT_PERF_STATS");
  return result;
}

ScopedFrameTimer::ScopedFrameTimer(FrameTimeBucket bucket) :
    bucket_(bucket),
    stats_(enabled() ? t_current_frame_stats : nullptr),
    begin_(enabled() ? dxmt::clock::now() : dxmt::clock::time_point{}),
    active_(begin_ != dxmt::clock::time_point{} && stats_) {}

ScopedFrameTimer::~ScopedFrameTimer() {
  stop();
}

void ScopedFrameTimer::stop() {
  if (!active_)
    return;
  const auto end = dxmt::clock::now();
  addDurationToBucket(*stats_, bucket_, end - begin_);
  active_ = false;
}

ScopedCodeTimer::ScopedCodeTimer(PerfCodePath path) :
    path_(path),
    stats_(enabled() ? t_current_frame_stats : nullptr),
    parent_(stats_ && t_active_code_timer &&
                    t_active_code_timer->stats_ == stats_
                ? t_active_code_timer
                : nullptr),
    begin_(stats_ ? dxmt::clock::now() : dxmt::clock::time_point{}),
    active_(stats_ && begin_ != dxmt::clock::time_point{}) {
  if (active_)
    t_active_code_timer = this;
}

ScopedCodeTimer::~ScopedCodeTimer() {
  stop();
}

void ScopedCodeTimer::stop() {
  if (!active_)
    return;

  const auto elapsed = dxmt::clock::now() - begin_;
  addExclusiveCodeTiming(stats_, path_, elapsed, child_interval_);
  if (parent_)
    parent_->child_interval_ += elapsed;
  t_active_code_timer = parent_;
  active_ = false;
}

ScopedFrameStatisticsBinding::ScopedFrameStatisticsBinding(
    FrameStatistics *stats) : previous_(t_current_frame_stats) {
  t_current_frame_stats = stats;
}

ScopedFrameStatisticsBinding::~ScopedFrameStatisticsBinding() {
  t_current_frame_stats = previous_;
}

void setCurrentFrameStatistics(FrameStatistics *stats) {
  if (!enabled())
    return;
  t_current_frame_stats = stats;
}

FrameStatistics *currentFrameStatistics() {
  return t_current_frame_stats;
}

void recordExecuteTime(FrameStatistics *stats, ExecuteTimeBucket bucket,
                       dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  addDurationToBucket(*stats, bucket, duration);
}

void recordReplayBreakdown(FrameStatistics *stats,
                           dxmt::clock::duration record_loop,
                           dxmt::clock::duration flush_pass,
                           dxmt::clock::duration timestamp_resolve,
                           dxmt::clock::duration cpu_query_resolve) {
  if (!enabled() || !stats)
    return;
  stats->frame_replay_record_loop_interval += record_loop;
  stats->frame_replay_flush_pass_interval += flush_pass;
  stats->frame_replay_timestamp_resolve_interval += timestamp_resolve;
  stats->frame_replay_cpu_query_resolve_interval += cpu_query_resolve;
}

void recordCompiledPassBuildTime(FrameStatistics *stats,
                                 dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_compiled_pass_build_interval += duration;
}

void recordCompiledDrawEncodeTime(FrameStatistics *stats,
                                  dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_compiled_draw_encode_interval += duration;
}

void recordCompiledDispatchEncodeTime(FrameStatistics *stats,
                                      dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_compiled_dispatch_encode_interval += duration;
}

void recordArgumentTableUpdateTime(FrameStatistics *stats,
                                   dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_argument_table_update_interval += duration;
}

void recordArgumentTableBindTime(FrameStatistics *stats,
                                 dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_argument_table_bind_interval += duration;
}

void recordResidencySubmitTime(FrameStatistics *stats,
                               dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_residency_submit_interval += duration;
}

void recordPsoCacheLookupTime(FrameStatistics *stats,
                              dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_pso_cache_lookup_interval += duration;
}

void recordPsoMaterializeTime(FrameStatistics *stats,
                              dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_pso_materialize_interval += duration;
}

void recordPsoCompileWaitTime(FrameStatistics *stats,
                              dxmt::clock::duration duration) {
  if (!enabled() || !stats)
    return;
  stats->frame_pso_compile_wait_interval += duration;
}

void recordCompiledGraphicsPackets(FrameStatistics *stats, uint64_t count) {
  if (!enabled() || !stats)
    return;
  stats->frame_compiled_graphics_packets += count;
}

void recordFallbackGraphicsPackets(FrameStatistics *stats, uint64_t count,
                                   CompiledFallbackReason reason) {
  if (!enabled() || !stats)
    return;
  stats->frame_fallback_graphics_packets += count;
  addCompiledFallbackReason(*stats, reason, count);
}

void recordCompiledComputePackets(FrameStatistics *stats, uint64_t count) {
  if (!enabled() || !stats)
    return;
  stats->frame_compiled_compute_packets += count;
}

void recordFallbackComputePackets(FrameStatistics *stats, uint64_t count,
                                  CompiledFallbackReason reason) {
  if (!enabled() || !stats)
    return;
  stats->frame_fallback_compute_packets += count;
  addCompiledFallbackReason(*stats, reason, count);
}

void recordStateRecordsElided(FrameStatistics *stats, uint64_t count) {
  if (!enabled() || !stats)
    return;
  stats->frame_state_records_elided += count;
}

void recordFrameBoundary(uint64_t frame) {
  if (!enabled())
    return;
  g_counters.frames.fetch_add(1, std::memory_order_relaxed);
  maybeFlush(frame);
}

void recordFrameBoundary(uint64_t frame, const FrameStatistics &frame_stats,
                         const FrameStatistics &average_stats,
                         uint64_t frame_wall_us) {
  if (!enabled())
    return;

  auto frame_stats_with_external = frame_stats;
  frame_stats_with_external.frame_descriptor_content_writes =
      g_counters.descriptor_content_writes.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_descriptor_content_write_cbv =
      g_counters.descriptor_content_write_cbv.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_descriptor_content_write_srv =
      g_counters.descriptor_content_write_srv.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_descriptor_content_write_uav =
      g_counters.descriptor_content_write_uav.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_descriptor_content_write_sampler =
      g_counters.descriptor_content_write_sampler.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_descriptor_content_write_copy =
      g_counters.descriptor_content_write_copy.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_descriptor_content_write_feedback_uav =
      g_counters.descriptor_content_write_feedback_uav.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_native_descriptor_buffer_records =
      g_counters.native_descriptor_buffer_records.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_native_descriptor_buffer_record_cbv =
      g_counters.native_descriptor_buffer_record_cbv.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_native_descriptor_buffer_record_srv =
      g_counters.native_descriptor_buffer_record_srv.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_native_descriptor_buffer_record_uav =
      g_counters.native_descriptor_buffer_record_uav.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_native_descriptor_buffer_record_counters =
      g_counters.native_descriptor_buffer_record_counters.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_native_descriptor_buffer_record_missing_resource =
      g_counters.native_descriptor_buffer_record_missing_resource.exchange(
          0, std::memory_order_relaxed);
  frame_stats_with_external.frame_native_descriptor_resource_table_entries =
      g_counters.native_descriptor_resource_table_entries.exchange(
          0, std::memory_order_relaxed);

  g_counters.frames.fetch_add(1, std::memory_order_relaxed);
  g_counters.frame_wall_us.fetch_add(frame_wall_us,
                                    std::memory_order_relaxed);
  updateMax(g_counters.frame_wall_max_us, frame_wall_us);

  const auto render_draws =
      uint64_t(frame_stats.render_draw_count) +
      frame_stats.render_indexed_draw_count +
      frame_stats.render_indirect_draw_count +
      frame_stats.render_mesh_draw_count +
      frame_stats.render_tile_dispatch_count;
  const auto compute_passes =
      uint64_t(frame_stats.compute_pass_with_dispatch_count) +
      frame_stats.compute_pass_without_dispatch_count;
  const auto encoder_switches =
      uint64_t(frame_stats.encoder_switch_render_compute_count) +
      frame_stats.encoder_switch_compute_render_count +
      frame_stats.encoder_switch_to_other_count;
  const auto flush_us = durationUs(frame_stats.flush_total_interval);
  const auto commit_us = durationUs(frame_stats.commit_interval);
  const auto execute_command_lists_us =
      durationUs(frame_stats.frame_execute_command_lists_interval);
  const auto present_us = durationUs(frame_stats.frame_present_interval);
  const auto present_queue_wait_us =
      durationUs(frame_stats.frame_present_queue_wait_interval);
  const auto present_work_us =
      present_us > present_queue_wait_us ? present_us - present_queue_wait_us
                                         : 0;
  const auto pre_present_wall_us =
      frame_wall_us > present_us ? frame_wall_us - present_us : 0;
  const auto queue_signal_us =
      durationUs(frame_stats.frame_queue_signal_interval);
  const auto queue_wait_us = durationUs(frame_stats.frame_queue_wait_interval);
  const auto create_resource_us =
      durationUs(frame_stats.frame_create_resource_interval);
  const auto create_reserved_resource_us =
      durationUs(frame_stats.frame_create_reserved_resource_interval);
  const auto create_heap_us = durationUs(frame_stats.frame_create_heap_interval);
  const auto create_pipeline_us =
      durationUs(frame_stats.frame_create_pipeline_interval);
  const auto tile_mapping_us =
      durationUs(frame_stats.frame_tile_mapping_interval);
  const auto cmdlist_record_us =
      durationUs(frame_stats.frame_cmdlist_record_interval);
  const auto resource_lock_us =
      durationUs(frame_stats.frame_resource_lock_interval);
  const auto staging_upload_us =
      durationUs(frame_stats.frame_staging_upload_interval);
  const auto chunk_commit_us =
      durationUs(frame_stats.frame_chunk_commit_interval);
  const auto draw_record_us =
      durationUs(frame_stats.frame_draw_record_interval);
  const auto draw_resolve_us =
      durationUs(frame_stats.frame_draw_resolve_interval);
  const auto draw_emit_us = durationUs(frame_stats.frame_draw_emit_interval);
  // Calling-thread frontend cost only. The encode-thread halves (resolve,
  // emit) run off the app's thread, so folding them in would double-count
  // against the frame wall.
  const auto frontend_known_us = resource_lock_us + staging_upload_us +
                                 chunk_commit_us + draw_record_us +
                                 create_resource_us + cmdlist_record_us;
  const auto present_latency_wait_us =
      durationUs(frame_stats.present_latency_interval);
  const auto drawable_blocking_us =
      durationUs(frame_stats.drawable_blocking_interval);
  const auto execute_validate_us =
      durationUs(frame_stats.frame_execute_validate_interval);
  const auto execute_collect_us =
      durationUs(frame_stats.frame_execute_collect_interval);
  const auto execute_enqueue_us =
      durationUs(frame_stats.frame_execute_enqueue_interval);
  const auto execute_drain_us =
      durationUs(frame_stats.frame_execute_drain_interval);
  const auto execute_drain_lock_us =
      durationUs(frame_stats.frame_execute_drain_lock_interval);
  const auto execute_resource_state_lock_hold_us = durationUs(
      frame_stats.frame_execute_resource_state_lock_hold_interval);
  const auto execute_replay_us =
      durationUs(frame_stats.frame_execute_replay_interval);
  const auto execute_decay_us =
      durationUs(frame_stats.frame_execute_decay_interval);
  const auto execute_signal_us =
      durationUs(frame_stats.frame_execute_signal_interval);
  const auto execute_commit_us =
      durationUs(frame_stats.frame_execute_commit_interval);
  const auto execute_wait_arm_us =
      durationUs(frame_stats.frame_execute_wait_arm_interval);
  const auto replay_record_loop_us =
      durationUs(frame_stats.frame_replay_record_loop_interval);
  const auto replay_flush_pass_us =
      durationUs(frame_stats.frame_replay_flush_pass_interval);
  const auto replay_timestamp_resolve_us =
      durationUs(frame_stats.frame_replay_timestamp_resolve_interval);
  const auto replay_cpu_query_resolve_us =
      durationUs(frame_stats.frame_replay_cpu_query_resolve_interval);
  const auto replay_timed_us =
      replay_record_loop_us + replay_flush_pass_us + replay_timestamp_resolve_us +
      replay_cpu_query_resolve_us;
  const auto replay_worker_summary = summarizeReplayWorkerFrame(frame_stats);
  const auto replay_get_pipeline_us =
      durationUs(frame_stats.frame_replay_get_pipeline_interval);
  const auto replay_get_metal_pso_us =
      durationUs(frame_stats.frame_replay_get_metal_pso_interval);
  const auto replay_select_pso_us =
      durationUs(frame_stats.frame_replay_select_pso_interval);
  const auto replay_desc_access_us =
      durationUs(frame_stats.frame_replay_desc_access_interval);
  const auto replay_state_update_us =
      durationUs(frame_stats.frame_replay_state_update_interval);
  const auto replay_attach_us =
      durationUs(frame_stats.frame_replay_attach_interval);
  const auto replay_bind_snapshot_us =
      durationUs(frame_stats.frame_replay_bind_snapshot_interval);
  const auto replay_estimate_us =
      durationUs(frame_stats.frame_replay_estimate_interval);
  const auto replay_packet_us =
      durationUs(frame_stats.frame_replay_packet_interval);
  const auto replay_queue_us =
      durationUs(frame_stats.frame_replay_queue_interval);
  const auto replay_emit_us =
      durationUs(frame_stats.frame_replay_emit_interval);
  const auto replay_flush_blit_us =
      durationUs(frame_stats.frame_replay_flush_blit_interval);
  const auto replay_flush_compute_us =
      durationUs(frame_stats.frame_replay_flush_compute_interval);
  const auto replay_flush_graphics_us =
      durationUs(frame_stats.frame_replay_flush_graphics_interval);
  const auto replay_flush_barrier_us =
      durationUs(frame_stats.frame_replay_flush_barrier_interval);
  const auto replay_build_plan_us =
      durationUs(frame_stats.frame_replay_build_plan_interval);
  const auto replay_emit_timestamp_us =
      durationUs(frame_stats.frame_replay_emit_timestamp_interval);
  const auto replay_record_draw_us =
      durationUs(frame_stats.frame_replay_record_draw_interval);
  const auto replay_record_draw_indexed_us =
      durationUs(frame_stats.frame_replay_record_draw_indexed_interval);
  const auto replay_record_dispatch_us =
      durationUs(frame_stats.frame_replay_record_dispatch_interval);
  const auto replay_record_pipeline_state_us =
      durationUs(frame_stats.frame_replay_record_pipeline_state_interval);
  const auto replay_record_descriptor_heaps_us =
      durationUs(frame_stats.frame_replay_record_descriptor_heaps_interval);
  const auto replay_record_root_signature_us =
      durationUs(frame_stats.frame_replay_record_root_signature_interval);
  const auto replay_record_root_table_us =
      durationUs(frame_stats.frame_replay_record_root_table_interval);
  const auto replay_record_root_descriptor_us =
      durationUs(frame_stats.frame_replay_record_root_descriptor_interval);
  const auto replay_record_root_constants_us =
      durationUs(frame_stats.frame_replay_record_root_constants_interval);
  const auto replay_record_vertex_index_state_us =
      durationUs(frame_stats.frame_replay_record_vertex_index_state_interval);
  const auto replay_record_render_targets_us =
      durationUs(frame_stats.frame_replay_record_render_targets_interval);
  const auto replay_record_resource_barrier_us =
      durationUs(frame_stats.frame_replay_record_resource_barrier_interval);
  const auto replay_record_copy_clear_resolve_us =
      durationUs(frame_stats.frame_replay_record_copy_clear_resolve_interval);
  const auto replay_record_query_us =
      durationUs(frame_stats.frame_replay_record_query_interval);
  const auto replay_record_execute_indirect_us =
      durationUs(frame_stats.frame_replay_record_execute_indirect_interval);
  const auto replay_record_temporal_upscale_us =
      durationUs(frame_stats.frame_replay_record_temporal_upscale_interval);
  const auto compiled_pass_build_us =
      durationUs(frame_stats.frame_compiled_pass_build_interval);
  const auto compiled_draw_encode_us =
      durationUs(frame_stats.frame_compiled_draw_encode_interval);
  const auto compiled_draw_direct_encode_us =
      durationUs(frame_stats.frame_compiled_draw_direct_encode_interval);
  const auto compiled_draw_replay_encode_us =
      durationUs(frame_stats.frame_compiled_draw_replay_encode_interval);
  const auto compiled_draw_common_us =
      durationUs(frame_stats.frame_compiled_draw_common_interval);
  const auto compiled_draw_retain_us =
      durationUs(frame_stats.frame_compiled_draw_retain_interval);
  const auto compiled_draw_pipeline_us =
      durationUs(frame_stats.frame_compiled_draw_pipeline_interval);
  const auto compiled_draw_dsso_us =
      durationUs(frame_stats.frame_compiled_draw_dsso_interval);
  const auto compiled_draw_rasterizer_us =
      durationUs(frame_stats.frame_compiled_draw_rasterizer_interval);
  const auto compiled_draw_binding_gate_us =
      durationUs(frame_stats.frame_compiled_draw_binding_gate_interval);
  const auto compiled_draw_binding_snapshot_us =
      durationUs(frame_stats.frame_compiled_draw_binding_snapshot_interval);
  const auto compiled_draw_dynamic_state_us =
      durationUs(frame_stats.frame_compiled_draw_dynamic_state_interval);
  const auto compiled_draw_visibility_us =
      durationUs(frame_stats.frame_compiled_draw_visibility_interval);
  const auto compiled_draw_body_us =
      durationUs(frame_stats.frame_compiled_draw_body_interval);
  const auto compiled_snapshot_static_samplers_us =
      durationUs(frame_stats.frame_compiled_snapshot_static_samplers_interval);
  const auto compiled_snapshot_entries_us =
      durationUs(frame_stats.frame_compiled_snapshot_entries_interval);
  const auto compiled_snapshot_root_constants_us =
      durationUs(frame_stats.frame_compiled_snapshot_root_constants_interval);
  const auto compiled_snapshot_descriptors_us =
      durationUs(frame_stats.frame_compiled_snapshot_descriptors_interval);
  const auto compiled_snapshot_clear_descriptors_us =
      durationUs(frame_stats.frame_compiled_snapshot_clear_descriptors_interval);
  const auto compiled_snapshot_bindless_fill_us =
      durationUs(frame_stats.frame_compiled_snapshot_bindless_fill_interval);
  const auto compiled_snapshot_vertex_buffers_us =
      durationUs(frame_stats.frame_compiled_snapshot_vertex_buffers_interval);
  const auto compiled_snapshot_vertex_table_us =
      durationUs(frame_stats.frame_compiled_snapshot_vertex_table_interval);
  const auto compiled_snapshot_restore_argbuf_us =
      durationUs(frame_stats.frame_compiled_snapshot_restore_argbuf_interval);
  const auto compiled_snapshot_shader_bindings_us =
      durationUs(frame_stats.frame_compiled_snapshot_shader_bindings_interval);
  const auto compiled_snapshot_bindless_shader_bindings_us =
      durationUs(
          frame_stats.frame_compiled_snapshot_bindless_shader_bindings_interval);
  const auto compiled_snapshot_legacy_shader_bindings_us =
      durationUs(
          frame_stats.frame_compiled_snapshot_legacy_shader_bindings_interval);
  const auto compiled_dispatch_encode_us =
      durationUs(frame_stats.frame_compiled_dispatch_encode_interval);
  const auto argument_table_update_us =
      durationUs(frame_stats.frame_argument_table_update_interval);
  const auto argument_table_bind_us =
      durationUs(frame_stats.frame_argument_table_bind_interval);
  const auto residency_submit_us =
      durationUs(frame_stats.frame_residency_submit_interval);
  const auto pso_cache_lookup_us =
      durationUs(frame_stats.frame_pso_cache_lookup_interval);
  const auto pso_materialize_us =
      durationUs(frame_stats.frame_pso_materialize_interval);
  const auto pso_compile_wait_us =
      durationUs(frame_stats.frame_pso_compile_wait_interval);
  const auto compiled_fallback_reason_count =
      [&](CompiledFallbackReason reason) -> uint64_t {
    const auto index = static_cast<size_t>(reason);
    return index < frame_stats.frame_compiled_fallback_reasons.size()
               ? frame_stats.frame_compiled_fallback_reasons[index]
               : 0;
  };
  const auto execute_known_us =
      execute_validate_us + execute_collect_us + execute_enqueue_us +
      execute_drain_us;
  const auto flush_collect_us = durationUs(frame_stats.flush_collect_interval);
  const auto flush_relation_us =
      durationUs(frame_stats.flush_relation_interval);
  const auto flush_query_us = durationUs(frame_stats.flush_query_interval);
  const auto flush_timestamp_us =
      durationUs(frame_stats.flush_timestamp_interval);
  const auto flush_render_us = durationUs(frame_stats.flush_render_interval);
  const auto flush_compute_us = durationUs(frame_stats.flush_compute_interval);
  const auto flush_blit_us = durationUs(frame_stats.flush_blit_interval);
  const auto flush_clear_us = durationUs(frame_stats.flush_clear_interval);
  const auto flush_resolve_us = durationUs(frame_stats.flush_resolve_interval);
  const auto flush_scaler_us = durationUs(frame_stats.flush_scaler_interval);
  const auto flush_present_us = durationUs(frame_stats.flush_present_interval);
  const auto flush_event_us = durationUs(frame_stats.flush_event_interval);
  const auto flush_sample_us = durationUs(frame_stats.flush_sample_interval);
  const auto flush_pending_fence_us =
      durationUs(frame_stats.flush_pending_fence_interval);
  const auto flush_signal_us = durationUs(frame_stats.flush_signal_interval);
  const auto flush_cleanup_us = durationUs(frame_stats.flush_cleanup_interval);
  const auto flush_known_us =
      flush_collect_us + flush_relation_us + flush_query_us +
      flush_timestamp_us + flush_render_us + flush_compute_us +
      flush_blit_us + flush_clear_us + flush_resolve_us + flush_scaler_us +
      flush_present_us + flush_event_us + flush_sample_us +
      flush_pending_fence_us + flush_signal_us + flush_cleanup_us;
  const auto monotonic_us = durationUs(dxmt::clock::now() - g_perf_start_time);
  const auto previous_monotonic_us =
      g_last_frame_log_monotonic_us.exchange(monotonic_us,
                                             std::memory_order_relaxed);
  const auto log_delta_us = previous_monotonic_us
                                ? monotonic_us - previous_monotonic_us
                                : 0;

  g_counters.frame_command_buffers.fetch_add(
      frame_stats.command_buffer_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_command_buffers_max,
            frame_stats.command_buffer_count);
  g_counters.frame_render_passes.fetch_add(frame_stats.render_pass_count,
                                          std::memory_order_relaxed);
  updateMax(g_counters.frame_render_passes_max,
            frame_stats.render_pass_count);
  g_counters.frame_render_commands.fetch_add(frame_stats.render_command_count,
                                            std::memory_order_relaxed);
  updateMax(g_counters.frame_render_commands_max,
            frame_stats.render_command_count);
  g_counters.frame_render_draws.fetch_add(render_draws,
                                         std::memory_order_relaxed);
  updateMax(g_counters.frame_render_draws_max, render_draws);
  g_counters.frame_render_indexed.fetch_add(
      frame_stats.render_indexed_draw_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_render_indexed_max,
            frame_stats.render_indexed_draw_count);
  g_counters.frame_render_pso_binds.fetch_add(
      frame_stats.render_pso_bind_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_render_pso_binds_max,
            frame_stats.render_pso_bind_count);
  g_counters.frame_compute_passes.fetch_add(compute_passes,
                                           std::memory_order_relaxed);
  updateMax(g_counters.frame_compute_passes_max, compute_passes);
  g_counters.frame_compute_commands.fetch_add(
      frame_stats.compute_command_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_compute_commands_max,
            frame_stats.compute_command_count);
  g_counters.frame_compute_dispatches.fetch_add(
      frame_stats.compute_dispatch_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_compute_dispatches_max,
            frame_stats.compute_dispatch_count);
  g_counters.frame_blit_passes.fetch_add(frame_stats.blit_pass_count,
                                        std::memory_order_relaxed);
  updateMax(g_counters.frame_blit_passes_max, frame_stats.blit_pass_count);
  g_counters.frame_blit_empty_passes.fetch_add(
      frame_stats.blit_pass_empty_count, std::memory_order_relaxed);
  g_counters.frame_blit_deferred_fence_only_passes.fetch_add(
      frame_stats.blit_pass_deferred_fence_only_count,
      std::memory_order_relaxed);
  g_counters.frame_blit_merged_fence_only_passes.fetch_add(
      frame_stats.blit_pass_merged_fence_only_count,
      std::memory_order_relaxed);
  g_counters.frame_blit_barrier_only_passes.fetch_add(
      frame_stats.blit_barrier_only_pass_count, std::memory_order_relaxed);
  g_counters.frame_blit_separator_passes.fetch_add(
      frame_stats.blit_separator_pass_count, std::memory_order_relaxed);
  g_counters.frame_resource_barrier_batches_merged.fetch_add(
      frame_stats.resource_barrier_batches_merged, std::memory_order_relaxed);
  g_counters.frame_resource_barrier_batches_graphics_inlined.fetch_add(
      frame_stats.resource_barrier_batches_graphics_inlined,
      std::memory_order_relaxed);
  g_counters.frame_resource_barrier_batches_compute_inlined.fetch_add(
      frame_stats.resource_barrier_batches_compute_inlined,
      std::memory_order_relaxed);
  g_counters.frame_flush_chunks.fetch_add(frame_stats.flush_chunk_count,
                                         std::memory_order_relaxed);
  updateMax(g_counters.frame_flush_chunks_max,
            frame_stats.flush_chunk_count);
  g_counters.frame_flush_encoders.fetch_add(frame_stats.flush_encoder_count,
                                           std::memory_order_relaxed);
  updateMax(g_counters.frame_flush_encoders_max,
            frame_stats.flush_encoder_count);
  g_counters.frame_encoded_encoders.fetch_add(
      frame_stats.flush_encoded_encoder_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_encoded_encoders_max,
            frame_stats.flush_encoded_encoder_count);
  g_counters.frame_flush_render.fetch_add(
      frame_stats.flush_render_encoder_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_flush_render_max,
            frame_stats.flush_render_encoder_count);
  g_counters.frame_flush_compute.fetch_add(
      frame_stats.flush_compute_encoder_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_flush_compute_max,
            frame_stats.flush_compute_encoder_count);
  g_counters.frame_flush_blit.fetch_add(
      frame_stats.flush_blit_encoder_count, std::memory_order_relaxed);
  updateMax(g_counters.frame_flush_blit_max,
            frame_stats.flush_blit_encoder_count);
  g_counters.frame_encoder_switches.fetch_add(encoder_switches,
                                             std::memory_order_relaxed);
  updateMax(g_counters.frame_encoder_switches_max, encoder_switches);
  g_counters.frame_flush_us.fetch_add(flush_us, std::memory_order_relaxed);
  updateMax(g_counters.frame_flush_max_us, flush_us);
  g_counters.frame_commit_us.fetch_add(commit_us, std::memory_order_relaxed);
  updateMax(g_counters.frame_commit_max_us, commit_us);

  g_counters.avg_command_buffers.store(average_stats.command_buffer_count,
                                       std::memory_order_relaxed);
  g_counters.avg_render_passes.store(average_stats.render_pass_count,
                                     std::memory_order_relaxed);
  g_counters.avg_render_draws.store(
      uint64_t(average_stats.render_draw_count) +
          average_stats.render_indexed_draw_count +
          average_stats.render_indirect_draw_count +
          average_stats.render_mesh_draw_count +
          average_stats.render_tile_dispatch_count,
      std::memory_order_relaxed);
  g_counters.avg_render_indexed.store(
      average_stats.render_indexed_draw_count, std::memory_order_relaxed);
  g_counters.avg_compute_passes.store(
      uint64_t(average_stats.compute_pass_with_dispatch_count) +
          average_stats.compute_pass_without_dispatch_count,
      std::memory_order_relaxed);
  g_counters.avg_compute_dispatches.store(
      average_stats.compute_dispatch_count, std::memory_order_relaxed);
  g_counters.avg_blit_passes.store(average_stats.blit_pass_count,
                                   std::memory_order_relaxed);
  g_counters.avg_flush_chunks.store(average_stats.flush_chunk_count,
                                    std::memory_order_relaxed);
  g_counters.avg_flush_encoders.store(average_stats.flush_encoder_count,
                                      std::memory_order_relaxed);
  g_counters.avg_encoded_encoders.store(
      average_stats.flush_encoded_encoder_count, std::memory_order_relaxed);
  g_counters.avg_flush_render.store(average_stats.flush_render_encoder_count,
                                    std::memory_order_relaxed);
  g_counters.avg_flush_compute.store(average_stats.flush_compute_encoder_count,
                                     std::memory_order_relaxed);
  g_counters.avg_flush_blit.store(average_stats.flush_blit_encoder_count,
                                  std::memory_order_relaxed);
  g_counters.avg_encoder_switches.store(
      uint64_t(average_stats.encoder_switch_render_compute_count) +
          average_stats.encoder_switch_compute_render_count +
          average_stats.encoder_switch_to_other_count,
      std::memory_order_relaxed);
  g_counters.avg_flush_us.store(durationUs(average_stats.flush_total_interval),
                                std::memory_order_relaxed);
  g_counters.avg_commit_us.store(durationUs(average_stats.commit_interval),
                                 std::memory_order_relaxed);
  g_counters.avg_frame_wall_us.store(
      durationUs(average_stats.frame_wall_interval), std::memory_order_relaxed);
  g_counters.avg_drawable_block_us.store(
      durationUs(average_stats.drawable_blocking_interval),
      std::memory_order_relaxed);
  g_counters.avg_present_latency_us.store(
      durationUs(average_stats.present_latency_interval),
      std::memory_order_relaxed);

  Logger::logFileOnly(
      LogLevel::Info,
      str::format("DXMT perf frame: buildVersion=", DXMT_VERSION,
                  " frame=",
                  frame,
                  " monoUs=", monotonic_us,
                  " logDeltaUs=", log_delta_us,
                  " totalWallUs=", frame_wall_us,
                  " executeCommandListsUs=", execute_command_lists_us,
                  " executeKnownUs=", execute_known_us,
                  " executeValidateUs=", execute_validate_us,
                  " executeCollectUs=", execute_collect_us,
                  " executeEnqueueUs=", execute_enqueue_us,
                  " executeDrainUs=", execute_drain_us,
                  " executeDrainLockUs=", execute_drain_lock_us,
                  " executeResourceStateLockHoldUs=",
                  execute_resource_state_lock_hold_us,
                  " executeReplayUs=", execute_replay_us,
                  " executeDecayUs=", execute_decay_us,
                  " executeSignalUs=", execute_signal_us,
                  " executeCommitUs=", execute_commit_us,
                  " executeWaitArmUs=", execute_wait_arm_us,
                  " replayRecordLoopUs=", replay_record_loop_us,
                  " replayFlushPassUs=", replay_flush_pass_us,
                  " replayTimestampResolveUs=", replay_timestamp_resolve_us,
                  " replayCpuQueryResolveUs=", replay_cpu_query_resolve_us,
                  " replayTimedUs=", replay_timed_us,
                  " replaySupersededMaskUs=",
                  replay_worker_summary.superseded_mask_us,
                  " replayCompiledGraphicsUs=",
                  replay_worker_summary.compiled_graphics_us,
                  " replayCompiledGraphicsPackets=",
                  frame_stats.frame_replay_compiled_graphics_packet_count,
                  " replayCompiledComputeUs=",
                  replay_worker_summary.compiled_compute_us,
                  " replayCompiledComputePackets=",
                  frame_stats.frame_replay_compiled_compute_packet_count,
                  " replayFallbackClassificationUs=",
                  replay_worker_summary.fallback_classification_us,
                  " replayFallbackClassificationRanges=",
                  frame_stats.frame_replay_fallback_classification_count,
                  " replayFallbackRecordUs=",
                  replay_worker_summary.typed_record_us,
                  " nativeCommandBodyUs=",
                  replay_worker_summary.native_command_body_us,
                  " replayRecordControlUs=",
                  replay_worker_summary.record_control_us,
                  " replayClassifiedRecordUs=",
                  replay_worker_summary.classified_record_us,
                  " replayRecordCoveragePermille=",
                  replay_worker_summary.record_coverage_permille,
                  " replayGetPipelineUs=", replay_get_pipeline_us,
                  " replayGetMetalPsoUs=", replay_get_metal_pso_us,
                  " replaySelectPsoUs=", replay_select_pso_us,
                  " replayDescAccessUs=", replay_desc_access_us,
                  " replayStateUpdateUs=", replay_state_update_us,
                  " replayAttachUs=", replay_attach_us,
                  " replayBindSnapshotUs=", replay_bind_snapshot_us,
                  " replayEstimateUs=", replay_estimate_us,
                  " replayPacketUs=", replay_packet_us,
                  " replayQueueUs=", replay_queue_us,
                  " replayEmitUs=", replay_emit_us,
                  " replayFlushBlitUs=", replay_flush_blit_us,
                  " replayFlushComputeUs=", replay_flush_compute_us,
                  " replayFlushGraphicsUs=", replay_flush_graphics_us,
                  " replayFlushBarrierUs=", replay_flush_barrier_us,
                  " replayBuildPlanUs=", replay_build_plan_us,
                  " replayEmitTimestampUs=", replay_emit_timestamp_us,
                  " replayRecordDrawUs=", replay_record_draw_us,
                  " replayRecordDrawCount=",
                  frame_stats.frame_replay_record_draw_count,
                  " replayRecordDrawIndexedUs=", replay_record_draw_indexed_us,
                  " replayRecordDrawIndexedCount=",
                  frame_stats.frame_replay_record_draw_indexed_count,
                  " replayRecordDispatchUs=", replay_record_dispatch_us,
                  " replayRecordDispatchCount=",
                  frame_stats.frame_replay_record_dispatch_count,
                  " replayRecordPipelineStateUs=",
                  replay_record_pipeline_state_us,
                  " replayRecordPipelineStateCount=",
                  frame_stats.frame_replay_record_pipeline_state_count,
                  " replayRecordDescriptorHeapsUs=",
                  replay_record_descriptor_heaps_us,
                  " replayRecordDescriptorHeapsCount=",
                  frame_stats.frame_replay_record_descriptor_heaps_count,
                  " replayRecordRootSignatureUs=",
                  replay_record_root_signature_us,
                  " replayRecordRootSignatureCount=",
                  frame_stats.frame_replay_record_root_signature_count,
                  " replayRecordRootTableUs=", replay_record_root_table_us,
                  " replayRecordRootTableCount=",
                  frame_stats.frame_replay_record_root_table_count,
                  " replayRecordRootDescriptorUs=",
                  replay_record_root_descriptor_us,
                  " replayRecordRootDescriptorCount=",
                  frame_stats.frame_replay_record_root_descriptor_count,
                  " replayRecordRootConstantsUs=",
                  replay_record_root_constants_us,
                  " replayRecordRootConstantsCount=",
                  frame_stats.frame_replay_record_root_constants_count,
                  " replayRecordVertexIndexStateUs=",
                  replay_record_vertex_index_state_us,
                  " replayRecordVertexIndexStateCount=",
                  frame_stats.frame_replay_record_vertex_index_state_count,
                  " replayRecordRenderTargetsUs=",
                  replay_record_render_targets_us,
                  " replayRecordRenderTargetsCount=",
                  frame_stats.frame_replay_record_render_targets_count,
                  " replayRecordResourceBarrierUs=",
                  replay_record_resource_barrier_us,
                  " replayRecordResourceBarrierCount=",
                  frame_stats.frame_replay_record_resource_barrier_count,
                  " replayRecordCopyClearResolveUs=",
                  replay_record_copy_clear_resolve_us,
                  " replayRecordCopyClearResolveCount=",
                  frame_stats.frame_replay_record_copy_clear_resolve_count,
                  " replayRecordQueryUs=", replay_record_query_us,
                  " replayRecordQueryCount=",
                  frame_stats.frame_replay_record_query_count,
                  " replayRecordExecuteIndirectUs=",
                  replay_record_execute_indirect_us,
                  " replayRecordExecuteIndirectCount=",
                  frame_stats.frame_replay_record_execute_indirect_count,
                  " replayRecordTemporalUpscaleUs=",
                  replay_record_temporal_upscale_us,
                  " replayRecordTemporalUpscaleCount=",
                  frame_stats.frame_replay_record_temporal_upscale_count,
                  " compiledPassBuildUs=", compiled_pass_build_us,
                  " compiledDrawEncodeUs=", compiled_draw_encode_us,
                  " compiledDrawDirectEncodeUs=",
                  compiled_draw_direct_encode_us,
                  " compiledDrawReplayEncodeUs=",
                  compiled_draw_replay_encode_us,
                  " compiledDrawCommonUs=", compiled_draw_common_us,
                  " compiledDrawRetainUs=", compiled_draw_retain_us,
                  " compiledDrawPipelineUs=", compiled_draw_pipeline_us,
                  " compiledDrawDssoUs=", compiled_draw_dsso_us,
                  " compiledDrawRasterizerUs=", compiled_draw_rasterizer_us,
                  " compiledDrawBindingGateUs=",
                  compiled_draw_binding_gate_us,
                  " compiledDrawBindingSnapshotUs=",
                  compiled_draw_binding_snapshot_us,
                  " compiledDrawDynamicStateUs=",
                  compiled_draw_dynamic_state_us,
                  " compiledDrawVisibilityUs=", compiled_draw_visibility_us,
                  " compiledDrawBodyUs=", compiled_draw_body_us,
                  " compiledSnapshotStaticSamplersUs=",
                  compiled_snapshot_static_samplers_us,
                  " compiledSnapshotEntriesUs=",
                  compiled_snapshot_entries_us,
                  " compiledSnapshotRootConstantsUs=",
                  compiled_snapshot_root_constants_us,
                  " compiledSnapshotDescriptorsUs=",
                  compiled_snapshot_descriptors_us,
                  " compiledSnapshotClearDescriptorsUs=",
                  compiled_snapshot_clear_descriptors_us,
                  " compiledSnapshotBindlessFillUs=",
                  compiled_snapshot_bindless_fill_us,
                  " compiledSnapshotVertexBuffersUs=",
                  compiled_snapshot_vertex_buffers_us,
                  " compiledSnapshotVertexTableUs=",
                  compiled_snapshot_vertex_table_us,
                  " compiledSnapshotRestoreArgbufUs=",
                  compiled_snapshot_restore_argbuf_us,
                  " compiledSnapshotShaderBindingsUs=",
                  compiled_snapshot_shader_bindings_us,
                  " compiledSnapshotBindlessShaderBindingsUs=",
                  compiled_snapshot_bindless_shader_bindings_us,
                  " compiledSnapshotLegacyShaderBindingsUs=",
                  compiled_snapshot_legacy_shader_bindings_us,
                  " compiledDispatchEncodeUs=",
                  compiled_dispatch_encode_us,
                  " argumentTableUpdateUs=", argument_table_update_us,
                  " argumentTableBindUs=", argument_table_bind_us,
                  " residencySubmitUs=", residency_submit_us,
                  " psoCacheLookupUs=", pso_cache_lookup_us,
                  " psoMaterializeUs=", pso_materialize_us,
                  " psoCompileWaitUs=", pso_compile_wait_us,
                  " compiledGraphicsPackets=",
                  frame_stats.frame_compiled_graphics_packets,
                  " compiledDrawDirectPackets=",
                  frame_stats.frame_compiled_draw_direct_packets,
                  " compiledDrawReplayPackets=",
                  frame_stats.frame_compiled_draw_replay_packets,
                  " compiledDrawIndexedPackets=",
                  frame_stats.frame_compiled_draw_indexed_packets,
                  " compiledDrawNonIndexedPackets=",
                  frame_stats.frame_compiled_draw_nonindexed_packets,
                  " compiledDrawSnapshotApplied=",
                  frame_stats.frame_compiled_draw_binding_snapshot_applied,
                  " compiledDrawSnapshotSkipped=",
                  frame_stats.frame_compiled_draw_binding_snapshot_skipped,
                  " compiledDrawBindingGenerationHits=",
                  frame_stats.frame_compiled_draw_binding_generation_hits,
                  " compiledDrawBindingFingerprintHits=",
                  frame_stats.frame_compiled_draw_binding_fingerprint_hits,
                  " compiledDrawBindingMisses=",
                  frame_stats.frame_compiled_draw_binding_misses,
                  " compiledSnapshotEntries=",
                  frame_stats.frame_compiled_snapshot_entries,
                  " compiledSnapshotRootConstants=",
                  frame_stats.frame_compiled_snapshot_root_constants,
                  " compiledSnapshotDescriptors=",
                  frame_stats.frame_compiled_snapshot_descriptors,
                  " compiledSnapshotClearDescriptors=",
                  frame_stats.frame_compiled_snapshot_clear_descriptors,
                  " compiledSnapshotBindlessFills=",
                  frame_stats.frame_compiled_snapshot_bindless_fills,
                  " compiledSnapshotBindlessFillTexture=",
                  frame_stats.frame_compiled_snapshot_bindless_fill_texture,
                  " compiledSnapshotBindlessFillSampler=",
                  frame_stats.frame_compiled_snapshot_bindless_fill_sampler,
                  " compiledSnapshotBindlessFillTextureBuffer=",
                  frame_stats
                      .frame_compiled_snapshot_bindless_fill_texture_buffer,
                  " compiledSnapshotBindlessFillNull=",
                  frame_stats.frame_compiled_snapshot_bindless_fill_null,
                  " compiledSnapshotVertexBuffers=",
                  frame_stats.frame_compiled_snapshot_vertex_buffers,
                  " compiledSnapshotShaderBindings=",
                  frame_stats.frame_compiled_snapshot_shader_bindings,
                  " compiledSnapshotBindlessShaderBindings=",
                  frame_stats.frame_compiled_snapshot_bindless_shader_bindings,
                  " compiledSnapshotLegacyShaderBindings=",
                  frame_stats.frame_compiled_snapshot_legacy_shader_bindings,
                  " compiledSnapshotLegacyArgbufRestores=",
                  frame_stats.frame_compiled_snapshot_legacy_argbuf_restores,
                  " fallbackGraphicsPackets=",
                  frame_stats.frame_fallback_graphics_packets,
                  " compiledComputePackets=",
                  frame_stats.frame_compiled_compute_packets,
                  " fallbackComputePackets=",
                  frame_stats.frame_fallback_compute_packets,
                  " stateRecordsElided=",
                  frame_stats.frame_state_records_elided,
                  " compiledFallbackReasonLegacyPath=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::LegacyPath),
                  " compiledFallbackReasonResourceBarrier=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::ResourceBarrier),
                  " compiledFallbackReasonGeometryOrTessellation=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::GeometryOrTessellation),
                  " compiledFallbackReasonIndirect=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::Indirect),
                  " compiledFallbackReasonMissingCompiledEncoder=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::MissingCompiledEncoder),
                  " compiledFallbackReasonUnsupportedPipeline=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedPipeline),
                  " compiledFallbackReasonUnsupportedRootSignature=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedRootSignature),
                  " compiledFallbackReasonUnsupportedDescriptorTable=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedDescriptorTable),
                  " compiledFallbackReasonUnsupportedRootDescriptor=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedRootDescriptor),
                  " compiledFallbackReasonUnsupportedRootConstants=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedRootConstants),
                  " compiledFallbackReasonUnsupportedVertexIndexState=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedVertexIndexState),
                  " compiledFallbackReasonUnsupportedRenderTargetState=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedRenderTargetState),
                  " compiledFallbackReasonUnsupportedResourceAccess=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedResourceAccess),
                  " compiledFallbackReasonUnsupportedArgumentTable=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::UnsupportedArgumentTable),
                  " compiledFallbackReasonResidency=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::Residency),
                  " compiledFallbackReasonNativeUnsupportedRootSignature=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeUnsupportedRootSignature),
                  " compiledFallbackReasonNativeUnsupportedDescriptorRange=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeUnsupportedDescriptorRange),
                  " compiledFallbackReasonNativeDescriptorNullBase=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeDescriptorNullBase),
                  " compiledFallbackReasonNativeDescriptorMixedHeap=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeDescriptorMixedHeap),
                  " compiledFallbackReasonNativeDescriptorNoRange=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeDescriptorNoRange),
                  " compiledFallbackReasonNativeDescriptorInvalidHandle=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeDescriptorInvalidHandle),
                  " compiledFallbackReasonNativeDescriptorHeapTail=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeDescriptorHeapTail),
                  " compiledFallbackReasonNativeDescriptorAmbiguousRange=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeDescriptorAmbiguousRange),
                  " compiledFallbackReasonNativeDescriptorBackendGeneration=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeDescriptorBackendGeneration),
                  " compiledFallbackReasonNativeUnsupportedRootDescriptor=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeUnsupportedRootDescriptor),
                  " compiledFallbackReasonNativeUnsupportedGeometryPipeline=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeUnsupportedGeometryPipeline),
                  " compiledFallbackReasonNativeUnsupportedTessellationPipeline=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeUnsupportedTessellationPipeline),
                  " compiledFallbackReasonNativeUnsupportedExecuteIndirect=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeUnsupportedExecuteIndirect),
                  " compiledFallbackReasonNativeUnsupportedDynamicResource=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeUnsupportedDynamicResource),
                  " compiledFallbackReasonNativeMissingDescriptorBackend=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeMissingDescriptorBackend),
                  " compiledFallbackReasonNativeShaderAbiMismatch=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeShaderAbiMismatch),
                  " compiledFallbackReasonNativeResidencyUnsupported=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::NativeResidencyUnsupported),
                  " compiledFallbackReasonUnknown=",
                  compiled_fallback_reason_count(
                      CompiledFallbackReason::Unknown),
                  " replayDrawCount=", frame_stats.frame_replay_draw_count,
                  " replayPsoRootUnchanged=",
                  frame_stats.frame_replay_pso_root_unchanged,
                  " replayFullBindUnchanged=",
                  frame_stats.frame_replay_full_bind_unchanged,
                  " replayFlushBlitCalls=",
                  frame_stats.frame_replay_flush_blit_count,
                  " replayFlushComputeCalls=",
                  frame_stats.frame_replay_flush_compute_count,
                  " replayFlushGraphicsCalls=",
                  frame_stats.frame_replay_flush_graphics_count,
                  " replayFlushBarrierCalls=",
                  frame_stats.frame_replay_flush_barrier_count,
                  " replayEmitTimestampCalls=",
                  frame_stats.frame_replay_emit_timestamp_count,
                  " replayFlushPassBatchesCalls=",
                  frame_stats.frame_replay_flush_pass_batches_count,
                  " replayFlushPassBatchesEmpty=",
                  frame_stats.frame_replay_flush_pass_batches_empty,
                  " replayResourceAccessCalls=",
                  frame_stats.frame_replay_resource_access_count,
                  " replayResourceAccessSteadyNoop=",
                  frame_stats.frame_replay_resource_access_steady_noop,
                  " replayDescAccessHits=",
                  frame_stats.frame_replay_desc_access_hit,
                  " replayDescAccessMiss=",
                  frame_stats.frame_replay_desc_access_miss,
                  " replayDescAccessPassthrough=",
                  frame_stats.frame_replay_desc_access_passthrough,
                  " replaySupersededStateRecordsSkipped=",
                  frame_stats.frame_replay_superseded_state_records_skipped,
                  " replayCompiledGraphicsCandidates=",
                  frame_stats.frame_replay_compiled_graphics_candidates,
                  " replayCompiledGraphicsLegacy=",
                  frame_stats.frame_replay_compiled_graphics_legacy,
                  " replayCompiledGraphicsBarriers=",
                  frame_stats.frame_replay_compiled_graphics_barriers,
                  " replayCompiledGraphicsGsTs=",
                  frame_stats.frame_replay_compiled_graphics_gs_ts,
                  " replayCompiledGraphicsIndirect=",
                  frame_stats.frame_replay_compiled_graphics_indirect,
                  " replayMismatchBarriers=",
                  frame_stats.frame_replay_mismatch_barriers,
                  " replayAppBarrierTransitions=",
                  frame_stats.frame_replay_app_barrier_transitions,
                  " replayBindingGenBumps=",
                  frame_stats.frame_replay_binding_gen_bumps,
                  " replayBindingGenPipeline=",
                  frame_stats.frame_replay_binding_gen_pipeline,
                  " replayBindingGenDescriptorHeaps=",
                  frame_stats.frame_replay_binding_gen_descriptor_heaps,
                  " replayBindingGenVertexBuffers=",
                  frame_stats.frame_replay_binding_gen_vertex_buffers,
                  " replayBindingGenRootSignature=",
                  frame_stats.frame_replay_binding_gen_root_signature,
                  " replayBindingGenRootDescriptorTable=",
                  frame_stats.frame_replay_binding_gen_root_descriptor_table,
                  " replayBindingGenRootDescriptor=",
                  frame_stats.frame_replay_binding_gen_root_descriptor,
                  " replayBindingGenRootConstants=",
                  frame_stats.frame_replay_binding_gen_root_constants,
                  " replayBindingGenIndirectVertexBuffer=",
                  frame_stats.frame_replay_binding_gen_indirect_vertex_buffer,
                  " replayBindingGenIndirectRootConstants=",
                  frame_stats
                      .frame_replay_binding_gen_indirect_root_constants,
                  " replayBindingGenIndirectRootDescriptor=",
                  frame_stats
                      .frame_replay_binding_gen_indirect_root_descriptor,
                  " replaySnapshotRequests=",
                  frame_stats.frame_replay_snapshot_requests,
                  " replaySnapshotCacheHits=",
                  frame_stats.frame_replay_snapshot_cache_hits,
                  " replaySnapshotCacheMisses=",
                  frame_stats.frame_replay_snapshot_cache_misses,
                  " replaySnapshotPassthrough=",
                  frame_stats.frame_replay_snapshot_passthrough,
                  " replaySnapshotGraphicsGenChanges=",
                  frame_stats.frame_replay_snapshot_graphics_gen_changes,
                  " replaySnapshotDescriptorGenChanges=",
                  frame_stats.frame_replay_snapshot_descriptor_gen_changes,
                  " replaySnapshotBothGenChanges=",
                  frame_stats.frame_replay_snapshot_both_gen_changes,
                  " replaySnapshotNoGenChanges=",
                  frame_stats.frame_replay_snapshot_no_gen_changes,
                  " replaySnapshotCapturedEntries=",
                  frame_stats.frame_replay_snapshot_captured_entries,
                  " replaySnapshotCapturedDescriptors=",
                  frame_stats.frame_replay_snapshot_captured_descriptors,
                  " replaySnapshotCapturedMissingDescriptors=",
                  frame_stats
                      .frame_replay_snapshot_captured_missing_descriptors,
                  " replaySnapshotCapturedRootDescriptors=",
                  frame_stats.frame_replay_snapshot_captured_root_descriptors,
                  " replaySnapshotCapturedRootConstants=",
                  frame_stats.frame_replay_snapshot_captured_root_constants,
                  " replaySnapshotCapturedVertexBuffers=",
                  frame_stats.frame_replay_snapshot_captured_vertex_buffers,
                  " replaySnapshotCapturedBindless=",
                  frame_stats.frame_replay_snapshot_captured_bindless,
                  " nativeDescriptorRootTables=",
                  frame_stats.frame_native_descriptor_root_tables,
                  " nativeDescriptorRootTableBackendReady=",
                  frame_stats.frame_native_descriptor_root_table_backend_ready,
                  " nativeDescriptorRecordStorageReady=",
                  frame_stats.frame_native_descriptor_record_storage_ready,
                  " nativeDescriptorResourceRootTables=",
                  frame_stats.frame_native_descriptor_resource_root_tables,
                  " nativeDescriptorSamplerRootTables=",
                  frame_stats.frame_native_descriptor_sampler_root_tables,
                  " nativeDescriptorBufferRecords=",
                  frame_stats_with_external
                      .frame_native_descriptor_buffer_records,
                  " nativeDescriptorBufferRecordCBV=",
                  frame_stats_with_external
                      .frame_native_descriptor_buffer_record_cbv,
                  " nativeDescriptorBufferRecordSRV=",
                  frame_stats_with_external
                      .frame_native_descriptor_buffer_record_srv,
                  " nativeDescriptorBufferRecordUAV=",
                  frame_stats_with_external
                      .frame_native_descriptor_buffer_record_uav,
                  " nativeDescriptorBufferRecordCounters=",
                  frame_stats_with_external
                      .frame_native_descriptor_buffer_record_counters,
                  " nativeDescriptorBufferRecordMissingResource=",
                  frame_stats_with_external
                      .frame_native_descriptor_buffer_record_missing_resource,
                  " nativeDescriptorResourceTableEntries=",
                  frame_stats_with_external
                      .frame_native_descriptor_resource_table_entries,
                  " genericDescriptorSpanLookups=",
                  frame_stats.frame_generic_descriptor_span_lookups,
                  " genericDescriptorSpanUnique=",
                  frame_stats.frame_generic_descriptor_span_unique,
                  " genericDescriptorSpanReuses=",
                  frame_stats.frame_generic_descriptor_span_reuses,
                  " frozenNativeDirectPackets=",
                  frame_stats.frame_frozen_native_direct_packets,
                  " frozenRangeLookups=",
                  frame_stats.frame_frozen_range_lookups,
                  " frozenRangeUnique=",
                  frame_stats.frame_frozen_range_unique,
                  " frozenRangeReuses=",
                  frame_stats.frame_frozen_range_reuses,
                  " frozenRootWordLookups=",
                  frame_stats.frame_frozen_root_word_lookups,
                  " frozenRootWordReuses=",
                  frame_stats.frame_frozen_root_word_reuses,
                  " submissionGraphicsEncoderBoundaryMerges=",
                  frame_stats
                      .frame_submission_graphics_encoder_boundary_merges,
                  " submissionComputeEncoderBoundaryMerges=",
                  frame_stats
                      .frame_submission_compute_encoder_boundary_merges,
                  " submissionEncoderBoundaryFlushes=",
                  frame_stats.frame_submission_encoder_boundary_flushes,
                  " descriptorContentWrites=",
                  frame_stats_with_external.frame_descriptor_content_writes,
                  " descriptorContentWriteCBV=",
                  frame_stats_with_external.frame_descriptor_content_write_cbv,
                  " descriptorContentWriteSRV=",
                  frame_stats_with_external.frame_descriptor_content_write_srv,
                  " descriptorContentWriteUAV=",
                  frame_stats_with_external.frame_descriptor_content_write_uav,
                  " descriptorContentWriteSampler=",
                  frame_stats_with_external.frame_descriptor_content_write_sampler,
                  " descriptorContentWriteCopy=",
                  frame_stats_with_external.frame_descriptor_content_write_copy,
                  " descriptorContentWriteFeedbackUAV=",
                  frame_stats_with_external
                      .frame_descriptor_content_write_feedback_uav,
                  " presentUs=", present_us,
                  " presentQueueWaitUs=", present_queue_wait_us,
                  " presentWorkUs=", present_work_us,
                  " prePresentWallUs=", pre_present_wall_us,
                  " queueSignalUs=", queue_signal_us,
                  " queueWaitUs=", queue_wait_us,
                  " createResourceUs=", create_resource_us,
                  " createReservedResourceUs=", create_reserved_resource_us,
                  " createHeapUs=", create_heap_us,
                  " createPipelineUs=", create_pipeline_us,
                  " tileMappingUs=", tile_mapping_us,
                  " cmdlistRecordVectorAppendUs=", cmdlist_record_us,
                  " resourceLockUs=", resource_lock_us,
                  " resourceLockCount=",
                  frame_stats.frame_resource_lock_count,
                  " stagingUploadUs=", staging_upload_us,
                  " stagingUploadBytes=",
                  frame_stats.frame_staging_upload_bytes,
                  " bufRefreshBytes=", frame_stats.frame_buffer_refresh_bytes,
                  " bufRefreshCount=", frame_stats.frame_buffer_refresh_count,
                  " chunkCommitUs=", chunk_commit_us,
                  " chunkCommitCount=", frame_stats.frame_chunk_commit_count,
                  " drawRecordUs=", draw_record_us,
                  " drawIssueCount=", frame_stats.frame_draw_issue_count,
                  " taskFaults=", frame_stats.frame_task_fault_count,
                  " taskPageins=", frame_stats.frame_task_pagein_count,
                  " taskCowFaults=", frame_stats.frame_task_cow_fault_count,
                  " hostZeroFills=", frame_stats.frame_host_zero_fill_count,
                  " threadUserUs=",
                  durationUs(frame_stats.frame_thread_user_interval),
                  " threadKernelUs=",
                  durationUs(frame_stats.frame_thread_kernel_interval),
                  " threadOffCpuUs=",
                  pre_present_wall_us >
                          durationUs(frame_stats.frame_thread_user_interval) +
                              durationUs(frame_stats.frame_thread_kernel_interval)
                      ? pre_present_wall_us -
                            durationUs(frame_stats.frame_thread_user_interval) -
                            durationUs(frame_stats.frame_thread_kernel_interval)
                      : 0,
                  " drawResolveUs=", draw_resolve_us,
                  " drawResolveCount=", frame_stats.frame_draw_resolve_count,
                  " drawDroppedUnresolved=",
                  frame_stats.frame_draw_dropped_unresolved_count,
                  " drawDroppedPipeline=",
                  frame_stats.frame_draw_dropped_pipeline_count,
                  " drawDroppedSnapshot=",
                  frame_stats.frame_draw_dropped_snapshot_count,
                  " renameFresh=",
                  frame_stats.frame_dynamic_rename_fresh_count,
                  " renameRecycled=",
                  frame_stats.frame_dynamic_rename_recycled_count,
                  " gdiDC=", frame_stats.frame_gdi_dc_count,
                  " createResourceCount=",
                  frame_stats.frame_create_resource_count,
                  " psoCompileWaitCount=",
                  frame_stats.frame_pso_compile_wait_count,
                  " drawEmitUs=", draw_emit_us,
                  " frontendKnownUs=", frontend_known_us,
                  " frontendUnattributedUs=",
                  pre_present_wall_us > frontend_known_us
                      ? pre_present_wall_us - frontend_known_us
                      : 0,
                  " queryGetDataCount=",
                  frame_stats.frame_query_get_data_count,
                  " queryGetDataPendingCount=",
                  frame_stats.frame_query_get_data_pending_count,
                  " queryIssueCount=", frame_stats.frame_query_issue_count,
                  " rasterStatusPollCount=",
                  frame_stats.frame_raster_status_poll_count,
                  " deviceStatusPollCount=",
                  frame_stats.frame_device_status_poll_count,
                  " presentLatencyWaitUs=", present_latency_wait_us,
                  " asyncDrawableBlockingUs=", drawable_blocking_us,
                  " asyncFlushUs=", flush_us,
                  " asyncFlushKnownUs=", flush_known_us,
                  " asyncFlushCollectUs=", flush_collect_us,
                  " asyncFlushRelationUs=", flush_relation_us,
                  " asyncFlushQueryUs=", flush_query_us,
                  " asyncFlushTimestampUs=", flush_timestamp_us,
                  " asyncFlushRenderUs=", flush_render_us,
                  " asyncFlushComputeUs=", flush_compute_us,
                  " asyncFlushBlitUs=", flush_blit_us,
                  " asyncFlushClearUs=", flush_clear_us,
                  " asyncFlushResolveUs=", flush_resolve_us,
                  " asyncFlushScalerUs=", flush_scaler_us,
                  " asyncFlushPresentUs=", flush_present_us,
                  " asyncFlushEventUs=", flush_event_us,
                  " asyncFlushSampleUs=", flush_sample_us,
                  " asyncFlushPendingFenceUs=", flush_pending_fence_us,
                  " asyncFlushSignalUs=", flush_signal_us,
                  " asyncFlushCleanupUs=", flush_cleanup_us,
                  " asyncCommitUs=", commit_us,
                  " asyncEncodePrepareUs=",
                  durationUs(frame_stats.encode_prepare_interval),
                  " asyncEncodeFlushUs=",
                  durationUs(frame_stats.encode_flush_interval),
                  " commandBuffers=", frame_stats.command_buffer_count,
                  " renderPasses=", frame_stats.render_pass_count,
                  " renderCommands=", frame_stats.render_command_count,
                  " renderPsoBinds=", frame_stats.render_pso_bind_count,
                  " computePasses=", compute_passes,
                  " computeCommands=", frame_stats.compute_command_count,
                  " blitPasses=", frame_stats.blit_pass_count,
                  " blitPassOptimized=", frame_stats.blit_pass_optimized,
                  " blitPassCmd=", frame_stats.blit_pass_with_commands_count,
                  " emptyBlitPasses=", frame_stats.blit_pass_empty_count,
                  " deferredFenceOnlyBlitPasses=",
                  frame_stats.blit_pass_deferred_fence_only_count,
                  " mergedFenceOnlyBlitPasses=",
                  frame_stats.blit_pass_merged_fence_only_count,
                  " barrierOnlyBlitPasses=",
                  frame_stats.blit_barrier_only_pass_count,
                  " separatorBlitPasses=",
                  frame_stats.blit_separator_pass_count,
                  " blitPassWaitFence=",
                  frame_stats.blit_pass_with_fence_wait_count,
                  " blitPassUpdateFence=",
                  frame_stats.blit_pass_with_fence_update_count,
                  " blitFenceWaitEntries=",
                  frame_stats.blit_fence_wait_entry_count,
                  " blitFenceUpdateEntries=",
                  frame_stats.blit_fence_update_entry_count,
                  " resourceBarrierBatchesMerged=",
                  frame_stats.resource_barrier_batches_merged,
                  " resourceBarrierBatchesGraphicsInlined=",
                  frame_stats.resource_barrier_batches_graphics_inlined,
                  " resourceBarrierBatchesComputeInlined=",
                  frame_stats.resource_barrier_batches_compute_inlined,
                  " blitCommands=", frame_stats.blit_command_count,
                  " blitB2B=", frame_stats.blit_copy_buffer_to_buffer_count,
                  " blitB2T=", frame_stats.blit_copy_buffer_to_texture_count,
                  " blitT2B=", frame_stats.blit_copy_texture_to_buffer_count,
                  " blitT2T=", frame_stats.blit_copy_texture_to_texture_count,
                  " blitFill=", frame_stats.blit_fill_buffer_count,
                  " blitResolveCounters=", frame_stats.blit_resolve_counters_count,
                  " blitWaitFence=", frame_stats.blit_fence_wait_count,
                  " blitUpdateFence=", frame_stats.blit_fence_update_count,
                  " presentPasses=", frame_stats.present_pass_count,
                  " clearPasses=", frame_stats.clear_pass_count,
                  " clearPassOptimized=", frame_stats.clear_pass_optimized,
                  " resolvePassOptimized=", frame_stats.resolve_pass_optimized,
                  " flushEncoders=", frame_stats.flush_encoder_count,
                  " encodedEncoders=", frame_stats.flush_encoded_encoder_count,
                  " flushNullEncoders=", frame_stats.flush_null_encoder_count,
                  " flushRenderEncoders=", frame_stats.flush_render_encoder_count,
                  " flushComputeEncoders=", frame_stats.flush_compute_encoder_count,
                  " flushBlitEncoders=", frame_stats.flush_blit_encoder_count,
                  " flushTimestampEncoders=",
                  frame_stats.flush_timestamp_encoder_count,
                  " renderDraws=", render_draws,
                  " computeDispatches=", frame_stats.compute_dispatch_count));

  uint64_t code_path_exclusive_total_us = 0;
  uint64_t command_list_close_total_us = 0;
  uint64_t command_list_record_total_us = 0;
  uint64_t compiled_build_total_us = 0;
  std::string code_path_log = str::format(
      "DXMT perf code: frame=", frame, " totalWallUs=", frame_wall_us);
  for (size_t path = 0; path < kPerfCodePathCount; path++) {
    const auto total_us =
        durationUs(frame_stats.frame_code_path_intervals[path]);
    const auto max_us =
        durationUs(frame_stats.frame_code_path_max_intervals[path]);
    code_path_exclusive_total_us += total_us;
    if (path <= static_cast<size_t>(PerfCodePath::CommandListCloseApitrace))
      command_list_close_total_us += total_us;
    if (path >= static_cast<size_t>(PerfCodePath::CommandListDrawInstanced) &&
        path <= static_cast<size_t>(
                    PerfCodePath::CommandListRecordVectorAppend))
      command_list_record_total_us += total_us;
    if (path >= static_cast<size_t>(PerfCodePath::CompiledBuildLoopDispatch)) {
      compiled_build_total_us += total_us;
      command_list_close_total_us += total_us;
    }
    if (frame_stats.frame_code_path_counts[path]) {
      code_path_log += str::format(
          " ", kPerfCodePathNames[path], "Us=", total_us,
          " ", kPerfCodePathNames[path], "Calls=",
          frame_stats.frame_code_path_counts[path],
          " ", kPerfCodePathNames[path], "MaxUs=", max_us);
    }
  }
  code_path_log += str::format(
      " commandListCloseTotalUs=", command_list_close_total_us,
      " commandListRecordTotalUs=", command_list_record_total_us,
      " compiledBuildTotalUs=", compiled_build_total_us,
      " codePathExclusiveTotalUs=", code_path_exclusive_total_us);
  Logger::logFileOnly(LogLevel::Info, code_path_log);

  maybeFlush(frame);
}

ReplayWorkerSummary
summarizeReplayWorkerFrame(const FrameStatistics &stats) {
  ReplayWorkerSummary summary = {};
  summary.execute_replay_us =
      durationUs(stats.frame_execute_replay_interval);
  summary.record_loop_us =
      durationUs(stats.frame_replay_record_loop_interval);
  summary.superseded_mask_us =
      durationUs(stats.frame_replay_superseded_mask_interval);
  summary.compiled_graphics_us =
      durationUs(stats.frame_replay_compiled_graphics_interval);
  summary.compiled_compute_us =
      durationUs(stats.frame_replay_compiled_compute_interval);
  summary.fallback_classification_us =
      durationUs(stats.frame_replay_fallback_classification_interval);
  summary.replay_timed_us =
      summary.record_loop_us +
      durationUs(stats.frame_replay_flush_pass_interval) +
      durationUs(stats.frame_replay_timestamp_resolve_interval) +
      durationUs(stats.frame_replay_cpu_query_resolve_interval);
  const uint64_t command_body_us =
      durationUs(stats.frame_replay_record_draw_interval) +
      durationUs(stats.frame_replay_record_draw_indexed_interval) +
      durationUs(stats.frame_replay_record_dispatch_interval) +
      durationUs(stats.frame_replay_record_pipeline_state_interval) +
      durationUs(stats.frame_replay_record_descriptor_heaps_interval) +
      durationUs(stats.frame_replay_record_root_signature_interval) +
      durationUs(stats.frame_replay_record_root_table_interval) +
      durationUs(stats.frame_replay_record_root_descriptor_interval) +
      durationUs(stats.frame_replay_record_root_constants_interval) +
      durationUs(stats.frame_replay_record_vertex_index_state_interval) +
      durationUs(stats.frame_replay_record_render_targets_interval) +
      durationUs(stats.frame_replay_record_resource_barrier_interval) +
      durationUs(stats.frame_replay_record_copy_clear_resolve_interval) +
      durationUs(stats.frame_replay_record_query_interval) +
      durationUs(stats.frame_replay_record_execute_indirect_interval) +
      durationUs(stats.frame_replay_record_temporal_upscale_interval);
  summary.typed_record_us =
      durationUs(stats.frame_replay_typed_record_interval);
  summary.native_command_body_us =
      command_body_us > summary.typed_record_us
          ? command_body_us - summary.typed_record_us
          : 0;
  const uint64_t named_record_us =
      summary.superseded_mask_us + summary.compiled_graphics_us +
      summary.compiled_compute_us + summary.fallback_classification_us +
      command_body_us;
  // This is the exclusive cost of ReplayCommandRecords control flow: segment
  // validation/dispatch, record visitation, counters and timer bookkeeping.
  // It is intentionally named as executable logic rather than an API gap.
  summary.record_control_us =
      summary.record_loop_us > named_record_us
          ? summary.record_loop_us - named_record_us
          : 0;
  summary.classified_record_us =
      std::min(summary.record_loop_us,
               named_record_us + summary.record_control_us);
  const auto coverage_permille = [](uint64_t value, uint64_t total) {
    return total ? std::min<uint64_t>(1000, value * 1000 / total) : 0;
  };
  summary.replay_coverage_permille = coverage_permille(
      summary.replay_timed_us, summary.execute_replay_us);
  summary.record_coverage_permille = coverage_permille(
      summary.classified_record_us, summary.record_loop_us);
  return summary;
}

void recordReplayWorkerFrame(uint64_t frame, uintptr_t queue,
                             uint32_t queue_type, uint64_t execute_batches,
                             const FrameStatistics &stats) {
  if (!enabled() || !execute_batches)
    return;

  const auto summary = summarizeReplayWorkerFrame(stats);
  std::string timing_log = str::format(
      "DXMT perf replay-worker: frame=", frame,
      " queue=", queue,
      " queueType=", queue_type,
      " executeBatches=", execute_batches,
      " executeDrainUs=", durationUs(stats.frame_execute_drain_interval),
      " executeDrainLockUs=",
      durationUs(stats.frame_execute_drain_lock_interval),
      " executeResourceStateLockHoldUs=",
      durationUs(stats.frame_execute_resource_state_lock_hold_interval),
      " executeReplayUs=", summary.execute_replay_us,
      " executeDecayUs=", durationUs(stats.frame_execute_decay_interval),
      " executeSignalUs=", durationUs(stats.frame_execute_signal_interval),
      " executeCommitUs=", durationUs(stats.frame_execute_commit_interval),
      " replayRecordLoopUs=", summary.record_loop_us,
      " replayFlushPassUs=",
      durationUs(stats.frame_replay_flush_pass_interval),
      " replayTimestampResolveUs=",
      durationUs(stats.frame_replay_timestamp_resolve_interval),
      " replayCpuQueryResolveUs=",
      durationUs(stats.frame_replay_cpu_query_resolve_interval),
      " replayTimedUs=", summary.replay_timed_us,
      " replayCoveragePermille=", summary.replay_coverage_permille,
      " replaySupersededMaskUs=", summary.superseded_mask_us,
      " replayCompiledGraphicsUs=", summary.compiled_graphics_us,
      " replayCompiledGraphicsPackets=",
      stats.frame_replay_compiled_graphics_packet_count,
      " replayCompiledComputeUs=", summary.compiled_compute_us,
      " replayCompiledComputePackets=",
      stats.frame_replay_compiled_compute_packet_count,
      " replayFallbackClassificationUs=",
      summary.fallback_classification_us,
      " replayFallbackClassificationRanges=",
      stats.frame_replay_fallback_classification_count,
      " typedRecordUs=", summary.typed_record_us,
      " nativeCommandBodyUs=", summary.native_command_body_us,
      " replayRecordControlUs=", summary.record_control_us,
      " replayClassifiedRecordUs=", summary.classified_record_us,
      " replayRecordCoveragePermille=", summary.record_coverage_permille);
  timing_log += str::format(
      " replayRecordDrawUs=",
      durationUs(stats.frame_replay_record_draw_interval),
      " replayRecordDrawCount=", stats.frame_replay_record_draw_count,
      " replayRecordDrawIndexedUs=",
      durationUs(stats.frame_replay_record_draw_indexed_interval),
      " replayRecordDrawIndexedCount=",
      stats.frame_replay_record_draw_indexed_count,
      " replayRecordDispatchUs=",
      durationUs(stats.frame_replay_record_dispatch_interval),
      " replayRecordDispatchCount=", stats.frame_replay_record_dispatch_count,
      " replayRecordPipelineStateUs=",
      durationUs(stats.frame_replay_record_pipeline_state_interval),
      " replayRecordPipelineStateCount=",
      stats.frame_replay_record_pipeline_state_count,
      " replayRecordDescriptorHeapsUs=",
      durationUs(stats.frame_replay_record_descriptor_heaps_interval),
      " replayRecordDescriptorHeapsCount=",
      stats.frame_replay_record_descriptor_heaps_count,
      " replayRecordRootSignatureUs=",
      durationUs(stats.frame_replay_record_root_signature_interval),
      " replayRecordRootSignatureCount=",
      stats.frame_replay_record_root_signature_count,
      " replayRecordRootTableUs=",
      durationUs(stats.frame_replay_record_root_table_interval),
      " replayRecordRootTableCount=", stats.frame_replay_record_root_table_count,
      " replayRecordRootDescriptorUs=",
      durationUs(stats.frame_replay_record_root_descriptor_interval),
      " replayRecordRootDescriptorCount=",
      stats.frame_replay_record_root_descriptor_count,
      " replayRecordRootConstantsUs=",
      durationUs(stats.frame_replay_record_root_constants_interval),
      " replayRecordRootConstantsCount=",
      stats.frame_replay_record_root_constants_count,
      " replayRecordVertexIndexStateUs=",
      durationUs(stats.frame_replay_record_vertex_index_state_interval),
      " replayRecordVertexIndexStateCount=",
      stats.frame_replay_record_vertex_index_state_count,
      " replayRecordRenderTargetsUs=",
      durationUs(stats.frame_replay_record_render_targets_interval),
      " replayRecordRenderTargetsCount=",
      stats.frame_replay_record_render_targets_count,
      " replayRecordResourceBarrierUs=",
      durationUs(stats.frame_replay_record_resource_barrier_interval),
      " replayRecordResourceBarrierCount=",
      stats.frame_replay_record_resource_barrier_count,
      " replayRecordCopyClearResolveUs=",
      durationUs(stats.frame_replay_record_copy_clear_resolve_interval),
      " replayRecordCopyClearResolveCount=",
      stats.frame_replay_record_copy_clear_resolve_count,
      " replayRecordQueryUs=",
      durationUs(stats.frame_replay_record_query_interval),
      " replayRecordQueryCount=", stats.frame_replay_record_query_count,
      " replayRecordExecuteIndirectUs=",
      durationUs(stats.frame_replay_record_execute_indirect_interval),
      " replayRecordExecuteIndirectCount=",
      stats.frame_replay_record_execute_indirect_count,
      " replayRecordTemporalUpscaleUs=",
      durationUs(stats.frame_replay_record_temporal_upscale_interval),
      " replayRecordTemporalUpscaleCount=",
      stats.frame_replay_record_temporal_upscale_count);
  timing_log += str::format(
      " replayGetPipelineUs=",
      durationUs(stats.frame_replay_get_pipeline_interval),
      " replayGetMetalPsoUs=",
      durationUs(stats.frame_replay_get_metal_pso_interval),
      " replaySelectPsoUs=",
      durationUs(stats.frame_replay_select_pso_interval),
      " replayDescAccessUs=",
      durationUs(stats.frame_replay_desc_access_interval),
      " replayStateUpdateUs=",
      durationUs(stats.frame_replay_state_update_interval),
      " replayAttachUs=", durationUs(stats.frame_replay_attach_interval),
      " replayBindSnapshotUs=",
      durationUs(stats.frame_replay_bind_snapshot_interval),
      " replayEstimateUs=", durationUs(stats.frame_replay_estimate_interval),
      " replayPacketUs=", durationUs(stats.frame_replay_packet_interval),
      " replayQueueUs=", durationUs(stats.frame_replay_queue_interval),
      " replayEmitUs=", durationUs(stats.frame_replay_emit_interval),
      " replayFlushBlitUs=",
      durationUs(stats.frame_replay_flush_blit_interval),
      " replayFlushComputeUs=",
      durationUs(stats.frame_replay_flush_compute_interval),
      " replayFlushGraphicsUs=",
      durationUs(stats.frame_replay_flush_graphics_interval),
      " replayFlushBarrierUs=",
      durationUs(stats.frame_replay_flush_barrier_interval),
      " replayBuildPlanUs=",
      durationUs(stats.frame_replay_build_plan_interval),
      " replayEmitTimestampUs=",
      durationUs(stats.frame_replay_emit_timestamp_interval));
  Logger::logFileOnly(LogLevel::Info, timing_log);

  Logger::logFileOnly(
      LogLevel::Info,
      str::format(
          "DXMT perf replay-worker counters: frame=", frame,
          " queue=", queue,
          " queueType=", queue_type,
          " executeBatches=", execute_batches,
          " drawCount=", stats.frame_replay_draw_count,
          " psoRootUnchanged=", stats.frame_replay_pso_root_unchanged,
          " fullBindUnchanged=", stats.frame_replay_full_bind_unchanged,
          " flushBlitCalls=", stats.frame_replay_flush_blit_count,
          " flushComputeCalls=", stats.frame_replay_flush_compute_count,
          " flushGraphicsCalls=", stats.frame_replay_flush_graphics_count,
          " flushBarrierCalls=", stats.frame_replay_flush_barrier_count,
          " emitTimestampCalls=", stats.frame_replay_emit_timestamp_count,
          " flushPassBatchesCalls=",
          stats.frame_replay_flush_pass_batches_count,
          " flushPassBatchesEmpty=",
          stats.frame_replay_flush_pass_batches_empty,
          " resourceAccessCalls=", stats.frame_replay_resource_access_count,
          " resourceAccessSteadyNoop=",
          stats.frame_replay_resource_access_steady_noop,
          " descAccessHits=", stats.frame_replay_desc_access_hit,
          " descAccessMisses=", stats.frame_replay_desc_access_miss,
          " descAccessPassthrough=",
          stats.frame_replay_desc_access_passthrough,
          " supersededStateRecordsSkipped=",
          stats.frame_replay_superseded_state_records_skipped,
          " compiledGraphicsCandidates=",
          stats.frame_replay_compiled_graphics_candidates,
          " compiledGraphicsLegacy=",
          stats.frame_replay_compiled_graphics_legacy,
          " compiledGraphicsBarriers=",
          stats.frame_replay_compiled_graphics_barriers,
          " compiledGraphicsGsTs=", stats.frame_replay_compiled_graphics_gs_ts,
          " compiledGraphicsIndirect=",
          stats.frame_replay_compiled_graphics_indirect,
          " mismatchBarriers=", stats.frame_replay_mismatch_barriers,
          " appBarrierTransitions=",
          stats.frame_replay_app_barrier_transitions,
          " bindingGenBumps=", stats.frame_replay_binding_gen_bumps,
          " bindingGenPipeline=", stats.frame_replay_binding_gen_pipeline,
          " bindingGenDescriptorHeaps=",
          stats.frame_replay_binding_gen_descriptor_heaps,
          " bindingGenVertexBuffers=",
          stats.frame_replay_binding_gen_vertex_buffers,
          " bindingGenRootSignature=",
          stats.frame_replay_binding_gen_root_signature,
          " bindingGenRootDescriptorTable=",
          stats.frame_replay_binding_gen_root_descriptor_table,
          " bindingGenRootDescriptor=",
          stats.frame_replay_binding_gen_root_descriptor,
          " bindingGenRootConstants=",
          stats.frame_replay_binding_gen_root_constants,
          " bindingGenIndirectVertexBuffer=",
          stats.frame_replay_binding_gen_indirect_vertex_buffer,
          " bindingGenIndirectRootConstants=",
          stats.frame_replay_binding_gen_indirect_root_constants,
          " bindingGenIndirectRootDescriptor=",
          stats.frame_replay_binding_gen_indirect_root_descriptor,
          " snapshotRequests=", stats.frame_replay_snapshot_requests,
          " snapshotCacheHits=", stats.frame_replay_snapshot_cache_hits,
          " snapshotCacheMisses=", stats.frame_replay_snapshot_cache_misses,
          " snapshotPassthrough=", stats.frame_replay_snapshot_passthrough,
          " snapshotGraphicsGenChanges=",
          stats.frame_replay_snapshot_graphics_gen_changes,
          " snapshotDescriptorGenChanges=",
          stats.frame_replay_snapshot_descriptor_gen_changes,
          " snapshotBothGenChanges=",
          stats.frame_replay_snapshot_both_gen_changes,
          " snapshotNoGenChanges=",
          stats.frame_replay_snapshot_no_gen_changes,
          " snapshotCapturedEntries=",
          stats.frame_replay_snapshot_captured_entries,
          " snapshotCapturedDescriptors=",
          stats.frame_replay_snapshot_captured_descriptors,
          " snapshotCapturedMissingDescriptors=",
          stats.frame_replay_snapshot_captured_missing_descriptors,
          " snapshotCapturedRootDescriptors=",
          stats.frame_replay_snapshot_captured_root_descriptors,
          " snapshotCapturedRootConstants=",
          stats.frame_replay_snapshot_captured_root_constants,
          " snapshotCapturedVertexBuffers=",
          stats.frame_replay_snapshot_captured_vertex_buffers,
          " snapshotCapturedBindless=",
          stats.frame_replay_snapshot_captured_bindless));
}

void flushFinal(uint64_t frame) {
  flushCounters(frame, true);
}

void recordWaitCpuFence(uint64_t wait_us) {
  if (!enabled() || !wait_us)
    return;
  g_counters.wait_cpu_fence_count.fetch_add(1, std::memory_order_relaxed);
  g_counters.wait_cpu_fence_us.fetch_add(wait_us, std::memory_order_relaxed);
  updateMax(g_counters.wait_cpu_fence_max_us, wait_us);
}

void recordTimestampGpuResolve(uint64_t queries) {
  if (!enabled())
    return;
  g_counters.timestamp_gpu_runs.fetch_add(1, std::memory_order_relaxed);
  g_counters.timestamp_gpu_queries.fetch_add(queries,
                                            std::memory_order_relaxed);
}

void recordTimestampCpuFallback(uint64_t queries) {
  if (!enabled())
    return;
  g_counters.timestamp_cpu_fallbacks.fetch_add(1, std::memory_order_relaxed);
  g_counters.timestamp_cpu_fallback_queries.fetch_add(
      queries, std::memory_order_relaxed);
}

void recordTimestampCpuDeferred(uint64_t queries) {
  if (!enabled())
    return;
  g_counters.timestamp_cpu_deferred.fetch_add(1, std::memory_order_relaxed);
  g_counters.timestamp_cpu_deferred_queries.fetch_add(
      queries, std::memory_order_relaxed);
}

void recordTimestampCpuImmediate(bool unsafe) {
  if (!enabled())
    return;
  g_counters.timestamp_cpu_immediate.fetch_add(1, std::memory_order_relaxed);
  if (unsafe)
    g_counters.timestamp_cpu_unsafe.fetch_add(1, std::memory_order_relaxed);
}

void recordTimestampCpuMaterialized() {
  if (!enabled())
    return;
  g_counters.timestamp_cpu_materialized.fetch_add(1,
                                                 std::memory_order_relaxed);
}

void recordTimestampCpuWait(uint64_t wait_us) {
  if (!enabled() || !wait_us)
    return;
  g_counters.timestamp_cpu_wait_us.fetch_add(wait_us,
                                            std::memory_order_relaxed);
  updateMax(g_counters.timestamp_cpu_wait_max_us, wait_us);
}

void recordQueryBatchWait(uint64_t batches, uint64_t queries,
                          uint64_t wait_us) {
  if (!enabled())
    return;
  g_counters.query_batch_waits.fetch_add(batches, std::memory_order_relaxed);
  g_counters.query_batch_wait_queries.fetch_add(queries,
                                               std::memory_order_relaxed);
  g_counters.query_batch_wait_us.fetch_add(wait_us, std::memory_order_relaxed);
  updateMax(g_counters.query_batch_wait_max_us, wait_us);
}

void recordTileMapping(uint64_t standard_ops, uint64_t packed_ops,
                       uint64_t map_ops, uint64_t unmap_ops,
                       uint64_t invalid, uint64_t metal_failures,
                       uint64_t barriers) {
  if (!enabled())
    return;
  g_counters.tile_mapping_calls.fetch_add(1, std::memory_order_relaxed);
  g_counters.tile_mapping_standard_ops.fetch_add(standard_ops,
                                                std::memory_order_relaxed);
  g_counters.tile_mapping_packed_ops.fetch_add(packed_ops,
                                              std::memory_order_relaxed);
  g_counters.tile_mapping_map_ops.fetch_add(map_ops,
                                           std::memory_order_relaxed);
  g_counters.tile_mapping_unmap_ops.fetch_add(unmap_ops,
                                             std::memory_order_relaxed);
  g_counters.tile_mapping_invalid.fetch_add(invalid,
                                           std::memory_order_relaxed);
  g_counters.tile_mapping_metal_failures.fetch_add(
      metal_failures, std::memory_order_relaxed);
  g_counters.tile_mapping_barriers.fetch_add(barriers,
                                            std::memory_order_relaxed);
}

void recordGraphicsPipelineCreate(uint64_t duration_us, bool success) {
  if (!enabled())
    return;
  g_counters.graphics_pso_creates.fetch_add(1, std::memory_order_relaxed);
  g_counters.graphics_pso_create_us.fetch_add(duration_us,
                                             std::memory_order_relaxed);
  updateMax(g_counters.graphics_pso_create_max_us, duration_us);
  if (!success)
    g_counters.graphics_pso_create_failures.fetch_add(
        1, std::memory_order_relaxed);
}

void recordComputePipelineCreate(uint64_t duration_us, bool success) {
  if (!enabled())
    return;
  g_counters.compute_pso_creates.fetch_add(1, std::memory_order_relaxed);
  g_counters.compute_pso_create_us.fetch_add(duration_us,
                                            std::memory_order_relaxed);
  updateMax(g_counters.compute_pso_create_max_us, duration_us);
  if (!success)
    g_counters.compute_pso_create_failures.fetch_add(
        1, std::memory_order_relaxed);
}

void recordPsoArtifactCacheLookup(PsoArtifactKind kind,
                                  PsoArtifactCacheResult result,
                                  uint64_t wait_us) {
  if (!enabled())
    return;
  auto &counters = psoArtifactCounters(kind);
  switch (result) {
  case PsoArtifactCacheResult::Hit:
    counters.cache_hits.fetch_add(1, std::memory_order_relaxed);
    break;
  case PsoArtifactCacheResult::Miss:
    counters.cache_misses.fetch_add(1, std::memory_order_relaxed);
    break;
  case PsoArtifactCacheResult::Wait:
    counters.cache_waits.fetch_add(1, std::memory_order_relaxed);
    counters.cache_wait_us.fetch_add(wait_us, std::memory_order_relaxed);
    break;
  }
}

void recordPsoArtifactCompile(PsoArtifactKind kind, uint64_t duration_us,
                              bool success) {
  if (!enabled())
    return;
  auto &counters = psoArtifactCounters(kind);
  counters.compiles.fetch_add(1, std::memory_order_relaxed);
  counters.compile_us.fetch_add(duration_us, std::memory_order_relaxed);
  if (!success)
    counters.compile_failures.fetch_add(1, std::memory_order_relaxed);
}

void recordMetalCommandBufferCommit(uint64_t duration_us) {
  if (!enabled())
    return;
  g_counters.metal_commit_count.fetch_add(1, std::memory_order_relaxed);
  g_counters.metal_commit_wall_us.fetch_add(duration_us,
                                           std::memory_order_relaxed);
  updateMax(g_counters.metal_commit_wall_max_us, duration_us);
}

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
                              uint32_t max_encode_blit_encoders) {
  if (!enabled() || !command_buffers)
    return;
  const auto gpu_envelope_ns =
      gpu_end_ns > gpu_start_ns ? gpu_end_ns - gpu_start_ns : 0;
  Logger::logFileOnly(
      LogLevel::Info,
      str::format("DXMT perf gpu-frame: frame=", frame,
                  " commandBuffers=", command_buffers,
                  " gpuStartNs=", gpu_start_ns,
                  " gpuEndNs=", gpu_end_ns,
                  " gpuEnvelopeUs=", gpu_envelope_ns / 1000,
                  " gpuBusySumUs=", gpu_busy_sum_ns / 1000,
                  " gpuActiveUnionUs=", gpu_active_union_ns / 1000,
                  " gpuIdleUs=", gpu_idle_ns / 1000,
                  " commitToCompleteSumUs=", commit_to_complete_sum_us,
                  " commitToCompleteMaxUs=", commit_to_complete_max_us,
                  " boundaryToFirstPublishUs=", boundary_to_first_publish_us,
                  " publishSpanUs=", publish_span_us,
                  " publishGapMaxUs=", publish_gap_max_us,
                  " publishToEncodeSumUs=", publish_to_encode_sum_us,
                  " publishToEncodeMaxUs=", publish_to_encode_max_us,
                  " encodeSumUs=", encode_sum_us,
                  " encodeMaxUs=", encode_max_us,
                  " encodePrepareSumUs=", encode_prepare_sum_us,
                  " encodePrepareMaxUs=", encode_prepare_max_us,
                  " encodeFlushSumUs=", encode_flush_sum_us,
                  " encodeFlushMaxUs=", encode_flush_max_us,
                  " encodeToCommitSumUs=", encode_to_commit_sum_us,
                  " encodeToCommitMaxUs=", encode_to_commit_max_us,
                  " firstPublishToLastCommitUs=",
                  first_publish_to_last_commit_us,
                  " maxEncodeChunk=", max_encode_chunk,
                  " maxEncodeInputEncoders=", max_encode_input_encoders,
                  " maxEncodeEncodedEncoders=", max_encode_encoded_encoders,
                  " maxEncodeRenderEncoders=", max_encode_render_encoders,
                  " maxEncodeComputeEncoders=", max_encode_compute_encoders,
                  " maxEncodeBlitEncoders=", max_encode_blit_encoders));
}

void recordGpuFrameEncodeBreakdown(uint64_t frame,
                                   const FrameStatistics &stats) {
  if (!enabled())
    return;
  Logger::logFileOnly(
      LogLevel::Info,
      str::format(
          "DXMT perf encode-frame: frame=", frame,
          " encodePrepareUs=", durationUs(stats.encode_prepare_interval),
          " encodeFlushUs=", durationUs(stats.encode_flush_interval),
          " compiledDrawEncodeUs=",
          durationUs(stats.frame_compiled_draw_encode_interval),
          " compiledDrawCommonUs=",
          durationUs(stats.frame_compiled_draw_common_interval),
          " compiledDrawPipelineUs=",
          durationUs(stats.frame_compiled_draw_pipeline_interval),
          " compiledDrawDssoUs=",
          durationUs(stats.frame_compiled_draw_dsso_interval),
          " compiledDrawRasterizerUs=",
          durationUs(stats.frame_compiled_draw_rasterizer_interval),
          " compiledDrawBindingGateUs=",
          durationUs(stats.frame_compiled_draw_binding_gate_interval),
          " compiledDrawBindingSnapshotUs=",
          durationUs(stats.frame_compiled_draw_binding_snapshot_interval),
          " compiledDrawDynamicStateUs=",
          durationUs(stats.frame_compiled_draw_dynamic_state_interval),
          " compiledDrawVisibilityUs=",
          durationUs(stats.frame_compiled_draw_visibility_interval),
          " compiledDrawBodyUs=",
          durationUs(stats.frame_compiled_draw_body_interval),
          " compiledDrawDirectPackets=",
          stats.frame_compiled_draw_direct_packets,
          " compiledDrawReplayPackets=",
          stats.frame_compiled_draw_replay_packets,
          " compiledDrawBindingGenerationHits=",
          stats.frame_compiled_draw_binding_generation_hits,
          " compiledDrawBindingFingerprintHits=",
          stats.frame_compiled_draw_binding_fingerprint_hits,
          " compiledDrawBindingMisses=",
          stats.frame_compiled_draw_binding_misses,
          " compiledDispatchEncodeUs=",
          durationUs(stats.frame_compiled_dispatch_encode_interval),
          " argumentTableUpdateUs=",
          durationUs(stats.frame_argument_table_update_interval),
          " argumentTableBindUs=",
          durationUs(stats.frame_argument_table_bind_interval),
          " flushTotalUs=", durationUs(stats.flush_total_interval),
          " flushCollectUs=", durationUs(stats.flush_collect_interval),
          " flushRelationUs=", durationUs(stats.flush_relation_interval),
          " flushQueryUs=", durationUs(stats.flush_query_interval),
          " flushRenderUs=", durationUs(stats.flush_render_interval),
          " flushComputeUs=", durationUs(stats.flush_compute_interval),
          " flushBlitUs=", durationUs(stats.flush_blit_interval),
          " flushClearUs=", durationUs(stats.flush_clear_interval),
          " flushResolveUs=", durationUs(stats.flush_resolve_interval),
          " flushPresentUs=", durationUs(stats.flush_present_interval),
          " flushEventUs=", durationUs(stats.flush_event_interval),
          " flushSampleUs=", durationUs(stats.flush_sample_interval),
          " flushPendingFenceUs=",
          durationUs(stats.flush_pending_fence_interval),
          " flushSignalUs=", durationUs(stats.flush_signal_interval),
          " flushCleanupUs=", durationUs(stats.flush_cleanup_interval),
          " renderPasses=", stats.render_pass_count,
          " renderDraws=", stats.render_draw_count,
          " renderIndexedDraws=", stats.render_indexed_draw_count,
          " computeDispatches=", stats.compute_dispatch_count,
          " blitPasses=", stats.blit_pass_count,
          " flushEncoders=", stats.flush_encoder_count,
          " encodedEncoders=", stats.flush_encoded_encoder_count,
          // Per-draw encode cost. Also present on the frame line, but that
          // one is emitted at the present boundary, which the encode thread
          // has usually not reached yet; read these here.
          " drawResolveUs=", durationUs(stats.frame_draw_resolve_interval),
          " drawResolveCount=", stats.frame_draw_resolve_count,
          " drawEmitUs=", durationUs(stats.frame_draw_emit_interval),
          " psoCompileWaitUs=",
          durationUs(stats.frame_pso_compile_wait_interval),
          " psoCompileWaitCount=", stats.frame_pso_compile_wait_count));
}

void recordDrawableAcquire(uint64_t duration_us) {
  if (!enabled())
    return;
  g_counters.drawable_acquire_count.fetch_add(1, std::memory_order_relaxed);
  g_counters.drawable_acquire_us.fetch_add(duration_us,
                                           std::memory_order_relaxed);
  updateMax(g_counters.drawable_acquire_max_us, duration_us);
}

void recordDescriptorContentWrite(uint32_t kind) {
  if (!enabled())
    return;
  g_counters.descriptor_content_writes.fetch_add(1,
                                                 std::memory_order_relaxed);
  switch (kind) {
  case 1:
    g_counters.descriptor_content_write_cbv.fetch_add(
        1, std::memory_order_relaxed);
    break;
  case 2:
    g_counters.descriptor_content_write_srv.fetch_add(
        1, std::memory_order_relaxed);
    break;
  case 3:
    g_counters.descriptor_content_write_uav.fetch_add(
        1, std::memory_order_relaxed);
    break;
  case 4:
    g_counters.descriptor_content_write_sampler.fetch_add(
        1, std::memory_order_relaxed);
    break;
  case 5:
    g_counters.descriptor_content_write_copy.fetch_add(
        1, std::memory_order_relaxed);
    break;
  case 6:
    g_counters.descriptor_content_write_feedback_uav.fetch_add(
        1, std::memory_order_relaxed);
    break;
  default:
    break;
  }
}

void recordNativeDescriptorBufferRecord(uint32_t kind) {
  if (!enabled())
    return;
  g_counters.native_descriptor_buffer_records.fetch_add(
      1, std::memory_order_relaxed);
  switch (kind) {
  case 1:
    g_counters.native_descriptor_buffer_record_cbv.fetch_add(
        1, std::memory_order_relaxed);
    break;
  case 2:
    g_counters.native_descriptor_buffer_record_srv.fetch_add(
        1, std::memory_order_relaxed);
    break;
  case 3:
    g_counters.native_descriptor_buffer_record_uav.fetch_add(
        1, std::memory_order_relaxed);
    break;
  default:
    break;
  }
}

void recordNativeDescriptorBufferRecordCounter() {
  if (!enabled())
    return;
  g_counters.native_descriptor_buffer_record_counters.fetch_add(
      1, std::memory_order_relaxed);
}

void recordNativeDescriptorBufferRecordMissingResource() {
  if (!enabled())
    return;
  g_counters.native_descriptor_buffer_record_missing_resource.fetch_add(
      1, std::memory_order_relaxed);
}

void recordNativeDescriptorResourceTableEntry() {
  if (!enabled())
    return;
  g_counters.native_descriptor_resource_table_entries.fetch_add(
      1, std::memory_order_relaxed);
}

} // namespace dxmt::perf
