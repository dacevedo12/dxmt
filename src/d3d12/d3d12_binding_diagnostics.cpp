#include "d3d12_binding_diagnostics.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_shader_binding.hpp"
#include "dxmt_bindless_buffer_table.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "util_env.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {

namespace {

constexpr double kNanosecondsPerMillisecond = 1000000.0;

/** Fallback dump root when neither DXMT_DUMP_PATH nor DXMT_LOG_PATH is set. */
constexpr const char *kBindlessShaderDumpFallbackDir =
    "/tmp/dxmt-bindless-shaders";

/** Sub-directory used when a dump/log root is configured. */
constexpr const char *kBindlessShaderDumpSubdir = "bindless-shaders";

} // namespace

DescriptorTableBindingRecipeDiagStats &
BindingRecipeDiagStats() {
  static DescriptorTableBindingRecipeDiagStats stats;
  return stats;
}

uint64_t
BindingRecipeDiagNowNs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

double
BindingRecipeDiagNsToMs(uint64_t ns) {
  return static_cast<double>(ns) / kNanosecondsPerMillisecond;
}

double
BindingRecipeDiagAvgNs(uint64_t ns, uint64_t count) {
  return count ? static_cast<double>(ns) / static_cast<double>(count) : 0.0;
}

const char *
BindlessMirrorDiagPathName(bool bindless_bound, bool snapshot) {
  if (bindless_bound)
    return snapshot ? "bindless-snapshot" : "bindless-live";
  return "bindless-unbound";
}

const std::filesystem::path &
BindlessMirrorDiagShaderDumpDir() {
  static const std::filesystem::path path = []() {
    auto root = env::getEnvVar("DXMT_DUMP_PATH");
    if (root.empty())
      root = env::getEnvVar("DXMT_LOG_PATH");
    const std::filesystem::path dump_dir =
        root.empty() || root == "none"
            ? std::filesystem::path(kBindlessShaderDumpFallbackDir)
            : std::filesystem::path(root) / kBindlessShaderDumpSubdir;
    try {
      std::filesystem::create_directories(dump_dir);
    } catch (...) {
    }
    return dump_dir;
  }();
  return path;
}

std::string
BindlessMirrorDiagShaderDumpKey(const char *stage, std::string_view sha1) {
  std::string key;
  key.reserve(std::strlen(stage) + 1 + sha1.size());
  key.append(stage);
  key.push_back(':');
  key.append(sha1);
  return key;
}

std::string
BindlessMirrorShaderSha1(const PipelineDxilShader &shader) {
  const auto &bc = shader.bytecode();
  return Sha1HashState::compute(bc.data(), bc.size()).string();
}

bool
BindlessMirrorDiagEnabled() {
  return D3D12DiagBindingsEnabled();
}

bool
BindlessMirrorDiagShouldLog() {
  static std::atomic<uint32_t> count = 0;
  if (!BindlessMirrorDiagEnabled())
    return false;
  const auto occurrence = count.fetch_add(1, std::memory_order_relaxed) + 1;
  return occurrence <= 16 || (occurrence & (occurrence - 1)) == 0;
}

BindlessMirrorDiagStats &
BindlessMirrorDiagStatsInstance() {
  static BindlessMirrorDiagStats stats;
  return stats;
}

void
DumpBindlessMirrorShaderDxbc(const char *stage,
                             const PipelineDxilShader &shader) {
  if (!BindlessMirrorDiagEnabled())
    return;
  const auto &bytecode = shader.bytecode();
  if (bytecode.empty())
    return;

  const auto sha1 = BindlessMirrorShaderSha1(shader);
  const auto key = BindlessMirrorDiagShaderDumpKey(stage, sha1);
  auto &stats = BindlessMirrorDiagStatsInstance();
  {
    std::lock_guard lock(stats.mutex);
    if (!stats.dumped_shader_blobs.insert(key).second)
      return;
  }

  try {
    std::filesystem::create_directories(BindlessMirrorDiagShaderDumpDir());
    const auto path = BindlessMirrorDiagShaderDumpDir() /
                      (std::string(stage) + "_" + sha1 + ".dxbc");
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
      return;
    file.write(reinterpret_cast<const char *>(bytecode.data()),
               static_cast<std::streamsize>(bytecode.size()));
    file.flush();
  } catch (...) {
  }
}

void
LogBindlessMirrorDiagSummary(const char *reason) {
  if (!BindlessMirrorDiagEnabled())
    return;

  if (std::strcmp(reason, "present") == 0) {
    static std::atomic<uint32_t> present_count = 0;
    const auto occurrence =
        present_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (occurrence > 4 && (occurrence & (occurrence - 1)) != 0)
      return;
  }

  auto &stats = BindlessMirrorDiagStatsInstance();
  const auto total =
      stats.total_graphics_draws.load(std::memory_order_relaxed);
  const auto dispatches =
      stats.compute_dispatches.load(std::memory_order_relaxed);
  if (!total && !dispatches)
    return;

  INFO("DXMT bindless-mirror DIAG summary"
       " reason=", reason,
       " totalGraphicsDraws=", total,
       " computeDispatches=", dispatches,
       " bindlessBound=", stats.bindless_bound.load(std::memory_order_relaxed),
       " usesBindlessMirror=",
       stats.uses_bindless_mirror.load(std::memory_order_relaxed),
       " mismatch=", stats.mismatch.load(std::memory_order_relaxed),
       " psTexNull=", stats.ps_tex_null.load(std::memory_order_relaxed),
       " vsTexNull=", stats.vs_tex_null.load(std::memory_order_relaxed),
       " bufRangeBaseNull=",
       stats.buf_table_null.load(std::memory_order_relaxed),
       " texturePayloadMismatch=",
       stats.texture_payload_mismatch.load(std::memory_order_relaxed),
       " textureSourceMissing=",
       stats.texture_source_missing.load(std::memory_order_relaxed),
       " textureWindowMissing=",
       stats.texture_window_missing.load(std::memory_order_relaxed),
       " samplerPayloadMismatch=",
       stats.sampler_payload_mismatch.load(std::memory_order_relaxed),
       " samplerSourceMissing=",
       stats.sampler_source_missing.load(std::memory_order_relaxed),
       " samplerWindowMissing=",
       stats.sampler_window_missing.load(std::memory_order_relaxed),
       " rootOffsetMissing=",
       stats.root_offset_missing.load(std::memory_order_relaxed));

  if (std::strcmp(reason, "command-queue-destroy") != 0)
    return;

  std::vector<std::pair<std::string, uint64_t>> pairs;
  {
    std::lock_guard lock(stats.mutex);
    pairs.reserve(stats.bindless_shader_pairs.size());
    for (const auto &entry : stats.bindless_shader_pairs)
      pairs.emplace_back(entry.first, entry.second);
  }
  std::sort(pairs.begin(), pairs.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.second != rhs.second)
                return lhs.second > rhs.second;
              return lhs.first < rhs.first;
            });

  for (const auto &entry : pairs) {
    const auto sep = entry.first.find(':');
    const auto vs_sha1 = entry.first.substr(0, sep);
    const auto ps_sha1 = sep == std::string::npos ? std::string()
                                                  : entry.first.substr(sep + 1);
    INFO("DXMT bindless-mirror DIAG shader-pair"
         " samples=", entry.second,
         " vs=", vs_sha1,
         " ps=", ps_sha1);
  }
}

void
RecordBindlessMirrorDiagDraw(uint64_t frame_seq, const PipelineState *pipeline,
                             const BindlessMirrorDrawDiag &diag) {
  if (!BindlessMirrorDiagEnabled())
    return;

  auto &stats = BindlessMirrorDiagStatsInstance();
  const auto draw =
      stats.total_graphics_draws.fetch_add(1, std::memory_order_relaxed) + 1;
  if (diag.bindless_bound)
    stats.bindless_bound.fetch_add(1, std::memory_order_relaxed);
  if (diag.uses_bindless_mirror)
    stats.uses_bindless_mirror.fetch_add(1, std::memory_order_relaxed);
  const bool mismatch = diag.uses_bindless_mirror != diag.bindless_bound;
  if (mismatch)
    stats.mismatch.fetch_add(1, std::memory_order_relaxed);
  stats.ps_tex_null.fetch_add(diag.ps_tex_null, std::memory_order_relaxed);
  stats.vs_tex_null.fetch_add(diag.vs_tex_null, std::memory_order_relaxed);
  stats.buf_table_null.fetch_add(diag.buf_table_null_gpu_addr,
                                 std::memory_order_relaxed);
  stats.texture_payload_mismatch.fetch_add(
      diag.texture_payload_mismatch, std::memory_order_relaxed);
  stats.texture_source_missing.fetch_add(
      diag.texture_source_missing, std::memory_order_relaxed);
  stats.texture_window_missing.fetch_add(
      diag.texture_window_missing, std::memory_order_relaxed);
  stats.sampler_payload_mismatch.fetch_add(
      diag.sampler_payload_mismatch, std::memory_order_relaxed);
  stats.sampler_source_missing.fetch_add(
      diag.sampler_source_missing, std::memory_order_relaxed);
  stats.sampler_window_missing.fetch_add(
      diag.sampler_window_missing, std::memory_order_relaxed);
  stats.root_offset_missing.fetch_add(diag.root_offset_missing,
                                      std::memory_order_relaxed);

  const bool sample = BindlessMirrorDiagShouldLog();
  if (!sample)
    return;

  std::string vs_sha1;
  std::string ps_sha1;
  if (pipeline) {
    const auto *vs_shader =
        FindShaderForStage(*pipeline, PipelineStage::Vertex);
    const auto *ps_shader =
        FindShaderForStage(*pipeline, PipelineStage::Pixel);
    vs_sha1 = vs_shader ? BindlessMirrorShaderSha1(*vs_shader) : "";
    ps_sha1 = ps_shader ? BindlessMirrorShaderSha1(*ps_shader) : "";
    if (diag.uses_bindless_mirror && (vs_shader || ps_shader)) {
      if (vs_shader)
        DumpBindlessMirrorShaderDxbc("vs", *vs_shader);
      if (ps_shader)
        DumpBindlessMirrorShaderDxbc("ps", *ps_shader);
      std::lock_guard lock(stats.mutex);
      stats.bindless_shader_pairs[vs_sha1 + ":" + ps_sha1]++;
    }
  }

  INFO("DXMT bindless-mirror DIAG draw"
       " draw=", draw,
       " frame=", frame_seq,
       " d3dSeq=", DiagCurrentReplayRecordSequence(),
       " serial=", DiagCurrentReplayRecordSerial(),
       " vs=", vs_sha1,
       " ps=", ps_sha1,
       " bindingGeneration=", diag.binding_generation,
       " descriptorRevision=", diag.descriptor_revision.epoch, ":",
       diag.descriptor_revision.sequence,
       " bindingFingerprint=0x", std::hex, diag.binding_fingerprint,
       std::dec,
       " psoBindless=", diag.uses_bindless_mirror ? 1 : 0,
       " path=", diag.path,
       mismatch ? " MISMATCH" : "",
       " psTex=", diag.ps_tex,
       " vsTex=", diag.vs_tex,
       " texNull=", diag.tex_null,
       " bufRangeBases=", diag.buf_table_entries,
       " bufRangeBaseNullGpuAddr=", diag.buf_table_null_gpu_addr,
       " texturePayloadMismatch=", diag.texture_payload_mismatch,
       " textureSourceMissing=", diag.texture_source_missing,
       " textureWindowMissing=", diag.texture_window_missing,
       " samplerPayloadMismatch=", diag.sampler_payload_mismatch,
       " samplerSourceMissing=", diag.sampler_source_missing,
       " samplerWindowMissing=", diag.sampler_window_missing,
       " rootOffsetMissing=", diag.root_offset_missing,
       " exampleBufStage=",
       diag.buf_table_null_gpu_addr
           ? PipelineStageName(diag.example_buf_stage)
           : "none",
       " exampleBufQword=",
       diag.buf_table_null_gpu_addr ? diag.example_buf_qword : UINT32_MAX);
}

void
RecordBindlessMirrorDiagDispatch() {
  if (!BindlessMirrorDiagEnabled())
    return;
  BindlessMirrorDiagStatsInstance().compute_dispatches.fetch_add(
      1, std::memory_order_relaxed);
}

void
AddBindlessMirrorDiagBufTable(
    BindlessMirrorDrawDiag &diag, PipelineStage stage,
    const MTL_SM50_SHADER_ARGUMENT *cbuffers, uint32_t num_cbuffers,
    const MTL_SM50_SHADER_ARGUMENT *arguments, uint32_t num_arguments,
    const AllocatedArgumentBufferSlice &slice) {
  if (!BindlessMirrorDiagEnabled() || !slice.mapped || !slice.length)
    return;
  if ((num_cbuffers && !cbuffers) || (num_arguments && !arguments))
    return;

  const auto qword_count = uint32_t(slice.length / sizeof(uint64_t));
  const auto *qwords = static_cast<const uint64_t *>(slice.mapped);
  ForEachBufferTableField(
      cbuffers, num_cbuffers, arguments, num_arguments,
      [&](const MTL_SM50_SHADER_ARGUMENT &arg, uint32_t compact_base) {
    if (compact_base + 1 >= qword_count)
      return;
    diag.buf_table_entries++;
    if (qwords[compact_base] != 0)
      return;
    if (!diag.buf_table_null_gpu_addr) {
      diag.example_buf_stage = stage;
      diag.example_buf_qword = compact_base;
    }
    diag.buf_table_null_gpu_addr++;
  });
}

} // namespace dxmt::d3d12
