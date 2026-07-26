#pragma once

// Interpreted replay of a descriptor-table binding recipe into the argument
// encoder, plus its binding-recipe diagnostic accounting.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_native_binding.inc). It stays a template because it runs
// over live replay state, compiled packet state and frozen snapshot identities
// alike; the only piece of the command queue it ever used was the Metal device
// handle, which is now passed in.

#include "Metal.hpp"
#include "d3d12_binding_debug_log.hpp"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_bindless_mirror_slot_fill.hpp"
#include "d3d12_binding_recipe_cache.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_legacy_binding_encode.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "dxmt_context.hpp"
#include "log/log.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Binds every entry of `recipe` from the tables and heaps named by `state`.
// Entries whose table or descriptor cannot be resolved are reported through the
// selected-binding diagnostic and cleared instead of bound.
template <typename State>
void ApplyDescriptorTableBindingRecipe(
    WMT::Device device, ArgumentEncodingContext &enc, const State &state,
    const PipelineState &pipeline, bool compute,
    const DescriptorTableBindingRecipe &recipe) {
  const bool diag_enabled = D3D12DiagBindingRecipeCacheEnabled();
  const uint64_t apply_start_ns = diag_enabled ? BindingRecipeDiagNowNs() : 0;
  uint64_t missing_tables = 0;
  uint64_t cleared = 0;
  uint64_t bound = 0;
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_MAX_ROOT_COST> table_cache = {};
  std::array<bool, D3D12_MAX_ROOT_COST> table_cached = {};
  DescriptorHeap *cbv_srv_uav_heap = nullptr;
  DescriptorHeap *sampler_heap = nullptr;
  bool cbv_srv_uav_heap_cached = false;
  bool sampler_heap_cached = false;

  auto get_table = [&](UINT root_index) {
    if (root_index < table_cache.size()) {
      if (!table_cached[root_index]) {
        table_cache[root_index] = GetTableHandle(state, compute, root_index);
        table_cached[root_index] = true;
      }
      return table_cache[root_index];
    }
    return GetTableHandle(state, compute, root_index);
  };

  auto get_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
    if (heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
      if (!sampler_heap_cached) {
        sampler_heap = GetBoundDescriptorHeap(state, heap_type);
        sampler_heap_cached = true;
      }
      return sampler_heap;
    }
    if (!cbv_srv_uav_heap_cached) {
      cbv_srv_uav_heap = GetBoundDescriptorHeap(state, heap_type);
      cbv_srv_uav_heap_cached = true;
    }
    return cbv_srv_uav_heap;
  };

  for (const auto &entry : recipe.entries) {
    const auto stage = static_cast<PipelineStage>(entry.stage);
    const auto base = get_table(entry.root_index);
    if (!base.ptr) {
      std::string shader_digest;
      if (D3D12DiagBindingsEnabled() && D3D12DiagShaderFilterConfigured() &&
          D3D12DiagPipelineStageSelected(pipeline, stage, &shader_digest)) {
        static std::atomic<uint32_t> missing_table_count = 0;
        if (D3D12DiagShouldLog(missing_table_count, true)) {
          WARN("D3D12 diagnostic: selected binding unresolved",
               " reason=missing-table",
               " pso=", pipeline.GetShaderCacheKey(),
               " shader=", shader_digest,
               " stage=", PipelineStageName(stage),
               " root=", entry.root_index,
               " range=", entry.range_index,
               " slot=", entry.slot,
               " register=", entry.shader_register,
               " lower=", entry.register_lower_bound,
               " descriptorIndex=", entry.descriptor_index,
               " descriptorCount=", entry.descriptor_count);
        }
      }
      missing_tables++;
      continue;
    }
    const auto range_type =
        static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(entry.range_type);
    const auto heap_type = DescriptorHeapTypeForRange(range_type);
    auto *heap = get_heap(heap_type);
    const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
        heap, base, entry.range_offset, entry.descriptor_index,
        entry.descriptor_count, heap_type);
    if (!descriptor) {
      std::string shader_digest;
      if (D3D12DiagBindingsEnabled() && D3D12DiagShaderFilterConfigured() &&
          D3D12DiagPipelineStageSelected(pipeline, stage, &shader_digest)) {
        static std::atomic<uint32_t> missing_descriptor_count = 0;
        if (D3D12DiagShouldLog(missing_descriptor_count, true)) {
          WARN("D3D12 diagnostic: selected binding unresolved",
               " reason=missing-descriptor",
               " pso=", pipeline.GetShaderCacheKey(),
               " shader=", shader_digest,
               " stage=", PipelineStageName(stage),
               " root=", entry.root_index,
               " range=", entry.range_index,
               " slot=", entry.slot,
               " register=", entry.shader_register,
               " lower=", entry.register_lower_bound,
               " table=", uint64_t(base.ptr),
               " heap=", static_cast<const void *>(heap),
               " descriptorIndex=", entry.descriptor_index,
               " descriptorCount=", entry.descriptor_count);
        }
      }
      ClearDescriptorBinding(enc, stage, range_type, entry.slot);
      cleared++;
      continue;
    }
    MaybeFillBindlessMirrorSlot(device, enc, range_type, *descriptor, stage,
                                &entry.argument);
    DebugLogRootBinding(
        DescriptorRangeTypeName(range_type), pipeline, compute, stage,
        entry.root_index, entry.slot, entry.shader_register,
        entry.argument.RegisterCount ? entry.argument.RegisterSpace : 0,
        DescriptorRecordSizeBytes(*descriptor), 0, &*descriptor,
        &entry.argument);
    BindDescriptor(device, enc, stage, range_type, entry.slot, *descriptor,
                   &entry.argument);
    bound++;
  }
  if (diag_enabled) {
    auto &stats = BindingRecipeDiagStats();
    const auto calls =
        stats.apply_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    stats.apply_entries.fetch_add(recipe.entries.size(),
                                  std::memory_order_relaxed);
    stats.apply_missing_table.fetch_add(missing_tables,
                                        std::memory_order_relaxed);
    stats.apply_cleared.fetch_add(cleared, std::memory_order_relaxed);
    stats.apply_bound.fetch_add(bound, std::memory_order_relaxed);
    stats.apply_ns.fetch_add(BindingRecipeDiagNowNs() - apply_start_ns,
                             std::memory_order_relaxed);
    MaybeLogBindingRecipeDiagSummary(calls, "apply");
  }
}

} // namespace dxmt::d3d12
