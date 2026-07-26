#include "d3d12_submitted_descriptor_capture.hpp"

#include "d3d12_binding_fingerprint.hpp"
#include "d3d12_compiled_bindless_payload.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_descriptor_snapshot_journal.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_frozen_bindless_stage_tables.hpp"
#include "d3d12_native_stage_binding.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_root_binding_capture.hpp"
#include "d3d12_snapshot_capture_perf.hpp"
#include "d3d12_stage_plan_cache.hpp"
#include "d3d12_submitted_descriptor_capture_plan.hpp"
#include "d3d12_submitted_descriptor_capture_telemetry.hpp"
#include "d3d12_table_recipe_lookup.hpp"
#include "dxmt_perf_stats.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

namespace {

// The stores and the capture policy every packet of one Execute shares.
// Execute captures with all participating heap mirrors already locked and
// therefore does not re-validate the descriptor journal: nothing can have
// moved between the lock and the walk.
struct SubmittedDescriptorCaptureContext {
  std::shared_ptr<SubmittedDescriptorRecordStore> descriptor_records;
  std::shared_ptr<SubmittedBindingSnapshotArena> snapshot_arena;
  SubmittedNativeDescriptorSpanStore *native_span_store = nullptr;
  bool descriptor_heaps_locked = false;
  bool validate_journal = true;
};

// The bindless payload capture takes the Metal device that the queue used to
// supply, so hoisting it to a parameter keeps the one virtual GetMTLDevice()
// out of the per-descriptor loop.
template <typename Packet>
void CaptureCompiledDescriptorTableBindings(
    WMT::Device metal_device, GraphicsBindingSnapshot &snapshot,
    const Packet &packet, const DescriptorTableBindingRecipe &recipe,
    const SubmittedDescriptorCaptureContext &ctx) {
  const bool descriptor_heaps_locked = ctx.descriptor_heaps_locked;
  auto *native_span_store = ctx.native_span_store;
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_MAX_ROOT_COST> tables = {};
  for (const auto &table : packet.root_tables) {
    if (table.root_parameter_index < tables.size())
      tables[table.root_parameter_index] = table.base_descriptor;
  }
  auto *cbv_srv_uav_heap = dynamic_cast<DescriptorHeap *>(
      packet.descriptor_heaps.cbv_srv_uav.ptr());
  auto *sampler_heap = dynamic_cast<DescriptorHeap *>(
      packet.descriptor_heaps.sampler.ptr());

  CaptureCompiledDescriptorJournalCursor(
      snapshot, cbv_srv_uav_heap ? cbv_srv_uav_heap->GetMirror() : nullptr);
  CaptureCompiledDescriptorJournalCursor(
      snapshot, sampler_heap ? sampler_heap->GetMirror() : nullptr);

  if (snapshot.native || snapshot.bindless) {
    snapshot.native_descriptor_recipe = &recipe;
    snapshot.native_descriptor_indices.assign(recipe.entries.size(),
                                              UINT32_MAX);
    if (snapshot.bindless)
      snapshot.compiled_bindless_payloads.resize(recipe.entries.size());
    std::array<bool, D3D12_MAX_ROOT_COST> captured_roots = {};
    for (size_t recipe_index = 0; recipe_index < recipe.entries.size();
         ++recipe_index) {
      const auto &first_entry = recipe.entries[recipe_index];
      if (first_entry.root_index >= captured_roots.size() ||
          captured_roots[first_entry.root_index])
        continue;
      captured_roots[first_entry.root_index] = true;
      const auto root_index = first_entry.root_index;
      const auto base = tables[root_index];
      const auto first_range_type =
          static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(first_entry.range_type);
      const auto heap_type = DescriptorHeapTypeForRange(first_range_type);
      auto *heap = heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                       ? sampler_heap
                       : cbv_srv_uav_heap;
      auto *span_mirror = heap ? heap->GetMirror() : nullptr;
      // The span store lives for exactly one Execute submission and all
      // participating mirrors stay locked during capture. The heap cursor is
      // therefore not part of span identity: only this reflected root window
      // can affect the frozen contents.
      const SubmittedNativeDescriptorSpanKey span_key = {
          &recipe, heap, root_index, base.ptr};

      SubmittedNativeDescriptorSpan local_span;
      SubmittedNativeDescriptorSpan *span = nullptr;
      bool build_span = true;
      if (native_span_store) {
        native_span_store->lookup_count++;
        auto [it, inserted] = native_span_store->spans.try_emplace(span_key);
        span = &it->second;
        build_span = inserted;
        native_span_store->reuse_count += inserted ? 0 : 1;
      } else {
        span = &local_span;
      }
      if (build_span) {
        span->mirror = span_mirror;
        span->content_fingerprint = kGraphicsBindingFingerprintOffset;
        for (size_t i = recipe_index; i < recipe.entries.size(); ++i) {
          const auto &entry = recipe.entries[i];
          if (entry.root_index != root_index)
            continue;
          HashGraphicsBindingValue(span->content_fingerprint, entry.stage);
          HashGraphicsBindingValue(span->content_fingerprint,
                                   entry.range_type);
          const DescriptorRecord *descriptor = nullptr;
          std::optional<DescriptorRecord> descriptor_copy;
          if (base.ptr && descriptor_heaps_locked) {
            descriptor = GetBoundDescriptorRecordInRangeFromLockedHeap(
                heap, base, entry.range_offset, entry.descriptor_index,
                entry.descriptor_count, heap_type);
          } else if (base.ptr) {
            descriptor_copy = GetBoundDescriptorRecordInRangeFromHeap(
                heap, base, entry.range_offset, entry.descriptor_index,
                entry.descriptor_count, heap_type);
            descriptor = descriptor_copy ? &*descriptor_copy : nullptr;
          }
          HashGraphicsBindingValue(span->content_fingerprint,
                                   descriptor != nullptr);
          if (!descriptor)
            continue;
          const auto descriptor_index =
              snapshot.descriptor_records->capture(*descriptor);
          span->descriptor_indices.emplace_back(
              static_cast<uint32_t>(i), descriptor_index);
          if (snapshot.bindless) {
            span->bindless_payloads.emplace_back(
                static_cast<uint32_t>(i),
                dxmt::d3d12::CaptureCompiledBindlessPayload(
                    metal_device, *descriptor, entry.argument));
          }
          if (descriptor->mirror)
            span->used_slots.push_back(descriptor->heap_index);
          HashGraphicsBindingDescriptor(span->content_fingerprint,
                                        *descriptor);
        }
      }
      for (const auto &[index, descriptor_index] : span->descriptor_indices)
        snapshot.native_descriptor_indices[index] = descriptor_index;
      if (snapshot.bindless) {
        for (const auto &[index, payload] : span->bindless_payloads)
          snapshot.compiled_bindless_payloads[index] = payload;
      }
      RecordCompiledDescriptorJournalSpan(snapshot, *span);
      HashGraphicsBindingValue(snapshot.content_fingerprint, root_index);
      HashGraphicsBindingValue(snapshot.content_fingerprint, base.ptr);
      HashGraphicsBindingValue(snapshot.content_fingerprint,
                               span->content_fingerprint);
      HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                               root_index);
      HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                               base.ptr);
      HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                               span->content_fingerprint);
    }
    return;
  } else {
    snapshot.entries.reserve(recipe.entries.size());
  }
  for (size_t recipe_index = 0; recipe_index < recipe.entries.size();
       ++recipe_index) {
    const auto &recipe_entry = recipe.entries[recipe_index];
    if (recipe_entry.root_index >= tables.size()) {
      if (snapshot.native) {
        snapshot.native_descriptor_indices.push_back(UINT32_MAX);
        HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                                 recipe_entry.stage);
        HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                                 recipe_entry.range_type);
        HashGraphicsBindingValue(snapshot.resource_access_fingerprint, false);
      }
      continue;
    }
    const auto base = tables[recipe_entry.root_index];
    if (!base.ptr) {
      if (snapshot.native) {
        snapshot.native_descriptor_indices.push_back(UINT32_MAX);
        HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                                 recipe_entry.stage);
        HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                                 recipe_entry.range_type);
        HashGraphicsBindingValue(snapshot.resource_access_fingerprint, false);
      }
      continue;
    }

    const auto range_type =
        static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(recipe_entry.range_type);
    GraphicsBindingSnapshotEntry entry = {};
    entry.kind = GraphicsBindingSnapshotEntry::Kind::Descriptor;
    entry.stage = static_cast<PipelineStage>(recipe_entry.stage);
    entry.range_type = range_type;
    entry.root_index = recipe_entry.root_index;
    entry.slot = recipe_entry.slot;
    entry.shader_register = recipe_entry.shader_register;
    entry.register_lower_bound = recipe_entry.register_lower_bound;
    entry.root_offset_key = recipe_entry.root_offset_key;
    entry.argument = &recipe_entry.argument;
    entry.debug_kind = DescriptorRangeTypeName(range_type);

    const auto heap_type = DescriptorHeapTypeForRange(range_type);
    auto *heap = heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                     ? sampler_heap
                     : cbv_srv_uav_heap;
    std::optional<DescriptorRecord> descriptor_copy;
    const DescriptorRecord *descriptor = nullptr;
    if (descriptor_heaps_locked) {
      descriptor = GetBoundDescriptorRecordInRangeFromLockedHeap(
          heap, base, recipe_entry.range_offset,
          recipe_entry.descriptor_index, recipe_entry.descriptor_count,
          heap_type);
    } else {
      descriptor_copy = GetBoundDescriptorRecordInRangeFromHeap(
          heap, base, recipe_entry.range_offset,
          recipe_entry.descriptor_index, recipe_entry.descriptor_count,
          heap_type);
      descriptor = descriptor_copy ? &*descriptor_copy : nullptr;
    }
    if (descriptor) {
      entry.has_descriptor = true;
      entry.descriptor_index = snapshot.descriptor_records->capture(*descriptor);
      if (snapshot.bindless)
        entry.bindless_payload =
            dxmt::d3d12::CaptureCompiledBindlessPayload(
                metal_device, *descriptor, recipe_entry.argument);
      entry.debug_size = DescriptorRecordSizeBytes(*descriptor);
      RecordCompiledDescriptorJournalSlot(snapshot, *descriptor);
    }

    if (snapshot.native) {
      snapshot.native_descriptor_indices.push_back(
          entry.has_descriptor ? entry.descriptor_index : UINT32_MAX);
      HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                               recipe_entry.stage);
      HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                               recipe_entry.range_type);
      HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                               entry.has_descriptor);
      if (entry.has_descriptor)
        HashGraphicsBindingDescriptor(snapshot.resource_access_fingerprint,
                                      SnapshotNativeDescriptor(
                                          snapshot, recipe_index));
      continue;
    }

    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.kind);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.stage);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.range_type);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.root_index);
    HashGraphicsBindingValue(snapshot.content_fingerprint, entry.slot);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.shader_register);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.register_lower_bound);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.root_offset_key);
    HashGraphicsBindingValue(snapshot.content_fingerprint, *entry.argument);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             entry.has_descriptor);
    if (entry.has_descriptor)
      HashGraphicsBindingDescriptor(snapshot.content_fingerprint,
                                    SnapshotDescriptor(snapshot, entry));
    snapshot.entries.push_back(std::move(entry));
  }
}

// Resolves the memoized native stage plan and freezes that stage, mirroring
// the shim this capture used to call on the queue.
template <typename Packet>
void BuildFrozenNativeStageBindingForStage(
    const SubmissionBindingContext &binding, const Packet &packet,
    PipelineState &pipeline, RootSignature &root, PipelineStage stage,
    bool compute, SubmittedFrozenNativeDescriptorStore &store,
    CompiledNativeStageBinding &out, GraphicsBindingSnapshot *snapshot) {
  const auto plan = GetNativeRootBaseStagePlan(binding.native_stage_plans,
                                               pipeline, root, stage, compute);
  BuildFrozenNativeStageBinding(packet, pipeline, root, plan.get(), stage,
                                store, out, snapshot);
}

template <typename Packet>
std::shared_ptr<GraphicsBindingSnapshot>
CaptureCompiledDescriptorBindingSnapshot(
    const SubmissionBindingContext &binding,
    const SubmittedDescriptorCaptureContext &ctx, const Packet &packet,
    PipelineState &pipeline, RootSignature *root, bool compute,
    const DescriptorTableBindingRecipe *prepared_recipe) {
  dxmt::perf::ScopedCodeTimer snapshot_build_timer(
      dxmt::PerfCodePath::QueueCaptureDescriptorSnapshotBuild);
  GraphicsBindingSnapshotCaptureStats capture_stats = {};
  auto *native_span_store = ctx.native_span_store;
  std::shared_ptr<GraphicsBindingSnapshot> snapshot;
  if (ctx.snapshot_arena) {
    ctx.snapshot_arena->snapshots.emplace_back(ctx.descriptor_records);
    snapshot = std::shared_ptr<GraphicsBindingSnapshot>(
        ctx.snapshot_arena, &ctx.snapshot_arena->snapshots.back());
  } else {
    snapshot =
        std::make_shared<GraphicsBindingSnapshot>(ctx.descriptor_records);
  }
  snapshot->content_fingerprint = kGraphicsBindingFingerprintOffset;
  snapshot->resource_access_fingerprint = kGraphicsBindingFingerprintOffset;
  snapshot->pipeline_state = packet.pipeline.pipeline_state;
  snapshot->root_signature = packet.pipeline.root_signature;
  snapshot->root_signature_impl = root;
  snapshot->graphics_root_signature_impl = root;
  snapshot->cbv_srv_uav_heap = packet.descriptor_heaps.cbv_srv_uav;
  snapshot->sampler_heap = packet.descriptor_heaps.sampler;
  HashGraphicsBindingPointer(snapshot->resource_access_fingerprint,
                             snapshot->root_signature.ptr());
  HashGraphicsBindingValue(
      snapshot->resource_access_fingerprint,
      packet.pipeline.metadata.binding_layout_fingerprint);
  snapshot->native =
      packet.pipeline.metadata.uses_native_descriptor_table_abi;
  snapshot->bindless = !snapshot->native;
  snapshot->compiled_compute = compute;
  snapshot->compiled_binding_identity_hash =
      HashCompiledDescriptorBindingIdentity(packet, compute);
  snapshot->compiled_binding_program_identity = packet.binding_program.get();
  snapshot->compiled_root_tables_identity = packet.root_tables.identity();
  snapshot->compiled_root_descriptors_identity =
      packet.root_descriptors.identity();
  snapshot->compiled_root_constants_identity =
      packet.root_constants.identity();
  snapshot->compiled_root_tables = packet.root_tables;
  const auto *binding_recipe =
      prepared_recipe
          ? prepared_recipe
          : &GetDescriptorTableBindingRecipe(pipeline, *root, compute);
  snapshot->entries.reserve(
      6 * (packet.root_constants.size() + packet.root_descriptors.size()));
  const bool frozen_native_direct =
      snapshot->native && native_span_store &&
      native_span_store->frozen_native;
  if (!frozen_native_direct) {
    CaptureCompiledDescriptorTableBindings(binding.device, *snapshot, packet,
                                           *binding_recipe, ctx);
  }
  if (frozen_native_direct) {
    snapshot->frozen_native = native_span_store->frozen_native;
    snapshot->frozen_native->direct_packet_count++;
    snapshot->native_descriptor_accesses.reserve(
        binding_recipe->entries.size());
    if (compute) {
      BuildFrozenNativeStageBindingForStage(
          binding, packet, pipeline, *root, PipelineStage::Compute, true,
          *snapshot->frozen_native, snapshot->frozen_native_compute,
          snapshot.get());
    } else {
      BuildFrozenNativeStageBindingForStage(
          binding, packet, pipeline, *root, PipelineStage::Vertex, false,
          *snapshot->frozen_native, snapshot->frozen_native_vertex,
          snapshot.get());
      BuildFrozenNativeStageBindingForStage(
          binding, packet, pipeline, *root, PipelineStage::Pixel, false,
          *snapshot->frozen_native, snapshot->frozen_native_pixel,
          snapshot.get());
    }
    snapshot->frozen_descriptor_table_fingerprint =
        snapshot->content_fingerprint;
  }
  // Root CBV/SRV/UAV and 32-bit constants are part of the submitted binding
  // state. Capture them into the same snapshot so encode-time packBindless
  // does not fall back to empty live cbuf_/resview_ slots on the async path.
  if (!packet.root_constants.empty() || !packet.root_descriptors.empty()) {
    const auto parameters = root->GetParameters();
    for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
      const auto &parameter = parameters[root_index];
      if (parameter.parameter_type ==
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        continue;
      if (parameter.parameter_type ==
          D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        const CompiledCommandRootConstants *captured = nullptr;
        for (const auto &constants : packet.root_constants) {
          if (constants.root_parameter_index == root_index) {
            captured = &constants;
            break;
          }
        }
        CaptureGraphicsRootConstantsValues(
            *snapshot, pipeline, root_index, parameter,
            captured ? captured->values.span() : std::span<const UINT>{},
            captured ? captured->dst_offset : 0, compute);
        if (snapshot->frozen_native &&
            !snapshot->frozen_root_constants[root_index].valid) {
          const auto [offset, length] =
              snapshot->frozen_native->appendRootConstants(
                  parameter.constants.Num32BitValues,
                  captured ? captured->dst_offset : 0,
                  captured ? captured->values.span()
                           : std::span<const UINT>{});
          snapshot->frozen_root_constants[root_index] = {
              offset, length, length != 0};
        }
        capture_stats.root_constants++;
      } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV ||
                 parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV ||
                 parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
        const CompiledCommandRootDescriptor *captured = nullptr;
        for (const auto &descriptor : packet.root_descriptors) {
          if (descriptor.root_parameter_index == root_index &&
              descriptor.parameter_type == parameter.parameter_type) {
            captured = &descriptor;
            break;
          }
        }
        if (!captured)
          continue;
        const auto type =
            parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV
                ? DescriptorRecordType::ConstantBufferView
                : parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV
                      ? DescriptorRecordType::ShaderResourceView
                      : DescriptorRecordType::UnorderedAccessView;
        CaptureGraphicsRootDescriptorAddress(
            *snapshot, pipeline, root_index, parameter, type,
            captured->address, compute);
        capture_stats.root_descriptors++;
      }
    }
  }
  {
    dxmt::perf::ScopedCodeTimer finalize_timer(
        dxmt::PerfCodePath::QueueCaptureDescriptorJournalFinalize);
    FinalizeCompiledDescriptorSnapshot(*snapshot);
  }
  // Keep descriptor-table identity independent from root constants and root
  // descriptors. Those fields have their own slot masks; folding them into
  // this revision turns every root-word change into a complete descriptor
  // table rebind.
  uint64_t descriptor_table_fingerprint =
      kGraphicsBindingFingerprintOffset;
  HashGraphicsBindingPointer(descriptor_table_fingerprint,
                             snapshot->compiled_binding_program_identity);
  if (snapshot->frozen_native) {
    HashGraphicsBindingValue(
        descriptor_table_fingerprint,
        snapshot->frozen_descriptor_table_fingerprint);
  } else {
    HashGraphicsBindingPointer(descriptor_table_fingerprint,
                               snapshot->compiled_root_tables_identity);
    for (size_t index = 0;
         index < snapshot->native_descriptor_indices.size(); ++index) {
      const auto descriptor_index =
          snapshot->native_descriptor_indices[index];
      HashGraphicsBindingValue(descriptor_table_fingerprint,
                               descriptor_index != UINT32_MAX);
      if (descriptor_index != UINT32_MAX)
        HashGraphicsBindingDescriptor(
            descriptor_table_fingerprint,
            snapshot->descriptor_records->records[descriptor_index]);
    }
    for (const auto &entry : snapshot->entries) {
      if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor ||
          (entry.debug_kind &&
           std::strncmp(entry.debug_kind, "root-", 5) == 0))
        continue;
      HashGraphicsBindingValue(descriptor_table_fingerprint,
                               entry.root_index);
      HashGraphicsBindingValue(descriptor_table_fingerprint,
                               entry.range_type);
      HashGraphicsBindingValue(descriptor_table_fingerprint,
                               entry.has_descriptor);
      if (entry.has_descriptor)
        HashGraphicsBindingDescriptor(descriptor_table_fingerprint,
                                      SnapshotDescriptor(*snapshot, entry));
    }
  }
  snapshot->descriptor_content_revision = {
      reinterpret_cast<uintptr_t>(
          snapshot->compiled_binding_program_identity),
      descriptor_table_fingerprint};
  // Resource dependency identity is based on frozen descriptor contents,
  // not the packet/snapshot address. Games commonly rotate descriptor-table
  // handles while binding the same resources; pointer identity turned every
  // such draw into a cache miss and repeated the full hazard walk.
  for (const auto &entry : snapshot->entries) {
    if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor)
      continue;
    HashGraphicsBindingValue(snapshot->resource_access_fingerprint,
                             entry.stage);
    HashGraphicsBindingValue(snapshot->resource_access_fingerprint,
                             entry.range_type);
    HashGraphicsBindingValue(snapshot->resource_access_fingerprint,
                             entry.has_descriptor);
    if (entry.has_descriptor)
      HashGraphicsBindingDescriptor(snapshot->resource_access_fingerprint,
                                    SnapshotDescriptor(*snapshot, entry));
  }
  for (const auto &access : snapshot->native_descriptor_accesses) {
    HashGraphicsBindingValue(snapshot->resource_access_fingerprint,
                             access.stage);
    HashGraphicsBindingValue(snapshot->resource_access_fingerprint,
                             access.range_type);
    HashGraphicsBindingDescriptor(
        snapshot->resource_access_fingerprint,
        snapshot->descriptor_records->records[access.descriptor_index]);
  }
  capture_stats.entries = SnapshotBindingEntryCount(*snapshot);
  capture_stats.descriptors = snapshot->entries.size() +
                              snapshot->native_descriptor_indices.size() +
                              snapshot->native_descriptor_accesses.size();
  for (const auto descriptor_index : snapshot->native_descriptor_indices)
    capture_stats.missing_descriptors +=
        descriptor_index == UINT32_MAX ? 1 : 0;
  for (const auto &entry : snapshot->entries)
    capture_stats.missing_descriptors += entry.has_descriptor ? 0 : 1;
  capture_stats.bindless = snapshot->bindless ? 1 : 0;
  // D3DMetal model: materialize compact bindless tables at capture/submit,
  // not during Metal encode. Pass the live PipelineState so freeze does not
  // depend on COM downcast success for empty stages.
  if (snapshot->bindless)
    dxmt::d3d12::FreezeAllBindlessStageTables(binding.device,
                                              binding.bindless_stage_plans,
                                              *snapshot, pipeline, compute);
  RecordGraphicsBindingSnapshotCapturePerf(capture_stats);
  return snapshot;
}

template <typename Packet>
std::shared_ptr<GraphicsBindingSnapshot>
GetOrCaptureCompiledDescriptorBindingSnapshot(
    const SubmissionBindingContext &binding,
    const SubmittedDescriptorCaptureContext &ctx, ReplayState &state,
    const Packet &packet, PipelineState &pipeline, RootSignature *root,
    bool compute, const DescriptorTableBindingRecipe *prepared_recipe) {
  uint64_t key = 0;
  {
    dxmt::perf::ScopedCodeTimer cache_timer(
        dxmt::PerfCodePath::QueueCaptureDescriptorCacheLookup);
    key = HashCompiledDescriptorBindingIdentity(packet, compute);
    const auto [begin, end] =
        state.compiled_binding_snapshots.equal_range(key);
    for (auto it = begin; it != end; ++it) {
      if (CompiledDescriptorBindingIdentityMatches(
              *it->second, packet, compute) &&
          (!ctx.validate_journal ||
           CompiledDescriptorSnapshotStillCurrent(*it->second)))
        return it->second;
    }
  }
  auto snapshot = CaptureCompiledDescriptorBindingSnapshot(
      binding, ctx, packet, pipeline, root, compute, prepared_recipe);
  state.compiled_binding_snapshots.emplace(key, snapshot);
  return snapshot;
}

} // namespace

CompiledCommandDescriptorSnapshots CaptureSubmittedDescriptorSnapshots(
    const SubmissionBindingContext &binding,
    const CompiledCommandList &compiled,
    std::shared_ptr<SubmittedDescriptorRecordStore> descriptor_records,
    std::shared_ptr<SubmittedNativeDescriptorSpanStore> native_span_store) {
  CompiledCommandDescriptorSnapshots snapshots;
  snapshots.graphics.resize(compiled.graphics_packets.size());
  snapshots.compute.resize(compiled.compute_packets.size());

  // ExecuteCommandLists is the last synchronous boundary at which a
  // descriptor-table generation belongs unambiguously to this submission.
  // Capture only shader-reflected slots and reuse identical packet bindings
  // through the bounded heap journal; Metal encoding may happen much later.
  ReplayState cache = {};
  if (!descriptor_records)
    descriptor_records = std::make_shared<SubmittedDescriptorRecordStore>();
  if (!native_span_store)
    native_span_store =
        std::make_shared<SubmittedNativeDescriptorSpanStore>();
  const auto counters_before = SampleSubmittedDescriptorCaptureCounters(
      *descriptor_records, *native_span_store);
  auto snapshot_arena = std::make_shared<SubmittedBindingSnapshotArena>();
  snapshot_arena->snapshots.reserve(compiled.graphics_packets.size() +
                                    compiled.compute_packets.size());
  const auto submission_mirrors =
      CollectSubmissionDescriptorHeapMirrors(compiled);
  const auto packet_count = compiled.graphics_packets.size() +
                            compiled.compute_packets.size();
  SubmissionDescriptorRecipeCache recipe_cache;
  recipe_cache.Reserve(packet_count);
  // Resolve immutable reflection recipes before taking heap locks. The
  // locks below define the descriptor snapshot boundary only; database/cache
  // work must not extend the writer-blocking interval.
  const auto descriptor_capture_capacity =
      ComputeSubmittedDescriptorCaptureCapacity(compiled, recipe_cache);
  cache.compiled_binding_snapshots.reserve(compiled.graphics_packets.size() +
                                           compiled.compute_packets.size());
  // Reflection entry count is a safe upper bound but FH4 reuses the same
  // descriptor slots across many packets. Reserving the full product made
  // every Execute zero several megabytes of empty hash slots. Start from a
  // bounded estimate; the store remains lossless and grows if the submission
  // genuinely contains more unique slot versions.
  const auto descriptor_unique_estimate = std::min<size_t>(
      descriptor_capture_capacity, std::max<size_t>(16, packet_count * 4));
  if (descriptor_records->records.capacity() <
      descriptor_records->records.size() + descriptor_unique_estimate)
    descriptor_records->records.reserve(
        descriptor_records->records.size() + descriptor_unique_estimate);
  descriptor_records->reserveHeapRecords(
      descriptor_records->heap_record_count + descriptor_unique_estimate);
  // Execute already owns every participating mirror lock, so the per-packet
  // captures below read locked heaps and skip journal revalidation.
  const SubmittedDescriptorCaptureContext capture_context = {
      descriptor_records, snapshot_arena, native_span_store.get(), true,
      false};
  std::vector<DescriptorHeapMirror::ScopedLock> submission_mirror_locks;
  submission_mirror_locks.reserve(submission_mirrors.size());
  for (auto *mirror : submission_mirrors)
    submission_mirror_locks.emplace_back(mirror->AcquireLock());
  for (size_t i = 0; i < compiled.graphics_packets.size(); ++i) {
    const auto &packet = compiled.graphics_packets[i];
    if (packet.compatibility_reason !=
        CompiledCommandFallbackReason::None)
      continue;
    auto *pipeline = packet.pipeline.metadata.pipeline;
    if (!pipeline)
      pipeline = GetPipelineState(packet.pipeline.pipeline_state.ptr());
    auto *root = GetRootSignature(packet.pipeline.root_signature.ptr());
    if (!pipeline || !root ||
        (!pipeline->UsesBindlessMirror() &&
         !packet.pipeline.metadata.uses_native_descriptor_table_abi))
      continue;
    snapshots.graphics[i] = GetOrCaptureCompiledDescriptorBindingSnapshot(
        binding, capture_context, cache, packet, *pipeline, root, false,
        &recipe_cache.Get(*pipeline, *root, false, packet.pipeline.metadata));
    if (snapshots.graphics[i] && compiled.test_telemetry)
      RecordSubmittedDescriptorSnapshotTelemetry(*compiled.test_telemetry,
                                                 *snapshots.graphics[i]);
  }
  for (size_t i = 0; i < compiled.compute_packets.size(); ++i) {
    const auto &packet = compiled.compute_packets[i];
    if (packet.compatibility_reason !=
        CompiledCommandFallbackReason::None)
      continue;
    auto *pipeline = packet.pipeline.metadata.pipeline;
    if (!pipeline)
      pipeline = GetPipelineState(packet.pipeline.pipeline_state.ptr());
    auto *root = GetRootSignature(packet.pipeline.root_signature.ptr());
    if (!pipeline || !root ||
        (!pipeline->UsesBindlessMirror() &&
         !packet.pipeline.metadata.uses_native_descriptor_table_abi))
      continue;
    snapshots.compute[i] = GetOrCaptureCompiledDescriptorBindingSnapshot(
        binding, capture_context, cache, packet, *pipeline, root, true,
        &recipe_cache.Get(*pipeline, *root, true, packet.pipeline.metadata));
    if (snapshots.compute[i] && compiled.test_telemetry)
      RecordSubmittedDescriptorSnapshotTelemetry(*compiled.test_telemetry,
                                                 *snapshots.compute[i]);
  }
  if (compiled.test_telemetry)
    RecordSubmittedDescriptorCaptureTelemetry(
        *compiled.test_telemetry, snapshots, *descriptor_records,
        *native_span_store, counters_before);
  return snapshots;
}

} // namespace dxmt::d3d12
