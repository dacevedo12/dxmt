#include "d3d12_descriptor_access_fingerprint.hpp"

#include "d3d12_bound_descriptor_lookup.hpp"
#include "d3d12_descriptor_table_access.hpp"

#include <optional>

namespace dxmt::d3d12 {

uint64_t
HashDescriptorAccessBindingFingerprint(
    const ReplayState &state, bool compute, const RootSignature *root,
    const PipelineState *pipeline, const BindingPlan &plan,
    DescriptorHeap *cbv_heap, DescriptorHeap *sampler_heap,
    std::unordered_map<uint32_t, std::vector<uint32_t>> &entries_by_slot) {
  auto mix = [](uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
  };
  uint64_t fp = 1469598103934665603ull;
  fp = mix(fp, reinterpret_cast<uintptr_t>(root));
  fp = mix(fp, reinterpret_cast<uintptr_t>(pipeline));
  for (uint32_t entry_index = 0; entry_index < plan.entries.size();
       ++entry_index) {
    const auto &e = plan.entries[entry_index];
    if (e.kind == BindingEntryKind::Table) {
      const auto base = GetTableHandle(state, compute, e.root_index);
      std::optional<DescriptorRecord> descriptor =
          base.ptr ? GetBoundDescriptorRecordInRangeFromHeap(
                         e.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                             ? sampler_heap
                             : cbv_heap,
                         base, e.range_offset, e.descriptor_index, e.count,
                         e.heap_type)
                   : std::nullopt;
      fp = mix(fp, descriptor ? reinterpret_cast<uintptr_t>(
                                    descriptor->resource.ptr())
                              : 0);
      fp = mix(fp, (uint64_t(e.range_type) << 1) ^
                       (descriptor
                            ? (uint64_t(descriptor->type) << 20) ^
                                  reinterpret_cast<uintptr_t>(
                                      descriptor->counter_resource.ptr())
                            : 0));
      if (e.heap_type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER &&
          descriptor)
        entries_by_slot[descriptor->heap_index].push_back(entry_index);
    } else {
      fp = mix(fp, GetRootBufferAddressForFingerprint(
                       state, compute, e.root_index, e.buffer_type));
      fp = mix(fp, uint64_t(e.buffer_type) << 3);
    }
  }
  return fp;
}

} // namespace dxmt::d3d12
