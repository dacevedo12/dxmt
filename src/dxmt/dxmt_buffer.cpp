#include "dxmt_buffer.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_likely.hpp"
#include "util_math.hpp"
#include "wsi_platform.hpp"
#include <cassert>
#include <mutex>

namespace dxmt {

std::atomic_uint64_t global_buffer_seq = {0};

// See cpu_placed_peak_bytes (dxmt_buffer.hpp). Live is the sum of the backing
// bytes of every CpuPlaced allocation currently constructed; peak is its
// high-water. Both relaxed: the values are advisory diagnostics, not a
// synchronization edge.
static std::atomic<uint64_t> g_cpu_placed_live_bytes = {0};
static std::atomic<uint64_t> g_cpu_placed_peak_bytes = {0};

static void
noteCpuPlacedAlloc(uint64_t bytes) {
  uint64_t live = g_cpu_placed_live_bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  uint64_t peak = g_cpu_placed_peak_bytes.load(std::memory_order_relaxed);
  while (live > peak && !g_cpu_placed_peak_bytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
  }
}

static void
noteCpuPlacedFree(uint64_t bytes) {
  g_cpu_placed_live_bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

uint64_t
cpu_placed_peak_bytes() {
  return g_cpu_placed_peak_bytes.load(std::memory_order_relaxed);
}

static bool
ShouldLogBufferTextureViewFailure() {
  static std::atomic<uint32_t> count = 0;
  return count.fetch_add(1, std::memory_order_relaxed) < 32;
}

static WMTResourceOptions
GetBufferResourceOptions(Flags<BufferAllocationFlag> flags) {
  WMTResourceOptions options = WMTResourceHazardTrackingModeUntracked;
  if (flags.test(BufferAllocationFlag::CpuWriteCombined))
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  if (flags.test(BufferAllocationFlag::CpuInvisible))
    options |= WMTResourceStorageModePrivate;
  if (flags.test(BufferAllocationFlag::GpuManaged))
    options |= WMTResourceStorageModeManaged;
  return options;
}

static void
NormalizeBufferLength(WMTBufferInfo &info) {
  info.length = align(std::max(info.length, 256ull), 256ull);
}

BufferAllocation::BufferAllocation(WMT::Device device, const WMTBufferInfo &info, Flags<BufferAllocationFlag> flags) :
    info_(info),
    flags_(flags) {
  // (sub)allocate a minimum of 256B buffer so that texture can be created
  NormalizeBufferLength(info_);
  suballocation_size_ = info_.length;
  if (flags_.test(BufferAllocationFlag::SuballocateFromOnePage) && suballocation_size_ <= DXMT_PAGE_SIZE) {
    suballocation_size_ = align(info_.length, 16);
    suballocation_count_ = DXMT_PAGE_SIZE / suballocation_size_;
    info_.length = DXMT_PAGE_SIZE;
  }
  fenceTrackers.resize(suballocation_count_);
  if (flags_.test(BufferAllocationFlag::CpuPlaced) || flags_.test(BufferAllocationFlag::CpuShadow)) {
    placed_buffer = wsi::aligned_malloc(info_.length, DXMT_PAGE_SIZE);
    if (flags_.test(BufferAllocationFlag::CpuPlaced))
      info_.memory.set(placed_buffer);
  }
  obj_ = device.newBuffer(info_);
  gpuAddress_ = info_.gpu_address;
  mappedMemory_ = flags_.test(BufferAllocationFlag::CpuShadow)
                    ? placed_buffer
                    : info_.memory.get_accessible_or_null();
  // CpuPlaced registers an app-owned host backing with Metal and so charges
  // low-4GB address space; count it against the process high-water.
  if (flags_.test(BufferAllocationFlag::CpuPlaced))
    noteCpuPlacedAlloc(info_.length);
};

BufferAllocation::BufferAllocation(
    WMT::Reference<WMT::Buffer> &&buffer, const WMTBufferInfo &info,
    Flags<BufferAllocationFlag> flags)
    : obj_(std::move(buffer)), info_(info), flags_(flags),
      mappedMemory_(info_.memory.get_accessible_or_null()),
      gpuAddress_(info_.gpu_address), suballocation_size_(info_.length) {
  fenceTrackers.resize(1);
}

BufferAllocation::~BufferAllocation() {
  if (flags_.test(BufferAllocationFlag::CpuPlaced))
    noteCpuPlacedFree(info_.length);
  if (placed_buffer && !flags_.test(BufferAllocationFlag::ExternalCpuPlaced)) {
    wsi::aligned_free(placed_buffer);
    placed_buffer = nullptr;
  }
}

void
BufferAllocation::flushCpuShadow(uint64_t offset, uint64_t length, uint32_t suballocation) noexcept {
  if (!flags_.test(BufferAllocationFlag::CpuShadow) || !mappedMemory_ || !length)
    return;
  if (unlikely(suballocation >= suballocation_count_ || offset >= suballocation_size_))
    return;
  auto max_length = suballocation_size_ - offset;
  if (length > max_length)
    length = max_length;
  auto absolute_offset = suballocation * suballocation_size_ + offset;
  obj_.updateContents(
      absolute_offset,
      reinterpret_cast<const char *>(mappedMemory_) + absolute_offset,
      length
  );
}

WMT::Texture
Buffer::view(BufferViewKey key) {
  return view(key, current_.ptr());
};

WMT::Texture
Buffer::view(BufferViewKey key, BufferAllocation *allocation) {
  if (auto *view = tryView(key, allocation))
    return view->texture;
  return {};
};

BufferView const &
Buffer::view_(BufferViewKey key) {
  return view_(key, current_.ptr());
};

BufferView const &
Buffer::view_(BufferViewKey key, BufferAllocation *allocation) {
  auto *view = tryView(key, allocation);
  if (likely(view != nullptr))
    return *view;

  WARN("DXMT: invalid buffer texture view requested view=", key,
       " cached_views=", allocation ? allocation->cached_view_.size() : 0,
       " allocation=", reinterpret_cast<const void *>(allocation),
       " length=", length_);
  static BufferView null_view({}, 0, 0);
  return null_view;
};

BufferView *
Buffer::tryView(BufferViewKey key, BufferAllocation *allocation) {
  if (unlikely(!allocation))
    return nullptr;
  if (unlikely(key >= allocation->cached_view_.size())) {
    prepareAllocationViews(allocation);
  }
  if (unlikely(key >= allocation->cached_view_.size()))
    return nullptr;
  return allocation->cached_view_[key].get();
};

DXMT_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key) {
  return residency(key, current_.ptr());
}

DXMT_RESOURCE_RESIDENCY_STATE &
Buffer::residency(BufferViewKey key, BufferAllocation *allocation) {
  if (auto *view = tryView(key, allocation))
    return view->residency;

  WARN("DXMT: invalid buffer texture view residency requested view=", key,
       " cached_views=", allocation ? allocation->cached_view_.size() : 0,
       " allocation=", reinterpret_cast<const void *>(allocation),
       " length=", length_);
  static DXMT_RESOURCE_RESIDENCY_STATE null_residency = {};
  return null_residency;
}

void
Buffer::prepareAllocationViews(BufferAllocation *allocation) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  const auto descriptor_count = viewDescriptors_.size();
  for (unsigned version = allocation->version_; version < descriptor_count; version++) {
    auto descriptor = viewDescriptors_[version];
    auto format = descriptor.format;
    auto texel_size = MTLGetTexelSize(format);
    assert(texel_size);
    assert(!(allocation->suballocation_size_ & (texel_size - 1)));
    auto total_length = descriptor.byteLength ? descriptor.byteLength
                                              : allocation->suballocation_size_ * allocation->suballocation_count_;
    WMTTextureInfo info = {};
    info.type = descriptor.type;
    info.width = total_length / (uint64_t)texel_size;
    info.height = 1;
    info.depth = 1;
    info.array_length = 1;
    info.mipmap_level_count = 1;
    info.sample_count = 1;
    info.pixel_format = format;
    info.options = allocation->info_.options;
    auto usage = descriptor.usage;
    if (!allocation->flags().test(BufferAllocationFlag::GpuReadonly) &&
       ( allocation->flags().test(BufferAllocationFlag::GpuManaged) ||  allocation->flags().test(BufferAllocationFlag::GpuPrivate))) {
      if (usage & WMTTextureUsageShaderWrite) {
        if (format == WMTPixelFormatR32Uint || format == WMTPixelFormatR32Sint ||
            (format == WMTPixelFormatRG32Uint && device_.supportsFamily(WMTGPUFamilyApple8))) {
          usage |= WMTTextureUsageShaderAtomic;
        }
      }
    }
    info.usage = usage;

    auto view = allocation->obj_.newTexture(info, descriptor.byteOffset, total_length);
    if (!view && ShouldLogBufferTextureViewFailure()) {
      WARN("DXMT: failed to create buffer texture view"
           " version=", version,
           " format=", uint32_t(format),
           " type=", uint32_t(descriptor.type),
           " usage=", uint32_t(usage),
           " byteOffset=", descriptor.byteOffset,
           " byteLength=", total_length,
           " bytesPerRow=", total_length,
           " width=", info.width,
           " texelSize=", texel_size,
           " alignment=",
           device_.minimumLinearTextureAlignmentForPixelFormat(format),
           " allocationLength=", allocation->info_.length,
           " suballocationSize=", allocation->suballocation_size_,
           " suballocationCount=", allocation->suballocation_count_,
           " options=", uint32_t(allocation->info_.options));
    }

    allocation->cached_view_.push_back(std::make_unique<BufferView>(
        std::move(view), info.gpu_resource_id, allocation->suballocation_size_ / texel_size
    ));
  }
  allocation->version_ = descriptor_count;
};

BufferViewKey
Buffer::createView(BufferViewDescriptor const &descriptor) {
  std::unique_lock<dxmt::mutex> lock(mutex_);
  unsigned i = 0;
  for (; i < viewDescriptors_.size(); i++) {
    auto &existing = viewDescriptors_[i];
    if (existing.format == descriptor.format &&
        existing.usage == descriptor.usage &&
        existing.type == descriptor.type &&
        existing.byteOffset == descriptor.byteOffset &&
        existing.byteLength == descriptor.byteLength) {
      return i;
    }
  }
  viewDescriptors_.push_back(descriptor);
  return i;
}

Rc<BufferAllocation>
Buffer::allocate(Flags<BufferAllocationFlag> flags) {
  WMTBufferInfo info = {};
  info.memory.set(0);
  info.length = length_;
  info.options = GetBufferResourceOptions(flags);
  return new BufferAllocation(device_, info, flags);
};

Rc<BufferAllocation>
Buffer::allocatePlaced(WMT::Heap heap, uint64_t offset,
                       Flags<BufferAllocationFlag> flags) {
  if (!heap)
    return nullptr;

  WMTBufferInfo info = {};
  info.memory.set(nullptr);
  info.length = length_;
  info.options = GetBufferResourceOptions(flags);
  NormalizeBufferLength(info);
  auto buffer = WMT::Reference<WMT::Buffer>(
      MTLHeap_newBuffer(heap.handle, &info, offset));
  if (!buffer)
    return nullptr;
  return new BufferAllocation(std::move(buffer), info, flags);
}

Rc<BufferAllocation>
Buffer::allocateExternalCpu(Flags<BufferAllocationFlag> flags, void *memory) {
  WMTBufferInfo info = {};
  info.memory.set(memory);
  info.length = length_;
  info.options = GetBufferResourceOptions(flags);
  flags.set(BufferAllocationFlag::ExternalCpuPlaced);
  return new BufferAllocation(device_, info, flags);
};

Rc<BufferAllocation>
Buffer::rename(Rc<BufferAllocation> &&newAllocation) {
  Rc<BufferAllocation> old = std::move(current_);
  current_ = std::move(newAllocation);
  return old;
}

void
Buffer::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
Buffer::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

} // namespace dxmt
