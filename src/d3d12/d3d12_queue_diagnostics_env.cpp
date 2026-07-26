#include "d3d12_queue_diagnostics_env.hpp"

#include "dxmt_apitrace_d3d.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_lifecycle_telemetry.hpp"
#include "util_string.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace dxmt::d3d12 {

namespace {

thread_local uint64_t g_diag_replay_record_sequence = 0;
thread_local uint64_t g_diag_replay_record_serial = 0;

} // namespace

std::atomic<uint64_t> g_test_native_pipeline_compilation_occurrence = 0;
std::atomic<uint64_t> g_test_native_descriptor_lookup_occurrence = 0;

bool
ShouldInjectQueueReplayFault(const char *name,
                             std::atomic<uint64_t> &occurrence) {
  const auto setting = env::getEnvVar(name);
  if (setting.empty())
    return false;
  if (setting == "always" || setting == "all") {
    occurrence.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  char *end = nullptr;
  const auto target = std::strtoull(setting.c_str(), &end, 0);
  if (end == setting.c_str() || *end || !target)
    return false;
  return occurrence.fetch_add(1, std::memory_order_relaxed) + 1 == target;
}

void
RecordQueueReplayFault(const char *name) {
  const auto path = env::getEnvVar("DXMT_TEST_FAULT_MARKER");
  if (!path.empty()) {
    if (FILE *marker = std::fopen(path.c_str(), "a")) {
      std::fprintf(marker, "%s\n", name);
      std::fclose(marker);
    }
  }
}

bool
D3D12DiagEnabledEnv(const char *name) {
  auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" || value == "trace";
}

bool
D3D12SubmissionLifecycleEnabled() {
  return D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
}

void
D3D12SubmissionLifecycleLog(uintptr_t queue, UINT queue_type,
                            const char *stage, const char *operation,
                            uint64_t pair_id, uint64_t queue_pair_id,
                            uint64_t frame_id, uint64_t chunk_id,
                            uintptr_t command_buffer, uint64_t wait_sequence,
                            uint64_t dependency_sequence) {
  if (!D3D12SubmissionLifecycleEnabled())
    return;
  WARN_FILE_ONLY("D3D12 submission lifecycle:"
       " eventSeq=", dxmt::lifecycle::nextEventSequence(),
       " pairSeq=", pair_id,
       " queuePairSeq=", queue_pair_id,
       " queue=", queue,
       " queueType=", queue_type,
       " thread=", dxmt::lifecycle::threadId(),
       " stage=", stage,
       " operation=", operation,
       " frame=", frame_id,
       " chunk=", chunk_id,
       " cmdbuf=", command_buffer,
       " waitSeq=", wait_sequence,
       " dependencySeq=", dependency_sequence);
}

bool
D3D12RecordLoopBreakdownEnabled() {
  static const bool on = D3D12DiagEnabledEnv("DXMT_DIAG_REPLAY_BREAKDOWN");
  return on;
}

bool
D3D12RecordLoopStallEnabled() {
  static const bool on = D3D12DiagEnabledEnv("DXMT_DIAG_STALL");
  return on;
}

void
RecordExecuteDrainTime(dxmt::FrameStatistics *stats,
                       dxmt::perf::ExecuteTimeBucket bucket,
                       dxmt::clock::duration duration) {
  dxmt::perf::recordExecuteTime(stats, bucket, duration);
  dxmt::perf::recordExecuteTime(stats, dxmt::perf::ExecuteTimeBucket::Drain,
                                duration);
}

bool
D3D12EnabledEnvDefaultOn(const char *name) {
  auto value = env::getEnvVar(name);
  return value != "0" && value != "false" && value != "no" &&
         value != "off";
}

bool
D3D12TimestampGpuResolveEnabled() {
  static const bool enabled =
      D3D12EnabledEnvDefaultOn("DXMT_D3D12_TIMESTAMP_GPU_RESOLVE");
  return enabled;
}

bool
D3D12QueryCpuFallbackDeferEnabled() {
  static const bool enabled =
      D3D12EnabledEnvDefaultOn("DXMT_D3D12_QUERY_CPU_FALLBACK_DEFER");
  return enabled;
}

bool
D3D12QueryFallbackStatsEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_D3D12_QUERY_FALLBACK_STATS") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_D3D12_QUERY") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
  return enabled;
}

std::atomic<uint64_t> g_timestamp_gpu_resolve_runs = {0};
std::atomic<uint64_t> g_timestamp_gpu_resolve_queries = {0};
std::atomic<uint64_t> g_timestamp_cpu_fallbacks = {0};
std::atomic<uint64_t> g_timestamp_cpu_fallback_queries = {0};
std::atomic<uint64_t> g_timestamp_cpu_wait_us = {0};
std::atomic<uint64_t> g_timestamp_cpu_deferred_fallbacks = {0};
std::atomic<uint64_t> g_timestamp_cpu_deferred_queries = {0};
std::atomic<uint64_t> g_timestamp_cpu_map_materialized_fallbacks = {0};
std::atomic<uint64_t> g_timestamp_cpu_immediate_fallbacks = {0};
std::atomic<uint64_t> g_timestamp_cpu_unsafe_fallbacks = {0};

bool
D3D12DeferredTimestampMarkersEnabled() {
  static const bool enabled =
      D3D12EnabledEnvDefaultOn("DXMT_D3D12_DEFER_TIMESTAMP_MARKERS");
  return enabled;
}

bool
D3D12DiagTextureCopyEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_TEXTURE_COPY") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_D3D12_VIEWS") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_GPU_HANG_DENSE") ||
      D3D12DiagEnabledEnv("DXMT_VALIDATION") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_VALIDATION");
  return enabled;
}

bool
D3D12HangDenseEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_GPU_HANG_DENSE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE") ||
      D3D12DiagEnabledEnv("DXMT_VALIDATION") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_VALIDATION");
  return enabled;
}

bool
D3D12DiagDrawStateEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_DRAW_STATE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_RENDER_COMMANDS") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE");
  return enabled;
}

bool
D3D12DiagSwapChainEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_SWAPCHAIN") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE");
  return enabled;
}

bool
D3D12DiagIAReadbackEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_IA_READBACK") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_DRAW_STATE_READBACK");
  return enabled;
}

bool
D3D12DiagBindingsEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_BINDINGS") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE");
  return enabled;
}

bool
D3D12DiagShaderFilterConfigured() {
  const auto filters = env::getEnvVar("DXMT_DIAG_SHADER_HASHES");
  return !filters.empty() && filters != "all";
}

bool
D3D12DiagShaderKeySelected(std::string_view shader_key) {
  const auto filters = env::getEnvVar("DXMT_DIAG_SHADER_HASHES");
  if (filters.empty() || filters == "all")
    return true;

  for (const auto filter : str::split(filters, ",; ")) {
    if (filter == "all" || shader_key == filter ||
        shader_key.starts_with(filter))
      return true;
  }
  return false;
}

bool
D3D12DiagBindingRecipeCacheEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_BINDING_RECIPE_CACHE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
  return enabled;
}

bool
D3D12DiagDrawVisibilityEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_DRAW_VISIBILITY") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_DRAW_STATE_READBACK");
  return enabled;
}

bool
D3D12DiagCBVReadbackEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_CBV_READBACK") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_DRAW_STATE_READBACK");
  return enabled;
}

bool
D3D12DiagRootCauseDenseEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE");
  return enabled;
}

bool
D3D12DiagCorrectnessDenseEnabled() {
  static const bool enabled =
      D3D12DiagRootCauseDenseEnabled() ||
      D3D12DiagEnabledEnv("DXMT_DIAG_GPU_HANG_DENSE");
  return enabled;
}

uint32_t
D3D12DiagRootCauseDenseMaxSlots() {
  static const uint32_t value = [] {
    const auto text = env::getEnvVar("DXMT_DIAG_ROOT_CAUSE_MAX_SLOTS");
    if (text.empty())
      return 65536u;
    char *end = nullptr;
    const auto parsed = std::strtoul(text.c_str(), &end, 10);
    return end == text.c_str()
               ? 65536u
               : static_cast<uint32_t>(
                     std::clamp<unsigned long>(parsed, 1024, 1048576));
  }();
  return value;
}

bool
D3D12ApitraceGpuCbvSnapshotEnabled() {
  return false;
}

bool
DiagIsTargetCompositePso(const std::string &key) {
  return (D3D12DiagDrawStateEnabled() || D3D12DiagDrawVisibilityEnabled() ||
          D3D12DiagCBVReadbackEnabled()) &&
         key == "bee0b7250cb8447bc326e2ceeedb2f7083216c10";
}

uint64_t
DiagCurrentReplayRecordSequence() {
  if (g_diag_replay_record_sequence)
    return g_diag_replay_record_sequence;
  return dxmt::apitrace::current_d3d_sequence();
}

uint64_t
DiagCurrentReplayRecordSerial() {
  return g_diag_replay_record_serial;
}

DiagReplayRecordScope::DiagReplayRecordScope(uint64_t sequence, uint64_t serial)
    : old_sequence(g_diag_replay_record_sequence),
      old_serial(g_diag_replay_record_serial) {
  g_diag_replay_record_sequence = sequence;
  g_diag_replay_record_serial = serial;
}

DiagReplayRecordScope::~DiagReplayRecordScope() {
  g_diag_replay_record_sequence = old_sequence;
  g_diag_replay_record_serial = old_serial;
}

bool
D3D12DiagExecuteIndirectEnabled() {
  static const bool enabled =
      D3D12DiagEnabledEnv("DXMT_DIAG_EXECUTE_INDIRECT") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE") ||
      D3D12DiagEnabledEnv("DXMT_DIAG_ROOT_CAUSE_DENSE");
  return enabled;
}

uint32_t
D3D12DiagIAReadbackBytes() {
  static const uint32_t size = []() {
    auto value = env::getEnvVar("DXMT_DIAG_IA_READBACK_BYTES");
    if (value.empty())
      return 256u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 256u;
    return static_cast<uint32_t>(
        std::clamp<unsigned long>(parsed, 16, 4096));
  }();
  return size;
}

uint32_t
D3D12DiagCBVReadbackBytes() {
  static const uint32_t size = []() {
    auto value = env::getEnvVar("DXMT_DIAG_CBV_READBACK_BYTES");
    if (value.empty())
      return 256u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 256u;
    return static_cast<uint32_t>(
        std::clamp<unsigned long>(parsed, 16, 4096));
  }();
  return size;
}

bool
D3D12DiagShouldLog(std::atomic<uint32_t> &counter, bool enabled) {
  if (!enabled)
    return false;
  const auto occurrence = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return occurrence <= 16 || (occurrence & (occurrence - 1)) == 0;
}

bool
D3D12DiagRootCauseTargetMatches(const std::string &key) {
  static const std::vector<std::string> filters = [] {
    auto configured = env::getEnvVar("DXMT_DIAG_ROOT_CAUSE_TARGET_PSO");
    if (configured.empty())
      configured = env::getEnvVar("DXMT_DUMP_PIPELINE_KEY");

    std::vector<std::string> parsed;
    for (const auto filter : str::split(configured, ",; "))
      if (!filter.empty())
        parsed.emplace_back(filter);
    return parsed;
  }();
  if (filters.empty())
    return false;
  for (const auto &filter : filters)
    if (!filter.empty() && key.starts_with(filter))
      return true;
  return false;
}

uint32_t
D3D12DiagRootCauseTargetSampleLimit() {
  static const uint32_t limit = [] {
    const auto value = env::getEnvVar("DXMT_DIAG_ROOT_CAUSE_TARGET_SAMPLES");
    if (value.empty())
      return 8u;
    char *end = nullptr;
    const auto parsed = std::strtoul(value.c_str(), &end, 10);
    return end == value.c_str()
               ? 8u
               : static_cast<uint32_t>(
                     std::clamp<unsigned long>(parsed, 1, 64));
  }();
  return limit;
}

ReplayRttiCounters &replayRttiCounters() {
  static thread_local ReplayRttiCounters c;
  return c;
}

bool ReplayBreakdownEnabled() {
  static const bool on = D3D12DiagEnabledEnv("DXMT_DIAG_REPLAY_BREAKDOWN");
  return on;
}

bool ReplayPerfEnabled() {
  return ReplayBreakdownEnabled() || dxmt::perf::enabled();
}

bool DescAccessCacheEnabled() {
  static const bool on = env::getEnvVar("DXMT_DESCACCESS_CACHE") != "0";
  return on;
}

bool DescAccessVerify() {
  static const bool on = env::getEnvVar("DXMT_DESCACCESS_VERIFY") == "1";
  return on;
}

} // namespace dxmt::d3d12
