#include "d3d12_indirect_topology.hpp"

#include <climits>
#include <cstdint>
#include <cstring>

namespace dxmt::d3d12 {

UINT
IndirectArgumentByteSize(const D3D12_INDIRECT_ARGUMENT_DESC &argument) {
  // Type is whatever the application stored in the command signature desc, and
  // *loading* an out-of-range value through the enum type is undefined on its
  // own -- UBSan's -fsanitize=enum reports it before the switch below ever gets
  // to reject it. Read the object representation and dispatch on the integer so
  // the defensive default arm is reachable without invoking UB to get there.
  static_assert(sizeof(argument.Type) == sizeof(std::uint32_t));
  std::uint32_t type = 0;
  std::memcpy(&type, &argument.Type, sizeof(type));
  switch (type) {
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
    return sizeof(D3D12_DRAW_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
    return sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
    return sizeof(D3D12_DISPATCH_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
    return sizeof(D3D12_VERTEX_BUFFER_VIEW);
  case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
    return sizeof(D3D12_INDEX_BUFFER_VIEW);
  case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
    // Num32BitValuesToSet is application input and the product overflows a UINT
    // from 0x40000000 on. Returning the truncated size would understate the
    // span that ApplyIndirectStateArgument then memcpys, so report the value as
    // unsupported (0) the same way an unrecognized argument type is.
    const UINT64 constant_size =
        UINT64(sizeof(UINT)) * argument.Constant.Num32BitValuesToSet;
    return constant_size > UINT_MAX ? 0 : UINT(constant_size);
  }
  case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
  case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
  case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
    return sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
  default:
    return 0;
  }
}

DirectIndirectOperation
GetDirectIndirectOperation(
    const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &arguments) {
  if (arguments.size() != 1)
    return DirectIndirectOperation::None;

  switch (arguments[0].Type) {
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
    return DirectIndirectOperation::Draw;
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
    return DirectIndirectOperation::DrawIndexed;
  case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
    return DirectIndirectOperation::Dispatch;
  default:
    return DirectIndirectOperation::None;
  }
}

std::optional<WMTPrimitiveType>
GetPrimitiveType(D3D12_PRIMITIVE_TOPOLOGY topology) {
  switch (topology) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    return WMTPrimitiveTypePoint;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    return WMTPrimitiveTypeLine;
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    return WMTPrimitiveTypeLineStrip;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    return WMTPrimitiveTypeTriangle;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    return WMTPrimitiveTypeTriangleStrip;
  default:
    return std::nullopt;
  }
}

std::optional<uint32_t>
GetPatchControlPointCount(D3D12_PRIMITIVE_TOPOLOGY topology) {
  if (topology < D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST ||
      topology > D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)
    return std::nullopt;
  return uint32_t(topology) -
         uint32_t(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) + 1u;
}

bool
IsGeometryStripTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
  switch (topology) {
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    return true;
  default:
    return false;
  }
}

std::optional<std::pair<uint32_t, uint32_t>>
GetGeometryVertexCount(D3D12_PRIMITIVE_TOPOLOGY topology) {
  switch (topology) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
    return std::make_pair(32u, 32u);
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    return std::make_pair(32u, 31u);
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
    return std::make_pair(30u, 30u);
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    return std::make_pair(32u, 30u);
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
    return std::make_pair(32u, 29u);
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    return std::make_pair(32u, 28u);
  default:
    return std::nullopt;
  }
}

WMT::Reference<WMT::RenderPipelineState>
SelectGraphicsPipelineState(const PipelineMetalGraphicsState &metal,
                            D3D12_PRIMITIVE_TOPOLOGY topology,
                            DXGI_FORMAT index_format) {
  if (metal.use_tessellation) {
    if (index_format == DXGI_FORMAT_R16_UINT && metal.tessellation_pso_u16)
      return metal.tessellation_pso_u16;
    if (index_format == DXGI_FORMAT_R32_UINT && metal.tessellation_pso_u32)
      return metal.tessellation_pso_u32;
    return metal.pso;
  }
  if (metal.use_geometry && IsGeometryStripTopology(topology) &&
      metal.strip_pso)
    return metal.strip_pso;
  return metal.pso;
}

WMTIndexType
GetIndexType(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32
                                        : WMTIndexTypeUInt16;
}

UINT
GetIndexSize(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R32_UINT ? 4 : 2;
}

bool
IsSupportedIndexBufferFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R32_UINT;
}

size_t
QueryResultStride(D3D12_QUERY_TYPE type) {
  switch (type) {
  case D3D12_QUERY_TYPE_OCCLUSION:
  case D3D12_QUERY_TYPE_BINARY_OCCLUSION:
  case D3D12_QUERY_TYPE_TIMESTAMP:
    return sizeof(uint64_t);
  case D3D12_QUERY_TYPE_PIPELINE_STATISTICS:
    return sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
  case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0:
  case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
  case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2:
  case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3:
    return sizeof(D3D12_QUERY_DATA_SO_STATISTICS);
  default:
    return 0;
  }
}

} // namespace dxmt::d3d12
