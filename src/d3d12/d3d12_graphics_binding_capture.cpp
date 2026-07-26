#include "d3d12_graphics_binding_capture.hpp"

#include "d3d12_binding_fingerprint.hpp"
#include "d3d12_compiled_bindless_payload.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_frozen_bindless_stage_tables.hpp"
#include "d3d12_live_binding_capture.hpp"
#include "d3d12_native_stage_binding.hpp"
#include "d3d12_queue_view_binding.hpp"
#include "d3d12_table_recipe_lookup.hpp"

#include <array>
#include <cstring>
#include <memory>
#include <utility>

#include <d3d12.h>

namespace dxmt::d3d12 {

namespace {

// Resolves the root base words of one native stage through the memoized plan,
// mirroring the shim CaptureGraphicsBindingSnapshot used to call on the queue.
std::vector<uint32_t> NativeRootTableBaseWordsForStage(
    SubmissionStagePlanCache<NativeRootBaseStagePlan> &native_plan_cache,
    const ReplayState &state, const PipelineState &pipeline,
    const RootSignature &root, PipelineStage want_stage, bool compute,
    bool cbuffer) {
  const auto plan = GetNativeRootBaseStagePlan(native_plan_cache, pipeline,
                                               root, want_stage, compute);
  return BuildNativeRootTableBaseWords(state, *plan, compute, cbuffer);
}

} // namespace

void CaptureDescriptorTableBindings(WMT::Device device,
                                    GraphicsBindingSnapshot &snapshot,
                                    const ReplayState &state,
                                    const PipelineState &pipeline,
                                    const DescriptorTableBindingRecipe &recipe,
                                    bool compute) {
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_MAX_ROOT_COST> table_cache =
      {};
  std::array<bool, D3D12_MAX_ROOT_COST> table_cached = {};
  DescriptorHeap *cbv_srv_uav_heap = nullptr;
  DescriptorHeap *sampler_heap = nullptr;
  bool cbv_srv_uav_heap_cached = false;
  bool sampler_heap_cached = false;

  auto get_table = [&](UINT root_index) {
    if (root_index < table_cache.size()) {
      if (!table_cached[root_index]) {
        table_cache[root_index] = GetTableHandle(state, compute, root_index);
        table_cached[root_index] = true;
      }
      return table_cache[root_index];
    }
    return GetTableHandle(state, compute, root_index);
  };

  auto get_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
    if (heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER) {
      if (!sampler_heap_cached) {
        sampler_heap = GetBoundDescriptorHeap(state, heap_type);
        sampler_heap_cached = true;
      }
      return sampler_heap;
    }
    if (!cbv_srv_uav_heap_cached) {
      cbv_srv_uav_heap = GetBoundDescriptorHeap(state, heap_type);
      cbv_srv_uav_heap_cached = true;
    }
    return cbv_srv_uav_heap;
  };

  for (const auto &recipe_entry : recipe.entries) {
    const auto base = get_table(recipe_entry.root_index);
    if (!base.ptr)
      continue;

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
    const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
        get_heap(heap_type), base, recipe_entry.range_offset,
        recipe_entry.descriptor_index, recipe_entry.descriptor_count,
        heap_type);
    if (descriptor) {
      entry.has_descriptor = true;
      entry.descriptor_index = snapshot.descriptor_records->capture(*descriptor);
      entry.debug_size = DescriptorRecordSizeBytes(*descriptor);
      // Materialize sampler/texture identity for freeze so encode never
      // re-reads the live heap mirror (D3DMetal materialize-early).
      if (entry.argument)
        entry.bindless_payload = CaptureCompiledBindlessPayload(
            device, *descriptor, *entry.argument);
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

GraphicsBindingSnapshot CaptureGraphicsBindingSnapshot(
    WMT::Device device,
    SubmissionStagePlanCache<BindlessMirrorStagePlan> &bindless_plan_cache,
    SubmissionStagePlanCache<NativeRootBaseStagePlan> &native_plan_cache,
    const ReplayState &state, PipelineState &pipeline,
    GraphicsBindingSnapshotCaptureStats *capture_stats) {
  GraphicsBindingSnapshot snapshot = {};
  snapshot.content_fingerprint = kGraphicsBindingFingerprintOffset;
  snapshot.pipeline_state = state.pipeline_state;
  snapshot.root_signature = state.graphics_root_signature;
  snapshot.root_signature_impl = state.graphics_root_signature_impl;
  snapshot.graphics_root_signature_impl =
      state.graphics_root_signature_impl;
  // Captured DescriptorRecord values contain non-owning pointers to their
  // heap-owned mirrors. Keep both bound heaps alive for every deferred
  // snapshot path, not only the native descriptor-table ABI.
  snapshot.cbv_srv_uav_heap = state.cbv_srv_uav_heap;
  snapshot.sampler_heap = state.sampler_heap;
  snapshot.legacy_identity =
      std::make_unique<GraphicsBindingSnapshotLegacyIdentity>();
  auto &identity = *snapshot.legacy_identity;
  identity.graphics_root_signature_impl = state.graphics_root_signature_impl;
  identity.cbv_srv_uav_heap = state.cbv_srv_uav_heap;
  identity.sampler_heap = state.sampler_heap;
  identity.graphics_tables = state.graphics_tables;
  for (UINT i = 0; i < state.graphics_root_constants.size(); ++i) {
    identity.graphics_root_constants[i] =
        state.graphics_root_constants[i];
    identity.graphics_cbv_roots[i] = state.graphics_cbv_roots[i];
    identity.graphics_srv_roots[i] = state.graphics_srv_roots[i];
    identity.graphics_uav_roots[i] = state.graphics_uav_roots[i];
  }
  for (UINT i = 0; i < state.vertex_buffers.size(); ++i) {
    const auto &vertex_buffer = state.vertex_buffers[i];
    identity.vertex_buffer_views[i].valid = vertex_buffer.has_value();
    if (vertex_buffer)
      identity.vertex_buffer_views[i].view = *vertex_buffer;
  }
  snapshot.native =
      pipeline.GetShaderAbiVersion() ==
      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  HashGraphicsBindingPointer(snapshot.content_fingerprint,
                             snapshot.root_signature.ptr());
  HashGraphicsBindingPointer(snapshot.content_fingerprint,
                             snapshot.pipeline_state.ptr());
  HashGraphicsBindingPointer(snapshot.content_fingerprint, &pipeline);
  HashGraphicsBindingPointer(snapshot.content_fingerprint,
                             snapshot.cbv_srv_uav_heap.ptr());
  HashGraphicsBindingPointer(snapshot.content_fingerprint,
                             snapshot.sampler_heap.ptr());
  for (UINT i = 0; i < identity.graphics_tables.size(); i++) {
    if (!identity.graphics_tables[i].ptr)
      continue;
    HashGraphicsBindingValue(snapshot.content_fingerprint, i);
    HashGraphicsBindingValue(snapshot.content_fingerprint,
                             identity.graphics_tables[i].ptr);
  }

  auto *root = state.graphics_root_signature_impl;
  if (root && snapshot.native) {
    snapshot.native_vertex.cbuffer_root_bases =
        NativeRootTableBaseWordsForStage(native_plan_cache, state, pipeline,
                                         *root, PipelineStage::Vertex, false,
                                         true);
    snapshot.native_vertex.resource_root_bases =
        NativeRootTableBaseWordsForStage(native_plan_cache, state, pipeline,
                                         *root, PipelineStage::Vertex, false,
                                         false);
    snapshot.native_pixel.cbuffer_root_bases =
        NativeRootTableBaseWordsForStage(native_plan_cache, state, pipeline,
                                         *root, PipelineStage::Pixel, false,
                                         true);
    snapshot.native_pixel.resource_root_bases =
        NativeRootTableBaseWordsForStage(native_plan_cache, state, pipeline,
                                         *root, PipelineStage::Pixel, false,
                                         false);
  }
  if (root && !snapshot.native) {
    snapshot.bindless = pipeline.UsesBindlessMirror();

    // Capture descriptor table entries for every ABI. For bindless this is
    // the materialize-early step: freeze compact tex/sampler windows and
    // root_offsets from the captured recipe so encode only uploads (GPTK /
    // D3DMetal model). Skipping capture forced encode-time live rebuilds
    // through BuildBindlessRootOffsets on a GraphicsBindingSnapshot that
    // has no graphics_tables, producing MissingTable root-offset gaps.
    CaptureDescriptorTableBindings(
        device, snapshot, state, pipeline,
        GetDescriptorTableBindingRecipe(pipeline, *root, false), false);

    const auto parameters = root->GetParameters();
    for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
      const auto &parameter = parameters[root_index];
      if (parameter.parameter_type ==
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        continue;
      if (parameter.parameter_type ==
          D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        CaptureGraphicsRootConstants(snapshot, state, pipeline, root_index,
                                     parameter);
      } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
        CaptureGraphicsRootDescriptor(
            snapshot, state, pipeline, root_index, parameter,
            DescriptorRecordType::ConstantBufferView);
      } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
        CaptureGraphicsRootDescriptor(
            snapshot, state, pipeline, root_index, parameter,
            DescriptorRecordType::ShaderResourceView);
      } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
        CaptureGraphicsRootDescriptor(
            snapshot, state, pipeline, root_index, parameter,
            DescriptorRecordType::UnorderedAccessView);
      }
    }

    if (snapshot.bindless)
      FreezeAllBindlessStageTables(device, bindless_plan_cache, snapshot,
                                   pipeline, /*compute=*/false);
  }

  CaptureGraphicsVertexBuffers(snapshot, state, pipeline.GetGraphicsState());
  snapshot.resource_access_fingerprint =
      kGraphicsBindingFingerprintOffset;
  HashGraphicsBindingPointer(snapshot.resource_access_fingerprint,
                             snapshot.root_signature.ptr());
  HashGraphicsBindingPointer(snapshot.resource_access_fingerprint,
                             snapshot.pipeline_state.ptr());
  for (const auto &entry : snapshot.entries) {
    if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor)
      continue;
    HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                             entry.stage);
    HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                             entry.range_type);
    HashGraphicsBindingValue(snapshot.resource_access_fingerprint,
                             entry.has_descriptor);
    if (entry.has_descriptor)
      HashGraphicsBindingDescriptor(snapshot.resource_access_fingerprint,
                                    SnapshotDescriptor(snapshot, entry));
  }
  if (capture_stats) {
    capture_stats->entries += snapshot.entries.size();
    capture_stats->vertex_buffers += snapshot.vertex_buffers.size();
    capture_stats->bindless += snapshot.bindless ? 1 : 0;
    for (const auto &entry : snapshot.entries) {
      if (entry.kind == GraphicsBindingSnapshotEntry::Kind::RootConstants) {
        capture_stats->root_constants++;
        continue;
      }
      capture_stats->descriptors++;
      if (!entry.has_descriptor)
        capture_stats->missing_descriptors++;
      if (entry.debug_kind &&
          (!std::strcmp(entry.debug_kind, "root-cbv") ||
           !std::strcmp(entry.debug_kind, "root-srv") ||
           !std::strcmp(entry.debug_kind, "root-uav")))
        capture_stats->root_descriptors++;
    }
  }
  return snapshot;
}

} // namespace dxmt::d3d12
