#pragma once

// Freezes the part of a descriptor that a bindless draw still needs at Metal
// encode time, long after the D3D12 heap slot may have been rewritten.
//
// This was CaptureCompiledBindlessPayload() in
// d3d12_command_queue_pass_queue.inc. The only queue state it reached for was
// device_->GetMTLDevice(), needed to materialize a sampler that the heap
// mirror had not created yet, so it takes the Metal device as a parameter and
// otherwise depends on nothing but the descriptor and the shader argument.

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_replay_binding_types.hpp"

namespace dxmt::d3d12 {

/** Captures the bindless payload for one reflected shader argument. Sampler
 *  arguments keep an owning reference to the materialized sampler (creating it
 *  on the spot when the mirror has none); texture arguments are only tagged
 *  for the dynamic patch performed at encode time. Every other argument kind
 *  yields an empty payload. */
[[nodiscard]] FrozenBindlessDescriptorPayload CaptureCompiledBindlessPayload(
    WMT::Device device, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT &argument);

} // namespace dxmt::d3d12
