#include "d3d12_frozen_native_descriptor.hpp"

#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_buffer.hpp"

namespace dxmt::d3d12 {

void
CopyFrozenNativeDescriptorSlot(SubmittedFrozenNativeDescriptorStore &store,
                               uint32_t destination,
                               const DescriptorRecord &descriptor) {
  if (!descriptor.mirror || destination >= store.descriptor_table.size())
    return;
  const auto source = descriptor.heap_index;
  if (const auto entry = descriptor.mirror->descriptorTableEntry(source))
    store.descriptor_table[destination] = *entry;
  if (descriptor.mirror->isSamplerHeap())
    return;
  const auto source_record =
      descriptor.mirror->bufferDescriptorRecord(source);
  if (!source_record)
    return;
  auto record = *source_record;
  auto copy_resource = [&](uint32_t source_index, bool counter) {
    if (source_index == kNullDescriptorResourceIndex)
      return uint32_t(kNullDescriptorResourceIndex);
    const auto resource =
        descriptor.mirror->backendResourceRecord(source_index);
    if (!resource)
      return uint32_t(kNullDescriptorResourceIndex);
    const auto destination_index =
        1u + destination * 2u + uint32_t(counter);
    if (destination_index >= store.buffer_resources.size())
      return uint32_t(kNullDescriptorResourceIndex);
    store.buffer_resources[destination_index] = BufferResourceTableEntry{
        resource->gpu_address, resource->byte_size,
        resource->allocation_handle, resource->generation};
    if (resource->allocation &&
        store.retained_resource_handles.insert(
            resource->allocation.handle).second)
      store.retained_resources.push_back(resource->allocation);
    return destination_index;
  };
  record.resource_index = copy_resource(record.resource_index, false);
  record.counter_resource_index =
      copy_resource(record.counter_resource_index, true);
  store.buffer_records[destination] = record;
}

bool
CopyFrozenNativeRootDescriptorSlot(SubmittedFrozenNativeDescriptorStore &store,
                                   uint32_t destination,
                                   D3D12_GPU_VIRTUAL_ADDRESS address,
                                   D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
  if (!address || destination >= store.descriptor_table.size())
    return false;
  Resource *resource = nullptr;
  const auto resource_offset =
      ResolveBufferGpuAddress(address, resource);
  auto *allocation =
      resource ? resource->GetBufferAllocation() : nullptr;
  if (!resource || !allocation || !allocation->buffer())
    return false;
  const auto byte_offset =
      resource->GetHeapOffset() + resource_offset;
  const auto remaining =
      resource_offset < resource->GetResourceDesc().Width
          ? resource->GetResourceDesc().Width - resource_offset
          : 0;
  const auto resource_index = 1u + destination * 2u;
  if (!remaining || resource_index >= store.buffer_resources.size())
    return false;
  uint32_t flags = BufferDescriptorRecordFlagValid;
  switch (range_type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
    flags |= BufferDescriptorRecordFlagCBV;
    break;
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    flags |= BufferDescriptorRecordFlagSRV |
             BufferDescriptorRecordFlagRaw;
    break;
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
    flags |= BufferDescriptorRecordFlagUAV |
             BufferDescriptorRecordFlagRaw;
    break;
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    return false;
  }
  store.buffer_resources[resource_index] = BufferResourceTableEntry{
      allocation->gpuAddress() + allocation->currentSuballocationOffset(),
      resource->GetResourceDesc().Width,
      allocation->buffer().handle, 1};
  if (store.retained_resource_handles.insert(
          allocation->buffer().handle).second)
    store.retained_resources.emplace_back(allocation->buffer().handle);
  store.buffer_records[destination] = BufferDescriptorRecord{
      resource_index, flags, byte_offset, remaining, 1, 0,
      kNullDescriptorResourceIndex, 0};
  store.descriptor_table[destination].gpu_va = address;
  return true;
}

} // namespace dxmt::d3d12
