#pragma once

// Encoder side of ExecuteIndirect replay: what runs once a render or compute
// pass is already open.
//
// These were the `encode_draw` / `encode_dispatch` lambdas inside
// CommandQueueImpl (d3d12_command_queue_indirect.inc). They captured `this`
// only to reach the binding encoders, which are namespace-level now, so
// everything an indirect command needs to encode itself travels in a params
// struct plus a SubmissionBindingContext. The queue keeps pass admission and
// batching; nothing here can see it.

#include "Metal.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_submission_binding_context.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_context.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Everything one indirect draw needs after its render pass is open. `pipeline`
// outlives the encode because the pass batch retains the pipeline state object.
// The index_* members are engaged for indexed draws only.
struct IndirectDrawEncodeParams {
  WMT::Reference<WMT::RenderPipelineState> metal_pso;
  bool use_geometry = false;
  bool use_tessellation = false;
  uint64_t demote_msaa_srv_mask_lo = 0;
  uint64_t demote_msaa_srv_mask_hi = 0;
  WMT::Reference<WMT::DepthStencilState> depth_stencil;
  wmtcmd_render_setrasterizerstate rasterizer = {};
  uint32_t tess_threads_per_patch = 0;
  uint32_t tess_num_output_control_point_element = 0;
  PipelineState *pipeline = nullptr;
  ReplayState replay_state;
  std::optional<WMTPrimitiveType> primitive;
  std::optional<std::pair<uint32_t, uint32_t>> geometry_counts;
  std::optional<uint32_t> control_point_count;
  std::array<FLOAT, 4> blend_factor = {};
  UINT stencil_ref = 0;
  Rc<Buffer> arg_buffer;
  UINT64 arg_offset = 0;
  // Non-null means the count predicate still has to be applied per command, in
  // which case the arguments are read from `counted_args` instead.
  Rc<Buffer> count_buffer;
  Rc<Buffer> counted_args;
  UINT64 counted_offset = 0;
  UINT argument_size = 0;
  uint32_t max_object_threadgroups = 0;
  std::vector<D3D12_VIEWPORT> viewports;
  std::vector<D3D12_RECT> scissors;
  Rc<BufferAllocation> index_allocation;
  WMTIndexType index_type = {};
  UINT64 index_offset = 0;
};

/** Encodes one DrawInstancedIndirect into the open render encoder. Returns
 *  early without encoding a draw when the argument buffer overflowed or when
 *  the geometry/tessellation lowering cannot express this command; both are
 *  logged. */
void EncodeIndirectDraw(const SubmissionBindingContext &ctx,
                        ArgumentEncodingContext &enc,
                        IndirectDrawEncodeParams &params,
                        uint64_t &argbuf_offset);

/** Indexed counterpart of EncodeIndirectDraw; additionally requires
 *  `index_allocation`, `index_type` and `index_offset`. */
void EncodeIndirectDrawIndexed(const SubmissionBindingContext &ctx,
                               ArgumentEncodingContext &enc,
                               IndirectDrawEncodeParams &params,
                               uint64_t &argbuf_offset);

// Everything one indirect dispatch needs after its compute pass is open.
struct IndirectDispatchEncodeParams {
  WMT::Reference<WMT::ComputePipelineState> metal_pso;
  WMTSize threadgroup_size = {};
  PipelineState *pipeline = nullptr;
  ReplayState replay_state;
  uint64_t argument_buffer_size = 0;
  Rc<Buffer> arg_buffer;
  UINT64 arg_offset = 0;
  Rc<Buffer> count_buffer;
  Rc<Buffer> counted_args;
  UINT64 counted_offset = 0;
  UINT argument_size = 0;
  UINT command_index = 0;
};

/** Encodes one DispatchIndirect into the open compute encoder. */
void EncodeIndirectDispatch(const SubmissionBindingContext &ctx,
                            ArgumentEncodingContext &enc,
                            IndirectDispatchEncodeParams &params,
                            uint64_t &argbuf_offset);

} // namespace dxmt::d3d12
