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
  Flags<FeatureCompatibility> compatibility_flags;
  uint32_t command_buffer_count = 0;
  uint32_t sync_count = 0;
  clock::duration sync_interval{};
  clock::duration commit_interval{};
  uint32_t render_pass_count = 0;
  uint32_t render_msaa_pass_count = 0;
  uint64_t render_pixel_count = 0;
  uint64_t render_msaa_pixel_count = 0;
  uint32_t render_max_width = 0;
  uint32_t render_max_height = 0;
  uint32_t render_max_sample_count = 0;
  uint32_t render_pass_optimized = 0;
  uint32_t render_command_count = 0;
  uint32_t render_pso_bind_count = 0;
  uint32_t render_draw_count = 0;
  uint32_t render_indexed_draw_count = 0;
  uint32_t render_indirect_draw_count = 0;
  uint32_t render_mesh_draw_count = 0;
  uint32_t render_tile_dispatch_count = 0;
  uint32_t present_pass_count = 0;
  uint32_t clear_pass_count = 0;
  uint32_t clear_msaa_pass_count = 0;
  uint32_t clear_pass_optimized = 0;
  uint32_t resolve_pass_count = 0;
  uint32_t resolve_shader_pass_count = 0;
  uint32_t resolve_fixed_pass_count = 0;
  uint64_t resolve_pixel_count = 0;
  uint32_t resolve_max_width = 0;
  uint32_t resolve_max_height = 0;
  uint32_t resolve_pass_optimized = 0;
  uint32_t compute_pass_count = 0;
  uint32_t blit_pass_count = 0;
  uint32_t blit_pass_optimized = 0;
  uint32_t blit_merge_blocked_count = 0;
  uint32_t blit_command_count = 0;
  uint32_t blit_copy_buffer_to_buffer_count = 0;
  uint32_t blit_copy_buffer_to_texture_count = 0;
  uint32_t blit_copy_texture_to_buffer_count = 0;
  uint32_t blit_copy_texture_to_texture_count = 0;
  uint32_t blit_generate_mipmaps_count = 0;
  uint32_t blit_fill_buffer_count = 0;
  uint32_t d3d11_blit_switch_count = 0;
  uint32_t d3d11_blit_switch_generic_count = 0;
  uint32_t d3d11_blit_switch_update_count = 0;
  uint32_t d3d11_blit_switch_readback_count = 0;
  uint32_t d3d11_generate_mips_count = 0;
  uint32_t d3d11_resolve_subresource_count = 0;
  uint32_t d3d11_copy_structure_count = 0;
  uint32_t d3d11_copy_buffer_count = 0;
  uint32_t d3d11_copy_texture_count = 0;
  uint32_t d3d11_update_buffer_count = 0;
  uint32_t d3d11_update_buffer_fast_count = 0;
  uint32_t d3d11_update_buffer_fast_invalid_range_count = 0;
  uint32_t d3d11_update_buffer_fast_old_unmapped_count = 0;
  uint32_t d3d11_update_buffer_fast_new_unmapped_count = 0;
  uint32_t d3d11_update_buffer_no_dynamic_count = 0;
  uint32_t d3d11_update_buffer_dynamic_candidate_count = 0;
  uint32_t d3d11_update_buffer_dynamic_full_count = 0;
  uint32_t d3d11_update_buffer_dynamic_no_overwrite_count = 0;
  uint32_t d3d11_update_buffer_bind_constant_count = 0;
  uint32_t d3d11_update_buffer_bind_vertex_count = 0;
  uint32_t d3d11_update_buffer_bind_index_count = 0;
  uint32_t d3d11_update_buffer_bind_srv_count = 0;
  uint32_t d3d11_update_buffer_bind_output_count = 0;
  uint32_t d3d11_update_buffer_small_count = 0;
  uint32_t d3d11_update_buffer_medium_count = 0;
  uint32_t d3d11_update_buffer_large_count = 0;
  uint64_t d3d11_update_buffer_bytes = 0;
  uint64_t d3d11_update_buffer_fast_bytes = 0;
  uint32_t d3d11_update_buffer_deferred_count = 0;
  uint32_t d3d11_update_buffer_deferred_flush_count = 0;
  uint32_t d3d11_update_buffer_deferred_flushed_count = 0;
  uint32_t d3d11_update_buffer_deferred_max_batch = 0;
  uint64_t d3d11_update_buffer_deferred_bytes = 0;
  uint32_t d3d11_update_buffer_deferred_blit_flush_count = 0;
  uint32_t d3d11_update_buffer_deferred_compute_flush_count = 0;
  uint32_t d3d11_update_buffer_deferred_compute_packed_count = 0;
  uint32_t d3d11_update_buffer_deferred_all_flush_count = 0;
  uint32_t d3d11_update_buffer_rename_count = 0;
  uint64_t d3d11_update_buffer_rename_bytes = 0;
  uint32_t d3d11_update_texture_count = 0;
  uint32_t d3d11_invalid_render_target_count = 0;
  uint32_t render_merge_signature_mismatch_count = 0;
  uint32_t render_merge_mismatch_rtv_count = 0;
  uint32_t render_merge_mismatch_dsv_count = 0;
  uint32_t render_merge_mismatch_array_count = 0;
  uint32_t render_merge_mismatch_depth_count = 0;
  uint32_t render_merge_mismatch_stencil_count = 0;
  uint32_t render_merge_mismatch_color_count = 0;
  uint32_t render_merge_mismatch_load_store_count = 0;
  uint32_t render_merge_blocked_vertex_wait_count = 0;
  uint32_t encoder_relation_data_dependency_count = 0;
  uint32_t event_stall = 0;
  uint32_t latency = 0;
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
  ScalerInfo last_scaler_info{};

  void
  reset() {
    compatibility_flags.clrAll();
    command_buffer_count = 0;
    sync_count = 0;
    sync_interval = {};
    commit_interval = {};
    render_pass_count = 0;
    render_msaa_pass_count = 0;
    render_pixel_count = 0;
    render_msaa_pixel_count = 0;
    render_max_width = 0;
    render_max_height = 0;
    render_max_sample_count = 0;
    render_pass_optimized = 0;
    render_command_count = 0;
    render_pso_bind_count = 0;
    render_draw_count = 0;
    render_indexed_draw_count = 0;
    render_indirect_draw_count = 0;
    render_mesh_draw_count = 0;
    render_tile_dispatch_count = 0;
    present_pass_count = 0;
    clear_pass_count = 0;
    clear_msaa_pass_count = 0;
    clear_pass_optimized = 0;
    resolve_pass_count = 0;
    resolve_shader_pass_count = 0;
    resolve_fixed_pass_count = 0;
    resolve_pixel_count = 0;
    resolve_max_width = 0;
    resolve_max_height = 0;
    resolve_pass_optimized = 0;
    compute_pass_count = 0;
    blit_pass_count = 0;
    blit_pass_optimized = 0;
    blit_merge_blocked_count = 0;
    blit_command_count = 0;
    blit_copy_buffer_to_buffer_count = 0;
    blit_copy_buffer_to_texture_count = 0;
    blit_copy_texture_to_buffer_count = 0;
    blit_copy_texture_to_texture_count = 0;
    blit_generate_mipmaps_count = 0;
    blit_fill_buffer_count = 0;
    d3d11_blit_switch_count = 0;
    d3d11_blit_switch_generic_count = 0;
    d3d11_blit_switch_update_count = 0;
    d3d11_blit_switch_readback_count = 0;
    d3d11_generate_mips_count = 0;
    d3d11_resolve_subresource_count = 0;
    d3d11_copy_structure_count = 0;
    d3d11_copy_buffer_count = 0;
    d3d11_copy_texture_count = 0;
    d3d11_update_buffer_count = 0;
    d3d11_update_buffer_fast_count = 0;
    d3d11_update_buffer_fast_invalid_range_count = 0;
    d3d11_update_buffer_fast_old_unmapped_count = 0;
    d3d11_update_buffer_fast_new_unmapped_count = 0;
    d3d11_update_buffer_no_dynamic_count = 0;
    d3d11_update_buffer_dynamic_candidate_count = 0;
    d3d11_update_buffer_dynamic_full_count = 0;
    d3d11_update_buffer_dynamic_no_overwrite_count = 0;
    d3d11_update_buffer_bind_constant_count = 0;
    d3d11_update_buffer_bind_vertex_count = 0;
    d3d11_update_buffer_bind_index_count = 0;
    d3d11_update_buffer_bind_srv_count = 0;
    d3d11_update_buffer_bind_output_count = 0;
    d3d11_update_buffer_small_count = 0;
    d3d11_update_buffer_medium_count = 0;
    d3d11_update_buffer_large_count = 0;
    d3d11_update_buffer_bytes = 0;
    d3d11_update_buffer_fast_bytes = 0;
    d3d11_update_buffer_deferred_count = 0;
    d3d11_update_buffer_deferred_flush_count = 0;
    d3d11_update_buffer_deferred_flushed_count = 0;
    d3d11_update_buffer_deferred_max_batch = 0;
    d3d11_update_buffer_deferred_bytes = 0;
    d3d11_update_buffer_deferred_blit_flush_count = 0;
    d3d11_update_buffer_deferred_compute_flush_count = 0;
    d3d11_update_buffer_deferred_compute_packed_count = 0;
    d3d11_update_buffer_deferred_all_flush_count = 0;
    d3d11_update_buffer_rename_count = 0;
    d3d11_update_buffer_rename_bytes = 0;
    d3d11_update_texture_count = 0;
    d3d11_invalid_render_target_count = 0;
    render_merge_signature_mismatch_count = 0;
    render_merge_mismatch_rtv_count = 0;
    render_merge_mismatch_dsv_count = 0;
    render_merge_mismatch_array_count = 0;
    render_merge_mismatch_depth_count = 0;
    render_merge_mismatch_stencil_count = 0;
    render_merge_mismatch_color_count = 0;
    render_merge_mismatch_load_store_count = 0;
    render_merge_blocked_vertex_wait_count = 0;
    encoder_relation_data_dependency_count = 0;
    event_stall = 0;
    latency = 0;
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
    last_scaler_info.type = {};
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
    for(unsigned i = 0; i < kFrameStatisticsCount; i++) {
      if (i == current_frame)
        continue; // deliberately exclude current frame since it
      min_.command_buffer_count = std::min(min_.command_buffer_count, frames_[i].command_buffer_count);
      min_.sync_count = std::min(min_.sync_count, frames_[i].sync_count);
      min_.event_stall = std::min(min_.sync_count, frames_[i].event_stall);
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
      average_.sync_count += frames_[i].sync_count;
      average_.event_stall += frames_[i].event_stall;
      average_.commit_interval += frames_[i].commit_interval;
      average_.sync_interval += frames_[i].sync_interval;
      average_.encode_prepare_interval += frames_[i].encode_prepare_interval;
      average_.encode_flush_interval += frames_[i].encode_flush_interval;
      average_.drawable_blocking_interval += frames_[i].drawable_blocking_interval;
      average_.present_latency_interval += frames_[i].present_latency_interval;
    }
    average_.command_buffer_count /= (kFrameStatisticsCount - 1);
    average_.sync_count /= (kFrameStatisticsCount - 1);
    average_.event_stall /= (kFrameStatisticsCount - 1);
    average_.commit_interval /= (kFrameStatisticsCount - 1);
    average_.sync_interval /= (kFrameStatisticsCount - 1);
    average_.encode_prepare_interval /= (kFrameStatisticsCount - 1);
    average_.encode_flush_interval /= (kFrameStatisticsCount - 1);
    average_.drawable_blocking_interval /= (kFrameStatisticsCount - 1);
    average_.present_latency_interval /= (kFrameStatisticsCount - 1);
  };
};

} // namespace dxmt
