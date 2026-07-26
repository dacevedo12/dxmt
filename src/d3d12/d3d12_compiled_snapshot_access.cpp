#include "d3d12_compiled_snapshot_access.hpp"

#include "d3d12_legacy_binding_encode.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <utility>

namespace dxmt::d3d12 {

void AddCompiledDescriptorEncoderAccess(
    WMT::Device device, CompiledDirectAccessList &list, PipelineStage stage,
    D3D12_DESCRIPTOR_RANGE_TYPE range_type, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT *argument) {
  using Access = CompiledDirectAccessList::EncoderAccess;
  using Kind = CompiledDirectAccessList::Kind;
  switch (range_type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: {
    ConstantBufferBinding binding = {};
    if (!MakeConstantBufferBindingFromDescriptor(descriptor, binding) ||
        !binding.buffer || !descriptor.has_desc ||
        !descriptor.desc.cbv.SizeInBytes)
      return;
    AddCompiledDirectEncoderAccess(
        list, Access{stage, Kind::BufferRange, std::move(binding.buffer), {},
                     binding.offset, descriptor.desc.cbv.SizeInBytes, 0,
                     ResourceAccess::Read});
    return;
  }
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: {
    ResourceViewBinding binding = {};
    if (!MakeShaderResourceBindingFromDescriptor(device, descriptor, argument,
                                                 binding))
      return;
    if (binding.buffer) {
      AddCompiledDirectEncoderAccess(
          list, Access{stage,
                       binding.viewId ? Kind::BufferView : Kind::BufferRange,
                       std::move(binding.buffer), {},
                       binding.slice.byteOffset, binding.slice.byteLength,
                       binding.viewId, ResourceAccess::Read});
    } else if (binding.texture && binding.viewId) {
      AddCompiledDirectEncoderAccess(
          list, Access{stage, Kind::TextureView, {},
                       std::move(binding.texture), 0, 0, binding.viewId,
                       ResourceAccess::Read});
    }
    return;
  }
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: {
    UnorderedAccessViewBinding binding = {};
    if (!MakeUnorderedAccessBindingFromDescriptor(device, descriptor, argument,
                                                  binding))
      return;
    bool read = argument && ((argument->Flags >> 10) & 1);
    bool write = argument && ((argument->Flags >> 10) & 2);
    if (!read && !write)
      read = write = true;
    const int flags = (read ? ResourceAccess::Read : 0) |
                      (write ? ResourceAccess::Write : 0) | ResourceAccess::UAV;
    if (binding.buffer) {
      AddCompiledDirectEncoderAccess(
          list, Access{stage,
                       binding.viewId ? Kind::BufferView : Kind::BufferRange,
                       std::move(binding.buffer), {},
                       binding.slice.byteOffset, binding.slice.byteLength,
                       binding.viewId, flags});
    } else if (binding.texture && binding.viewId) {
      AddCompiledDirectEncoderAccess(
          list, Access{stage, Kind::TextureView, {},
                       std::move(binding.texture), 0, 0, binding.viewId,
                       flags});
    }
    if (binding.counter) {
      AddCompiledDirectEncoderAccess(
          list, Access{stage, Kind::BufferRange, std::move(binding.counter),
                       {}, 0, sizeof(uint32_t), 0, ResourceAccess::All});
    }
    return;
  }
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    return;
  }
}

void AddCompiledSnapshotEncoderAccesses(
    WMT::Device device, CompiledDirectAccessList &list,
    const GraphicsBindingSnapshot *snapshot) {
  if (!snapshot)
    return;
  for (const auto &access : snapshot->native_descriptor_accesses) {
    if (!snapshot->descriptor_records ||
        access.descriptor_index >= snapshot->descriptor_records->records.size())
      continue;
    AddCompiledDescriptorEncoderAccess(
        device, list, access.stage, access.range_type,
        snapshot->descriptor_records->records[access.descriptor_index],
        &access.argument);
  }
  if (snapshot->native_descriptor_recipe) {
    const auto &entries = snapshot->native_descriptor_recipe->entries;
    assert(entries.size() == snapshot->native_descriptor_indices.size());
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto descriptor_index = snapshot->native_descriptor_indices[i];
      if (descriptor_index == UINT32_MAX || !snapshot->descriptor_records ||
          descriptor_index >= snapshot->descriptor_records->records.size())
        continue;
      const auto &entry = entries[i];
      AddCompiledDescriptorEncoderAccess(
          device, list, static_cast<PipelineStage>(entry.stage),
          static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(entry.range_type),
          snapshot->descriptor_records->records[descriptor_index],
          &entry.argument);
    }
  }
  for (const auto &entry : snapshot->entries) {
    if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor ||
        !entry.has_descriptor || !entry.debug_kind ||
        std::strncmp(entry.debug_kind, "root-", 5) != 0)
      continue;
    AddCompiledDescriptorEncoderAccess(device, list, entry.stage,
                                       entry.range_type,
                                       SnapshotDescriptor(*snapshot, entry),
                                       entry.argument);
  }
}

} // namespace dxmt::d3d12
