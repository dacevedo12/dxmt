#include "d3d12_table_recipe_lookup.hpp"

#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_binding_recipe_cache.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_stage_plan_build.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace dxmt::d3d12 {

const DescriptorTableBindingRecipe &
GetDescriptorTableBindingRecipe(const PipelineState &pipeline,
                                const RootSignature &root, bool compute) {
  const bool diag_enabled = D3D12DiagBindingRecipeCacheEnabled();
  const uint64_t get_start_ns = diag_enabled ? BindingRecipeDiagNowNs() : 0;
  struct CacheKey {
    Sha1Digest digest = {};
    bool operator==(const CacheKey &other) const {
      return digest == other.digest;
    }
  };
  struct CacheKeyHash {
    size_t operator()(const CacheKey &key) const {
      return std::hash<Sha1Digest>()(key.digest);
    }
  };

  static std::mutex mutex;
  static std::unordered_map<CacheKey,
                            std::unique_ptr<DescriptorTableBindingRecipe>,
                            CacheKeyHash>
      cache;

  CacheKey key = {
      BuildDescriptorTableBindingRecipeKey(pipeline, root, compute)};
  std::lock_guard lock(mutex);
  auto it = cache.find(key);
  if (it != cache.end()) {
    if (diag_enabled) {
      auto &stats = BindingRecipeDiagStats();
      const uint64_t elapsed = BindingRecipeDiagNowNs() - get_start_ns;
      const auto calls =
          stats.get_calls.fetch_add(1, std::memory_order_relaxed) + 1;
      stats.get_ns.fetch_add(elapsed, std::memory_order_relaxed);
      stats.process_hits.fetch_add(1, std::memory_order_relaxed);
      stats.process_hit_ns.fetch_add(elapsed, std::memory_order_relaxed);
      MaybeLogBindingRecipeDiagSummary(calls, "get-process-hit");
    }
    return *it->second;
  }

  const uint64_t load_start_ns = diag_enabled ? BindingRecipeDiagNowNs() : 0;
  auto loaded = LoadDescriptorTableBindingRecipe(key.digest);
  if (diag_enabled) {
    auto &stats = BindingRecipeDiagStats();
    stats.db_load_calls.fetch_add(1, std::memory_order_relaxed);
    if (loaded)
      stats.db_load_hits.fetch_add(1, std::memory_order_relaxed);
    else
      stats.db_load_misses.fetch_add(1, std::memory_order_relaxed);
    stats.db_load_ns.fetch_add(BindingRecipeDiagNowNs() - load_start_ns,
                               std::memory_order_relaxed);
  }

  uint64_t build_start_ns = 0;
  if (diag_enabled && !loaded)
    build_start_ns = BindingRecipeDiagNowNs();
  DescriptorTableBindingRecipe recipe =
      loaded ? std::move(*loaded)
             : BuildDescriptorTableBindingRecipe(pipeline, root, compute);
  if (diag_enabled && !loaded) {
    auto &stats = BindingRecipeDiagStats();
    stats.build_calls.fetch_add(1, std::memory_order_relaxed);
    stats.build_entries.fetch_add(recipe.entries.size(),
                                  std::memory_order_relaxed);
    stats.build_ns.fetch_add(BindingRecipeDiagNowNs() - build_start_ns,
                             std::memory_order_relaxed);
  }
  if (!loaded) {
    const uint64_t store_start_ns =
        diag_enabled ? BindingRecipeDiagNowNs() : 0;
    StoreDescriptorTableBindingRecipe(key.digest, recipe);
    if (diag_enabled) {
      auto &stats = BindingRecipeDiagStats();
      const auto bytes =
          sizeof(DescriptorTableBindingRecipeBlobHeader) +
          recipe.entries.size() * sizeof(DescriptorTableBindingRecipeEntry);
      stats.store_calls.fetch_add(1, std::memory_order_relaxed);
      stats.store_entries.fetch_add(recipe.entries.size(),
                                    std::memory_order_relaxed);
      stats.store_bytes.fetch_add(bytes, std::memory_order_relaxed);
      stats.store_ns.fetch_add(BindingRecipeDiagNowNs() - store_start_ns,
                               std::memory_order_relaxed);
    }
  }
  auto inserted = cache.insert(
      {key, std::make_unique<DescriptorTableBindingRecipe>(std::move(recipe))});
  if (diag_enabled) {
    auto &stats = BindingRecipeDiagStats();
    const uint64_t elapsed = BindingRecipeDiagNowNs() - get_start_ns;
    const auto calls =
        stats.get_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    stats.get_ns.fetch_add(elapsed, std::memory_order_relaxed);
    stats.process_misses.fetch_add(1, std::memory_order_relaxed);
    stats.process_miss_ns.fetch_add(elapsed, std::memory_order_relaxed);
    MaybeLogBindingRecipeDiagSummary(calls, loaded ? "get-db-hit"
                                                   : "get-build");
  }
  return *inserted.first->second;
}

} // namespace dxmt::d3d12
