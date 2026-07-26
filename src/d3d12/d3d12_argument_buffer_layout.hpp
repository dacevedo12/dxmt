#pragma once

#include "d3d12_pipeline.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

// Sub-allocates one 32-byte aligned range from a monotonically increasing
// argument buffer cursor. Saturates the cursor to UINT64_MAX instead of
// wrapping, so an overflowing estimate can never produce an in-range offset.
uint64_t AllocateArgumentBuffer(uint64_t &cursor, uint64_t size);

[[nodiscard]] uint64_t AlignArgumentBufferSize(uint64_t size);

[[nodiscard]] uint64_t AdvanceArgumentBufferEstimate(uint64_t cursor,
                                                    uint64_t size);

[[nodiscard]] uint64_t
EstimateShaderArgumentBufferSize(const PipelineDxilShader &shader);

[[nodiscard]] uint64_t
EstimateGraphicsArgumentBufferSize(PipelineState &pipeline, bool use_geometry,
                                   bool use_tessellation);

[[nodiscard]] uint64_t
EstimateComputeArgumentBufferSize(PipelineState &pipeline);

// Size of the indirect argument record one (indexed) draw writes.
[[nodiscard]] uint64_t EstimateDrawArgumentBufferSize(bool indexed);

} // namespace dxmt::d3d12
