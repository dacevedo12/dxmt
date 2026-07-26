#pragma once

#include "Metal.hpp"
#include "winemetal.h"
#include "dxmt_descriptor_mirror.hpp"
#include "dxmt_descriptor_revision.hpp"
#include "d3d12_descriptor_journal.hpp"
#include "rc/util_rc_ptr.hpp"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace dxmt {
class Sampler;
class Texture;
class TextureAllocation;
struct TextureViewKey;
}

namespace dxmt::d3d12 {

enum class DescriptorBackendSlotKind : uint32_t {
  Empty,
  Buffer,
  Texture,
  Sampler,
};

enum BufferDescriptorRecordFlag : uint32_t {
  BufferDescriptorRecordFlagValid = 1u << 0,
  BufferDescriptorRecordFlagCBV = 1u << 1,
  BufferDescriptorRecordFlagSRV = 1u << 2,
  BufferDescriptorRecordFlagUAV = 1u << 3,
  BufferDescriptorRecordFlagRaw = 1u << 4,
  BufferDescriptorRecordFlagTyped = 1u << 5,
  BufferDescriptorRecordFlagStructured = 1u << 6,
  BufferDescriptorRecordFlagCounter = 1u << 7,
  BufferDescriptorRecordFlagTextureView = 1u << 8,
};

inline constexpr uint32_t kNullDescriptorResourceIndex = 0;

struct DescriptorTableEntry {
  uint64_t gpu_va = 0;
  uint64_t texture_view_id = 0;
  uint64_t metadata = 0;
};

static_assert(sizeof(DescriptorTableEntry) == sizeof(uint64_t) * 3);

struct BufferDescriptorRecord {
  uint32_t resource_index = kNullDescriptorResourceIndex;
  uint32_t flags = 0;
  uint64_t byte_offset = 0;
  uint64_t byte_size = 0;
  uint32_t stride = 0;
  uint32_t format = 0;
  uint32_t counter_resource_index = kNullDescriptorResourceIndex;
  uint64_t counter_offset = 0;
};

static_assert(sizeof(BufferDescriptorRecord) == 48);

struct BufferResourceTableEntry {
  uint64_t gpu_address = 0;
  uint64_t byte_size = 0;
  uint64_t allocation_handle = 0;
  uint64_t generation = 0;
};

static_assert(sizeof(BufferResourceTableEntry) == 32);

struct DescriptorBackendResourceRecord {
  WMT::Reference<WMT::Resource> allocation;
  uint64_t resource_identity = 0;
  uint64_t gpu_address = 0;
  uint64_t byte_size = 0;
  uint64_t allocation_handle = 0;
  uint64_t generation = 0;
};

enum NativeDescriptorDiagnosticFlag : uint32_t {
  NativeDescriptorDiagnosticNone = 0,
  NativeDescriptorDiagnosticInvalidFlags = 1u << 0,
  NativeDescriptorDiagnosticMissingResource = 1u << 1,
  NativeDescriptorDiagnosticMissingAllocation = 1u << 2,
  NativeDescriptorDiagnosticZeroGpuAddress = 1u << 3,
  NativeDescriptorDiagnosticZeroGeneration = 1u << 4,
  NativeDescriptorDiagnosticOffsetOutOfBounds = 1u << 5,
  NativeDescriptorDiagnosticSizeOutOfBounds = 1u << 6,
};

inline uint32_t DiagnoseNativeBufferDescriptor(
    const BufferDescriptorRecord &descriptor,
    const std::optional<DescriptorBackendResourceRecord> &resource) {
  // A completely zero record is the canonical null descriptor.
  if (descriptor.resource_index == kNullDescriptorResourceIndex &&
      descriptor.flags == 0)
    return NativeDescriptorDiagnosticNone;

  uint32_t result = NativeDescriptorDiagnosticNone;
  if (!(descriptor.flags & BufferDescriptorRecordFlagValid))
    result |= NativeDescriptorDiagnosticInvalidFlags;
  if (descriptor.resource_index == kNullDescriptorResourceIndex || !resource) {
    result |= NativeDescriptorDiagnosticMissingResource;
    return result;
  }
  if (!resource->allocation)
    result |= NativeDescriptorDiagnosticMissingAllocation;
  if (!resource->gpu_address)
    result |= NativeDescriptorDiagnosticZeroGpuAddress;
  if (!resource->generation)
    result |= NativeDescriptorDiagnosticZeroGeneration;
  if (descriptor.byte_offset > resource->byte_size) {
    result |= NativeDescriptorDiagnosticOffsetOutOfBounds;
  } else if (descriptor.byte_size >
             resource->byte_size - descriptor.byte_offset) {
    result |= NativeDescriptorDiagnosticSizeOutOfBounds;
  }
  return result;
}

struct DescriptorSlotMeta {
  DescriptorBackendSlotKind kind = DescriptorBackendSlotKind::Empty;
  uint32_t flags = 0;
  uint64_t generation = 0;
};

struct DescriptorResidencyTarget {
  // Resources referenced by the native descriptor table. These must survive
  // independently of any bindless-mirror repair payload.
  WMT::Reference<WMT::Resource> allocation;
  WMT::Reference<WMT::Resource> secondary_allocation;
  // Allocation backing a deferred bindless texture/texture-buffer payload.
  // Kept separate so repairing the fallback mirror cannot drop native buffer
  // or UAV-counter residency.
  WMT::Reference<WMT::Resource> mirror_allocation;
  Rc<dxmt::Sampler> sampler;
};

struct DescriptorResidencyTransition {
  DescriptorResidencyTarget previous;
};

struct DescriptorTextureSlotPayload {
  uint64_t handle = 0;
  uint64_t metadata = 0;
};

struct DescriptorSamplerSlotPayload {
  uint64_t handle = 0;
  uint64_t cube_handle = 0;
  uint64_t lod_bias = 0;
};

/**
 * Persistent descriptor mirror for one shader-visible D3D12 descriptor heap.
 *
 * A D3D12 descriptor heap has a single type, so each shader-visible heap backs exactly
 * one mirror array:
 *   - CBV_SRV_UAV heap -> a planar TEXTURE mirror for the bindless-mirror ABI
 *     plus the native three-qword descriptor table, buffer descriptor records, and
 *     resource table. Native buffer descriptors are materialized on write and
 *     resolve allocation identity through the resource table.
 *   - SAMPLER heap -> a planar SAMPLER mirror: typed sampler handles, cube
 *     sampler handles, then uint64 lod-bias metadata.
 *
 * The buffer is Metal shared-storage so the CPU writes the payload and the GPU reads it
 * indirectly as an argument buffer (bound at slot 30 by sub-step ③). It is sized to the
 * heap's real NumDescriptors (NOT kBindlessMirrorCapacity, which is an AIR GEP-typing
 * artifact only).
 *
 * THREADING:
 *   - Sampler slots are filled synchronously at CreateSampler/CopyDescriptors on the
 *     app thread (Sampler handles are a pure function of the D3D12_SAMPLER_DESC and are
 *     immutable once created).
 *   - Texture and texture-buffer SRV/UAV slots backed by a Metal4
 *     texture-view-pool descriptor are filled synchronously at descriptor
 *     write/copy time using the pool slot resource ID.
 *
 * The runtime allocates this for shader-visible CBV/SRV/UAV and SAMPLER heaps.
 * Every D3D12 shader-visible heap owns this backing; descriptor architecture is
 * not selected by an environment variable.
 */
class DescriptorHeapMirror {
public:
  using ScopedLock = std::unique_lock<std::recursive_mutex>;

  DescriptorHeapMirror(WMT::Device device, uint32_t num_descriptors, bool sampler_heap);

  // DescriptorRecord storage is owned by the heap, but shader-visible record
  // publication shares this lock with mirror state. Queue-side readers take a
  // snapshot while holding it; app-thread writers hold it from record mutation
  // through stale-generation publication.
  ScopedLock AcquireLock() const { return ScopedLock(mutex_); }

  /** GPU virtual address of the mirror buffer (for binding at slot 30 by ③). */
  uint64_t gpuAddress() const {
    std::lock_guard lock(mutex_);
    return gpu_address_;
  }

  /** The Metal buffer handle (for residency / binding by ③). */
  WMT::Buffer buffer() const {
    std::lock_guard lock(mutex_);
    return buffer_;
  }

  WMT::TextureViewPool textureViewPool() const {
    std::lock_guard lock(mutex_);
    return texture_view_pool_;
  }
  uint64_t textureViewPoolBaseResourceID() const {
    std::lock_guard lock(mutex_);
    return texture_view_pool_base_resource_id_;
  }
  uint64_t textureViewPoolSlotResourceID(uint32_t index) const {
    std::lock_guard lock(mutex_);
    return texture_view_pool_base_resource_id_ && index < num_descriptors_
               ? texture_view_pool_base_resource_id_ + index
               : 0;
  }

  uint64_t descriptorTableGpuAddress() const {
    std::lock_guard lock(mutex_);
    return table_gpu_address_;
  }

  WMT::Buffer descriptorTableBuffer() const {
    std::lock_guard lock(mutex_);
    return table_buffer_;
  }

  uint64_t bufferDescriptorRecordGpuAddress() const {
    std::lock_guard lock(mutex_);
    return buffer_record_gpu_address_;
  }

  WMT::Buffer bufferDescriptorRecordBuffer() const {
    std::lock_guard lock(mutex_);
    return buffer_record_buffer_;
  }

  uint64_t bufferResourceTableGpuAddress() const {
    std::lock_guard lock(mutex_);
    return buffer_resource_table_gpu_address_;
  }

  WMT::Buffer bufferResourceTableBuffer() const {
    std::lock_guard lock(mutex_);
    return buffer_resource_table_buffer_;
  }

  uint32_t numDescriptors() const { return num_descriptors_; }
  bool isSamplerHeap() const { return sampler_heap_; }

  bool descriptorTableBackendReady() const {
    std::lock_guard lock(mutex_);
    return DescriptorTableBackendReadyUnlocked();
  }

  bool nativeDescriptorRecordStorageReady() const {
    std::lock_guard lock(mutex_);
    return DescriptorTableBackendReadyUnlocked() &&
           (sampler_heap_ || (buffer_record_buffer_ &&
                              buffer_record_gpu_address_ &&
                              buffer_record_mapped_ &&
                              buffer_resource_table_buffer_ &&
                              buffer_resource_table_gpu_address_ &&
                              buffer_resource_table_mapped_));
  }

  std::optional<DescriptorTableEntry>
  descriptorTableEntry(uint32_t index) const {
    std::lock_guard lock(mutex_);
    return index < table_entries_.size()
               ? std::optional<DescriptorTableEntry>(table_entries_[index])
               : std::nullopt;
  }

  std::optional<BufferDescriptorRecord>
  bufferDescriptorRecord(uint32_t index) const {
    auto lock = AcquireLock();
    return bufferDescriptorRecord(lock, index);
  }
  std::optional<BufferDescriptorRecord>
  bufferDescriptorRecord(const ScopedLock &lock, uint32_t index) const {
    ValidateLock(lock);
    return buffer_record_mapped_ && index < num_descriptors_
               ? std::optional<BufferDescriptorRecord>(
                     buffer_record_mapped_[index])
               : std::nullopt;
  }

  std::optional<DescriptorSlotMeta> slotMeta(uint32_t index) const {
    std::lock_guard lock(mutex_);
    return index < slot_meta_.size()
               ? std::optional<DescriptorSlotMeta>(slot_meta_[index])
               : std::nullopt;
  }

  uint32_t RegisterBufferResource(uint32_t descriptor_index,
                                  bool counter_resource,
                                  uint64_t resource_identity,
                                  WMT::Resource allocation,
                                  uint64_t gpu_address,
                                  uint64_t byte_size);
  uint32_t RegisterBufferResource(const ScopedLock &lock,
                                  uint32_t descriptor_index,
                                  bool counter_resource,
                                  uint64_t resource_identity,
                                  WMT::Resource allocation,
                                  uint64_t gpu_address,
                                  uint64_t byte_size);
  bool RefreshBufferResource(uint32_t resource_index,
                             WMT::Resource allocation,
                             uint64_t gpu_address,
                             uint64_t byte_size);
  void InvalidateBufferResourceTableEntryForTesting(uint32_t resource_index);
  std::optional<DescriptorBackendResourceRecord>
  backendResourceRecord(uint32_t index) const {
    auto lock = AcquireLock();
    return backendResourceRecord(lock, index);
  }
  std::optional<DescriptorBackendResourceRecord>
  backendResourceRecord(const ScopedLock &lock, uint32_t index) const {
    ValidateLock(lock);
    return index < buffer_resources_.size()
               ? std::optional<DescriptorBackendResourceRecord>(
                     buffer_resources_[index])
               : std::nullopt;
  }
  uint32_t backendResourceCount() const {
    std::lock_guard lock(mutex_);
    return static_cast<uint32_t>(buffer_resources_.size());
  }
  uint64_t backendResourceTableGeneration() const {
    auto lock = AcquireLock();
    return backendResourceTableGeneration(lock);
  }
  uint64_t backendResourceTableGeneration(const ScopedLock &lock) const {
    ValidateLock(lock);
    return buffer_resource_table_generation_;
  }

  uint64_t changeJournalCursor() const {
    std::lock_guard lock(mutex_);
    return change_journal_.cursor();
  }

  DescriptorChangeSet changesSince(uint64_t cursor) const {
    std::lock_guard lock(mutex_);
    return change_journal_.ChangesSince(cursor);
  }

  void WriteNullTableEntry(uint32_t index);
  void WriteNullTableEntry(const ScopedLock &lock, uint32_t index);
  void WriteBufferTableEntry(uint32_t index, uint64_t gpu_va, uint64_t size,
                             bool typed, uint32_t texture_view_offset = 0);
  void WriteBufferTableEntry(const ScopedLock &lock, uint32_t index,
                             uint64_t gpu_va, uint64_t size, bool typed,
                             uint32_t texture_view_offset = 0);
  void WriteBufferTextureTableEntry(uint32_t index, uint64_t gpu_va,
                                    uint64_t size, uint64_t texture_view_id,
                                    uint32_t element_count,
                                    uint32_t first_element, uint32_t flags);
  void WriteBufferTextureTableEntry(
      const ScopedLock &lock, uint32_t index, uint64_t gpu_va, uint64_t size,
      uint64_t texture_view_id, uint32_t element_count,
      uint32_t first_element, uint32_t flags);
  void WriteBufferDescriptorRecord(uint32_t index,
                                   const BufferDescriptorRecord &record);
  void WriteBufferDescriptorRecord(const ScopedLock &lock, uint32_t index,
                                   const BufferDescriptorRecord &record);
  void WriteTextureTableEntry(uint32_t index, uint64_t gpu_resource_id,
                              uint32_t array_length,
                              float min_lod = 0.0f);
  void WriteTextureTableEntry(const ScopedLock &lock, uint32_t index,
                              uint64_t gpu_resource_id,
                              uint32_t array_length, float min_lod = 0.0f);
  void WriteTexturePoolTableEntry(uint32_t index, uint32_t array_length,
                                  float min_lod = 0.0f);
  void WriteSamplerTableEntry(uint32_t index, const Sampler *sampler);
  void WriteSamplerTableEntry(const ScopedLock &lock, uint32_t index,
                              const Sampler *sampler);
  uint64_t SetTexturePoolSlot(uint32_t index, dxmt::Texture *texture,
                              dxmt::TextureViewKey view,
                              dxmt::TextureAllocation *allocation);
  uint64_t SetTexturePoolSlot(const ScopedLock &lock, uint32_t index,
                              dxmt::Texture *texture,
                              dxmt::TextureViewKey view,
                              dxmt::TextureAllocation *allocation);
  uint64_t SetTexturePoolBufferSlot(
      uint32_t index, WMT::Buffer buffer,
      const WMTTextureBufferViewDescriptor &descriptor, uint64_t offset,
      uint64_t bytes_per_row);
  uint64_t SetTexturePoolBufferSlot(
      const ScopedLock &lock, uint32_t index, WMT::Buffer buffer,
      const WMTTextureBufferViewDescriptor &descriptor, uint64_t offset,
      uint64_t bytes_per_row);
  uint64_t CopyTexturePoolSlotFrom(uint32_t dst_index,
                                   const DescriptorHeapMirror &src,
                                   uint32_t src_index);

  DescriptorResidencyTransition ReplaceResidencyTarget(
      uint32_t index, DescriptorResidencyTarget target);
  DescriptorResidencyTransition ReplaceResidencyTarget(
      const ScopedLock &lock, uint32_t index,
      DescriptorResidencyTarget target);
  bool ReplaceMirrorResidencyTargetIfCurrent(
      uint32_t index, dxmt::DescriptorSlotVersion expected_version,
      DescriptorResidencyTarget target,
      DescriptorResidencyTransition *transition);
  std::vector<DescriptorResidencyTarget> DrainResidencyTargets();
  std::optional<DescriptorResidencyTarget>
  residencyTarget(uint32_t index) const {
    std::lock_guard lock(mutex_);
    return index < residency_targets_.size()
               ? std::optional<DescriptorResidencyTarget>(
                     residency_targets_[index])
               : std::nullopt;
  }

  /**
   * Read every qword belonging to one slot under a single mirror lock. When an
   * expected generation is supplied, the payload is returned only if that
   * descriptor version is still fully published. This prevents deferred
   * encoding from combining fields written by different descriptor versions.
   */
  std::optional<DescriptorTextureSlotPayload> textureSlotPayload(
      uint32_t index,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt) const;
  std::optional<DescriptorSamplerSlotPayload> samplerSlotPayload(
      uint32_t index,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt) const;

  /**
   * Fill a SAMPLER slot synchronously (app-thread safe). `sampler` may be null, in which
   * case the heap-owned null sampler payload is written. Byte-identical to encodeShader
   * resources via the shared writer.
   */
  bool FillSamplerSlot(
      uint32_t index, const Sampler *sampler, uint64_t null_handle,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt);
  bool FillSamplerSlot(
      const ScopedLock &lock, uint32_t index, const Sampler *sampler,
      uint64_t null_handle,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt);

  /**
   * Fill a TEXTURE slot with an already-resolved (gpuResourceID, arrayLength, minLOD).
   * Texture pool resource IDs may be written on the app thread; typed buffer view
   * payloads are still resolved on the encode thread.
   */
  bool FillTextureSlot(
      uint32_t index, uint64_t gpu_resource_id, uint32_t array_length,
      float min_lod = 0.0f,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt);
  bool FillTextureSlot(
      const ScopedLock &lock, uint32_t index, uint64_t gpu_resource_id,
      uint32_t array_length, float min_lod = 0.0f,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt);

  /** Fill a TEXTURE slot with an already-encoded handle/metadata pair. */
  bool FillTextureSlotPayload(
      uint32_t index, uint64_t handle, uint64_t metadata,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt);

  /** Clear a TEXTURE slot and mark the current stale generation as handled. */
  bool ClearTextureSlot(
      uint32_t index,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt);
  bool ClearTextureSlot(
      const ScopedLock &lock, uint32_t index,
      std::optional<dxmt::DescriptorSlotVersion> expected_version =
          std::nullopt);

  /** Begin one heap-local slot publication while holding the mirror lock. */
  dxmt::DescriptorSlotVersion BeginSlotWrite(uint32_t index);
  dxmt::DescriptorSlotVersion BeginSlotWrite(const ScopedLock &lock,
                                             uint32_t index);

  std::optional<dxmt::DescriptorSlotVersion>
  slotPendingVersion(uint32_t index) const {
    std::lock_guard lock(mutex_);
    return index < needs_fill_.size() && needs_fill_[index]
               ? std::optional<dxmt::DescriptorSlotVersion>(
                     stale_versions_[index])
               : std::nullopt;
  }

  /** Slot version last FILLED by the encode thread. */
  dxmt::DescriptorSlotVersion slotFilledVersion(uint32_t index) const {
    std::lock_guard lock(mutex_);
    return index < filled_versions_.size() ? filled_versions_[index]
                                           : dxmt::DescriptorSlotVersion{};
  }

  /** Slot version assigned by the most recent app-thread write. */
  dxmt::DescriptorSlotVersion slotStaleVersion(uint32_t index) const {
    std::lock_guard lock(mutex_);
    return index < stale_versions_.size() ? stale_versions_[index]
                                          : dxmt::DescriptorSlotVersion{};
  }

  bool SlotNeedsFill(uint32_t index) const {
    std::lock_guard lock(mutex_);
    return index < needs_fill_.size() && needs_fill_[index];
  }

private:
  void ValidateLock(const ScopedLock &lock) const {
    if (!lock.owns_lock() || lock.mutex() != &mutex_)
      std::abort();
  }
  bool CanPublishVersionUnlocked(
      uint32_t index,
      const std::optional<dxmt::DescriptorSlotVersion> &expected_version) const {
    return !expected_version ||
           (index < needs_fill_.size() && needs_fill_[index] &&
            stale_versions_[index] == *expected_version);
  }
  bool IsPublishedVersionUnlocked(
      uint32_t index,
      const std::optional<dxmt::DescriptorSlotVersion> &expected_version) const {
    return !expected_version ||
           (index < needs_fill_.size() &&
            index < filled_versions_.size() && !needs_fill_[index] &&
            filled_versions_[index] == *expected_version);
  }
  void MarkVersionFilledUnlocked(
      uint32_t index,
      const std::optional<dxmt::DescriptorSlotVersion> &expected_version) {
    if (index < filled_versions_.size())
      filled_versions_[index] =
          expected_version.value_or(stale_versions_[index]);
    if (index < needs_fill_.size())
      needs_fill_[index] = 0;
  }
  bool DescriptorTableBackendReadyUnlocked() const {
    if (!table_buffer_ || !table_gpu_address_ || !table_mapped_)
      return false;
    return sampler_heap_ || texture_view_pool_base_resource_id_;
  }
  uint64_t *TextureHandlePtrUnlocked(uint32_t index);
  uint64_t *TextureMetadataPtrUnlocked(uint32_t index);
  uint64_t *SamplerHandlePtrUnlocked(uint32_t index);
  uint64_t *SamplerCubeHandlePtrUnlocked(uint32_t index);
  uint64_t *SamplerLodBiasPtrUnlocked(uint32_t index);
  uint32_t BufferResourceIndex(uint32_t descriptor_index,
                               bool counter_resource) const;
  uint64_t NextBufferResourceTableGenerationUnlocked();
  void ClearBufferResourceUnlocked(uint32_t resource_index);
  void ClearBufferResourcesForSlotUnlocked(uint32_t descriptor_index,
                                           bool clear_primary,
                                           bool clear_counter);
  void WriteTableEntryUnlocked(uint32_t index,
                               const DescriptorTableEntry &entry);
  void WriteBufferResourceTableEntryUnlocked(
      uint32_t index, const DescriptorBackendResourceRecord &record);
  void WriteBufferDescriptorRecordUnlocked(
      uint32_t index, const BufferDescriptorRecord &record);
  void WriteSlotMetaUnlocked(uint32_t index, DescriptorBackendSlotKind kind,
                             uint32_t flags = 0);
  bool FillSamplerSlotUnlocked(
      uint32_t index, const Sampler *sampler, uint64_t null_handle,
      std::optional<dxmt::DescriptorSlotVersion> expected_version);
  bool FillTextureSlotUnlocked(
      uint32_t index, uint64_t gpu_resource_id, uint32_t array_length,
      float min_lod,
      std::optional<dxmt::DescriptorSlotVersion> expected_version);
  bool FillTextureSlotPayloadUnlocked(
      uint32_t index, uint64_t handle, uint64_t metadata,
      std::optional<dxmt::DescriptorSlotVersion> expected_version);
  bool ClearTextureSlotUnlocked(
      uint32_t index,
      std::optional<dxmt::DescriptorSlotVersion> expected_version);
  WMT::Reference<WMT::Buffer> buffer_;
  WMT::Reference<WMT::Buffer> table_buffer_;
  WMT::Reference<WMT::Buffer> buffer_record_buffer_;
  WMT::Reference<WMT::Buffer> buffer_resource_table_buffer_;
  WMT::Reference<WMT::TextureViewPool> texture_view_pool_;
  WMT::Reference<WMT::SamplerState> null_sampler_;
  uint64_t *mapped_ = nullptr;
  DescriptorTableEntry *table_mapped_ = nullptr;
  BufferDescriptorRecord *buffer_record_mapped_ = nullptr;
  BufferResourceTableEntry *buffer_resource_table_mapped_ = nullptr;
  uint64_t gpu_address_ = 0;
  uint64_t table_gpu_address_ = 0;
  uint64_t buffer_record_gpu_address_ = 0;
  uint64_t buffer_resource_table_gpu_address_ = 0;
  uint64_t texture_view_pool_base_resource_id_ = 0;
  uint64_t null_sampler_handle_ = 0;
  uint64_t buffer_resource_table_generation_ = 0;
  uint32_t buffer_resource_table_capacity_ = 0;
  uint32_t num_descriptors_ = 0;
  bool sampler_heap_ = false;
  std::vector<DescriptorTableEntry> table_entries_;
  std::vector<DescriptorBackendResourceRecord> buffer_resources_;
  std::vector<DescriptorSlotMeta> slot_meta_;
  std::vector<DescriptorResidencyTarget> residency_targets_;
  // Per-slot version bookkeeping. Each heap advances its slots independently;
  // the process-wide content revision is a separate cache invalidation clock.
  std::vector<dxmt::DescriptorSlotVersion> stale_versions_;
  std::vector<dxmt::DescriptorSlotVersion> filled_versions_;
  // Every write sets this flag and every completed fill clears it under
  // mutex_. The epoch+sequence pair also prevents finite-width ABA.
  std::vector<uint8_t> needs_fill_;
  DescriptorChangeJournal change_journal_;
  mutable std::recursive_mutex mutex_;
};

} // namespace dxmt::d3d12
