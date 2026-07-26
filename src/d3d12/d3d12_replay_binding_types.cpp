#include "d3d12_replay_binding_types.hpp"

#include <cstddef>
#include <cstring>

namespace dxmt::d3d12 {

bool FinalizeFrozenNativeDescriptorStore(
    SubmittedFrozenNativeDescriptorStore &store, WMT::Device device,
    dxmt::CommandQueue &queue) {
  if (store.finalized)
    return store.ready;
  store.finalized = true;
  if (store.root_words.empty()) {
    // A native shader with no reflected root arguments has a valid empty
    // binding program. It needs no backing allocation, but it must still be
    // accepted by the compiled-direct path instead of falling back to the
    // removed live descriptor implementation.
    if (store.descriptor_table.empty() && store.buffer_records.empty() &&
        store.pending_root_constant_descriptors.empty()) {
      store.ready = true;
      return true;
    }
    return false;
  }

  constexpr uint64_t kSectionAlignment = 256;
  auto align_section = [](uint64_t value) {
    return (value + kSectionAlignment - 1) & ~(kSectionAlignment - 1);
  };
  const uint64_t root_size =
      store.root_words.size() * sizeof(store.root_words[0]);
  const uint64_t descriptor_table_size =
      store.descriptor_table.size() * sizeof(store.descriptor_table[0]);
  const uint64_t buffer_record_size =
      store.buffer_records.size() * sizeof(store.buffer_records[0]);
  const uint64_t buffer_resource_table_size =
      store.buffer_resources.size() * sizeof(store.buffer_resources[0]);
  store.descriptor_table_buffer_offset = align_section(root_size);
  store.buffer_record_buffer_offset = align_section(
      store.descriptor_table_buffer_offset + descriptor_table_size);
  store.buffer_resource_table_buffer_offset = align_section(
      store.buffer_record_buffer_offset + buffer_record_size);
  const uint64_t total_size = align_section(
      store.buffer_resource_table_buffer_offset +
      buffer_resource_table_size);

  WMTBufferInfo info = {};
  info.length = total_size;
  info.options = static_cast<WMTResourceOptions>(
      WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
  info.memory.set(nullptr);
  store.root_base_buffer = device.newBuffer(info);
  store.root_base_gpu_address = info.gpu_address;
  auto *mapped = static_cast<std::byte *>(
      info.memory.get_accessible_or_null());
  if (!store.root_base_buffer || !mapped) {
    store.root_base_buffer = nullptr;
    return false;
  }
  store.root_base_residency =
      queue.RegisterLifetimeResidency(
          dxmt::ResidencyOwnership::Lifetime(store.root_base_buffer));
  if (!store.root_base_residency) {
    store.root_base_buffer = nullptr;
    return false;
  }
  for (const auto &pending : store.pending_root_constant_descriptors) {
    if (pending.descriptor_slot >= store.buffer_records.size() ||
        pending.descriptor_slot >= store.descriptor_table.size())
      continue;
    const auto resource_index =
        1u + pending.descriptor_slot * 2u;
    if (resource_index >= store.buffer_resources.size())
      continue;
    store.buffer_resources[resource_index] = BufferResourceTableEntry{
        store.root_base_gpu_address, total_size,
        store.root_base_buffer.handle, 1};
    store.buffer_records[pending.descriptor_slot] =
        BufferDescriptorRecord{
            resource_index,
            BufferDescriptorRecordFlagValid |
                BufferDescriptorRecordFlagCBV,
            pending.byte_offset,
            pending.byte_length,
            0,
            0,
            kNullDescriptorResourceIndex,
            0};
    store.descriptor_table[pending.descriptor_slot].gpu_va =
        store.root_base_gpu_address + pending.byte_offset;
  }
  std::memcpy(mapped, store.root_words.data(), root_size);
  if (descriptor_table_size)
    std::memcpy(mapped + store.descriptor_table_buffer_offset,
                store.descriptor_table.data(), descriptor_table_size);
  if (buffer_record_size)
    std::memcpy(mapped + store.buffer_record_buffer_offset,
                store.buffer_records.data(), buffer_record_size);
  if (buffer_resource_table_size)
    std::memcpy(mapped + store.buffer_resource_table_buffer_offset,
                store.buffer_resources.data(), buffer_resource_table_size);
  store.descriptor_table_buffer = store.root_base_buffer;
  store.buffer_record_buffer = store.root_base_buffer;
  store.buffer_resource_table_buffer = store.root_base_buffer;
  store.ready = true;
  return store.ready;
}

} // namespace dxmt::d3d12
