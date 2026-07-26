#pragma once

#include "d3d12_command_list.hpp"
#include "d3d12_query.hpp"

#include <cstddef>
#include <d3d12.h>

namespace dxmt::d3d12 {

// True when two byte ranges of the same buffer intersect. Empty ranges never
// overlap. Written so the subtraction can never underflow.
[[nodiscard]] bool BufferRangesOverlap(UINT64 a_offset, UINT64 a_size,
                                       UINT64 b_offset, UINT64 b_size);

// Narrows a ResolveQueryData record to the [start_index, start_index +
// query_count) sub-range, advancing the destination offset by the queries that
// were dropped from the front. `result_stride` is the per-query result size of
// the record's query type.
[[nodiscard]] ResolveQueryDataRecord
SliceResolveRecord(const ResolveQueryDataRecord &record, UINT start_index,
                   UINT query_count, size_t result_stride);

// Narrows a query resolve snapshot to `query_count` entries starting at
// `offset`. Returns an empty snapshot (type preserved) when the requested
// window does not fit; the bounds check cannot overflow.
[[nodiscard]] QueryResolveSnapshot
SliceResolveSnapshot(const QueryResolveSnapshot &snapshot, UINT offset,
                     UINT query_count);

} // namespace dxmt::d3d12
