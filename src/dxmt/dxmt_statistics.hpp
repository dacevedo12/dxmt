#pragma once

#include "util_flags.hpp"
#include <array>
#include <chrono>

namespace dxmt {

using clock = std::chrono::high_resolution_clock;

enum class FeatureCompatibility {
    UnsupportedGeometryDraw,
    UnsupportedTessellationOutputPrimitive,
    UnsupportedIndirectTessellationDraw,
    UnsupportedGeometryTessellationDraw,
    UnsupportedDrawAuto,
    UnsupportedPredication,
    UnsupportedStreamOutputAppending,
    UnsupportedMultipleStreamOutput,
  };

enum class ScalerType {
  None,
  Spatial,
  Temporal,
};

enum class CompiledFallbackReason : uint32_t {
  LegacyPath,
  ResourceBarrier,
  GeometryOrTessellation,
  Indirect,
  MissingCompiledEncoder,
  UnsupportedPipeline,
  UnsupportedRootSignature,
  UnsupportedDescriptorTable,
  UnsupportedRootDescriptor,
  UnsupportedRootConstants,
  UnsupportedVertexIndexState,
  UnsupportedRenderTargetState,
  UnsupportedResourceAccess,
  UnsupportedArgumentTable,
  Residency,
  NativeUnsupportedRootSignature,
  NativeUnsupportedDescriptorRange,
  NativeDescriptorNullBase,
  NativeDescriptorMixedHeap,
  NativeDescriptorNoRange,
  NativeDescriptorInvalidHandle,
  NativeDescriptorHeapTail,
  NativeDescriptorAmbiguousRange,
  NativeDescriptorBackendGeneration,
  NativeUnsupportedRootDescriptor,
  NativeUnsupportedGeometryPipeline,
  NativeUnsupportedTessellationPipeline,
  NativeUnsupportedExecuteIndirect,
  NativeUnsupportedDynamicResource,
  NativeMissingDescriptorBackend,
  NativeShaderAbiMismatch,
  NativeResidencyUnsupported,
  Unknown,
  Count,
};

constexpr size_t kCompiledFallbackReasonCount =
    static_cast<size_t>(CompiledFallbackReason::Count);

// Exclusive CPU time for concrete D3D12 implementation paths. Nested scopes
// subtract their elapsed time from their parent, so every instruction executed
// below an instrumented entry point belongs to exactly one code path.
enum class PerfCodePath : uint32_t {
  CommandListCloseControl,
  CommandListCloseBuildCompiled,
  CommandListCloseAllocatorRelease,
  CommandListCloseApitrace,
  CommandListResetControl,
  CommandListResetAllocator,
  CommandListResetStateClear,
  CommandListResetInitialPipeline,
  CommandListResetApitrace,
  CommandListReferenceCount,
  CommandListObjectApi,
  CommandListFeatureSupport,
  CommandListDrawInstanced,
  CommandListDrawIndexedInstanced,
  CommandListDispatch,
  CommandListCopyBuffer,
  CommandListCopyTexture,
  CommandListCopyResource,
  CommandListCopyTiles,
  CommandListResolve,
  CommandListClearState,
  CommandListPipelineState,
  CommandListResourceBarrier,
  CommandListDescriptorHeaps,
  CommandListRootSignature,
  CommandListRootDescriptorTable,
  CommandListRootConstants,
  CommandListRootDescriptor,
  CommandListPrimitiveTopology,
  CommandListViewports,
  CommandListScissors,
  CommandListBlendFactor,
  CommandListStencilRef,
  CommandListExecuteBundle,
  CommandListIndexBuffer,
  CommandListVertexBuffers,
  CommandListStreamOutput,
  CommandListRenderTargets,
  CommandListClearDepthStencil,
  CommandListClearRenderTarget,
  CommandListClearUav,
  CommandListDiscard,
  CommandListQueryBeginEnd,
  CommandListQueryResolve,
  CommandListPredication,
  CommandListExecuteIndirect,
  CommandListMarkerEvent,
  CommandListDepthBounds,
  CommandListSamplePositions,
  CommandListViewInstanceMask,
  CommandListWriteBufferImmediate,
  CommandListProtectedSession,
  CommandListRenderPassBegin,
  CommandListRenderPassEnd,
  CommandListTemporalUpscale,
  CommandListUnsupportedFeature,
  CommandListSnapshotCapture,
  CommandListRecordVectorAppend,
  DeviceCreateDescriptorHeap,
  DeviceGetDescriptorHandleIncrementSize,
  DeviceCreateConstantBufferView,
  DeviceCreateShaderResourceView,
  DeviceCreateUnorderedAccessView,
  DeviceCreateRenderTargetView,
  DeviceCreateDepthStencilView,
  DeviceCreateSampler,
  DeviceCopyDescriptors,
  DeviceCopyDescriptorsSimple,
  DeviceCreateSamplerFeedbackUav,
  DescriptorHandleResolve,
  DescriptorLockAcquire,
  DescriptorRecordReset,
  DescriptorRecordCopy,
  DescriptorSlotBeginWrite,
  DescriptorRevisionCommit,
  DescriptorSamplerMirrorMaterialize,
  DescriptorTableMaterializeControl,
  DescriptorTableMirrorMutation,
  DescriptorTableTextureViewCreate,
  DescriptorTableBufferTextureView,
  DescriptorTableNativeBufferRecord,
  DescriptorTableResidencyUpdate,
  DescriptorTableDenseDiagnostic,
  QueueExecuteControl,
  QueueExecuteValidation,
  QueueExecuteApitrace,
  QueueExecuteCollect,
  QueueExecutePreparePlan,
  QueueExecuteCaptureDescriptors,
  QueueCaptureDescriptorCacheLookup,
  QueueCaptureDescriptorSnapshotBuild,
  QueueCaptureDescriptorJournalFinalize,
  QueueEnqueueControl,
  QueueEnqueueFrameTag,
  QueueEnqueueLock,
  QueueEnqueueNotify,
  CompiledBuildLoopDispatch,
  CompiledBuildPipelineMetadata,
  CompiledBuildPacketStateCopy,
  CompiledBuildRootTableMaterialize,
  CompiledBuildNativeBindingDispatch,
  CompiledBuildNativeStageCacheKey,
  CompiledBuildNativeStageCacheLookup,
  CompiledBuildNativeShaderLookup,
  CompiledBuildNativeRootBaseLayout,
  CompiledBuildNativeRootBaseRangeScan,
  CompiledBuildNativePayloadAppend,
  CompiledBuildNativeDiagnosticCopy,
  CompiledBuildNativeStageCacheStore,
  CompiledBuildSegmentAppend,
  CompiledBuildStateUpdate,
  CompiledBuildPayloadBufferCreate,
  CompiledBuildPayloadBufferCopy,
  CompiledBuildFallbackRewrite,
  Count,
};

constexpr size_t kPerfCodePathCount =
    static_cast<size_t>(PerfCodePath::Count);

struct ScalerInfo {
  ScalerType type = ScalerType::None;
  uint32_t input_width;
  uint32_t input_height;
  uint32_t output_width;
  uint32_t output_height;
  bool auto_exposure;
  bool motion_vector_highres;
};

struct FrameStatistics {
  clock::time_point begin_time{};
  Flags<FeatureCompatibility> compatibility_flags;
  uint32_t command_buffer_count = 0;
  uint32_t sync_count = 0;
  clock::duration sync_interval{};
  clock::duration commit_interval{};
  uint32_t render_pass_count = 0;
  uint32_t render_pass_optimized = 0;
  uint32_t render_command_count = 0;
  uint32_t render_pso_bind_count = 0;
  uint32_t render_draw_count = 0;
  uint32_t render_indexed_draw_count = 0;
  uint32_t render_indirect_draw_count = 0;
  uint32_t render_mesh_draw_count = 0;
  uint32_t render_tile_dispatch_count = 0;
  uint32_t render_color_load_count = 0;
  uint32_t render_color_store_count = 0;
  uint32_t render_depth_load_count = 0;
  uint32_t render_depth_store_count = 0;
  uint32_t render_stencil_load_count = 0;
  uint32_t render_stencil_store_count = 0;
  uint32_t render_attachment_count = 0;
  uint32_t render_attachment_reuse_count = 0;
  uint32_t render_attachment_reload_count = 0;
  uint32_t render_attachment_store_then_load_count = 0;
  uint32_t encoder_switch_render_compute_count = 0;
  uint32_t encoder_switch_compute_render_count = 0;
  uint32_t encoder_switch_to_other_count = 0;
  uint32_t present_pass_count = 0;
  uint32_t clear_pass_count = 0;
  uint32_t clear_pass_optimized = 0;
  uint32_t resolve_pass_optimized = 0;
  uint32_t compute_pass_count = 0;
  uint32_t compute_pass_optimized = 0;
  uint32_t compute_pass_with_dispatch_count = 0;
  uint32_t compute_pass_without_dispatch_count = 0;
  uint32_t compute_command_count = 0;
  uint32_t compute_pso_bind_count = 0;
  uint32_t compute_set_buffer_count = 0;
  uint32_t compute_set_texture_count = 0;
  uint32_t compute_set_bytes_count = 0;
  uint32_t compute_use_resource_count = 0;
  uint32_t compute_dispatch_count = 0;
  uint32_t compute_dispatch_indirect_count = 0;
  uint32_t compute_memory_barrier_count = 0;
  uint32_t compute_fence_wait_count = 0;
  uint32_t compute_fence_update_count = 0;
  uint32_t blit_pass_count = 0;
  uint32_t blit_pass_optimized = 0;
  uint32_t blit_pass_with_commands_count = 0;
  uint32_t blit_pass_empty_count = 0;
  uint32_t blit_pass_deferred_fence_only_count = 0;
  uint32_t blit_pass_merged_fence_only_count = 0;
  uint32_t blit_barrier_only_pass_count = 0;
  uint32_t blit_separator_pass_count = 0;
  uint32_t resource_barrier_batches_merged = 0;
  uint32_t resource_barrier_batches_graphics_inlined = 0;
  uint32_t resource_barrier_batches_compute_inlined = 0;
  uint32_t blit_pass_with_fence_wait_count = 0;
  uint32_t blit_pass_with_fence_update_count = 0;
  uint32_t blit_fence_wait_entry_count = 0;
  uint32_t blit_fence_update_entry_count = 0;
  uint32_t blit_command_count = 0;
  uint32_t blit_copy_buffer_to_buffer_count = 0;
  uint32_t blit_copy_buffer_to_texture_count = 0;
  uint32_t blit_copy_texture_to_buffer_count = 0;
  uint32_t blit_copy_texture_to_texture_count = 0;
  uint32_t blit_generate_mipmaps_count = 0;
  uint32_t blit_fence_wait_count = 0;
  uint32_t blit_fence_update_count = 0;
  uint32_t blit_fill_buffer_count = 0;
  uint32_t blit_resolve_counters_count = 0;
  uint32_t event_stall = 0;
  uint32_t latency = 0;
  uint32_t flush_chunk_count = 0;
  uint32_t flush_empty_chunk_count = 0;
  uint32_t flush_event_only_chunk_count = 0;
  uint32_t flush_signal_chunk_count = 0;
  uint32_t flush_wait_chunk_count = 0;
  uint32_t flush_present_chunk_count = 0;
  uint32_t flush_encoder_count = 0;
  uint32_t flush_encoded_encoder_count = 0;
  uint32_t flush_null_encoder_count = 0;
  uint32_t flush_render_encoder_count = 0;
  uint32_t flush_compute_encoder_count = 0;
  uint32_t flush_blit_encoder_count = 0;
  uint32_t flush_timestamp_encoder_count = 0;
  clock::duration flush_total_interval{};
  clock::duration flush_collect_interval{};
  clock::duration flush_relation_interval{};
  clock::duration flush_query_interval{};
  clock::duration flush_timestamp_interval{};
  clock::duration flush_render_interval{};
  clock::duration flush_compute_interval{};
  clock::duration flush_blit_interval{};
  clock::duration flush_clear_interval{};
  clock::duration flush_resolve_interval{};
  clock::duration flush_scaler_interval{};
  clock::duration flush_present_interval{};
  clock::duration flush_event_interval{};
  clock::duration flush_sample_interval{};
  clock::duration flush_pending_fence_interval{};
  clock::duration flush_signal_interval{};
  clock::duration flush_cleanup_interval{};
  clock::duration flush_max_chunk_interval{};
  uint32_t shader_binding_upload_count = 0;
  uint32_t shader_binding_dirty_cbuffer_count = 0;
  uint32_t shader_binding_dirty_sampler_count = 0;
  uint32_t shader_binding_dirty_srv_count = 0;
  uint32_t shader_binding_dirty_uav_count = 0;
  uint32_t shader_binding_clean_uav_count = 0;
  clock::duration encode_prepare_interval{};
  clock::duration encode_flush_interval{};
  clock::duration drawable_blocking_interval{};
  clock::duration present_latency_interval{};
  clock::duration frame_execute_command_lists_interval{};
  clock::duration frame_present_interval{};
  clock::duration frame_present_queue_wait_interval{};
  clock::duration frame_queue_signal_interval{};
  clock::duration frame_queue_wait_interval{};
  clock::duration frame_create_resource_interval{};
  clock::duration frame_create_reserved_resource_interval{};
  clock::duration frame_create_heap_interval{};
  clock::duration frame_create_pipeline_interval{};
  clock::duration frame_tile_mapping_interval{};
  clock::duration frame_cmdlist_record_interval{};
  clock::duration frame_execute_validate_interval{};
  clock::duration frame_execute_collect_interval{};
  clock::duration frame_execute_enqueue_interval{};
  clock::duration frame_execute_drain_interval{};
  clock::duration frame_execute_drain_lock_interval{};
  clock::duration frame_execute_resource_state_lock_hold_interval{};
  clock::duration frame_execute_replay_interval{};
  clock::duration frame_execute_decay_interval{};
  clock::duration frame_execute_signal_interval{};
  clock::duration frame_execute_commit_interval{};
  clock::duration frame_execute_wait_arm_interval{};
  clock::duration frame_replay_record_loop_interval{};
  clock::duration frame_replay_superseded_mask_interval{};
  clock::duration frame_replay_compiled_graphics_interval{};
  clock::duration frame_replay_compiled_compute_interval{};
  clock::duration frame_replay_fallback_classification_interval{};
  clock::duration frame_replay_typed_record_interval{};
  clock::duration frame_replay_flush_pass_interval{};
  clock::duration frame_replay_timestamp_resolve_interval{};
  clock::duration frame_replay_cpu_query_resolve_interval{};
  clock::duration frame_replay_get_pipeline_interval{};
  clock::duration frame_replay_get_metal_pso_interval{};
  clock::duration frame_replay_select_pso_interval{};
  clock::duration frame_replay_desc_access_interval{};
  clock::duration frame_replay_state_update_interval{};
  clock::duration frame_replay_attach_interval{};
  clock::duration frame_replay_bind_snapshot_interval{};
  clock::duration frame_replay_estimate_interval{};
  clock::duration frame_replay_packet_interval{};
  clock::duration frame_replay_queue_interval{};
  clock::duration frame_replay_emit_interval{};
  clock::duration frame_replay_flush_blit_interval{};
  clock::duration frame_replay_flush_compute_interval{};
  clock::duration frame_replay_flush_graphics_interval{};
  clock::duration frame_replay_flush_barrier_interval{};
  clock::duration frame_replay_build_plan_interval{};
  clock::duration frame_replay_emit_timestamp_interval{};
  clock::duration frame_replay_record_draw_interval{};
  clock::duration frame_replay_record_draw_indexed_interval{};
  clock::duration frame_replay_record_dispatch_interval{};
  clock::duration frame_replay_record_pipeline_state_interval{};
  clock::duration frame_replay_record_descriptor_heaps_interval{};
  clock::duration frame_replay_record_root_signature_interval{};
  clock::duration frame_replay_record_root_table_interval{};
  clock::duration frame_replay_record_root_descriptor_interval{};
  clock::duration frame_replay_record_root_constants_interval{};
  clock::duration frame_replay_record_vertex_index_state_interval{};
  clock::duration frame_replay_record_render_targets_interval{};
  clock::duration frame_replay_record_resource_barrier_interval{};
  clock::duration frame_replay_record_copy_clear_resolve_interval{};
  clock::duration frame_replay_record_query_interval{};
  clock::duration frame_replay_record_execute_indirect_interval{};
  clock::duration frame_replay_record_temporal_upscale_interval{};
  clock::duration frame_compiled_pass_build_interval{};
  clock::duration frame_compiled_draw_encode_interval{};
  clock::duration frame_compiled_draw_direct_encode_interval{};
  clock::duration frame_compiled_draw_replay_encode_interval{};
  clock::duration frame_compiled_draw_common_interval{};
  clock::duration frame_compiled_draw_retain_interval{};
  clock::duration frame_compiled_draw_pipeline_interval{};
  clock::duration frame_compiled_draw_dsso_interval{};
  clock::duration frame_compiled_draw_rasterizer_interval{};
  clock::duration frame_compiled_draw_binding_gate_interval{};
  clock::duration frame_compiled_draw_binding_snapshot_interval{};
  clock::duration frame_compiled_draw_dynamic_state_interval{};
  clock::duration frame_compiled_draw_visibility_interval{};
  clock::duration frame_compiled_draw_body_interval{};
  clock::duration frame_compiled_snapshot_static_samplers_interval{};
  clock::duration frame_compiled_snapshot_entries_interval{};
  clock::duration frame_compiled_snapshot_root_constants_interval{};
  clock::duration frame_compiled_snapshot_descriptors_interval{};
  clock::duration frame_compiled_snapshot_clear_descriptors_interval{};
  clock::duration frame_compiled_snapshot_bindless_fill_interval{};
  clock::duration frame_compiled_snapshot_vertex_buffers_interval{};
  clock::duration frame_compiled_snapshot_vertex_table_interval{};
  clock::duration frame_compiled_snapshot_restore_argbuf_interval{};
  clock::duration frame_compiled_snapshot_shader_bindings_interval{};
  clock::duration frame_compiled_snapshot_bindless_shader_bindings_interval{};
  clock::duration frame_compiled_snapshot_legacy_shader_bindings_interval{};
  clock::duration frame_compiled_dispatch_encode_interval{};
  clock::duration frame_argument_table_update_interval{};
  clock::duration frame_argument_table_bind_interval{};
  clock::duration frame_residency_submit_interval{};
  clock::duration frame_pso_cache_lookup_interval{};
  clock::duration frame_pso_materialize_interval{};
  clock::duration frame_pso_compile_wait_interval{};
  std::array<clock::duration, kPerfCodePathCount> frame_code_path_intervals{};
  std::array<clock::duration, kPerfCodePathCount> frame_code_path_max_intervals{};
  std::array<uint64_t, kPerfCodePathCount> frame_code_path_counts{};
  uint64_t frame_replay_draw_count = 0;
  uint64_t frame_replay_compiled_graphics_packet_count = 0;
  uint64_t frame_replay_compiled_compute_packet_count = 0;
  uint64_t frame_replay_fallback_classification_count = 0;
  uint64_t frame_replay_record_draw_count = 0;
  uint64_t frame_replay_record_draw_indexed_count = 0;
  uint64_t frame_replay_record_dispatch_count = 0;
  uint64_t frame_replay_record_pipeline_state_count = 0;
  uint64_t frame_replay_record_descriptor_heaps_count = 0;
  uint64_t frame_replay_record_root_signature_count = 0;
  uint64_t frame_replay_record_root_table_count = 0;
  uint64_t frame_replay_record_root_descriptor_count = 0;
  uint64_t frame_replay_record_root_constants_count = 0;
  uint64_t frame_replay_record_vertex_index_state_count = 0;
  uint64_t frame_replay_record_render_targets_count = 0;
  uint64_t frame_replay_record_resource_barrier_count = 0;
  uint64_t frame_replay_record_copy_clear_resolve_count = 0;
  uint64_t frame_replay_record_query_count = 0;
  uint64_t frame_replay_record_execute_indirect_count = 0;
  uint64_t frame_replay_record_temporal_upscale_count = 0;
  uint64_t frame_replay_pso_root_unchanged = 0;
  uint64_t frame_replay_full_bind_unchanged = 0;
  uint64_t frame_replay_flush_blit_count = 0;
  uint64_t frame_replay_flush_compute_count = 0;
  uint64_t frame_replay_flush_graphics_count = 0;
  uint64_t frame_replay_flush_barrier_count = 0;
  uint64_t frame_replay_emit_timestamp_count = 0;
  uint64_t frame_replay_flush_pass_batches_count = 0;
  uint64_t frame_replay_flush_pass_batches_empty = 0;
  uint64_t frame_replay_resource_access_count = 0;
  uint64_t frame_replay_resource_access_steady_noop = 0;
  uint64_t frame_replay_desc_access_hit = 0;
  uint64_t frame_replay_desc_access_miss = 0;
  uint64_t frame_replay_desc_access_passthrough = 0;
  uint64_t frame_replay_superseded_state_records_skipped = 0;
  uint64_t frame_replay_compiled_graphics_candidates = 0;
  uint64_t frame_replay_compiled_graphics_legacy = 0;
  uint64_t frame_replay_compiled_graphics_barriers = 0;
  uint64_t frame_replay_compiled_graphics_gs_ts = 0;
  uint64_t frame_replay_compiled_graphics_indirect = 0;
  uint64_t frame_compiled_graphics_packets = 0;
  uint64_t frame_compiled_draw_direct_packets = 0;
  uint64_t frame_compiled_draw_replay_packets = 0;
  uint64_t frame_compiled_draw_indexed_packets = 0;
  uint64_t frame_compiled_draw_nonindexed_packets = 0;
  uint64_t frame_compiled_draw_binding_snapshot_applied = 0;
  uint64_t frame_compiled_draw_binding_snapshot_skipped = 0;
  uint64_t frame_compiled_draw_binding_generation_hits = 0;
  uint64_t frame_compiled_draw_binding_fingerprint_hits = 0;
  uint64_t frame_compiled_draw_binding_misses = 0;
  uint64_t frame_compiled_snapshot_entries = 0;
  uint64_t frame_compiled_snapshot_root_constants = 0;
  uint64_t frame_compiled_snapshot_descriptors = 0;
  uint64_t frame_compiled_snapshot_clear_descriptors = 0;
  uint64_t frame_compiled_snapshot_bindless_fills = 0;
  uint64_t frame_compiled_snapshot_bindless_fill_texture = 0;
  uint64_t frame_compiled_snapshot_bindless_fill_sampler = 0;
  uint64_t frame_compiled_snapshot_bindless_fill_texture_buffer = 0;
  uint64_t frame_compiled_snapshot_bindless_fill_null = 0;
  uint64_t frame_compiled_snapshot_vertex_buffers = 0;
  uint64_t frame_compiled_snapshot_shader_bindings = 0;
  uint64_t frame_compiled_snapshot_bindless_shader_bindings = 0;
  uint64_t frame_compiled_snapshot_legacy_shader_bindings = 0;
  uint64_t frame_compiled_snapshot_legacy_argbuf_restores = 0;
  uint64_t frame_fallback_graphics_packets = 0;
  uint64_t frame_compiled_compute_packets = 0;
  uint64_t frame_fallback_compute_packets = 0;
  uint64_t frame_state_records_elided = 0;
  std::array<uint64_t, kCompiledFallbackReasonCount>
      frame_compiled_fallback_reasons{};
  uint64_t frame_replay_mismatch_barriers = 0;
  uint64_t frame_replay_app_barrier_transitions = 0;
  uint64_t frame_replay_binding_gen_bumps = 0;
  uint64_t frame_replay_binding_gen_pipeline = 0;
  uint64_t frame_replay_binding_gen_descriptor_heaps = 0;
  uint64_t frame_replay_binding_gen_vertex_buffers = 0;
  uint64_t frame_replay_binding_gen_root_signature = 0;
  uint64_t frame_replay_binding_gen_root_descriptor_table = 0;
  uint64_t frame_replay_binding_gen_root_descriptor = 0;
  uint64_t frame_replay_binding_gen_root_constants = 0;
  uint64_t frame_replay_binding_gen_indirect_vertex_buffer = 0;
  uint64_t frame_replay_binding_gen_indirect_root_constants = 0;
  uint64_t frame_replay_binding_gen_indirect_root_descriptor = 0;
  uint64_t frame_replay_snapshot_requests = 0;
  uint64_t frame_replay_snapshot_cache_hits = 0;
  uint64_t frame_replay_snapshot_cache_misses = 0;
  uint64_t frame_replay_snapshot_passthrough = 0;
  uint64_t frame_replay_snapshot_graphics_gen_changes = 0;
  uint64_t frame_replay_snapshot_descriptor_gen_changes = 0;
  uint64_t frame_replay_snapshot_both_gen_changes = 0;
  uint64_t frame_replay_snapshot_no_gen_changes = 0;
  uint64_t frame_replay_snapshot_captured_entries = 0;
  uint64_t frame_replay_snapshot_captured_descriptors = 0;
  uint64_t frame_replay_snapshot_captured_missing_descriptors = 0;
  uint64_t frame_replay_snapshot_captured_root_descriptors = 0;
  uint64_t frame_replay_snapshot_captured_root_constants = 0;
  uint64_t frame_replay_snapshot_captured_vertex_buffers = 0;
  uint64_t frame_replay_snapshot_captured_bindless = 0;
  uint64_t frame_native_descriptor_root_tables = 0;
  uint64_t frame_native_descriptor_root_table_backend_ready = 0;
  uint64_t frame_native_descriptor_record_storage_ready = 0;
  uint64_t frame_native_descriptor_resource_root_tables = 0;
  uint64_t frame_native_descriptor_sampler_root_tables = 0;
  uint64_t frame_native_descriptor_buffer_records = 0;
  uint64_t frame_native_descriptor_buffer_record_cbv = 0;
  uint64_t frame_native_descriptor_buffer_record_srv = 0;
  uint64_t frame_native_descriptor_buffer_record_uav = 0;
  uint64_t frame_native_descriptor_buffer_record_counters = 0;
  uint64_t frame_native_descriptor_buffer_record_missing_resource = 0;
  uint64_t frame_native_descriptor_resource_table_entries = 0;
  uint64_t frame_generic_descriptor_span_lookups = 0;
  uint64_t frame_generic_descriptor_span_unique = 0;
  uint64_t frame_generic_descriptor_span_reuses = 0;
  uint64_t frame_frozen_native_direct_packets = 0;
  uint64_t frame_frozen_range_lookups = 0;
  uint64_t frame_frozen_range_unique = 0;
  uint64_t frame_frozen_range_reuses = 0;
  uint64_t frame_frozen_root_word_lookups = 0;
  uint64_t frame_frozen_root_word_reuses = 0;
  uint64_t frame_submission_graphics_encoder_boundary_merges = 0;
  uint64_t frame_submission_compute_encoder_boundary_merges = 0;
  uint64_t frame_submission_encoder_boundary_flushes = 0;
  uint64_t frame_descriptor_content_writes = 0;
  uint64_t frame_descriptor_content_write_cbv = 0;
  uint64_t frame_descriptor_content_write_srv = 0;
  uint64_t frame_descriptor_content_write_uav = 0;
  uint64_t frame_descriptor_content_write_sampler = 0;
  uint64_t frame_descriptor_content_write_copy = 0;
  uint64_t frame_descriptor_content_write_feedback_uav = 0;
  ScalerInfo last_scaler_info{};
  // Appended after last_scaler_info: FrameStatistics lives in the shared dxmt
  // static library and d3d11/d3d12 read the fields above across the link
  // boundary, so a new field must not shift an existing field's offset.
  clock::duration frame_wall_interval{};
  // Frontend cost, appended under the same ABI rule. The counters above are
  // encoder-level: they begin once a chunk exists. These cover what a D3D entry
  // point charges the calling thread before that point (a Lock that must wait
  // out the GPU, a staging upload, sealing the per-frame rings at the chunk
  // boundary, per-draw capture) and the encode thread's per-draw cost of
  // turning a captured draw into commands. Without them a frame's wall time is
  // only attributable from the chunk boundary onward, so a stall in an entry
  // point reads as unaccounted. Note frame_chunk_commit_interval covers the
  // whole commit entry point, of which the older commit_interval times only the
  // ring backpressure wait. Both are reported: on a d3d9 workload the wait is a
  // rounding error next to the entry point total, so collapsing them would hide
  // wherever the rest of that time goes.
  // The two groups are written by different threads and are kept disjoint for
  // that reason: the calling thread owns everything down to the poll counters,
  // the encode thread owns only the draw resolve and emit intervals (and the
  // pipeline compile wait above). No field takes writes from both, so these
  // stay plain like the rest of the struct.
  clock::duration frame_resource_lock_interval{};
  clock::duration frame_staging_upload_interval{};
  clock::duration frame_chunk_commit_interval{};
  clock::duration frame_draw_record_interval{};
  clock::duration frame_draw_resolve_interval{};
  clock::duration frame_draw_emit_interval{};
  uint64_t frame_resource_lock_count = 0;
  uint64_t frame_staging_upload_bytes = 0;
  // Buffer cache maintenance: a refresh moves a whole mirror into a renamed
  // allocation. This is what decides whether tracking a dirty bit instead of a
  // dirty range costs anything measurable, so it is the tripwire for the
  // storage model as a whole.
  uint64_t frame_buffer_refresh_bytes = 0;
  uint64_t frame_buffer_refresh_count = 0;
  uint64_t frame_chunk_commit_count = 0;
  uint64_t frame_draw_resolve_count = 0;
  // Entry counts for two intervals that predate these fields. An interval
  // alone cannot tell one expensive event from a churn of cheap ones, which
  // is the distinction that decides whether to optimize the operation or the
  // call rate. The compile wait counts only draws that actually blocked, not
  // the readiness probe every draw pays.
  uint64_t frame_create_resource_count = 0;
  uint64_t frame_pso_compile_wait_count = 0;
  // Draws counted where the app issues them, which is a different population
  // from render_draw_count above: that one counts the Metal draw commands the
  // encode thread emits, so it excludes draws that batching merged and draws
  // dropped for a failed pipeline link. Being written on the calling thread it
  // also lands on the present-boundary report, where the encode-thread counts
  // are not written yet, so a per-draw cost for the frame is computable from a
  // single record instead of joining two.
  uint64_t frame_draw_issue_count = 0;
  // CPU the calling thread actually consumed over the frame, against which the
  // frame wall can be split three ways. Wall far above user+kernel means the
  // thread was not running at all, so the cost is a block and not work, and the
  // thread is parked in a syscall where its stack can still be walked. Wall
  // tracking kernel means time in fault handling; wall tracking user means
  // execution. An interval alone cannot tell those apart, and every bucket
  // above measures wall. The source is the platform's per-thread CPU clock,
  // which quantizes to about 10ms, so these say nothing about a frame that ran
  // to schedule and are meant for the multi-hundred-millisecond outliers where
  // the split is the whole question.
  clock::duration frame_thread_user_interval{};
  clock::duration frame_thread_kernel_interval{};
  // Faults taken over the frame. Kernel time on its own says the thread was in
  // the kernel but not why; a fault count that moves with it says the kernel
  // time is fault handling. The zero fill share is the discriminator that
  // matters: faults that zero a page are commit or first touch, faults that do
  // not are protection faults on pages that were already resident, and those
  // two want completely different fixes. Host zero fill counts other processes
  // too, so read its rate within a burst rather than its absolute value.
  uint64_t frame_task_fault_count = 0;
  uint64_t frame_task_pagein_count = 0;
  uint64_t frame_task_cow_fault_count = 0;
  uint64_t frame_host_zero_fill_count = 0;
  // Entry points an app polls rather than does work in. A frame that spends its
  // wall clock here is waiting on us, so a high poll count next to a low draw
  // count separates app spin from translation cost.
  uint64_t frame_query_get_data_count = 0;
  uint64_t frame_query_get_data_pending_count = 0;
  uint64_t frame_query_issue_count = 0;
  uint64_t frame_raster_status_poll_count = 0;
  uint64_t frame_device_status_poll_count = 0;

  void
  reset() {
    begin_time = {};
    compatibility_flags.clrAll();
    command_buffer_count = 0;
    sync_count = 0;
    sync_interval = {};
    commit_interval = {};
    render_pass_count = 0;
    render_pass_optimized = 0;
    render_command_count = 0;
    render_pso_bind_count = 0;
    render_draw_count = 0;
    render_indexed_draw_count = 0;
    render_indirect_draw_count = 0;
    render_mesh_draw_count = 0;
    render_tile_dispatch_count = 0;
    render_color_load_count = 0;
    render_color_store_count = 0;
    render_depth_load_count = 0;
    render_depth_store_count = 0;
    render_stencil_load_count = 0;
    render_stencil_store_count = 0;
    render_attachment_count = 0;
    render_attachment_reuse_count = 0;
    render_attachment_reload_count = 0;
    render_attachment_store_then_load_count = 0;
    encoder_switch_render_compute_count = 0;
    encoder_switch_compute_render_count = 0;
    encoder_switch_to_other_count = 0;
    present_pass_count = 0;
    clear_pass_count = 0;
    clear_pass_optimized = 0;
    resolve_pass_optimized = 0;
    compute_pass_count = 0;
    compute_pass_optimized = 0;
    compute_pass_with_dispatch_count = 0;
    compute_pass_without_dispatch_count = 0;
    compute_command_count = 0;
    compute_pso_bind_count = 0;
    compute_set_buffer_count = 0;
    compute_set_texture_count = 0;
    compute_set_bytes_count = 0;
    compute_use_resource_count = 0;
    compute_dispatch_count = 0;
    compute_dispatch_indirect_count = 0;
    compute_memory_barrier_count = 0;
    compute_fence_wait_count = 0;
    compute_fence_update_count = 0;
    blit_pass_count = 0;
    blit_pass_optimized = 0;
    blit_pass_with_commands_count = 0;
    blit_pass_empty_count = 0;
    blit_pass_deferred_fence_only_count = 0;
    blit_pass_merged_fence_only_count = 0;
    blit_barrier_only_pass_count = 0;
    blit_separator_pass_count = 0;
    resource_barrier_batches_merged = 0;
    resource_barrier_batches_graphics_inlined = 0;
    resource_barrier_batches_compute_inlined = 0;
    blit_pass_with_fence_wait_count = 0;
    blit_pass_with_fence_update_count = 0;
    blit_fence_wait_entry_count = 0;
    blit_fence_update_entry_count = 0;
    blit_command_count = 0;
    blit_copy_buffer_to_buffer_count = 0;
    blit_copy_buffer_to_texture_count = 0;
    blit_copy_texture_to_buffer_count = 0;
    blit_copy_texture_to_texture_count = 0;
    blit_generate_mipmaps_count = 0;
    blit_fence_wait_count = 0;
    blit_fence_update_count = 0;
    blit_fill_buffer_count = 0;
    blit_resolve_counters_count = 0;
    event_stall = 0;
    latency = 0;
    flush_chunk_count = 0;
    flush_empty_chunk_count = 0;
    flush_event_only_chunk_count = 0;
    flush_signal_chunk_count = 0;
    flush_wait_chunk_count = 0;
    flush_present_chunk_count = 0;
    flush_encoder_count = 0;
    flush_encoded_encoder_count = 0;
    flush_null_encoder_count = 0;
    flush_render_encoder_count = 0;
    flush_compute_encoder_count = 0;
    flush_blit_encoder_count = 0;
    flush_timestamp_encoder_count = 0;
    flush_total_interval = {};
    flush_collect_interval = {};
    flush_relation_interval = {};
    flush_query_interval = {};
    flush_timestamp_interval = {};
    flush_render_interval = {};
    flush_compute_interval = {};
    flush_blit_interval = {};
    flush_clear_interval = {};
    flush_resolve_interval = {};
    flush_scaler_interval = {};
    flush_present_interval = {};
    flush_event_interval = {};
    flush_sample_interval = {};
    flush_pending_fence_interval = {};
    flush_signal_interval = {};
    flush_cleanup_interval = {};
    flush_max_chunk_interval = {};
    shader_binding_upload_count = 0;
    shader_binding_dirty_cbuffer_count = 0;
    shader_binding_dirty_sampler_count = 0;
    shader_binding_dirty_srv_count = 0;
    shader_binding_dirty_uav_count = 0;
    shader_binding_clean_uav_count = 0;
    encode_prepare_interval = {};
    encode_flush_interval = {};
    drawable_blocking_interval = {};
    present_latency_interval = {};
    frame_execute_command_lists_interval = {};
    frame_present_interval = {};
    frame_present_queue_wait_interval = {};
    frame_queue_signal_interval = {};
    frame_queue_wait_interval = {};
    frame_create_resource_interval = {};
    frame_create_reserved_resource_interval = {};
    frame_create_heap_interval = {};
    frame_create_pipeline_interval = {};
    frame_tile_mapping_interval = {};
    frame_cmdlist_record_interval = {};
    frame_execute_validate_interval = {};
    frame_execute_collect_interval = {};
    frame_execute_enqueue_interval = {};
    frame_execute_drain_interval = {};
    frame_execute_drain_lock_interval = {};
    frame_execute_resource_state_lock_hold_interval = {};
    frame_execute_replay_interval = {};
    frame_execute_decay_interval = {};
    frame_execute_signal_interval = {};
    frame_execute_commit_interval = {};
    frame_execute_wait_arm_interval = {};
    frame_replay_record_loop_interval = {};
    frame_replay_superseded_mask_interval = {};
    frame_replay_compiled_graphics_interval = {};
    frame_replay_compiled_compute_interval = {};
    frame_replay_fallback_classification_interval = {};
    frame_replay_typed_record_interval = {};
    frame_replay_flush_pass_interval = {};
    frame_replay_timestamp_resolve_interval = {};
    frame_replay_cpu_query_resolve_interval = {};
    frame_replay_get_pipeline_interval = {};
    frame_replay_get_metal_pso_interval = {};
    frame_replay_select_pso_interval = {};
    frame_replay_desc_access_interval = {};
    frame_replay_state_update_interval = {};
    frame_replay_attach_interval = {};
    frame_replay_bind_snapshot_interval = {};
    frame_replay_estimate_interval = {};
    frame_replay_packet_interval = {};
    frame_replay_queue_interval = {};
    frame_replay_emit_interval = {};
    frame_replay_flush_blit_interval = {};
    frame_replay_flush_compute_interval = {};
    frame_replay_flush_graphics_interval = {};
    frame_replay_flush_barrier_interval = {};
    frame_replay_build_plan_interval = {};
    frame_replay_emit_timestamp_interval = {};
    frame_replay_record_draw_interval = {};
    frame_replay_record_draw_indexed_interval = {};
    frame_replay_record_dispatch_interval = {};
    frame_replay_record_pipeline_state_interval = {};
    frame_replay_record_descriptor_heaps_interval = {};
    frame_replay_record_root_signature_interval = {};
    frame_replay_record_root_table_interval = {};
    frame_replay_record_root_descriptor_interval = {};
    frame_replay_record_root_constants_interval = {};
    frame_replay_record_vertex_index_state_interval = {};
    frame_replay_record_render_targets_interval = {};
    frame_replay_record_resource_barrier_interval = {};
    frame_replay_record_copy_clear_resolve_interval = {};
    frame_replay_record_query_interval = {};
    frame_replay_record_execute_indirect_interval = {};
    frame_replay_record_temporal_upscale_interval = {};
    frame_compiled_pass_build_interval = {};
    frame_compiled_draw_encode_interval = {};
    frame_compiled_draw_direct_encode_interval = {};
    frame_compiled_draw_replay_encode_interval = {};
    frame_compiled_draw_common_interval = {};
    frame_compiled_draw_retain_interval = {};
    frame_compiled_draw_pipeline_interval = {};
    frame_compiled_draw_dsso_interval = {};
    frame_compiled_draw_rasterizer_interval = {};
    frame_compiled_draw_binding_gate_interval = {};
    frame_compiled_draw_binding_snapshot_interval = {};
    frame_compiled_draw_dynamic_state_interval = {};
    frame_compiled_draw_visibility_interval = {};
    frame_compiled_draw_body_interval = {};
    frame_compiled_snapshot_static_samplers_interval = {};
    frame_compiled_snapshot_entries_interval = {};
    frame_compiled_snapshot_root_constants_interval = {};
    frame_compiled_snapshot_descriptors_interval = {};
    frame_compiled_snapshot_clear_descriptors_interval = {};
    frame_compiled_snapshot_bindless_fill_interval = {};
    frame_compiled_snapshot_vertex_buffers_interval = {};
    frame_compiled_snapshot_vertex_table_interval = {};
    frame_compiled_snapshot_restore_argbuf_interval = {};
    frame_compiled_snapshot_shader_bindings_interval = {};
    frame_compiled_snapshot_bindless_shader_bindings_interval = {};
    frame_compiled_snapshot_legacy_shader_bindings_interval = {};
    frame_compiled_dispatch_encode_interval = {};
    frame_argument_table_update_interval = {};
    frame_argument_table_bind_interval = {};
    frame_residency_submit_interval = {};
    frame_pso_cache_lookup_interval = {};
    frame_pso_materialize_interval = {};
    frame_pso_compile_wait_interval = {};
    frame_code_path_intervals.fill({});
    frame_code_path_max_intervals.fill({});
    frame_code_path_counts.fill(0);
    frame_replay_draw_count = 0;
    frame_replay_compiled_graphics_packet_count = 0;
    frame_replay_compiled_compute_packet_count = 0;
    frame_replay_fallback_classification_count = 0;
    frame_replay_record_draw_count = 0;
    frame_replay_record_draw_indexed_count = 0;
    frame_replay_record_dispatch_count = 0;
    frame_replay_record_pipeline_state_count = 0;
    frame_replay_record_descriptor_heaps_count = 0;
    frame_replay_record_root_signature_count = 0;
    frame_replay_record_root_table_count = 0;
    frame_replay_record_root_descriptor_count = 0;
    frame_replay_record_root_constants_count = 0;
    frame_replay_record_vertex_index_state_count = 0;
    frame_replay_record_render_targets_count = 0;
    frame_replay_record_resource_barrier_count = 0;
    frame_replay_record_copy_clear_resolve_count = 0;
    frame_replay_record_query_count = 0;
    frame_replay_record_execute_indirect_count = 0;
    frame_replay_record_temporal_upscale_count = 0;
    frame_replay_pso_root_unchanged = 0;
    frame_replay_full_bind_unchanged = 0;
    frame_replay_flush_blit_count = 0;
    frame_replay_flush_compute_count = 0;
    frame_replay_flush_graphics_count = 0;
    frame_replay_flush_barrier_count = 0;
    frame_replay_emit_timestamp_count = 0;
    frame_replay_flush_pass_batches_count = 0;
    frame_replay_flush_pass_batches_empty = 0;
    frame_replay_resource_access_count = 0;
    frame_replay_resource_access_steady_noop = 0;
    frame_replay_desc_access_hit = 0;
    frame_replay_desc_access_miss = 0;
    frame_replay_desc_access_passthrough = 0;
    frame_replay_superseded_state_records_skipped = 0;
    frame_replay_compiled_graphics_candidates = 0;
    frame_replay_compiled_graphics_legacy = 0;
    frame_replay_compiled_graphics_barriers = 0;
    frame_replay_compiled_graphics_gs_ts = 0;
    frame_replay_compiled_graphics_indirect = 0;
    frame_compiled_graphics_packets = 0;
    frame_compiled_draw_direct_packets = 0;
    frame_compiled_draw_replay_packets = 0;
    frame_compiled_draw_indexed_packets = 0;
    frame_compiled_draw_nonindexed_packets = 0;
    frame_compiled_draw_binding_snapshot_applied = 0;
    frame_compiled_draw_binding_snapshot_skipped = 0;
    frame_compiled_draw_binding_generation_hits = 0;
    frame_compiled_draw_binding_fingerprint_hits = 0;
    frame_compiled_draw_binding_misses = 0;
    frame_compiled_snapshot_entries = 0;
    frame_compiled_snapshot_root_constants = 0;
    frame_compiled_snapshot_descriptors = 0;
    frame_compiled_snapshot_clear_descriptors = 0;
    frame_compiled_snapshot_bindless_fills = 0;
    frame_compiled_snapshot_bindless_fill_texture = 0;
    frame_compiled_snapshot_bindless_fill_sampler = 0;
    frame_compiled_snapshot_bindless_fill_texture_buffer = 0;
    frame_compiled_snapshot_bindless_fill_null = 0;
    frame_compiled_snapshot_vertex_buffers = 0;
    frame_compiled_snapshot_shader_bindings = 0;
    frame_compiled_snapshot_bindless_shader_bindings = 0;
    frame_compiled_snapshot_legacy_shader_bindings = 0;
    frame_compiled_snapshot_legacy_argbuf_restores = 0;
    frame_fallback_graphics_packets = 0;
    frame_compiled_compute_packets = 0;
    frame_fallback_compute_packets = 0;
    frame_state_records_elided = 0;
    frame_compiled_fallback_reasons = {};
    frame_replay_mismatch_barriers = 0;
    frame_replay_app_barrier_transitions = 0;
    frame_replay_binding_gen_bumps = 0;
    frame_replay_binding_gen_pipeline = 0;
    frame_replay_binding_gen_descriptor_heaps = 0;
    frame_replay_binding_gen_vertex_buffers = 0;
    frame_replay_binding_gen_root_signature = 0;
    frame_replay_binding_gen_root_descriptor_table = 0;
    frame_replay_binding_gen_root_descriptor = 0;
    frame_replay_binding_gen_root_constants = 0;
    frame_replay_binding_gen_indirect_vertex_buffer = 0;
    frame_replay_binding_gen_indirect_root_constants = 0;
    frame_replay_binding_gen_indirect_root_descriptor = 0;
    frame_replay_snapshot_requests = 0;
    frame_replay_snapshot_cache_hits = 0;
    frame_replay_snapshot_cache_misses = 0;
    frame_replay_snapshot_passthrough = 0;
    frame_replay_snapshot_graphics_gen_changes = 0;
    frame_replay_snapshot_descriptor_gen_changes = 0;
    frame_replay_snapshot_both_gen_changes = 0;
    frame_replay_snapshot_no_gen_changes = 0;
    frame_replay_snapshot_captured_entries = 0;
    frame_replay_snapshot_captured_descriptors = 0;
    frame_replay_snapshot_captured_missing_descriptors = 0;
    frame_replay_snapshot_captured_root_descriptors = 0;
    frame_replay_snapshot_captured_root_constants = 0;
    frame_replay_snapshot_captured_vertex_buffers = 0;
    frame_replay_snapshot_captured_bindless = 0;
    frame_native_descriptor_root_tables = 0;
    frame_native_descriptor_root_table_backend_ready = 0;
    frame_native_descriptor_record_storage_ready = 0;
    frame_native_descriptor_resource_root_tables = 0;
    frame_native_descriptor_sampler_root_tables = 0;
    frame_native_descriptor_buffer_records = 0;
    frame_native_descriptor_buffer_record_cbv = 0;
    frame_native_descriptor_buffer_record_srv = 0;
    frame_native_descriptor_buffer_record_uav = 0;
    frame_native_descriptor_buffer_record_counters = 0;
    frame_native_descriptor_buffer_record_missing_resource = 0;
    frame_native_descriptor_resource_table_entries = 0;
    frame_generic_descriptor_span_lookups = 0;
    frame_generic_descriptor_span_unique = 0;
    frame_generic_descriptor_span_reuses = 0;
    frame_frozen_native_direct_packets = 0;
    frame_frozen_range_lookups = 0;
    frame_frozen_range_unique = 0;
    frame_frozen_range_reuses = 0;
    frame_frozen_root_word_lookups = 0;
    frame_frozen_root_word_reuses = 0;
    frame_submission_graphics_encoder_boundary_merges = 0;
    frame_submission_compute_encoder_boundary_merges = 0;
    frame_submission_encoder_boundary_flushes = 0;
    frame_descriptor_content_writes = 0;
    frame_descriptor_content_write_cbv = 0;
    frame_descriptor_content_write_srv = 0;
    frame_descriptor_content_write_uav = 0;
    frame_descriptor_content_write_sampler = 0;
    frame_descriptor_content_write_copy = 0;
    frame_descriptor_content_write_feedback_uav = 0;
    last_scaler_info.type = {};
    frame_wall_interval = {};
    frame_resource_lock_interval = {};
    frame_staging_upload_interval = {};
    frame_chunk_commit_interval = {};
    frame_draw_record_interval = {};
    frame_draw_resolve_interval = {};
    frame_draw_emit_interval = {};
    frame_resource_lock_count = 0;
    frame_staging_upload_bytes = 0;
    frame_buffer_refresh_bytes = 0;
    frame_buffer_refresh_count = 0;
    frame_chunk_commit_count = 0;
    frame_draw_resolve_count = 0;
    frame_create_resource_count = 0;
    frame_pso_compile_wait_count = 0;
    frame_draw_issue_count = 0;
    frame_thread_user_interval = {};
    frame_thread_kernel_interval = {};
    frame_task_fault_count = 0;
    frame_task_pagein_count = 0;
    frame_task_cow_fault_count = 0;
    frame_host_zero_fill_count = 0;
    frame_query_get_data_count = 0;
    frame_query_get_data_pending_count = 0;
    frame_query_issue_count = 0;
    frame_raster_status_poll_count = 0;
    frame_device_status_poll_count = 0;
  };
};

constexpr size_t kFrameStatisticsCount = 16;

class FrameStatisticsContainer {
  std::array<FrameStatistics, kFrameStatisticsCount> frames_;
  FrameStatistics min_;
  FrameStatistics max_;
  FrameStatistics average_;

public:
  FrameStatistics &
  at(uint64_t frame) {
    return frames_[frame % kFrameStatisticsCount];
  }
  const FrameStatistics &
  at(uint64_t frame) const {
    return frames_.at(frame % kFrameStatisticsCount);
  }

  const FrameStatistics &
  min() const {
    return min_;
  }

  const FrameStatistics &
  max() const {
    return max_;
  }

  const FrameStatistics &
  average() const {
    return average_;
  }

  void
  compute(uint64_t current_frame) {
    min_.reset();
    max_.reset();
    average_.reset();
    current_frame = current_frame % kFrameStatisticsCount;
    bool first_sample = true;
    for(unsigned i = 0; i < kFrameStatisticsCount; i++) {
      if (i == current_frame)
        continue; // deliberately exclude current frame since it
      if (first_sample) {
        min_.command_buffer_count = frames_[i].command_buffer_count;
        min_.sync_count = frames_[i].sync_count;
        min_.event_stall = frames_[i].event_stall;
        min_.commit_interval = frames_[i].commit_interval;
        min_.sync_interval = frames_[i].sync_interval;
        min_.encode_prepare_interval = frames_[i].encode_prepare_interval;
        min_.encode_flush_interval = frames_[i].encode_flush_interval;
        min_.drawable_blocking_interval = frames_[i].drawable_blocking_interval;
        min_.present_latency_interval = frames_[i].present_latency_interval;
        first_sample = false;
      }
      min_.command_buffer_count = std::min(min_.command_buffer_count, frames_[i].command_buffer_count);
      min_.sync_count = std::min(min_.sync_count, frames_[i].sync_count);
      min_.event_stall = std::min(min_.event_stall, frames_[i].event_stall);
      min_.commit_interval = std::min(min_.commit_interval, frames_[i].commit_interval);
      min_.sync_interval = std::min(min_.sync_interval, frames_[i].sync_interval);
      min_.encode_prepare_interval = std::min(min_.encode_prepare_interval, frames_[i].encode_prepare_interval);
      min_.encode_flush_interval = std::min(min_.encode_flush_interval, frames_[i].encode_flush_interval);
      min_.drawable_blocking_interval =
          std::min(min_.drawable_blocking_interval, frames_[i].drawable_blocking_interval);
      min_.present_latency_interval = std::min(min_.present_latency_interval, frames_[i].present_latency_interval);

      max_.command_buffer_count = std::max(max_.command_buffer_count, frames_[i].command_buffer_count);
      max_.sync_count = std::max(max_.sync_count, frames_[i].sync_count);
      max_.event_stall = std::max(max_.event_stall, frames_[i].event_stall);
      max_.commit_interval = std::max(max_.commit_interval, frames_[i].commit_interval);
      max_.sync_interval = std::max(max_.sync_interval, frames_[i].sync_interval);
      max_.encode_prepare_interval = std::max(max_.encode_prepare_interval, frames_[i].encode_prepare_interval);
      max_.encode_flush_interval = std::max(max_.encode_flush_interval, frames_[i].encode_flush_interval);
      max_.drawable_blocking_interval =
          std::max(max_.drawable_blocking_interval, frames_[i].drawable_blocking_interval);
      max_.present_latency_interval = std::max(max_.present_latency_interval, frames_[i].present_latency_interval);

      average_.command_buffer_count += frames_[i].command_buffer_count;
      average_.render_pass_count += frames_[i].render_pass_count;
      average_.render_pass_optimized += frames_[i].render_pass_optimized;
      average_.render_command_count += frames_[i].render_command_count;
      average_.render_pso_bind_count += frames_[i].render_pso_bind_count;
      average_.render_draw_count += frames_[i].render_draw_count;
      average_.render_indexed_draw_count += frames_[i].render_indexed_draw_count;
      average_.render_indirect_draw_count += frames_[i].render_indirect_draw_count;
      average_.render_mesh_draw_count += frames_[i].render_mesh_draw_count;
      average_.render_tile_dispatch_count += frames_[i].render_tile_dispatch_count;
      average_.render_color_load_count += frames_[i].render_color_load_count;
      average_.render_color_store_count += frames_[i].render_color_store_count;
      average_.render_depth_load_count += frames_[i].render_depth_load_count;
      average_.render_depth_store_count += frames_[i].render_depth_store_count;
      average_.render_stencil_load_count += frames_[i].render_stencil_load_count;
      average_.render_stencil_store_count += frames_[i].render_stencil_store_count;
      average_.render_attachment_count += frames_[i].render_attachment_count;
      average_.render_attachment_reuse_count += frames_[i].render_attachment_reuse_count;
      average_.render_attachment_reload_count += frames_[i].render_attachment_reload_count;
      average_.render_attachment_store_then_load_count += frames_[i].render_attachment_store_then_load_count;
      average_.encoder_switch_render_compute_count += frames_[i].encoder_switch_render_compute_count;
      average_.encoder_switch_compute_render_count += frames_[i].encoder_switch_compute_render_count;
      average_.encoder_switch_to_other_count += frames_[i].encoder_switch_to_other_count;
      average_.present_pass_count += frames_[i].present_pass_count;
      average_.clear_pass_count += frames_[i].clear_pass_count;
      average_.clear_pass_optimized += frames_[i].clear_pass_optimized;
      average_.resolve_pass_optimized += frames_[i].resolve_pass_optimized;
      average_.compute_pass_count += frames_[i].compute_pass_count;
      average_.compute_pass_optimized += frames_[i].compute_pass_optimized;
      average_.compute_pass_with_dispatch_count += frames_[i].compute_pass_with_dispatch_count;
      average_.compute_pass_without_dispatch_count += frames_[i].compute_pass_without_dispatch_count;
      average_.compute_command_count += frames_[i].compute_command_count;
      average_.compute_pso_bind_count += frames_[i].compute_pso_bind_count;
      average_.compute_set_buffer_count += frames_[i].compute_set_buffer_count;
      average_.compute_set_texture_count += frames_[i].compute_set_texture_count;
      average_.compute_set_bytes_count += frames_[i].compute_set_bytes_count;
      average_.compute_use_resource_count += frames_[i].compute_use_resource_count;
      average_.compute_dispatch_count += frames_[i].compute_dispatch_count;
      average_.compute_dispatch_indirect_count += frames_[i].compute_dispatch_indirect_count;
      average_.compute_memory_barrier_count += frames_[i].compute_memory_barrier_count;
      average_.compute_fence_wait_count += frames_[i].compute_fence_wait_count;
      average_.compute_fence_update_count += frames_[i].compute_fence_update_count;
      average_.blit_pass_count += frames_[i].blit_pass_count;
      average_.blit_pass_optimized += frames_[i].blit_pass_optimized;
      average_.blit_pass_with_commands_count += frames_[i].blit_pass_with_commands_count;
      average_.blit_pass_empty_count += frames_[i].blit_pass_empty_count;
      average_.blit_pass_deferred_fence_only_count += frames_[i].blit_pass_deferred_fence_only_count;
      average_.blit_pass_merged_fence_only_count += frames_[i].blit_pass_merged_fence_only_count;
      average_.blit_barrier_only_pass_count += frames_[i].blit_barrier_only_pass_count;
      average_.blit_separator_pass_count += frames_[i].blit_separator_pass_count;
      average_.resource_barrier_batches_merged += frames_[i].resource_barrier_batches_merged;
      average_.resource_barrier_batches_graphics_inlined += frames_[i].resource_barrier_batches_graphics_inlined;
      average_.resource_barrier_batches_compute_inlined += frames_[i].resource_barrier_batches_compute_inlined;
      average_.blit_pass_with_fence_wait_count += frames_[i].blit_pass_with_fence_wait_count;
      average_.blit_pass_with_fence_update_count += frames_[i].blit_pass_with_fence_update_count;
      average_.blit_fence_wait_entry_count += frames_[i].blit_fence_wait_entry_count;
      average_.blit_fence_update_entry_count += frames_[i].blit_fence_update_entry_count;
      average_.blit_command_count += frames_[i].blit_command_count;
      average_.blit_copy_buffer_to_buffer_count += frames_[i].blit_copy_buffer_to_buffer_count;
      average_.blit_copy_buffer_to_texture_count += frames_[i].blit_copy_buffer_to_texture_count;
      average_.blit_copy_texture_to_buffer_count += frames_[i].blit_copy_texture_to_buffer_count;
      average_.blit_copy_texture_to_texture_count += frames_[i].blit_copy_texture_to_texture_count;
      average_.blit_generate_mipmaps_count += frames_[i].blit_generate_mipmaps_count;
      average_.blit_fence_wait_count += frames_[i].blit_fence_wait_count;
      average_.blit_fence_update_count += frames_[i].blit_fence_update_count;
      average_.blit_fill_buffer_count += frames_[i].blit_fill_buffer_count;
      average_.blit_resolve_counters_count += frames_[i].blit_resolve_counters_count;
      average_.sync_count += frames_[i].sync_count;
      average_.event_stall += frames_[i].event_stall;
      average_.latency += frames_[i].latency;
      average_.flush_chunk_count += frames_[i].flush_chunk_count;
      average_.flush_empty_chunk_count += frames_[i].flush_empty_chunk_count;
      average_.flush_event_only_chunk_count += frames_[i].flush_event_only_chunk_count;
      average_.flush_signal_chunk_count += frames_[i].flush_signal_chunk_count;
      average_.flush_wait_chunk_count += frames_[i].flush_wait_chunk_count;
      average_.flush_present_chunk_count += frames_[i].flush_present_chunk_count;
      average_.flush_encoder_count += frames_[i].flush_encoder_count;
      average_.flush_encoded_encoder_count += frames_[i].flush_encoded_encoder_count;
      average_.flush_null_encoder_count += frames_[i].flush_null_encoder_count;
      average_.flush_render_encoder_count += frames_[i].flush_render_encoder_count;
      average_.flush_compute_encoder_count += frames_[i].flush_compute_encoder_count;
      average_.flush_blit_encoder_count += frames_[i].flush_blit_encoder_count;
      average_.flush_timestamp_encoder_count += frames_[i].flush_timestamp_encoder_count;
      average_.flush_total_interval += frames_[i].flush_total_interval;
      average_.flush_collect_interval += frames_[i].flush_collect_interval;
      average_.flush_relation_interval += frames_[i].flush_relation_interval;
      average_.flush_query_interval += frames_[i].flush_query_interval;
      average_.flush_timestamp_interval += frames_[i].flush_timestamp_interval;
      average_.flush_render_interval += frames_[i].flush_render_interval;
      average_.flush_compute_interval += frames_[i].flush_compute_interval;
      average_.flush_blit_interval += frames_[i].flush_blit_interval;
      average_.flush_clear_interval += frames_[i].flush_clear_interval;
      average_.flush_resolve_interval += frames_[i].flush_resolve_interval;
      average_.flush_scaler_interval += frames_[i].flush_scaler_interval;
      average_.flush_present_interval += frames_[i].flush_present_interval;
      average_.flush_event_interval += frames_[i].flush_event_interval;
      average_.flush_sample_interval += frames_[i].flush_sample_interval;
      average_.flush_pending_fence_interval += frames_[i].flush_pending_fence_interval;
      average_.flush_signal_interval += frames_[i].flush_signal_interval;
      average_.flush_cleanup_interval += frames_[i].flush_cleanup_interval;
      average_.flush_max_chunk_interval =
          std::max(average_.flush_max_chunk_interval, frames_[i].flush_max_chunk_interval);
      average_.commit_interval += frames_[i].commit_interval;
      average_.sync_interval += frames_[i].sync_interval;
      average_.encode_prepare_interval += frames_[i].encode_prepare_interval;
      average_.encode_flush_interval += frames_[i].encode_flush_interval;
      average_.drawable_blocking_interval += frames_[i].drawable_blocking_interval;
      average_.present_latency_interval += frames_[i].present_latency_interval;
      average_.shader_binding_upload_count += frames_[i].shader_binding_upload_count;
      average_.shader_binding_dirty_cbuffer_count += frames_[i].shader_binding_dirty_cbuffer_count;
      average_.shader_binding_dirty_sampler_count += frames_[i].shader_binding_dirty_sampler_count;
      average_.shader_binding_dirty_srv_count += frames_[i].shader_binding_dirty_srv_count;
      average_.shader_binding_dirty_uav_count += frames_[i].shader_binding_dirty_uav_count;
      average_.shader_binding_clean_uav_count += frames_[i].shader_binding_clean_uav_count;
      average_.frame_execute_command_lists_interval += frames_[i].frame_execute_command_lists_interval;
      average_.frame_present_interval += frames_[i].frame_present_interval;
      average_.frame_present_queue_wait_interval +=
          frames_[i].frame_present_queue_wait_interval;
      average_.frame_queue_signal_interval += frames_[i].frame_queue_signal_interval;
      average_.frame_queue_wait_interval += frames_[i].frame_queue_wait_interval;
      average_.frame_create_resource_interval += frames_[i].frame_create_resource_interval;
      average_.frame_create_reserved_resource_interval += frames_[i].frame_create_reserved_resource_interval;
      average_.frame_create_heap_interval += frames_[i].frame_create_heap_interval;
      average_.frame_create_pipeline_interval += frames_[i].frame_create_pipeline_interval;
      average_.frame_tile_mapping_interval += frames_[i].frame_tile_mapping_interval;
      average_.frame_cmdlist_record_interval += frames_[i].frame_cmdlist_record_interval;
      average_.frame_execute_validate_interval += frames_[i].frame_execute_validate_interval;
      average_.frame_execute_collect_interval += frames_[i].frame_execute_collect_interval;
      average_.frame_execute_enqueue_interval += frames_[i].frame_execute_enqueue_interval;
      average_.frame_execute_drain_interval += frames_[i].frame_execute_drain_interval;
      average_.frame_execute_drain_lock_interval += frames_[i].frame_execute_drain_lock_interval;
      average_.frame_execute_resource_state_lock_hold_interval +=
          frames_[i].frame_execute_resource_state_lock_hold_interval;
      average_.frame_execute_replay_interval += frames_[i].frame_execute_replay_interval;
      average_.frame_execute_decay_interval += frames_[i].frame_execute_decay_interval;
      average_.frame_execute_signal_interval += frames_[i].frame_execute_signal_interval;
      average_.frame_execute_commit_interval += frames_[i].frame_execute_commit_interval;
      average_.frame_execute_wait_arm_interval += frames_[i].frame_execute_wait_arm_interval;
      average_.frame_replay_record_loop_interval += frames_[i].frame_replay_record_loop_interval;
      average_.frame_replay_superseded_mask_interval += frames_[i].frame_replay_superseded_mask_interval;
      average_.frame_replay_compiled_graphics_interval += frames_[i].frame_replay_compiled_graphics_interval;
      average_.frame_replay_compiled_compute_interval += frames_[i].frame_replay_compiled_compute_interval;
      average_.frame_replay_fallback_classification_interval += frames_[i].frame_replay_fallback_classification_interval;
      average_.frame_replay_typed_record_interval += frames_[i].frame_replay_typed_record_interval;
      average_.frame_replay_flush_pass_interval += frames_[i].frame_replay_flush_pass_interval;
      average_.frame_replay_timestamp_resolve_interval += frames_[i].frame_replay_timestamp_resolve_interval;
      average_.frame_replay_cpu_query_resolve_interval += frames_[i].frame_replay_cpu_query_resolve_interval;
      average_.frame_replay_get_pipeline_interval += frames_[i].frame_replay_get_pipeline_interval;
      average_.frame_replay_get_metal_pso_interval += frames_[i].frame_replay_get_metal_pso_interval;
      average_.frame_replay_select_pso_interval += frames_[i].frame_replay_select_pso_interval;
      average_.frame_replay_desc_access_interval += frames_[i].frame_replay_desc_access_interval;
      average_.frame_replay_state_update_interval += frames_[i].frame_replay_state_update_interval;
      average_.frame_replay_attach_interval += frames_[i].frame_replay_attach_interval;
      average_.frame_replay_bind_snapshot_interval += frames_[i].frame_replay_bind_snapshot_interval;
      average_.frame_replay_estimate_interval += frames_[i].frame_replay_estimate_interval;
      average_.frame_replay_packet_interval += frames_[i].frame_replay_packet_interval;
      average_.frame_replay_queue_interval += frames_[i].frame_replay_queue_interval;
      average_.frame_replay_emit_interval += frames_[i].frame_replay_emit_interval;
      average_.frame_replay_flush_blit_interval += frames_[i].frame_replay_flush_blit_interval;
      average_.frame_replay_flush_compute_interval += frames_[i].frame_replay_flush_compute_interval;
      average_.frame_replay_flush_graphics_interval += frames_[i].frame_replay_flush_graphics_interval;
      average_.frame_replay_flush_barrier_interval += frames_[i].frame_replay_flush_barrier_interval;
      average_.frame_replay_build_plan_interval += frames_[i].frame_replay_build_plan_interval;
      average_.frame_replay_emit_timestamp_interval += frames_[i].frame_replay_emit_timestamp_interval;
      average_.frame_replay_record_draw_interval += frames_[i].frame_replay_record_draw_interval;
      average_.frame_replay_record_draw_indexed_interval += frames_[i].frame_replay_record_draw_indexed_interval;
      average_.frame_replay_record_dispatch_interval += frames_[i].frame_replay_record_dispatch_interval;
      average_.frame_replay_record_pipeline_state_interval += frames_[i].frame_replay_record_pipeline_state_interval;
      average_.frame_replay_record_descriptor_heaps_interval += frames_[i].frame_replay_record_descriptor_heaps_interval;
      average_.frame_replay_record_root_signature_interval += frames_[i].frame_replay_record_root_signature_interval;
      average_.frame_replay_record_root_table_interval += frames_[i].frame_replay_record_root_table_interval;
      average_.frame_replay_record_root_descriptor_interval += frames_[i].frame_replay_record_root_descriptor_interval;
      average_.frame_replay_record_root_constants_interval += frames_[i].frame_replay_record_root_constants_interval;
      average_.frame_replay_record_vertex_index_state_interval += frames_[i].frame_replay_record_vertex_index_state_interval;
      average_.frame_replay_record_render_targets_interval += frames_[i].frame_replay_record_render_targets_interval;
      average_.frame_replay_record_resource_barrier_interval += frames_[i].frame_replay_record_resource_barrier_interval;
      average_.frame_replay_record_copy_clear_resolve_interval += frames_[i].frame_replay_record_copy_clear_resolve_interval;
      average_.frame_replay_record_query_interval += frames_[i].frame_replay_record_query_interval;
      average_.frame_replay_record_execute_indirect_interval += frames_[i].frame_replay_record_execute_indirect_interval;
      average_.frame_replay_record_temporal_upscale_interval += frames_[i].frame_replay_record_temporal_upscale_interval;
      average_.frame_compiled_pass_build_interval += frames_[i].frame_compiled_pass_build_interval;
      average_.frame_compiled_draw_encode_interval += frames_[i].frame_compiled_draw_encode_interval;
      average_.frame_compiled_draw_direct_encode_interval += frames_[i].frame_compiled_draw_direct_encode_interval;
      average_.frame_compiled_draw_replay_encode_interval += frames_[i].frame_compiled_draw_replay_encode_interval;
      average_.frame_compiled_draw_common_interval += frames_[i].frame_compiled_draw_common_interval;
      average_.frame_compiled_draw_retain_interval += frames_[i].frame_compiled_draw_retain_interval;
      average_.frame_compiled_draw_pipeline_interval += frames_[i].frame_compiled_draw_pipeline_interval;
      average_.frame_compiled_draw_dsso_interval += frames_[i].frame_compiled_draw_dsso_interval;
      average_.frame_compiled_draw_rasterizer_interval += frames_[i].frame_compiled_draw_rasterizer_interval;
      average_.frame_compiled_draw_binding_gate_interval += frames_[i].frame_compiled_draw_binding_gate_interval;
      average_.frame_compiled_draw_binding_snapshot_interval += frames_[i].frame_compiled_draw_binding_snapshot_interval;
      average_.frame_compiled_draw_dynamic_state_interval += frames_[i].frame_compiled_draw_dynamic_state_interval;
      average_.frame_compiled_draw_visibility_interval += frames_[i].frame_compiled_draw_visibility_interval;
      average_.frame_compiled_draw_body_interval += frames_[i].frame_compiled_draw_body_interval;
      average_.frame_compiled_snapshot_static_samplers_interval += frames_[i].frame_compiled_snapshot_static_samplers_interval;
      average_.frame_compiled_snapshot_entries_interval += frames_[i].frame_compiled_snapshot_entries_interval;
      average_.frame_compiled_snapshot_root_constants_interval += frames_[i].frame_compiled_snapshot_root_constants_interval;
      average_.frame_compiled_snapshot_descriptors_interval += frames_[i].frame_compiled_snapshot_descriptors_interval;
      average_.frame_compiled_snapshot_clear_descriptors_interval += frames_[i].frame_compiled_snapshot_clear_descriptors_interval;
      average_.frame_compiled_snapshot_bindless_fill_interval += frames_[i].frame_compiled_snapshot_bindless_fill_interval;
      average_.frame_compiled_snapshot_vertex_buffers_interval += frames_[i].frame_compiled_snapshot_vertex_buffers_interval;
      average_.frame_compiled_snapshot_vertex_table_interval += frames_[i].frame_compiled_snapshot_vertex_table_interval;
      average_.frame_compiled_snapshot_restore_argbuf_interval += frames_[i].frame_compiled_snapshot_restore_argbuf_interval;
      average_.frame_compiled_snapshot_shader_bindings_interval += frames_[i].frame_compiled_snapshot_shader_bindings_interval;
      average_.frame_compiled_snapshot_bindless_shader_bindings_interval += frames_[i].frame_compiled_snapshot_bindless_shader_bindings_interval;
      average_.frame_compiled_snapshot_legacy_shader_bindings_interval += frames_[i].frame_compiled_snapshot_legacy_shader_bindings_interval;
      average_.frame_compiled_dispatch_encode_interval += frames_[i].frame_compiled_dispatch_encode_interval;
      average_.frame_argument_table_update_interval += frames_[i].frame_argument_table_update_interval;
      average_.frame_argument_table_bind_interval += frames_[i].frame_argument_table_bind_interval;
      average_.frame_residency_submit_interval += frames_[i].frame_residency_submit_interval;
      average_.frame_pso_cache_lookup_interval += frames_[i].frame_pso_cache_lookup_interval;
      average_.frame_pso_materialize_interval += frames_[i].frame_pso_materialize_interval;
      average_.frame_pso_compile_wait_interval += frames_[i].frame_pso_compile_wait_interval;
      for (size_t path = 0; path < kPerfCodePathCount; path++) {
        average_.frame_code_path_intervals[path] +=
            frames_[i].frame_code_path_intervals[path];
        average_.frame_code_path_max_intervals[path] = std::max(
            average_.frame_code_path_max_intervals[path],
            frames_[i].frame_code_path_max_intervals[path]);
        average_.frame_code_path_counts[path] +=
            frames_[i].frame_code_path_counts[path];
      }
      average_.frame_replay_draw_count += frames_[i].frame_replay_draw_count;
      average_.frame_replay_compiled_graphics_packet_count += frames_[i].frame_replay_compiled_graphics_packet_count;
      average_.frame_replay_compiled_compute_packet_count += frames_[i].frame_replay_compiled_compute_packet_count;
      average_.frame_replay_fallback_classification_count += frames_[i].frame_replay_fallback_classification_count;
      average_.frame_replay_record_draw_count += frames_[i].frame_replay_record_draw_count;
      average_.frame_replay_record_draw_indexed_count += frames_[i].frame_replay_record_draw_indexed_count;
      average_.frame_replay_record_dispatch_count += frames_[i].frame_replay_record_dispatch_count;
      average_.frame_replay_record_pipeline_state_count += frames_[i].frame_replay_record_pipeline_state_count;
      average_.frame_replay_record_descriptor_heaps_count += frames_[i].frame_replay_record_descriptor_heaps_count;
      average_.frame_replay_record_root_signature_count += frames_[i].frame_replay_record_root_signature_count;
      average_.frame_replay_record_root_table_count += frames_[i].frame_replay_record_root_table_count;
      average_.frame_replay_record_root_descriptor_count += frames_[i].frame_replay_record_root_descriptor_count;
      average_.frame_replay_record_root_constants_count += frames_[i].frame_replay_record_root_constants_count;
      average_.frame_replay_record_vertex_index_state_count += frames_[i].frame_replay_record_vertex_index_state_count;
      average_.frame_replay_record_render_targets_count += frames_[i].frame_replay_record_render_targets_count;
      average_.frame_replay_record_resource_barrier_count += frames_[i].frame_replay_record_resource_barrier_count;
      average_.frame_replay_record_copy_clear_resolve_count += frames_[i].frame_replay_record_copy_clear_resolve_count;
      average_.frame_replay_record_query_count += frames_[i].frame_replay_record_query_count;
      average_.frame_replay_record_execute_indirect_count += frames_[i].frame_replay_record_execute_indirect_count;
      average_.frame_replay_record_temporal_upscale_count += frames_[i].frame_replay_record_temporal_upscale_count;
      average_.frame_replay_pso_root_unchanged += frames_[i].frame_replay_pso_root_unchanged;
      average_.frame_replay_full_bind_unchanged += frames_[i].frame_replay_full_bind_unchanged;
      average_.frame_replay_flush_blit_count += frames_[i].frame_replay_flush_blit_count;
      average_.frame_replay_flush_compute_count += frames_[i].frame_replay_flush_compute_count;
      average_.frame_replay_flush_graphics_count += frames_[i].frame_replay_flush_graphics_count;
      average_.frame_replay_flush_barrier_count += frames_[i].frame_replay_flush_barrier_count;
      average_.frame_replay_emit_timestamp_count += frames_[i].frame_replay_emit_timestamp_count;
      average_.frame_replay_flush_pass_batches_count += frames_[i].frame_replay_flush_pass_batches_count;
      average_.frame_replay_flush_pass_batches_empty += frames_[i].frame_replay_flush_pass_batches_empty;
      average_.frame_replay_resource_access_count += frames_[i].frame_replay_resource_access_count;
      average_.frame_replay_resource_access_steady_noop += frames_[i].frame_replay_resource_access_steady_noop;
      average_.frame_replay_desc_access_hit += frames_[i].frame_replay_desc_access_hit;
      average_.frame_replay_desc_access_miss += frames_[i].frame_replay_desc_access_miss;
      average_.frame_replay_desc_access_passthrough += frames_[i].frame_replay_desc_access_passthrough;
      average_.frame_replay_superseded_state_records_skipped += frames_[i].frame_replay_superseded_state_records_skipped;
      average_.frame_replay_compiled_graphics_candidates += frames_[i].frame_replay_compiled_graphics_candidates;
      average_.frame_replay_compiled_graphics_legacy += frames_[i].frame_replay_compiled_graphics_legacy;
      average_.frame_replay_compiled_graphics_barriers += frames_[i].frame_replay_compiled_graphics_barriers;
      average_.frame_replay_compiled_graphics_gs_ts += frames_[i].frame_replay_compiled_graphics_gs_ts;
      average_.frame_replay_compiled_graphics_indirect += frames_[i].frame_replay_compiled_graphics_indirect;
      average_.frame_compiled_graphics_packets += frames_[i].frame_compiled_graphics_packets;
      average_.frame_compiled_draw_direct_packets += frames_[i].frame_compiled_draw_direct_packets;
      average_.frame_compiled_draw_replay_packets += frames_[i].frame_compiled_draw_replay_packets;
      average_.frame_compiled_draw_indexed_packets += frames_[i].frame_compiled_draw_indexed_packets;
      average_.frame_compiled_draw_nonindexed_packets += frames_[i].frame_compiled_draw_nonindexed_packets;
      average_.frame_compiled_draw_binding_snapshot_applied += frames_[i].frame_compiled_draw_binding_snapshot_applied;
      average_.frame_compiled_draw_binding_snapshot_skipped += frames_[i].frame_compiled_draw_binding_snapshot_skipped;
      average_.frame_compiled_draw_binding_generation_hits += frames_[i].frame_compiled_draw_binding_generation_hits;
      average_.frame_compiled_draw_binding_fingerprint_hits += frames_[i].frame_compiled_draw_binding_fingerprint_hits;
      average_.frame_compiled_draw_binding_misses += frames_[i].frame_compiled_draw_binding_misses;
      average_.frame_compiled_snapshot_entries += frames_[i].frame_compiled_snapshot_entries;
      average_.frame_compiled_snapshot_root_constants += frames_[i].frame_compiled_snapshot_root_constants;
      average_.frame_compiled_snapshot_descriptors += frames_[i].frame_compiled_snapshot_descriptors;
      average_.frame_compiled_snapshot_clear_descriptors += frames_[i].frame_compiled_snapshot_clear_descriptors;
      average_.frame_compiled_snapshot_bindless_fills += frames_[i].frame_compiled_snapshot_bindless_fills;
      average_.frame_compiled_snapshot_bindless_fill_texture += frames_[i].frame_compiled_snapshot_bindless_fill_texture;
      average_.frame_compiled_snapshot_bindless_fill_sampler += frames_[i].frame_compiled_snapshot_bindless_fill_sampler;
      average_.frame_compiled_snapshot_bindless_fill_texture_buffer += frames_[i].frame_compiled_snapshot_bindless_fill_texture_buffer;
      average_.frame_compiled_snapshot_bindless_fill_null += frames_[i].frame_compiled_snapshot_bindless_fill_null;
      average_.frame_compiled_snapshot_vertex_buffers += frames_[i].frame_compiled_snapshot_vertex_buffers;
      average_.frame_compiled_snapshot_shader_bindings += frames_[i].frame_compiled_snapshot_shader_bindings;
      average_.frame_compiled_snapshot_bindless_shader_bindings += frames_[i].frame_compiled_snapshot_bindless_shader_bindings;
      average_.frame_compiled_snapshot_legacy_shader_bindings += frames_[i].frame_compiled_snapshot_legacy_shader_bindings;
      average_.frame_compiled_snapshot_legacy_argbuf_restores += frames_[i].frame_compiled_snapshot_legacy_argbuf_restores;
      average_.frame_fallback_graphics_packets += frames_[i].frame_fallback_graphics_packets;
      average_.frame_compiled_compute_packets += frames_[i].frame_compiled_compute_packets;
      average_.frame_fallback_compute_packets += frames_[i].frame_fallback_compute_packets;
      average_.frame_state_records_elided += frames_[i].frame_state_records_elided;
      for (size_t reason = 0; reason < kCompiledFallbackReasonCount; reason++)
        average_.frame_compiled_fallback_reasons[reason] +=
            frames_[i].frame_compiled_fallback_reasons[reason];
      average_.frame_replay_mismatch_barriers += frames_[i].frame_replay_mismatch_barriers;
      average_.frame_replay_app_barrier_transitions += frames_[i].frame_replay_app_barrier_transitions;
      average_.frame_replay_binding_gen_bumps += frames_[i].frame_replay_binding_gen_bumps;
      average_.frame_replay_binding_gen_pipeline += frames_[i].frame_replay_binding_gen_pipeline;
      average_.frame_replay_binding_gen_descriptor_heaps += frames_[i].frame_replay_binding_gen_descriptor_heaps;
      average_.frame_replay_binding_gen_vertex_buffers += frames_[i].frame_replay_binding_gen_vertex_buffers;
      average_.frame_replay_binding_gen_root_signature += frames_[i].frame_replay_binding_gen_root_signature;
      average_.frame_replay_binding_gen_root_descriptor_table += frames_[i].frame_replay_binding_gen_root_descriptor_table;
      average_.frame_replay_binding_gen_root_descriptor += frames_[i].frame_replay_binding_gen_root_descriptor;
      average_.frame_replay_binding_gen_root_constants += frames_[i].frame_replay_binding_gen_root_constants;
      average_.frame_replay_binding_gen_indirect_vertex_buffer += frames_[i].frame_replay_binding_gen_indirect_vertex_buffer;
      average_.frame_replay_binding_gen_indirect_root_constants += frames_[i].frame_replay_binding_gen_indirect_root_constants;
      average_.frame_replay_binding_gen_indirect_root_descriptor += frames_[i].frame_replay_binding_gen_indirect_root_descriptor;
      average_.frame_replay_snapshot_requests += frames_[i].frame_replay_snapshot_requests;
      average_.frame_replay_snapshot_cache_hits += frames_[i].frame_replay_snapshot_cache_hits;
      average_.frame_replay_snapshot_cache_misses += frames_[i].frame_replay_snapshot_cache_misses;
      average_.frame_replay_snapshot_passthrough += frames_[i].frame_replay_snapshot_passthrough;
      average_.frame_replay_snapshot_graphics_gen_changes += frames_[i].frame_replay_snapshot_graphics_gen_changes;
      average_.frame_replay_snapshot_descriptor_gen_changes += frames_[i].frame_replay_snapshot_descriptor_gen_changes;
      average_.frame_replay_snapshot_both_gen_changes += frames_[i].frame_replay_snapshot_both_gen_changes;
      average_.frame_replay_snapshot_no_gen_changes += frames_[i].frame_replay_snapshot_no_gen_changes;
      average_.frame_replay_snapshot_captured_entries += frames_[i].frame_replay_snapshot_captured_entries;
      average_.frame_replay_snapshot_captured_descriptors += frames_[i].frame_replay_snapshot_captured_descriptors;
      average_.frame_replay_snapshot_captured_missing_descriptors += frames_[i].frame_replay_snapshot_captured_missing_descriptors;
      average_.frame_replay_snapshot_captured_root_descriptors += frames_[i].frame_replay_snapshot_captured_root_descriptors;
      average_.frame_replay_snapshot_captured_root_constants += frames_[i].frame_replay_snapshot_captured_root_constants;
      average_.frame_replay_snapshot_captured_vertex_buffers += frames_[i].frame_replay_snapshot_captured_vertex_buffers;
      average_.frame_replay_snapshot_captured_bindless += frames_[i].frame_replay_snapshot_captured_bindless;
      average_.frame_native_descriptor_root_tables += frames_[i].frame_native_descriptor_root_tables;
      average_.frame_native_descriptor_root_table_backend_ready += frames_[i].frame_native_descriptor_root_table_backend_ready;
      average_.frame_native_descriptor_record_storage_ready += frames_[i].frame_native_descriptor_record_storage_ready;
      average_.frame_native_descriptor_resource_root_tables += frames_[i].frame_native_descriptor_resource_root_tables;
      average_.frame_native_descriptor_sampler_root_tables += frames_[i].frame_native_descriptor_sampler_root_tables;
      average_.frame_native_descriptor_buffer_records += frames_[i].frame_native_descriptor_buffer_records;
      average_.frame_native_descriptor_buffer_record_cbv += frames_[i].frame_native_descriptor_buffer_record_cbv;
      average_.frame_native_descriptor_buffer_record_srv += frames_[i].frame_native_descriptor_buffer_record_srv;
      average_.frame_native_descriptor_buffer_record_uav += frames_[i].frame_native_descriptor_buffer_record_uav;
      average_.frame_native_descriptor_buffer_record_counters += frames_[i].frame_native_descriptor_buffer_record_counters;
      average_.frame_native_descriptor_buffer_record_missing_resource += frames_[i].frame_native_descriptor_buffer_record_missing_resource;
      average_.frame_native_descriptor_resource_table_entries += frames_[i].frame_native_descriptor_resource_table_entries;
      average_.frame_generic_descriptor_span_lookups += frames_[i].frame_generic_descriptor_span_lookups;
      average_.frame_generic_descriptor_span_unique += frames_[i].frame_generic_descriptor_span_unique;
      average_.frame_generic_descriptor_span_reuses += frames_[i].frame_generic_descriptor_span_reuses;
      average_.frame_frozen_native_direct_packets += frames_[i].frame_frozen_native_direct_packets;
      average_.frame_frozen_range_lookups += frames_[i].frame_frozen_range_lookups;
      average_.frame_frozen_range_unique += frames_[i].frame_frozen_range_unique;
      average_.frame_frozen_range_reuses += frames_[i].frame_frozen_range_reuses;
      average_.frame_frozen_root_word_lookups += frames_[i].frame_frozen_root_word_lookups;
      average_.frame_frozen_root_word_reuses += frames_[i].frame_frozen_root_word_reuses;
      average_.frame_submission_graphics_encoder_boundary_merges +=
          frames_[i].frame_submission_graphics_encoder_boundary_merges;
      average_.frame_submission_compute_encoder_boundary_merges +=
          frames_[i].frame_submission_compute_encoder_boundary_merges;
      average_.frame_submission_encoder_boundary_flushes +=
          frames_[i].frame_submission_encoder_boundary_flushes;
      average_.frame_descriptor_content_writes += frames_[i].frame_descriptor_content_writes;
      average_.frame_descriptor_content_write_cbv += frames_[i].frame_descriptor_content_write_cbv;
      average_.frame_descriptor_content_write_srv += frames_[i].frame_descriptor_content_write_srv;
      average_.frame_descriptor_content_write_uav += frames_[i].frame_descriptor_content_write_uav;
      average_.frame_descriptor_content_write_sampler += frames_[i].frame_descriptor_content_write_sampler;
      average_.frame_descriptor_content_write_copy += frames_[i].frame_descriptor_content_write_copy;
      average_.frame_descriptor_content_write_feedback_uav += frames_[i].frame_descriptor_content_write_feedback_uav;
      average_.frame_wall_interval += frames_[i].frame_wall_interval;
      average_.frame_resource_lock_interval += frames_[i].frame_resource_lock_interval;
      average_.frame_staging_upload_interval += frames_[i].frame_staging_upload_interval;
      average_.frame_chunk_commit_interval += frames_[i].frame_chunk_commit_interval;
      average_.frame_draw_record_interval += frames_[i].frame_draw_record_interval;
      average_.frame_draw_resolve_interval += frames_[i].frame_draw_resolve_interval;
      average_.frame_draw_emit_interval += frames_[i].frame_draw_emit_interval;
      average_.frame_resource_lock_count += frames_[i].frame_resource_lock_count;
      average_.frame_staging_upload_bytes += frames_[i].frame_staging_upload_bytes;
      average_.frame_buffer_refresh_bytes += frames_[i].frame_buffer_refresh_bytes;
      average_.frame_buffer_refresh_count += frames_[i].frame_buffer_refresh_count;
      average_.frame_chunk_commit_count += frames_[i].frame_chunk_commit_count;
      average_.frame_draw_resolve_count += frames_[i].frame_draw_resolve_count;
      average_.frame_create_resource_count += frames_[i].frame_create_resource_count;
      average_.frame_pso_compile_wait_count += frames_[i].frame_pso_compile_wait_count;
      average_.frame_draw_issue_count += frames_[i].frame_draw_issue_count;
      average_.frame_thread_user_interval += frames_[i].frame_thread_user_interval;
      average_.frame_thread_kernel_interval += frames_[i].frame_thread_kernel_interval;
      average_.frame_task_fault_count += frames_[i].frame_task_fault_count;
      average_.frame_task_pagein_count += frames_[i].frame_task_pagein_count;
      average_.frame_task_cow_fault_count += frames_[i].frame_task_cow_fault_count;
      average_.frame_host_zero_fill_count += frames_[i].frame_host_zero_fill_count;
      average_.frame_query_get_data_count += frames_[i].frame_query_get_data_count;
      average_.frame_query_get_data_pending_count += frames_[i].frame_query_get_data_pending_count;
      average_.frame_query_issue_count += frames_[i].frame_query_issue_count;
      average_.frame_raster_status_poll_count += frames_[i].frame_raster_status_poll_count;
      average_.frame_device_status_poll_count += frames_[i].frame_device_status_poll_count;
    }
    average_.command_buffer_count /= (kFrameStatisticsCount - 1);
    average_.render_pass_count /= (kFrameStatisticsCount - 1);
    average_.render_pass_optimized /= (kFrameStatisticsCount - 1);
    average_.render_command_count /= (kFrameStatisticsCount - 1);
    average_.render_pso_bind_count /= (kFrameStatisticsCount - 1);
    average_.render_draw_count /= (kFrameStatisticsCount - 1);
    average_.render_indexed_draw_count /= (kFrameStatisticsCount - 1);
    average_.render_indirect_draw_count /= (kFrameStatisticsCount - 1);
    average_.render_mesh_draw_count /= (kFrameStatisticsCount - 1);
    average_.render_tile_dispatch_count /= (kFrameStatisticsCount - 1);
    average_.render_color_load_count /= (kFrameStatisticsCount - 1);
    average_.render_color_store_count /= (kFrameStatisticsCount - 1);
    average_.render_depth_load_count /= (kFrameStatisticsCount - 1);
    average_.render_depth_store_count /= (kFrameStatisticsCount - 1);
    average_.render_stencil_load_count /= (kFrameStatisticsCount - 1);
    average_.render_stencil_store_count /= (kFrameStatisticsCount - 1);
    average_.render_attachment_count /= (kFrameStatisticsCount - 1);
    average_.render_attachment_reuse_count /= (kFrameStatisticsCount - 1);
    average_.render_attachment_reload_count /= (kFrameStatisticsCount - 1);
    average_.render_attachment_store_then_load_count /= (kFrameStatisticsCount - 1);
    average_.encoder_switch_render_compute_count /= (kFrameStatisticsCount - 1);
    average_.encoder_switch_compute_render_count /= (kFrameStatisticsCount - 1);
    average_.encoder_switch_to_other_count /= (kFrameStatisticsCount - 1);
    average_.present_pass_count /= (kFrameStatisticsCount - 1);
    average_.clear_pass_count /= (kFrameStatisticsCount - 1);
    average_.clear_pass_optimized /= (kFrameStatisticsCount - 1);
    average_.resolve_pass_optimized /= (kFrameStatisticsCount - 1);
    average_.compute_pass_count /= (kFrameStatisticsCount - 1);
    average_.compute_pass_optimized /= (kFrameStatisticsCount - 1);
    average_.compute_pass_with_dispatch_count /= (kFrameStatisticsCount - 1);
    average_.compute_pass_without_dispatch_count /= (kFrameStatisticsCount - 1);
    average_.compute_command_count /= (kFrameStatisticsCount - 1);
    average_.compute_pso_bind_count /= (kFrameStatisticsCount - 1);
    average_.compute_set_buffer_count /= (kFrameStatisticsCount - 1);
    average_.compute_set_texture_count /= (kFrameStatisticsCount - 1);
    average_.compute_set_bytes_count /= (kFrameStatisticsCount - 1);
    average_.compute_use_resource_count /= (kFrameStatisticsCount - 1);
    average_.compute_dispatch_count /= (kFrameStatisticsCount - 1);
    average_.compute_dispatch_indirect_count /= (kFrameStatisticsCount - 1);
    average_.compute_memory_barrier_count /= (kFrameStatisticsCount - 1);
    average_.compute_fence_wait_count /= (kFrameStatisticsCount - 1);
    average_.compute_fence_update_count /= (kFrameStatisticsCount - 1);
    average_.blit_pass_count /= (kFrameStatisticsCount - 1);
    average_.blit_pass_optimized /= (kFrameStatisticsCount - 1);
    average_.blit_pass_with_commands_count /= (kFrameStatisticsCount - 1);
    average_.blit_pass_empty_count /= (kFrameStatisticsCount - 1);
    average_.blit_pass_deferred_fence_only_count /= (kFrameStatisticsCount - 1);
    average_.blit_pass_merged_fence_only_count /= (kFrameStatisticsCount - 1);
    average_.blit_barrier_only_pass_count /= (kFrameStatisticsCount - 1);
    average_.blit_separator_pass_count /= (kFrameStatisticsCount - 1);
    average_.resource_barrier_batches_merged /= (kFrameStatisticsCount - 1);
    average_.resource_barrier_batches_graphics_inlined /= (kFrameStatisticsCount - 1);
    average_.resource_barrier_batches_compute_inlined /= (kFrameStatisticsCount - 1);
    average_.blit_pass_with_fence_wait_count /= (kFrameStatisticsCount - 1);
    average_.blit_pass_with_fence_update_count /= (kFrameStatisticsCount - 1);
    average_.blit_fence_wait_entry_count /= (kFrameStatisticsCount - 1);
    average_.blit_fence_update_entry_count /= (kFrameStatisticsCount - 1);
    average_.blit_command_count /= (kFrameStatisticsCount - 1);
    average_.blit_copy_buffer_to_buffer_count /= (kFrameStatisticsCount - 1);
    average_.blit_copy_buffer_to_texture_count /= (kFrameStatisticsCount - 1);
    average_.blit_copy_texture_to_buffer_count /= (kFrameStatisticsCount - 1);
    average_.blit_copy_texture_to_texture_count /= (kFrameStatisticsCount - 1);
    average_.blit_generate_mipmaps_count /= (kFrameStatisticsCount - 1);
    average_.blit_fence_wait_count /= (kFrameStatisticsCount - 1);
    average_.blit_fence_update_count /= (kFrameStatisticsCount - 1);
    average_.blit_fill_buffer_count /= (kFrameStatisticsCount - 1);
    average_.blit_resolve_counters_count /= (kFrameStatisticsCount - 1);
    average_.sync_count /= (kFrameStatisticsCount - 1);
    average_.event_stall /= (kFrameStatisticsCount - 1);
    average_.latency /= (kFrameStatisticsCount - 1);
    average_.flush_chunk_count /= (kFrameStatisticsCount - 1);
    average_.flush_empty_chunk_count /= (kFrameStatisticsCount - 1);
    average_.flush_event_only_chunk_count /= (kFrameStatisticsCount - 1);
    average_.flush_signal_chunk_count /= (kFrameStatisticsCount - 1);
    average_.flush_wait_chunk_count /= (kFrameStatisticsCount - 1);
    average_.flush_present_chunk_count /= (kFrameStatisticsCount - 1);
    average_.flush_encoder_count /= (kFrameStatisticsCount - 1);
    average_.flush_encoded_encoder_count /= (kFrameStatisticsCount - 1);
    average_.flush_null_encoder_count /= (kFrameStatisticsCount - 1);
    average_.flush_render_encoder_count /= (kFrameStatisticsCount - 1);
    average_.flush_compute_encoder_count /= (kFrameStatisticsCount - 1);
    average_.flush_blit_encoder_count /= (kFrameStatisticsCount - 1);
    average_.flush_timestamp_encoder_count /= (kFrameStatisticsCount - 1);
    average_.flush_total_interval /= (kFrameStatisticsCount - 1);
    average_.flush_collect_interval /= (kFrameStatisticsCount - 1);
    average_.flush_relation_interval /= (kFrameStatisticsCount - 1);
    average_.flush_query_interval /= (kFrameStatisticsCount - 1);
    average_.flush_timestamp_interval /= (kFrameStatisticsCount - 1);
    average_.flush_render_interval /= (kFrameStatisticsCount - 1);
    average_.flush_compute_interval /= (kFrameStatisticsCount - 1);
    average_.flush_blit_interval /= (kFrameStatisticsCount - 1);
    average_.flush_clear_interval /= (kFrameStatisticsCount - 1);
    average_.flush_resolve_interval /= (kFrameStatisticsCount - 1);
    average_.flush_scaler_interval /= (kFrameStatisticsCount - 1);
    average_.flush_present_interval /= (kFrameStatisticsCount - 1);
    average_.flush_event_interval /= (kFrameStatisticsCount - 1);
    average_.flush_sample_interval /= (kFrameStatisticsCount - 1);
    average_.flush_pending_fence_interval /= (kFrameStatisticsCount - 1);
    average_.flush_signal_interval /= (kFrameStatisticsCount - 1);
    average_.flush_cleanup_interval /= (kFrameStatisticsCount - 1);
    average_.commit_interval /= (kFrameStatisticsCount - 1);
    average_.sync_interval /= (kFrameStatisticsCount - 1);
    average_.encode_prepare_interval /= (kFrameStatisticsCount - 1);
    average_.encode_flush_interval /= (kFrameStatisticsCount - 1);
    average_.drawable_blocking_interval /= (kFrameStatisticsCount - 1);
    average_.present_latency_interval /= (kFrameStatisticsCount - 1);
    average_.shader_binding_upload_count /= (kFrameStatisticsCount - 1);
    average_.shader_binding_dirty_cbuffer_count /= (kFrameStatisticsCount - 1);
    average_.shader_binding_dirty_sampler_count /= (kFrameStatisticsCount - 1);
    average_.shader_binding_dirty_srv_count /= (kFrameStatisticsCount - 1);
    average_.shader_binding_dirty_uav_count /= (kFrameStatisticsCount - 1);
    average_.shader_binding_clean_uav_count /= (kFrameStatisticsCount - 1);
    average_.frame_execute_command_lists_interval /= (kFrameStatisticsCount - 1);
    average_.frame_present_interval /= (kFrameStatisticsCount - 1);
    average_.frame_present_queue_wait_interval /=
        (kFrameStatisticsCount - 1);
    average_.frame_queue_signal_interval /= (kFrameStatisticsCount - 1);
    average_.frame_queue_wait_interval /= (kFrameStatisticsCount - 1);
    average_.frame_create_resource_interval /= (kFrameStatisticsCount - 1);
    average_.frame_create_reserved_resource_interval /= (kFrameStatisticsCount - 1);
    average_.frame_create_heap_interval /= (kFrameStatisticsCount - 1);
    average_.frame_create_pipeline_interval /= (kFrameStatisticsCount - 1);
    average_.frame_tile_mapping_interval /= (kFrameStatisticsCount - 1);
    average_.frame_cmdlist_record_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_validate_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_collect_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_enqueue_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_drain_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_drain_lock_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_resource_state_lock_hold_interval /=
        (kFrameStatisticsCount - 1);
    average_.frame_execute_replay_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_decay_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_signal_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_commit_interval /= (kFrameStatisticsCount - 1);
    average_.frame_execute_wait_arm_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_loop_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_superseded_mask_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_graphics_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_compute_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_fallback_classification_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_typed_record_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_pass_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_timestamp_resolve_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_cpu_query_resolve_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_get_pipeline_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_get_metal_pso_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_select_pso_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_desc_access_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_state_update_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_attach_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_bind_snapshot_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_estimate_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_packet_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_queue_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_emit_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_blit_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_compute_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_graphics_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_barrier_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_build_plan_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_emit_timestamp_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_draw_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_draw_indexed_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_dispatch_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_pipeline_state_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_descriptor_heaps_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_root_signature_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_root_table_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_root_descriptor_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_root_constants_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_vertex_index_state_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_render_targets_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_resource_barrier_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_copy_clear_resolve_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_query_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_execute_indirect_interval /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_temporal_upscale_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_pass_build_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_encode_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_direct_encode_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_replay_encode_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_common_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_retain_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_pipeline_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_dsso_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_rasterizer_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_binding_gate_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_binding_snapshot_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_dynamic_state_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_visibility_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_body_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_static_samplers_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_entries_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_root_constants_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_descriptors_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_clear_descriptors_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_bindless_fill_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_vertex_buffers_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_vertex_table_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_restore_argbuf_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_shader_bindings_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_bindless_shader_bindings_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_legacy_shader_bindings_interval /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_dispatch_encode_interval /= (kFrameStatisticsCount - 1);
    average_.frame_argument_table_update_interval /= (kFrameStatisticsCount - 1);
    average_.frame_argument_table_bind_interval /= (kFrameStatisticsCount - 1);
    average_.frame_residency_submit_interval /= (kFrameStatisticsCount - 1);
    average_.frame_pso_cache_lookup_interval /= (kFrameStatisticsCount - 1);
    average_.frame_pso_materialize_interval /= (kFrameStatisticsCount - 1);
    average_.frame_pso_compile_wait_interval /= (kFrameStatisticsCount - 1);
    for (size_t path = 0; path < kPerfCodePathCount; path++) {
      average_.frame_code_path_intervals[path] /=
          (kFrameStatisticsCount - 1);
      average_.frame_code_path_counts[path] /=
          (kFrameStatisticsCount - 1);
    }
    average_.frame_replay_draw_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_graphics_packet_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_compute_packet_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_fallback_classification_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_draw_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_draw_indexed_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_dispatch_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_pipeline_state_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_descriptor_heaps_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_root_signature_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_root_table_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_root_descriptor_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_root_constants_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_vertex_index_state_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_render_targets_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_resource_barrier_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_copy_clear_resolve_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_query_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_execute_indirect_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_record_temporal_upscale_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_pso_root_unchanged /= (kFrameStatisticsCount - 1);
    average_.frame_replay_full_bind_unchanged /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_blit_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_compute_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_graphics_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_barrier_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_emit_timestamp_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_pass_batches_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_flush_pass_batches_empty /= (kFrameStatisticsCount - 1);
    average_.frame_replay_resource_access_count /= (kFrameStatisticsCount - 1);
    average_.frame_replay_resource_access_steady_noop /= (kFrameStatisticsCount - 1);
    average_.frame_replay_desc_access_hit /= (kFrameStatisticsCount - 1);
    average_.frame_replay_desc_access_miss /= (kFrameStatisticsCount - 1);
    average_.frame_replay_desc_access_passthrough /= (kFrameStatisticsCount - 1);
    average_.frame_replay_superseded_state_records_skipped /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_graphics_candidates /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_graphics_legacy /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_graphics_barriers /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_graphics_gs_ts /= (kFrameStatisticsCount - 1);
    average_.frame_replay_compiled_graphics_indirect /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_graphics_packets /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_direct_packets /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_replay_packets /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_indexed_packets /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_nonindexed_packets /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_binding_snapshot_applied /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_binding_snapshot_skipped /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_binding_generation_hits /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_binding_fingerprint_hits /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_draw_binding_misses /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_entries /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_root_constants /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_descriptors /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_clear_descriptors /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_bindless_fills /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_bindless_fill_texture /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_bindless_fill_sampler /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_bindless_fill_texture_buffer /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_bindless_fill_null /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_vertex_buffers /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_shader_bindings /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_bindless_shader_bindings /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_legacy_shader_bindings /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_snapshot_legacy_argbuf_restores /= (kFrameStatisticsCount - 1);
    average_.frame_fallback_graphics_packets /= (kFrameStatisticsCount - 1);
    average_.frame_compiled_compute_packets /= (kFrameStatisticsCount - 1);
    average_.frame_fallback_compute_packets /= (kFrameStatisticsCount - 1);
    average_.frame_state_records_elided /= (kFrameStatisticsCount - 1);
    for (size_t reason = 0; reason < kCompiledFallbackReasonCount; reason++)
      average_.frame_compiled_fallback_reasons[reason] /=
          (kFrameStatisticsCount - 1);
    average_.frame_replay_mismatch_barriers /= (kFrameStatisticsCount - 1);
    average_.frame_replay_app_barrier_transitions /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_bumps /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_pipeline /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_descriptor_heaps /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_vertex_buffers /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_root_signature /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_root_descriptor_table /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_root_descriptor /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_root_constants /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_indirect_vertex_buffer /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_indirect_root_constants /= (kFrameStatisticsCount - 1);
    average_.frame_replay_binding_gen_indirect_root_descriptor /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_requests /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_cache_hits /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_cache_misses /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_passthrough /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_graphics_gen_changes /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_descriptor_gen_changes /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_both_gen_changes /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_no_gen_changes /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_captured_entries /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_captured_descriptors /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_captured_missing_descriptors /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_captured_root_descriptors /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_captured_root_constants /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_captured_vertex_buffers /= (kFrameStatisticsCount - 1);
    average_.frame_replay_snapshot_captured_bindless /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_root_tables /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_root_table_backend_ready /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_record_storage_ready /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_resource_root_tables /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_sampler_root_tables /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_buffer_records /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_buffer_record_cbv /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_buffer_record_srv /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_buffer_record_uav /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_buffer_record_counters /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_buffer_record_missing_resource /= (kFrameStatisticsCount - 1);
    average_.frame_native_descriptor_resource_table_entries /= (kFrameStatisticsCount - 1);
    average_.frame_generic_descriptor_span_lookups /= (kFrameStatisticsCount - 1);
    average_.frame_generic_descriptor_span_unique /= (kFrameStatisticsCount - 1);
    average_.frame_generic_descriptor_span_reuses /= (kFrameStatisticsCount - 1);
    average_.frame_frozen_native_direct_packets /= (kFrameStatisticsCount - 1);
    average_.frame_frozen_range_lookups /= (kFrameStatisticsCount - 1);
    average_.frame_frozen_range_unique /= (kFrameStatisticsCount - 1);
    average_.frame_frozen_range_reuses /= (kFrameStatisticsCount - 1);
    average_.frame_frozen_root_word_lookups /= (kFrameStatisticsCount - 1);
    average_.frame_frozen_root_word_reuses /= (kFrameStatisticsCount - 1);
    average_.frame_submission_graphics_encoder_boundary_merges /=
        (kFrameStatisticsCount - 1);
    average_.frame_submission_compute_encoder_boundary_merges /=
        (kFrameStatisticsCount - 1);
    average_.frame_submission_encoder_boundary_flushes /=
        (kFrameStatisticsCount - 1);
    average_.frame_descriptor_content_writes /= (kFrameStatisticsCount - 1);
    average_.frame_descriptor_content_write_cbv /= (kFrameStatisticsCount - 1);
    average_.frame_descriptor_content_write_srv /= (kFrameStatisticsCount - 1);
    average_.frame_descriptor_content_write_uav /= (kFrameStatisticsCount - 1);
    average_.frame_descriptor_content_write_sampler /= (kFrameStatisticsCount - 1);
    average_.frame_descriptor_content_write_copy /= (kFrameStatisticsCount - 1);
    average_.frame_descriptor_content_write_feedback_uav /= (kFrameStatisticsCount - 1);
    average_.frame_wall_interval /= (kFrameStatisticsCount - 1);
    average_.frame_resource_lock_interval /= (kFrameStatisticsCount - 1);
    average_.frame_staging_upload_interval /= (kFrameStatisticsCount - 1);
    average_.frame_chunk_commit_interval /= (kFrameStatisticsCount - 1);
    average_.frame_draw_record_interval /= (kFrameStatisticsCount - 1);
    average_.frame_draw_resolve_interval /= (kFrameStatisticsCount - 1);
    average_.frame_draw_emit_interval /= (kFrameStatisticsCount - 1);
    average_.frame_resource_lock_count /= (kFrameStatisticsCount - 1);
    average_.frame_staging_upload_bytes /= (kFrameStatisticsCount - 1);
    average_.frame_buffer_refresh_bytes /= (kFrameStatisticsCount - 1);
    average_.frame_buffer_refresh_count /= (kFrameStatisticsCount - 1);
    average_.frame_chunk_commit_count /= (kFrameStatisticsCount - 1);
    average_.frame_draw_resolve_count /= (kFrameStatisticsCount - 1);
    average_.frame_create_resource_count /= (kFrameStatisticsCount - 1);
    average_.frame_pso_compile_wait_count /= (kFrameStatisticsCount - 1);
    average_.frame_draw_issue_count /= (kFrameStatisticsCount - 1);
    average_.frame_thread_user_interval /= (kFrameStatisticsCount - 1);
    average_.frame_thread_kernel_interval /= (kFrameStatisticsCount - 1);
    average_.frame_task_fault_count /= (kFrameStatisticsCount - 1);
    average_.frame_task_pagein_count /= (kFrameStatisticsCount - 1);
    average_.frame_task_cow_fault_count /= (kFrameStatisticsCount - 1);
    average_.frame_host_zero_fill_count /= (kFrameStatisticsCount - 1);
    average_.frame_query_get_data_count /= (kFrameStatisticsCount - 1);
    average_.frame_query_get_data_pending_count /= (kFrameStatisticsCount - 1);
    average_.frame_query_issue_count /= (kFrameStatisticsCount - 1);
    average_.frame_raster_status_poll_count /= (kFrameStatisticsCount - 1);
    average_.frame_device_status_poll_count /= (kFrameStatisticsCount - 1);
  };
};

} // namespace dxmt
