#pragma once

// Namespace-level replay command / batch / ReplayState types.
//
// These definitions used to be nested inside class CommandQueueImpl
// (d3d12_command_queue_replay_types.inc). They form the one dependency chain
// that *does* name the queue class: ReplayPassEncodeCommand::Encode takes a
// CommandQueueImpl reference, ReplayGraphicsPassCommand /
// ReplayComputePassCommand hold a shared_ptr to that encoder base,
// ReplayGraphicsPassBatch / ReplayComputePassBatch hold vectors of those
// commands, ReplayState holds both batches, and ReplayDispatchPacket holds an
// optional ReplayState.
//
// Hoisting them was only possible once CommandQueueImpl moved out of the
// anonymous namespace of d3d12_command_queue.cpp into the named dxmt::d3d12
// namespace: a forward declaration of an anonymous-namespace class in a header
// would denote a *different* type and break every `override`. The forward
// declaration below therefore refers to exactly the class defined in
// d3d12_command_queue.cpp.
//
// Only *declarations* that mention CommandQueueImpl live here. Nothing in this
// header requires the complete definition of CommandQueueImpl: the pure virtual
// Encode declaration and the template bodies below merely bind or forward a
// CommandQueueImpl reference, which is well-formed for an incomplete type. Any
// implementation that needs CommandQueueImpl's members (for example
// ResetReplayCommandListSemanticState, which calls the member
// ResetReplayApiState) must stay in the .cpp/.inc. Operating on these types
// alone is not such a case: CloneReplayStateWithoutBatch, for one, only reads
// ReplayState plus the namespace-level perDrawSubTimers() ledger, so it lives
// in d3d12_replay_state_clone.hpp.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_device_queue_state.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_state_types.hpp"
#include "d3d12_resource_barrier_batch.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_context.hpp"
#include "dxmt_descriptor_revision.hpp"
#include "dxmt_statistics.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Defined in d3d12_command_queue.cpp, in this same named namespace.
class CommandQueueImpl;

class ReplayPassEncodeCommand {
public:
  ReplayPassEncodeCommand() = default;
  ReplayPassEncodeCommand(const ReplayPassEncodeCommand &) = delete;
  ReplayPassEncodeCommand &
  operator=(const ReplayPassEncodeCommand &) = delete;
  virtual ~ReplayPassEncodeCommand() noexcept = default;
  virtual void Encode(CommandQueueImpl &queue,
                      ArgumentEncodingContext &enc,
                      uint64_t &argbuf_offset) = 0;
};

template <typename EncoderFn>
class ReplayOwnedEncodeCommand final : public ReplayPassEncodeCommand {
public:
  explicit ReplayOwnedEncodeCommand(EncoderFn encoder) noexcept
      : encoder_(std::move(encoder)) {}

  void Encode(CommandQueueImpl &, ArgumentEncodingContext &enc,
              uint64_t &argbuf_offset) override {
    encoder_(enc, argbuf_offset);
  }

private:
  EncoderFn encoder_;
};

template <typename Payload, typename EncoderFn>
class ReplayCompiledEncodeCommand final
    : public ReplayPassEncodeCommand {
public:
  ReplayCompiledEncodeCommand(Payload payload, EncoderFn encoder) noexcept
      : payload_(std::move(payload)), encoder_(encoder) {}

  void Encode(CommandQueueImpl &queue, ArgumentEncodingContext &enc,
              uint64_t &argbuf_offset) override {
    encoder_(queue, enc, payload_, argbuf_offset);
  }

private:
  Payload payload_;
  EncoderFn encoder_;
};

struct ReplayGraphicsPassCommand {
  ReplayGraphicsPassCommand() = default;
  ReplayGraphicsPassCommand(const ReplayGraphicsPassCommand &) = delete;
  ReplayGraphicsPassCommand &
  operator=(const ReplayGraphicsPassCommand &) = delete;
  ReplayGraphicsPassCommand(ReplayGraphicsPassCommand &&) noexcept =
      default;
  ReplayGraphicsPassCommand &
  operator=(ReplayGraphicsPassCommand &&) noexcept = default;
  ~ReplayGraphicsPassCommand() noexcept = default;

  std::shared_ptr<ReplayPassEncodeCommand> encoder;
  uint64_t d3d_sequence = 0;
  uint64_t argument_buffer_size = 0;
  uint64_t argument_buffer_offset = 0;
  uint32_t command_index = 0;
  ReplayGraphicsCommandKind kind = ReplayGraphicsCommandKind::Draw;
  bool parallel_candidate = false;
  bool use_geometry = false;
  bool use_tessellation = false;
  bool bindless_compiled_candidate = false;
};

struct ReplayGraphicsPassBatch {
  ReplayRenderPassAttachments attachments;
  std::shared_ptr<ReplayEncoderArena> encoder_arena;
  std::vector<ReplayGraphicsPassCommand> commands;
  std::unordered_multimap<uint64_t,
                          std::shared_ptr<GraphicsBindingSnapshot>>
      captured_binding_snapshots;
  uint64_t argument_buffer_size = 0;
  ReplayGraphicsPassPlan plan;
  bool use_geometry = false;
  bool use_tessellation = false;
  bool active = false;
};

struct ReplayComputePassCommand {
  ReplayComputePassCommand() = default;
  ReplayComputePassCommand(const ReplayComputePassCommand &) = delete;
  ReplayComputePassCommand &
  operator=(const ReplayComputePassCommand &) = delete;
  ReplayComputePassCommand(ReplayComputePassCommand &&) noexcept = default;
  ReplayComputePassCommand &
  operator=(ReplayComputePassCommand &&) noexcept = default;
  ~ReplayComputePassCommand() noexcept = default;

  std::shared_ptr<ReplayPassEncodeCommand> encoder;
  uint64_t d3d_sequence = 0;
  uint64_t argument_buffer_offset = 0;
  uint64_t argument_buffer_size = 0;
  ReplayComputeCommandKind kind = ReplayComputeCommandKind::Dispatch;
  bool compiled_candidate = false;
};

struct ReplayComputePassBatch {
  std::shared_ptr<ReplayEncoderArena> encoder_arena;
  std::vector<ReplayComputePassCommand> commands;
  uint64_t argument_buffer_size = 0;
  bool active = false;
};

static_assert(
    std::is_nothrow_move_constructible_v<ReplayGraphicsPassCommand>);
static_assert(
    std::is_nothrow_move_constructible_v<ReplayComputePassCommand>);
static_assert(!std::is_copy_constructible_v<ReplayGraphicsPassCommand>);
static_assert(!std::is_copy_constructible_v<ReplayComputePassCommand>);
static_assert(std::is_nothrow_move_constructible_v<
              ReplayCommandStorage<ReplayGraphicsPassCommand>>);
static_assert(std::is_nothrow_move_constructible_v<
              ReplayCommandStorage<ReplayComputePassCommand>>);

struct ReplayState {
  // Root parameter index is bounded by the D3D12 root-signature 64-DWORD
  // limit, so the hottest root bindings use a fixed array indexed by parameter
  // instead of a hash map — O(1) index, no hash+bucket on the per-draw replay
  // hot path (RootDescriptorTable replay + GetTableHandle per descriptor).
  static constexpr UINT kMaxRootParameters = 64;
  std::unordered_map<ID3D12Resource *, ReplayResourceStateEntry> *resource_states =
      nullptr;
  D3D12_COMMAND_LIST_TYPE queue_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  std::vector<Com<ID3D12Resource>> *touched_resources = nullptr;
  // PERF: companion membership set for touched_resources to make
  // TouchReplayResource O(1) instead of O(R) linear scan (was O(R^2)/list).
  std::unordered_set<ID3D12Resource *> *touched_resources_set = nullptr;
  // Per-list cache of resources whose subresources are all uniformly in a
  // read-only state with no write-access sync debt. A repeated read access
  // with the same desired state is then a guaranteed no-op, so it can skip
  // GetResource, GetReplayResourceStates and the per-subresource loop.
  // Invalidated whenever the resource is transitioned, aliased, or written.
  std::unordered_map<ID3D12Resource *, D3D12_RESOURCE_STATES> steady_read_states;
  Com<ID3D12PipelineState> pipeline_state;
  Com<ID3D12RootSignature> graphics_root_signature;
  Com<ID3D12RootSignature> compute_root_signature;
  RootSignature *graphics_root_signature_impl = nullptr;
  RootSignature *compute_root_signature_impl = nullptr;
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  std::vector<D3D12_VIEWPORT> viewports;
  std::vector<D3D12_RECT> scissors;
  std::array<FLOAT, 4> blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
  UINT stencil_ref = 0;
  std::vector<DescriptorRecord> render_targets;
  std::optional<DescriptorRecord> depth_stencil;
  std::array<std::optional<D3D12_VERTEX_BUFFER_VIEW>, 32> vertex_buffers = {};
  std::optional<D3D12_INDEX_BUFFER_VIEW> index_buffer;
  std::array<ResolvedReplayVertexBuffer, 32> resolved_vertex_buffers = {};
  ResolvedReplayIndexBuffer resolved_index_buffer = {};
  std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, ResolvedReplayVertexBuffer>
      resolved_vertex_buffer_cache;
  std::unordered_map<D3D12_GPU_VIRTUAL_ADDRESS, ResolvedReplayIndexBuffer>
      resolved_index_buffer_cache;
  Com<ID3D12DescriptorHeap> cbv_srv_uav_heap;
  Com<ID3D12DescriptorHeap> sampler_heap;
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxRootParameters> graphics_tables = {};
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxRootParameters> compute_tables = {};
  std::array<ReplayRootConstantsSlot, kMaxRootParameters>
      graphics_root_constants = {};
  std::array<ReplayRootConstantsSlot, kMaxRootParameters>
      compute_root_constants = {};
  std::array<ReplayRootDescriptorSlot, kMaxRootParameters>
      graphics_cbv_roots = {};
  std::array<ReplayRootDescriptorSlot, kMaxRootParameters>
      compute_cbv_roots = {};
  std::array<ReplayRootDescriptorSlot, kMaxRootParameters>
      graphics_srv_roots = {};
  std::array<ReplayRootDescriptorSlot, kMaxRootParameters>
      compute_srv_roots = {};
  std::array<ReplayRootDescriptorSlot, kMaxRootParameters>
      graphics_uav_roots = {};
  std::array<ReplayRootDescriptorSlot, kMaxRootParameters>
      compute_uav_roots = {};
  uint64_t current_record_d3d_sequence = 0;
  dxmt::CompiledFallbackReason compiled_fallback_reason =
      dxmt::CompiledFallbackReason::Unknown;
  uint64_t graphics_binding_generation = 1;
  uint64_t last_snapshot_request_graphics_generation = 0;
  dxmt::DescriptorContentRevision last_snapshot_request_descriptor_revision =
      {};
  bool has_last_snapshot_request_generation = false;
  // descAccess content-fingerprint cache (DXMT_DESCACCESS_CACHE): skip the
  // per-draw hazard-record loop when (PSO, root-sig, descriptor CONTENT) is
  // unchanged AND no resource state changed since the last record. The content
  // fingerprint sees through ring-buffer handle churn — the same resources at
  // new handles produce the same fingerprint. access_epoch bumps on every real
  // resource-state change (barriers + non-no-op hazard records), so a cache
  // hit proves "same bindings, frozen state" → the hazard result is identical.
  uint64_t access_epoch = 0;
  std::unordered_map<uint64_t, uint64_t> da_cache;
  struct DescriptorJournalAccessCache {
    uint64_t binding_identity = 0;
    uint64_t access_epoch = 0;
    DescriptorHeapMirror *mirror = nullptr;
    uint64_t cursor = 0;
    std::unordered_map<uint32_t, std::vector<uint32_t>> entries_by_slot;
    bool valid = false;
  } descriptor_journal_access_cache;
  std::unordered_multimap<uint64_t,
                          std::shared_ptr<GraphicsBindingSnapshot>>
      compiled_binding_snapshots;
  Com<ID3D12Resource> predication_buffer;
  UINT64 predication_buffer_offset = 0;
  D3D12_PREDICATION_OP predication_operation =
      D3D12_PREDICATION_OP_EQUAL_ZERO;
  ReplayGraphicsPassBatch graphics_pass_batch;
  ReplayComputePassBatch compute_pass_batch;
  ReplayBlitBatch blit_batch;
  ResourceAccessBarrierBatch pending_resource_barriers;
  std::vector<PendingTimestampMarker> pending_timestamp_markers;
  std::vector<PendingTimestampResolve> pending_timestamp_resolves;
  std::vector<PendingCpuQueryResolve> pending_immediate_cpu_query_resolves;
  CompiledCommandTestTelemetry *submission_boundary_telemetry = nullptr;
  CompiledEncoderKind submission_boundary_kind =
      CompiledEncoderKind::None;
};

struct ReplayDispatchPacket {
  WMT::Reference<WMT::ComputePipelineState> metal_pso;
  WMTSize threadgroup_size = {};
  PipelineState *pipeline = nullptr;
  DispatchRecord dispatch = {};
  Rc<Buffer> indirect_argument_buffer;
  UINT64 indirect_argument_offset = 0;
  UINT indirect_argument_size = 0;
  uint64_t argument_buffer_size = 0;
  std::optional<ReplayState> replay_state;
  std::optional<CompiledDirectComputeBindingPayload>
      compiled_binding_payload;
};

} // namespace dxmt::d3d12
