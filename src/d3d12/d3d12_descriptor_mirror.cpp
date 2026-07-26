#include "d3d12_descriptor_mirror.hpp"

#include "dxmt_perf_stats.hpp"
#include "dxmt_sampler.hpp"
#include "dxmt_texture.hpp"
#include "log/log.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <unordered_set>
#include <utility>

namespace dxmt::d3d12 {
namespace {

constexpr uint32_t kDescriptorTableEntryQwords = 3;
constexpr uint64_t kDescriptorTableTypedBufferBit = 1ull << 63;

uint32_t
BufferResourceTableCapacity(uint32_t descriptor_count) {
  if (!descriptor_count)
    return 1;

  const uint64_t capacity = uint64_t(descriptor_count) * 2u + 1u;
  return uint32_t(std::min<uint64_t>(capacity, UINT32_MAX));
}

uint64_t
BufferDescriptorMetadata(uint64_t size, bool typed, uint32_t texture_view_offset) {
  // This is the legacy three-qword table payload. Native CBV/raw/structured
  // buffer access reads the full 64-bit BufferDescriptorRecord::byte_size;
  // typed texture-buffer descriptors use D3D12's 32-bit NumElements fields.
  return std::min<uint64_t>(size, UINT32_MAX) |
         (uint64_t(texture_view_offset & 0xffu) << 32) |
         (typed ? kDescriptorTableTypedBufferBit : 0);
}

uint64_t
FloatMetadata(float value) {
  return uint64_t(std::bit_cast<uint32_t>(value));
}

WMTSamplerInfo
NullSamplerInfo() {
  WMTSamplerInfo info = {};
  info.support_argument_buffers = true;
  info.border_color = WMTSamplerBorderColorTransparentBlack;
  info.compare_function = WMTCompareFunctionNever;
  info.normalized_coords = true;
  info.r_address_mode = WMTSamplerAddressModeClampToEdge;
  info.s_address_mode = WMTSamplerAddressModeClampToEdge;
  info.t_address_mode = WMTSamplerAddressModeClampToEdge;
  info.min_filter = WMTSamplerMinMagFilterNearest;
  info.mag_filter = WMTSamplerMinMagFilterNearest;
  info.mip_filter = WMTSamplerMipFilterNotMipmapped;
  info.lod_min_clamp = 0.0f;
  info.lod_max_clamp = std::numeric_limits<float>::max();
  info.max_anisotroy = 1;
  info.lod_average = false;
  return info;
}

} // namespace

DescriptorHeapMirror::DescriptorHeapMirror(WMT::Device device, uint32_t num_descriptors, bool sampler_heap)
    : num_descriptors_(num_descriptors), sampler_heap_(sampler_heap),
      change_journal_(std::max<uint64_t>(256, uint64_t(num_descriptors) * 2)) {
  const uint32_t plane_count = sampler_heap_ ? 3 : 2;
  const uint64_t length =
      uint64_t(num_descriptors_) * plane_count * sizeof(uint64_t);

  WMTBufferInfo info = {};
  info.length = length ? length : sizeof(uint64_t);
  info.options = static_cast<WMTResourceOptions>(
      WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
  info.memory.set(nullptr);
  buffer_ = device.newBuffer(info);
  if (!buffer_) {
    ERR("DescriptorHeapMirror: failed to allocate mirror buffer length=", info.length,
        " sampler=", sampler_heap_);
    return;
  }
  gpu_address_ = info.gpu_address;
  mapped_ = reinterpret_cast<uint64_t *>(info.memory.get_accessible_or_null());
  if (!mapped_) {
    ERR("DescriptorHeapMirror: mirror buffer is not CPU-accessible length=", info.length);
    return;
  }
  std::memset(mapped_, 0, info.length);

  WMTBufferInfo table_info = {};
  table_info.length = num_descriptors_
                          ? uint64_t(num_descriptors_) *
                                kDescriptorTableEntryQwords * sizeof(uint64_t)
                          : sizeof(DescriptorTableEntry);
  table_info.options = static_cast<WMTResourceOptions>(
      WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
  table_info.memory.set(nullptr);
  table_buffer_ = device.newBuffer(table_info);
  if (!table_buffer_) {
    ERR("DescriptorHeapMirror: failed to allocate descriptor table buffer length=",
        table_info.length, " sampler=", sampler_heap_);
  } else {
    table_gpu_address_ = table_info.gpu_address;
    table_mapped_ =
        reinterpret_cast<DescriptorTableEntry *>(table_info.memory.get_accessible_or_null());
    if (!table_mapped_) {
      ERR("DescriptorHeapMirror: descriptor table buffer is not CPU-accessible length=",
          table_info.length);
    } else {
      auto *qwords = reinterpret_cast<uint64_t *>(table_mapped_);
      const size_t qword_count =
          static_cast<size_t>(table_info.length / sizeof(*qwords));
      std::fill_n(qwords, qword_count, uint64_t{0});
    }
  }
  table_entries_.assign(num_descriptors_, {});
  slot_meta_.resize(num_descriptors_);
  residency_targets_.resize(num_descriptors_);

  if (!sampler_heap_) {
    buffer_resource_table_capacity_ =
        BufferResourceTableCapacity(num_descriptors_);
    // Entry zero is the null resource. Every descriptor owns one stable primary
    // and one stable counter entry after it. Reusing a descriptor therefore
    // replaces (and releases) its old resource instead of growing an append-only
    // identity map until the fixed GPU table is exhausted.
    buffer_resources_.resize(buffer_resource_table_capacity_);
    WMTBufferInfo resource_table_info = {};
    resource_table_info.length =
        uint64_t(buffer_resource_table_capacity_) *
        sizeof(BufferResourceTableEntry);
    resource_table_info.options = static_cast<WMTResourceOptions>(
        WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
    resource_table_info.memory.set(nullptr);
    buffer_resource_table_buffer_ = device.newBuffer(resource_table_info);
    if (!buffer_resource_table_buffer_) {
      ERR("DescriptorHeapMirror: failed to allocate buffer resource table length=",
          resource_table_info.length);
    } else {
      buffer_resource_table_gpu_address_ = resource_table_info.gpu_address;
      buffer_resource_table_mapped_ =
          reinterpret_cast<BufferResourceTableEntry *>(
              resource_table_info.memory.get_accessible_or_null());
      if (!buffer_resource_table_mapped_) {
        ERR("DescriptorHeapMirror: buffer resource table is not CPU-accessible length=",
            resource_table_info.length);
      } else {
        std::fill(buffer_resource_table_mapped_,
                  buffer_resource_table_mapped_ +
                      buffer_resource_table_capacity_,
                  BufferResourceTableEntry{});
      }
    }

    WMTBufferInfo record_info = {};
    record_info.length =
        num_descriptors_
            ? uint64_t(num_descriptors_) * sizeof(BufferDescriptorRecord)
            : sizeof(BufferDescriptorRecord);
    record_info.options = static_cast<WMTResourceOptions>(
        WMTResourceStorageModeShared | WMTResourceHazardTrackingModeUntracked);
    record_info.memory.set(nullptr);
    buffer_record_buffer_ = device.newBuffer(record_info);
    if (!buffer_record_buffer_) {
      ERR("DescriptorHeapMirror: failed to allocate buffer descriptor records length=",
          record_info.length);
    } else {
      buffer_record_gpu_address_ = record_info.gpu_address;
      buffer_record_mapped_ = reinterpret_cast<BufferDescriptorRecord *>(
          record_info.memory.get_accessible_or_null());
      if (!buffer_record_mapped_) {
        ERR("DescriptorHeapMirror: buffer descriptor records are not CPU-accessible length=",
            record_info.length);
      } else {
        const auto record_count =
            record_info.length / sizeof(BufferDescriptorRecord);
        std::fill(buffer_record_mapped_, buffer_record_mapped_ + record_count,
                  BufferDescriptorRecord{});
      }
    }
  }

  if (!sampler_heap_) {
    WMTTextureViewPoolInfo pool_info = {};
    pool_info.initial_count = num_descriptors_;
    WMT::Error error;
    texture_view_pool_ = device.newTextureViewPool(pool_info, error);
    if (!texture_view_pool_) {
      ERR("DescriptorHeapMirror: failed to allocate texture view pool descriptors=",
          num_descriptors_);
    } else {
      texture_view_pool_base_resource_id_ =
          texture_view_pool_.baseResourceID();
      if (!texture_view_pool_base_resource_id_)
        ERR("DescriptorHeapMirror: texture view pool returned null baseResourceID descriptors=",
            num_descriptors_);
    }
  } else {
    auto null_info = NullSamplerInfo();
    null_sampler_ = device.newSamplerState(null_info);
    if (!null_sampler_) {
      ERR("DescriptorHeapMirror: failed to allocate null sampler descriptors=",
          num_descriptors_);
    } else {
      null_sampler_handle_ = null_info.gpu_resource_id;
      if (!null_sampler_handle_)
        ERR("DescriptorHeapMirror: null sampler returned null gpuResourceID descriptors=",
            num_descriptors_);
    }
  }

  stale_versions_.assign(num_descriptors_, {});
  filled_versions_.assign(num_descriptors_, {});
  needs_fill_.assign(num_descriptors_, 0);
  if (sampler_heap_) {
    for (uint32_t i = 0; i < num_descriptors_; i++) {
      WriteTableEntryUnlocked(i, {null_sampler_handle_, null_sampler_handle_,
                                  FloatMetadata(0.0f)});
      FillSamplerSlotUnlocked(i, nullptr, null_sampler_handle_, std::nullopt);
    }
  }
}

uint32_t
DescriptorHeapMirror::BufferResourceIndex(uint32_t descriptor_index,
                                          bool counter_resource) const {
  if (sampler_heap_ || descriptor_index >= num_descriptors_)
    return kNullDescriptorResourceIndex;
  const uint64_t index = 1u + uint64_t(descriptor_index) * 2u +
                         uint64_t(counter_resource);
  return index < buffer_resource_table_capacity_
             ? static_cast<uint32_t>(index)
             : kNullDescriptorResourceIndex;
}

uint64_t
DescriptorHeapMirror::NextBufferResourceTableGenerationUnlocked() {
  if (buffer_resource_table_generation_ ==
      std::numeric_limits<uint64_t>::max())
    buffer_resource_table_generation_ = 1;
  else
    ++buffer_resource_table_generation_;
  return buffer_resource_table_generation_;
}

uint32_t
DescriptorHeapMirror::RegisterBufferResource(uint32_t descriptor_index,
                                             bool counter_resource,
                                             uint64_t resource_identity,
                                             WMT::Resource allocation,
                                             uint64_t gpu_address,
                                             uint64_t byte_size) {
  auto lock = AcquireLock();
  return RegisterBufferResource(lock, descriptor_index, counter_resource,
                                resource_identity, allocation, gpu_address,
                                byte_size);
}

uint32_t
DescriptorHeapMirror::RegisterBufferResource(
    const ScopedLock &lock, uint32_t descriptor_index, bool counter_resource,
    uint64_t resource_identity, WMT::Resource allocation,
    uint64_t gpu_address, uint64_t byte_size) {
  ValidateLock(lock);
  if (sampler_heap_ || !resource_identity || !allocation ||
      !buffer_resource_table_mapped_)
    return kNullDescriptorResourceIndex;

  const uint32_t index =
      BufferResourceIndex(descriptor_index, counter_resource);
  if (index == kNullDescriptorResourceIndex ||
      index >= buffer_resources_.size())
    return kNullDescriptorResourceIndex;

  const uint64_t allocation_handle = allocation.handle;
  auto &record = buffer_resources_[index];
  if (record.resource_identity == resource_identity &&
      record.allocation_handle == allocation_handle &&
      record.gpu_address == gpu_address && record.byte_size == byte_size)
    return index;

  record.allocation = allocation;
  record.resource_identity = resource_identity;
  record.gpu_address = gpu_address;
  record.byte_size = byte_size;
  record.allocation_handle = allocation_handle;
  record.generation = NextBufferResourceTableGenerationUnlocked();
  WriteBufferResourceTableEntryUnlocked(index, record);
  return index;
}

bool
DescriptorHeapMirror::RefreshBufferResource(uint32_t resource_index,
                                            WMT::Resource allocation,
                                            uint64_t gpu_address,
                                            uint64_t byte_size) {
  std::lock_guard lock(mutex_);
  if (sampler_heap_ || resource_index == kNullDescriptorResourceIndex ||
      resource_index >= buffer_resources_.size() || !allocation ||
      !buffer_resource_table_mapped_ || !byte_size)
    return false;

  auto &record = buffer_resources_[resource_index];
  const uint64_t allocation_handle = allocation.handle;
  if (record.allocation_handle == allocation_handle &&
      record.gpu_address == gpu_address && record.byte_size == byte_size)
    return false;

  record.allocation = allocation;
  record.allocation_handle = allocation_handle;
  record.gpu_address = gpu_address;
  record.byte_size = byte_size;
  record.generation = NextBufferResourceTableGenerationUnlocked();
  WriteBufferResourceTableEntryUnlocked(resource_index, record);
  return true;
}

void
DescriptorHeapMirror::InvalidateBufferResourceTableEntryForTesting(
    uint32_t resource_index) {
  std::lock_guard lock(mutex_);
  ClearBufferResourceUnlocked(resource_index);
}

void
DescriptorHeapMirror::ClearBufferResourceUnlocked(uint32_t resource_index) {
  if (resource_index == kNullDescriptorResourceIndex ||
      resource_index >= buffer_resources_.size())
    return;
  auto &record = buffer_resources_[resource_index];
  if (!record.resource_identity && !record.allocation &&
      !record.allocation_handle && !record.gpu_address && !record.byte_size)
    return;
  record = {};
  record.generation = NextBufferResourceTableGenerationUnlocked();
  WriteBufferResourceTableEntryUnlocked(resource_index, record);
}

void
DescriptorHeapMirror::ClearBufferResourcesForSlotUnlocked(
    uint32_t descriptor_index, bool clear_primary, bool clear_counter) {
  if (clear_primary)
    ClearBufferResourceUnlocked(BufferResourceIndex(descriptor_index, false));
  if (clear_counter)
    ClearBufferResourceUnlocked(BufferResourceIndex(descriptor_index, true));
}

void
DescriptorHeapMirror::WriteTableEntryUnlocked(
    uint32_t index, const DescriptorTableEntry &entry) {
  if (index >= table_entries_.size())
    return;
  table_entries_[index] = entry;
  if (table_mapped_)
    table_mapped_[index] = entry;
}

void
DescriptorHeapMirror::WriteBufferResourceTableEntryUnlocked(
    uint32_t index, const DescriptorBackendResourceRecord &record) {
  if (!buffer_resource_table_mapped_ ||
      index >= buffer_resource_table_capacity_)
    return;
  buffer_resource_table_mapped_[index] = BufferResourceTableEntry{
      record.gpu_address,
      record.byte_size,
      record.allocation_handle,
      record.generation,
  };
}

void
DescriptorHeapMirror::WriteSlotMetaUnlocked(uint32_t index,
                                            DescriptorBackendSlotKind kind,
                                            uint32_t flags) {
  if (index >= slot_meta_.size())
    return;
  auto &meta = slot_meta_[index];
  meta.kind = kind;
  meta.flags = flags;
  meta.generation++;
  if (!meta.generation)
    meta.generation = 1;
  change_journal_.Record(index, meta.generation);
}

void
DescriptorHeapMirror::WriteNullTableEntry(uint32_t index) {
  auto lock = AcquireLock();
  WriteNullTableEntry(lock, index);
}

void
DescriptorHeapMirror::WriteNullTableEntry(const ScopedLock &lock,
                                          uint32_t index) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  if (sampler_heap_) {
    WriteTableEntryUnlocked(index,
                            {null_sampler_handle_, null_sampler_handle_,
                             FloatMetadata(0.0f)});
    WriteSlotMetaUnlocked(index, DescriptorBackendSlotKind::Empty);
    FillSamplerSlotUnlocked(index, nullptr, null_sampler_handle_, std::nullopt);
    return;
  }
  WriteTableEntryUnlocked(index, {});
  WriteBufferDescriptorRecordUnlocked(index, {});
  WriteSlotMetaUnlocked(index, DescriptorBackendSlotKind::Empty);
  ClearTextureSlotUnlocked(index, std::nullopt);
}

void
DescriptorHeapMirror::WriteBufferTableEntry(uint32_t index, uint64_t gpu_va,
                                            uint64_t size, bool typed,
                                            uint32_t texture_view_offset) {
  auto lock = AcquireLock();
  WriteBufferTableEntry(lock, index, gpu_va, size, typed,
                        texture_view_offset);
}

void
DescriptorHeapMirror::WriteBufferTableEntry(
    const ScopedLock &lock, uint32_t index, uint64_t gpu_va, uint64_t size,
    bool typed, uint32_t texture_view_offset) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  WriteTableEntryUnlocked(index,
                          {gpu_va, 0,
                           BufferDescriptorMetadata(size, typed,
                                                    texture_view_offset)});
  WriteSlotMetaUnlocked(index, DescriptorBackendSlotKind::Buffer,
                        typed ? BufferDescriptorRecordFlagTyped : 0u);
  if (!typed)
    ClearTextureSlotUnlocked(index, std::nullopt);
}

void
DescriptorHeapMirror::WriteBufferTextureTableEntry(
    uint32_t index, uint64_t gpu_va, uint64_t size,
    uint64_t texture_view_id, uint32_t element_count,
    uint32_t first_element, uint32_t flags) {
  auto lock = AcquireLock();
  WriteBufferTextureTableEntry(lock, index, gpu_va, size, texture_view_id,
                               element_count, first_element, flags);
}

void
DescriptorHeapMirror::WriteBufferTextureTableEntry(
    const ScopedLock &lock, uint32_t index, uint64_t gpu_va, uint64_t size,
    uint64_t texture_view_id, uint32_t element_count,
    uint32_t first_element, uint32_t flags) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  const uint64_t metadata =
      (uint64_t(element_count) << 32) | uint64_t(first_element);
  WriteTableEntryUnlocked(index, {gpu_va, texture_view_id, metadata});
  WriteSlotMetaUnlocked(index, DescriptorBackendSlotKind::Buffer,
                        flags | BufferDescriptorRecordFlagTextureView);
  FillTextureSlotPayloadUnlocked(index, texture_view_id, metadata,
                                 std::nullopt);
}

void
DescriptorHeapMirror::WriteBufferDescriptorRecord(
    uint32_t index, const BufferDescriptorRecord &record) {
  auto lock = AcquireLock();
  WriteBufferDescriptorRecord(lock, index, record);
}

void
DescriptorHeapMirror::WriteBufferDescriptorRecord(
    const ScopedLock &lock, uint32_t index,
    const BufferDescriptorRecord &record) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  WriteBufferDescriptorRecordUnlocked(index, record);
}

void
DescriptorHeapMirror::WriteBufferDescriptorRecordUnlocked(
    uint32_t index, const BufferDescriptorRecord &record) {
  if (!buffer_record_mapped_ || index >= num_descriptors_)
    return;
  if (!(record.flags & BufferDescriptorRecordFlagValid))
    ClearBufferResourcesForSlotUnlocked(index, true, true);
  else if (!(record.flags & BufferDescriptorRecordFlagCounter))
    ClearBufferResourcesForSlotUnlocked(index, false, true);
  buffer_record_mapped_[index] = record;
}

void
DescriptorHeapMirror::WriteTextureTableEntry(uint32_t index,
                                             uint64_t gpu_resource_id,
                                             uint32_t array_length,
                                             float min_lod) {
  auto lock = AcquireLock();
  WriteTextureTableEntry(lock, index, gpu_resource_id, array_length, min_lod);
}

void
DescriptorHeapMirror::WriteTextureTableEntry(
    const ScopedLock &lock, uint32_t index, uint64_t gpu_resource_id,
    uint32_t array_length, float min_lod) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  WriteTableEntryUnlocked(index,
                          {0, gpu_resource_id,
                           MirrorTextureMetadata(array_length, min_lod)});
  WriteBufferDescriptorRecordUnlocked(index, {});
  WriteSlotMetaUnlocked(index, DescriptorBackendSlotKind::Texture);
}

void
DescriptorHeapMirror::WriteTexturePoolTableEntry(uint32_t index,
                                                 uint32_t array_length,
                                                 float min_lod) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  std::lock_guard lock(mutex_);
  const uint64_t resource_id =
      texture_view_pool_base_resource_id_ && index < num_descriptors_
          ? texture_view_pool_base_resource_id_ + index
          : 0;
  WriteTableEntryUnlocked(index,
                          {0, resource_id,
                           MirrorTextureMetadata(array_length, min_lod)});
  WriteBufferDescriptorRecordUnlocked(index, {});
  WriteSlotMetaUnlocked(index, DescriptorBackendSlotKind::Texture);
  if (resource_id)
    FillTextureSlotUnlocked(index, resource_id, array_length, min_lod,
                            std::nullopt);
}

void
DescriptorHeapMirror::WriteSamplerTableEntry(uint32_t index,
                                             const Sampler *sampler) {
  auto lock = AcquireLock();
  WriteSamplerTableEntry(lock, index, sampler);
}

void
DescriptorHeapMirror::WriteSamplerTableEntry(const ScopedLock &lock,
                                             uint32_t index,
                                             const Sampler *sampler) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  const uint64_t sampler_handle = sampler ? sampler->sampler_state_handle : 0;
  const uint64_t sampler_cube_handle =
      sampler ? sampler->sampler_state_cube_handle : 0;
  const float lod_bias = sampler ? sampler->lod_bias : 0.0f;
  WriteTableEntryUnlocked(index,
                          {sampler_handle, sampler_cube_handle,
                           FloatMetadata(lod_bias)});
  WriteSlotMetaUnlocked(index, DescriptorBackendSlotKind::Sampler);
}

uint64_t
DescriptorHeapMirror::SetTexturePoolSlot(uint32_t index, Texture *texture,
                                         TextureViewKey view,
                                         TextureAllocation *allocation) {
  auto lock = AcquireLock();
  return SetTexturePoolSlot(lock, index, texture, view, allocation);
}

uint64_t
DescriptorHeapMirror::SetTexturePoolSlot(
    const ScopedLock &lock, uint32_t index, Texture *texture,
    TextureViewKey view, TextureAllocation *allocation) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  if (sampler_heap_ || !texture_view_pool_ || index >= num_descriptors_ ||
      !texture || !allocation)
    return 0;
  return texture->setViewPoolSlot(texture_view_pool_, view, allocation, index);
}

uint64_t
DescriptorHeapMirror::SetTexturePoolBufferSlot(
    uint32_t index, WMT::Buffer buffer,
    const WMTTextureBufferViewDescriptor &descriptor, uint64_t offset,
    uint64_t bytes_per_row) {
  auto lock = AcquireLock();
  return SetTexturePoolBufferSlot(lock, index, buffer, descriptor, offset,
                                  bytes_per_row);
}

uint64_t
DescriptorHeapMirror::SetTexturePoolBufferSlot(
    const ScopedLock &lock, uint32_t index, WMT::Buffer buffer,
    const WMTTextureBufferViewDescriptor &descriptor, uint64_t offset,
    uint64_t bytes_per_row) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  if (sampler_heap_ || !texture_view_pool_ || index >= num_descriptors_ ||
      !buffer)
    return 0;
  return texture_view_pool_.setTextureViewFromBuffer(
      buffer, descriptor, offset, bytes_per_row, index);
}

uint64_t
DescriptorHeapMirror::CopyTexturePoolSlotFrom(uint32_t dst_index,
                                              const DescriptorHeapMirror &src,
                                              uint32_t src_index) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  if (this == &src) {
    std::lock_guard lock(mutex_);
    if (sampler_heap_ || !texture_view_pool_ ||
        dst_index >= num_descriptors_ || src_index >= num_descriptors_)
      return 0;
    return texture_view_pool_.copyResourceViews(texture_view_pool_, src_index,
                                                1, dst_index);
  }
  std::scoped_lock lock(mutex_, src.mutex_);
  if (sampler_heap_ || src.sampler_heap_ || !texture_view_pool_ ||
      !src.texture_view_pool_ || dst_index >= num_descriptors_ ||
      src_index >= src.num_descriptors_)
    return 0;
  return texture_view_pool_.copyResourceViews(src.texture_view_pool_,
                                              src_index, 1, dst_index);
}

DescriptorResidencyTransition
DescriptorHeapMirror::ReplaceResidencyTarget(
    uint32_t index, DescriptorResidencyTarget target) {
  auto lock = AcquireLock();
  return ReplaceResidencyTarget(lock, index, std::move(target));
}

DescriptorResidencyTransition
DescriptorHeapMirror::ReplaceResidencyTarget(
    const ScopedLock &lock, uint32_t index,
    DescriptorResidencyTarget target) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  if (index >= residency_targets_.size())
    return {};
  DescriptorResidencyTransition transition = {};
  auto &current = residency_targets_[index];
  if (current.allocation.handle == target.allocation.handle &&
      current.secondary_allocation.handle ==
          target.secondary_allocation.handle &&
      current.mirror_allocation.handle == target.mirror_allocation.handle &&
      current.sampler.ptr() == target.sampler.ptr())
    return transition;
  transition.previous = std::move(current);
  residency_targets_[index] = std::move(target);
  return transition;
}

bool
DescriptorHeapMirror::ReplaceMirrorResidencyTargetIfCurrent(
    uint32_t index, dxmt::DescriptorSlotVersion expected_version,
    DescriptorResidencyTarget target,
    DescriptorResidencyTransition *transition) {
  std::lock_guard lock(mutex_);
  if (index >= residency_targets_.size() ||
      index >= filled_versions_.size() ||
      index >= needs_fill_.size() || needs_fill_[index] ||
      filled_versions_[index] != expected_version)
    return false;
  auto &current = residency_targets_[index];
  DescriptorResidencyTarget after = current;
  after.mirror_allocation = target.mirror_allocation;
  after.sampler = target.sampler;
  if (transition) {
    transition->previous.mirror_allocation =
        std::move(current.mirror_allocation);
    transition->previous.sampler = std::move(current.sampler);
  }
  current.mirror_allocation = std::move(target.mirror_allocation);
  current.sampler = std::move(target.sampler);
  return true;
}

std::vector<DescriptorResidencyTarget>
DescriptorHeapMirror::DrainResidencyTargets() {
  std::lock_guard lock(mutex_);
  std::vector<DescriptorResidencyTarget> drained;
  drained.swap(residency_targets_);
  std::unordered_set<obj_handle_t> retained_allocations;
  for (auto &target : drained) {
    const auto retain_once = [&](WMT::Reference<WMT::Resource> &allocation) {
      if (allocation &&
          !retained_allocations.insert(allocation.handle).second)
        allocation = {};
    };
    retain_once(target.allocation);
    retain_once(target.secondary_allocation);
    retain_once(target.mirror_allocation);
  }
  return drained;
}

uint64_t *
DescriptorHeapMirror::TextureHandlePtrUnlocked(uint32_t index) {
  if (sampler_heap_ || !mapped_ || index >= num_descriptors_)
    return nullptr;
  return mapped_ + index;
}

uint64_t *
DescriptorHeapMirror::TextureMetadataPtrUnlocked(uint32_t index) {
  if (sampler_heap_ || !mapped_ || index >= num_descriptors_)
    return nullptr;
  return mapped_ + num_descriptors_ + index;
}

uint64_t *
DescriptorHeapMirror::SamplerHandlePtrUnlocked(uint32_t index) {
  if (!sampler_heap_ || !mapped_ || index >= num_descriptors_)
    return nullptr;
  return mapped_ + index;
}

uint64_t *
DescriptorHeapMirror::SamplerCubeHandlePtrUnlocked(uint32_t index) {
  if (!sampler_heap_ || !mapped_ || index >= num_descriptors_)
    return nullptr;
  return mapped_ + num_descriptors_ + index;
}

uint64_t *
DescriptorHeapMirror::SamplerLodBiasPtrUnlocked(uint32_t index) {
  if (!mapped_ || index >= num_descriptors_)
    return nullptr;
  return mapped_ + uint64_t(num_descriptors_) * 2 + index;
}

std::optional<DescriptorTextureSlotPayload>
DescriptorHeapMirror::textureSlotPayload(
    uint32_t index,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) const {
  std::lock_guard lock(mutex_);
  if (sampler_heap_ || !mapped_ || index >= num_descriptors_ ||
      !IsPublishedVersionUnlocked(index, expected_version))
    return std::nullopt;
  return DescriptorTextureSlotPayload{
      mapped_[index], mapped_[num_descriptors_ + index]};
}

std::optional<DescriptorSamplerSlotPayload>
DescriptorHeapMirror::samplerSlotPayload(
    uint32_t index,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) const {
  std::lock_guard lock(mutex_);
  if (!sampler_heap_ || !mapped_ || index >= num_descriptors_ ||
      !IsPublishedVersionUnlocked(index, expected_version))
    return std::nullopt;
  return DescriptorSamplerSlotPayload{
      mapped_[index], mapped_[num_descriptors_ + index],
      mapped_[uint64_t(num_descriptors_) * 2 + index]};
}

bool
DescriptorHeapMirror::FillSamplerSlot(
    uint32_t index, const Sampler *sampler, uint64_t null_handle,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  auto lock = AcquireLock();
  return FillSamplerSlot(lock, index, sampler, null_handle, expected_version);
}

bool
DescriptorHeapMirror::FillSamplerSlot(
    const ScopedLock &lock, uint32_t index, const Sampler *sampler,
    uint64_t null_handle,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  return FillSamplerSlotUnlocked(index, sampler, null_handle,
                                 expected_version);
}

bool
DescriptorHeapMirror::FillSamplerSlotUnlocked(
    uint32_t index, const Sampler *sampler, uint64_t null_handle,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  if (!CanPublishVersionUnlocked(index, expected_version))
    return false;
  auto *handle = SamplerHandlePtrUnlocked(index);
  auto *cube = SamplerCubeHandlePtrUnlocked(index);
  auto *lod_bias = SamplerLodBiasPtrUnlocked(index);
  if (!handle || !cube || !lod_bias)
    return false;
  uint64_t encoded[kMirrorSamplerQwords] = {};
  if (sampler)
    EncodeMirrorSamplerSlot(encoded, *sampler);
  else
    EncodeMirrorSamplerSlotNull(encoded, null_handle);
  *handle = encoded[0];
  *cube = encoded[1];
  *lod_bias = encoded[2];
  MarkVersionFilledUnlocked(index, expected_version);
  return true;
}

bool
DescriptorHeapMirror::FillTextureSlot(
    uint32_t index, uint64_t gpu_resource_id, uint32_t array_length,
    float min_lod,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  auto lock = AcquireLock();
  return FillTextureSlot(lock, index, gpu_resource_id, array_length, min_lod,
                         expected_version);
}

bool
DescriptorHeapMirror::FillTextureSlot(
    const ScopedLock &lock, uint32_t index, uint64_t gpu_resource_id,
    uint32_t array_length, float min_lod,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  return FillTextureSlotUnlocked(index, gpu_resource_id, array_length, min_lod,
                                 expected_version);
}

bool
DescriptorHeapMirror::FillTextureSlotUnlocked(
    uint32_t index, uint64_t gpu_resource_id, uint32_t array_length,
    float min_lod,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  if (!CanPublishVersionUnlocked(index, expected_version))
    return false;
  auto *handle = TextureHandlePtrUnlocked(index);
  auto *metadata = TextureMetadataPtrUnlocked(index);
  if (!handle || !metadata)
    return false;
  uint64_t encoded[kMirrorTextureQwords] = {};
  EncodeMirrorTextureSlot(encoded, gpu_resource_id, array_length, min_lod);
  *handle = encoded[0];
  *metadata = encoded[1];
  MarkVersionFilledUnlocked(index, expected_version);
  return true;
}

bool
DescriptorHeapMirror::FillTextureSlotPayload(
    uint32_t index, uint64_t handle_payload, uint64_t metadata_payload,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  std::lock_guard lock(mutex_);
  return FillTextureSlotPayloadUnlocked(index, handle_payload,
                                        metadata_payload, expected_version);
}

bool
DescriptorHeapMirror::FillTextureSlotPayloadUnlocked(
    uint32_t index, uint64_t handle_payload, uint64_t metadata_payload,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  if (!CanPublishVersionUnlocked(index, expected_version))
    return false;
  auto *handle = TextureHandlePtrUnlocked(index);
  auto *metadata = TextureMetadataPtrUnlocked(index);
  if (!handle || !metadata)
    return false;
  *handle = handle_payload;
  *metadata = metadata_payload;
  MarkVersionFilledUnlocked(index, expected_version);
  return true;
}

bool
DescriptorHeapMirror::ClearTextureSlot(
    uint32_t index,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  auto lock = AcquireLock();
  return ClearTextureSlot(lock, index, expected_version);
}

bool
DescriptorHeapMirror::ClearTextureSlot(
    const ScopedLock &lock, uint32_t index,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  return ClearTextureSlotUnlocked(index, expected_version);
}

bool
DescriptorHeapMirror::ClearTextureSlotUnlocked(
    uint32_t index,
    std::optional<dxmt::DescriptorSlotVersion> expected_version) {
  if (!CanPublishVersionUnlocked(index, expected_version))
    return false;
  auto *handle = TextureHandlePtrUnlocked(index);
  auto *metadata = TextureMetadataPtrUnlocked(index);
  if (!handle || !metadata)
    return false;
  *handle = 0;
  *metadata = 0;
  MarkVersionFilledUnlocked(index, expected_version);
  return true;
}

dxmt::DescriptorSlotVersion
DescriptorHeapMirror::BeginSlotWrite(uint32_t index) {
  auto lock = AcquireLock();
  return BeginSlotWrite(lock, index);
}

dxmt::DescriptorSlotVersion
DescriptorHeapMirror::BeginSlotWrite(const ScopedLock &lock, uint32_t index) {
  dxmt::perf::ScopedCodeTimer timer(
      dxmt::PerfCodePath::DescriptorTableMirrorMutation);
  ValidateLock(lock);
  if (index >= stale_versions_.size())
    return {};

  auto next = stale_versions_[index];
  if (!next) {
    next = {1, 1};
  } else if (next.sequence != UINT64_MAX) {
    ++next.sequence;
  } else {
    if (next.epoch == UINT64_MAX)
      std::abort();
    ++next.epoch;
    next.sequence = 1;
  }
  stale_versions_[index] = next;
  needs_fill_[index] = 1;
  return next;
}

} // namespace dxmt::d3d12
