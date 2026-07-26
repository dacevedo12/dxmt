#pragma once

// Descriptor-heap and descriptor-table lookups shared by the replay binding
// paths.
//
// These helpers used to live inside CommandQueueImpl
// (d3d12_command_queue_descriptor_binding.inc). Every one of them resolves a
// descriptor purely from the heap object plus the handle/offsets handed to it,
// so none of them touch the command queue instance and the whole set can be
// compiled and analysed on its own.

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_root_signature.hpp"
#include "log/log.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Binding plans are immutable functions of a root signature and pipeline.
// Stable object identities make cross-submit reuse ABA-safe; keep a bounded
// working set and evict one cold/arbitrary entry only when the cap is hit.
inline constexpr size_t kSubmissionPlanCacheLimit = 16384;

// Cache key for a per-(root signature, pipeline, stage) submission plan.
[[nodiscard]] uint64_t HashSubmissionPlanIdentity(uint64_t root_identity,
                                                  uint64_t pipeline_identity,
                                                  PipelineStage stage,
                                                  bool compute);

// Descriptor offset of `range` inside its table: the explicit
// offset_in_descriptors_from_table_start, or the running append offset when the
// range was declared as D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND (UINT_MAX).
[[nodiscard]] UINT DescriptorRangeOffset(const RootSignatureRange &range,
                                         UINT running_offset);

// Short diagnostic label for a descriptor range type.
[[nodiscard]] const char *
DescriptorRangeTypeName(D3D12_DESCRIPTOR_RANGE_TYPE range_type);

// Shader-read resource state appropriate for `stage`.
[[nodiscard]] D3D12_RESOURCE_STATES
ShaderReadStateForStage(PipelineStage stage);

// Byte size described by a descriptor record: the CBV size for constant buffer
// views, otherwise the backing resource width. 0 when unknown.
[[nodiscard]] UINT64
DescriptorRecordSizeBytes(const DescriptorRecord &descriptor);

// Resolves `handle` against `descriptor_heap`, warning and returning nullopt
// when the handle does not belong to the heap or the slot is not a
// shader-visible descriptor of `heap_type`. Takes the heap's mirror lock while
// reading the record.
[[nodiscard]] std::optional<DescriptorRecord>
GetBoundDescriptorRecordFromHeap(DescriptorHeap *descriptor_heap,
                                 D3D12_GPU_DESCRIPTOR_HANDLE handle,
                                 D3D12_DESCRIPTOR_HEAP_TYPE heap_type);

// Same as above for the descriptor at `range_offset + descriptor_index` inside
// the table starting at `base`. A non-zero `descriptor_count` bounds the index.
[[nodiscard]] std::optional<DescriptorRecord>
GetBoundDescriptorRecordInRangeFromHeap(DescriptorHeap *descriptor_heap,
                                        D3D12_GPU_DESCRIPTOR_HANDLE base,
                                        UINT range_offset,
                                        UINT descriptor_index,
                                        UINT descriptor_count,
                                        D3D12_DESCRIPTOR_HEAP_TYPE heap_type);

// Borrowing, silent variant for callers that already hold the heap's mirror
// lock. Returns nullptr instead of warning.
[[nodiscard]] const DescriptorRecord *
GetBoundDescriptorRecordInRangeFromLockedHeap(
    DescriptorHeap *descriptor_heap, D3D12_GPU_DESCRIPTOR_HANDLE base,
    UINT range_offset, UINT descriptor_index, UINT descriptor_count,
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type);

// The descriptor heap of `heap_type` currently bound in `state`, or nullptr
// (with a warning) when no such heap is bound.
template <typename State>
[[nodiscard]] DescriptorHeap *
GetBoundDescriptorHeap(const State &state,
                       D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
  const auto &heap = heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                         ? state.sampler_heap
                         : state.cbv_srv_uav_heap;
  auto *descriptor_heap = dynamic_cast<DescriptorHeap *>(heap.ptr());
  if (!descriptor_heap) {
    WARN("D3D12CommandQueue: GPU descriptor handle used without bound heap type=",
         uint32_t(heap_type));
    return nullptr;
  }
  return descriptor_heap;
}

// Base GPU handle of the descriptor table bound at `root_parameter_index`.
// Works over live replay state, compiled packet state and frozen snapshots.
template <typename State>
[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE
GetTableHandle(const State &state, bool compute, UINT root_parameter_index) {
  if constexpr (requires { state.compute_tables; }) {
    const auto &tables = compute ? state.compute_tables : state.graphics_tables;
    return root_parameter_index < tables.size()
               ? tables[root_parameter_index]
               : D3D12_GPU_DESCRIPTOR_HANDLE{};
  } else if constexpr (requires { state.compiled_root_tables; }) {
    (void)compute;
    for (const auto &table : state.compiled_root_tables) {
      if (table.root_parameter_index == root_parameter_index)
        return table.base_descriptor;
    }
    // Replay bindless snapshots store table bases in legacy_identity; the
    // compiled_root_tables vector is only filled for close-time packets.
    // Without this fallback, BuildBindlessRootOffsets sees empty tables and
    // emits MissingTable root-offset gaps (GPU hang risk).
    if constexpr (requires { state.legacy_identity; }) {
      if (state.legacy_identity &&
          root_parameter_index <
              state.legacy_identity->graphics_tables.size())
        return state.legacy_identity->graphics_tables[root_parameter_index];
    }
    return {};
  } else {
    (void)compute;
    const auto &tables = state.graphics_tables;
    return root_parameter_index < tables.size()
               ? tables[root_parameter_index]
               : D3D12_GPU_DESCRIPTOR_HANDLE{};
  }
}

} // namespace dxmt::d3d12
