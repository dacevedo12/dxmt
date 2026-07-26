#pragma once

// Deferred bindless-mirror slot fill and the sampler verify probes.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_bindless_mirror.inc). Each one publishes into the heap's
// own DescriptorHeapMirror through the encoder handed to it and materializes
// views from the Metal device handle handed to it, so none of them touch the
// command queue instance and the whole set can be compiled and analysed on its
// own.

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_bindless_mirror_fill.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "dxmt_context.hpp"
#include "dxmt_sampler.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Compares one mirrored sampler slot against the payload the descriptor would
// encode and reports mismatches. No-op unless the verify path is enabled.
void VerifyBindlessMirrorSamplerSlot(DescriptorHeapMirror *mirror, UINT slot,
                                     const Sampler *sampler,
                                     uint64_t dummy_handle, PipelineStage stage,
                                     const DXMT12_MTL4_SHADER_ARGUMENT *arg);

// Verify hook for a mirrored texture slot. Currently a no-op; the texture
// payload is checked by the bindless-window probes instead.
void VerifyBindlessMirrorTextureSlot(ArgumentEncodingContext &enc,
                                     DescriptorHeapMirror *mirror, UINT slot,
                                     const Rc<Texture> &texture,
                                     TextureViewKey view);

// Materializes `record`'s sampler (when it has one) and verifies it against the
// mirror slot it should occupy.
void VerifyBindlessMirrorSamplerDescriptor(
    WMT::Device device, ArgumentEncodingContext &enc,
    const DescriptorRecord &record, PipelineStage stage,
    const DXMT12_MTL4_SHADER_ARGUMENT *arg);

// Publishes `record` into its mirror slot when the slot still carries the
// pending write this record snapshotted. Returns what kind of fill happened.
BindlessMirrorFillKind
FillBindlessMirrorSlot(WMT::Device device, ArgumentEncodingContext &enc,
                       DescriptorHeapMirror *mirror,
                       const DescriptorRecord &record, PipelineStage stage,
                       const DXMT12_MTL4_SHADER_ARGUMENT *arg);

// FillBindlessMirrorSlot for the range types that own a mirror (SRV/UAV and
// sampler); other range types are ignored.
BindlessMirrorFillKind MaybeFillBindlessMirrorSlot(
    WMT::Device device, ArgumentEncodingContext &enc,
    D3D12_DESCRIPTOR_RANGE_TYPE range_type, const DescriptorRecord &record,
    PipelineStage stage, const DXMT12_MTL4_SHADER_ARGUMENT *arg);

} // namespace dxmt::d3d12
