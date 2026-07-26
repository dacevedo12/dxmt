#include "d3d12_indirect_argument_stream.hpp"

#include "d3d12_indirect_encoding.hpp"
#include "log/log.hpp"

#include <cstring>

namespace dxmt::d3d12 {

ExecuteIndirectDispatchPlan
PlanExecuteIndirectDispatch(const CommandSignature &signature) {
  const auto &desc = signature.GetDesc();
  const auto &arguments = signature.GetArguments();
  if (!desc.ByteStride || arguments.empty())
    return {};
  if (RequiresRootSignature(arguments) && !signature.GetRootSignature()) {
    WARN("D3D12CommandQueue: ExecuteIndirect skipped because command signature has root arguments but no root signature");
    return {};
  }

  return {true, GetDirectIndirectOperation(arguments)};
}

IndirectArgumentStreamLayout
ComputeIndirectArgumentStreamLayout(const D3D12_INDIRECT_ARGUMENT_DESC &argument,
                                    UINT byte_stride, UINT command_count) {
  const UINT argument_size = IndirectArgumentByteSize(argument);
  if (!argument_size || byte_stride < argument_size) {
    WARN("D3D12CommandQueue: ExecuteIndirect argument layout exceeds stride");
    return {};
  }

  IndirectArgumentStreamLayout layout = {};
  layout.valid = true;
  layout.argument_size = argument_size;
  layout.source_length =
      UINT64(command_count - 1) * byte_stride + argument_size;
  layout.counted_length = UINT64(command_count) * argument_size;
  return layout;
}

UINT64
IndirectCommandArgumentOffset(bool prepared_counted_stream,
                              UINT64 arg_base_offset, UINT command_index,
                              UINT byte_stride, UINT argument_size) {
  return prepared_counted_stream
             ? UINT64(command_index) * argument_size
             : arg_base_offset + UINT64(command_index) * byte_stride;
}

IndirectArgumentSpan
LocateIndirectArgument(const uint8_t *command, size_t command_size,
                       size_t argument_offset,
                       const D3D12_INDIRECT_ARGUMENT_DESC &argument) {
  const auto argument_size = IndirectArgumentByteSize(argument);
  if (!argument_size || argument_offset + argument_size > command_size) {
    WARN("D3D12CommandQueue: ExecuteIndirect argument layout exceeds stride");
    return {};
  }
  return {true, argument_size, command + argument_offset};
}

DrawInstancedRecord
DecodeIndirectDrawArguments(const uint8_t *bytes) {
  D3D12_DRAW_ARGUMENTS args = {};
  std::memcpy(&args, bytes, sizeof(args));
  return DrawInstancedRecord{args.VertexCountPerInstance, args.InstanceCount,
                             args.StartVertexLocation,
                             args.StartInstanceLocation};
}

DrawIndexedInstancedRecord
DecodeIndirectDrawIndexedArguments(const uint8_t *bytes) {
  D3D12_DRAW_INDEXED_ARGUMENTS args = {};
  std::memcpy(&args, bytes, sizeof(args));
  return DrawIndexedInstancedRecord{
      args.IndexCountPerInstance, args.InstanceCount, args.StartIndexLocation,
      args.BaseVertexLocation, args.StartInstanceLocation};
}

DispatchRecord
DecodeIndirectDispatchArguments(const uint8_t *bytes) {
  D3D12_DISPATCH_ARGUMENTS args = {};
  std::memcpy(&args, bytes, sizeof(args));
  return DispatchRecord{args.ThreadGroupCountX, args.ThreadGroupCountY,
                        args.ThreadGroupCountZ};
}

} // namespace dxmt::d3d12
