#pragma once

// Namespace-level replay draw packet types.
//
// These definitions used to be nested inside class CommandQueueImpl
// (d3d12_command_queue_draw_encode.inc). They are pure data carriers: a draw
// record resolved against the replay state at submission time, handed to the
// encoder thread. Nothing here names the queue class, so hoisting them to
// dxmt::d3d12 lets the packet operations (fingerprinting, dynamic render state
// recipe building) compile as independent translation units.
//
// The counterpart for compute is ReplayDispatchPacket in
// d3d12_replay_queue_state_types.hpp; that one has to stay coupled to the
// queue class because it embeds a ReplayState.

#include "Metal.hpp"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_descriptor_revision.hpp"
#include "dxmt_occlusion_query.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <d3d12.h>

namespace dxmt::d3d12 {

class PipelineState;

// State shared by every graphics draw packet: the resolved Metal PSO and
// depth/stencil/rasterizer state, the binding program (compiled direct path)
// or binding snapshot (interpreted path), and the dynamic render state.
struct ReplayDrawPacketCommon {
  WMT::Reference<WMT::RenderPipelineState> metal_pso;
  WMT::Reference<WMT::DepthStencilState> depth_stencil;
  wmtcmd_render_setrasterizerstate rasterizer = {};
  PipelineState *pipeline = nullptr;
  std::shared_ptr<const GraphicsBindingSnapshot> binding_snapshot;
  std::optional<CompiledDirectGraphicsBindingPayload>
      compiled_binding_payload;
  CompiledPacketBindingState compiled_binding_state = {};
  CompiledDirectAccessList compiled_direct_access;
  std::shared_ptr<const CompiledBindingProgram> binding_program;
  CompiledBindingDelta binding_delta;
  CompiledEncoderBindingIdentity binding_identity = {};
  std::shared_ptr<CompiledCommandTestTelemetry> test_telemetry;
  uint64_t binding_generation = 0;
  dxmt::DescriptorContentRevision descriptor_content_revision = {};
  uint64_t binding_content_fingerprint = 0;
  uint64_t pixel_shader_demote_msaa_srv_mask_lo = 0;
  uint64_t pixel_shader_demote_msaa_srv_mask_hi = 0;
  std::optional<WMTPrimitiveType> primitive;
  std::optional<std::pair<uint32_t, uint32_t>> geometry_counts;
  std::optional<uint32_t> control_point_count;
  std::array<FLOAT, 4> blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
  UINT stencil_ref = 0;
  Rc<VisibilityResultQuery> visibility_query;
  std::shared_ptr<const dxmt::d3d12::CompiledDynamicRenderStateRecipe>
      dynamic_state_recipe;
  CompiledImmutableVector<D3D12_VIEWPORT> viewports;
  CompiledImmutableVector<D3D12_RECT> scissors;
  uint64_t max_object_threadgroups = 0;
  uint32_t tess_threads_per_patch = 0;
  uint32_t tess_num_output_control_point_element = 0;
  bool use_geometry = false;
  bool use_tessellation = false;
  BindlessMirrorDrawDiag bindless_diag = {};
};

struct ReplayDrawInstancedPacket {
  ReplayDrawPacketCommon common;
  UINT vertex_start = 0;
  UINT vertex_count = 0;
  UINT instance_count = 0;
  UINT base_instance = 0;
};

struct ReplayDrawIndexedInstancedPacket {
  ReplayDrawPacketCommon common;
  Rc<BufferAllocation> index_allocation;
  WMTIndexType index_type = WMTIndexTypeUInt16;
  UINT64 index_binding_offset = 0;
  UINT64 index_offset = 0;
  UINT index_count = 0;
  UINT start_index = 0;
  UINT instance_count = 0;
  INT base_vertex = 0;
  UINT base_instance = 0;
};

struct ReplayDrawIndirectCompiledPacket {
  ReplayDrawPacketCommon common;
  Rc<Buffer> argument_buffer;
  UINT64 argument_offset = 0;
  UINT argument_size = 0;
  Rc<BufferAllocation> index_allocation;
  WMTIndexType index_type = WMTIndexTypeUInt16;
  UINT64 index_buffer_offset = 0;
  bool indexed = false;
};

} // namespace dxmt::d3d12
