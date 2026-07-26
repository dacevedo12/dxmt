#include "d3d12_bound_descriptor_lookup.hpp"

#include "d3d12_descriptor_table_access.hpp"

namespace dxmt::d3d12 {

std::optional<DescriptorRecord>
GetBoundDescriptorRecord(const ReplayState &state,
                         D3D12_GPU_DESCRIPTOR_HANDLE handle,
                         D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
  return GetBoundDescriptorRecordFromHeap(
      GetBoundDescriptorHeap(state, heap_type), handle, heap_type);
}

std::optional<DescriptorRecord>
GetBoundDescriptorRecordInRange(const ReplayState &state,
                                D3D12_GPU_DESCRIPTOR_HANDLE base,
                                UINT range_offset, UINT descriptor_index,
                                UINT descriptor_count,
                                D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
  return GetBoundDescriptorRecordInRangeFromHeap(
      GetBoundDescriptorHeap(state, heap_type), base, range_offset,
      descriptor_index, descriptor_count, heap_type);
}

D3D12_GPU_VIRTUAL_ADDRESS
GetRootBufferAddressForFingerprint(const ReplayState &state, bool compute,
                                   UINT root_index, DescriptorRecordType type) {
  const auto &map =
      type == DescriptorRecordType::ConstantBufferView
          ? (compute ? state.compute_cbv_roots : state.graphics_cbv_roots)
          : type == DescriptorRecordType::ShaderResourceView
                ? (compute ? state.compute_srv_roots : state.graphics_srv_roots)
                : (compute ? state.compute_uav_roots : state.graphics_uav_roots);
  if (root_index >= ReplayState::kMaxRootParameters)
    return 0;
  const auto &slot = map[root_index];
  return slot.valid ? slot.address : 0;
}

} // namespace dxmt::d3d12
