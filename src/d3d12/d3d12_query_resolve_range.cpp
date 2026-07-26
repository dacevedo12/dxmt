#include "d3d12_query_resolve_range.hpp"

namespace dxmt::d3d12 {

bool
BufferRangesOverlap(UINT64 a_offset, UINT64 a_size, UINT64 b_offset,
                    UINT64 b_size) {
  if (!a_size || !b_size)
    return false;
  if (a_offset <= b_offset)
    return b_offset - a_offset < a_size;
  return a_offset - b_offset < b_size;
}

ResolveQueryDataRecord
SliceResolveRecord(const ResolveQueryDataRecord &record, UINT start_index,
                   UINT query_count, size_t result_stride) {
  auto slice = record;
  slice.start_index = start_index;
  slice.query_count = query_count;
  slice.dst_buffer_offset +=
      UINT64(start_index - record.start_index) * result_stride;
  return slice;
}

QueryResolveSnapshot
SliceResolveSnapshot(const QueryResolveSnapshot &snapshot, UINT offset,
                     UINT query_count) {
  QueryResolveSnapshot slice;
  slice.type = snapshot.type;
  if (offset > snapshot.entries.size() ||
      query_count > snapshot.entries.size() - offset)
    return slice;
  slice.entries.insert(slice.entries.end(), snapshot.entries.begin() + offset,
                       snapshot.entries.begin() + offset + query_count);
  return slice;
}

} // namespace dxmt::d3d12
