#include "dxmt_texture.hpp"
#include "dxmt_format.hpp"
#include "dxmt_residency.hpp"
#include "log/log.hpp"
#include "wsi_platform.hpp"
#include <atomic>
#include <cassert>

namespace dxmt {

std::atomic_uint64_t global_texture_seq = {0};

static void
LogTextureUsageMismatch(
    const char *message, Texture *descriptor, WMT::Texture texture, WMTTextureUsage info_usage,
    WMTResourceOptions options, Flags<TextureAllocationFlag> flags, uint64_t gpu_resource_id
) {
  if (!descriptor || !texture)
    return;

  auto descriptor_usage = descriptor->usage();
  auto actual_usage = texture.usage();
  if (descriptor_usage == actual_usage)
    return;

  WARN(
      message,
      " descriptor=", reinterpret_cast<const void *>(descriptor),
      " descriptor_usage=", uint32_t(descriptor_usage),
      " info_usage=", uint32_t(info_usage),
      " actual_usage=", uint32_t(actual_usage),
      " options=", uint64_t(options),
      " flags=", uint32_t(flags.raw()),
      " gpu_resource_id=", gpu_resource_id,
      " descriptor_format=", uint32_t(descriptor->pixelFormat()),
      " actual_format=", uint32_t(texture.pixelFormat()),
      " actual_size=", texture.width(), "x", texture.height(), "x", texture.depth(),
      " actual_array=", texture.arrayLength(),
      " actual_mips=", texture.mipmapLevelCount()
  );
}

void
TextureView::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
TextureView::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

TextureView::TextureView(TextureAllocation *allocation) :
    texture(allocation->texture()),
    gpuResourceID(allocation->gpuResourceID),
    allocation(allocation),
    key(allocation->descriptor->fullView) {
  LogTextureUsageMismatch(
      "DXMT diagnostic: full texture view usage mismatch", allocation->descriptor, texture,
      allocation->descriptor->usage(), WMTResourceOptions(0), allocation->flags(), gpuResourceID
  );
}

TextureView::TextureView(TextureAllocation *allocation, unsigned index, TextureViewDescriptor descriptor) :
    gpuResourceID(0),
    allocation(allocation),
    key(descriptor, index, allocation->descriptor->miplevelCount()) {
  auto parent = allocation->texture();
  auto parent_format = parent ? parent.pixelFormat() : WMTPixelFormatInvalid;
  auto view_format = descriptor.format;
  bool compressed_srgb_srv =
      !(descriptor.intendedUsage & WMTTextureUsageRenderTarget) &&
      IsBlockCompressionFormat(descriptor.format) &&
      Is_sRGBVariant(descriptor.format) &&
      Forget_sRGB(descriptor.format) == parent_format;
  bool full_parent_view =
      compressed_srgb_srv &&
      descriptor.type == allocation->descriptor->textureType() &&
      descriptor.firstMiplevel == 0 &&
      descriptor.miplevelCount == allocation->descriptor->miplevelCount() &&
      descriptor.firstArraySlice == 0 &&
      descriptor.arraySize == allocation->descriptor->arrayLength();

  if (compressed_srgb_srv) {
    view_format = parent_format;
    if (full_parent_view) {
      texture = parent;
      gpuResourceID = allocation->gpuResourceID;
      return;
    }
  }

  auto view = parent.newTextureView(
      view_format, descriptor.type, descriptor.firstMiplevel, descriptor.miplevelCount,
      descriptor.firstArraySlice, descriptor.arraySize, descriptor.swizzle, gpuResourceID
  );
  auto parent_usage = parent ? parent.usage() : WMTTextureUsageUnknown;
  auto view_usage = view ? view.usage() : WMTTextureUsageUnknown;
  if ((descriptor.intendedUsage & WMTTextureUsageRenderTarget) && view &&
      !(view_usage & WMTTextureUsageRenderTarget) && parent &&
      (parent_usage & WMTTextureUsageRenderTarget) &&
      parent.pixelFormat() == view.pixelFormat() &&
      descriptor.firstMiplevel == 0 && descriptor.firstArraySlice == 0) {
    texture = parent;
    gpuResourceID = allocation->gpuResourceID;
    return;
  }
  texture = std::move(view);
  auto descriptor_usage = allocation->descriptor->usage();
  if ((descriptor.intendedUsage & WMTTextureUsageRenderTarget) && !(view_usage & WMTTextureUsageRenderTarget)) {
    WARN(
        "DXMT diagnostic: texture view missing WMTTextureUsageRenderTarget",
        " descriptor=", reinterpret_cast<const void *>(allocation->descriptor),
        " view=", uint64_t(key),
        " view_index=", index,
        " descriptor_usage=", uint32_t(descriptor_usage),
        " parent_usage=", uint32_t(parent_usage),
        " view_usage=", uint32_t(view_usage),
        " parent_format=", parent ? uint32_t(parent.pixelFormat()) : 0,
        " view_format=", texture ? uint32_t(texture.pixelFormat()) : 0,
        " requested_format=", uint32_t(descriptor.format),
        " requested_type=", uint32_t(descriptor.type),
        " requested_mip=", descriptor.firstMiplevel, "+", descriptor.miplevelCount,
        " requested_slice=", descriptor.firstArraySlice, "+", descriptor.arraySize,
        " gpu_resource_id=", gpuResourceID
    );
  }
}

TextureAllocation::TextureAllocation(
    Texture *descriptor, WMT::Reference<WMT::Buffer> &&buffer, void *mapped_buffer, const WMTTextureInfo &info,
    unsigned bytes_per_row, Flags<TextureAllocationFlag> flags, bool externally_owned_memory
) :
    descriptor(descriptor),
    mappedMemory(mapped_buffer),
    buffer_(std::move(buffer)),
    flags_(flags),
    externally_owned_memory_(externally_owned_memory) {
  auto info_copy = info;
  obj_ = buffer_.newTexture(info_copy, 0, bytes_per_row);

  gpuResourceID = info_copy.gpu_resource_id;
  machPort = 0;
  LogTextureUsageMismatch(
      "DXMT diagnostic: buffer texture allocation usage mismatch", descriptor, obj_, info_copy.usage,
      info_copy.options, flags, gpuResourceID
  );
  fenceTrackers.resize(
      flags.test(TextureAllocationFlag::ShaderReadonly) ? 1 : descriptor->arrayLength() * descriptor->miplevelCount()
  );
};

TextureAllocation::TextureAllocation(
    Texture *descriptor, WMT::Reference<WMT::Texture> &&texture, const WMTTextureInfo &textureDescriptor,
    Flags<TextureAllocationFlag> flags
) :
    descriptor(descriptor),
    obj_(std::move(texture)),
    flags_(flags) {
  mappedMemory = nullptr;
  gpuResourceID = textureDescriptor.gpu_resource_id;
  machPort = textureDescriptor.mach_port;
  LogTextureUsageMismatch(
      "DXMT diagnostic: texture allocation usage mismatch", descriptor, obj_, textureDescriptor.usage,
      textureDescriptor.options, flags, gpuResourceID
  );
  fenceTrackers.resize(
      flags.test(TextureAllocationFlag::ShaderReadonly) ? 1 : descriptor->arrayLength() * descriptor->miplevelCount()
  );
};

TextureAllocation::~TextureAllocation(){
  // Free the caller-supplied host backing on both archs: wrapBuffer's
  // caller (d3d9 buffer-backed) does wsi::aligned_malloc unconditionally
  // and hands the page to us as owner-of-record. The legacy "skip free
  // unless __i386__" guard was correct for allocate(): there x86_64
  // leaves buffer_info.memory null and Metal owns the host backing, so
  // mappedMemory is null and free is a no-op; but it leaks the page
  // for the wrapBuffer path on x86_64 builds. aligned_free(nullptr)
  // is a no-op (both _aligned_free and free), so it's safe to drop the
  // guard entirely. The externally_owned_memory_ branch stays as
  // scaffolding for a future pool-donation hook (caller reclaims the
  // page): no caller flips it today.
  if (!externally_owned_memory_)
    wsi::aligned_free(mappedMemory);
};

void
Texture::prepareAllocationViews(TextureAllocation *allocation) {
  if (allocation->version_ < 1) {
    allocation->cached_view_.push_back(new TextureView(allocation));
    allocation->version_ = 1;
  }
  std::shared_lock<dxmt::shared_mutex> lock(mutex_);
  for (unsigned version = allocation->version_; version < version_; version++) {
    allocation->cached_view_.push_back(new TextureView(allocation, version, viewDescriptors_[version]));
  }
  allocation->version_ = version_;
}

TextureViewKey
Texture::createView(TextureViewDescriptor const &descriptor) {
  std::unique_lock<dxmt::shared_mutex> lock(mutex_);
  unsigned i = 0;
  for (; i < version_; i++) {
    if (viewDescriptors_[i].format != descriptor.format)
      continue;
    if (viewDescriptors_[i].type != descriptor.type)
      continue;
    if (viewDescriptors_[i].firstMiplevel != descriptor.firstMiplevel)
      continue;
    if (viewDescriptors_[i].miplevelCount != descriptor.miplevelCount)
      continue;
    if (viewDescriptors_[i].firstArraySlice != descriptor.firstArraySlice)
      continue;
    if (viewDescriptors_[i].arraySize != descriptor.arraySize)
      continue;
    if (viewDescriptors_[i].intendedUsage != descriptor.intendedUsage)
      continue;
    if (viewDescriptors_[i].swizzle.r != descriptor.swizzle.r ||
        viewDescriptors_[i].swizzle.g != descriptor.swizzle.g ||
        viewDescriptors_[i].swizzle.b != descriptor.swizzle.b ||
        viewDescriptors_[i].swizzle.a != descriptor.swizzle.a)
      continue;
    return TextureViewKey(descriptor, i, info_.mipmap_level_count);
  }
  viewDescriptors_.push_back(descriptor);
  version_ = version_ + 1;
  return TextureViewKey(descriptor, i, info_.mipmap_level_count);
}

Texture::Texture(const WMTTextureInfo &descriptor, WMT::Device device) :
    info_(descriptor),
    device_(device) {

  viewDescriptors_.push_back({
      .format = info_.pixel_format,
      .type = info_.type,
      .firstMiplevel = 0,
      .miplevelCount = info_.mipmap_level_count,
      .firstArraySlice = 0,
      .arraySize = arrayLength(),
      // The default fullView inherits the parent texture's intended
      // usage. Metal's newTextureView with a swizzle parameter can
      // silently strip usage flags (e.g. RenderTarget) from the view's
      // MTLTexture; TextureView's ctor has a workaround that substitutes
      // the parent texture when (intendedUsage & RT) and the view lost
      // RT, but that workaround only fires when intendedUsage carries
      // the bit. Without this default, d3d9's `fullView` produces a view
      // missing RT usage and the RenderPass guard correctly rejects it.
      .intendedUsage = info_.usage,
  });
  version_ = 1;
  fullView = TextureViewKey(viewDescriptors_[0], 0, info_.mipmap_level_count);
}

Texture::Texture(
    unsigned bytes_per_image, unsigned bytes_per_row, const WMTTextureInfo &descriptor, WMT::Device device
) :
    info_(descriptor),
    bytes_per_image_(bytes_per_image),
    bytes_per_row_(bytes_per_row),
    device_(device) {

  assert(info_.type == WMTTextureType2D);
  assert(info_.mipmap_level_count == 1);
  assert(info_.array_length == 1);

  viewDescriptors_.push_back({
      .format = info_.pixel_format,
      .type = info_.type,
      .firstMiplevel = 0,
      .miplevelCount = 1,
      .firstArraySlice = 0,
      .arraySize = 1,
      // Buffer-backed textures inherit the parent's intended usage on
      // the default view for the same reason as the GPU-backed ctor
      // above: TextureView's RT-workaround branch only fires when
      // intendedUsage carries the bit.
      .intendedUsage = info_.usage,
  });
  version_ = 1;
  fullView = TextureViewKey(viewDescriptors_[0], 0, info_.mipmap_level_count);
}

Rc<TextureAllocation>
Texture::allocate(Flags<TextureAllocationFlag> flags) {
  // Default hazard tracking (was forced Untracked). Render-to-texture
  // followed by sample-from-texture in a later encoder requires a
  // barrier that Untracked mode doesn't insert; MTL_SHADER_VALIDATION=1
  // masks the symptom because the validation layer's instrumentation
  // implicitly forces synchronization. Auto-tracking resolves it without
  // per-call barriers.
  WMTResourceOptions options = (WMTResourceOptions)0;
  WMTTextureInfo info = info_; // copy
  info.mach_port = 0;
  if (flags.test(TextureAllocationFlag::CpuWriteCombined)) {
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  }
  if (flags.test(TextureAllocationFlag::CpuInvisible)) {
    options |= WMTResourceStorageModePrivate;
  }
  if (flags.test(TextureAllocationFlag::GpuManaged)) {
    options |= WMTResourceStorageModeManaged;
  }
  if (flags.test(TextureAllocationFlag::PlacementSparse)) {
    info.reserved |= WMTTextureInfoFlagPlacementSparse;
  }
  // Memoryless wins on transient render-targets: never spills to DRAM,
  // contents only live on the tile while the encoder owns it. Apple TBDR
  // best practice for Discard depth/stencil + MSAA color. Overrides the
  // CpuInvisible Private if both happen to be set (callers shouldn't mix,
  // but Memoryless is the stricter mode that subsumes the no-CPU-mapping
  // intent of Private).
  if (flags.test(TextureAllocationFlag::Memoryless)) {
    options = static_cast<WMTResourceOptions>(
        (static_cast<uint64_t>(options) &
         ~static_cast<uint64_t>(WMTResourceStorageModePrivate | WMTResourceStorageModeManaged)) |
        static_cast<uint64_t>(WMTResourceStorageModeMemoryless)
    );
  }
  info.options = options;
  if (bytes_per_image_) {
    WMTBufferInfo buffer_info;
    buffer_info.length = bytes_per_image_;
    buffer_info.options = options;
    buffer_info.memory.set(nullptr);
#ifdef __i386__
    buffer_info.memory.set(wsi::aligned_malloc(bytes_per_image_, DXMT_PAGE_SIZE));
#endif
    auto buffer = device_.newBuffer(buffer_info);
    return new TextureAllocation(this, std::move(buffer), buffer_info.memory.get(), info, bytes_per_row_, flags);
  }
  auto texture = flags.test(TextureAllocationFlag::Shared) ? device_.newSharedTexture(info) : device_.newTexture(info);
  return new TextureAllocation(this, std::move(texture), info, flags);
}

Rc<TextureAllocation>
Texture::wrapBuffer(
    WMT::Reference<WMT::Buffer> buffer, void *mapped, Flags<TextureAllocationFlag> flags
) {
  // Only valid for buffer-backed Textures (constructed with the
  // bytes_per_image/bytes_per_row ctor): the bytes_per_row is what
  // newTexture-on-buffer uses to lay out the texture view.
  assert(bytes_per_image_ != 0);
  // Mirror allocate()'s flag→options mapping so the WMTTextureInfo we
  // pass to TextureAllocation matches the WMTResourceOptions the
  // caller's newBuffer was already configured with. Untracked is not
  // exposed here for the same reason as allocate(): d3d9's buffer-
  // backed path samples after blit-encoder generateMipmaps /
  // replaceRegion, which needs the Tracked default barrier.
  WMTResourceOptions options = (WMTResourceOptions)0;
  if (flags.test(TextureAllocationFlag::CpuWriteCombined))
    options |= WMTResourceOptionCPUCacheModeWriteCombined;
  if (flags.test(TextureAllocationFlag::CpuInvisible))
    options |= WMTResourceStorageModePrivate;
  if (flags.test(TextureAllocationFlag::GpuManaged))
    options |= WMTResourceStorageModeManaged;
  WMTTextureInfo info = info_;
  info.mach_port = 0;
  info.options = options;
  // externally_owned_memory_ stays false (default): the allocation
  // takes ownership of `mapped` and frees it unconditionally in the dtor.
  // Pool-style donation back to the caller is unsafe so long as
  // in-flight chunks may still retain this allocation via the chunk
  // ref_tracker: that's precisely the lifetime gap this whole
  // refactor closes. A future hook on TextureAllocation destruction
  // (which fires after the last ref_tracker release) can re-introduce
  // donation safely; doing it from MTLD3D9Texture's dtor cannot.
  return new TextureAllocation(this, std::move(buffer), mapped, info, bytes_per_row_, flags);
}

Rc<TextureAllocation>
Texture::import(mach_port_t mach_port) {
  Flags<TextureAllocationFlag> flags;
  WMTTextureInfo info;
  info.mach_port = mach_port;
  auto texture = device_.newSharedTexture(info);
  // now allocation's info is populated
  // and we may check if it is consistent with texture's info (it should be)
  if (texture) {
    // doing some unnecessary checks for the sake of completeness
    if (info.options & WMTResourceStorageModeManaged) // should be always false
      flags.set(TextureAllocationFlag::GpuManaged);
    if (info.options & WMTResourceStorageModePrivate) // should be always true
      flags.set(TextureAllocationFlag::GpuPrivate);
    if (info.options & WMTResourceHazardTrackingModeUntracked)
      flags.set(TextureAllocationFlag::NoTracking);
    if ((info.usage & (WMTTextureUsageShaderWrite | WMTTextureUsageRenderTarget)) == 0)
      flags.set(TextureAllocationFlag::ShaderReadonly);
    flags.set(TextureAllocationFlag::Shared);
    return new TextureAllocation(this, std::move(texture), info, flags);
  }
  assert(texture && "failed to import shared texture");
  return nullptr;
}

TextureView &
Texture::view(TextureViewKey key) {
  return view(key, current_.ptr());
}

TextureView &
Texture::view(TextureViewKey key, TextureAllocation* allocation) {
  if (unlikely(allocation->version_ != version_)) {
    prepareAllocationViews(allocation);
  }
  return *allocation->cached_view_[key.index];
}

TextureViewKey Texture::checkViewUseArray(TextureViewKey key, bool isArray) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key.index];
  shared_lock = {};
  static constexpr uint32_t ARRAY_TYPE_MASK = 0b0101001010;
  if (unlikely(bool((1 << uint32_t(view.type)) & ARRAY_TYPE_MASK) != isArray)) {
    // TODO: this process can be cached
    auto new_view_desc = view;
    switch (view.type) {
    case WMTTextureType1D:
      new_view_desc.type = WMTTextureType1DArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType1DArray:
      new_view_desc.type = WMTTextureType1D;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2D:
      new_view_desc.type = WMTTextureType2DArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DArray:
      new_view_desc.type = WMTTextureType2D;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DMultisample:
      new_view_desc.type = WMTTextureType2DMultisampleArray;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureType2DMultisampleArray:
      new_view_desc.type = WMTTextureType2DMultisample;
      new_view_desc.arraySize = 1;
      break;
    case WMTTextureTypeCube:
      new_view_desc.type = WMTTextureTypeCubeArray;
      new_view_desc.arraySize = 6;
      break;
    case WMTTextureTypeCubeArray:
      new_view_desc.type = WMTTextureTypeCube;
      new_view_desc.arraySize = 6;
      break;
    default:
      return key; // should be unreachable
    }
    return createView(new_view_desc);
  }
  return key;
}

TextureViewKey Texture::checkViewUseFormat(TextureViewKey key, WMTPixelFormat format) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key.index];
  shared_lock = {};
  if (unlikely(view.format != format)) {
    auto new_view_desc = view;
    new_view_desc.format = format;
    return createView(new_view_desc);
  }
  return key;
}

TextureViewKey Texture::checkViewUseSwizzle(TextureViewKey key, WMTTextureSwizzleChannels swizzle) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key.index];
  shared_lock = {};
  if (unlikely(
          view.swizzle.r != swizzle.r || view.swizzle.g != swizzle.g || view.swizzle.b != swizzle.b ||
          view.swizzle.a != swizzle.a
      )) {
    auto new_view_desc = view;
    new_view_desc.swizzle = swizzle;
    return createView(new_view_desc);
  }
  return key;
}

TextureViewKey Texture::checkViewUseMipRange(TextureViewKey key, uint32_t firstMiplevel, uint32_t miplevelCount) {
  std::shared_lock<dxmt::shared_mutex> shared_lock(mutex_);
  auto view = viewDescriptors_[key.index];
  shared_lock = {};
  if (unlikely(view.firstMiplevel != firstMiplevel || view.miplevelCount != miplevelCount)) {
    auto new_view_desc = view;
    new_view_desc.firstMiplevel = firstMiplevel;
    new_view_desc.miplevelCount = miplevelCount;
    return createView(new_view_desc);
  }
  return key;
}

Rc<TextureAllocation>
Texture::rename(Rc<TextureAllocation> &&newAllocation) {
  Rc<TextureAllocation> old = std::move(current_);
  current_ = std::move(newAllocation);
  return old;
}

void Texture::incRef(){
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void Texture::decRef(){
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

RenamableTexturePool::RenamableTexturePool(
    Texture *texture, size_t capacity, Flags<TextureAllocationFlag> allocation_flags
) :
    texture_(texture),
    allocations_(capacity, nullptr),
    allocation_flags_(allocation_flags),
    capacity_(capacity) {}

void
RenamableTexturePool::incRef() {
  refcount_.fetch_add(1u, std::memory_order_acquire);
};

void
RenamableTexturePool::decRef() {
  if (refcount_.fetch_sub(1u, std::memory_order_release) == 1u)
    delete this;
};

Rc<TextureAllocation>
RenamableTexturePool::getNext(uint64_t frame) {
  if (frame > last_frame_) {
    last_frame_ = frame;
    current_index_ = 0;
  }
  auto current_index = current_index_++ % capacity_;
  if (!allocations_[current_index].ptr())
    allocations_[current_index] = texture_->allocate(allocation_flags_);
  return allocations_[current_index];
}

} // namespace dxmt
