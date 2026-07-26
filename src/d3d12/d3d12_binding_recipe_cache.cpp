#include "d3d12_binding_recipe_cache.hpp"

#include "Metal.hpp"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "dxmt_shader_cache.hpp"
#include "log/log.hpp"
#include "util_env.hpp"

#include <cstring>
#include <string_view>
#include <vector>

namespace dxmt::d3d12 {

// Reflected descriptor ranges are bounded per stage and there are at most a
// handful of stages, so a legitimate recipe stays orders of magnitude below
// this. It exists purely to keep a corrupted file from sizing an allocation.
namespace {
constexpr uint32_t kMaxCachedRecipeEntries = 1u << 16;
} // namespace

namespace {

// The first few calls are logged unconditionally so a short run still emits a
// summary; after that one summary per interval keeps the log bounded.
constexpr uint64_t kBindingRecipeDiagWarmupCalls = 16;
constexpr uint64_t kBindingRecipeDiagSummaryInterval = 65536;

} // namespace

void
LogBindingRecipeDiagSummary(const char *reason) {
  if (!D3D12DiagBindingRecipeCacheEnabled())
    return;

  auto &stats = BindingRecipeDiagStats();
  const auto get_calls = stats.get_calls.load(std::memory_order_relaxed);
  const auto apply_calls = stats.apply_calls.load(std::memory_order_relaxed);
  if (!get_calls && !apply_calls)
    return;

  const auto get_ns = stats.get_ns.load(std::memory_order_relaxed);
  const auto process_hits = stats.process_hits.load(std::memory_order_relaxed);
  const auto process_hit_ns =
      stats.process_hit_ns.load(std::memory_order_relaxed);
  const auto process_misses =
      stats.process_misses.load(std::memory_order_relaxed);
  const auto process_miss_ns =
      stats.process_miss_ns.load(std::memory_order_relaxed);
  const auto db_load_calls =
      stats.db_load_calls.load(std::memory_order_relaxed);
  const auto db_load_hits =
      stats.db_load_hits.load(std::memory_order_relaxed);
  const auto db_load_misses =
      stats.db_load_misses.load(std::memory_order_relaxed);
  const auto db_load_ns = stats.db_load_ns.load(std::memory_order_relaxed);
  const auto build_calls = stats.build_calls.load(std::memory_order_relaxed);
  const auto build_entries =
      stats.build_entries.load(std::memory_order_relaxed);
  const auto build_ns = stats.build_ns.load(std::memory_order_relaxed);
  const auto store_calls = stats.store_calls.load(std::memory_order_relaxed);
  const auto store_entries =
      stats.store_entries.load(std::memory_order_relaxed);
  const auto store_bytes = stats.store_bytes.load(std::memory_order_relaxed);
  const auto store_ns = stats.store_ns.load(std::memory_order_relaxed);
  const auto apply_entries =
      stats.apply_entries.load(std::memory_order_relaxed);
  const auto apply_bound = stats.apply_bound.load(std::memory_order_relaxed);
  const auto apply_cleared =
      stats.apply_cleared.load(std::memory_order_relaxed);
  const auto apply_missing_table =
      stats.apply_missing_table.load(std::memory_order_relaxed);
  const auto apply_ns = stats.apply_ns.load(std::memory_order_relaxed);
  const auto gate_generation_hits =
      stats.gate_generation_hits.load(std::memory_order_relaxed);
  const auto gate_fingerprint_hits =
      stats.gate_fingerprint_hits.load(std::memory_order_relaxed);
  const auto gate_misses = stats.gate_misses.load(std::memory_order_relaxed);

  INFO("D3D12 binding recipe diagnostic: summary"
       " reason=", reason,
       " getCalls=", get_calls,
       " getMs=", BindingRecipeDiagNsToMs(get_ns),
       " getAvgNs=", BindingRecipeDiagAvgNs(get_ns, get_calls),
       " processHits=", process_hits,
       " processHitMs=", BindingRecipeDiagNsToMs(process_hit_ns),
       " processHitAvgNs=", BindingRecipeDiagAvgNs(process_hit_ns, process_hits),
       " processMisses=", process_misses,
       " processMissMs=", BindingRecipeDiagNsToMs(process_miss_ns),
       " processMissAvgNs=", BindingRecipeDiagAvgNs(process_miss_ns, process_misses),
       " dbLoadCalls=", db_load_calls,
       " dbLoadHits=", db_load_hits,
       " dbLoadMisses=", db_load_misses,
       " dbLoadMs=", BindingRecipeDiagNsToMs(db_load_ns),
       " dbLoadAvgNs=", BindingRecipeDiagAvgNs(db_load_ns, db_load_calls),
       " buildCalls=", build_calls,
       " buildEntries=", build_entries,
       " buildMs=", BindingRecipeDiagNsToMs(build_ns),
       " buildAvgNs=", BindingRecipeDiagAvgNs(build_ns, build_calls),
       " storeCalls=", store_calls,
       " storeEntries=", store_entries,
       " storeBytes=", store_bytes,
       " storeMs=", BindingRecipeDiagNsToMs(store_ns),
       " storeAvgNs=", BindingRecipeDiagAvgNs(store_ns, store_calls),
       " applyCalls=", apply_calls,
       " applyEntries=", apply_entries,
       " applyBound=", apply_bound,
       " applyCleared=", apply_cleared,
       " applyMissingTable=", apply_missing_table,
       " applyMs=", BindingRecipeDiagNsToMs(apply_ns),
       " applyAvgNs=", BindingRecipeDiagAvgNs(apply_ns, apply_calls),
       " applyAvgEntryNs=", BindingRecipeDiagAvgNs(apply_ns, apply_entries),
       " gateGenHits=", gate_generation_hits,
       " gateFpHits=", gate_fingerprint_hits,
       " gateMisses=", gate_misses);
}

void
MaybeLogBindingRecipeDiagSummary(uint64_t calls, const char *reason) {
  if (!D3D12DiagBindingRecipeCacheEnabled())
    return;
  if (calls <= kBindingRecipeDiagWarmupCalls ||
      (calls % kBindingRecipeDiagSummaryInterval) == 0)
    LogBindingRecipeDiagSummary(reason);
}

std::string
BuildDescriptorTableBindingRecipeCachePath() {
  return dxmt::GetDXMTShaderCacheDirectory() + "d3d12_binding_recipes.db";
}

Sha1Digest
BuildDescriptorTableBindingRecipeKey(const PipelineState &pipeline,
                                     const RootSignature &root, bool compute) {
  Sha1HashState hash;
  static constexpr std::string_view prefix = "dxmt.d3d12.binding-recipe.v1";
  const auto &shader_key = pipeline.GetShaderCacheKey();
  const auto root_blob = root.GetSerializedBlob();
  hash.update(prefix.data(), prefix.size());
  hash.update(&compute, sizeof(compute));
  hash.update(shader_key.data(), shader_key.size());
  if (!root_blob.empty())
    hash.update(root_blob.data(), root_blob.size());
  return hash.final();
}

std::optional<DescriptorTableBindingRecipe>
LoadDescriptorTableBindingRecipe(const Sha1Digest &key) {
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0")
    return std::nullopt;
  auto reader = WMT::CacheReader::alloc_init(
      BuildDescriptorTableBindingRecipeCachePath().c_str(),
      kD3D12BindingRecipeCacheVersion);
  if (!reader)
    return std::nullopt;
  auto data = reader.get(key);
  if (!data)
    return std::nullopt;

  DescriptorTableBindingRecipeBlobHeader header = {};
  const auto header_size = uint64_t(sizeof(header));
  if (data.copy(&header, header_size) != header_size)
    return std::nullopt;
  if (header.magic != DescriptorTableBindingRecipeBlobHeader().magic ||
      header.version != DescriptorTableBindingRecipeBlobHeader().version ||
      header.entry_size != sizeof(DescriptorTableBindingRecipeEntry))
    return std::nullopt;

  // The blob is on-disk state and must be treated as untrusted input: a
  // truncated or corrupted cache file carries an arbitrary entry_count, and
  // sizing the buffer from it before validating lets that file drive a
  // multi-gigabyte allocation. The existing copy() check would still reject
  // the file, but only after the allocation already happened. Bound the count
  // first; a recipe describes reflected descriptor ranges across at most a
  // handful of shader stages, so anything near this ceiling is corruption.
  if (header.entry_count > kMaxCachedRecipeEntries)
    return std::nullopt;
  const uint64_t total_size =
      header_size + uint64_t(header.entry_count) * header.entry_size;
  std::vector<uint8_t> bytes(total_size);
  if (data.copy(bytes.data(), bytes.size()) != total_size)
    return std::nullopt;

  DescriptorTableBindingRecipe recipe = {};
  recipe.entries.resize(header.entry_count);
  if (header.entry_count)
    std::memcpy(recipe.entries.data(), bytes.data() + header_size,
                recipe.entries.size() * sizeof(recipe.entries[0]));
  return recipe;
}

void
StoreDescriptorTableBindingRecipe(const Sha1Digest &key,
                                  const DescriptorTableBindingRecipe &recipe) {
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0")
    return;
  auto writer = WMT::CacheWriter::alloc_init(
      BuildDescriptorTableBindingRecipeCachePath().c_str(),
      kD3D12BindingRecipeCacheVersion);
  if (!writer)
    return;

  DescriptorTableBindingRecipeBlobHeader header = {};
  header.entry_count = recipe.entries.size();
  std::vector<uint8_t> bytes(
      sizeof(header) + recipe.entries.size() * sizeof(recipe.entries[0]));
  std::memcpy(bytes.data(), &header, sizeof(header));
  if (!recipe.entries.empty())
    std::memcpy(bytes.data() + sizeof(header), recipe.entries.data(),
                recipe.entries.size() * sizeof(recipe.entries[0]));
  auto data = WMT::MakeDispatchData(bytes.data(), bytes.size());
  writer.set(key, data);
}

} // namespace dxmt::d3d12
