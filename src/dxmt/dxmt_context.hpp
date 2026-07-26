#pragma once

#include "Metal.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_command.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_residency.hpp"
#include "dxmt_sampler.hpp"
#include "dxmt_scaler.hpp"
#include "dxmt_statistics.hpp"
#include "dxmt_texture.hpp"
#include "dxmt_descriptor_revision.hpp"
#include "dxmt_gpu_lifetime.hpp"
#include "log/log.hpp"
#include "rc/util_rc_ptr.hpp"
#include "airconv_public.h"
#include <array>
#include <cassert>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#define DXMT_IMPLEMENT_ME __builtin_unreachable();
#define DXMT_UNREACHABLE __builtin_unreachable();

namespace dxmt {

class LifetimeResidencyRegistration;

constexpr size_t kCommandChunkCPUHeapSize = 0x400000;
constexpr size_t kCommandChunkGPUHeapSize = 0x400000;
constexpr size_t kEncodingContextCPUHeapSize = 0x100000;
constexpr size_t kEncodingContextCPUHeapLifetime = 600;

constexpr auto kIntrapassControlBitIgnoreUAVWaW = 1ull << 1;

constexpr uint32_t kCommandBufferFenceEdgeCapacity = 96;
constexpr uint32_t kCommandBufferEncoderDiagnosticCapacity = 64;

enum CommandBufferFenceEdgeFlag : uint32_t {
  CommandBufferFenceEdgePreRaster = 1u << 0,
  CommandBufferFenceEdgeFragment = 1u << 1,
  CommandBufferFenceEdgeCompute = 1u << 2,
  CommandBufferFenceEdgeBlit = 1u << 3,
  CommandBufferFenceEdgeOther = 1u << 4,
  CommandBufferFenceEdgePending = 1u << 5,
  CommandBufferFenceEdgeProducerPreRaster = 1u << 6,
  CommandBufferFenceEdgeProducerFragment = 1u << 7,
};

struct CommandBufferFenceEdgeDiagnostic {
  uint64_t producer_id = 0;
  uint64_t consumer_id = 0;
  uint32_t producer_index = 0;
  uint32_t consumer_index = 0;
  uint32_t slot = 0;
  uint32_t flags = 0;
  uint64_t resource_object = 0;
  uint64_t resource_identity = 0;
  uint64_t allocation_object = 0;
  uint64_t metal_resource_handle = 0;
  uint64_t gpu_address_or_resource_id = 0;
  uint64_t producer_offset = 0;
  uint64_t producer_length = 0;
  uint64_t producer_view_id = 0;
  uint64_t consumer_offset = 0;
  uint64_t consumer_length = 0;
  uint64_t consumer_view_id = 0;
  uint64_t resource_match_hash = 0;
  uint32_t producer_access = 0;
  uint32_t consumer_access = 0;
  uint32_t producer_stage_kind = 0;
  uint32_t consumer_stage_kind = 0;
  uint32_t resource_match_count = 0;
  uint32_t reserved = 0;
};

enum CommandBufferEncoderDiagnosticFlag : uint32_t {
  CommandBufferEncoderRequiresCrossSubmitWait = 1u << 0,
  CommandBufferEncoderHasVertexStageFence = 1u << 1,
};

struct CommandBufferEncoderDiagnostic {
  uint64_t encoder_id = 0;
  uint64_t vertex_encoder_id = 0;
  uint64_t wait_hash = 0;
  uint64_t update_hash = 0;
  uint64_t pipeline_handle = 0;
  uint64_t argument_buffer_handle = 0;
  uint64_t argument_buffer_offset = 0;
  uint64_t argument_buffer_size = 0;
  uint64_t primary_resource = 0;
  uint64_t secondary_resource = 0;
  uint64_t resource_plan_hash = 0;
  uint64_t d3d_sequence_begin = 0;
  uint64_t d3d_sequence_end = 0;
  uint32_t encoder_index = 0;
  uint32_t type = 0;
  uint32_t wait_count = 0;
  uint32_t update_count = 0;
  uint32_t flags = 0;
  uint32_t resource_plan_count = 0;
};

struct CommandBufferDiagnosticInfo {
  uint64_t frame_id = 0;
  uint64_t chunk_id = 0;
  uint64_t d3d_sequence_begin = 0;
  uint64_t d3d_sequence_end = 0;
  uint64_t resource_initializer_event_id = 0;
  uint32_t input_encoder_count = 0;
  uint32_t encoded_encoder_count = 0;
  uint32_t render_encoder_count = 0;
  uint32_t compute_encoder_count = 0;
  uint32_t blit_encoder_count = 0;
  uint32_t other_encoder_count = 0;
  uint32_t present_encoder_count = 0;
  uint32_t clear_encoder_count = 0;
  uint32_t resolve_encoder_count = 0;
  uint32_t scaler_encoder_count = 0;
  uint32_t signal_event_count = 0;
  uint32_t wait_event_count = 0;
  uint32_t timestamp_encoder_count = 0;
  uint32_t barrier_only_pass_count = 0;
  uint32_t fence_wait_count = 0;
  uint32_t fence_update_count = 0;
  uint32_t prior_local_fence_wait_count = 0;
  uint32_t future_local_fence_wait_count = 0;
  uint32_t same_encoder_fence_wait_count = 0;
  uint32_t external_fence_wait_count = 0;
  uint32_t repeated_fence_update_count = 0;
  uint32_t render_valid_cross_stage_count = 0;
  uint32_t render_same_stage_wait_count = 0;
  uint32_t render_reverse_stage_wait_count = 0;
  uint32_t local_fence_id_count = 0;
  uint32_t bound_fence_slot_count = 0;
  uint32_t encoded_fence_wait_count = 0;
  uint32_t skipped_external_fence_wait_count = 0;
  uint32_t fence_edge_count = 0;
  uint32_t fence_edge_overflow_count = 0;
  uint32_t encoder_diagnostic_count = 0;
  uint32_t encoder_diagnostic_overflow_count = 0;
  uint32_t sparse_mapping_call_count = 0;
  uint32_t sparse_mapping_operation_count = 0;
  uint32_t sparse_mapping_map_count = 0;
  uint32_t sparse_mapping_unmap_count = 0;
  uint32_t sparse_mapping_failure_count = 0;
  uint32_t sparse_mapping_barrier_count = 0;
  uint64_t sparse_resource_identity = 0;
  uint64_t sparse_texture_handle = 0;
  uint64_t sparse_heap_handle = 0;
  uint64_t sparse_gpu_resource_id = 0;
  uint64_t sparse_mapping_generation_begin = 0;
  uint64_t sparse_mapping_generation_end = 0;
  uint32_t sparse_access_count = 0;
  uint32_t sparse_access_flags = 0;
  uint32_t sparse_access_level = 0;
  uint32_t sparse_access_slice = 0;
  uint64_t sparse_access_descriptor = 0;
  uint64_t sparse_access_resource_identity = 0;
  uint64_t sparse_access_texture_handle = 0;
  uint64_t sparse_access_gpu_resource_id = 0;
  uint64_t sparse_access_encoder_id = 0;
  std::array<CommandBufferFenceEdgeDiagnostic,
             kCommandBufferFenceEdgeCapacity>
      fence_edges = {};
  std::array<CommandBufferEncoderDiagnostic,
             kCommandBufferEncoderDiagnosticCapacity>
      encoders = {};
};

struct FencePoolPreparationResult {
  uint32_t external_wait_count = 0;
  uint32_t external_blit_wait_count = 0;
  uint32_t external_explicit_barrier_wait_count = 0;
};

inline std::size_t
align_forward_adjustment(const void *const ptr, const std::size_t &alignment) noexcept {
  const auto iptr = reinterpret_cast<std::uintptr_t>(ptr);
  const auto aligned = (iptr - 1u + alignment) & -alignment;
  return aligned - iptr;
}

inline void *
ptr_add(const void *const p, const std::uintptr_t &amount) noexcept {
  return reinterpret_cast<void *>(reinterpret_cast<std::uintptr_t>(p) + amount);
}

enum class PipelineStage { Vertex = 0, Pixel = 1, Geometry = 2, Hull = 3, Domain = 4, Compute = 5 };

enum class PipelineKind { Ordinary, Tessellation, Geometry };

constexpr unsigned kStages = 6;
constexpr unsigned kSRVBindings = 128;
constexpr unsigned kUAVBindings = 64;
constexpr unsigned kVertexBufferSlots = 32;

struct VertexBufferBinding {
  Rc<Buffer> buffer;
  unsigned offset;
  unsigned stride;
};

struct ConstantBufferBinding {
  Rc<Buffer> buffer;
  WMT::Reference<WMT::Buffer> direct_buffer;
  uint64_t direct_gpu_address = 0;
  uint64_t direct_length = 0;
  unsigned offset;
};

struct ConstantBufferBindingSnapshot {
  bool captured = false;
  ConstantBufferBinding binding;
};

struct SamplerBinding {
  Rc<Sampler> sampler;
};

struct ResourceViewBinding {
  uint64_t viewId;
  Rc<Buffer> buffer;
  Rc<Texture> texture;
  BufferSlice slice;
};

struct UnorderedAccessViewBinding {
  uint64_t viewId;
  Rc<Buffer> buffer;
  Rc<Texture> texture;
  Rc<Buffer> counter;
  BufferSlice slice;
};

struct ShaderResourceBindingSnapshot {
  bool srv_captured = false;
  bool uav_captured = false;
  Sampler *sampler = nullptr;
  ResourceViewBinding srv;
  UnorderedAccessViewBinding uav;
};

struct BindlessBufferTableSnapshot {
  const ConstantBufferBindingSnapshot *constant_buffers = nullptr;
  const ShaderResourceBindingSnapshot *resources = nullptr;
};

struct DummyTextureBinding {
  WMT::Reference<WMT::Texture> texture;
  std::shared_ptr<LifetimeResidencyRegistration> residency_registration;
  uint64_t gpu_resource_id = 0;
  uint32_t array_length = 1;
  DXMT_RESOURCE_RESIDENCY_STATE residency;
};

enum class EncoderType {
  Null,
  Render,
  Compute,
  Blit,
  Clear,
  Resolve,
  Present,
  SpatialUpscale,
  SignalEvent,
  TemporalUpscale,
  WaitForEvent,
  SampleTimestamp,
  ResolveTimestamp,
};

struct EncoderData {
  EncoderType type;
  EncoderData *next = nullptr;
  EncoderId id;
  FenceSet fence_wait;
  FenceSet fence_update;
  EncoderBarrierState barrier_state;
  uint64_t diagnostic_d3d_sequence_begin = 0;
  uint64_t diagnostic_d3d_sequence_end = 0;
  bool requires_cross_submit_wait = false;
};

struct GSDispatchArgumentsMarshal {
  WMT::Reference<WMT::Buffer> draw_arguments;
  uint64_t draw_arguments_va;
  uint64_t max_object_threadgroups;
  WMT::Buffer dispatch_arguments_buffer;
  uint64_t dispatch_arguments_va;
  uint32_t vertex_count_per_warp;
};

struct TSDispatchArgumentsMarshal {
  WMT::Reference<WMT::Buffer> draw_arguments;
  uint64_t draw_arguments_va;
  uint64_t max_object_threadgroups;
  WMT::Buffer dispatch_arguments_buffer;
  uint64_t dispatch_arguments_va;
  uint32_t control_point_count;
  uint32_t patch_per_group;
};

struct RenderEncoderColorAttachmentData {
  TextureViewRef attachment;
  Rc<Buffer> buffer_attachment;
  WMT::Reference<WMT::Texture> buffer_texture;
  uint64_t buffer_view_id = 0;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  struct WMTClearColor clear_color;
  TextureViewRef resolve_attachment;
  uint16_t resolve_level;
  uint16_t resolve_slice;
  uint32_t resolve_depth_plane;
};

struct RenderEncoderDepthAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  float clear_depth;
};

struct RenderEncoderStencilAttachmentData {
  TextureViewRef attachment;
  enum WMTLoadAction load_action;
  enum WMTStoreAction store_action;
  uint16_t level;
  uint16_t slice;
  uint32_t depth_plane;
  uint8_t clear_stencil;
};

struct ArgumentTableSliceCacheEntry {
  uint64_t hash = 0;
  uint64_t offset = 0;
  uint32_t size = 0;
  bool valid = false;
};

struct ArgumentTableSliceCache {
  std::array<ArgumentTableSliceCacheEntry, 31> entries = {};
  std::array<uint64_t, 31> hits_by_index = {};
  uint64_t lookups = 0;
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t bytes_avoided = 0;
};

struct RenderArgumentBufferOffsetState {
  WMTRenderStages stages = WMTRenderStageVertex;
  uint64_t offset = 0;
  uint8_t index = 0;
  bool valid = false;
};

struct RenderDynamicStateCache {
  WMT::DepthStencilState depth_stencil = {};
  uint8_t stencil_ref = 0;
  bool depth_stencil_valid = false;
  wmtcmd_render_setrasterizerstate rasterizer = {};
  bool rasterizer_valid = false;
  std::array<float, 4> blend_factor = {};
  bool blend_valid = false;
  std::vector<WMTViewport> viewports;
  bool viewports_valid = false;
  std::vector<WMTScissorRect> scissors;
  bool scissors_valid = false;
};

struct RenderBindingStateCache {
  const void *binding_program = nullptr;
  const void *resource_heap = nullptr;
  const void *sampler_heap = nullptr;
  const void *root_tables = nullptr;
  const void *root_constants = nullptr;
  const void *root_descriptors = nullptr;
  const void *vertex_bindings = nullptr;
  uint64_t graphics_generation = 0;
  DescriptorContentRevision descriptor_content_revision = {};
  uint64_t content_fingerprint = 0;
  bool valid = false;
};

struct ComputeBindingStateCache {
  const void *binding_program = nullptr;
  const void *resource_heap = nullptr;
  const void *sampler_heap = nullptr;
  const void *root_tables = nullptr;
  const void *root_constants = nullptr;
  const void *root_descriptors = nullptr;
  DescriptorContentRevision descriptor_content_revision = {};
  uint64_t content_fingerprint = 0;
  uint64_t pso_handle = 0;
  WMTSize threadgroup_size = {};
  bool binding_valid = false;
  bool pipeline_valid = false;
};

struct EncoderResourcePlanKey {
  uintptr_t object = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t view_id = 0;
  uint8_t stage = 0;
  uint8_t kind = 0;

  bool operator==(const EncoderResourcePlanKey &) const = default;
};

struct EncoderResourcePlanKeyHash {
  size_t operator()(const EncoderResourcePlanKey &key) const {
    size_t hash = std::hash<uintptr_t>{}(key.object);
    auto mix = [&](uint64_t value) {
      hash ^= std::hash<uint64_t>{}(value) + 0x9e3779b97f4a7c15ull +
              (hash << 6) + (hash >> 2);
    };
    mix(key.offset);
    mix(key.length);
    mix(key.view_id);
    mix(key.stage);
    mix(key.kind);
    return hash;
  }
};

struct ComputeArgumentBufferOffsetState {
  uint64_t offset = 0;
  uint8_t index = 0;
  bool valid = false;
};

struct NativeArgumentBufferBindingState {
  uint64_t buffer_handle = 0;
  uint64_t offset = 0;
  bool valid = false;
};

struct RenderEncoderData : EncoderData {
  std::array<RenderEncoderColorAttachmentData, 8> colors;
  RenderEncoderDepthAttachmentData depth;
  RenderEncoderStencilAttachmentData stencil;
  uint8_t default_raster_sample_count;
  uint16_t render_target_array_length;
  uint32_t render_target_height;
  uint32_t render_target_width;
  std::vector<GSDispatchArgumentsMarshal> gs_arg_marshal_tasks;
  std::vector<TSDispatchArgumentsMarshal> ts_arg_marshal_tasks;
  wmtcmd_render_nop cmd_head;
  wmtcmd_base *cmd_tail;
  /**
   * L2 pass-owned argument buffer. Host writes go through getMappedArgumentBuffer
   * which bounds-checks against this slice; do not store a raw mapped pointer.
   */
  AllocatedArgumentBufferSlice argbuf_slice{};
  uint64_t encoder_id_vertex;
  FenceSet fence_wait_vertex;
  FenceSet fence_update_vertex;
  uint8_t dsv_planar_flags;
  uint8_t dsv_readonly_flags;
  uint8_t render_target_count;
  bool use_visibility_result = 0;
  bool use_tessellation = 0;
  bool use_geometry = 0;
  // A Fragment -> PreRaster split is a semantic encoder boundary. The
  // immediate predecessor must not be folded back into this pass.
  bool prevent_merge_with_previous = 0;
  TileBarrierPSOKey tile_barrier_pso_key = {};
  WMT::RenderPipelineState last_pso = {};
  uint64_t pixel_shader_demote_msaa_srv_mask_lo = 0;
  uint64_t pixel_shader_demote_msaa_srv_mask_hi = 0;
  ArgumentTableSliceCache argument_table_cache_vertex;
  ArgumentTableSliceCache argument_table_cache_fragment;
  ArgumentTableSliceCache argument_table_cache_object;
  ArgumentTableSliceCache argument_table_cache_mesh;
  std::array<RenderArgumentBufferOffsetState, 16> argument_buffer_offsets = {};
  std::array<NativeArgumentBufferBindingState, 31>
      native_argument_buffers_vertex = {};
  std::array<NativeArgumentBufferBindingState, 31>
      native_argument_buffers_fragment = {};
  RenderDynamicStateCache dynamic_state_cache = {};
  RenderBindingStateCache binding_state_cache = {};
  // Diagnostic summary only. Resource hazards are published for every
  // command; this map must never gate ordered access tracking.
  std::unordered_map<EncoderResourcePlanKey, int,
                     EncoderResourcePlanKeyHash>
      resource_plan_accesses;
};

struct ComputeEncoderData : EncoderData {
  wmtcmd_compute_nop cmd_head;
  wmtcmd_base *cmd_tail;
  AllocatedArgumentBufferSlice argbuf_slice{};
  ArgumentTableSliceCache argument_table_cache;
  std::array<ComputeArgumentBufferOffsetState, 8> argument_buffer_offsets = {};
  std::array<NativeArgumentBufferBindingState, 31>
      native_argument_buffers = {};
  ComputeBindingStateCache binding_state_cache = {};
  // Diagnostic summary only; see RenderEncoderData::resource_plan_accesses.
  std::unordered_map<EncoderResourcePlanKey, int,
                     EncoderResourcePlanKeyHash>
      resource_plan_accesses;
};

struct BlitEncoderData : EncoderData {
  wmtcmd_blit_nop cmd_head;
  wmtcmd_base *cmd_tail;
};

struct ClearEncoderData : EncoderData {
  WMTClearColor color = {};
  std::pair<float, uint8_t> depth_stencil = {};
  TextureViewRef attachment;
  Rc<Buffer> buffer_attachment;
  WMT::Reference<WMT::Texture> buffer_texture;
  uint64_t buffer_view_id = 0;
  unsigned clear_dsv;
  unsigned array_length;
  unsigned width;
  unsigned height;
  unsigned sample_count;
  uint16_t level = 0;
  uint16_t slice = 0;
  uint32_t depth_plane = 0;
  uint32_t stencil_depth_plane = 0;
  bool has_rects = false;

  ClearEncoderData() {}
};

struct ResolveEncoderData : EncoderData {
  TextureViewRef src;
  TextureViewRef dst;
  WMT::RenderPipelineState pso;
  std::optional<WMTScissorRect> src_rect;
  WMTOrigin dst_origin;
  WMTSize resolve_size;
};

class Presenter;

struct PresentData : EncoderData {
  WMT::Reference<WMT::Texture> backbuffer;
  Rc<Presenter> presenter;
  double after;
  DXMTPresentMetadata metadata;
  uint64_t apitrace_frame_index = ~0ull;
  uint32_t sync_interval = 0;
  uint32_t flags = 0;
};

struct SpatialUpscaleData : EncoderData {
  WMT::Reference<WMT::Texture> backbuffer;
  WMT::Reference<WMT::Texture> upscaled;
  Rc<SpatialScaler> scaler;
};

struct SignalEventData : EncoderData {
  WMT::Reference<WMT::Event> event;
  uint64_t value;
};

struct WaitForEventData : EncoderData {
  WMT::Reference<WMT::Event> event;
  uint64_t value;
};

struct SampleTimestampData: EncoderData {
  uint64_t readback_index;
  // TODO: small_vector opt
  std::vector<Rc<TimestampQuery>> queries;
};

struct ResolveTimestampData : EncoderData {
  struct Range {
#if DXMT_DX12_METAL4
    WMT::Reference<WMT::CounterHeap> src_heap;
#endif
    WMT::Reference<WMT::Buffer> dst_buffer;
    uint64_t start_index;
    uint64_t query_count;
    uint64_t dst_offset;
    uint64_t dst_length;
  };
  std::vector<Range> ranges;
};

struct TemporalUpscaleData : EncoderData {
  WMT::Reference<WMT::Texture> input;
  WMT::Reference<WMT::Texture> output;
  WMT::Reference<WMT::Texture> depth;
  WMT::Reference<WMT::Texture> motion_vector;
  WMT::Reference<WMT::Texture> exposure;
  Rc<TemporalScaler> scaler;
  WMTFXTemporalScalerProps props;
};

template <PipelineKind kind>
constexpr DXMT_RESOURCE_RESIDENCY
GetResidencyMask(PipelineStage type, bool read, bool write) {
  switch (type) {
  case PipelineStage::Vertex:
    if constexpr (kind == PipelineKind::Tessellation)
      return (read ? DXMT_RESOURCE_RESIDENCY_OBJECT_READ : DXMT_RESOURCE_RESIDENCY_NULL) |
             (write ? DXMT_RESOURCE_RESIDENCY_OBJECT_WRITE : DXMT_RESOURCE_RESIDENCY_NULL);
    else if constexpr (kind == PipelineKind::Geometry)
      return (read ? DXMT_RESOURCE_RESIDENCY_OBJECT_GS_READ : DXMT_RESOURCE_RESIDENCY_NULL) |
             (write ? DXMT_RESOURCE_RESIDENCY_OBJECT_GS_WRITE : DXMT_RESOURCE_RESIDENCY_NULL);
    else
      return (read ? DXMT_RESOURCE_RESIDENCY_VERTEX_READ : DXMT_RESOURCE_RESIDENCY_NULL) |
             (write ? DXMT_RESOURCE_RESIDENCY_VERTEX_WRITE : DXMT_RESOURCE_RESIDENCY_NULL);
  case PipelineStage::Pixel:
    return (read ? DXMT_RESOURCE_RESIDENCY_FRAGMENT_READ : DXMT_RESOURCE_RESIDENCY_NULL) |
           (write ? DXMT_RESOURCE_RESIDENCY_FRAGMENT_WRITE : DXMT_RESOURCE_RESIDENCY_NULL);
  case PipelineStage::Hull:
    return (read ? DXMT_RESOURCE_RESIDENCY_OBJECT_READ : DXMT_RESOURCE_RESIDENCY_NULL) |
           (write ? DXMT_RESOURCE_RESIDENCY_OBJECT_WRITE : DXMT_RESOURCE_RESIDENCY_NULL);
  case PipelineStage::Geometry:
    return (read ? DXMT_RESOURCE_RESIDENCY_MESH_GS_READ : DXMT_RESOURCE_RESIDENCY_NULL) |
           (write ? DXMT_RESOURCE_RESIDENCY_MESH_GS_WRITE : DXMT_RESOURCE_RESIDENCY_NULL);
  case PipelineStage::Domain:
    return (read ? DXMT_RESOURCE_RESIDENCY_MESH_READ : DXMT_RESOURCE_RESIDENCY_NULL) |
           (write ? DXMT_RESOURCE_RESIDENCY_MESH_WRITE : DXMT_RESOURCE_RESIDENCY_NULL);
  case PipelineStage::Compute:
    return (read ? DXMT_RESOURCE_RESIDENCY_READ : DXMT_RESOURCE_RESIDENCY_NULL) |
           (write ? DXMT_RESOURCE_RESIDENCY_WRITE : DXMT_RESOURCE_RESIDENCY_NULL);
  }
  DXMT_UNREACHABLE;
}

enum DXMT_ENCODER_LIST_OP {
  DXMT_ENCODER_LIST_OP_SWAP = 0,
  DXMT_ENCODER_LIST_OP_SYNCHRONIZE = 1,
};

class CommandQueue;

struct AllocatedTempBufferSlice {
  WMT::Buffer gpu_buffer;
  uint64_t offset;
  uint64_t gpu_address;
};

class ArgumentEncodingContext {
private:
  template <PipelineStage stage> void track(GenericAccessTracker &tracker, int flags);
  bool trackerRequiresReverseRenderPassBoundary(GenericAccessTracker &tracker,
                                                int flags);
  void noteSparseTextureAccess(TextureAllocation *allocation, unsigned level,
                               unsigned slice, int flags);

public:
  /**
   * The dummy sampler's gpu_resource_id used by encodeShaderResources for an unbound
   * sampler. Exposed so the bindless-mirror fill path (sub-step ②/③, in the d3d12
   * command queue) can write the byte-identical null-sampler payload.
   */
  uint64_t dummySamplerHandle() const { return dummy_sampler_info_.gpu_resource_id; }

  template <PipelineStage stage>
  void
  trackBuffer(BufferAllocation *allocation, int flags) {
    retainAllocation(allocation);
    if (allocation->flags().test(BufferAllocationFlag::GpuReadonly))
      return;
    auto &tracker = allocation->fenceTrackers[allocation->currentSuballocation()];
    track<stage>(tracker, flags);
  }

public:
  template<PipelineStage stage = PipelineStage::Compute>
  std::pair<BufferAllocation *, uint64_t>
  access(Rc<Buffer> const &buffer, unsigned offset, unsigned length, int flags) {
    auto allocation = buffer->current();
    trackBuffer<stage>(allocation, flags);
    retainResourceForCurrentCommandBuffer(allocation->buffer());
    return {allocation, allocation->currentSuballocationOffset()};
  }

  template<PipelineStage stage = PipelineStage::Compute>
  std::pair<BufferView const &, uint32_t>
  access(Rc<Buffer> const &buffer, uint64_t viewId, int flags) {
    auto allocation = buffer->current();
    trackBuffer<stage>(allocation, flags);
    auto &view = buffer->view_(viewId, allocation);
    retainResourceForCurrentCommandBuffer(allocation->buffer());
    retainResourceForCurrentCommandBuffer(view.texture);
    return {view, allocation->currentSuballocationOffset(view.suballocation_texel)};
  }

  template<PipelineStage stage = PipelineStage::Compute>
  WMT::Texture
  access(Rc<Texture> const &texture, unsigned level, unsigned slice, int flags) {
    auto allocation = texture->current();
    retainAllocation(allocation);
    retainResourceForCurrentCommandBuffer(allocation->texture());
    noteSparseTextureAccess(allocation, level, slice, flags);
    if (!allocation->flags().test(TextureAllocationFlag::GpuReadonly)) {
      if (likely(allocation->flags().test(TextureAllocationFlag::ShaderReadonly))) {
        track<stage>(allocation->fenceTrackers[0], flags);
      } else {
        const auto mip_count = allocation->descriptor->miplevelCount();
        const auto tracker_index = slice * mip_count + level;
        if (unlikely(level >= mip_count ||
                     tracker_index >= allocation->fenceTrackers.size())) {
          WARN("DXMT: texture access subresource out of range level=", level,
               " slice=", slice, " mip_count=", mip_count,
               " tracker_count=", allocation->fenceTrackers.size());
          track<stage>(allocation->fenceTrackers[0], flags);
          return allocation->texture();
        }
        auto &tracker = allocation->fenceTrackers[tracker_index];
        track<stage>(tracker, flags);
      }
    }
    return allocation->texture();
  }

  template<PipelineStage stage = PipelineStage::Compute>
  TextureView &
  access(Rc<Texture> const &texture, uint64_t viewId, int flags) {
    assert(viewId);
    auto allocation = texture->current();
    retainAllocation(allocation);
    auto &view = texture->view(viewId, allocation);
    retainResourceForCurrentCommandBuffer(view.texture);
    TextureViewKey key = viewId;
    noteSparseTextureAccess(allocation, key.mip_start, key.array_start, flags);
    if (!allocation->flags().test(TextureAllocationFlag::GpuReadonly)) {
      if (likely(allocation->flags().test(TextureAllocationFlag::ShaderReadonly))) {
        track<stage>(allocation->fenceTrackers[0], flags);
      } else {
        const auto mip_count = allocation->descriptor->miplevelCount();
        for (unsigned slice = key.array_start; slice < key.array_end; slice++) {
          for (unsigned level = key.mip_start; level < key.mip_end; level++) {
            const auto tracker_index = slice * mip_count + level;
            if (unlikely(level >= mip_count ||
                         tracker_index >= allocation->fenceTrackers.size())) {
              WARN("DXMT: texture view access subresource out of range view=",
                   viewId, " level=", level, " slice=", slice,
                   " mip_count=", mip_count, " tracker_count=",
                   allocation->fenceTrackers.size());
              track<stage>(allocation->fenceTrackers[0], flags);
              return view;
            }
            auto &tracker = allocation->fenceTrackers[tracker_index];
            track<stage>(tracker, flags);
          }
        }
      }
    }
    return view;
  }

  template <PipelineStage stage>
  void
  bindConstantBuffer(unsigned slot, unsigned offset, Rc<Buffer> &&buffer) {
    unsigned idx = slot + 14 * unsigned(stage);
    auto &entry = cbuf_[idx];
    entry.offset = offset;
    entry.buffer = std::move(buffer);
    entry.direct_buffer = {};
    entry.direct_gpu_address = 0;
    entry.direct_length = 0;
  }

  template <PipelineStage stage>
  void
  bindConstantBufferDirect(unsigned slot, WMT::Buffer buffer, uint64_t gpu_address,
                           uint64_t length) {
    retainResourceForCurrentCommandBuffer(buffer);
    unsigned idx = slot + 14 * unsigned(stage);
    auto &entry = cbuf_[idx];
    entry.buffer = {};
    entry.direct_buffer = WMT::Reference<WMT::Buffer>(buffer);
    entry.direct_gpu_address = gpu_address;
    entry.direct_length = length;
    entry.offset = 0;
  }

  template <PipelineStage stage>
  void
  bindConstantBufferOffset(unsigned slot, unsigned offset) {
    unsigned idx = slot + 14 * unsigned(stage);
    auto &entry = cbuf_[idx];
    entry.offset = offset;
  }

  template <PipelineStage stage>
  void
  bindSampler(unsigned slot, Rc<Sampler> sampler) {
    unsigned idx = slot + 16 * unsigned(stage);
    auto &entry = sampler_[idx];
    entry.sampler = sampler;
  }

  template <PipelineStage stage>
  void
  bindBuffer(unsigned slot, Rc<Buffer> &&buffer, uint64_t viewId, BufferSlice slice) {
    unsigned idx = slot + kSRVBindings * unsigned(stage);
    auto &entry = resview_[idx];
    entry.texture = {};
    entry.buffer = std::move(buffer);
    entry.viewId = viewId;
    entry.slice = slice;
  }

  template <PipelineStage stage>
  void
  bindTexture(unsigned slot, Rc<Texture> &&texture, uint64_t viewId) {
    unsigned idx = slot + kSRVBindings * unsigned(stage);
    auto &entry = resview_[idx];
    entry.buffer = {};
    entry.texture = std::move(texture);
    entry.viewId = viewId;
  }

  template <PipelineStage stage>
  void bindOutputBuffer(unsigned slot, Rc<Buffer> &&buffer, uint64_t viewId, Rc<Buffer> &&counter, BufferSlice slice);

  template <PipelineStage stage> void bindOutputTexture(unsigned slot, Rc<Texture> &&texture, uint64_t viewId);

  void bindStreamOutputBuffer(unsigned slot, unsigned offset, Rc<Buffer> &&buffer);
  void bindStreamOutputBufferOffset(unsigned slot, unsigned offset);

  void
  bindVertexBuffer(unsigned slot, unsigned offset, unsigned stride, Rc<Buffer> &&buffer) {
    auto &entry = vbuf_[slot];
    entry.buffer = std::move(buffer);
    entry.offset = offset;
    entry.stride = stride;
  }

  void
  bindVertexBufferOffset(unsigned slot, unsigned offset, unsigned stride) {
    auto &entry = vbuf_[slot];
    entry.offset = offset;
    entry.stride = stride;
  }

  void
  bindIndexBuffer(Rc<Buffer> &&buffer) {
    ibuf_ = std::move(buffer);
  }

  std::pair<WMT::Buffer, uint64_t>
  currentIndexBuffer() {
    // because of indirect draw, we can't predicate the accessed buffer range
    auto [ibuf_alloc, offset] = access<PipelineStage::Vertex>(ibuf_, 0, ibuf_->length(), ResourceAccess::Read);
    return {ibuf_alloc->buffer(), offset};
  };

  void clearState() {
    vbuf_ = {{}};
    ibuf_ = {};
    cbuf_ = {{}};
    sampler_ = {{}};
    resview_ = {{}};
    om_uav_ = {{}};
    cs_uav_ = {{}};
  }

  template <PipelineKind kind> void encodeVertexBuffers(uint32_t ia_slot_mask, uint64_t argument_buffer_offset);
  template <PipelineStage stage, PipelineKind kind>
  void encodeConstantBuffers(
      const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
      uint64_t argument_buffer_offset, const std::string &shader_hash
  );
  template <PipelineStage stage, PipelineKind kind>
  void encodeConstantBuffers(
      const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
      uint64_t argument_buffer_offset, const ConstantBufferBinding *bindings
  );
  template <PipelineStage stage, PipelineKind kind>
  void encodeShaderResources(
      const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments,
      uint64_t argument_buffer_offset, const std::string &shader_hash,
      const ShaderResourceBindingSnapshot *bindings,
      uint64_t demote_msaa_srv_mask_lo = 0, uint64_t demote_msaa_srv_mask_hi = 0
  );

  // Bindless-mirror (Stage-1 sub-step ③): pack the per-draw slot-27 buf_table. The CBV
  // half and the SRV/UAV-buffer half are split so each reuses its legacy resolve path
  // verbatim (encodeConstantBuffers / encodeShaderResources). buf_table/qwords is the
  // CPU-mapped ring slice; *_compact are the per-index compact qword bases from the
  // shared ForEachBufferTableField walk (must match airconv). See dxmt_bindless_buffer_table.hpp.
  template <PipelineStage stage, PipelineKind kind>
  void packBindlessCBuffers(
      const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
      uint64_t *buf_table, uint32_t buf_table_qwords, const uint32_t *cb_compact,
      const ConstantBufferBindingSnapshot *bindings = nullptr
  );
  template <PipelineStage stage, PipelineKind kind>
  void packBindlessBufferTable(
      const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments,
      const std::string &shader_hash, uint64_t *buf_table, uint32_t buf_table_qwords,
      const uint32_t *res_compact, const BindlessBufferTableSnapshot *bindings,
      uint64_t demote_msaa_srv_mask_lo = 0, uint64_t demote_msaa_srv_mask_hi = 0
  );
  template <PipelineStage stage, PipelineKind kind>
  void verifyBindlessBufferTable(
      const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
      const MTL_SM50_SHADER_ARGUMENT *arguments, const uint64_t *buf_table,
      uint32_t buf_table_qwords, const uint32_t *cb_compact, const uint32_t *res_compact,
      uint64_t verify_draw_id, uint64_t verify_draw_serial
  );

  // Bindless-mirror (Stage-1 sub-step ③.3): pack ONE stage's slot-27 buf_table for the
  // current draw. Allocates the buf_table ring slice (sized via BuildBufferTableCompactBases,
  // the SAME walk airconv bakes its compact bases from), runs the ③.1 packers
  // (packBindlessCBuffers + packBindlessBufferTable) — which resolve buffer addresses with
  // the legacy access<>()/makeResident expressions and persist tex/sampler residency — then
  // flushes the slice. Runs inside the encode path (encoder_current set). Returns the buf_table
  // slice (empty/null gpu_buffer when the stage has no buffer fields, in which case the caller
  // still binds the mirrors). It does NOT emit the slot binds (see bindBindlessTables); the
  // caller resolves root_offsets + the two mirror buffers which need root/heap state.
  // Takes the same raw reflection/argument pointers as the ③.1 packers (constant_buffers ==
  // constantBufferInfo(), arguments == resourceArgumentInfo()) to keep the dxmt layer free of
  // the d3d12 PipelineDxilShader type.
  template <PipelineStage stage, PipelineKind kind>
  AllocatedArgumentBufferSlice
  packBindlessStage(const MTL_SHADER_REFLECTION *reflection,
                    const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
                    const MTL_SM50_SHADER_ARGUMENT *arguments,
                    const std::string &shader_hash,
                    uint64_t verify_draw_id = 0,
                    uint64_t verify_draw_serial = 0,
                    const BindlessBufferTableSnapshot *bindings = nullptr,
                    uint64_t demote_msaa_srv_mask_lo = 0,
                    uint64_t demote_msaa_srv_mask_hi = 0,
                    const char *diagnostic_path = "bindless-live");

  // Bindless-mirror (Stage-1 sub-step ③.3): emit this draw's deferred per-stage slot binds.
  // The standard layout uses 27..30; tessellation VS uses 23..26 so its tables remain distinct
  // from HS in the same object function. Null buffers are skipped. Sets the
  // Ordinary vertex work maps to the vertex stage. Geometry/tessellation
  // emulation maps D3D vertex/hull work to object and geometry/domain work to
  // mesh. Pixel maps to fragment, and compute uses its dedicated command.
  template <PipelineStage stage, PipelineKind kind = PipelineKind::Ordinary>
  void bindBindlessTables(const AllocatedArgumentBufferSlice &buf_table,
                          const AllocatedArgumentBufferSlice &root_offsets,
                          const AllocatedArgumentBufferSlice &tex_mirror,
                          const AllocatedArgumentBufferSlice &sampler_mirror);

  template <PipelineStage stage>
  void bindNativeRootTableBases(const AllocatedArgumentBufferSlice &root_bases,
                                uint32_t bind_index);

  void bindNativeArgumentBuffer(WMT::Buffer buffer, uint64_t offset,
                                uint32_t bind_index, bool compute,
                                WMTRenderStages render_stages = {});
  void bindNativeNullConstantBuffer(bool compute,
                                    WMTRenderStages render_stages = {});
  void bindNativeNullBuffer(bool compute,
                            WMTRenderStages render_stages = {});
  void diagnoseNativeShaderBinding(
      PipelineStage stage, const std::string &shader_hash, const char *path,
      const AllocatedArgumentBufferSlice &cbuffer_root_bases,
      const AllocatedArgumentBufferSlice &resource_root_bases);
  void invalidateNativeArgumentBuffers(bool compute,
                                       WMTRenderStages render_stages = {});

  bool retainAllocation(Allocation* allocation);

  void retainResource(WMT::Resource resource) {
    retainResourceForCurrentCommandBuffer(resource);
  }

  /** True while beginCommandBufferResourceLifetime is active for this context. */
  bool hasActiveCommandBufferGeneration() const {
    return bool(current_command_buffer_) &&
           current_command_buffer_resources_ != nullptr &&
           current_command_buffer_resource_handles_ != nullptr;
  }

  /**
   * Generation-pin a Metal resource. Returns false if no generation is active
   * or the resource is null (fail-closed; no soft-skip success).
   */
  bool tryRetainResourceForCurrentCommandBuffer(WMT::Resource resource);

  void beginCommandBufferResourceLifetime(
      WMT::CommandBuffer command_buffer,
      std::vector<WMT::Reference<WMT::Resource>> &resources,
      std::unordered_set<obj_handle_t> &resource_handles);

  void endCommandBufferResourceLifetime();

  template <PipelineStage stage, PipelineKind kind>
  void
  makeResident(WMT::Resource resource, DXMT_RESOURCE_RESIDENCY requested) {
    retainResourceForCurrentCommandBuffer(resource);
    if constexpr (stage == PipelineStage::Compute) {
      auto &cmd = encodeComputeCommand<wmtcmd_compute_useresource>();
      cmd.type = WMTComputeCommandUseResource;
      cmd.resource = resource;
      cmd.usage = GetUsageFromResidencyMask(requested);
    } else {
      auto &cmd = encodeRenderCommand<wmtcmd_render_useresource>();
      cmd.type = WMTRenderCommandUseResource;
      cmd.resource = resource;
      cmd.usage = GetUsageFromResidencyMask(requested);
      cmd.stages = GetStagesFromResidencyMask(requested);
    }
  }

  template <PipelineStage stage>
  uint64_t
  currentResourceResidencyEncoderId() {
    if constexpr (stage == PipelineStage::Compute ||
                  stage == PipelineStage::Pixel) {
      return currentEncoderId();
    } else {
      return currentRenderEncoder()->encoder_id_vertex;
    }
  }

  template <PipelineStage stage, PipelineKind kind>
  void
  makeResident(Buffer *buffer, bool read = true, bool write = false) {
    auto allocation = buffer->current();
    uint64_t encoder_id = currentResourceResidencyEncoderId<stage>();
    DXMT_RESOURCE_RESIDENCY requested = GetResidencyMask<kind>(stage, read, write);
    if (CheckResourceResidency(allocation->residencyState, encoder_id, requested)) {
      makeResident<stage, kind>(allocation->buffer(), requested);
    };
  }
  template <PipelineStage stage, PipelineKind kind>
  void
  makeResident(Buffer *buffer, uint64_t viewId, bool read = true, bool write = false) {
    auto allocation = buffer->current();
    uint64_t encoder_id = currentResourceResidencyEncoderId<stage>();
    DXMT_RESOURCE_RESIDENCY requested = GetResidencyMask<kind>(stage, read, write);
    auto *view = buffer->tryView(viewId, allocation);
    if (!view || !view->texture)
      return;
    if (CheckResourceResidency(view->residency, encoder_id, requested)) {
      makeResident<stage, kind>(view->texture, requested);
    };
  }
  template <PipelineStage stage, PipelineKind kind>
  void
  makeResident(Texture *texture, uint64_t viewId, bool read = true, bool write = false) {
    auto allocation = texture->current();
    uint64_t encoder_id = currentResourceResidencyEncoderId<stage>();
    DXMT_RESOURCE_RESIDENCY requested = GetResidencyMask<kind>(stage, read, write);
    auto &view = texture->view(viewId, allocation);
    if (CheckResourceResidency(view.residency, encoder_id, requested)) {
      makeResident<stage, kind>(view.texture, requested);
    };
  }

  DummyTextureBinding &dummySRVTexture(const MTL_SM50_SHADER_ARGUMENT &arg);

  template <typename cmd_struct>
  cmd_struct &
  encodeRenderCommand() {
    assert(encoder_current->type == EncoderType::Render);
    noteCurrentEncoderD3DSequence();
    auto encoder = static_cast<RenderEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)allocate_cpu_heap(sizeof(cmd_struct), 16);
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  void
  encodeGSDispatchArgumentsMarshal(
      WMT::Buffer draw_args, uint64_t draw_args_resource_id, uint32_t draw_args_offset, uint32_t vertex_count_per_warp,
      WMT::Buffer dispatch_args, uint64_t dispatch_args_resource_id, uint32_t write_offset, uint64_t max_object_threadgroups
  ) {
    assert(encoder_current->type == EncoderType::Render);
    retainResourceForCurrentCommandBuffer(draw_args);
    retainResourceForCurrentCommandBuffer(dispatch_args);
    auto data = static_cast<RenderEncoderData *>(encoder_current);
    data->gs_arg_marshal_tasks.push_back(
        {draw_args, draw_args_resource_id + draw_args_offset, max_object_threadgroups, dispatch_args,
         dispatch_args_resource_id + write_offset, vertex_count_per_warp}
    );
  }

  void
  encodeTSDispatchArgumentsMarshal(
      WMT::Buffer draw_args, uint64_t draw_args_resource_id, uint32_t draw_args_offset, uint32_t control_point_count,
      uint32_t patch_per_group, WMT::Buffer dispatch_args, uint64_t dispatch_args_resource_id, uint32_t write_offset,
      uint64_t max_object_threadgroups
  ) {
    assert(encoder_current->type == EncoderType::Render);
    retainResourceForCurrentCommandBuffer(draw_args);
    retainResourceForCurrentCommandBuffer(dispatch_args);
    auto data = static_cast<RenderEncoderData *>(encoder_current);
    data->ts_arg_marshal_tasks.push_back(
        {draw_args, draw_args_resource_id + draw_args_offset, max_object_threadgroups, dispatch_args,
         dispatch_args_resource_id + write_offset, control_point_count, patch_per_group}
    );
  }

  template <typename cmd_struct>
  cmd_struct &
  encodeComputeCommand() {
    assert(encoder_current->type == EncoderType::Compute);
    noteCurrentEncoderD3DSequence();
    auto encoder = static_cast<ComputeEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)allocate_cpu_heap(sizeof(cmd_struct), 16);
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  template <typename cmd_struct>
  cmd_struct &
  encodeBlitCommand() {
    assert(encoder_current->type == EncoderType::Blit);
    noteCurrentEncoderD3DSequence();
    auto encoder = static_cast<BlitEncoderData *>(encoder_current);
    auto storage = (cmd_struct *)allocate_cpu_heap(sizeof(cmd_struct), 16);
    encoder->cmd_tail->next.set(storage);
    encoder->cmd_tail = (wmtcmd_base *)storage;
    storage->next.set(nullptr);
    return *storage;
  }

  template <typename T>
  T *
  allocate() {
    return (new (allocate_cpu_heap(sizeof(T), alignof(T))) T());
  };

  void present(Rc<Texture> &texture, Rc<Presenter> &presenter, double after,
               DXMTPresentMetadata metadata, uint64_t apitrace_frame_index = ~0ull,
               uint32_t sync_interval = 0, uint32_t flags = 0);
  void present(Rc<Texture> &texture, TextureViewKey view, Rc<Presenter> &presenter,
               double after, DXMTPresentMetadata metadata,
               uint64_t apitrace_frame_index = ~0ull, uint32_t sync_interval = 0,
               uint32_t flags = 0);

  void upscale(Rc<Texture> &texture, Rc<Texture> &upscaled, Rc<SpatialScaler> &scaler);

  void upscaleTemporal(
      Rc<Texture> &input, Rc<Texture> &output, Rc<Texture> &depth, Rc<Texture> &motion_vector, TextureViewKey mvViewId,
      Rc<Texture> &exposure, Rc<TemporalScaler> &scaler, const WMTFXTemporalScalerProps &props
  );

  void signalEvent(uint64_t value);
  void signalEvent(WMT::Reference<WMT::Event> &&event, uint64_t value);
  void waitEvent(WMT::Reference<WMT::Event> &&event, uint64_t value);

  uint64_t
  nextEncoderId() {
    return encoder_id_++;
  };

  void clearColor(Rc<Texture> &&texture, uint64_t viewId, unsigned arrayLength,
                  WMTClearColor color, bool has_rects = false,
                  uint32_t depth_plane = 0);
  void clearColor(Rc<Buffer> &&buffer, uint64_t viewId, unsigned width,
                  WMTClearColor color, bool has_rects = false);
  void clearDepthStencil(
      Rc<Texture> &&texture, uint64_t viewId, unsigned arrayLength, unsigned flag,
      float depth, uint8_t stencil, bool has_rects = false
  );
  void resolveTexture(
      Rc<Texture> &&src, TextureViewKey src_view, Rc<Texture> &&dst, TextureViewKey dst_view,
      WMT::RenderPipelineState pso = {}, std::optional<WMTScissorRect> src_rect = std::nullopt,
      WMTOrigin dst_origin = {}, WMTSize resolve_size = {}
  );

  RenderEncoderData *startRenderPass(
      uint8_t dsv_planar_flags, uint8_t dsv_readonly_flags, uint8_t render_target_count, uint64_t argument_buffer_size
  );
  EncoderData *startComputePass(uint64_t argument_buffer_size);
  EncoderData *startBlitPass();

  void endPass();
  void noteCurrentEncoderD3DSequence();

  void appendRenderArgumentBufferBindings(
      RenderEncoderData *data, WMT::Buffer buffer, bool use_geometry,
      bool use_tessellation);

  constexpr EncoderData *
  currentEncoder() {
    assert(encoder_current);
    return encoder_current;
  }

  constexpr uint64_t
  currentEncoderId() {
    assert(encoder_current);
    return encoder_current->id;
  }

  constexpr RenderEncoderData *
  currentRenderEncoder() {
    assert(encoder_current && encoder_current->type == EncoderType::Render);
    return static_cast<RenderEncoderData *>(encoder_current);
  }

  void *
  allocate_cpu_heap(size_t size, size_t alignment) {
    assert(size < kEncodingContextCPUHeapSize);
    for (;;) {
      std::size_t adjustment = align_forward_adjustment((void *)cpu_buffer_offset_, alignment);
      auto aligned = cpu_buffer_offset_ + adjustment;
      cpu_buffer_offset_ = aligned + size;
      if (unlikely(cpu_buffer_offset_ >= kEncodingContextCPUHeapSize)) {
        current_buffer_chunk_++;
        while (current_buffer_chunk_ >= cpu_buffer_chunks_.size()) {
          cpu_buffer_chunks_.emplace_back();
        }
        auto &chunk = cpu_buffer_chunks_[current_buffer_chunk_];
        chunk.underused_times = 0;
        cpu_buffer_ = chunk.ptr;
        cpu_buffer_offset_ = 0;
        continue;
      }
      return ptr_add(cpu_buffer_, aligned);
    }
  }

  template <typename T, bool ComputeCommandEncoder = false>
  T *
  getMappedArgumentBuffer(size_t offset, size_t byte_size = sizeof(T)) {
    auto &slice = [&]() -> AllocatedArgumentBufferSlice & {
      if constexpr (ComputeCommandEncoder)
        return static_cast<ComputeEncoderData *>(encoder_current)->argbuf_slice;
      return static_cast<RenderEncoderData *>(encoder_current)->argbuf_slice;
    }();
    if (!slice.valid()) {
      argument_buffer_overflowed_ = true;
      return nullptr;
    }
    const uint64_t offset64 = offset;
    const uint64_t size64 = byte_size;
    const bool in_write_window =
        !size64 ||
        (offset64 >= argument_buffer_write_begin_ &&
         offset64 <= argument_buffer_write_end_ &&
         size64 <= argument_buffer_write_end_ - offset64);
    if (!in_write_window) {
      argument_buffer_overflowed_ = true;
      return nullptr;
    }
    // Prefer slice.map for element types; byte_size path uses raw offset.
    if (size64 == sizeof(T) && (offset64 % sizeof(T)) == 0) {
      auto *p = slice.template map<T>(static_cast<size_t>(offset64 / sizeof(T)), 1);
      if (!p)
        argument_buffer_overflowed_ = true;
      return p;
    }
    if (offset64 > slice.length || size64 > slice.length - offset64) {
      argument_buffer_overflowed_ = true;
      return nullptr;
    }
    return reinterpret_cast<T *>(static_cast<char *>(slice.mapped) + offset);
  }

  void setArgumentBufferWriteRange(uint64_t offset, uint64_t size) {
    argument_buffer_overflowed_ = false;
    argument_buffer_write_begin_ = offset;
    if (size > UINT64_MAX - offset) {
      argument_buffer_write_end_ = offset;
      argument_buffer_overflowed_ = true;
      return;
    }
    argument_buffer_write_end_ = offset + size;
  }

  bool argumentBufferOverflowed() const {
    return argument_buffer_overflowed_;
  }

  void markArgumentBufferOverflow() {
    argument_buffer_overflowed_ = true;
  }

  template <bool ComputeCommandEncoder = false>
  uint64_t
  getFinalArgumentBufferOffset(size_t offset) {
    if constexpr (ComputeCommandEncoder)
      return static_cast<ComputeEncoderData *>(encoder_current)->argbuf_slice.offset + offset;
    return static_cast<RenderEncoderData *>(encoder_current)->argbuf_slice.offset + offset;
  }

  template <bool ComputeCommandEncoder = false>
  WMT::Buffer
  getFinalArgumentBuffer() {
    if constexpr (ComputeCommandEncoder)
      return static_cast<ComputeEncoderData *>(encoder_current)->argbuf_slice.gpu_buffer;
    return static_cast<RenderEncoderData *>(encoder_current)->argbuf_slice.gpu_buffer;
  }

  uint64_t deduplicateRenderArgumentTableSlice(WMTRenderStages stages, uint8_t index, uint64_t offset, uint64_t size);

  uint64_t deduplicateRenderArgumentTableSliceBytes(
      WMTRenderStages stages, uint8_t index, uint64_t offset, uint64_t size, const void *bytes);

  uint64_t deduplicateComputeArgumentTableSlice(uint8_t index, uint64_t offset, uint64_t size);

  uint64_t deduplicateComputeArgumentTableSliceBytes(uint8_t index, uint64_t offset, uint64_t size, const void *bytes);

  bool shouldEmitRenderArgumentBufferOffset(
      WMTRenderStages stages, uint8_t index, uint64_t offset, bool content_cache_hit);

  bool shouldEmitComputeArgumentBufferOffset(uint8_t index, uint64_t offset, bool content_cache_hit);

  std::pair<WMT::Buffer , size_t> allocateTempBuffer(size_t size, size_t alignment);
  
  AllocatedTempBufferSlice allocateTempBuffer1(size_t size, size_t alignment);

  QueryReadbacks flushCommands(
      WMT::CommandBuffer cmdbuf, uint64_t seqId, uint64_t event_seq_id,
      CommandBufferDiagnosticInfo *diagnostic_info = nullptr);

  uint64_t currentSeqId() {return seq_id_;}

  uint64_t currentFrameId() {return frame_id_;}

  CommandQueue& queue() { return queue_;}

  void
  $$setEncodingContext(uint64_t seq_id, uint64_t frame_id);

  void bumpVisibilityResultOffset();
  void beginVisibilityResultQuery(Rc<VisibilityResultQuery> &&query);
  void endVisibilityResultQuery(Rc<VisibilityResultQuery> &&query);
  void
  pushDeferredVisibilityQueries(Rc<VisibilityResultQuery> *list) {
    deferred_visibility_query_stack_.push_back(list);
  }
  void
  popDeferredVisibilityQueries() {
    assert(!deferred_visibility_query_stack_.empty());
    deferred_visibility_query_stack_.pop_back();
  }
  Rc<VisibilityResultQuery>
  currentDeferredVisibilityQuery(uint32_t query_id) {
    assert(!deferred_visibility_query_stack_.empty());
    return deferred_visibility_query_stack_.back()[query_id];
  }

  void sampleTimestamp(Rc<TimestampQuery> &&query);
  void resolveTimestamp(uint64_t start_index, uint64_t query_count,
                        WMT::Reference<WMT::Buffer> dst_buffer, uint64_t dst_offset,
                        uint64_t dst_length);
#if DXMT_DX12_METAL4
  void resolveTimestamp(WMT::Reference<WMT::CounterHeap> src_heap,
                        uint64_t start_index, uint64_t query_count,
                        WMT::Reference<WMT::Buffer> dst_buffer,
                        uint64_t dst_offset, uint64_t dst_length);
#endif
  void appendResolveTimestampRange(
#if DXMT_DX12_METAL4
      WMT::Reference<WMT::CounterHeap> src_heap,
#endif
      uint64_t start_index, uint64_t query_count,
      WMT::Reference<WMT::Buffer> dst_buffer, uint64_t dst_offset,
      uint64_t dst_length);

  void barrierOnQueue(WMT::CommandBuffer cmdbuf) {
    barrier_index_++;
    cmdbuf.encodeSignalEvent(barrier_event_, barrier_index_);
    cmdbuf.encodeWaitForEvent(barrier_event_, barrier_index_);
  };

  void resolveComputePassBarrier();

  void resolveRenderPassBarrier();

  bool requiresReverseRenderPassBoundary(Rc<Buffer> const &buffer,
                                         int flags);
  bool requiresReverseRenderPassBoundary(Rc<Texture> const &texture,
                                         unsigned level, unsigned slice,
                                         int flags);
  bool requiresReverseRenderPassBoundary(Rc<Texture> const &texture,
                                         uint64_t view_id, int flags);

  /**
   * Metal4 cannot express Fragment→PreRaster reverse barriers as
   * barrierAfterEncoderStages (debug layer rejects Fragment as after stages).
   * GPTK SynchroniseStages flush-first: close the current render pass (UpdateFence
   * at Fragment) and reopen with Load so the reverse wait becomes an inter-pass
   * MTLFence edge (deptrack re-establishes the wait on next resource access).
   */
  void splitRenderPassForReverseStage();

  void requireCrossSubmitWait() {
    assert(encoder_current);
    encoder_current->requires_cross_submit_wait = true;
  }

  FrameStatistics&
  currentFrameStatistics();

  void
  setCompatibilityFlag(FeatureCompatibility flag) {
    currentFrameStatistics().compatibility_flags.set(flag);
  }

  void
  setIntrapassBarrierControl(uint64_t control_bits) {
    intrapass_barrier_control_bits_ = control_bits;
  }

  ArgumentEncodingContext(CommandQueue &queue, WMT::Device device, InternalCommandLibrary &lib);
  ~ArgumentEncodingContext();

  uint32_t tess_num_output_control_point_element;
  uint32_t tess_threads_per_patch;

  EmulatedCommandContext emulated_cmd;
  ClearRenderTargetContext clear_rt_cmd;
  ResolveTextureContext resolve_texture_cmd;
  DepthStencilBlitContext blit_depth_stencil_cmd;
  ClearResourceKernelContext clear_res_cmd;
  MTLFXMVScaleContext mv_scale_cmd;
  TileBarrierContext tile_barrier_cmd;

private:
  void retainResourceForCurrentCommandBuffer(WMT::Resource resource);

  /**
   * Metal 4 replay is planned after every encoder has published its resource
   * dependencies. Independent intervening encoders may be crossed so a
   * compatible earlier encoder can be coalesced into a later one; event and
   * timestamp encoders, or a dependency edge, stop the search.
   *
   * This is the DXMT equivalent of D3DMetal's
   * BuildEncoderSubmissionOrder/MergeEncoders stage. The array keeps recording
   * order: coalescing prepends the earlier command stream to the later encoder
   * and leaves a Null tombstone at the original position.
   */
  void planEncoderSubmissionOrder(EncoderData **encoders,
                                  unsigned encoder_count);
  DXMT_ENCODER_LIST_OP checkEncoderRelation(EncoderData* former, EncoderData* latter);
  bool hasDataDependency(EncoderData* from, EncoderData* to);
  bool tryMergeBlitEncoders(BlitEncoderData* former, BlitEncoderData* latter);
  bool tryMergeComputeEncoders(ComputeEncoderData* former, ComputeEncoderData* latter);
  WMT::Fence fenceForEncoder(EncoderId id);
  FencePoolPreparationResult
  prepareFencePool(EncoderData **encoders, unsigned encoder_count,
                   CommandBufferDiagnosticInfo *diagnostic_info);
  template <typename Fn>
  void withFence(EncoderId id, Fn &&fn) {
    if (auto fence = fenceForEncoder(id))
      fn(fence);
  }
  bool tryDeferFenceOnlyBlitPass(EncoderData* encoder);
  void appendPendingFenceOnlyBlitPass();
  void mergePendingFenceOnlyBlitPassInto(EncoderData* encoder);
  void appendComputeArgumentBufferBindings(ComputeEncoderData* data, WMT::Buffer buffer);
  bool isEncoderSignatureMatched(RenderEncoderData* former, RenderEncoderData* latter);
  RenderEncoderColorAttachmentData *isClearColorSignatureMatched(ClearEncoderData* former, RenderEncoderData* latter);
  RenderEncoderDepthAttachmentData *isClearDepthSignatureMatched(ClearEncoderData* former, RenderEncoderData* latter);
  RenderEncoderStencilAttachmentData *isClearStencilSignatureMatched(ClearEncoderData* former, RenderEncoderData* latter);

  class ResolveSignatureMatchResult {
  public:
    RenderEncoderColorAttachmentData *src{};
    TextureViewRef dst{};
  };
  ResolveSignatureMatchResult isResolveSignatureMatched(RenderEncoderData *former, ResolveEncoderData *latter);

  std::array<VertexBufferBinding, kVertexBufferSlots> vbuf_;
  Rc<Buffer> ibuf_;

  // The extent of a std::array is a std::size_t, so form the product there
  // instead of multiplying two `unsigned` constants and letting the result be
  // widened afterwards.  Same reasoning as ComputePlacedFootprintRowLayout()
  // in d3d12_copy_footprint.cpp: the target width belongs at the
  // multiplication, not after it.
  //
  // Being constants is not what makes these safe -- the values are (84, 96,
  // 768 for kStages=6/kSRVBindings=128 above).  Unsigned wraparound is
  // well-defined, so `std::array<T, kA * kB>` with an oversized `unsigned`
  // product is a *valid* constant expression that silently yields a truncated
  // extent; it is not the hard compile error a signed overflow would be.
  std::array<ConstantBufferBinding, std::size_t{14} * kStages> cbuf_;
  std::array<SamplerBinding, std::size_t{16} * kStages> sampler_;
  std::array<ResourceViewBinding, std::size_t{kSRVBindings} * kStages> resview_;

  std::array<UnorderedAccessViewBinding, kUAVBindings> om_uav_;
  std::array<UnorderedAccessViewBinding, kUAVBindings> cs_uav_;

  WMT::Reference<WMT::SamplerState> dummy_sampler_;
  WMTSamplerInfo dummy_sampler_info_;
  WMT::Reference<WMT::Buffer> dummy_cbuffer_;
  std::shared_ptr<LifetimeResidencyRegistration>
      dummy_cbuffer_residency_registration_;
  void *dummy_cbuffer_host_;
  WMTBufferInfo dummy_cbuffer_info_;
  std::array<DummyTextureBinding, 28> dummy_srv_textures_;

  EncoderData encoder_head = {EncoderType::Null, nullptr, ~0ull};
  EncoderData *encoder_last = &encoder_head;
  EncoderData *encoder_current = nullptr;
  FenceSet pending_fence_only_blit_wait_;
  FenceSet pending_fence_only_blit_update_;
  bool has_pending_fence_only_blit_ = false;
  unsigned encoder_count_ = 0;
  
  uint64_t encoder_id_ = kParityLane; // actually important to not start from 0
  /**
   * Bumped at each flushCommands boundary so GenericAccessTracker epochs drop
   * residual exclusive_/shared_ state from prior command buffers (GPTK CB-local
   * ResolveBarriers — cross-submit is timeline only).
   */
  uint64_t access_tracker_generation_ = 1;
  /**
   * Fence slots are interval-coloured within a command buffer and isolated
   * between in-flight command buffers. The queue admits at most 128 command
   * buffers, so a bank selected by event_seq_id cannot be reused until its
   * previous generation has completed.
   */
  static constexpr uint32_t kFencePoolBanks = 256;
  uint32_t fence_pool_bank_index_ = 0;
  std::array<std::vector<WMT::Reference<WMT::Fence>>, kFencePoolBanks>
      fence_pool_banks_;
  CommandBufferFenceBindingTable fence_bindings_;
  FenceLocalityCheck fence_locality_;

  uint64_t seq_id_;
  uint64_t frame_id_;
  WMT::CommandBuffer current_command_buffer_;
  std::vector<WMT::Reference<WMT::Resource>>
      *current_command_buffer_resources_ = nullptr;
  std::unordered_set<obj_handle_t>
      *current_command_buffer_resource_handles_ = nullptr;
  CommandBufferDiagnosticInfo sparse_access_diagnostic_ = {};

  struct chunk {
    void *ptr;
    size_t underused_times;

    chunk() {
      ptr = malloc(kEncodingContextCPUHeapSize);
      underused_times = 0;
    }
    chunk(const chunk &copy) = delete;
    chunk(chunk &&move) noexcept {
      ptr = move.ptr;
      underused_times = move.underused_times;
      move.ptr = nullptr;
    };
    ~chunk() {
      free(ptr);
      ptr = nullptr;
    }
  };
  std::vector<chunk> cpu_buffer_chunks_;
  uint32_t current_buffer_chunk_ = 0;
  void *cpu_buffer_;
  uint64_t cpu_buffer_offset_;

  VisibilityResultOffsetBumpState vro_state_;
  std::vector<Rc<VisibilityResultQuery>> pending_queries_;
  unsigned active_visibility_query_count_ = 0;
  Flags<FeatureCompatibility> compatibility_flag_;
  TimestampQueryState timestamp_state_;
  std::vector<Rc<VisibilityResultQuery> *> deferred_visibility_query_stack_;

  WMT::Reference<WMT::Event> barrier_event_;
  uint64_t barrier_index_ = 0;

  uint64_t intrapass_barrier_control_bits_ = 0;

  uint64_t argument_buffer_write_begin_ = 0;
  uint64_t argument_buffer_write_end_ = UINT64_MAX;
  bool argument_buffer_overflowed_ = false;

  WMT::Device device_;
  CommandQueue& queue_;
};

template <>
inline void
ArgumentEncodingContext::bindOutputBuffer<PipelineStage::Compute>(
    unsigned slot, Rc<Buffer> &&buffer, uint64_t viewId, Rc<Buffer> &&counter, BufferSlice slice
) {
  auto &entry = cs_uav_[slot];
  entry.texture = {};
  entry.buffer = std::move(buffer);
  entry.viewId = viewId;
  entry.counter = std::move(counter);
  entry.slice = slice;
}
template <>
inline void
ArgumentEncodingContext::bindOutputBuffer<PipelineStage::Pixel>(
    unsigned slot, Rc<Buffer> &&buffer, uint64_t viewId, Rc<Buffer> &&counter, BufferSlice slice
) {
  auto &entry = om_uav_[slot];
  entry.texture = {};
  entry.buffer = std::move(buffer);
  entry.viewId = viewId;
  entry.counter = std::move(counter);
  entry.slice = slice;
}

template <>
inline void
ArgumentEncodingContext::bindOutputTexture<PipelineStage::Compute>(
    unsigned slot, Rc<Texture> &&texture, uint64_t viewId
) {
  auto &entry = cs_uav_[slot];
  entry.buffer = {};
  entry.texture = std::move(texture);
  entry.viewId = viewId;
}
template <>
inline void
ArgumentEncodingContext::bindOutputTexture<PipelineStage::Pixel>(
    unsigned slot, Rc<Texture> &&texture, uint64_t viewId
) {
  auto &entry = om_uav_[slot];
  entry.buffer = {};
  entry.texture = std::move(texture);
  entry.viewId = viewId;
}

template <PipelineStage stage>
inline void
ArgumentEncodingContext::track(GenericAccessTracker &tracker, int flags) {
  tracker.ensureGeneration(access_tracker_generation_);
  auto current_encoder = currentRenderEncoder();
  auto id = current_encoder->encoder_id_vertex;
  EncoderBarrierState &barrier_state = current_encoder->barrier_state;
  if (flags & ResourceAccess::Write)
    tracker.accessExclusivePreRaster(
        id, current_encoder->fence_wait_vertex, barrier_state, flags & ResourceAccess::UAV
    );
  else
    tracker.accessSharedPreRaster(id, current_encoder->fence_wait_vertex, barrier_state);
}

template <>
inline void
ArgumentEncodingContext::track<PipelineStage::Compute>(GenericAccessTracker &tracker, int flags) {
  tracker.ensureGeneration(access_tracker_generation_);
  auto current_encoder = currentEncoder();
  EncoderBarrierState &barrier_state = current_encoder->barrier_state;
  if (flags & ResourceAccess::Write)
    tracker.accessExclusive(
        currentEncoderId(), current_encoder->fence_wait, barrier_state, flags & ResourceAccess::UAV
    );
  else
    tracker.accessShared(currentEncoderId(), current_encoder->fence_wait, barrier_state);
}

template <>
inline void
ArgumentEncodingContext::track<PipelineStage::Pixel>(GenericAccessTracker &tracker, int flags) {
  tracker.ensureGeneration(access_tracker_generation_);
  auto current_encoder = currentRenderEncoder();
  EncoderBarrierState &barrier_state = current_encoder->barrier_state;
  if (flags & ResourceAccess::Write)
    tracker.accessExclusiveFragment(
        currentEncoderId(), current_encoder->fence_wait, barrier_state, flags & ResourceAccess::UAV
    );
  else
    tracker.accessSharedFragment(currentEncoderId(), current_encoder->fence_wait, barrier_state);
  return;
}

} // namespace dxmt
