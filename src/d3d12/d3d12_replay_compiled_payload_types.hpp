#pragma once

// Namespace-level compiled-packet binding payload types.
//
// These definitions used to be nested inside the anonymous-namespace class
// CommandQueueImpl (d3d12_command_queue_replay_types.inc). They describe the
// immutable per-packet binding state handed to the compiled encoders and never
// name the queue class itself.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_context.hpp"
#include "dxmt_descriptor_revision.hpp"
#include "dxmt_texture.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct CompiledRootConstantsSlot {
  bool valid = false;
  UINT dst_offset = 0;
  std::vector<UINT> values;
  WMT::Reference<WMT::Buffer> frozen_buffer;
  uint64_t frozen_offset = 0;
  uint64_t frozen_length = 0;
  uint64_t frozen_gpu_address = 0;
};

struct CompiledRootDescriptorSlot {
  bool valid = false;
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
  std::optional<DescriptorRecord> frozen_descriptor;
};

struct CompiledPacketBindingState {
  static constexpr UINT kMaxRootParameters = 64;
  std::array<CompiledRootConstantsSlot, kMaxRootParameters> root_constants =
      {};
  std::array<CompiledRootDescriptorSlot, kMaxRootParameters> cbv_roots = {};
  std::array<CompiledRootDescriptorSlot, kMaxRootParameters> srv_roots = {};
  std::array<CompiledRootDescriptorSlot, kMaxRootParameters> uav_roots = {};
};

struct CompiledEncoderBindingIdentity {
  const void *program = nullptr;
  const void *resource_heap = nullptr;
  const void *sampler_heap = nullptr;
  const void *root_tables = nullptr;
  const void *root_constants = nullptr;
  const void *root_descriptors = nullptr;
  const void *vertex_bindings = nullptr;
};

enum class CompiledNativeBindingOpKind : uint8_t {
  ArgumentBuffer,
  NullConstantBuffer,
  NullBuffer,
};

struct CompiledNativeBindingRecipe {
  struct Op {
    CompiledNativeBindingOpKind kind =
        CompiledNativeBindingOpKind::ArgumentBuffer;
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t offset = 0;
    uint32_t index = 0;
    uint32_t required_dirty_fields = UINT32_MAX;
    uint64_t root_table_mask = 0;
    uint64_t root_constant_mask = 0;
    uint64_t root_descriptor_mask = 0;
    bool compute = false;
    WMTRenderStages render_stages = {};
  };
  std::vector<Op> ops;
};

struct CompiledDirectGraphicsBindingPayload {
  Com<ID3D12PipelineState> pipeline_state;
  Com<ID3D12RootSignature> root_signature;
  std::shared_ptr<const std::vector<CompiledCommandRootDescriptorTable>>
      root_tables;
  CompiledCommandInputAssemblerState input_assembler;
  std::shared_ptr<GraphicsBindingSnapshot> bindless_snapshot;
  std::shared_ptr<SubmittedFrozenNativeDescriptorStore> frozen_native;
  std::shared_ptr<const CompiledNativeBindingRecipe> native_binding_recipe;
  std::shared_ptr<const CompiledVertexBindingRecipe> vertex_binding_recipe;
};

struct CompiledDirectAccessList {
  enum class Kind : uint8_t { BufferRange, BufferView, TextureView };

  struct EncoderAccess {
    PipelineStage stage = PipelineStage::Vertex;
    Kind kind = Kind::BufferRange;
    Rc<Buffer> buffer;
    Rc<Texture> texture;
    uint64_t offset = 0;
    uint64_t length = 0;
    uint64_t view_id = 0;
    int flags = 0;
  };

  CompiledImmutableVector<Rc<BufferAllocation>>
      static_buffer_allocations;
  std::vector<Rc<BufferAllocation>> buffer_allocations;
  std::vector<EncoderAccess> encoder_accesses;
  // True when descriptor-backed accesses were resolved from the immutable
  // Execute snapshot. A false value is an explicit compatibility escape
  // hatch for packets created without a submission snapshot.
};

struct CompiledDirectComputeBindingPayload {
  CompiledComputePacket packet;
  std::shared_ptr<const std::vector<CompiledCommandRootDescriptorTable>>
      root_tables;
  CompiledPacketBindingState binding_state;
  CompiledDirectAccessList direct_access;
  RootSignature *root = nullptr;
  std::shared_ptr<GraphicsBindingSnapshot> bindless_snapshot;
  std::shared_ptr<SubmittedFrozenNativeDescriptorStore> frozen_native;
  std::shared_ptr<const CompiledNativeBindingRecipe> native_binding_recipe;
  CompiledEncoderBindingIdentity binding_identity = {};
  dxmt::DescriptorContentRevision descriptor_content_revision = {};
  std::shared_ptr<CompiledCommandTestTelemetry> test_telemetry;
};

} // namespace dxmt::d3d12
