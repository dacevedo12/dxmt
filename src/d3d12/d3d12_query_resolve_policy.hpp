#pragma once

// Selection policy for pending query resolves: which deferred ResolveQueryData
// records a buffer access must materialize, and which timestamp samples the GPU
// can resolve instead of the CPU fallback.
//
// These predicates used to be private members of the anonymous-namespace class
// CommandQueueImpl (d3d12_command_queue_query_resolve.inc). They only read the
// promoted query-resolve value types, so hoisting them to dxmt::d3d12 lets them
// be compiled and analyzed independently.

#include "d3d12_command_list.hpp"
#include "d3d12_replay_state_types.hpp"

#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

// True when a pending CPU query resolve writes into [offset, offset + size) of
// `resource`. A null resource matches every record.
[[nodiscard]] bool
ResolveRecordTouchesRange(const ResolveQueryDataRecord &record,
                          ID3D12Resource *resource, UINT64 offset,
                          UINT64 size);

// True when a pending timestamp sample can be resolved on the GPU. Without a
// Metal 4 counter heap that requires the sample to still belong to the current
// submission sequence.
[[nodiscard]] bool
CanGpuResolveTimestampSample(const PendingTimestampResolve::Sample &sample,
                             uint64_t sequence, uint64_t current_sequence);

} // namespace dxmt::d3d12
