#pragma once

// Aggregate types of the compiled command-list replay execution path.
//
// NativeBindingRecipeKey / NativeBindingRecipeKeyHash /
// NativeBindingRecipeCache, ReplayBreakdownAccumulators,
// CompiledReplayContext, CompiledGraphicsPacketPlan and
// CompiledComputePacketPlan used to be private nested types of class
// CommandQueueImpl (d3d12_command_queue_execute.inc). None of them names
// CommandQueueImpl or reaches into a queue instance: they are plain data
// aggregates over already-hoisted replay types, and CompiledReplayContext in
// particular is nothing but a bundle of references to the caller's locals.
//
// ReplayBreakdownAccumulators was the last one blocked, because it embeds a
// StallProbe; that type now lives at namespace scope in
// d3d12_replay_stall_probe.hpp.
//
// Unqualified uses from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_command_list.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_replay_stall_probe.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_descriptor_revision.hpp"
#include "dxmt_statistics.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Identity of a native binding recipe: the Close-time binding program plus
// the Execute-time descriptor snapshot it was frozen against.
struct NativeBindingRecipeKey {
  const CompiledBindingProgram *program = nullptr;
  const GraphicsBindingSnapshot *snapshot = nullptr;

  bool operator==(const NativeBindingRecipeKey &) const = default;
};

struct NativeBindingRecipeKeyHash {
  size_t operator()(const NativeBindingRecipeKey &key) const {
    auto hash = std::hash<const void *>{}(key.program);
    hash ^= std::hash<const void *>{}(key.snapshot) +
            0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
  }
};

using NativeBindingRecipeCache =
    std::unordered_map<NativeBindingRecipeKey,
                       std::shared_ptr<const CompiledNativeBindingRecipe>,
                       NativeBindingRecipeKeyHash>;

// Replay-stage timers and counters accumulated over one compiled command
// list. Aggregated so the extracted helpers take a single reference instead
// of a dozen out-parameters; every member keeps its original initial value.
struct ReplayBreakdownAccumulators {
  // PERF DIAG: per-record-type cost accumulation (cleared & dumped per slow
  // replay below) to find WHICH D3D12 command type dominates the recordLoop.
  std::unordered_map<const char *, std::pair<uint64_t, uint64_t>> by_type;
  StallProbe stall_accum = {};
  uint64_t stall_total_us = 0;
  clock::duration superseded_mask_interval = {};
  clock::duration compiled_graphics_interval = {};
  clock::duration compiled_compute_interval = {};
  clock::duration fallback_classification_interval = {};
  clock::duration typed_record_interval = {};
  uint64_t compiled_graphics_packets = 0;
  uint64_t compiled_compute_packets = 0;
  uint64_t fallback_classification_ranges = 0;
};

// Mutable cursor plus immutable inputs of one ExecuteCompiledCommandList
// call. Members are references to the caller's locals, so the extracted
// helpers read and write the very same objects the inline code did.
struct CompiledReplayContext {
  dxmt::CommandQueue &queue;
  CommandChunk *&chunk;
  ReplayState &state;
  const SubmittedCompiledCommandListPlan *submitted;
  const CompiledCommandDescriptorSnapshots *submitted_descriptor_snapshots;
  const CompiledCommandList *compiled;
  CompiledCommandTestTelemetry *test_telemetry;
  const std::vector<CommandRecord> &records;
  const CompiledImmutableVector<std::uint8_t> &superseded_state_record_mask;
  UINT &record_index;
  uint64_t &replay_record_serial;
  std::vector<UINT> &deferred_state_records;
  bool &deferred_state_is_fallback;
  NativeBindingRecipeCache &graphics_native_binding_recipes;
  NativeBindingRecipeCache &compute_native_binding_recipes;
  ReplayBreakdownAccumulators &rb;
  bool replay_perf;
  bool replay_breakdown;
};

// Everything QueueCompiledGraphicsPacket resolves before it can emit: the
// Metal pipeline selection, the frozen descriptor backing and the per-packet
// binding / direct-access lists shared by all three emit paths.
struct CompiledGraphicsPacketPlan {
  bool native_packet = false;
  PipelineState *pipeline = nullptr;
  RootSignature *root = nullptr;
  std::shared_ptr<SubmittedFrozenNativeDescriptorStore> frozen_native;
  uint64_t compiled_demote_msaa_srv_mask_lo = 0;
  uint64_t compiled_demote_msaa_srv_mask_hi = 0;
  const PipelineMetalGraphicsState *metal = nullptr;
  WMT::Reference<WMT::RenderPipelineState> metal_pso;
  std::optional<WMTPrimitiveType> primitive;
  bool indexed = false;
  // Owns the per-packet attachment identity encoder_attachments may point
  // at, so that pointer stays live for the whole packet.
  std::optional<ReplayRenderPassAttachments> packet_attachments;
  const ReplayRenderPassAttachments *encoder_attachments = nullptr;
  CompiledPacketBindingState binding_state = {};
  CompiledDirectAccessList direct_access = {};
  uint64_t argument_buffer_size = 0;
  std::shared_ptr<GraphicsBindingSnapshot> bindless_snapshot;
  std::shared_ptr<const CompiledNativeBindingRecipe> native_binding_recipe;
  uint64_t binding_fingerprint = 0;
  dxmt::DescriptorContentRevision submitted_descriptor_revision = {};
};

// Compute counterpart: the resolved pipeline plus the fully built dispatch
// packet that the direct and indirect emit paths consume.
struct CompiledComputePacketPlan {
  bool native_packet = false;
  PipelineState *pipeline = nullptr;
  RootSignature *root = nullptr;
  const PipelineMetalComputeState *metal = nullptr;
  std::shared_ptr<SubmittedFrozenNativeDescriptorStore> frozen_native;
  uint64_t argument_buffer_size = 0;
  ReplayDispatchPacket dispatch_packet = {};
};

} // namespace dxmt::d3d12
