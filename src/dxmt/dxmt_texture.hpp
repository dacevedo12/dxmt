#pragma once
#include "Metal.hpp"
#include "dxmt_deptrack.hpp"
#include "dxmt_residency.hpp"
#include "dxmt_allocation.hpp"
#include "rc/util_rc_ptr.hpp"
#include "thread.hpp"
#include "util_flags.hpp"
#include "util_svector.hpp"

namespace dxmt {

enum class TextureAllocationFlag : uint32_t {
  GpuReadonly = 0,
  NoTracking = 0,
  GpuPrivate = 1,
  CpuInvisible = 1,
  CpuWriteCombined = 2,
  OwnedByCommandList = 3,
  GpuManaged = 4,
  Shared = 5,
  ShaderReadonly = 6,
  PlacementSparse = 7,
  // Apple TBDR tile-local storage: render-target contents live only on
  // the GPU tile, never in DRAM. Wins on transient depth/stencil + MSAA
  // color where the contents are produced and consumed within one
  // render pass and don't need to survive past it. Caller must use
  // DontCare load/store actions at the render-pass layer.
  Memoryless = 8,
};

struct TextureViewDescriptor {
  WMTPixelFormat format    : 24;
  WMTTextureType type      : 8;
  uint32_t firstMiplevel   : 4 = 0;
  uint32_t miplevelCount   : 4 = 1;
  uint32_t firstArraySlice : 12 = 0;
  uint32_t arraySize       : 12 = 1;
  WMTTextureUsage intendedUsage : 8 = WMTTextureUsageUnknown;
  WMTTextureSwizzleChannels swizzle = {
      WMTTextureSwizzleRed,
      WMTTextureSwizzleGreen,
      WMTTextureSwizzleBlue,
      WMTTextureSwizzleAlpha,
  };
};

struct TextureViewKey {
  union {
    struct {
      uint64_t index       : 28;
      uint64_t mip_count   : 4;
      uint64_t mip_start   : 4;
      uint64_t array_start : 12;
      uint64_t mip_end     : 4;
      uint64_t array_end   : 12;
    };
    uint64_t impl_;
  };

  TextureViewKey() {
    impl_ = 0;
  }
  TextureViewKey(const TextureViewDescriptor &descriptor, unsigned index, unsigned total_mip_count) {
    mip_start = descriptor.firstMiplevel;
    array_start = descriptor.firstArraySlice;
    mip_end = descriptor.firstMiplevel + descriptor.miplevelCount;
    array_end = descriptor.firstArraySlice + descriptor.arraySize;
    mip_count = total_mip_count;
    this->index = index;
  }
  TextureViewKey(uint64_t impl) {
    impl_ = impl;
  }
  operator uint64_t() const {
    return impl_;
  }
};

class Texture;
class TextureAllocation;

class TextureView {
public:
  virtual ~TextureView() {};

  void incRef();
  void decRef();

  WMT::Reference<WMT::Texture> texture;
  uint64_t gpuResourceID;
  DXMT_RESOURCE_RESIDENCY_STATE residency{};
  TextureAllocation *allocation; // `TextureAllocation` holds strong reference to `TextureView`
  TextureViewKey key;

  TextureView(const TextureView &) = delete;
  TextureView(TextureView &&) = delete;
  TextureView &operator=(const TextureView &) = delete;
  TextureView &operator=(TextureView &&) = delete;
  TextureView(TextureAllocation *allocation);
  TextureView(TextureAllocation *allocation, unsigned index, TextureViewDescriptor descriptor);

private:
  std::atomic<uint32_t> refcount_ = {0u};
};

class TextureViewRef : public Rc<TextureView> {
public:
  using Rc<TextureView>::Rc;

  WMT::Texture
  texture() const {
    if (!*this)
      return {};
    return ptr()->texture;
  }

  TextureViewRef &
  operator=(TextureView &ref) {
    return (*this = &ref);
  }
};

class TextureAllocation : public Allocation {
  friend class Texture;

  /**
   * notes on thread-safety:
   * all states in `TextureAllocation` is either immutable or only accessed by `dxmt-encode-thread`
   */

public:

  WMT::Texture texture() const {
    return obj_;
  }

  // Underlying linear MTLBuffer for buffer-backed allocations (the
  // bytes_per_image / bytes_per_row Texture ctor produces these). The
  // d3d9 path needs the buffer handle so its level-0 surface can keep
  // a separate retain on it for the LockRect contract; Lock returns
  // the buffer's mapped bytes as pBits and Unlock is a no-op (UMA).
  // Returns a default-constructed (null) WMT::Buffer for non-buffer-
  // backed allocations.
  WMT::Buffer buffer() const {
    return buffer_;
  }

  Flags<TextureAllocationFlag>
  flags() const {
    return flags_;
  }

  Texture *descriptor;
  void *mappedMemory;
  uint64_t gpuResourceID;
  mach_port_t machPort;
  small_vector<GenericAccessTracker, 1> fenceTrackers;

private:
  TextureAllocation(
      Texture *descriptor, WMT::Reference<WMT::Buffer> &&buffer, void *mapped_buffer, const WMTTextureInfo &info,
      unsigned bytes_per_row, Flags<TextureAllocationFlag> flags, bool externally_owned_memory = false
  );
  TextureAllocation(
      Texture *descriptor, WMT::Reference<WMT::Texture> &&texture, const WMTTextureInfo &textureDescriptor,
      Flags<TextureAllocationFlag> flags
  );
  ~TextureAllocation();

  TextureAllocation(const TextureAllocation &) = delete;
  TextureAllocation(TextureAllocation &&) = delete;

  WMT::Reference<WMT::Texture> obj_;
  WMT::Reference<WMT::Buffer> buffer_;
  uint32_t version_ = 0;
  Flags<TextureAllocationFlag> flags_;
  small_vector<TextureViewRef, 4> cached_view_;
  // Scaffolding for a future pool-donation hook. When true, the dtor
  // skips wsi::aligned_free on mappedMemory because the caller has
  // reclaimed the page (e.g. for re-vending through a device-level
  // reuse pool). No call site sets this today; wrapBuffer always
  // hands ownership to the allocation, which frees on drop. Donating
  // safely needs the donation to fire AFTER the last ref_tracker
  // release. Donating from a texture wrapper destructor is unsafe
  // under the unified path because the ref_tracker may still hold
  // views into the allocation at that point.
  bool externally_owned_memory_ = false;
};

class Texture {

public:
  void incRef();
  void decRef();

  void setDiagnosticIdentity(uint64_t identity) {
    diagnostic_identity_ = identity;
  }

  uint64_t diagnosticIdentity() const {
    return diagnostic_identity_;
  }

  TextureViewKey createView(TextureViewDescriptor const &descriptor);

  constexpr TextureAllocation *
  current() {
    return current_.ptr();
  }

  WMTTextureType
  textureType() const {
    return info_.type;
  }

  WMTPixelFormat
  pixelFormat() const {
    return info_.pixel_format;
  }

  WMTTextureType
  textureType(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return viewDescriptors_[view.index].type;
  }

  WMTPixelFormat
  pixelFormat(TextureViewKey view) {
    std::shared_lock<dxmt::shared_mutex> lock(mutex_);
    return viewDescriptors_[view.index].format;
  }

  WMTTextureUsage
  usage() const {
    return info_.usage;
  }

  // Fix 5 (d3d9 one-shot render-target guard): a process-monotonic,
  // never-reused creation serial. The d3d9 encode-thread RT-recency sets key
  // on this rather than the raw Texture* so a freed-then-realloced Texture at
  // the same address gets a fresh serial and cannot alias into a false
  // "recently drawn" match (which would re-open the very one-shot-RTT hole the
  // guard closes). Inert for non-d3d9 callers.
  uint64_t
  serial() const {
    return creation_serial_;
  }

  unsigned
  sampleCount() const {
    return info_.sample_count;
  }

  unsigned
  width() const {
    return info_.width;
  }

  unsigned
  height() const {
    return info_.height;
  }

  unsigned
  depth() const {
    return info_.depth;
  }

  unsigned
  width(TextureViewKey view) {
    return std::max(info_.width >> view.mip_start, 1u);
  }

  unsigned
  height(TextureViewKey view) {
    return std::max(info_.height >> view.mip_start, 1u);
  }

  /**
  \warning for cube texture, this would be multiple of 6.
  */
  unsigned
  arrayLength() const {
    switch (info_.type) {
    case WMTTextureTypeCubeArray:
    case WMTTextureTypeCube:
      return info_.array_length * 6;
    default:
      break;
    }
    return info_.array_length;
  }

  unsigned
  arrayLength(TextureViewKey view) {
    return view.array_end - view.array_start;
  }

  unsigned
  miplevelCount() const {
    return info_.mipmap_level_count;
  }

  TextureViewKey fullView;

  Rc<TextureAllocation> allocate(Flags<TextureAllocationFlag> flags);
  Rc<TextureAllocation> allocatePlaced(WMT::Heap heap, uint64_t offset,
                                       Flags<TextureAllocationFlag> flags);
  Rc<TextureAllocation> import(mach_port_t mach_port);

  // Buffer-backed allocation that adopts a caller-supplied (buffer,
  // mapped) pair instead of calling device_.newBuffer(). Only valid on
  // a Texture constructed via the buffer-backed ctor below (i.e. where
  // bytes_per_image was supplied). The returned allocation owns both
  // the buffer (its WMT::Reference is moved in) and the mapped page
  // (freed via wsi::aligned_free on drop), and stays alive across
  // chunk encode → GPU completion via the chunk ref_tracker. Used by
  // d3d9's buffer-backed CreateTexture where LockRect must hand the
  // i386 game its 32-bit-addressable pBits; the caller does the
  // wsi::aligned_malloc + newBuffer itself (it may pool-acquire
  // instead of malloc) and passes the pair through here.
  Rc<TextureAllocation> wrapBuffer(
      WMT::Reference<WMT::Buffer> buffer, void *mapped, Flags<TextureAllocationFlag> flags
  );

  TextureView &view(TextureViewKey key);
  TextureView &view(TextureViewKey key, TextureAllocation *allocation);
  uint64_t setViewPoolSlot(WMT::TextureViewPool pool, TextureViewKey key,
                           TextureAllocation *allocation, uint32_t slot);

  TextureViewKey checkViewUseArray(TextureViewKey key, bool isArray);
  TextureViewKey checkViewUseFormat(TextureViewKey key, WMTPixelFormat format);
  // Derive a view that applies a per-channel sample swizzle (d3d9
  // X-channel / luminance / depth-replicate formats). Returns `key`
  // unchanged when the swizzle already matches. See checkViewUseFormat.
  TextureViewKey checkViewUseSwizzle(TextureViewKey key, WMTTextureSwizzleChannels swizzle);
  // Derive a view clamped to mips [firstMiplevel, firstMiplevel+count).
  // d3d9 SetLOD(N) clamps sampling to mips N..(level_count-1). Returns
  // `key` unchanged when the range already matches.
  TextureViewKey checkViewUseMipRange(TextureViewKey key, uint32_t firstMiplevel, uint32_t miplevelCount);

  Rc<TextureAllocation> rename(Rc<TextureAllocation> &&newAllocation);

  Texture(const WMTTextureInfo &info, WMT::Device device);

  Texture(uint64_t bytes_per_image, uint32_t bytes_per_row,
          const WMTTextureInfo &info, WMT::Device device);

private:
  uint64_t diagnostic_identity_ = 0;
  void prepareAllocationViews(TextureAllocation* allocation);

  // See serial(). Assigned once at construction from a process-wide monotonic
  // counter; relaxed fetch_add is enough since the recency key needs
  // uniqueness, not ordering. Both ctors pick it up via the member's default
  // initializer below.
  static uint64_t
  nextCreationSerial() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
  }

  WMTTextureInfo info_;
  uint64_t bytes_per_image_ = 0;
  uint32_t bytes_per_row_ = 0;
  uint64_t creation_serial_ = nextCreationSerial();

  Rc<TextureAllocation> current_;
  std::atomic<uint32_t> refcount_ = {0u};

  small_vector<TextureViewDescriptor, 4> viewDescriptors_;
  dxmt::shared_mutex mutex_;
  WMT::Device device_;
};

class RenamableTexturePool {
public:
  void incRef();
  void decRef();
  Rc<TextureAllocation> getNext(uint64_t frame_);

  RenamableTexturePool(Texture *texture, size_t capacity,Flags<TextureAllocationFlag> allocation_flags);

private:
  Rc<Texture> texture_;
  std::vector<Rc<TextureAllocation>> allocations_;
  uint64_t last_frame_ = 0;
  Flags<TextureAllocationFlag> allocation_flags_;
  unsigned capacity_;
  unsigned current_index_ = 0;
  std::atomic<uint32_t> refcount_ = {0u};
};

} // namespace dxmt
