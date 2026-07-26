#pragma once

// Stateless queries over a captured DescriptorRecord, plus the pixel-shader
// MSAA-SRV demote mask they feed. Nothing here touches the command queue
// instance or its nested state types, so it can be compiled and analysed on
// its own.

#include "d3d12_descriptor_heap.hpp"

#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Binding slots covered by one half of the pixel-shader MSAA-SRV demote mask
// pair: `mask_lo` covers [0, kPixelShaderMsaaSrvDemoteMaskBits) and `mask_hi`
// the next range of the same width.
inline constexpr UINT kPixelShaderMsaaSrvDemoteMaskBits = 64;

// SRV/UAV view dimension carried by a descriptor record, or 0 when the record
// has no desc or is neither an SRV nor a UAV. Diagnostics only.
[[nodiscard]] uint32_t
DescriptorViewDimension(const DescriptorRecord &descriptor);

// True when the record is an SRV that resolves to a multisampled texture.
// Records without a desc fall back to the resource's own sample count.
[[nodiscard]] bool
ShaderResourceViewIsMultisampledTexture(const DescriptorRecord &descriptor);

// Marks `binding_slot` in the demote mask pair. Slots at or beyond twice
// kPixelShaderMsaaSrvDemoteMaskBits are dropped.
void SetPixelShaderMsaaSrvDemoteBit(uint64_t &mask_lo, uint64_t &mask_hi,
                                    UINT binding_slot);

} // namespace dxmt::d3d12
