#include "d3d9_texture.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_image_lock.hpp"
#include "d3d9_private_data.hpp"
#include "d3d9_resource_priority.hpp"
#include "wsi_platform.hpp"

#include <algorithm>

namespace dxmt {

// Shared per-level setup. The ctor stashes m_textureRaw +
// m_dxmtTexture (which owns the underlying buffer + mapped pointer for
// the buffer-backed flavour) then calls in here so the
// surface array eager-creation and sysmem mirror logic only lives in
// one place. backingBuffer/backingPtr/bufferPitch are extracted from
// dxmtTexture's allocation by the caller for the buffer-backed case;
// they're zero/null on the regular path.
static void
buildLevelsAndMirror(
    MTLD3D9Texture *self, MTLD3D9Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, WMT::Texture parentTex, WMT::Buffer backingBuffer, void *backingPtr, uint32_t bufferPitch,
    const Rc<dxmt::Texture> &dxmtTexture, WMT::Reference<WMT::Buffer> &mirrorBufferOut, void *&mirrorBackingOut,
    std::vector<size_t> &mirrorOffsets, std::vector<Com<MTLD3D9Surface, false>> &levelsOut,
    WMTPixelFormat &metalFormatOut, void *userMemory
) {
  // A texture-less mirror (packed-YUV SCRATCH: no Metal format to back it)
  // has no parent to read a pixel format from; the levels are CPU-only.
  metalFormatOut = parentTex ? parentTex.pixelFormat() : WMTPixelFormatInvalid;

  const bool buffer_backed = (backingBuffer != nullptr);
  // D3D9Ex user-memory (single level): the app pointer is the packed CPU
  // master. Seed it as the mirror base so mirrorBase() / UpdateTexture
  // source from it directly and ensureMirror early-outs; level 0 aliases
  // it for LockRect below. No mirror buffer, no copy.
  if (userMemory)
    mirrorBackingOut = userMemory;
  // D3DUSAGE_DYNAMIC DEFAULT textures (video planes, dynamic normal maps)
  // are LockRect'd every frame to stream data; per MSDN a DYNAMIC texture
  // is lockable regardless of pool. Give them the same sysmem mirror +
  // upload-on-unlock as MANAGED: the upload-ring snapshot in
  // MTLD3D9Surface::UnlockRect makes the persistent mirror safe to re-lock
  // each frame (DXVK's per-lock-staging shape), so no DISCARD rename-ring
  // is needed here. RT/DS DEFAULT textures stay GPU-only.
  // 3Dc is mappable regardless of pool or the dynamic flag (wined3d marks
  // the vendor FOURCCs MAPPABLE; wine's resource-access test locks a plain
  // DEFAULT ATI2 texture), so DEFAULT 3Dc rides the same mirror.
  // DOC-DIV (vs DXVK): the RENDERTARGET/DEPTHSTENCIL exclusion below keeps the
  // DEFAULT-pool depth vendor FOURCCs (DF16/DF24/INTZ) unlockable. dxmt matches
  // the PRIMARY reference here (wined3d gives DS-usage textures no map access);
  // DXVK deliberately widened its IsVendorFormat arm to make every vendor FOURCC
  // lockable. Those formats are bind-and-sample shadow aliases, not locked, so
  // dxmt sits with wined3d.
  const bool needs_mirror =
      !buffer_backed && (pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH ||
                         (pool == D3DPOOL_DEFAULT && ((usage & D3DUSAGE_DYNAMIC) || Is3DcFormat(format)) &&
                          !(usage & D3DUSAGE_RENDERTARGET) && !(usage & D3DUSAGE_DEPTHSTENCIL)));
  mirrorOffsets.resize(levels + 1u);
  size_t total_bytes = 0;
  for (UINT i = 0; i < levels; ++i) {
    UINT level_w = std::max<UINT>(1u, width >> i);
    UINT level_h = std::max<UINT>(1u, height >> i);
    mirrorOffsets[i] = total_bytes;
    // The mirror stride is the 4-byte-aligned row pitch (D3DFormatLockPitch),
    // the value LockRect reports and native D3D9 lays rows out at; a tight
    // pitch would diverge from the reported one for sub-DWORD-width rows. The
    // aligned stride is a no-op for block-compressed and 4-bpp formats.
    total_bytes += static_cast<size_t>(D3DFormatLockPitch(format, level_w)) *
                   static_cast<size_t>(D3DFormatRowCount(format, level_h));
  }
  // 3Dc mirror pad: the levels above are sized by the linear 1-byte fiction
  // (LockPitch x RowCount), but stageTextureUpload's BC arm reads the BC-real
  // footprint (MetalTransferPitch x MetalTransferRows) from the last level's
  // offset, which for a sub-4x4 tail mip exceeds the fiction allocation and
  // overreads past the mirror. Pad the total so the last level holds its BC
  // footprint (wined3d texture.c wined3d_texture_init pads its BROKEN_PITCH
  // sysmem allocation by one block for exactly this). Intermediate tail mips
  // overread into the next level's region, harmless and bug-compatible with
  // wined3d; the sub-4x4 content stays fiction-quality either way.
  if (Is3DcFormat(format) && levels > 0) {
    UINT last_w = std::max<UINT>(1u, width >> (levels - 1));
    UINT last_h = std::max<UINT>(1u, height >> (levels - 1));
    size_t bc_footprint = static_cast<size_t>(D3DFormatMetalTransferPitch(format, last_w)) *
                          static_cast<size_t>(D3DFormatMetalTransferRows(format, last_h));
    size_t min_total = mirrorOffsets[levels - 1] + bc_footprint;
    if (min_total > total_bytes)
      total_bytes = min_total;
  }
  mirrorOffsets[levels] = total_bytes;
  // Mirror alloc is deferred to the first LockRect on any level
  // surface; see MTLD3D9Texture::ensureMirror. Apps that batch-create
  // textures up front (boot-time atlas builds) avoid paying
  // wsi::aligned_malloc + memset + Metal newBuffer (wine_unix_call) per
  // texture before the data even exists. Per-Lock cost is the same;
  // the win is on the cold-create path (audit M-PERF #2).
  (void)total_bytes;
  (void)device;
  (void)mirrorBufferOut;
  (void)mirrorBackingOut;

  levelsOut.reserve(levels);
  for (UINT i = 0; i < levels; ++i) {
    UINT level_w = std::max<UINT>(1u, width >> i);
    UINT level_h = std::max<UINT>(1u, height >> i);

    D3DSURFACE_DESC desc{};
    desc.Format = format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = usage;
    desc.Pool = pool;
    desc.MultiSampleType = D3DMULTISAMPLE_NONE;
    desc.MultiSampleQuality = 0;
    desc.Width = level_w;
    desc.Height = level_h;

    void *cpu_ptr = nullptr;
    uint32_t pitch = 0;
    WMT::Reference<WMT::Buffer> level_buffer;
    const bool user_mem_level = (userMemory != nullptr) && (i == 0);
    if (buffer_backed && i == 0) {
      // buffer-backed level-0: surface aliases the backing buffer's
      // bytes; Lock returns the wsi::aligned_malloc'd pointer +
      // aligned pitch; Unlock fires m_buffer != nullptr gate and
      // skips stageTextureUpload.
      cpu_ptr = backingPtr;
      pitch = bufferPitch;
      level_buffer = WMT::Reference<WMT::Buffer>(backingBuffer);
    } else if (user_mem_level) {
      // D3D9Ex user-memory level-0: alias the app pointer with a tightly
      // packed pitch (the runtime expects packed data, never padded).
      // m_buffer stays null so a write Unlock takes the dirty-record
      // path; the GPU side is fed by UpdateTexture from mirrorBase().
      cpu_ptr = userMemory;
      pitch = D3DFormatRowPitch(format, level_w);
    }
    // For needs_mirror surfaces (MANAGED/SYSTEMMEM/SCRATCH non-buffer-
    // backed), cpu_ptr/mirror_src/pitch stay null until ensureMirror
    // runs; m_lazyMirrorParent set below routes LockRect through it.

    auto *level = new MTLD3D9Surface(
        device, desc,
        /*container=*/static_cast<IDirect3DBaseTexture9 *>(self),
        WMT::Reference<WMT::Texture>(parentTex), // independent retain on the same NSObject
        /*mipLevel=*/i,
        /*selfPin=*/false,
        /*parentTextureType=*/WMTTextureType2D,
        /*buffer=*/std::move(level_buffer),
        /*cpuPtr=*/cpu_ptr,
        /*pitch=*/pitch,
        /*arraySlice=*/0,
        /*ownedBacking=*/nullptr,
        /*dxmtTexture=*/dxmtTexture,
        // Relaxed double-Unlock contract: wined3d surface.c only
        // softens the gate for D3DRTYPE_TEXTURE containers (not cube,
        // not standalone, not swapchain). Surfaces vended via
        // GetSurfaceLevel route the second Unlock into D3D_OK; the
        // INVALIDCALL elsewhere still catches genuine bookkeeping bugs.
        /*textureMipSurface=*/true,
        // Sub-resource: the level shares this texture's public refcount.
        /*baseTexture=*/static_cast<IDirect3DBaseTexture9 *>(self)
    );
    if (needs_mirror && !user_mem_level)
      level->setLazyMirrorParent(self, i);
    levelsOut.emplace_back(level);
  }
}

MTLD3D9Texture::MTLD3D9Texture(
    MTLD3D9Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    Rc<dxmt::Texture> texture, uint32_t bufferPitch, void *userMemory
) :
    m_device(device),
    m_textureRaw(texture ? WMT::Reference<WMT::Texture>(texture->current()->texture()) : WMT::Reference<WMT::Texture>()),
    m_dxmtTexture(std::move(texture)),
    m_userMemory(userMemory != nullptr),
    m_usage(usage),
    m_pool(pool),
    m_format(format),
    m_width(width),
    m_height(height) {
  AddRefPrivate();
  // m_dirty_any = true by default-init; set the union rect to the full
  // level-0 extent so first consumer pass sees the whole texture as the
  // dirty region.
  m_dirty_rect = RECT{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
  // Buffer-backed flavour: bufferPitch > 0 signals that
  // m_dxmtTexture's allocation owns the wsi::aligned_malloc'd page and
  // the Metal buffer wrapping it. Lift those out for buildLevelsAndMirror
  // so the level-0 surface can alias LockRect's pBits onto the same
  // bytes. The allocation retains the buffer; passing parentTex (non-
  // retaining) is fine because the buffer's Metal lifetime tracks the
  // allocation's, not this scope's.
  WMT::Buffer levelBackingBuffer{};
  void *levelBackingPtr = nullptr;
  if (bufferPitch != 0) {
    auto *alloc = m_dxmtTexture->current();
    levelBackingBuffer = alloc->buffer();
    levelBackingPtr = alloc->mappedMemory;
  }
  buildLevelsAndMirror(
      this, m_device, width, height, levels, usage, format, pool, m_textureRaw, levelBackingBuffer, levelBackingPtr,
      bufferPitch, m_dxmtTexture, m_mirrorBuffer, m_mirrorBacking, m_mirrorOffsets, m_levels, m_metalFormat, userMemory
  );
  // One bit per mip level for the MANAGED mirror-eviction bookkeeping; a
  // 15-level (16K) texture fits a uint32 with room to spare.
  m_all_levels_mask = levels >= 32 ? ~0u : ((1u << levels) - 1u);
  // A freshly created MANAGED texture is fully dirty: its GPU copy holds nothing
  // the app has written, so every level needs an upload from the sysmem master
  // on first use (wined3d marks all sub-resources dirty at create; DXVK
  // SetAllNeedUpload). The pre-draw managed sweep consumes this; a plain Unlock
  // clears each level's bit as it eagerly uploads (keeping the common
  // single-level fill byte-identical), so the sweep only pushes levels written
  // but never individually Unlocked (the level-0-locks-every-level idiom) plus
  // any NO_DIRTY_UPDATE write left resident on a fresh master.
  m_needs_upload_mask = (m_pool == D3DPOOL_MANAGED) ? m_all_levels_mask : 0u;
  // Point every level surface at this texture's shared Lock/GetDC state so a
  // GetDC or LockRect coordinates texture-wide (wined3d resource.map_count).
  for (auto &level : m_levels)
    level->setSharedLockState(&m_shared_lock_state);
}

void
MTLD3D9Texture::ensureMirror() {
  // Idempotent: first call allocates, subsequent calls early-out.
  if (m_mirrorBacking != nullptr)
    return;
  // Pools that never need a mirror. DEFAULT-pool textures live entirely
  // on the GPU; the legacy buffer-backed path aliases
  // level 0 onto a wsi::aligned_malloc'd buffer that IS the mirror. The
  // exception is D3DUSAGE_DYNAMIC DEFAULT textures (gated into needs_mirror
  // in buildLevelsAndMirror): the app LockRects them per frame to stream
  // data, so they get a sysmem mirror + upload-on-unlock like MANAGED.
  // 3Dc stays mappable on DEFAULT without the dynamic flag (the vendor
  // FOURCCs are MAPPABLE in wined3d's tables), same as the ctor-side
  // needs_mirror rule.
  if (m_pool == D3DPOOL_DEFAULT && !(m_usage & D3DUSAGE_DYNAMIC) && !Is3DcFormat(m_format))
    return;
  // Buffer-backed: level 0 already aliases the
  // wsi-malloc'd page owned by m_dxmtTexture's allocation, so the
  // mirror would be a redundant second copy. The presence of an
  // allocation-side buffer is the universal "buffer-backed" predicate
  // now that the legacy ctor is gone.
  if (m_dxmtTexture && m_dxmtTexture->current() && m_dxmtTexture->current()->buffer() != nullptr)
    return;
  if (m_mirrorOffsets.empty())
    return;
  const size_t total_bytes = m_mirrorOffsets.back();
  if (total_bytes == 0)
    return;

  // Try the device-level buffer-backing pool first. On hit we skip
  // the newBuffer XPC + first-touch memset cliff (a freshly-destroyed
  // texture of the same size hands its mirror back; LockRect on the
  // mirror will overwrite stale pages, same correctness model as the
  // VB pool). Cold path falls through to aligned_malloc + newBuffer.
  uint64_t mirror_gpu_addr = 0;
  void *mirror_host = nullptr;
  if (!m_device->acquireBufferBacking(total_bytes, m_mirrorBuffer, mirror_gpu_addr, mirror_host, m_mirrorBacking)) {
    m_mirrorBacking = wsi::aligned_malloc(total_bytes, DXMT_PAGE_SIZE);
    if (!m_mirrorBacking)
      return;
    std::memset(m_mirrorBacking, 0, total_bytes);

    WMTBufferInfo binfo{};
    binfo.length = total_bytes;
    // Shared storage so the backing can be donated to and reused from the
    // device block pool (releaseBufferBacking / acquireBufferBacking, whose
    // entries require a Metal registration). The GPU never reads this buffer:
    // UnlockRect snapshots the mirror bytes through the upload ring, and
    // readbackSurfaceMirror copies GPU texels back into the CPU backing.
    binfo.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
    binfo.memory.set(m_mirrorBacking);
    m_mirrorBuffer = m_device->metalDevice().newBuffer(binfo);
    if (m_mirrorBuffer == nullptr) {
      wsi::aligned_free(m_mirrorBacking);
      m_mirrorBacking = nullptr;
      return;
    }
  }

  // Patch every level surface: fills in cpu_ptr and pitch (computed per-level
  // from m_format and the dimension cached at ctor time).
  for (UINT i = 0; i < m_levels.size(); ++i) {
    void *level_ptr = static_cast<uint8_t *>(m_mirrorBacking) + m_mirrorOffsets[i];
    UINT level_w = std::max<UINT>(1u, m_width >> i);
    // Aligned stride, matching the mirror sizing in buildLevelsAndMirror.
    uint32_t pitch = D3DFormatLockPitch(m_format, level_w);
    m_levels[i]->patchMirror(level_ptr, pitch);
  }
}

// An app that reads a MANAGED texture back more than this many times keeps its
// mirror resident: re-downloading on every read would thrash. Mirrors wined3d's
// download_count > WINED3D_TEXTURE_DYNAMIC_MAP_THRESHOLD guard in evict_sysmem.
static constexpr uint32_t kMirrorReadEvictThreshold = 4;

void
MTLD3D9Texture::dropMirror() {
  if (m_userMemory || m_mirrorBacking == nullptr)
    return;
  // The level surfaces share one backing, and D3D9 lets an app hold two mip
  // levels locked at once: freeing while any level is locked would yank the
  // backing out from under a live pBits. Skip; the next write-Unlock retries
  // (m_uploaded_mask stays complete until the drop actually happens).
  for (auto &level : m_levels)
    if (level->locked())
      return;
  // The GPU never samples the mirror buffer (UnlockRect snapshots into the
  // upload ring rather than blitting from the mirror), so releasing it cannot
  // race in-flight work. Null every level surface's cpu_ptr so the next Lock
  // re-arms ensureMirror, then hand the backing to the device pool (the byte
  // cap there returns it to the OS when the pool is over budget).
  for (auto &level : m_levels)
    level->clearMirrorPatch();
  const size_t mirror_bytes = m_mirrorOffsets.empty() ? 0u : m_mirrorOffsets.back();
  m_device->releaseBufferBacking(std::move(m_mirrorBuffer), m_mirrorBacking, /*gpu_address=*/0, mirror_bytes);
  m_mirrorBacking = nullptr;
  m_mirror_stale_mask = m_all_levels_mask;
  m_uploaded_mask = 0;
}

void
MTLD3D9Texture::noteLevelUploaded(uint32_t level) {
  // Only MANAGED reclaims: SYSTEMMEM/SCRATCH mirrors are the CPU master with no
  // GPU copy to restore from, and DEFAULT/DYNAMIC stream every frame.
  if (m_pool != D3DPOOL_MANAGED || m_userMemory || m_mirrorBacking == nullptr)
    return;
  if (level < 32) {
    m_uploaded_mask |= (1u << level);
    // The level just written matches the Metal texture again.
    m_mirror_stale_mask &= ~(1u << level);
    // This level's bytes are now on the GPU, so the deferred sweep need not
    // push it. A NO_DIRTY_UPDATE Unlock skips the eager upload, so it never
    // reaches here and the bit (if set) stays for the sweep.
    m_needs_upload_mask &= ~(1u << level);
  }
  if (m_uploaded_mask == m_all_levels_mask && m_mirror_download_count <= kMirrorReadEvictThreshold)
    dropMirror();
}

void
MTLD3D9Texture::materializeLevelForLock(uint32_t level) {
  if (level >= 32 || !(m_mirror_stale_mask & (1u << level)))
    return;
  // ensureMirror (run by the surface before this) re-allocated a blank backing;
  // download the level's bytes back from the Metal texture so the Lock hands
  // out the real contents.
  if (m_mirrorBacking != nullptr && level < m_levels.size()) {
    m_device->readbackSurfaceMirror(m_levels[level].ptr());
    ++m_mirror_download_count;
  }
  m_mirror_stale_mask &= ~(1u << level);
}

void
MTLD3D9Texture::restoreMirrorForSource() {
  // UpdateTexture reads mirrorBase() directly; re-materialize every evicted
  // level before it does. ensureMirror re-allocates the backing if needed.
  if (m_mirror_stale_mask == 0)
    return;
  ensureMirror();
  for (uint32_t i = 0; i < m_levels.size() && i < 32; ++i) {
    if (m_mirror_stale_mask & (1u << i))
      m_device->readbackSurfaceMirror(m_levels[i].ptr());
  }
  m_mirror_stale_mask = 0;
  ++m_mirror_download_count;
}

void
MTLD3D9Texture::stageMirrorLevel(uint32_t level, LONG l, LONG t, LONG r, LONG b) {
  if (level >= m_levels.size() || m_mirrorBacking == nullptr)
    return;
  const LONG lw = static_cast<LONG>(std::max<UINT>(1u, m_width >> level));
  const LONG lh = static_cast<LONG>(std::max<UINT>(1u, m_height >> level));
  const bool compressed = IsCompressedFormat(m_format);
  // Block-align a compressed rect out to the 4x4 grid, then clamp into the level
  // so a scaled-up rect can never walk the mirror read off the end.
  if (compressed) {
    l &= ~3;
    t &= ~3;
    r = (r + 3) & ~3;
    b = (b + 3) & ~3;
  }
  l = std::max<LONG>(0, std::min<LONG>(l, lw));
  t = std::max<LONG>(0, std::min<LONG>(t, lh));
  r = std::max<LONG>(0, std::min<LONG>(r, lw));
  b = std::max<LONG>(0, std::min<LONG>(b, lh));
  // 3Dc keeps a linear one-byte fiction with no block mapping, so push the whole
  // level from the mirror head (stageTextureUpload converts the layout to BC5),
  // the same reset UpdateTexture applies.
  if (Is3DcFormat(m_format)) {
    l = 0;
    t = 0;
    r = lw;
    b = lh;
  }
  if (r <= l || b <= t)
    return;
  const uint32_t src_pitch = D3DFormatLockPitch(m_format, static_cast<UINT>(lw));
  if (src_pitch == 0)
    return;
  // Byte offset of the (block-floored) rect corner into the level: the
  // resource_offset_map_pointer math shared with LockRect / UnlockRect.
  uint64_t row_off, col_off;
  if (compressed) {
    const uint32_t bytes_per_block = (m_format == D3DFMT_DXT1) ? 8u : 16u;
    row_off = static_cast<uint64_t>(t >> 2) * src_pitch;
    col_off = static_cast<uint64_t>(l >> 2) * bytes_per_block;
  } else {
    row_off = static_cast<uint64_t>(t) * src_pitch;
    col_off = static_cast<uint64_t>(l) * D3DFormatBytesPerPixel(m_format);
  }
  WMTOrigin origin{};
  origin.x = static_cast<uint32_t>(l);
  origin.y = static_cast<uint32_t>(t);
  origin.z = 0;
  WMTSize size{};
  size.width = static_cast<uint32_t>(r - l);
  size.height = static_cast<uint32_t>(b - t);
  size.depth = 1;
  const uint8_t *src = static_cast<const uint8_t *>(m_mirrorBacking) + mirrorOffset(level) + row_off + col_off;
  m_device->stageTextureUpload(
      metalTexture(), m_dxmtTexture, level, /*slice=*/0, origin, size, src, src_pitch, compressed
  );
}

void
MTLD3D9Texture::stageDirtyRegionUpload(const RECT *pRect) {
  if (m_pool != D3DPOOL_MANAGED)
    return;
  // Re-materialise any evicted level from the GPU first so the read sees the
  // real bytes; a non-stale live mirror (the common case: the app just wrote it)
  // is left untouched, and a never-locked texture stays mirror-less and no-ops.
  restoreMirrorForSource();
  if (m_mirrorBacking == nullptr)
    return;
  for (uint32_t level = 0; level < m_levels.size(); ++level) {
    LONG l, t, r, b;
    if (pRect) {
      // Scale the level-0 rect down to this level (round the far edge out, the
      // same >> level shape UpdateTexture uses; the per-level clamp is in
      // stageMirrorLevel).
      l = pRect->left >> level;
      t = pRect->top >> level;
      r = (pRect->right + ((1 << level) - 1)) >> level;
      b = (pRect->bottom + ((1 << level) - 1)) >> level;
    } else {
      l = 0;
      t = 0;
      r = static_cast<LONG>(std::max<UINT>(1u, m_width >> level));
      b = static_cast<LONG>(std::max<UINT>(1u, m_height >> level));
    }
    stageMirrorLevel(level, l, t, r, b);
  }
}

void
MTLD3D9Texture::sweepManagedUpload() {
  const uint32_t pending = m_needs_upload_mask;
  if (pending == 0 || m_mirrorBacking == nullptr)
    return;
  // Push every pending level at full extent from the mirror. A stale (evicted)
  // level's bytes already live on the GPU, so skip the push; the mask clear
  // below still retires it. Stage all before retiring eviction state:
  // noteLevelUploaded may drop the mirror once the last level lands, and the
  // reads must be done by then.
  uint32_t staged = 0;
  for (uint32_t level = 0; level < m_levels.size() && level < 32; ++level) {
    if (!(pending & (1u << level)) || (m_mirror_stale_mask & (1u << level)))
      continue;
    const LONG lw = static_cast<LONG>(std::max<UINT>(1u, m_width >> level));
    const LONG lh = static_cast<LONG>(std::max<UINT>(1u, m_height >> level));
    stageMirrorLevel(level, 0, 0, lw, lh);
    staged |= (1u << level);
  }
  m_needs_upload_mask &= ~pending;
  for (uint32_t level = 0; level < 32; ++level)
    if (staged & (1u << level))
      noteLevelUploaded(level);
}

void
MTLD3D9Texture::evictManagedMirror() {
  // EvictManagedResources: native drops MANAGED from VRAM and fully reloads it
  // from the sysmem master on next use, which makes a prior NO_DIRTY_UPDATE write
  // visible. UMA has no VRAM to free, so reproduce only the observable reload by
  // re-arming every level for the sweep. Meaningful only while the mirror is live
  // and may hold bytes the GPU lacks; an already-evicted mirror means the GPU is
  // authoritative, so there is nothing to re-push.
  if (m_pool != D3DPOOL_MANAGED || m_mirrorBacking == nullptr)
    return;
  m_needs_upload_mask |= m_all_levels_mask;
}

MTLD3D9Texture::~MTLD3D9Texture() {
  // Tear down per-level surfaces first so the GPU stops sampling
  // before we drop the underlying allocations.
  m_levels.clear();
  // Buffer-backed: allocation owns wsi page + Metal buffer, destroyed
  // when last Rc<TextureAllocation> drops. Donating from dtor would race
  // in-flight chunks: UAF. Use post-completion hook instead. Mirror donation
  // safe: the mirror is never GPU-referenced (UnlockRect snapshots its bytes
  // into the seq-tagged staging ring and readback is synchronous), so no
  // in-flight chunk can be reading it when the dtor donates.
  // User-memory textures alias the app's pointer as the mirror base; the
  // app owns that storage, so the texture must not pool or free it.
  if (!m_userMemory && (m_mirrorBacking || m_mirrorBuffer != nullptr)) {
    const size_t mirror_bytes = m_mirrorOffsets.empty() ? 0u : m_mirrorOffsets.back();
    m_device->releaseBufferBacking(std::move(m_mirrorBuffer), m_mirrorBacking, /*gpu_address=*/0, mirror_bytes);
    m_mirrorBacking = nullptr;
  }
  if (m_isLosable)
    m_device->onLosableResourceDestroyed(m_losableBytes);
}

void
MTLD3D9Texture::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    // Level-0 bytes as the reported figure (mip tail is a minor term; the
    // value feeds GetAvailableTextureMem, which is documented inexact).
    m_losableBytes = static_cast<int64_t>(D3DFormatLockPitch(m_format, m_width)) *
                     static_cast<int64_t>(D3DFormatRowCount(m_format, m_height));
    m_device->onLosableResourceCreated(m_losableBytes);
  }
}

ULONG STDMETHODCALLTYPE
MTLD3D9Texture::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Texture::Release() {
  // D3D9 clamps Release-at-0 (a quirk apps rely on; com/com_object.hpp
  // ComObjectClamp). This class multiply-inherits (ComObject +
  // MTLD3D9CommonTexture) so ComObjectClamp cannot wrap it; fold the guard by
  // hand. It also keeps a level's delegated Release from underflowing the
  // shared counter.
  if (m_refCount.load() == 0)
    return 0;
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed(m_losableBytes);
    }
    // The destructor returns the mirror buffer backing to the device pool, so
    // the device has to outlive it. Drop the device pin LAST: capture it
    // (ReleasePrivate frees `this`), let the destructor run while the pin still
    // keeps the device alive, then release the pin, which may now free it.
    MTLD3D9Device *device = m_device;
    // Drop the ctor self-pin exactly once, matching MTLD3D9Surface.
    // Subsequent pub Get→Release cycles must NOT call ReleasePrivate
    // again; m_textures[N] / m_levels surface containers etc. may
    // still hold their own priv refs, and over-decrementing kills the
    // object out from under them.
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
    device->Release();
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9) ||
      riid == __uuidof(IDirect3DTexture9)) {
    *ppvObject = static_cast<IDirect3DTexture9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::FreePrivateData(REFGUID refguid) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9FreePrivateData(m_privateData, refguid);
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::SetPriority(DWORD PriorityNew) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetResourcePriority(m_pool, m_priority, PriorityNew);
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetPriority() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9Texture::PreLoad() {
  D9DeviceLock lock = m_device->LockDevice();
  // Apple Silicon's unified memory makes residency hints a no-op.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9Texture::GetType() {
  D9DeviceLock lock = m_device->LockDevice();
  return D3DRTYPE_TEXTURE;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::SetLOD(DWORD LODNew) {
  D9DeviceLock lock = m_device->LockDevice();
  // Per D3D9: SetLOD only meaningful for D3DPOOL_MANAGED. For other
  // pools the runtime returns 0 and ignores the new value. wined3d
  // texture.c d3d9_texture_2d_SetLOD asserts this.
  if (m_pool != D3DPOOL_MANAGED)
    return 0;
  // CreateTexture rules out an empty m_levels, but the unsigned
  // subtraction is a footgun in case validation drift ever lets a
  // zero-level texture through.
  DWORD max_lod = m_levels.empty() ? 0u : static_cast<DWORD>(m_levels.size() - 1);
  DWORD prev = m_lod.load(std::memory_order_relaxed);
  m_lod.store(std::min(LODNew, max_lod), std::memory_order_relaxed);
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetLOD() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_lod.load(std::memory_order_relaxed);
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetLevelCount() {
  D9DeviceLock lock = m_device->LockDevice();
  return static_cast<DWORD>(m_levels.size());
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
  D9DeviceLock lock = m_device->LockDevice();
  // wined3d texture.c d3d9_texture_2d_SetAutoGenFilterType: reject
  // D3DTEXF_NONE: the runtime requires a valid auto-gen filter, and
  // apps that test capability by trying NONE first expect
  // D3DERR_INVALIDCALL back.
  if (FilterType == D3DTEXF_NONE)
    return D3DERR_INVALIDCALL;
  m_autoGenFilter = FilterType;
  return D3D_OK;
}

D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9Texture::GetAutoGenFilterType() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_autoGenFilter;
}

void STDMETHODCALLTYPE
MTLD3D9Texture::GenerateMipSubLevels() {
  D9DeviceLock lock = m_device->LockDevice();
  // Only AUTOGENMIPMAP textures auto-regenerate: an explicit-mip texture fills
  // its levels by Lock/Unlock and must not have them overwritten by a downsample
  // of level 0 (D3D9 limits runtime auto-generation to AUTOGENMIPMAP). The app
  // sees a single level (app_levels == 1), so a guard on the app-visible level
  // count would never fire; generateMipmaps itself no-ops when the backing Metal
  // chain is single-level, which is the correct predicate, so gate on
  // AUTOGENMIPMAP alone and delegate the rest to it.
  if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP))
    return;
  // Skip when level 0 has not changed since the last regeneration, and clear the
  // dirty bit after: a defensive per-frame caller must not re-downsample every
  // frame, and an explicit call must not double up with the pre-draw sweep, which
  // consumes the same bit. Both refs gate identically (wined3d gen_auto_mipmap
  // early-returns then clears; DXVK NeedsMipGen then MarkTextureMipsUnDirty).
  if (!mipsDirty())
    return;
  m_device->generateMipmaps(m_textureRaw, m_dxmtTexture);
  clearMipsDirty();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppSurfaceLevel)
    return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = nullptr;
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  // wined3d texture.c: AUTOGENMIPMAP exposes only level 0.
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = ::dxmt::ref<IDirect3DSurface9>(m_levels[Level].ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::UnlockRect(UINT Level) {
  D9DeviceLock lock = m_device->LockDevice();
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  // Dirty-region + AUTOGENMIPMAP recording lives in MTLD3D9Surface::UnlockRect
  // so the GetSurfaceLevel / GetDC entry point (a direct surface unlock, not
  // routed through this wrapper) records them too. wined3d records both via the
  // shared texture regardless of entry point.
  return m_levels[Level]->UnlockRect();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::AddDirtyRect(const RECT *pDirtyRect) {
  D9DeviceLock lock = m_device->LockDevice();
  // wined3d wined3d_texture_add_dirty_region: rect==NULL marks the
  // whole sub-resource set dirty; otherwise it validates the region against the
  // level-0 extent (check_box_dimensions: OOB / inverted / block-misaligned ->
  // INVALIDCALL) then unions it. DXVK shares dxmt's old laxness here, and the
  // UpdateTexture consumer clamps the region (no memory hazard), so this is a
  // pure HR-correctness fix toward the primary reference. Storage is a single
  // union RECT at level-0 coordinates; UpdateTexture scales it down per-level.
  if (pDirtyRect &&
      !image_box_dimensions_valid(
          static_cast<uint32_t>(pDirtyRect->left), static_cast<uint32_t>(pDirtyRect->top),
          static_cast<uint32_t>(pDirtyRect->right), static_cast<uint32_t>(pDirtyRect->bottom), 0, 1, m_width, m_height,
          1, D3DFormatBlockWidth(m_format), D3DFormatBlockHeight(m_format)
      ))
    return D3DERR_INVALIDCALL;
  unionDirtyRect(pDirtyRect);
  // MANAGED sampled texture: AddDirtyRect re-uploads the marked region from the
  // sysmem master to the GPU (wined3d reloads the dirty region on next use; DXVK
  // AddDirtyBox + SetNeedsUpload consumed at PrepareDraw). dxmt pushes it here
  // rather than at the next draw, matching its eager upload-on-unlock model; the
  // op rides the arrival-order stream so a later draw samples it. A partial rect
  // stays partial (only the given region is refreshed, the rest keeps its prior
  // GPU contents), so two separate dirty rectangles are tracked independently.
  // No-op for SYSTEMMEM (which feeds UpdateTexture via the dirty rect above, not
  // a self-upload) and for a never-locked texture with no mirror.
  stageDirtyRegionUpload(pDirtyRect);
  return D3D_OK;
}

void
MTLD3D9Texture::unionDirtyRect(const RECT *pRect) {
  if (pRect == nullptr) {
    // NULL = mark fully dirty at level-0 extent.
    m_dirty_any = true;
    m_dirty_rect = RECT{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
    return;
  }
  if (!m_dirty_any) {
    m_dirty_any = true;
    m_dirty_rect = *pRect;
    return;
  }
  if (pRect->left < m_dirty_rect.left)
    m_dirty_rect.left = pRect->left;
  if (pRect->top < m_dirty_rect.top)
    m_dirty_rect.top = pRect->top;
  if (pRect->right > m_dirty_rect.right)
    m_dirty_rect.right = pRect->right;
  if (pRect->bottom > m_dirty_rect.bottom)
    m_dirty_rect.bottom = pRect->bottom;
}

} // namespace dxmt
