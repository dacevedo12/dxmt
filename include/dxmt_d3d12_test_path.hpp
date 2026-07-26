#pragma once

#include <d3d12.h>

#include <cstdint>

namespace dxmt::d3d12::test {

// Test-only command-list controls carried through ID3D12Object private data.
// These controls are deliberately per-list so path tests do not depend on
// process-global environment variables or leak state into unrelated tests.
enum class ExecutionPathMode : std::uint32_t {
  // Normal compiler selection. Runtime packet fallbacks remain observable in
  // ExecutionPathStats.
  Auto = 0,
  // Every compilable Draw/DrawIndexed/Dispatch record must select a compiled
  // packet at Close. Copy, barrier, query, and similar commands select typed
  // nodes, and a queue-time compatibility packet is reported by
  // native_requirement_satisfied == 0 after completion.
  NativeCompiled = 1,
  // Compile every command into the compatibility node path. This exercises
  // the shared typed encoders without retaining the removed production
  // record-stream interpreter.
  CompatibilityCompiled = 2,
  // Source compatibility for older tests; the runtime semantics are now
  // CompatibilityCompiled rather than legacy record replay.
  Fallback = CompatibilityCompiled,
};

enum ExecutionPathFlags : std::uint32_t {
  ExecutionPathFlagNone = 0,
  ExecutionPathFlagInjectEmptyNativeSegment = 1u << 0,
  ExecutionPathFlagInjectEmptyFallbackSegment = 1u << 1,
};

// Ordered test-only view of the segments selected by the compiled command
// builder.  Keep this independent from the implementation enum so tests can
// inspect a stable ABI without including private command-list headers.
enum class ExecutionPathSegmentKind : std::uint32_t {
  Graphics = 0,
  Compute = 1,
  Fallback = 2,
  Typed = 3,
};

inline constexpr std::uint32_t kExecutionPathMaxTracedSegments = 64;

struct ExecutionPathConfig {
  std::uint32_t struct_size = sizeof(ExecutionPathConfig);
  ExecutionPathMode mode = ExecutionPathMode::Auto;
  std::uint32_t flags = ExecutionPathFlagNone;
};

struct ExecutionPathStats {
  std::uint32_t struct_size = sizeof(ExecutionPathStats);
  ExecutionPathMode mode = ExecutionPathMode::Auto;
  std::uint64_t command_list_generation = 0;
  std::uint32_t record_count = 0;
  std::uint32_t work_record_count = 0;
  std::uint32_t compiled_work_record_count = 0;
  std::uint32_t native_requirement_satisfied = 0;
  std::uint32_t graphics_segments = 0;
  std::uint32_t compute_segments = 0;
  std::uint32_t fallback_segments = 0;
  std::uint32_t empty_native_segments = 0;
  std::uint32_t empty_fallback_segments = 0;
  std::uint32_t selected_graphics_packets = 0;
  std::uint32_t selected_compute_packets = 0;
  std::uint32_t retained_graphics_packets = 0;
  std::uint32_t retained_compute_packets = 0;
  std::uint32_t has_native_root_base_buffer = 0;
  std::uint32_t replayed_graphics_packets = 0;
  std::uint32_t replayed_compute_packets = 0;
  std::uint32_t replayed_fallback_ranges = 0;
  std::uint32_t replayed_fallback_records = 0;
  std::uint32_t replayed_compiled_packet_fallbacks = 0;
  std::uint32_t replayed_empty_native_segments = 0;
  std::uint32_t replayed_empty_fallback_segments = 0;
  std::uint32_t submitted_graphics_packets = 0;
  std::uint32_t submitted_compute_packets = 0;
  std::uint32_t submission_prepare_failures = 0;
  std::uint32_t submitted_descriptor_snapshots = 0;
  std::uint32_t submitted_descriptor_entries = 0;
  std::uint32_t submitted_unique_descriptor_snapshots = 0;
  std::uint32_t submitted_unique_descriptor_records = 0;
  std::uint32_t submitted_descriptor_record_reuses = 0;
  std::uint32_t submitted_generation_shares = 0;
  std::uint32_t submitted_generation_deep_copies = 0;
  std::uint32_t unexpected_container_growths = 0;
  std::uint32_t storage_allocation_events = 0;
  std::uint32_t node_storage_allocation_events = 0;
  std::uint32_t state_storage_allocation_events = 0;
  std::uint32_t access_storage_allocation_events = 0;
  std::uint32_t immutable_state_reuses = 0;
  std::uint32_t state_delta_packets = 0;
  std::uint32_t zero_state_delta_packets = 0;
  std::uint32_t compiled_barrier_ranges = 0;
  std::uint32_t compiled_barriers = 0;
  std::uint32_t compiled_resource_state_deltas = 0;
  std::uint32_t selected_typed_nodes = 0;
  std::uint32_t replayed_compatibility_packets = 0;
  std::uint32_t legacy_replay_records = 0;
  std::uint32_t generic_descriptor_span_lookups = 0;
  std::uint32_t generic_descriptor_span_unique = 0;
  std::uint32_t generic_descriptor_span_reuses = 0;
  std::uint32_t frozen_native_direct_packets = 0;
  std::uint32_t frozen_range_lookups = 0;
  std::uint32_t frozen_range_unique = 0;
  std::uint32_t frozen_range_reuses = 0;
  std::uint32_t frozen_root_word_lookups = 0;
  std::uint32_t frozen_root_word_reuses = 0;
  std::uint32_t submission_graphics_encoder_boundary_merges = 0;
  std::uint32_t submission_compute_encoder_boundary_merges = 0;
  std::uint32_t submission_encoder_boundary_flushes = 0;
  std::uint32_t replayed_indirect_nodes = 0;
  std::uint32_t replayed_indirect_dependency_nodes = 0;
  std::uint32_t replayed_indirect_fallbacks = 0;
  std::uint32_t encoder_full_binding_programs = 0;
  std::uint32_t encoder_delta_binding_programs = 0;
  std::uint32_t encoder_binding_program_hits = 0;
  std::uint32_t encoder_native_binding_ops = 0;
  std::uint32_t encoder_native_binding_ops_skipped = 0;
  std::uint32_t encoder_resource_plan_publications = 0;
  std::uint32_t encoder_resource_plan_reuses = 0;
  std::uint32_t encoder_graph_node_count = 0;
  std::uint32_t graphics_encoder_node_count = 0;
  std::uint32_t compute_encoder_node_count = 0;
  std::uint32_t encoder_graph_elided_state_records = 0;
  std::uint32_t encoder_attachment_materializations = 0;
  std::uint32_t encoder_group_count = 0;
  std::uint32_t encoder_inlined_barrier_nodes = 0;
  std::uint32_t close_materialized_root_table_sets = 0;
  std::uint32_t submitted_root_table_fast_patches = 0;
  std::uint32_t submitted_root_table_full_materializations = 0;
  std::uint32_t submitted_native_binding_plan_reuses = 0;
  std::uint32_t submitted_native_binding_recipe_reuses = 0;
  std::uint32_t close_dynamic_render_state_recipes = 0;
  std::uint32_t close_vertex_binding_recipes = 0;
  std::uint32_t close_direct_access_plans = 0;
  std::uint32_t close_binding_programs = 0;
  std::uint32_t close_binding_program_reuses = 0;
  std::uint32_t close_full_binding_programs = 0;
  std::uint32_t close_delta_binding_programs = 0;
  // segment_count always reports the complete count. traced_segment_count is
  // capped at kExecutionPathMaxTracedSegments; the parallel arrays preserve
  // builder order and make N/F boundary topology directly testable.
  std::uint32_t segment_count = 0;
  std::uint32_t traced_segment_count = 0;
  ExecutionPathSegmentKind
      segment_kinds[kExecutionPathMaxTracedSegments] = {};
  std::uint32_t
      segment_first_record_indices[kExecutionPathMaxTracedSegments] = {};
  std::uint32_t segment_record_counts[kExecutionPathMaxTracedSegments] = {};
};

// Marks an already-published shader-visible descriptor slot as requiring one
// encode-time mirror repair. This lets the residency tests exercise the exact
// deferred typed-buffer path without relying on scheduling races.
struct DescriptorHeapSlotRepairConfig {
  std::uint32_t struct_size = sizeof(DescriptorHeapSlotRepairConfig);
  std::uint32_t slot = 0;
};

// Read-only device residency accounting exposed through ID3D12Device private
// data. The values are sampled under the device residency owner's lock.
struct PersistentResidencyStats {
  std::uint32_t struct_size = sizeof(PersistentResidencyStats);
  std::uint32_t entry_count = 0;
  std::uint64_t total_ref_count = 0;
  std::uint32_t pending_removal_count = 0;
  std::uint32_t cached_allocation_count = 0;
  std::uint64_t commit_count = 0;
};

// Read-only identity of the immutable native artifact backing a public PSO.
// Public ID3D12PipelineState objects remain distinct even when this value is
// shared. The address is diagnostic only and must never be dereferenced.
struct PipelineNativeArtifactIdentity {
  std::uint32_t struct_size = sizeof(PipelineNativeArtifactIdentity);
  std::uintptr_t artifact = 0;
};

struct PipelineNativeArtifactCacheStats {
  std::uint32_t struct_size = sizeof(PipelineNativeArtifactCacheStats);
  std::uint64_t hits = 0;
  std::uint64_t misses = 0;
  std::uint64_t waits = 0;
  std::uint64_t compiles = 0;
  std::uint64_t compile_failures = 0;
};

inline constexpr GUID kExecutionPathConfigGuid = {
    0x6ca960e8,
    0xe87f,
    0x45a8,
    {0x9f, 0x5e, 0x2a, 0x42, 0x2e, 0xf9, 0xc3, 0x8d}};

inline constexpr GUID kExecutionPathStatsGuid = {
    0x52cbcfab,
    0xf833,
    0x412e,
    {0x89, 0xcb, 0xd0, 0xa7, 0x8f, 0x35, 0xef, 0x1e}};

inline constexpr GUID kDescriptorHeapSlotRepairGuid = {
    0x6389fa37,
    0x0efe,
    0x4725,
    {0xab, 0xd4, 0xe5, 0xc7, 0xad, 0x78, 0x0e, 0xe8}};

inline constexpr GUID kPersistentResidencyStatsGuid = {
    0x16d2bca8,
    0x0347,
    0x407f,
    {0xaa, 0x72, 0x04, 0x5f, 0x65, 0xed, 0xb7, 0x90}};

inline constexpr GUID kPipelineNativeArtifactIdentityGuid = {
    0x5e486ca2,
    0x0362,
    0x4af9,
    {0x92, 0x94, 0x2d, 0x8c, 0x39, 0xe2, 0x25, 0x67}};

inline constexpr GUID kPipelineNativeArtifactCacheStatsGuid = {
    0x870fe3ef,
    0xe4c0,
    0x44e6,
    {0xa6, 0xdc, 0xf9, 0x71, 0x83, 0xb1, 0x48, 0x31}};

} // namespace dxmt::d3d12::test
