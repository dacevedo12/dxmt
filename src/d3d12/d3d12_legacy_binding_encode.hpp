#pragma once

// Legacy (non-bindless) descriptor -> argument-encoder binding path.
//
// These helpers used to live inside CommandQueueImpl
// (d3d12_command_queue_bindless_mirror.inc and
// d3d12_command_queue_binding_plans.inc). Every one of them turns a captured
// DescriptorRecord (or a static-sampler declaration) into an
// ArgumentEncodingContext binding using only the Metal device handle handed to
// it, so none of them touch the command queue instance and the whole set can be
// compiled and analysed on its own.

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_context.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Legacy shader ABI limits for the interpreted binding path. A descriptor
// targeting a slot at or beyond these is reported and dropped.
inline constexpr UINT kLegacyConstantBufferSlotCount = 14;
inline constexpr UINT kLegacySamplerSlotCount = 16;

// Byte offset of a buffer resource inside its own allocation, resolved through
// the GPU virtual address map. False when `d3d_resource` is not a dxmt buffer
// or the address map does not resolve back to it.
[[nodiscard]] bool ResolveDescriptorBufferOffset(ID3D12Resource *d3d_resource,
                                                 UINT64 &offset);

// --- descriptor -> binding snapshots ---------------------------------------

// CreateConstantBufferView(nullptr, ...) is an explicitly populated null
// descriptor and is preserved as a valid empty snapshot, so a true result with
// an empty `binding` is meaningful.
[[nodiscard]] bool
MakeConstantBufferBindingFromDescriptor(const DescriptorRecord &descriptor,
                                        ConstantBufferBinding &binding);

[[nodiscard]] bool MakeShaderResourceBindingFromDescriptor(
    WMT::Device device, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT *argument, ResourceViewBinding &binding);

[[nodiscard]] bool MakeUnorderedAccessBindingFromDescriptor(
    WMT::Device device, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT *argument,
    UnorderedAccessViewBinding &binding);

// --- encoder binds ---------------------------------------------------------

void ClearShaderResourceBinding(ArgumentEncodingContext &enc,
                                PipelineStage stage, UINT slot);

void ClearUnorderedAccessBinding(ArgumentEncodingContext &enc,
                                 PipelineStage stage, UINT slot);

void BindConstantBufferDescriptor(ArgumentEncodingContext &enc,
                                  PipelineStage stage, UINT slot,
                                  const DescriptorRecord &descriptor);

void BindShaderResourceDescriptor(
    WMT::Device device, ArgumentEncodingContext &enc, PipelineStage stage,
    UINT slot, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT *argument);

void BindUnorderedAccessDescriptor(
    WMT::Device device, ArgumentEncodingContext &enc, PipelineStage stage,
    UINT slot, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT *argument);

void BindSamplerDescriptor(WMT::Device device, ArgumentEncodingContext &enc,
                           PipelineStage stage, UINT slot,
                           const DescriptorRecord &descriptor);

// Dispatches to the per-range-type bind above.
void BindDescriptor(WMT::Device device, ArgumentEncodingContext &enc,
                    PipelineStage stage,
                    D3D12_DESCRIPTOR_RANGE_TYPE range_type, UINT slot,
                    const DescriptorRecord &descriptor,
                    const DXMT12_MTL4_SHADER_ARGUMENT *argument);

// Clears the SRV/UAV binding at `slot`; CBV and sampler ranges are left alone.
void ClearDescriptorBinding(ArgumentEncodingContext &enc, PipelineStage stage,
                            D3D12_DESCRIPTOR_RANGE_TYPE range_type, UINT slot);

// Binds every static sampler declared by `root` into the slots the pipeline
// reflection assigns them.
void ApplyStaticSamplers(WMT::Device device, ArgumentEncodingContext &enc,
                         const PipelineState &pipeline,
                         const RootSignature &root, bool compute);

} // namespace dxmt::d3d12
