#include "d3d12_compiled_binding_tables.hpp"

#include "d3d12_command_list.hpp"
#include "dxmt_statistics.hpp"

namespace dxmt::d3d12 {

const CompiledCommandRootDescriptorTable *
FindCompiledRootTable(
    const std::vector<CompiledCommandRootDescriptorTable> &tables,
    UINT root_index, D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
  for (const auto &table : tables) {
    if (table.root_parameter_index == root_index &&
        table.heap_type == heap_type && table.resolved)
      return &table;
  }
  return nullptr;
}

void
RecordCompiledDescriptorBackendStats(
    dxmt::FrameStatistics *stats,
    const std::vector<CompiledCommandRootDescriptorTable> &tables) {
  if (!stats)
    return;
  for (const auto &table : tables) {
    if (!table.resolved)
      continue;
    stats->frame_native_descriptor_root_tables++;
    if (table.descriptor_table_backend_ready)
      stats->frame_native_descriptor_root_table_backend_ready++;
    if (table.native_descriptor_record_storage_ready)
      stats->frame_native_descriptor_record_storage_ready++;
    if (table.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
      stats->frame_native_descriptor_sampler_root_tables++;
    else
      stats->frame_native_descriptor_resource_root_tables++;
  }
}

uint64_t
ResolveCompiledDirtyMask(const void *cached_identity,
                         const void *base_identity, uint64_t compiled_mask) {
  return cached_identity == base_identity && compiled_mask ? compiled_mask
                                                           : UINT64_MAX;
}

uint32_t
ResolveCompiledDirtyMask(const void *cached_identity,
                         const void *base_identity, uint32_t compiled_mask) {
  return cached_identity == base_identity && compiled_mask ? compiled_mask
                                                           : UINT32_MAX;
}

void
RetainDirectBufferForGpu(ArgumentEncodingContext &enc, WMT::Buffer buffer) {
  if (!buffer)
    return;
  const auto sequence = enc.currentSeqId();
  enc.queue().RetainGpuOwner(sequence, WMT::Reference<WMT::Buffer>(buffer));
}

} // namespace dxmt::d3d12
