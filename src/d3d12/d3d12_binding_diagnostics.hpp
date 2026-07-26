#pragma once

// Instance-independent diagnostic value types and helpers for the D3D12
// binding path: the descriptor-table binding-recipe counters, the per-draw
// bindless-mirror counters, and the stateless shader-dump / formatting
// utilities. None of these touch the command queue instance.

#include "airconv_dx12_metal4.h"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_pipeline.hpp"
#include "dxmt_context.hpp"
#include "dxmt_descriptor_revision.hpp"

#include <atomic>
#include <cstdint>
#include <d3d12.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace dxmt::d3d12 {

/** Process-wide counters for the descriptor-table binding recipe cache. */
struct DescriptorTableBindingRecipeDiagStats {
  std::atomic<uint64_t> get_calls = 0;
  std::atomic<uint64_t> get_ns = 0;
  std::atomic<uint64_t> process_hits = 0;
  std::atomic<uint64_t> process_hit_ns = 0;
  std::atomic<uint64_t> process_misses = 0;
  std::atomic<uint64_t> process_miss_ns = 0;
  std::atomic<uint64_t> db_load_calls = 0;
  std::atomic<uint64_t> db_load_hits = 0;
  std::atomic<uint64_t> db_load_misses = 0;
  std::atomic<uint64_t> db_load_ns = 0;
  std::atomic<uint64_t> build_calls = 0;
  std::atomic<uint64_t> build_entries = 0;
  std::atomic<uint64_t> build_ns = 0;
  std::atomic<uint64_t> store_calls = 0;
  std::atomic<uint64_t> store_entries = 0;
  std::atomic<uint64_t> store_bytes = 0;
  std::atomic<uint64_t> store_ns = 0;
  std::atomic<uint64_t> apply_calls = 0;
  std::atomic<uint64_t> apply_entries = 0;
  std::atomic<uint64_t> apply_bound = 0;
  std::atomic<uint64_t> apply_cleared = 0;
  std::atomic<uint64_t> apply_missing_table = 0;
  std::atomic<uint64_t> apply_ns = 0;
  std::atomic<uint64_t> gate_generation_hits = 0;
  std::atomic<uint64_t> gate_fingerprint_hits = 0;
  std::atomic<uint64_t> gate_misses = 0;
};

[[nodiscard]] DescriptorTableBindingRecipeDiagStats &BindingRecipeDiagStats();

[[nodiscard]] uint64_t BindingRecipeDiagNowNs();

[[nodiscard]] double BindingRecipeDiagNsToMs(uint64_t ns);

[[nodiscard]] double BindingRecipeDiagAvgNs(uint64_t ns, uint64_t count);

/** Per-draw bindless-mirror observations aggregated into the diag summary. */
struct BindlessMirrorDrawDiag {
  const char *path = "bindless-unbound";
  bool uses_bindless_mirror = false;
  bool bindless_bound = false;
  uint32_t ps_tex = 0;
  uint32_t vs_tex = 0;
  uint32_t tex_null = 0;
  uint32_t ps_tex_null = 0;
  uint32_t vs_tex_null = 0;
  uint32_t buf_table_entries = 0;
  uint32_t buf_table_null_gpu_addr = 0;
  PipelineStage example_buf_stage = PipelineStage::Vertex;
  uint32_t example_buf_qword = 0;
  uint32_t texture_payload_mismatch = 0;
  uint32_t texture_source_missing = 0;
  uint32_t texture_window_missing = 0;
  uint32_t sampler_payload_mismatch = 0;
  uint32_t sampler_source_missing = 0;
  uint32_t sampler_window_missing = 0;
  uint32_t root_offset_missing = 0;
  uint64_t binding_generation = 0;
  dxmt::DescriptorContentRevision descriptor_revision = {};
  uint64_t binding_fingerprint = 0;
};

/** One descriptor slot under inspection by the bindless-window probes. */
struct BindlessMirrorDiagProbe {
  const char *path = nullptr;
  const PipelineState *pipeline = nullptr;
  const BindlessMirrorWindow *window = nullptr;
  PipelineStage stage = PipelineStage::Pixel;
  const DXMT12_MTL4_SHADER_ARGUMENT *arg = nullptr;
  UINT shader_register = 0;
  UINT lower_bound = 0;
  uint32_t root_offset = 0;
  uint32_t absolute_slot = 0;
  std::optional<DescriptorRecord> descriptor;
};

enum BindlessRootOffsetIssue : uint32_t {
  BindlessRootOffsetIssueMissingPlanCoverage = 1u << 0,
  BindlessRootOffsetIssueMissingTable = 1u << 1,
  BindlessRootOffsetIssueMissingDescriptor = 1u << 2,
  BindlessRootOffsetIssueMissingMirror = 1u << 3,
  BindlessRootOffsetIssueMissingWindow = 1u << 4,
  BindlessRootOffsetIssueMissingSnapshotEntry = 1u << 5,
};

[[nodiscard]] const char *BindlessMirrorDiagPathName(bool bindless_bound,
                                                     bool snapshot);

/** Directory receiving dumped DXBC blobs; created once on first use. */
[[nodiscard]] const std::filesystem::path &BindlessMirrorDiagShaderDumpDir();

/** Dedup key for an already dumped shader blob ("<stage>:<sha1>"). */
[[nodiscard]] std::string
BindlessMirrorDiagShaderDumpKey(const char *stage, std::string_view sha1);

[[nodiscard]] std::string
BindlessMirrorShaderSha1(const PipelineDxilShader &shader);

// --- bindless-mirror diagnostic aggregation --------------------------------

// Master gate for the bindless-mirror diagnostics (DXMT_DIAG_BINDINGS).
[[nodiscard]] bool BindlessMirrorDiagEnabled();

// Rate limiter shared by the per-draw and per-slot bindless-mirror log lines:
// the first 16 occurrences, then powers of two.
[[nodiscard]] bool BindlessMirrorDiagShouldLog();

/**
 * Process-wide bindless-mirror counters.
 *
 * The instance behind BindlessMirrorDiagStatsInstance() outlives every command
 * queue: shader-pair samples and the dumped-blob set must survive queue
 * teardown so the destroy-time summary can report the whole run.
 */
struct BindlessMirrorDiagStats {
  std::atomic<uint64_t> total_graphics_draws = 0;
  std::atomic<uint64_t> compute_dispatches = 0;
  std::atomic<uint64_t> bindless_bound = 0;
  std::atomic<uint64_t> uses_bindless_mirror = 0;
  std::atomic<uint64_t> mismatch = 0;
  std::atomic<uint64_t> ps_tex_null = 0;
  std::atomic<uint64_t> vs_tex_null = 0;
  std::atomic<uint64_t> buf_table_null = 0;
  std::atomic<uint64_t> texture_payload_mismatch = 0;
  std::atomic<uint64_t> texture_source_missing = 0;
  std::atomic<uint64_t> texture_window_missing = 0;
  std::atomic<uint64_t> sampler_payload_mismatch = 0;
  std::atomic<uint64_t> sampler_source_missing = 0;
  std::atomic<uint64_t> sampler_window_missing = 0;
  std::atomic<uint64_t> root_offset_missing = 0;
  std::mutex mutex;
  std::unordered_map<std::string, uint64_t> bindless_shader_pairs;
  std::unordered_set<std::string> dumped_shader_blobs;
};

[[nodiscard]] BindlessMirrorDiagStats &BindlessMirrorDiagStatsInstance();

// Writes `shader`'s DXBC blob to the dump directory once per (stage, sha1).
void DumpBindlessMirrorShaderDxbc(const char *stage,
                                  const PipelineDxilShader &shader);

// Emits the aggregate counters. "present" is throttled; the shader-pair
// breakdown is only printed for "command-queue-destroy".
void LogBindlessMirrorDiagSummary(const char *reason);

// Folds one draw's observations into the process counters and emits the
// sampled per-draw log line.
void RecordBindlessMirrorDiagDraw(uint64_t frame_seq,
                                  const PipelineState *pipeline,
                                  const BindlessMirrorDrawDiag &diag);

void RecordBindlessMirrorDiagDispatch();

// Counts the buffer-table qwords that carry a null GPU address for `stage`,
// recording the first offender in `diag`.
void AddBindlessMirrorDiagBufTable(
    BindlessMirrorDrawDiag &diag, PipelineStage stage,
    const MTL_SM50_SHADER_ARGUMENT *cbuffers, uint32_t num_cbuffers,
    const MTL_SM50_SHADER_ARGUMENT *arguments, uint32_t num_arguments,
    const AllocatedArgumentBufferSlice &slice);

} // namespace dxmt::d3d12
