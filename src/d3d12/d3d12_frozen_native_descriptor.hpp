#pragma once

// Copying one submitted descriptor into the immutable native descriptor
// backend of an Execute submission.
//
// These helpers used to live inside CommandQueueImpl
// (d3d12_command_queue_native_binding.inc). They only touch the store and the
// descriptor they are handed, so they can be compiled and analysed on their
// own.

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_replay_binding_types.hpp"

#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Copies the descriptor-table entry (and, for CBV/SRV/UAV heaps, the buffer
// descriptor record plus its backend resources) of `descriptor` into slot
// `destination` of `store`. Out-of-range destinations and records without a
// heap mirror are ignored.
void CopyFrozenNativeDescriptorSlot(
    SubmittedFrozenNativeDescriptorStore &store, uint32_t destination,
    const DescriptorRecord &descriptor);

// Materializes a root CBV/SRV/UAV `address` directly into slot `destination`
// of `store`, bypassing any descriptor heap. Returns false when the address
// cannot be resolved to a live buffer allocation, when the slot is out of
// range, or for sampler ranges.
bool CopyFrozenNativeRootDescriptorSlot(
    SubmittedFrozenNativeDescriptorStore &store, uint32_t destination,
    D3D12_GPU_VIRTUAL_ADDRESS address,
    D3D12_DESCRIPTOR_RANGE_TYPE range_type);

} // namespace dxmt::d3d12
