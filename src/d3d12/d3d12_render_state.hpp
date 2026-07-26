#pragma once

#include "d3d12_pipeline.hpp"

#include <cstdint>
#include <d3d12.h>
#include <vector>

namespace dxmt::d3d12 {

template <typename T> class CompiledImmutableVector;

// Input slots at or above this index cannot be represented in the 32-bit
// vertex buffer slot mask and are therefore ignored.
inline constexpr UINT kInputSlotMaskBitCount = 32;

// Makes sure a draw has both viewports and scissor rects. When only viewports
// were recorded, viewport-sized default scissor rects are synthesized in place.
// Returns false when the draw has no viewport at all and must be skipped.
// `context` only names the caller in the emitted warnings.
[[nodiscard]] bool ResolveDynamicRasterRects(std::vector<D3D12_VIEWPORT> &viewports,
                                             std::vector<D3D12_RECT> &scissors,
                                             const char *context);

[[nodiscard]] bool
ResolveDynamicRasterRects(const CompiledImmutableVector<D3D12_VIEWPORT> &viewports,
                          CompiledImmutableVector<D3D12_RECT> &scissors,
                          const char *context);

// Bit mask of the vertex buffer input slots referenced by the graphics
// pipeline's input layout. Returns 0 when there is no graphics pipeline.
[[nodiscard]] uint32_t
InputSlotMask(const PipelineGraphicsState *graphics_state);

} // namespace dxmt::d3d12
