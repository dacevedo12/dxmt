#include "d3d12_indirect_encoding.hpp"

#include "d3d12_command_list.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_replay_pass_batch_ops.hpp"
#include "d3d12_replay_root_state_ops.hpp"
#include "log/log.hpp"

#include <atomic>
#include <cstring>

namespace dxmt::d3d12 {

bool
RequiresRootSignature(
    const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &arguments) {
  for (const auto &argument : arguments) {
    switch (argument.Type) {
    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
    case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
    case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
    case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
      return true;
    default:
      break;
    }
  }
  return false;
}

void
PrepareCountedIndirectArguments(ArgumentEncodingContext &enc,
                                const Rc<Buffer> &arg_buffer, UINT64 arg_offset,
                                const Rc<Buffer> &count_buffer,
                                UINT64 count_offset,
                                const Rc<Buffer> &counted_args,
                                UINT64 counted_offset, UINT argument_size,
                                UINT command_index) {
  enc.startComputePass(0);
  auto [arg_allocation, arg_sub_offset] =
      enc.access<PipelineStage::Compute>(arg_buffer, arg_offset, argument_size,
                                         ResourceAccess::Read);
  auto [count_allocation, count_sub_offset] =
      enc.access<PipelineStage::Compute>(count_buffer, count_offset,
                                         sizeof(UINT), ResourceAccess::Read);
  auto [counted_allocation, counted_sub_offset] =
      enc.access<PipelineStage::Compute>(counted_args, counted_offset,
                                         argument_size, ResourceAccess::Write);
  enc.emulated_cmd.PrepareCountedIndirectArguments(
      count_allocation->buffer(), count_sub_offset + count_offset,
      arg_allocation->buffer(), arg_sub_offset + arg_offset,
      counted_allocation->buffer(), counted_sub_offset + counted_offset,
      argument_size, command_index);
  enc.endPass();
}

void
ExpandCountedIndirectArgumentStream(
    ArgumentEncodingContext &enc, const Rc<Buffer> &arg_buffer,
    UINT64 arg_base_offset, UINT64 source_length,
    const Rc<Buffer> &count_buffer, UINT64 count_offset,
    const Rc<Buffer> &counted_args, UINT64 destination_length,
    UINT command_count, UINT stride, UINT argument_size) {
  enc.startComputePass(0);
  auto [arg_allocation, arg_sub_offset] = enc.access<PipelineStage::Compute>(
      arg_buffer, arg_base_offset, source_length, ResourceAccess::Read);
  auto [count_allocation, count_sub_offset] =
      enc.access<PipelineStage::Compute>(count_buffer, count_offset,
                                         sizeof(UINT), ResourceAccess::Read);
  auto [counted_allocation, counted_sub_offset] =
      enc.access<PipelineStage::Compute>(counted_args, 0, destination_length,
                                         ResourceAccess::Write);
  for (UINT command_index = 0; command_index < command_count; command_index++) {
    enc.emulated_cmd.PrepareCountedIndirectArguments(
        count_allocation->buffer(), count_sub_offset + count_offset,
        arg_allocation->buffer(),
        arg_sub_offset + arg_base_offset + UINT64(command_index) * stride,
        counted_allocation->buffer(),
        counted_sub_offset + UINT64(command_index) * argument_size,
        argument_size, command_index);
  }
  enc.endPass();
}

bool
ApplyIndirectStateArgument(ReplayState &state,
                           const D3D12_INDIRECT_ARGUMENT_DESC &argument,
                           const uint8_t *bytes, bool compute) {
  switch (argument.Type) {
  case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
    D3D12_VERTEX_BUFFER_VIEW view = {};
    std::memcpy(&view, bytes, sizeof(view));
    if (argument.VertexBuffer.Slot < state.vertex_buffers.size()) {
      state.vertex_buffers[argument.VertexBuffer.Slot] = view;
      BumpGraphicsBindingGeneration(
          state, GraphicsBindingGenerationBumpSource::IndirectVertexBuffer);
    }
    break;
  }
  case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
    D3D12_INDEX_BUFFER_VIEW view = {};
    std::memcpy(&view, bytes, sizeof(view));
    state.index_buffer = view;
    break;
  }
  case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
    // DestOffsetIn32BitValues and Num32BitValuesToSet come from the command
    // signature the application created, not from the argument buffer. The
    // Indirect Drawing spec requires DestOffsetIn32BitValues +
    // Num32BitValuesToSet to stay inside the range the root signature declares
    // for the parameter; the declared range is not resolved here, but a root
    // signature is capped at D3D12_MAX_ROOT_COST DWORDs and each 32-bit root
    // constant costs one DWORD, so that cap holds for every legal signature.
    // Refuse the argument beyond it: the resize below would otherwise turn
    // Num32BitValuesToSet into a multi-gigabyte allocation, and StoreRootConstants
    // would hand ApplyRootConstants a destination offset that no root-constant
    // slot can hold. Only the constants are dropped; the rest of the command
    // still replays, matching how the SetRoot32BitConstants entry points drop a
    // single out-of-range command.
    const uint64_t constants_end =
        uint64_t(argument.Constant.DestOffsetIn32BitValues) +
        argument.Constant.Num32BitValuesToSet;
    if (constants_end > D3D12_MAX_ROOT_COST) {
      static std::atomic<uint32_t> log_count = 0;
      if (log_count.fetch_add(1, std::memory_order_relaxed) < 8) {
        WARN("D3D12CommandQueue: ExecuteIndirect root constants destination"
             " range is outside the root signature and was dropped"
             " rootParameterIndex=", argument.Constant.RootParameterIndex,
             " num32BitValuesToSet=", argument.Constant.Num32BitValuesToSet,
             " destOffsetIn32BitValues=",
             argument.Constant.DestOffsetIn32BitValues);
      }
      break;
    }
    RootConstantsRecord constants = {};
    constants.compute = compute;
    constants.root_parameter_index = argument.Constant.RootParameterIndex;
    constants.dst_offset = argument.Constant.DestOffsetIn32BitValues;
    constants.values.resize(argument.Constant.Num32BitValuesToSet);
    if (!constants.values.empty())
      std::memcpy(constants.values.data(), bytes,
                  constants.values.size() * sizeof(UINT));
    StoreRootConstants(state, constants);
    if (!compute)
      BumpGraphicsBindingGeneration(
          state, GraphicsBindingGenerationBumpSource::IndirectRootConstants);
    break;
  }
  case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: {
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    std::memcpy(&address, bytes, sizeof(address));
    StoreRootDescriptor(
        state, RootDescriptorRecord{compute, D3D12_ROOT_PARAMETER_TYPE_CBV,
                                    argument.ConstantBufferView
                                        .RootParameterIndex,
                                    address});
    if (!compute)
      BumpGraphicsBindingGeneration(
          state, GraphicsBindingGenerationBumpSource::IndirectRootDescriptor);
    break;
  }
  case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    std::memcpy(&address, bytes, sizeof(address));
    StoreRootDescriptor(
        state, RootDescriptorRecord{compute, D3D12_ROOT_PARAMETER_TYPE_SRV,
                                    argument.ShaderResourceView
                                        .RootParameterIndex,
                                    address});
    if (!compute)
      BumpGraphicsBindingGeneration(
          state, GraphicsBindingGenerationBumpSource::IndirectRootDescriptor);
    break;
  }
  case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
    std::memcpy(&address, bytes, sizeof(address));
    StoreRootDescriptor(
        state, RootDescriptorRecord{compute, D3D12_ROOT_PARAMETER_TYPE_UAV,
                                    argument.UnorderedAccessView
                                        .RootParameterIndex,
                                    address});
    if (!compute)
      BumpGraphicsBindingGeneration(
          state, GraphicsBindingGenerationBumpSource::IndirectRootDescriptor);
    break;
  }
  default:
    WARN("D3D12CommandQueue: unsupported ExecuteIndirect argument type ",
         argument.Type);
    return false;
  }
  return true;
}

void
LogDispatchIndirectEncode(UINT command_index, bool has_count, UINT64 arg_offset,
                          UINT argument_size, UINT64 counted_offset,
                          obj_handle_t indirect_buffer, UINT64 indirect_offset,
                          obj_handle_t metal_pso, WMTSize threadgroup_size) {
  static std::atomic<uint32_t> dispatch_indirect_log_count = 0;
  if (D3D12DiagShouldLog(dispatch_indirect_log_count,
                         D3D12DiagExecuteIndirectEnabled())) {
    WARN_FILE_ONLY("D3D12 diagnostic: DispatchIndirect encode"
         " commandIndex=", command_index,
         " hasCount=", has_count,
         " argOffset=", arg_offset,
         " argumentSize=", argument_size,
         " countedOffset=", counted_offset,
         " indirectBuffer=0x", std::hex, indirect_buffer,
         " indirectOffset=0x", indirect_offset,
         " metalPso=0x", metal_pso,
         std::dec,
         " threadgroup=", threadgroup_size.width, "x",
         threadgroup_size.height, "x", threadgroup_size.depth);
  }
}

IndirectDrawTopologyLowering
LowerIndirectDrawTopology(const PipelineMetalGraphicsState &metal,
                         D3D12_PRIMITIVE_TOPOLOGY topology, bool indexed) {
  IndirectDrawTopologyLowering lowering;
  if (metal.use_tessellation) {
    lowering.control_point_count = GetPatchControlPointCount(topology);
    if (!lowering.control_point_count) {
      // TODO(d3d12): support non-patch topologies with tessellation PSOs if needed.
      WARN(indexed
               ? "D3D12CommandQueue: tessellation indirect indexed draw skipped because primitive topology is not a patch list topology="
               : "D3D12CommandQueue: tessellation indirect draw skipped because primitive topology is not a patch list topology=",
           uint32_t(topology));
      return lowering;
    }
  } else if (metal.use_geometry) {
    lowering.geometry_counts = GetGeometryVertexCount(topology);
    if (!lowering.geometry_counts) {
      WARN(indexed
               ? "D3D12CommandQueue: geometry indirect indexed draw skipped because primitive topology is unsupported topology="
               : "D3D12CommandQueue: geometry indirect draw skipped because primitive topology is unsupported topology=",
           uint32_t(topology));
      return lowering;
    }
  } else {
    lowering.primitive = GetPrimitiveType(topology);
    if (!lowering.primitive) {
      WARN(indexed
               ? "D3D12CommandQueue: indirect indexed draw skipped because primitive topology is unsupported topology="
               : "D3D12CommandQueue: indirect draw skipped because primitive topology is unsupported topology=",
           uint32_t(topology));
      return lowering;
    }
  }
  lowering.ok = true;
  return lowering;
}

void
LogExecuteIndirectDirect(DirectIndirectOperation operation, UINT command_count,
                         UINT stride, UINT argument_size,
                         ID3D12Resource *arg_resource, UINT64 arg_heap_offset,
                         UINT64 arg_d3d_offset, UINT64 arg_base_offset,
                         ID3D12Resource *count_resource,
                         UINT64 count_heap_offset, UINT64 count_d3d_offset,
                         UINT64 count_base_offset) {
  static std::atomic<uint32_t> execute_indirect_log_count = 0;
  if (!D3D12DiagShouldLog(execute_indirect_log_count,
                          D3D12DiagExecuteIndirectEnabled()))
    return;
  WARN_FILE_ONLY("D3D12 diagnostic: ExecuteIndirect direct"
       " op=", static_cast<uint32_t>(operation),
       " maxCount=", command_count,
       " stride=", stride,
       " argSize=", argument_size,
       " hasCount=", count_resource != nullptr,
       " argResource=", arg_resource,
       " argHeapOffset=", arg_heap_offset,
       " argD3DOffset=", arg_d3d_offset,
       " argBaseOffset=", arg_base_offset,
       " countResource=", count_resource,
       " countHeapOffset=", count_heap_offset,
       " countD3DOffset=", count_d3d_offset,
       " countBaseOffset=", count_base_offset);
}

} // namespace dxmt::d3d12
