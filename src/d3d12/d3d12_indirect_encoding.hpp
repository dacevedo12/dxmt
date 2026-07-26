#pragma once

#include "d3d12_indirect_topology.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "dxmt_context.hpp"

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// True when the command signature writes root-signature state from the indirect
// argument buffer, which is only legal when the signature also carries a root
// signature.
[[nodiscard]] bool
RequiresRootSignature(const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &arguments);

// Applies the ExecuteIndirect count predicate to a single command, zeroing the
// copy in `counted_args` when `command_index` is past the GPU-side count. Runs
// in its own compute pass.
void PrepareCountedIndirectArguments(ArgumentEncodingContext &enc,
                                     const Rc<Buffer> &arg_buffer,
                                     UINT64 arg_offset,
                                     const Rc<Buffer> &count_buffer,
                                     UINT64 count_offset,
                                     const Rc<Buffer> &counted_args,
                                     UINT64 counted_offset, UINT argument_size,
                                     UINT command_index);

// Applies the count predicate to a whole ExecuteIndirect argument stream in one
// compute pass, writing `command_count` tightly packed commands into
// `counted_args`. Doing all commands at once avoids one compute + render pass
// pair per command, whose artificial render-pass boundaries can discard
// tile-local attachment contents on TBDR hardware.
void ExpandCountedIndirectArgumentStream(
    ArgumentEncodingContext &enc, const Rc<Buffer> &arg_buffer,
    UINT64 arg_base_offset, UINT64 source_length,
    const Rc<Buffer> &count_buffer, UINT64 count_offset,
    const Rc<Buffer> &counted_args, UINT64 destination_length,
    UINT command_count, UINT stride, UINT argument_size);

// How the direct-path indirect draw replays lower the bound primitive
// topology for the PSO variant they selected. Exactly one of the three
// results is engaged: the control point count for a tessellation PSO, the
// (vertices per warp, vertex increment per warp) pair for a geometry PSO, and
// otherwise the plain Metal primitive type.
struct IndirectDrawTopologyLowering {
  std::optional<WMTPrimitiveType> primitive;
  std::optional<std::pair<uint32_t, uint32_t>> geometry_counts;
  std::optional<uint32_t> control_point_count;
  // False when the topology cannot be lowered for this PSO. The reason has
  // already been logged and the draw has to be dropped.
  bool ok = false;
};

// `indexed` only selects which of the two log wordings is used, so that the
// indexed and non-indexed indirect draw paths keep their own messages.
[[nodiscard]] IndirectDrawTopologyLowering
LowerIndirectDrawTopology(const PipelineMetalGraphicsState &metal,
                          D3D12_PRIMITIVE_TOPOLOGY topology, bool indexed);

// Applies one of the state-setting ExecuteIndirect arguments (vertex/index
// buffer view, root constants, root CBV/SRV/UAV) read from the CPU-visible
// argument buffer at `bytes` to the replay state. `compute` is the currently
// bound pipeline kind, which decides which root slot array is written.
//
// Returns false when `argument` is not a state-setting argument this fallback
// knows how to lower, in which case the reason has already been logged and the
// whole ExecuteIndirect has to be dropped. The draw and dispatch arguments are
// handled by the caller, which owns the replay entry points.
[[nodiscard]] bool
ApplyIndirectStateArgument(ReplayState &state,
                           const D3D12_INDIRECT_ARGUMENT_DESC &argument,
                           const uint8_t *bytes, bool compute);

// Rate-limited diagnostic trace of one encoded indirect dispatch. Does nothing
// unless ExecuteIndirect diagnostics are enabled.
void LogDispatchIndirectEncode(UINT command_index, bool has_count,
                               UINT64 arg_offset, UINT argument_size,
                               UINT64 counted_offset,
                               obj_handle_t indirect_buffer,
                               UINT64 indirect_offset, obj_handle_t metal_pso,
                               WMTSize threadgroup_size);

// Rate-limited diagnostic trace of a direct-path ExecuteIndirect replay. Does
// nothing unless ExecuteIndirect diagnostics are enabled.
void LogExecuteIndirectDirect(DirectIndirectOperation operation,
                              UINT command_count, UINT stride,
                              UINT argument_size, ID3D12Resource *arg_resource,
                              UINT64 arg_heap_offset, UINT64 arg_d3d_offset,
                              UINT64 arg_base_offset,
                              ID3D12Resource *count_resource,
                              UINT64 count_heap_offset, UINT64 count_d3d_offset,
                              UINT64 count_base_offset);

} // namespace dxmt::d3d12
