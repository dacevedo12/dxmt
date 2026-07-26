#pragma once

#include "dxmt_perf_stats.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Test-only fault injection occurrence counters for the replay path. The
// counters live here so the queue replay TUs and any extracted replay module
// share a single occurrence sequence.
extern std::atomic<uint64_t> g_test_native_pipeline_compilation_occurrence;
extern std::atomic<uint64_t> g_test_native_descriptor_lookup_occurrence;

// True when the environment variable `name` selects this occurrence for fault
// injection. "always"/"all" fire on every call; a numeric value fires on the
// n-th call. Always bumps `occurrence` when it can fire.
[[nodiscard]] bool
ShouldInjectQueueReplayFault(const char *name, std::atomic<uint64_t> &occurrence);

// Appends `name` to the DXMT_TEST_FAULT_MARKER file when configured.
void RecordQueueReplayFault(const char *name);

// Truthy-env test shared by every DXMT_DIAG_* switch.
[[nodiscard]] bool D3D12DiagEnabledEnv(const char *name);

// Default-on env test: anything other than an explicit off value enables.
[[nodiscard]] bool D3D12EnabledEnvDefaultOn(const char *name);

[[nodiscard]] bool D3D12SubmissionLifecycleEnabled();

void D3D12SubmissionLifecycleLog(uintptr_t queue, UINT queue_type,
                                 const char *stage, const char *operation,
                                 uint64_t pair_id, uint64_t queue_pair_id,
                                 uint64_t frame_id, uint64_t chunk_id,
                                 uintptr_t command_buffer,
                                 uint64_t wait_sequence,
                                 uint64_t dependency_sequence);

[[nodiscard]] bool D3D12RecordLoopBreakdownEnabled();
[[nodiscard]] bool D3D12RecordLoopStallEnabled();

// Records an execute-phase duration into `bucket` and the aggregate Drain
// bucket.
void RecordExecuteDrainTime(dxmt::FrameStatistics *stats,
                            dxmt::perf::ExecuteTimeBucket bucket,
                            dxmt::clock::duration duration);

[[nodiscard]] bool D3D12TimestampGpuResolveEnabled();
[[nodiscard]] bool D3D12QueryCpuFallbackDeferEnabled();
[[nodiscard]] bool D3D12QueryFallbackStatsEnabled();
[[nodiscard]] bool D3D12DeferredTimestampMarkersEnabled();

// Timestamp/query resolve statistics shared by the query resolve and replay
// state TUs.
extern std::atomic<uint64_t> g_timestamp_gpu_resolve_runs;
extern std::atomic<uint64_t> g_timestamp_gpu_resolve_queries;
extern std::atomic<uint64_t> g_timestamp_cpu_fallbacks;
extern std::atomic<uint64_t> g_timestamp_cpu_fallback_queries;
extern std::atomic<uint64_t> g_timestamp_cpu_wait_us;
extern std::atomic<uint64_t> g_timestamp_cpu_deferred_fallbacks;
extern std::atomic<uint64_t> g_timestamp_cpu_deferred_queries;
extern std::atomic<uint64_t> g_timestamp_cpu_map_materialized_fallbacks;
extern std::atomic<uint64_t> g_timestamp_cpu_immediate_fallbacks;
extern std::atomic<uint64_t> g_timestamp_cpu_unsafe_fallbacks;

[[nodiscard]] bool D3D12DiagTextureCopyEnabled();
[[nodiscard]] bool D3D12HangDenseEnabled();
[[nodiscard]] bool D3D12DiagDrawStateEnabled();
[[nodiscard]] bool D3D12DiagSwapChainEnabled();
[[nodiscard]] bool D3D12DiagIAReadbackEnabled();
[[nodiscard]] bool D3D12DiagBindingsEnabled();
[[nodiscard]] bool D3D12DiagShaderFilterConfigured();
[[nodiscard]] bool D3D12DiagShaderKeySelected(std::string_view shader_key);
[[nodiscard]] bool D3D12DiagBindingRecipeCacheEnabled();
[[nodiscard]] bool D3D12DiagDrawVisibilityEnabled();
[[nodiscard]] bool D3D12DiagCBVReadbackEnabled();
[[nodiscard]] bool D3D12DiagRootCauseDenseEnabled();
[[nodiscard]] bool D3D12DiagCorrectnessDenseEnabled();
[[nodiscard]] uint32_t D3D12DiagRootCauseDenseMaxSlots();
[[nodiscard]] bool D3D12ApitraceGpuCbvSnapshotEnabled();
[[nodiscard]] bool DiagIsTargetCompositePso(const std::string &key);
[[nodiscard]] bool D3D12DiagExecuteIndirectEnabled();
[[nodiscard]] uint32_t D3D12DiagIAReadbackBytes();
[[nodiscard]] uint32_t D3D12DiagCBVReadbackBytes();

// Exponential log throttle: the first 16 occurrences log, then powers of two.
[[nodiscard]] bool D3D12DiagShouldLog(std::atomic<uint32_t> &counter,
                                      bool enabled);

[[nodiscard]] bool D3D12DiagRootCauseTargetMatches(const std::string &key);
[[nodiscard]] uint32_t D3D12DiagRootCauseTargetSampleLimit();

// Replay record identity for diagnostics. Falls back to the apitrace D3D
// sequence when no record scope is active.
[[nodiscard]] uint64_t DiagCurrentReplayRecordSequence();
[[nodiscard]] uint64_t DiagCurrentReplayRecordSerial();

// Scoped override of the current replay record identity.
struct DiagReplayRecordScope {
  uint64_t old_sequence = 0;
  uint64_t old_serial = 0;

  DiagReplayRecordScope(uint64_t sequence, uint64_t serial);
  ~DiagReplayRecordScope();
};

// PERF DIAG (DXMT_DIAG_REPLAY_BREAKDOWN): count dynamic_cast RTTI resolves on
// the replay hot path. Under Rosetta x86 translation each dynamic_cast walks
// the RTTI tree (__dynamic_cast) and is a prime constant-factor-tax suspect.
// File-scope thread_local so the free GetResource/GetPipelineState helpers can
// bump it; reset at compiled-node execution start, read in the breakdown dump.
struct ReplayRttiCounters {
  uint64_t getResource = 0;
  uint64_t getPipeline = 0;
};

[[nodiscard]] ReplayRttiCounters &replayRttiCounters();
[[nodiscard]] bool ReplayBreakdownEnabled();
[[nodiscard]] bool ReplayPerfEnabled();

// descAccess content-fingerprint cache (default ON; DXMT_DESCACCESS_CACHE=0 to
// disable for instant A/B). DXMT_DESCACCESS_VERIFY=1 keeps computing the cache
// but never skips (still runs the full hazard pass) — a failsafe to confirm the
// fingerprint/epoch logic and hit-rate without risking a wrong skip.
[[nodiscard]] bool DescAccessCacheEnabled();
[[nodiscard]] bool DescAccessVerify();

} // namespace dxmt::d3d12
