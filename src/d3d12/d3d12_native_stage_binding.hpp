#pragma once

// Walkers over a NativeRootBaseStagePlan: freezing one stage's descriptor
// table into the immutable native backend, declaring its resource accesses,
// and resolving its argument-buffer root base words.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_native_binding.inc). They stay templates because they
// run over live replay state, compiled packet state and frozen snapshot
// identities alike. Everything the command queue used to supply is now a
// parameter: the memoized stage plan, the Metal device handle and the DXMT
// queue that owns the argument-buffer ring.

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_argument_upload.hpp"
#include "d3d12_binding_fingerprint.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_snapshot_journal.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_frozen_native_descriptor.hpp"
#include "d3d12_native_access_track.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_shader_binding.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Freezes the descriptors, root constants and root descriptors `plan` selects
// for `stage` out of `packet` into `store`, and reports the resulting root base
// offsets in `out`. A null `plan` leaves `out` empty but ready.
//
// Identical descriptor ranges are deduplicated through `store.range_bases`, so
// the key pushed for every captured slot has to carry the full identity of what
// was copied (mirror, heap slot, slot version, backend generation); anything
// left out of the key would let a stale range be reused.
template <typename Packet>
void BuildFrozenNativeStageBinding(const Packet &packet,
                                   const PipelineState &pipeline,
                                   const RootSignature &root,
                                   const NativeRootBaseStagePlan *plan,
                                   PipelineStage stage,
                                   SubmittedFrozenNativeDescriptorStore &store,
                                   CompiledNativeStageBinding &out,
                                   GraphicsBindingSnapshot *snapshot = nullptr) {
  out = {};
  if (!plan) {
    out.ready = true;
    return;
  }
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_MAX_ROOT_COST> tables = {};
  for (const auto &table : packet.root_tables)
    if (table.root_parameter_index < tables.size())
      tables[table.root_parameter_index] = table.base_descriptor;
  auto get_heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return dynamic_cast<DescriptorHeap *>(
        (type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
             ? packet.descriptor_heaps.sampler
             : packet.descriptor_heaps.cbv_srv_uav)
            .ptr());
  };
  const auto *shader = FindShaderForStage(pipeline, stage);
  const auto *cbuffers = shader ? shader->constantBufferInfo() : nullptr;
  const auto cbuffer_count =
      shader ? shader->reflection().NumConstantBuffers : 0;
  const auto *arguments = shader ? shader->resourceArgumentInfo() : nullptr;
  const auto argument_count = shader ? shader->reflection().NumArguments : 0;

  auto build_words = [&](bool cbuffer, uint32_t word_count) {
    std::vector<uint32_t> words(word_count, 0);
    for (const auto &group : plan->groups) {
      if (group.cbuffer != cbuffer ||
          group.root_base_key >= word_count || !group.range_length)
        continue;
      const auto key_index = group.root_base_key;
      const auto range_length = group.range_length;

      struct CapturedSlot {
        uint32_t destination = 0;
        NativeRootBaseStagePlanEntry::Source source =
            NativeRootBaseStagePlanEntry::Source::DescriptorTable;
        const DescriptorRecord *descriptor = nullptr;
        uint64_t root_constant_offset = 0;
        uint32_t root_constant_length = 0;
        D3D12_GPU_VIRTUAL_ADDRESS root_descriptor_address = 0;
        D3D12_DESCRIPTOR_RANGE_TYPE range_type =
            D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
      };
      std::vector<CapturedSlot> captured;
      captured.reserve(group.captured_capacity);
      std::vector<uint64_t> range_key = {range_length};
      range_key.reserve(1 + group.captured_capacity * 6);
      for (const auto entry_index : group.entry_indices) {
        const auto &entry = plan->entries[entry_index];
        if (entry.source ==
            NativeRootBaseStagePlanEntry::Source::RootConstants) {
          const auto found = std::find_if(
              packet.root_constants.begin(), packet.root_constants.end(),
              [&](const auto &constants) {
                return constants.root_parameter_index == entry.root_index;
              });
          const auto parameters = root.GetParameters();
          const auto declared_count =
              entry.root_index < parameters.size()
                  ? parameters[entry.root_index].constants.Num32BitValues
                  : 0;
          const auto [constant_offset, constant_length] =
              store.appendRootConstants(
                  declared_count,
                  found != packet.root_constants.end() ? found->dst_offset
                                                       : 0,
                  found != packet.root_constants.end()
                      ? found->values.span()
                      : std::span<const UINT>{});
          const auto destination = entry.argument_local_start;
          captured.push_back(
              {destination, entry.source, nullptr, constant_offset,
               constant_length, 0, entry.range_type});
          range_key.push_back(destination);
          range_key.push_back(uint64_t(entry.source));
          range_key.push_back(constant_offset);
          range_key.push_back(constant_length);
          if (snapshot && entry.root_index <
                              snapshot->frozen_root_constants.size()) {
            snapshot->frozen_root_constants[entry.root_index] = {
                constant_offset, constant_length, constant_length != 0};
          }
          continue;
        }
        if (entry.source ==
            NativeRootBaseStagePlanEntry::Source::RootDescriptor) {
          const auto parameter_type =
              entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV
                  ? D3D12_ROOT_PARAMETER_TYPE_CBV
                  : entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                        ? D3D12_ROOT_PARAMETER_TYPE_SRV
                        : D3D12_ROOT_PARAMETER_TYPE_UAV;
          const auto found = std::find_if(
              packet.root_descriptors.begin(), packet.root_descriptors.end(),
              [&](const auto &descriptor) {
                return descriptor.root_parameter_index == entry.root_index &&
                       descriptor.parameter_type == parameter_type;
              });
          const auto address =
              found != packet.root_descriptors.end() ? found->address : 0;
          const auto destination = entry.argument_local_start;
          captured.push_back(
              {destination, entry.source, nullptr, 0, 0, address,
               entry.range_type});
          range_key.push_back(destination);
          range_key.push_back(uint64_t(entry.source));
          range_key.push_back(address);
          range_key.push_back(uint64_t(entry.range_type));
          continue;
        }
        auto *heap = get_heap(entry.heap_type);
        const auto base = entry.root_index < tables.size()
                              ? tables[entry.root_index]
                              : D3D12_GPU_DESCRIPTOR_HANDLE{};
        for (uint32_t local = 0;
             local < entry.range_count &&
             entry.descriptor_index + local < entry.descriptor_count;
             ++local) {
          const auto destination = entry.argument_local_start + local;
          const auto *record =
              base.ptr ? GetBoundDescriptorRecordInRangeFromLockedHeap(
                             heap, base, entry.range_offset,
                             entry.descriptor_index + local,
                             entry.descriptor_count, entry.heap_type)
                       : nullptr;
          captured.push_back(
              {destination, entry.source, record, 0, 0, 0,
               entry.range_type});
          if (snapshot) {
            CaptureCompiledDescriptorJournalCursor(
                *snapshot, heap ? heap->GetMirror() : nullptr);
            HashGraphicsBindingValue(snapshot->content_fingerprint, stage);
            HashGraphicsBindingValue(snapshot->content_fingerprint,
                                     entry.root_index);
            HashGraphicsBindingValue(snapshot->content_fingerprint,
                                     entry.range_type);
            HashGraphicsBindingValue(snapshot->content_fingerprint,
                                     destination);
            HashGraphicsBindingValue(snapshot->content_fingerprint,
                                     record != nullptr);
            if (record) {
              RecordCompiledDescriptorJournalSlot(*snapshot, *record);
              HashGraphicsBindingDescriptor(snapshot->content_fingerprint,
                                            *record);
            }
          }
          if (snapshot && record &&
              entry.range_type != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
            const auto *argument =
                entry.cbuffer
                    ? (cbuffers && entry.argument_index < cbuffer_count
                           ? &cbuffers[entry.argument_index]
                           : nullptr)
                    : (arguments && entry.argument_index < argument_count
                           ? &arguments[entry.argument_index]
                           : nullptr);
            if (argument) {
              const auto descriptor_record_index =
                  snapshot->descriptor_records->capture(*record);
              snapshot->native_descriptor_accesses.push_back(
                  CompiledNativeDescriptorAccess{
                      stage,
                      entry.range_type,
                      static_cast<uint16_t>(
                          argument->SM50BindingSlot +
                          entry.argument_local_start + local),
                      *argument,
                      descriptor_record_index});
            }
          }
          range_key.push_back(destination);
          range_key.push_back(
              record ? reinterpret_cast<uintptr_t>(record->mirror) : 0);
          range_key.push_back(record ? record->heap_index : UINT32_MAX);
          range_key.push_back(record ? record->slot_version.epoch : 0);
          range_key.push_back(record ? record->slot_version.sequence : 0);
          range_key.push_back(
              record && record->mirror
                  ? record->mirror->backendResourceTableGeneration()
                  : 0);
        }
      }
      store.range_lookups++;
      auto [range_it, inserted] =
          store.range_bases.try_emplace(std::move(range_key), 0);
      store.range_reuses += inserted ? 0 : 1;
      if (inserted) {
        range_it->second = store.allocateSlots(range_length);
        for (const auto &slot : captured) {
          const auto destination = range_it->second + slot.destination;
          if (slot.source ==
              NativeRootBaseStagePlanEntry::Source::DescriptorTable) {
            if (slot.descriptor)
              CopyFrozenNativeDescriptorSlot(store, destination,
                                             *slot.descriptor);
          } else if (slot.source ==
                     NativeRootBaseStagePlanEntry::Source::RootConstants) {
            if (slot.root_constant_length)
              store.pending_root_constant_descriptors.push_back(
                  {destination, slot.root_constant_offset,
                   slot.root_constant_length});
          } else {
            CopyFrozenNativeRootDescriptorSlot(
                store, destination, slot.root_descriptor_address,
                slot.range_type);
          }
        }
      }
      words[key_index] = range_it->second;
    }
    return store.appendRootWords(words);
  };

  for (const auto &entry : plan->entries) {
    if (entry.root_index >= 64)
      continue;
    auto &mask =
        entry.source == NativeRootBaseStagePlanEntry::Source::RootConstants
            ? (entry.cbuffer ? out.cbuffer_root_constant_mask
                             : out.resource_root_constant_mask)
        : entry.source ==
                NativeRootBaseStagePlanEntry::Source::RootDescriptor
            ? (entry.cbuffer ? out.cbuffer_root_descriptor_mask
                             : out.resource_root_descriptor_mask)
            : (entry.cbuffer ? out.cbuffer_root_table_mask
                             : out.resource_root_table_mask);
    mask |= uint64_t{1} << entry.root_index;
  }
  std::tie(out.cbuffer_root_base_offset, out.cbuffer_root_base_count) =
      build_words(true, plan->max_cbuffer_key_plus_one);
  std::tie(out.resource_root_base_offset, out.resource_root_base_count) =
      build_words(false, plan->max_resource_key_plus_one);
  out.ready = true;
}

// Declares the resource accesses every descriptor `plan` selects for `Stage`
// implies. A null `plan` or a stage without a shader tracks nothing.
template <PipelineStage Stage, typename State>
void TrackNativeStageDescriptorAccesses(WMT::Device device,
                                        ArgumentEncodingContext &enc,
                                        const State &state,
                                        const PipelineState &pipeline,
                                        const NativeRootBaseStagePlan *plan,
                                        bool compute) {
  const auto *shader = FindShaderForStage(pipeline, Stage);
  if (!plan || !shader)
    return;

  const auto *cbuffers = shader->constantBufferInfo();
  const auto cbuffer_count = shader->reflection().NumConstantBuffers;
  const auto *arguments = shader->resourceArgumentInfo();
  const auto argument_count = shader->reflection().NumArguments;
  for (const auto &entry : plan->entries) {
    const auto *argument =
        entry.cbuffer
            ? (cbuffers && entry.argument_index < cbuffer_count
                   ? &cbuffers[entry.argument_index]
                   : nullptr)
            : (arguments && entry.argument_index < argument_count
                   ? &arguments[entry.argument_index]
                   : nullptr);
    if (!argument)
      continue;
    const auto base = GetTableHandle(state, compute, entry.root_index);
    auto *heap = GetBoundDescriptorHeap(state, entry.heap_type);
    if (!base.ptr || !heap)
      continue;
    for (uint32_t local = 0; local < entry.range_count; ++local) {
      if (entry.descriptor_index + local >= entry.descriptor_count)
        break;
      const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
          heap, base, entry.range_offset, entry.descriptor_index + local,
          entry.descriptor_count, entry.heap_type);
      if (descriptor)
        TrackNativeDescriptorAccess<Stage>(device, enc, *descriptor,
                                           entry.range_type, argument);
    }
  }
}

// Resolves the argument-buffer root base word of every root base key `plan`
// declares for the cbuffer (`cbuffer` = true) or resource argument buffer.
//
// The base word is the heap index the shader's first reflected register maps
// to, so an entry whose resolved descriptor sits below its argument-local start
// would underflow the window and is skipped instead.
template <typename State>
[[nodiscard]] std::vector<uint32_t>
BuildNativeRootTableBaseWords(const State &state,
                              const NativeRootBaseStagePlan &plan,
                              bool compute, bool cbuffer) {
  const uint32_t max_key_plus_one =
      cbuffer ? plan.max_cbuffer_key_plus_one
              : plan.max_resource_key_plus_one;
  if (!max_key_plus_one)
    return {};

  std::vector<uint32_t> root_bases(max_key_plus_one, 0);
  for (const auto &entry : plan.entries) {
    if (entry.cbuffer != cbuffer || entry.root_base_key >= max_key_plus_one)
      continue;
    const auto base_handle = GetTableHandle(state, compute, entry.root_index);
    if (!base_handle.ptr)
      continue;
    auto *heap = GetBoundDescriptorHeap(state, entry.heap_type);
    const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
        heap, base_handle, entry.range_offset, entry.descriptor_index,
        entry.descriptor_count, entry.heap_type);
    if (!descriptor || descriptor->heap_index < entry.argument_local_start)
      continue;
    root_bases[entry.root_base_key] =
        descriptor->heap_index - entry.argument_local_start;
  }
  return root_bases;
}

// Resolves and uploads the root base words of one stage in a single step.
template <typename State>
[[nodiscard]] AllocatedArgumentBufferSlice
BuildNativeRootTableBases(::dxmt::CommandQueue &queue,
                          ArgumentEncodingContext &enc, const State &state,
                          const NativeRootBaseStagePlan &plan, bool compute,
                          bool cbuffer) {
  return UploadNativeRootTableBases(
      queue, enc,
      BuildNativeRootTableBaseWords(state, plan, compute, cbuffer));
}

} // namespace dxmt::d3d12
