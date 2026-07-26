#pragma once

// Resource-access publication for compiled-direct encoding: turning captured
// descriptors (and a whole GraphicsBindingSnapshot) into the ordered encoder
// access list consumed by PublishCompiledDirectAccessListForEncode.
//
// These helpers used to live inside CommandQueueImpl
// (d3d12_command_queue_compiled_encode.inc). The only piece of the command
// queue they ever used was the Metal device handle, which is now passed in, so
// they can be compiled and analysed on their own.

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_compiled_direct_access.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_replay_binding_types.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Appends the encoder accesses one captured descriptor implies for `stage`.
// Sampler ranges carry no resource access and are ignored.
void AddCompiledDescriptorEncoderAccess(
    WMT::Device device, CompiledDirectAccessList &list, PipelineStage stage,
    D3D12_DESCRIPTOR_RANGE_TYPE range_type, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT *argument);

// Appends the encoder accesses for every descriptor a snapshot carries: the
// tracked native descriptor accesses, the frozen native descriptor recipe and
// the root descriptor entries. A null snapshot is ignored.
void AddCompiledSnapshotEncoderAccesses(
    WMT::Device device, CompiledDirectAccessList &list,
    const GraphicsBindingSnapshot *snapshot);

} // namespace dxmt::d3d12
