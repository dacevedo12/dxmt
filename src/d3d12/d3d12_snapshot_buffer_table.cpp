#include "d3d12_snapshot_buffer_table.hpp"

#include "d3d12_legacy_binding_encode.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

BindlessBufferTableSnapshotStorage BuildSnapshotBindlessBufferTableBindings(
    WMT::Device device, const GraphicsBindingSnapshot &snapshot,
    PipelineStage want_stage) {
  BindlessBufferTableSnapshotStorage bindings = {};
  ForEachCompiledDescriptorTableEntry(
      snapshot, [&](const auto &entry, const DescriptorRecord &descriptor) {
        if (static_cast<PipelineStage>(entry.stage) != want_stage)
          return;
        const auto range_type =
            static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(entry.range_type);
        if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
          const auto slot =
              kSnapshotConstantBufferSlotsPerStage * unsigned(want_stage) +
              entry.slot;
          if (slot < bindings.constant_buffers.size()) {
            bindings.constant_buffers[slot].captured = true;
            (void)MakeConstantBufferBindingFromDescriptor(
                descriptor, bindings.constant_buffers[slot].binding);
          }
        } else if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
          const auto slot = kSRVBindings * unsigned(want_stage) + entry.slot;
          if (slot < bindings.resources.size()) {
            auto &binding = bindings.resources[slot];
            binding.srv_captured = true;
            (void)MakeShaderResourceBindingFromDescriptor(
                device, descriptor, &entry.argument, binding.srv);
          }
        } else if (range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV &&
                   entry.slot < kUAVBindings) {
          auto &binding = bindings.resources[entry.slot];
          binding.uav_captured = true;
          (void)MakeUnorderedAccessBindingFromDescriptor(
              device, descriptor, &entry.argument, binding.uav);
        }
      });
  for (const auto &entry : snapshot.entries) {
    if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor ||
        !entry.has_descriptor || entry.stage != want_stage)
      continue;
    const auto &descriptor = SnapshotDescriptor(snapshot, entry);
    if (entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
      const auto slot =
          kSnapshotConstantBufferSlotsPerStage * unsigned(want_stage) +
          entry.slot;
      if (slot < bindings.constant_buffers.size()) {
        bindings.constant_buffers[slot].captured = true;
        (void)MakeConstantBufferBindingFromDescriptor(
            descriptor, bindings.constant_buffers[slot].binding);
      }
    } else if (entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
      const auto slot = kSRVBindings * unsigned(want_stage) + entry.slot;
      if (slot < bindings.resources.size()) {
        auto &binding = bindings.resources[slot];
        binding.srv_captured = true;
        (void)MakeShaderResourceBindingFromDescriptor(
            device, descriptor, entry.argument, binding.srv);
      }
    } else if (entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
      if (entry.slot < kUAVBindings) {
        auto &binding = bindings.resources[entry.slot];
        binding.uav_captured = true;
        (void)MakeUnorderedAccessBindingFromDescriptor(
            device, descriptor, entry.argument, binding.uav);
      }
    }
  }
  return bindings;
}

} // namespace dxmt::d3d12
