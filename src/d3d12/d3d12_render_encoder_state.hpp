#pragma once

#include "Metal.hpp"
#include "dxmt_context.hpp"

#include <array>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

template <typename T> class CompiledImmutableVector;
struct CompiledDynamicRenderStateRecipe;

// Emits a Metal render pipeline state switch, skipping the command when the
// encoder already has `metal_pso` bound.
void EncodeRenderPipelineStateIfChanged(ArgumentEncodingContext &enc,
                                        WMT::RenderPipelineState metal_pso);

// Emits blend factor / stencil reference, viewports and scissor rects into the
// current render encoder, skipping each group whose cached value is unchanged.
// Metal requires one scissor per viewport, so the scissor array is resized to
// the viewport count and clamped to the render target extent.
void EncodeDynamicRenderState(ArgumentEncodingContext &enc,
                              const std::vector<D3D12_VIEWPORT> &viewports,
                              const std::vector<D3D12_RECT> &scissors,
                              const std::array<FLOAT, 4> &blend_factor,
                              UINT stencil_ref);

void EncodeDynamicRenderState(
    ArgumentEncodingContext &enc,
    const CompiledImmutableVector<D3D12_VIEWPORT> &viewports,
    const CompiledImmutableVector<D3D12_RECT> &scissors,
    const std::array<FLOAT, 4> &blend_factor, UINT stencil_ref);

// Same as above for a recipe whose viewports and scissor rects were already
// resolved and clamped at submission time.
void EncodeCompiledDynamicRenderState(
    ArgumentEncodingContext &enc,
    const CompiledDynamicRenderStateRecipe &recipe);

} // namespace dxmt::d3d12
