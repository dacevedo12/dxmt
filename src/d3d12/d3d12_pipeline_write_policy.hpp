#pragma once

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "winemetal.h"

#include <d3d12.h>

namespace dxmt::d3d12 {

[[nodiscard]] bool StencilOpWrites(D3D12_STENCIL_OP op);

[[nodiscard]] bool
StencilFaceWrites(const D3D12_DEPTH_STENCILOP_DESC &face);

[[nodiscard]] bool PipelineWritesDepth(const PipelineGraphicsState *graphics);

[[nodiscard]] bool
PipelineWritesStencil(const PipelineGraphicsState *graphics);

[[nodiscard]] int
AccessForDepthStencilPlane(const DescriptorRecord &descriptor,
                           D3D12_DSV_FLAGS read_only_flag,
                           bool pipeline_writes);

[[nodiscard]] D3D12_RESOURCE_STATES
DepthStencilResourceStateForAccess(int depth_access, int stencil_access);

// Rejects threadgroup sizes and grids the D3D12 runtime would refuse, before
// they reach the Metal encoder.
[[nodiscard]] bool ValidateComputeDispatch(const WMTSize &threadgroup_size,
                                           UINT x, UINT y, UINT z);

} // namespace dxmt::d3d12
