#include "dxmt_apitrace_d3d.hpp"

#ifdef DXMT_APITRACE_D3D

#include "dxmt_apitrace.hpp"

#include "apitrace/d3d12_capture.hpp"

#include <cstdlib>
#include <cstring>

namespace dxmt::apitrace {
namespace {

bool
d3d_builtin_capture_disabled() {
  const char *value = std::getenv("APITRACE_D3D12_BUILTIN_CAPTURE");
  return value && (std::strcmp(value, "0") == 0 ||
                   std::strcmp(value, "false") == 0 ||
                   std::strcmp(value, "FALSE") == 0);
}

} // namespace

bool
d3d_enabled() {
  return enabled() && !d3d_builtin_capture_disabled();
}

uint64_t
begin_d3d_call(const char *opname) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_call(opname);
}

uint64_t
begin_d3d_call(const char *opname, const char *payload_json) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_call(opname, payload_json);
}

uint64_t
begin_d3d_call(const char *opname, const char *payload_json,
               const void *const *object_refs, uint32_t object_ref_count,
               const uint64_t *blob_refs, uint32_t blob_ref_count,
               int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_call(
      opname, payload_json, object_refs, object_ref_count, blob_refs,
      blob_ref_count, result_code);
}

uint64_t
record_call(const char *opname) {
  return begin_d3d_call(opname);
}

uint64_t
record_call(const char *opname, const char *payload_json) {
  return begin_d3d_call(opname, payload_json);
}

uint64_t
record_call(const char *opname, const char *payload_json,
            const void *const *object_refs, uint32_t object_ref_count,
            const uint64_t *blob_refs, uint32_t blob_ref_count,
            int32_t result_code) {
  return begin_d3d_call(opname, payload_json, object_refs, object_ref_count,
                        blob_refs, blob_ref_count, result_code);
}

void
end_d3d_call(uint64_t) {}

uint64_t
current_d3d_sequence() {
  if (!d3d_enabled())
    return 0;
  return ::apitrace::d3d12::current_sequence();
}

uint64_t
object_id(const void *object) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::object_id(object);
}

uint64_t
blob_id_for_bytes(const char *debug_name, const void *data, size_t size) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::register_blob(debug_name, data, size);
}

static ::apitrace::d3d12::CaptureObjectKind
ToCaptureObjectKind(D3DObjectKind kind) {
  switch (kind) {
  case D3DObjectKind::Device:
    return ::apitrace::d3d12::CaptureObjectKind::Device;
  case D3DObjectKind::CommandQueue:
    return ::apitrace::d3d12::CaptureObjectKind::CommandQueue;
  case D3DObjectKind::CommandAllocator:
    return ::apitrace::d3d12::CaptureObjectKind::CommandAllocator;
  case D3DObjectKind::CommandList:
    return ::apitrace::d3d12::CaptureObjectKind::CommandList;
  case D3DObjectKind::CommandSignature:
    return ::apitrace::d3d12::CaptureObjectKind::CommandSignature;
  case D3DObjectKind::Fence:
    return ::apitrace::d3d12::CaptureObjectKind::Fence;
  case D3DObjectKind::SwapChain:
    return ::apitrace::d3d12::CaptureObjectKind::SwapChain;
  case D3DObjectKind::Resource:
    return ::apitrace::d3d12::CaptureObjectKind::Resource;
  case D3DObjectKind::View:
    return ::apitrace::d3d12::CaptureObjectKind::View;
  case D3DObjectKind::Shader:
    return ::apitrace::d3d12::CaptureObjectKind::Shader;
  case D3DObjectKind::PipelineState:
    return ::apitrace::d3d12::CaptureObjectKind::PipelineState;
  case D3DObjectKind::RootSignature:
    return ::apitrace::d3d12::CaptureObjectKind::RootSignature;
  case D3DObjectKind::DescriptorHeap:
    return ::apitrace::d3d12::CaptureObjectKind::DescriptorHeap;
  case D3DObjectKind::QueryHeap:
    return ::apitrace::d3d12::CaptureObjectKind::QueryHeap;
  case D3DObjectKind::Unknown:
  default:
    return ::apitrace::d3d12::CaptureObjectKind::Unknown;
  }
}

void
record_object_create(const void *object, D3DObjectKind kind, const void *parent_object,
                     const char *debug_name, const char *payload_json) {
  if (!d3d_enabled())
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_object_create(
      object, ToCaptureObjectKind(kind), parent_object, debug_name, payload_json);
}

void
record_object_destroy(const void *object, D3DObjectKind kind, const char *payload_json) {
  if (!d3d_enabled())
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_object_destroy(
      object, ToCaptureObjectKind(kind), payload_json);
}

void
record_resource_blob(const char *debug_name, const uint64_t *blob_refs,
                     uint32_t blob_ref_count, const char *payload_json) {
  if (!d3d_enabled())
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_resource_blob(
      debug_name, blob_refs, blob_ref_count, payload_json);
}

uint64_t
record_create_command_queue(ID3D12Device *device,
                            const D3D12_COMMAND_QUEUE_DESC *desc,
                            const void *command_queue,
                            int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_command_queue(
      device, desc, command_queue, result_code);
}

uint64_t
record_create_command_allocator(ID3D12Device *device, uint32_t type,
                                const void *command_allocator,
                                int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_command_allocator(
      device, type, command_allocator, result_code);
}

uint64_t
record_create_command_list(ID3D12Device *device, uint32_t node_mask,
                           uint32_t type, const void *command_allocator,
                           const void *initial_pipeline_state,
                           const void *command_list,
                           int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_command_list(
      device, node_mask, type, command_allocator, initial_pipeline_state,
      command_list, result_code);
}

uint64_t
record_create_command_list1(ID3D12Device *device, uint32_t node_mask,
                            uint32_t type, uint32_t flags,
                            const void *command_list, int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_command_list1(
      device, node_mask, type, flags, command_list, result_code);
}

uint64_t
record_create_graphics_pipeline_state(
    ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
    const void *pipeline_state, int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_graphics_pipeline_state(
      device, desc, pipeline_state, result_code);
}

uint64_t
record_create_compute_pipeline_state(
    ID3D12Device *device, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
    const void *pipeline_state, int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_compute_pipeline_state(
      device, desc, pipeline_state, result_code);
}

uint64_t
record_create_pipeline_state(ID3D12Device *device, const void *stream,
                             size_t stream_size, const void *pipeline_state,
                             int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_pipeline_state(
      device, stream, stream_size, pipeline_state, result_code);
}

uint64_t
record_create_descriptor_heap(ID3D12Device *device,
                              const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                              const void *descriptor_heap,
                              uint32_t descriptor_size,
                              uint64_t cpu_start,
                              uint64_t gpu_start,
                              int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_descriptor_heap(
      device, desc, descriptor_heap, descriptor_size, cpu_start, gpu_start,
      result_code);
}

uint64_t
record_create_query_heap(ID3D12Device *device,
                         const D3D12_QUERY_HEAP_DESC *desc,
                         const void *query_heap,
                         int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_query_heap(
      device, desc, query_heap, result_code);
}

uint64_t
record_create_root_signature(ID3D12Device *device, uint32_t node_mask,
                             const void *bytecode, size_t bytecode_length,
                             const void *root_signature,
                             int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_root_signature(
      device, node_mask, bytecode, bytecode_length, root_signature,
      result_code);
}

uint64_t
record_create_committed_resource(
    ID3D12Device *device, const D3D12_HEAP_PROPERTIES *heap_properties,
    uint32_t heap_flags, const D3D12_RESOURCE_DESC *desc,
    uint32_t initial_state, const D3D12_CLEAR_VALUE *optimized_clear_value,
    const void *resource, uint64_t gpu_virtual_address, int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_committed_resource(
      device, heap_properties, heap_flags, desc, initial_state,
      optimized_clear_value, resource, gpu_virtual_address, result_code);
}

uint64_t
record_create_heap(ID3D12Device *device, const D3D12_HEAP_DESC *desc,
                   const void *heap, int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_heap(
      device, desc, heap, result_code);
}

uint64_t
record_create_placed_resource(
    ID3D12Device *device, const void *heap, uint64_t heap_offset,
    const D3D12_RESOURCE_DESC *desc, uint32_t initial_state,
    const D3D12_CLEAR_VALUE *optimized_clear_value, const void *resource,
    uint64_t gpu_virtual_address, int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_placed_resource(
      device, heap, heap_offset, desc, initial_state, optimized_clear_value,
      resource, gpu_virtual_address, result_code);
}

uint64_t
record_create_fence(ID3D12Device *device, uint64_t initial_value,
                    uint32_t flags, const void *fence, int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_fence(
      device, initial_value, flags, fence, result_code);
}

uint64_t
record_create_command_signature(
    ID3D12Device *device, const D3D12_COMMAND_SIGNATURE_DESC *desc,
    const void *root_signature, const void *command_signature,
    int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_command_signature(
      device, desc, root_signature, command_signature, result_code);
}

uint64_t
record_create_constant_buffer_view(
    ID3D12Device *device, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor, const void *resolved_resource,
    uint64_t resolved_resource_offset, uint64_t resolved_resource_width) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_constant_buffer_view(
      device, desc, descriptor, resolved_resource, resolved_resource_offset,
      resolved_resource_width);
}

uint64_t
record_create_shader_resource_view(
    ID3D12Device *device, const void *resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_shader_resource_view(
      device, resource, desc, descriptor);
}

uint64_t
record_create_unordered_access_view(
    ID3D12Device *device, const void *resource, const void *counter_resource,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_unordered_access_view(
      device, resource, counter_resource, desc, descriptor);
}

uint64_t
record_create_render_target_view(
    ID3D12Device *device, const void *resource,
    const D3D12_RENDER_TARGET_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_render_target_view(
      device, resource, desc, descriptor);
}

uint64_t
record_create_depth_stencil_view(
    ID3D12Device *device, const void *resource,
    const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
    D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_depth_stencil_view(
      device, resource, desc, descriptor);
}

uint64_t
record_create_sampler(ID3D12Device *device, const D3D12_SAMPLER_DESC *desc,
                      D3D12_CPU_DESCRIPTOR_HANDLE descriptor) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_create_sampler(device, desc, descriptor);
}

uint64_t
record_copy_descriptors(
    ID3D12Device *device, uint32_t dst_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_starts,
    const uint32_t *dst_descriptor_range_sizes,
    uint32_t src_descriptor_range_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_starts,
    const uint32_t *src_descriptor_range_sizes,
    uint32_t descriptor_heap_type, uint32_t descriptor_size) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_copy_descriptors(
      device, dst_descriptor_range_count, dst_descriptor_range_starts,
      dst_descriptor_range_sizes, src_descriptor_range_count,
      src_descriptor_range_starts, src_descriptor_range_sizes,
      descriptor_heap_type, descriptor_size);
}

uint64_t
record_copy_descriptors_simple(
    ID3D12Device *device, uint32_t descriptor_count,
    D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_start,
    D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_start,
    uint32_t descriptor_heap_type, uint32_t descriptor_size) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_copy_descriptors_simple(
      device, descriptor_count, dst_descriptor_range_start,
      src_descriptor_range_start, descriptor_heap_type, descriptor_size);
}

uint64_t
record_draw_instanced(const void *command_list,
                      uint32_t vertex_count_per_instance,
                      uint32_t instance_count, uint32_t start_vertex_location,
                      uint32_t start_instance_location) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_draw_instanced(
      command_list, vertex_count_per_instance, instance_count,
      start_vertex_location, start_instance_location);
}

uint64_t
record_draw_indexed_instanced(const void *command_list,
                              uint32_t index_count_per_instance,
                              uint32_t instance_count,
                              uint32_t start_index_location,
                              int32_t base_vertex_location,
                              uint32_t start_instance_location) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_draw_indexed_instanced(
      command_list, index_count_per_instance, instance_count,
      start_index_location, base_vertex_location, start_instance_location);
}

uint64_t
record_dispatch(const void *command_list, uint32_t x, uint32_t y, uint32_t z) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_dispatch(command_list, x, y, z);
}

uint64_t
record_close_command_list(const void *command_list, int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_close_command_list(
      command_list, result_code);
}

uint64_t
record_reset_command_list(const void *command_list,
                          const void *command_allocator,
                          const void *initial_pipeline_state,
                          int32_t result_code) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_reset_command_list(
      command_list, command_allocator, initial_pipeline_state, result_code);
}

uint64_t
record_execute_indirect(const void *command_list,
                        const void *command_signature,
                        uint32_t max_command_count, const void *arg_buffer,
                        uint64_t arg_buffer_offset, const void *count_buffer,
                        uint64_t count_buffer_offset) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_execute_indirect(
      command_list, command_signature, max_command_count, arg_buffer,
      arg_buffer_offset, count_buffer, count_buffer_offset);
}

uint64_t
record_execute_bundle(const void *command_list, const void *bundle_command_list) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_execute_bundle(
      command_list, bundle_command_list);
}

uint64_t
record_copy_buffer_region(const char *function_name, const void *command_list,
                          const void *dst_buffer,
                          uint64_t dst_offset, const void *src_buffer,
                          uint64_t src_offset, uint64_t byte_count) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_copy_buffer_region(
      function_name, command_list, dst_buffer, dst_offset, src_buffer,
      src_offset, byte_count);
}

uint64_t
record_copy_texture_region(const void *command_list,
                           const D3D12_TEXTURE_COPY_LOCATION *dst,
                           uint32_t dst_x, uint32_t dst_y, uint32_t dst_z,
                           const D3D12_TEXTURE_COPY_LOCATION *src,
                           const D3D12_BOX *src_box) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_copy_texture_region(
      command_list, dst, dst_x, dst_y, dst_z, src, src_box);
}

uint64_t
record_copy_resource(const void *command_list, const void *dst_resource,
                     const void *src_resource) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_copy_resource(
      command_list, dst_resource, src_resource);
}

uint64_t
record_resolve_subresource(const void *command_list, const void *dst_resource,
                           uint32_t dst_subresource,
                           const void *src_resource,
                           uint32_t src_subresource, uint32_t format) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_resolve_subresource(
      command_list, dst_resource, dst_subresource, src_resource,
      src_subresource, format);
}

uint64_t
record_ia_set_primitive_topology(const void *command_list,
                                 uint32_t primitive_topology) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_ia_set_primitive_topology(
      command_list, primitive_topology);
}

uint64_t
record_rs_set_viewports(const void *command_list, uint32_t viewport_count,
                        const D3D12_VIEWPORT *viewports) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_rs_set_viewports(
      command_list, viewport_count, viewports);
}

uint64_t
record_rs_set_scissor_rects(const void *command_list, uint32_t rect_count,
                            const void *rects) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_rs_set_scissor_rects(
      command_list, rect_count, rects);
}

uint64_t
record_set_pipeline_state(const void *command_list,
                          const void *pipeline_state) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_set_pipeline_state(
      command_list, pipeline_state);
}

uint64_t
record_clear_state(const void *command_list, const void *pipeline_state) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_clear_state(command_list, pipeline_state);
}

uint64_t
record_om_set_blend_factor(const void *command_list,
                           const float blend_factor[4]) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_om_set_blend_factor(
      command_list, blend_factor);
}

uint64_t
record_om_set_stencil_ref(const void *command_list, uint32_t stencil_ref) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_om_set_stencil_ref(
      command_list, stencil_ref);
}

uint64_t
record_resource_barrier(const void *command_list, uint32_t barrier_count,
                        const D3D12_RESOURCE_BARRIER *barriers) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_resource_barrier(
      command_list, barrier_count, barriers);
}

uint64_t
record_set_descriptor_heaps(const void *command_list, uint32_t heap_count,
                            const void *const *heaps) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_set_descriptor_heaps(
      command_list, heap_count, heaps);
}

uint64_t
record_set_root_signature(const void *command_list, bool compute,
                          const void *root_signature) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_set_root_signature(
      command_list, compute, root_signature);
}

uint64_t
record_set_root_descriptor_table(const void *command_list, bool compute,
                                 uint32_t root_parameter_index,
                                 D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_set_root_descriptor_table(
      command_list, compute, root_parameter_index, base_descriptor);
}

uint64_t
record_set_root_32bit_constants(const void *command_list, bool compute,
                                uint32_t root_parameter_index,
                                uint32_t constant_count,
                                const uint32_t *values,
                                uint32_t dst_offset) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_set_root_32bit_constants(
      command_list, compute, root_parameter_index, constant_count, values,
      dst_offset);
}

uint64_t
record_set_root_descriptor(const void *command_list, bool compute,
                           uint32_t parameter_type,
                           uint32_t root_parameter_index,
                           uint64_t gpu_virtual_address) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_set_root_descriptor(
      command_list, compute, parameter_type, root_parameter_index,
      gpu_virtual_address);
}

uint64_t
record_ia_set_index_buffer(const void *command_list,
                           const D3D12_INDEX_BUFFER_VIEW *view) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_ia_set_index_buffer(command_list, view);
}

uint64_t
record_ia_set_vertex_buffers(const void *command_list, uint32_t start_slot,
                             uint32_t view_count,
                             const D3D12_VERTEX_BUFFER_VIEW *views) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_ia_set_vertex_buffers(
      command_list, start_slot, view_count, views);
}

uint64_t
record_om_set_render_targets(
    const void *command_list, uint32_t render_target_descriptor_count,
    const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
    bool single_descriptor_handle,
    const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_om_set_render_targets(
      command_list, render_target_descriptor_count, render_target_descriptors,
      single_descriptor_handle, depth_stencil_descriptor);
}

uint64_t
record_clear_depth_stencil_view(const void *command_list,
                                D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                uint32_t flags, float depth, uint8_t stencil,
                                uint32_t rect_count, const void *rects) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_clear_depth_stencil_view(
      command_list, dsv, flags, depth, stencil, rect_count, rects);
}

uint64_t
record_clear_render_target_view(const void *command_list,
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                                const float color[4], uint32_t rect_count,
                                const void *rects) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_clear_render_target_view(
      command_list, rtv, color, rect_count, rects);
}

uint64_t
record_clear_unordered_access_view_uint(
    const void *command_list, D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor, const void *resource,
    const uint32_t values[4], uint32_t rect_count, const void *rects) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_clear_unordered_access_view_uint(
      command_list, gpu_descriptor, cpu_descriptor, resource, values,
      rect_count, rects);
}

uint64_t
record_clear_unordered_access_view_float(
    const void *command_list, D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor,
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor, const void *resource,
    const float values[4], uint32_t rect_count, const void *rects) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_clear_unordered_access_view_float(
      command_list, gpu_descriptor, cpu_descriptor, resource, values,
      rect_count, rects);
}

uint64_t
record_discard_resource(const void *command_list, const void *resource,
                        uint32_t first_subresource,
                        uint32_t subresource_count, uint32_t rect_count,
                        const void *rects) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_discard_resource(
      command_list, resource, first_subresource, subresource_count,
      rect_count, rects);
}

uint64_t
record_begin_query(const void *command_list, const void *query_heap,
                   uint32_t type, uint32_t index) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_begin_query(
      command_list, query_heap, type, index);
}

uint64_t
record_end_query(const void *command_list, const void *query_heap,
                 uint32_t type, uint32_t index) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_end_query(
      command_list, query_heap, type, index);
}

uint64_t
record_resolve_query_data(const void *command_list, const void *query_heap,
                          uint32_t type, uint32_t start_index,
                          uint32_t query_count, const void *dst_buffer,
                          uint64_t aligned_dst_buffer_offset) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_resolve_query_data(
      command_list, query_heap, type, start_index, query_count, dst_buffer,
      aligned_dst_buffer_offset);
}

uint64_t
record_set_predication(const void *command_list, const void *buffer,
                       uint64_t aligned_buffer_offset, uint32_t operation) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_set_predication(
      command_list, buffer, aligned_buffer_offset, operation);
}

uint64_t
record_resolve_subresource_region(
    const void *command_list, const void *dst_resource, uint32_t dst_subresource,
    uint32_t dst_x, uint32_t dst_y, const void *src_resource,
    uint32_t src_subresource, const void *src_rect, uint32_t format,
    uint32_t mode) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_resolve_subresource_region(
      command_list, dst_resource, dst_subresource, dst_x, dst_y,
      src_resource, src_subresource, src_rect, format, mode);
}

uint64_t
record_write_buffer_immediate(
    const void *command_list, uint32_t count,
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
    const void *modes) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_write_buffer_immediate(
      command_list, count, parameters, modes);
}

uint64_t
record_begin_render_pass(const void *command_list, uint32_t render_targets_count,
                         const RenderPassRenderTargetDesc *render_targets,
                         const RenderPassDepthStencilDesc *depth_stencil,
                         uint32_t flags) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_begin_render_pass(
      command_list, render_targets_count,
      reinterpret_cast<const ::apitrace::d3d12::RenderPassRenderTargetDesc *>(render_targets),
      reinterpret_cast<const ::apitrace::d3d12::RenderPassDepthStencilDesc *>(depth_stencil),
      flags);
}

uint64_t
record_end_render_pass(const void *command_list) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_end_render_pass(command_list);
}

uint64_t
record_temporal_upscale(
    const void *command_list, uint32_t input_content_width,
    uint32_t input_content_height, bool auto_exposure, bool in_reset,
    bool depth_reversed, bool motion_vector_in_display_res, const void *color,
    const void *depth, const void *motion_vector, const void *output,
    float motion_vector_scale_x, float motion_vector_scale_y,
    float pre_exposure, const void *exposure_texture, float jitter_offset_x,
    float jitter_offset_y) {
  if (!d3d_enabled())
    return 0;
  ensure_session_open();
  return ::apitrace::d3d12::record_temporal_upscale(
      command_list, input_content_width, input_content_height, auto_exposure,
      in_reset, depth_reversed, motion_vector_in_display_res, color, depth,
      motion_vector, output, motion_vector_scale_x, motion_vector_scale_y,
      pre_exposure, exposure_texture, jitter_offset_x, jitter_offset_y);
}

void
on_d3d12_create_device(void *device) {
  if (!d3d_enabled() || !device)
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_d3d12_create_device(device);
}

void
on_d3d11_create_device(void *device) {
  if (!d3d_enabled() || !device)
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_d3d11_create_device(device);
}

void
on_dxgi_create_swapchain(void *factory, void *device, void *swapchain) {
  if (!d3d_enabled() || !swapchain)
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_dxgi_create_swapchain(factory, device, swapchain);
}

void
on_d3d12_execute_command_lists(void *queue, void *command_list) {
  if (!d3d_enabled() || !queue || !command_list)
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_execute_command_lists(queue, command_list);
}

void
on_d3d12_resource_barrier(void *command_list, uint32_t count) {
  if (!d3d_enabled() || !command_list || !count)
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_call("ID3D12GraphicsCommandList::ResourceBarrier");
}

void
on_d3d12_present(void *swapchain, uint32_t sync_interval, uint32_t flags) {
  if (!d3d_enabled() || !swapchain)
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_present(swapchain, sync_interval, flags);
}

void
on_fence_dependency(
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
    uint32_t mask_count) {
  if (!d3d_enabled())
    return;
  ensure_session_open();
  ::apitrace::d3d12::record_fence_dependency(
      scope, d3d_sequence, encoder_id, implicit_pre_raster_wait, strong_masks,
      strong_count, full_masks, full_count, minimal_masks, minimal_count,
      mask_count);
}

} // namespace dxmt::apitrace

#endif // DXMT_APITRACE_D3D
