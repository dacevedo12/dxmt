#pragma once

// Namespace-level replay binding / descriptor-snapshot types.
//
// These definitions used to be nested inside the anonymous-namespace class
// CommandQueueImpl (d3d12_command_queue_replay_types.inc). None of them names
// the queue class, so hoisting them to dxmt::d3d12 lets binding capture and
// encode helpers be moved into independently compiled translation units.

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_command_list.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"
#include "dxmt_descriptor_revision.hpp"
#include "dxmt_sampler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct ReplayRootConstantsSlot {
  bool valid = false;
  std::vector<UINT> values;
};

struct ReplayRootDescriptorSlot {
  bool valid = false;
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

struct SubmittedDescriptorRecordKey {
  DescriptorHeapMirror *mirror = nullptr;
  UINT heap_index = 0;
  dxmt::DescriptorSlotVersion version = {};

  bool operator==(const SubmittedDescriptorRecordKey &) const = default;
};

struct SubmittedDescriptorRecordKeyHash {
  size_t operator()(const SubmittedDescriptorRecordKey &key) const {
    auto hash = std::hash<DescriptorHeapMirror *>{}(key.mirror);
    auto mix = [&](uint64_t value) {
      hash ^= std::hash<uint64_t>{}(value) + 0x9e3779b97f4a7c15ull +
              (hash << 6) + (hash >> 2);
    };
    mix(key.heap_index);
    mix(key.version.epoch);
    mix(key.version.sequence);
    return hash;
  }
};

// A submission freezes each shader-visible heap slot once. Snapshot entries
// retain only an index into this store, avoiding thousands of repeated COM
// references and full DescriptorRecord copies for adjacent packets that use
// the same slot/version.
struct SubmittedDescriptorRecordStore {
  struct HeapRecordSlot {
    SubmittedDescriptorRecordKey key = {};
    uint32_t index_plus_one = 0;
  };

  void reserveHeapRecords(size_t count) {
    size_t capacity = 16;
    while (capacity < count * 2)
      capacity *= 2;
    if (capacity <= heap_record_slots.size())
      return;
    rehashHeapRecords(capacity);
  }

  uint32_t capture(const DescriptorRecord &record) {
    capture_count++;
    if (record.mirror) {
      const SubmittedDescriptorRecordKey key = {
          record.mirror, record.heap_index, record.slot_version};
      if (heap_record_slots.empty() ||
          (heap_record_count + 1) * 2 > heap_record_slots.size())
        reserveHeapRecords(std::max<size_t>(16, heap_record_count + 1));
      auto &slot = findHeapRecordSlot(key);
      if (slot.index_plus_one) {
        reuse_count++;
        return slot.index_plus_one - 1;
      }
      const auto index = static_cast<uint32_t>(records.size());
      records.push_back(record);
      slot.key = key;
      slot.index_plus_one = index + 1;
      heap_record_count++;
      return index;
    }
    const auto index = static_cast<uint32_t>(records.size());
    records.push_back(record);
    return index;
  }

private:
  HeapRecordSlot &findHeapRecordSlot(
      const SubmittedDescriptorRecordKey &key) {
    const auto mask = heap_record_slots.size() - 1;
    auto index = SubmittedDescriptorRecordKeyHash{}(key) & mask;
    while (heap_record_slots[index].index_plus_one &&
           !(heap_record_slots[index].key == key))
      index = (index + 1) & mask;
    return heap_record_slots[index];
  }

  void rehashHeapRecords(size_t capacity) {
    std::vector<HeapRecordSlot> old = std::move(heap_record_slots);
    heap_record_slots.clear();
    heap_record_slots.resize(capacity);
    for (const auto &slot : old) {
      if (!slot.index_plus_one)
        continue;
      findHeapRecordSlot(slot.key) = slot;
    }
  }

public:

  std::vector<DescriptorRecord> records;
  std::vector<HeapRecordSlot> heap_record_slots;
  uint32_t heap_record_count = 0;
  uint32_t capture_count = 0;
  uint32_t reuse_count = 0;
};

// D3DMetal-style bindless materialization: freeze CPU-side tables at capture /
// submit time (descriptor contents already resolved), then encode only uploads
// and binds. Encode must not re-walk recipes or rebuild compact windows.
struct FrozenBindlessStageTables {
  std::vector<uint32_t> root_offsets;
  std::vector<uint64_t> texture_window;
  std::vector<uint64_t> sampler_window;
  uint32_t texture_field_pairs = 0;
  bool valid = false;

  bool empty() const {
    return !valid ||
           (root_offsets.empty() && texture_window.empty() &&
            sampler_window.empty());
  }
};

struct FrozenBindlessDescriptorPayload {
  enum class Kind : uint8_t {
    None,
    Sampler,
    TextureDynamicPatch,
  };

  Kind kind = Kind::None;
  // Sampler handles are allocation identities, not descriptor values. Keep
  // the submitted sampler alive instead of consulting the mutable heap
  // mirror when the command buffer is encoded later.
  Rc<dxmt::Sampler> sampler;
};

struct GraphicsBindingSnapshotEntry {
  enum class Kind : uint8_t { Descriptor, RootConstants };

  Kind kind = Kind::Descriptor;
  PipelineStage stage = PipelineStage::Vertex;
  D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  UINT root_index = 0;
  UINT slot = 0;
  UINT shader_register = 0;
  UINT register_space = 0;
  UINT register_lower_bound = 0;
  UINT root_offset_key = 0;
  UINT64 debug_size = 0;
  D3D12_GPU_VIRTUAL_ADDRESS debug_address = 0;
  const char *debug_kind = nullptr;
  bool has_descriptor = false;
  uint32_t descriptor_index = UINT32_MAX;
  FrozenBindlessDescriptorPayload bindless_payload;
  const DXMT12_MTL4_SHADER_ARGUMENT *argument = nullptr;
  std::vector<UINT> constants;
  UINT constant_count = 0;
};

struct GraphicsVertexBufferBindingSnapshot {
  UINT slot = 0;
  UINT stride = 0;
  UINT64 offset = 0;
  Rc<Buffer> buffer;
};

struct GraphicsRootDescriptorBindingIdentity {
  bool valid = false;
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

struct GraphicsVertexBufferBindingIdentity {
  bool valid = false;
  D3D12_VERTEX_BUFFER_VIEW view = {};
};

struct GraphicsBindingSnapshotLegacyIdentity {
  RootSignature *graphics_root_signature_impl = nullptr;
  Com<ID3D12DescriptorHeap> cbv_srv_uav_heap;
  Com<ID3D12DescriptorHeap> sampler_heap;
  std::array<D3D12_GPU_DESCRIPTOR_HANDLE, 64> graphics_tables = {};
  std::array<ReplayRootConstantsSlot, 64> graphics_root_constants = {};
  std::array<ReplayRootDescriptorSlot, 64> graphics_cbv_roots = {};
  std::array<ReplayRootDescriptorSlot, 64> graphics_srv_roots = {};
  std::array<ReplayRootDescriptorSlot, 64> graphics_uav_roots = {};
  std::array<GraphicsVertexBufferBindingIdentity, 32> vertex_buffer_views =
      {};
};

struct NativeStageBindingToken {
  std::vector<uint32_t> cbuffer_root_bases;
  std::vector<uint32_t> resource_root_bases;
};

struct CompiledNativeDescriptorAccess {
  PipelineStage stage = PipelineStage::Vertex;
  D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  uint16_t slot = 0;
  DXMT12_MTL4_SHADER_ARGUMENT argument = {};
  uint32_t descriptor_index = UINT32_MAX;
};

struct DescriptorJournalSnapshotToken {
  DescriptorHeapMirror *mirror = nullptr;
  uint64_t cursor = 0;
  std::vector<uint32_t> used_slots;
  bool valid = true;
  bool cursor_captured = false;
};

struct DescriptorTableBindingRecipeEntry {
  uint16_t root_index = 0;
  uint16_t range_index = 0;
  uint16_t stage = 0;
  uint16_t slot = 0;
  uint32_t range_offset = 0;
  uint32_t descriptor_index = 0;
  uint32_t descriptor_count = 0;
  uint32_t range_type = 0;
  uint32_t shader_register = 0;
  uint32_t register_lower_bound = 0;
  uint32_t root_offset_key = 0;
  DXMT12_MTL4_SHADER_ARGUMENT argument = {};
};

struct DescriptorTableBindingRecipe {
  std::vector<DescriptorTableBindingRecipeEntry> entries;
};

// Immutable, compact native descriptor backend for one Execute submission.
// Root bases are remapped into these arrays, so only reflected descriptor
// ranges are copied and later application writes cannot alter queued work.
struct SubmittedFrozenNativeDescriptorStore {
  struct PendingRootConstantDescriptor {
    uint32_t descriptor_slot = 0;
    uint64_t byte_offset = 0;
    uint64_t byte_length = 0;
  };
  struct RangeKeyHash {
    size_t operator()(const std::vector<uint64_t> &key) const {
      size_t hash = 1469598103934665603ull;
      for (const auto value : key)
        hash ^= std::hash<uint64_t>{}(value) + 0x9e3779b97f4a7c15ull +
                (hash << 6) + (hash >> 2);
      return hash;
    }
  };

  uint32_t allocateSlots(uint32_t count) {
    const auto base = static_cast<uint32_t>(descriptor_table.size());
    descriptor_table.resize(size_t(base) + count);
    buffer_records.resize(size_t(base) + count);
    buffer_resources.resize(1u + (size_t(base) + count) * 2u);
    return base;
  }

  std::pair<uint64_t, uint32_t>
  appendRootWords(const std::vector<uint32_t> &words) {
    if (words.empty())
      return {};
    root_word_lookups++;
    if (const auto found = root_word_offsets.find(words);
        found != root_word_offsets.end()) {
      root_word_reuses++;
      return {found->second, static_cast<uint32_t>(words.size())};
    }
    while (root_words.size() & 3u)
      root_words.push_back(0);
    const auto offset = uint64_t(root_words.size()) * sizeof(uint32_t);
    root_words.insert(root_words.end(), words.begin(), words.end());
    root_word_offsets.emplace(words, offset);
    return {offset, static_cast<uint32_t>(words.size())};
  }

  std::pair<uint64_t, uint32_t>
  appendRootConstants(uint32_t declared_count, uint32_t dst_offset,
                      std::span<const uint32_t> values) {
    // The previous 32-bit `dst_offset + values.size()` wrapped for a hostile
    // offset, making `word_count` smaller than `dst_offset` so the std::copy
    // below wrote past the end of `words`. Evaluate the end in 64-bit.
    //
    // The bound is D3D12_MAX_ROOT_COST rather than `declared_count`, matching
    // BindRootConstants() in d3d12_root_parameter_apply.hpp: a command list may
    // legitimately set more words than the root signature declares, and that
    // tolerance has to be the same on every path or the same draw renders
    // differently depending on whether it took the frozen-native or the live
    // one. Root signature creation already rejects a total root cost above
    // D3D12_MAX_ROOT_COST (RootSignatureCostWithinLimit), so this is a verified
    // hard ceiling and the allocation below is bounded either way.
    if (uint64_t(dst_offset) + values.size() > D3D12_MAX_ROOT_COST) {
      static std::atomic<uint32_t> log_count = 0;
      if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
        WARN("D3D12CommandQueue: root constants destination range exceeds the"
             " maximum root cost; dropping the values"
             " declaredNum32BitValues=", declared_count,
             " destOffsetIn32BitValues=", dst_offset,
             " valueCount=", values.size());
      }
      values = {};
      dst_offset = 0;
    }
    const auto word_count =
        values.empty()
            ? declared_count
            : std::max<uint32_t>(declared_count,
                                 dst_offset + uint32_t(values.size()));
    if (!word_count)
      return {};
    std::vector<uint32_t> words(word_count, 0);
    if (!values.empty())
      std::copy(values.begin(), values.end(),
                words.begin() + dst_offset);
    root_constant_lookups++;
    if (const auto found = root_constant_offsets.find(words);
        found != root_constant_offsets.end()) {
      root_constant_reuses++;
      return {found->second, word_count * uint32_t(sizeof(uint32_t))};
    }
    constexpr size_t kConstantAlignmentWords =
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT / sizeof(uint32_t);
    while (root_words.size() % kConstantAlignmentWords)
      root_words.push_back(0);
    const auto offset = uint64_t(root_words.size()) * sizeof(uint32_t);
    root_words.insert(root_words.end(), words.begin(), words.end());
    root_constant_offsets.emplace(std::move(words), offset);
    return {offset, word_count * uint32_t(sizeof(uint32_t))};
  }

  std::vector<DescriptorTableEntry> descriptor_table;
  std::vector<BufferDescriptorRecord> buffer_records;
  std::vector<BufferResourceTableEntry> buffer_resources =
      std::vector<BufferResourceTableEntry>(1);
  std::vector<WMT::Reference<WMT::Resource>> retained_resources;
  std::unordered_set<uint64_t> retained_resource_handles;
  std::vector<uint32_t> root_words;
  std::vector<PendingRootConstantDescriptor>
      pending_root_constant_descriptors;
  std::unordered_map<std::vector<uint64_t>, uint32_t, RangeKeyHash>
      range_bases;
  std::map<std::vector<uint32_t>, uint64_t> root_word_offsets;
  std::map<std::vector<uint32_t>, uint64_t> root_constant_offsets;
  uint64_t direct_packet_count = 0;
  uint64_t range_lookups = 0;
  uint64_t range_reuses = 0;
  uint64_t root_word_lookups = 0;
  uint64_t root_word_reuses = 0;
  uint64_t root_constant_lookups = 0;
  uint64_t root_constant_reuses = 0;
  std::atomic<uint64_t> retained_sequence = UINT64_MAX;
  std::shared_ptr<dxmt::LifetimeResidencyRegistration>
      root_base_residency;
  WMT::Reference<WMT::Buffer> descriptor_table_buffer;
  WMT::Reference<WMT::Buffer> buffer_record_buffer;
  WMT::Reference<WMT::Buffer> buffer_resource_table_buffer;
  WMT::Reference<WMT::Buffer> root_base_buffer;
  uint64_t root_base_gpu_address = 0;
  uint64_t descriptor_table_buffer_offset = 0;
  uint64_t buffer_record_buffer_offset = 0;
  uint64_t buffer_resource_table_buffer_offset = 0;
  bool finalized = false;
  bool ready = false;
};

bool FinalizeFrozenNativeDescriptorStore(
    SubmittedFrozenNativeDescriptorStore &store, WMT::Device device,
    dxmt::CommandQueue &queue);

struct GraphicsBindingSnapshot {
  GraphicsBindingSnapshot()
      : descriptor_records(
            std::make_shared<SubmittedDescriptorRecordStore>()) {}

  explicit GraphicsBindingSnapshot(
      std::shared_ptr<SubmittedDescriptorRecordStore> records)
      : descriptor_records(
            records ? std::move(records)
                    : std::make_shared<SubmittedDescriptorRecordStore>()) {}

  Com<ID3D12PipelineState> pipeline_state;
  Com<ID3D12RootSignature> root_signature;
  Com<ID3D12DescriptorHeap> cbv_srv_uav_heap;
  Com<ID3D12DescriptorHeap> sampler_heap;
  RootSignature *root_signature_impl = nullptr;
  RootSignature *graphics_root_signature_impl = nullptr;
  std::shared_ptr<SubmittedDescriptorRecordStore> descriptor_records;
  CompiledImmutableVector<GraphicsBindingSnapshotEntry> entries;
  // Compiled descriptor-table packets need the immutable recipe metadata plus
  // one frozen record index per reflected descriptor. Keeping those two
  // arrays separate avoids rebuilding the much larger generic entry object
  // for every draw while preserving the exact submitted descriptor values.
  const DescriptorTableBindingRecipe *native_descriptor_recipe = nullptr;
  std::vector<uint32_t> native_descriptor_indices;
  // One immutable bindless materialization recipe per descriptor recipe
  // entry. This stays aligned with native_descriptor_indices even for null
  // descriptors so encode never needs to inspect the live heap mirror.
  std::vector<FrozenBindlessDescriptorPayload>
      compiled_bindless_payloads;
  // Native direct packets already walk the stage plan while freezing the
  // submitted descriptor backend. Keep only the metadata required by hazard
  // tracking and PS MSAA demotion instead of scanning the generic recipe.
  std::vector<CompiledNativeDescriptorAccess> native_descriptor_accesses;
  std::vector<GraphicsVertexBufferBindingSnapshot> vertex_buffers;
  uint32_t vertex_slot_mask = 0;
  uint64_t content_fingerprint = 0;
  uint64_t frozen_descriptor_table_fingerprint = 0;
  uint64_t resource_access_fingerprint = 0;
  dxmt::DescriptorContentRevision descriptor_content_revision = {};
  // The interpreted replay cache needs value-by-value identity matching.
  // Submitted compiled packets instead carry immutable store identities, so
  // avoid constructing several kilobytes of legacy arrays for every draw.
  std::unique_ptr<GraphicsBindingSnapshotLegacyIdentity> legacy_identity;
  // Bindless tables are frozen at capture/submit time. Encode only uploads
  // those immutable tables (D3DMetal: materialize early, bind late).
  bool bindless = false;
  bool native = false;
  NativeStageBindingToken native_vertex;
  NativeStageBindingToken native_pixel;
  std::shared_ptr<SubmittedFrozenNativeDescriptorStore> frozen_native;
  CompiledNativeStageBinding frozen_native_vertex;
  CompiledNativeStageBinding frozen_native_pixel;
  CompiledNativeStageBinding frozen_native_compute;
  struct FrozenRootConstant {
    uint64_t offset = 0;
    uint32_t length = 0;
    bool valid = false;
  };
  std::array<FrozenRootConstant, 64> frozen_root_constants = {};
  bool compiled_compute = false;
  // Full packet binding identity (tables + root CBV/SRV/UAV + root
  // constants). Used to prevent reuse across draws that share table bases
  // but differ in root descriptor addresses or constants.
  uint64_t compiled_binding_identity_hash = 0;
  const void *compiled_binding_program_identity = nullptr;
  const void *compiled_root_tables_identity = nullptr;
  const void *compiled_root_descriptors_identity = nullptr;
  const void *compiled_root_constants_identity = nullptr;
  CompiledImmutableVector<CompiledCommandRootDescriptorTable>
      compiled_root_tables;
  std::array<DescriptorJournalSnapshotToken, 2> descriptor_journals = {};
  FrozenBindlessStageTables frozen_bindless_vertex;
  FrozenBindlessStageTables frozen_bindless_pixel;
  FrozenBindlessStageTables frozen_bindless_compute;
};

struct SubmittedBindingSnapshotArena {
  std::vector<GraphicsBindingSnapshot> snapshots;
};

inline const DescriptorRecord &SnapshotDescriptor(
    const GraphicsBindingSnapshot &snapshot,
    const GraphicsBindingSnapshotEntry &entry) {
  assert(snapshot.descriptor_records);
  assert(entry.descriptor_index < snapshot.descriptor_records->records.size());
  return snapshot.descriptor_records->records[entry.descriptor_index];
}

inline const DescriptorRecord &SnapshotNativeDescriptor(
    const GraphicsBindingSnapshot &snapshot, size_t index) {
  assert(index < snapshot.native_descriptor_indices.size());
  const auto descriptor_index = snapshot.native_descriptor_indices[index];
  assert(descriptor_index != UINT32_MAX);
  assert(snapshot.descriptor_records);
  assert(descriptor_index < snapshot.descriptor_records->records.size());
  return snapshot.descriptor_records->records[descriptor_index];
}

template <typename Fn>
void ForEachCompiledDescriptorTableEntry(
    const GraphicsBindingSnapshot &snapshot, Fn &&fn) {
  if (!snapshot.native_descriptor_recipe)
    return;
  const auto &entries = snapshot.native_descriptor_recipe->entries;
  assert(entries.size() == snapshot.native_descriptor_indices.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    if (snapshot.native_descriptor_indices[i] == UINT32_MAX)
      continue;
    fn(entries[i], SnapshotNativeDescriptor(snapshot, i));
  }
}

inline size_t SnapshotBindingEntryCount(
    const GraphicsBindingSnapshot &snapshot) {
  return snapshot.entries.size() + snapshot.native_descriptor_indices.size() +
         snapshot.native_descriptor_accesses.size();
}

struct CompiledCommandDescriptorSnapshots {
  std::vector<std::shared_ptr<GraphicsBindingSnapshot>> graphics;
  std::vector<std::shared_ptr<GraphicsBindingSnapshot>> compute;
};

struct GraphicsBindingSnapshotCaptureStats {
  uint64_t entries = 0;
  uint64_t descriptors = 0;
  uint64_t missing_descriptors = 0;
  uint64_t root_descriptors = 0;
  uint64_t root_constants = 0;
  uint64_t vertex_buffers = 0;
  uint64_t bindless = 0;
};

struct SubmittedNativeDescriptorSpanKey {
  const DescriptorTableBindingRecipe *recipe = nullptr;
  DescriptorHeap *heap = nullptr;
  UINT root_index = 0;
  UINT64 base = 0;

  bool operator==(const SubmittedNativeDescriptorSpanKey &) const = default;
};

struct SubmittedNativeDescriptorSpanKeyHash {
  size_t operator()(const SubmittedNativeDescriptorSpanKey &key) const {
    size_t hash = std::hash<const void *>{}(key.recipe);
    auto mix = [&](uint64_t value) {
      hash ^= std::hash<uint64_t>{}(value) + 0x9e3779b97f4a7c15ull +
              (hash << 6) + (hash >> 2);
    };
    mix(reinterpret_cast<uintptr_t>(key.heap));
    mix(key.root_index);
    mix(key.base);
    return hash;
  }
};

struct SubmittedNativeDescriptorSpan {
  std::vector<std::pair<uint32_t, uint32_t>> descriptor_indices;
  std::vector<std::pair<uint32_t, FrozenBindlessDescriptorPayload>>
      bindless_payloads;
  DescriptorHeapMirror *mirror = nullptr;
  std::vector<uint32_t> used_slots;
  uint64_t content_fingerprint = 0;
};

struct SubmittedNativeDescriptorSpanStore {
  std::unordered_map<SubmittedNativeDescriptorSpanKey,
                     SubmittedNativeDescriptorSpan,
                     SubmittedNativeDescriptorSpanKeyHash>
      spans;
  uint64_t lookup_count = 0;
  uint64_t reuse_count = 0;
  std::shared_ptr<SubmittedFrozenNativeDescriptorStore> frozen_native;
};

} // namespace dxmt::d3d12
