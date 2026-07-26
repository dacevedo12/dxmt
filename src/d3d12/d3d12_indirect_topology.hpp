#pragma once

#include "Metal.hpp"
#include "d3d12_pipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// GPU-side layout of the indirect argument buffers DXMT writes for Metal.
struct DXMT_DRAW_ARGUMENTS {
  uint32_t VertexCount;
  uint32_t InstanceCount;
  uint32_t StartVertex;
  uint32_t StartInstance;
};

struct DXMT_DRAW_INDEXED_ARGUMENTS {
  uint32_t IndexCount;
  uint32_t InstanceCount;
  uint32_t StartIndex;
  int32_t BaseVertex;
  uint32_t StartInstance;
};

struct DXMT_DISPATCH_ARGUMENTS {
  uint32_t X;
  uint32_t Y;
  uint32_t Z;
};

// The single operation a command signature performs when it carries exactly one
// draw/dispatch argument and nothing else.
enum class DirectIndirectOperation {
  None,
  Draw,
  DrawIndexed,
  Dispatch,
};

// Byte size an indirect argument occupies in a D3D12 argument buffer. Returns
// 0 for argument types this layer does not understand and for a root-constant
// argument whose payload does not fit a UINT; every caller already treats 0 as
// "reject this command signature".
[[nodiscard]] UINT
IndirectArgumentByteSize(const D3D12_INDIRECT_ARGUMENT_DESC &argument);

[[nodiscard]] DirectIndirectOperation GetDirectIndirectOperation(
    const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &arguments);

// Metal primitive type for a D3D topology, or nullopt when it needs lowering.
[[nodiscard]] std::optional<WMTPrimitiveType>
GetPrimitiveType(D3D12_PRIMITIVE_TOPOLOGY topology);

// Control point count of a patch list topology, or nullopt when not a patch.
[[nodiscard]] std::optional<uint32_t>
GetPatchControlPointCount(D3D12_PRIMITIVE_TOPOLOGY topology);

[[nodiscard]] bool IsGeometryStripTopology(D3D12_PRIMITIVE_TOPOLOGY topology);

// (input vertices, emitted primitives) budget of one geometry shader batch.
[[nodiscard]] std::optional<std::pair<uint32_t, uint32_t>>
GetGeometryVertexCount(D3D12_PRIMITIVE_TOPOLOGY topology);

// Picks the tessellation / geometry-strip / plain PSO variant for a draw.
[[nodiscard]] WMT::Reference<WMT::RenderPipelineState>
SelectGraphicsPipelineState(const PipelineMetalGraphicsState &metal,
                            D3D12_PRIMITIVE_TOPOLOGY topology,
                            DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN);

[[nodiscard]] WMTIndexType GetIndexType(DXGI_FORMAT format);
[[nodiscard]] UINT GetIndexSize(DXGI_FORMAT format);
[[nodiscard]] bool IsSupportedIndexBufferFormat(DXGI_FORMAT format);

// Per-query result size written by ResolveQueryData, or 0 when unsupported.
[[nodiscard]] size_t QueryResultStride(D3D12_QUERY_TYPE type);

} // namespace dxmt::d3d12
