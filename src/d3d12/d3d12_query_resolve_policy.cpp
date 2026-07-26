#include "d3d12_query_resolve_policy.hpp"

#include "d3d12_indirect_topology.hpp"
#include "d3d12_query_resolve_range.hpp"
#include "d3d12_queue_replay_helpers.hpp"

namespace dxmt::d3d12 {

bool
ResolveRecordTouchesRange(const ResolveQueryDataRecord &record,
                          ID3D12Resource *resource, UINT64 offset,
                          UINT64 size) {
  if (!resource)
    return true;

  auto *dst = GetResource(record.dst_buffer.ptr());
  if (!dst)
    return false;

  const UINT64 byte_count =
      UINT64(record.query_count) * QueryResultStride(record.type);
  if (!byte_count)
    return false;

  return record.dst_buffer.ptr() == resource &&
         BufferRangesOverlap(record.dst_buffer_offset, byte_count, offset,
                             size);
}

bool
CanGpuResolveTimestampSample(const PendingTimestampResolve::Sample &sample,
                             uint64_t sequence, uint64_t current_sequence) {
  if (sample.index == ~0ull)
    return false;
#if DXMT_DX12_METAL4
  return sample.heap && sample.heap_entry_size;
#else
  return sequence == current_sequence;
#endif
}

} // namespace dxmt::d3d12
