#pragma once

#include <d3d12.h>

#include <cstddef>
#include <cstdint>

namespace dxmt::apitrace {

enum class D3DObjectKind {
  Unknown,
  Device,
  CommandQueue,
  CommandAllocator,
  CommandList,
  CommandSignature,
  Fence,
  SwapChain,
  Resource,
  View,
  Shader,
  PipelineState,
  RootSignature,
  DescriptorHeap,
  QueryHeap,
};

struct RenderPassClearValue {
  uint32_t format = 0;
  float color[4] = {};
  float depth = 0.0f;
  uint8_t stencil = 0;
};

struct RenderPassBeginningAccessDesc {
  uint32_t type = 0;
  RenderPassClearValue clear = {};
};

struct RenderPassResolveSubresourceDesc {
  uint32_t src_subresource = 0;
  uint32_t dst_subresource = 0;
  uint32_t dst_x = 0;
  uint32_t dst_y = 0;
  bool has_src_rect = false;
  int32_t src_left = 0;
  int32_t src_top = 0;
  int32_t src_right = 0;
  int32_t src_bottom = 0;
};

struct RenderPassEndingAccessDesc {
  uint32_t type = 0;
  const void *src_resource = nullptr;
  const void *dst_resource = nullptr;
  uint32_t subresource_count = 0;
  const RenderPassResolveSubresourceDesc *subresources = nullptr;
  uint32_t format = 0;
  uint32_t resolve_mode = 0;
  bool preserve_resolve_source = false;
};

struct RenderPassRenderTargetDesc {
  uint64_t cpu_descriptor = 0;
  RenderPassBeginningAccessDesc beginning_access = {};
  RenderPassEndingAccessDesc ending_access = {};
};

struct RenderPassDepthStencilDesc {
  uint64_t cpu_descriptor = 0;
  RenderPassBeginningAccessDesc depth_beginning_access = {};
  RenderPassBeginningAccessDesc stencil_beginning_access = {};
  RenderPassEndingAccessDesc depth_ending_access = {};
  RenderPassEndingAccessDesc stencil_ending_access = {};
};

#ifdef DXMT_APITRACE_D3D

bool d3d_enabled();
void set_current_d3d_sequence(uint64_t d3d_sequence);

uint64_t begin_d3d_call(const char *opname);
uint64_t begin_d3d_call(const char *opname, const char *payload_json);
uint64_t begin_d3d_call(const char *opname, const char *payload_json,
                        const void *const *object_refs, uint32_t object_ref_count,
                        const uint64_t *blob_refs, uint32_t blob_ref_count,
                        int32_t result_code = 0);
uint64_t record_call(const char *opname);
uint64_t record_call(const char *opname, const char *payload_json);
uint64_t record_call(const char *opname, const char *payload_json,
                     const void *const *object_refs, uint32_t object_ref_count,
                     const uint64_t *blob_refs, uint32_t blob_ref_count,
                     int32_t result_code = 0);
void end_d3d_call(uint64_t seq);

uint64_t current_d3d_sequence();
uint64_t object_id(const void *object);
uint64_t blob_id_for_bytes(const char *debug_name, const void *data, size_t size);
void record_object_create(const void *object, D3DObjectKind kind, const void *parent_object,
                          const char *debug_name, const char *payload_json);
void record_object_destroy(const void *object, D3DObjectKind kind, const char *payload_json);
void record_resource_blob(const char *debug_name, const uint64_t *blob_refs,
                          uint32_t blob_ref_count, const char *payload_json);

uint64_t record_create_command_queue(ID3D12Device *device,
                                     const D3D12_COMMAND_QUEUE_DESC *desc,
                                     const void *command_queue,
                                     int32_t result_code);
uint64_t record_create_command_allocator(ID3D12Device *device, uint32_t type,
                                         const void *command_allocator,
                                         int32_t result_code);
uint64_t record_create_command_list(ID3D12Device *device, uint32_t node_mask,
                                    uint32_t type, const void *command_allocator,
                                    const void *initial_pipeline_state,
                                    const void *command_list,
                                    int32_t result_code);
uint64_t record_create_command_list1(ID3D12Device *device, uint32_t node_mask,
                                     uint32_t type, uint32_t flags,
                                     const void *command_list,
                                     int32_t result_code);
uint64_t record_create_graphics_pipeline_state(
    ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
    const void *pipeline_state, int32_t result_code);
uint64_t record_create_compute_pipeline_state(
    ID3D12Device *device, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
    const void *pipeline_state, int32_t result_code);
uint64_t record_create_pipeline_state(ID3D12Device *device, const void *stream,
                                      size_t stream_size,
                                      const void *pipeline_state,
                                      int32_t result_code);
uint64_t record_create_descriptor_heap(ID3D12Device *device,
                                       const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                                       const void *descriptor_heap,
                                       uint32_t descriptor_size,
                                       uint64_t cpu_start,
                                       uint64_t gpu_start,
                                       int32_t result_code);
uint64_t record_create_query_heap(ID3D12Device *device,
                                  const D3D12_QUERY_HEAP_DESC *desc,
                                  const void *query_heap,
                                  int32_t result_code);
uint64_t record_create_root_signature(ID3D12Device *device, uint32_t node_mask,
                                      const void *bytecode,
                                      size_t bytecode_length,
                                      const void *root_signature,
                                      int32_t result_code);
uint64_t record_create_committed_resource(
    ID3D12Device *device, const D3D12_HEAP_PROPERTIES *heap_properties,
    uint32_t heap_flags, const D3D12_RESOURCE_DESC *desc,
    uint32_t initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value,
    const void *resource, uint64_t gpu_virtual_address, int32_t result_code);
uint64_t record_create_heap(ID3D12Device *device, const D3D12_HEAP_DESC *desc,
                            const void *heap, int32_t result_code);
uint64_t record_create_placed_resource(
    ID3D12Device *device, const void *heap, uint64_t heap_offset,
    const D3D12_RESOURCE_DESC *desc, uint32_t initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value, const void *resource,
    uint64_t gpu_virtual_address, int32_t result_code);
uint64_t record_create_fence(ID3D12Device *device, uint64_t initial_value,
                             uint32_t flags, const void *fence,
                             int32_t result_code);
uint64_t record_create_command_signature(
    ID3D12Device *device, const D3D12_COMMAND_SIGNATURE_DESC *desc,
    const void *root_signature, const void *command_signature,
    int32_t result_code);
uint64_t record_create_constant_buffer_view(
    ID3D12Device *device, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor, const void *resolved_resource,
    uint64_t resolved_resource_offset, uint64_t resolved_resource_width);
uint64_t record_create_shader_resource_view(
    ID3D12Device *device, const void *resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
uint64_t record_create_unordered_access_view(
    ID3D12Device *device, const void *resource, const void *counter_resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
uint64_t record_create_render_target_view(
    ID3D12Device *device, const void *resource,
    const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
uint64_t record_create_depth_stencil_view(
    ID3D12Device *device, const void *resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
uint64_t record_create_sampler(ID3D12Device *device,
                               const D3D12_SAMPLER_DESC *desc,
                               D3D12_CPU_DESCRIPTOR_HANDLE descriptor);
uint64_t record_copy_descriptors(ID3D12Device *device,
                                 uint32_t dst_descriptor_range_count,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
                                 const uint32_t *dst_descriptor_range_sizes,
                                 uint32_t src_descriptor_range_count,
                                 const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
                                 const uint32_t *src_descriptor_range_sizes,
                                 uint32_t descriptor_heap_type,
                                 uint32_t descriptor_size);
uint64_t record_copy_descriptors_simple(ID3D12Device *device,
                                        uint32_t descriptor_count,
                                        D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_start,
                                        D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_start,
                                        uint32_t descriptor_heap_type,
                                        uint32_t descriptor_size);

uint64_t record_draw_instanced(const void *command_list,
                               uint32_t vertex_count_per_instance,
                               uint32_t instance_count,
                               uint32_t start_vertex_location,
                               uint32_t start_instance_location);
uint64_t record_draw_indexed_instanced(const void *command_list,
                                       uint32_t index_count_per_instance,
                                       uint32_t instance_count,
                                       uint32_t start_index_location,
                                       int32_t base_vertex_location,
                                       uint32_t start_instance_location);
uint64_t record_dispatch(const void *command_list, uint32_t x, uint32_t y,
                         uint32_t z);
uint64_t record_close_command_list(const void *command_list,
                                   int32_t result_code);
uint64_t record_reset_command_list(const void *command_list,
                                   const void *command_allocator,
                                   const void *initial_pipeline_state,
                                   int32_t result_code);
uint64_t record_execute_indirect(const void *command_list,
                                 const void *command_signature,
                                 uint32_t max_command_count,
                                 const void *arg_buffer,
                                 uint64_t arg_buffer_offset,
                                 const void *count_buffer,
                                 uint64_t count_buffer_offset);
uint64_t record_execute_bundle(const void *command_list,
                               const void *bundle_command_list);
uint64_t record_copy_buffer_region(const char *function_name,
                                   const void *command_list,
                                   const void *dst_buffer,
                                   uint64_t dst_offset,
                                   const void *src_buffer,
                                   uint64_t src_offset,
                                   uint64_t byte_count);
uint64_t record_copy_texture_region(const void *command_list,
                                    const D3D12_TEXTURE_COPY_LOCATION *dst,
                                    uint32_t dst_x, uint32_t dst_y,
                                    uint32_t dst_z,
                                    const D3D12_TEXTURE_COPY_LOCATION *src,
                                    const D3D12_BOX *src_box);
uint64_t record_copy_resource(const void *command_list, const void *dst_resource,
                              const void *src_resource);
uint64_t record_resolve_subresource(const void *command_list,
                                    const void *dst_resource,
                                    uint32_t dst_subresource,
                                    const void *src_resource,
                                    uint32_t src_subresource,
                                    uint32_t format);
uint64_t record_ia_set_primitive_topology(const void *command_list,
                                          uint32_t primitive_topology);
uint64_t record_rs_set_viewports(const void *command_list,
                                 uint32_t viewport_count,
                                 const D3D12_VIEWPORT *viewports);
uint64_t record_rs_set_scissor_rects(const void *command_list,
                                     uint32_t rect_count, const void *rects);
uint64_t record_set_pipeline_state(const void *command_list,
                                   const void *pipeline_state);
uint64_t record_clear_state(const void *command_list,
                            const void *pipeline_state);
uint64_t record_om_set_blend_factor(const void *command_list,
                                    const float blend_factor[4]);
uint64_t record_om_set_stencil_ref(const void *command_list,
                                   uint32_t stencil_ref);
uint64_t record_resource_barrier(const void *command_list,
                                 uint32_t barrier_count,
                                 const D3D12_RESOURCE_BARRIER *barriers);
uint64_t record_set_descriptor_heaps(const void *command_list,
                                     uint32_t heap_count,
                                     const void *const *heaps);
uint64_t record_set_root_signature(const void *command_list, bool compute,
                                   const void *root_signature);
uint64_t record_set_root_descriptor_table(
    const void *command_list, bool compute, uint32_t root_parameter_index,
    D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor);
uint64_t record_set_root_32bit_constants(
    const void *command_list, bool compute, uint32_t root_parameter_index,
    uint32_t constant_count, const uint32_t *values, uint32_t dst_offset);
uint64_t record_set_root_descriptor(const void *command_list, bool compute,
                                    uint32_t parameter_type,
                                    uint32_t root_parameter_index,
                                    uint64_t gpu_virtual_address);
uint64_t record_ia_set_index_buffer(const void *command_list,
                                    const D3D12_INDEX_BUFFER_VIEW *view);
uint64_t record_ia_set_vertex_buffers(const void *command_list,
                                      uint32_t start_slot, uint32_t view_count,
                                      const D3D12_VERTEX_BUFFER_VIEW *views);
uint64_t record_om_set_render_targets(
    const void *command_list, uint32_t render_target_descriptor_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
    bool single_descriptor_handle,
    const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor);
uint64_t record_clear_depth_stencil_view(const void *command_list,
                                         D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                         uint32_t flags, float depth,
                                         uint8_t stencil, uint32_t rect_count,
                                         const void *rects);
uint64_t record_clear_render_target_view(const void *command_list,
                                         D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                         const float color[4],
                                         uint32_t rect_count,
                                         const void *rects);
uint64_t record_clear_unordered_access_view_uint(
    const void *command_list, D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor, const void *resource,
    const uint32_t values[4], uint32_t rect_count, const void *rects);
uint64_t record_clear_unordered_access_view_float(
    const void *command_list, D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor, const void *resource,
    const float values[4], uint32_t rect_count, const void *rects);
uint64_t record_discard_resource(const void *command_list, const void *resource,
                                 uint32_t first_subresource,
                                 uint32_t subresource_count,
                                 uint32_t rect_count, const void *rects);
uint64_t record_begin_query(const void *command_list, const void *query_heap,
                            uint32_t type, uint32_t index);
uint64_t record_end_query(const void *command_list, const void *query_heap,
                          uint32_t type, uint32_t index);
uint64_t record_resolve_query_data(const void *command_list,
                                   const void *query_heap, uint32_t type,
                                   uint32_t start_index, uint32_t query_count,
                                   const void *dst_buffer,
                                   uint64_t aligned_dst_buffer_offset);
uint64_t record_set_predication(const void *command_list, const void *buffer,
                                uint64_t aligned_buffer_offset,
                                uint32_t operation);
uint64_t record_resolve_subresource_region(
    const void *command_list, const void *dst_resource, uint32_t dst_subresource,
    uint32_t dst_x, uint32_t dst_y, const void *src_resource,
    uint32_t src_subresource, const void *src_rect, uint32_t format,
    uint32_t mode);
uint64_t record_write_buffer_immediate(const void *command_list, uint32_t count,
                                       const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
                                       const void *modes);
uint64_t record_begin_render_pass(const void *command_list,
                                  uint32_t render_targets_count,
                                  const RenderPassRenderTargetDesc *render_targets,
                                  const RenderPassDepthStencilDesc *depth_stencil,
                                  uint32_t flags);
uint64_t record_end_render_pass(const void *command_list);
uint64_t record_temporal_upscale(
    const void *command_list, uint32_t input_content_width,
    uint32_t input_content_height, bool auto_exposure, bool in_reset,
    bool depth_reversed, bool motion_vector_in_display_res, const void *color,
    const void *depth, const void *motion_vector, const void *output,
    float motion_vector_scale_x, float motion_vector_scale_y,
    float pre_exposure, const void *exposure_texture, float jitter_offset_x,
    float jitter_offset_y);

void on_d3d12_create_device(void *device);
void on_d3d11_create_device(void *device);
void on_dxgi_create_swapchain(void *factory, void *device, void *swapchain);
void on_d3d12_execute_command_lists(void *queue, void *command_list);
void on_d3d12_resource_barrier(void *command_list, uint32_t count);
uint64_t on_d3d12_present(void *swapchain, uint32_t sync_interval, uint32_t flags,
                          int32_t result_code, bool frame_presented);
void record_present_frame(uint64_t frame_index, uint32_t width, uint32_t height,
                          uint32_t row_pitch, uint32_t sync_interval,
                          uint32_t flags, const void *rgba_data,
                          size_t rgba_size);
void on_fence_dependency(
    const char *scope,
    uint64_t d3d_sequence,
    uint64_t encoder_id,
    bool implicit_pre_raster_wait,
    const uint64_t *strong_masks,
    uint32_t strong_count,
    const uint64_t *full_masks,
    uint32_t full_count,
    const uint64_t *minimal_masks,
    uint32_t minimal_count,
    uint32_t mask_count);

#else

inline bool d3d_enabled() { return false; }
inline uint64_t begin_d3d_call(const char *) { return 0; }
inline uint64_t begin_d3d_call(const char *, const char *) { return 0; }
inline uint64_t begin_d3d_call(const char *, const char *, const void *const *, uint32_t,
                               const uint64_t *, uint32_t, int32_t = 0) { return 0; }
inline uint64_t record_call(const char *) { return 0; }
inline uint64_t record_call(const char *, const char *) { return 0; }
inline uint64_t record_call(const char *, const char *, const void *const *, uint32_t,
                            const uint64_t *, uint32_t, int32_t = 0) { return 0; }
inline void end_d3d_call(uint64_t) {}
inline uint64_t current_d3d_sequence() { return 0; }
inline uint64_t object_id(const void *) { return 0; }
inline uint64_t blob_id_for_bytes(const char *, const void *, size_t) { return 0; }
inline void record_object_create(const void *, D3DObjectKind, const void *, const char *, const char *) {}
inline void record_object_destroy(const void *, D3DObjectKind, const char *) {}
inline void record_resource_blob(const char *, const uint64_t *, uint32_t, const char *) {}
inline uint64_t record_create_command_queue(ID3D12Device *, const D3D12_COMMAND_QUEUE_DESC *, const void *, int32_t) { return 0; }
inline uint64_t record_create_command_allocator(ID3D12Device *, uint32_t, const void *, int32_t) { return 0; }
inline uint64_t record_create_command_list(ID3D12Device *, uint32_t, uint32_t, const void *, const void *, const void *, int32_t) { return 0; }
inline uint64_t record_create_command_list1(ID3D12Device *, uint32_t, uint32_t, uint32_t, const void *, int32_t) { return 0; }
inline uint64_t record_create_graphics_pipeline_state(ID3D12Device *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *, const void *, int32_t) { return 0; }
inline uint64_t record_create_compute_pipeline_state(ID3D12Device *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *, const void *, int32_t) { return 0; }
inline uint64_t record_create_pipeline_state(ID3D12Device *, const void *, size_t, const void *, int32_t) { return 0; }
inline uint64_t record_create_descriptor_heap(ID3D12Device *, const D3D12_DESCRIPTOR_HEAP_DESC *, const void *, uint32_t, uint64_t, uint64_t, int32_t) { return 0; }
inline uint64_t record_create_query_heap(ID3D12Device *, const D3D12_QUERY_HEAP_DESC *, const void *, int32_t) { return 0; }
inline uint64_t record_create_root_signature(ID3D12Device *, uint32_t, const void *, size_t, const void *, int32_t) { return 0; }
inline uint64_t record_create_committed_resource(ID3D12Device *, const D3D12_HEAP_PROPERTIES *, uint32_t, const D3D12_RESOURCE_DESC *, uint32_t, const D3D12_CLEAR_VALUE *, const void *, uint64_t, int32_t) { return 0; }
inline uint64_t record_create_heap(ID3D12Device *, const D3D12_HEAP_DESC *, const void *, int32_t) { return 0; }
inline uint64_t record_create_placed_resource(ID3D12Device *, const void *, uint64_t, const D3D12_RESOURCE_DESC *, uint32_t, const D3D12_CLEAR_VALUE *, const void *, uint64_t, int32_t) { return 0; }
inline uint64_t record_create_fence(ID3D12Device *, uint64_t, uint32_t, const void *, int32_t) { return 0; }
inline uint64_t record_create_command_signature(ID3D12Device *, const D3D12_COMMAND_SIGNATURE_DESC *, const void *, const void *, int32_t) { return 0; }
inline uint64_t record_create_constant_buffer_view(ID3D12Device *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE, const void *, uint64_t, uint64_t) { return 0; }
inline uint64_t record_create_shader_resource_view(ID3D12Device *, const void *, const D3D12_SHADER_RESOURCE_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) { return 0; }
inline uint64_t record_create_unordered_access_view(ID3D12Device *, const void *, const void *, const D3D12_UNORDERED_ACCESS_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) { return 0; }
inline uint64_t record_create_render_target_view(ID3D12Device *, const void *, const D3D12_RENDER_TARGET_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) { return 0; }
inline uint64_t record_create_depth_stencil_view(ID3D12Device *, const void *, const D3D12_DEPTH_STENCIL_VIEW_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) { return 0; }
inline uint64_t record_create_sampler(ID3D12Device *, const D3D12_SAMPLER_DESC *, D3D12_CPU_DESCRIPTOR_HANDLE) { return 0; }
inline uint64_t record_copy_descriptors(ID3D12Device *, uint32_t, const D3D12_CPU_DESCRIPTOR_HANDLE *, const uint32_t *, uint32_t, const D3D12_CPU_DESCRIPTOR_HANDLE *, const uint32_t *, uint32_t, uint32_t) { return 0; }
inline uint64_t record_copy_descriptors_simple(ID3D12Device *, uint32_t, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, uint32_t, uint32_t) { return 0; }
inline uint64_t record_draw_instanced(const void *, uint32_t, uint32_t, uint32_t, uint32_t) { return 0; }
inline uint64_t record_draw_indexed_instanced(const void *, uint32_t, uint32_t, uint32_t, int32_t, uint32_t) { return 0; }
inline uint64_t record_dispatch(const void *, uint32_t, uint32_t, uint32_t) { return 0; }
inline uint64_t record_close_command_list(const void *, int32_t) { return 0; }
inline uint64_t record_reset_command_list(const void *, const void *, const void *, int32_t) { return 0; }
inline uint64_t record_execute_indirect(const void *, const void *, uint32_t, const void *, uint64_t, const void *, uint64_t) { return 0; }
inline uint64_t record_execute_bundle(const void *, const void *) { return 0; }
inline uint64_t record_copy_buffer_region(const char *, const void *, const void *, uint64_t, const void *, uint64_t, uint64_t) { return 0; }
inline uint64_t record_copy_texture_region(const void *, const D3D12_TEXTURE_COPY_LOCATION *, uint32_t, uint32_t, uint32_t, const D3D12_TEXTURE_COPY_LOCATION *, const D3D12_BOX *) { return 0; }
inline uint64_t record_copy_resource(const void *, const void *, const void *) { return 0; }
inline uint64_t record_resolve_subresource(const void *, const void *, uint32_t, const void *, uint32_t, uint32_t) { return 0; }
inline uint64_t record_ia_set_primitive_topology(const void *, uint32_t) { return 0; }
inline uint64_t record_rs_set_viewports(const void *, uint32_t, const D3D12_VIEWPORT *) { return 0; }
inline uint64_t record_rs_set_scissor_rects(const void *, uint32_t, const void *) { return 0; }
inline uint64_t record_set_pipeline_state(const void *, const void *) { return 0; }
inline uint64_t record_clear_state(const void *, const void *) { return 0; }
inline uint64_t record_om_set_blend_factor(const void *, const float[4]) { return 0; }
inline uint64_t record_om_set_stencil_ref(const void *, uint32_t) { return 0; }
inline uint64_t record_resource_barrier(const void *, uint32_t, const D3D12_RESOURCE_BARRIER *) { return 0; }
inline uint64_t record_set_descriptor_heaps(const void *, uint32_t, const void *const *) { return 0; }
inline uint64_t record_set_root_signature(const void *, bool, const void *) { return 0; }
inline uint64_t record_set_root_descriptor_table(const void *, bool, uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE) { return 0; }
inline uint64_t record_set_root_32bit_constants(const void *, bool, uint32_t, uint32_t, const uint32_t *, uint32_t) { return 0; }
inline uint64_t record_set_root_descriptor(const void *, bool, uint32_t, uint32_t, uint64_t) { return 0; }
inline uint64_t record_ia_set_index_buffer(const void *, const D3D12_INDEX_BUFFER_VIEW *) { return 0; }
inline uint64_t record_ia_set_vertex_buffers(const void *, uint32_t, uint32_t, const D3D12_VERTEX_BUFFER_VIEW *) { return 0; }
inline uint64_t record_om_set_render_targets(const void *, uint32_t, const D3D12_CPU_DESCRIPTOR_HANDLE *, bool, const D3D12_CPU_DESCRIPTOR_HANDLE *) { return 0; }
inline uint64_t record_clear_depth_stencil_view(const void *, D3D12_CPU_DESCRIPTOR_HANDLE, uint32_t, float, uint8_t, uint32_t, const void *) { return 0; }
inline uint64_t record_clear_render_target_view(const void *, D3D12_CPU_DESCRIPTOR_HANDLE, const float[4], uint32_t, const void *) { return 0; }
inline uint64_t record_clear_unordered_access_view_uint(const void *, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, const void *, const uint32_t[4], uint32_t, const void *) { return 0; }
inline uint64_t record_clear_unordered_access_view_float(const void *, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, const void *, const float[4], uint32_t, const void *) { return 0; }
inline uint64_t record_discard_resource(const void *, const void *, uint32_t, uint32_t, uint32_t, const void *) { return 0; }
inline uint64_t record_begin_query(const void *, const void *, uint32_t, uint32_t) { return 0; }
inline uint64_t record_end_query(const void *, const void *, uint32_t, uint32_t) { return 0; }
inline uint64_t record_resolve_query_data(const void *, const void *, uint32_t, uint32_t, uint32_t, const void *, uint64_t) { return 0; }
inline uint64_t record_set_predication(const void *, const void *, uint64_t, uint32_t) { return 0; }
inline uint64_t record_resolve_subresource_region(const void *, const void *, uint32_t, uint32_t, uint32_t, const void *, uint32_t, const void *, uint32_t, uint32_t) { return 0; }
inline uint64_t record_write_buffer_immediate(const void *, uint32_t, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *, const void *) { return 0; }
inline uint64_t record_begin_render_pass(const void *, uint32_t, const RenderPassRenderTargetDesc *, const RenderPassDepthStencilDesc *, uint32_t) { return 0; }
inline uint64_t record_end_render_pass(const void *) { return 0; }
inline uint64_t record_temporal_upscale(const void *, uint32_t, uint32_t, bool, bool, bool, bool, const void *, const void *, const void *, const void *, float, float, float, const void *, float, float) { return 0; }
inline void on_d3d12_create_device(void *) {}
inline void on_d3d11_create_device(void *) {}
inline void on_dxgi_create_swapchain(void *, void *, void *) {}
inline void on_d3d12_execute_command_lists(void *, void *) {}
inline void on_d3d12_resource_barrier(void *, uint32_t) {}
inline uint64_t on_d3d12_present(void *, uint32_t, uint32_t, int32_t, bool) { return ~0ull; }
inline void record_present_frame(uint64_t, uint32_t, uint32_t, uint32_t,
                                 uint32_t, uint32_t, const void *, size_t) {}
inline void on_fence_dependency(
    const char *,
    uint64_t,
    uint64_t,
    bool,
    const uint64_t *,
    uint32_t,
    const uint64_t *,
    uint32_t,
    const uint64_t *,
    uint32_t,
    uint32_t) {}

#endif

} // namespace dxmt::apitrace
