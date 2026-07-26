#pragma once

// Descriptor lookups that resolve against the heaps and root slots currently
// recorded in a ReplayState.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_descriptor_binding.inc). They only read the replay state
// handed to them and delegate to the instance-free heap helpers in
// d3d12_descriptor_table_access.hpp, so they never touch the command queue
// instance and can be compiled and analysed on their own.

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_replay_queue_state_types.hpp"

#include <optional>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Resolves `handle` against the heap of `heap_type` currently bound in `state`.
[[nodiscard]] std::optional<DescriptorRecord>
GetBoundDescriptorRecord(const ReplayState &state,
                         D3D12_GPU_DESCRIPTOR_HANDLE handle,
                         D3D12_DESCRIPTOR_HEAP_TYPE heap_type);

// Same, for the descriptor at `range_offset + descriptor_index` inside the
// table starting at `base`.
[[nodiscard]] std::optional<DescriptorRecord>
GetBoundDescriptorRecordInRange(const ReplayState &state,
                                D3D12_GPU_DESCRIPTOR_HANDLE base,
                                UINT range_offset, UINT descriptor_index,
                                UINT descriptor_count,
                                D3D12_DESCRIPTOR_HEAP_TYPE heap_type);

// Root CBV/SRV/UAV GPU address for the descAccess content fingerprint (reads
// the same maps RecordRootBufferDescriptorAccess uses; 0 = unbound).
[[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS
GetRootBufferAddressForFingerprint(const ReplayState &state, bool compute,
                                   UINT root_index, DescriptorRecordType type);

} // namespace dxmt::d3d12
