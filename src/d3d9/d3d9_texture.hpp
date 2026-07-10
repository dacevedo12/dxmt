#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"
#include "d3d9_common_texture.hpp"
#include "d3d9_surface.hpp"
#include "dxmt_texture.hpp"
#include "rc/util_rc_ptr.hpp"

#include <atomic>
#include <vector>

namespace dxmt {

class MTLD3D9Device;

// IDirect3DTexture9: one Metal texture with N mip levels, one MTLD3D9Surface
// per level sharing parent's Metal handle. Lifetime: Texture holds raw MTLD3D9Device*;
// first AddRef bumps device, last Release drops it. Each surface holds AddRefPrivate
// on texture. GetSurfaceLevel returns same object. References: wined3d d3d9_texture_2d_*.
class MTLD3D9Texture final : public ComObject<IDirect3DTexture9>, public MTLD3D9CommonTexture, public D9LazyMirrorHost {
public:
  // Unified ctor: rooted in Rc<dxmt::Texture> so chunk-emitcc lambdas keep
  // Metal handle alive across calling-thread → encode-thread → GPU-completion via
  // ref_tracker. Two flavours: regular (bufferPitch==0, Metal texture direct) and
  // buffer-backed (bufferPitch>0, wsi::aligned_malloc wrapped in Metal buffer).
  // Buffer path: ref_tracker keeps buffer alive to GPU completion, closing prior UAF.
  // userMemory: D3D9Ex SYSTEMMEM user-memory (single level). The app pointer is the
  // packed CPU master: level-0 LockRect aliases it and UpdateTexture stages from it,
  // matching wined3d (no copy). The texture must not free it.
  MTLD3D9Texture(
      MTLD3D9Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
      Rc<dxmt::Texture> texture, uint32_t bufferPitch = 0, void *userMemory = nullptr
  );
  ~MTLD3D9Texture();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;

  // IDirect3DResource9
  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override;
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override;
  DWORD STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override;
  DWORD STDMETHODCALLTYPE GetPriority() override;
  void STDMETHODCALLTYPE PreLoad() override;
  D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

  // IDirect3DBaseTexture9
  DWORD STDMETHODCALLTYPE SetLOD(DWORD LODNew) override;
  DWORD STDMETHODCALLTYPE GetLOD() override;
  DWORD STDMETHODCALLTYPE GetLevelCount() override;
  HRESULT STDMETHODCALLTYPE SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) override;
  D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE GetAutoGenFilterType() override;
  void STDMETHODCALLTYPE GenerateMipSubLevels() override;

  // IDirect3DTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
  HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) override;
  HRESULT STDMETHODCALLTYPE LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE UnlockRect(UINT Level) override;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT *pDirtyRect) override;

  // MTLD3D9CommonTexture overrides: see d3d9_common_texture.hpp.
  // Definitions are inline so the per-bind virtual dispatch lands on
  // a non-virtual ComObject tail call rather than a separate function
  // call.
  WMT::Texture
  metalTexture() const override {
    return m_textureRaw;
  }
  // Internal: Rc<> handle for chunk lambdas to capture and keep
  // texture alive across calling/encode/GPU-completion boundary.
  // Always non-null, const-reference return.
  const Rc<dxmt::Texture> &
  dxmtTexture() const override {
    return m_dxmtTexture;
  }
  // SetTexture's cross-device check uses this; identity-only, no ref
  // cycle. Hot path. Always non-null while alive.
  MTLD3D9Device *
  deviceRaw() const override {
    return m_device;
  }
  D3DRESOURCETYPE
  commonTextureType() const override {
    return D3DRTYPE_TEXTURE;
  }
  uint32_t
  commonTextureLod() const override {
    return static_cast<uint32_t>(m_lod.load(std::memory_order_relaxed));
  }
  WMTPixelFormat
  metalPixelFormat() const override {
    return m_metalFormat;
  }
  // Lazy mirror allocator: buildLevelsAndMirror computes the per-level
  // offsets eagerly but defers the wsi::aligned_malloc + Metal newBuffer
  // thunk until the first LockRect on any level surface. Called from
  // both MTLD3D9Texture::LockRect (the IDirect3DTexture9 path) and
  // MTLD3D9Surface::LockRect (the GetSurfaceLevel-direct path). Safe to
  // call repeatedly; the first call allocates and patches every level
  // surface's m_cpu_ptr + pitch, subsequent calls early-
  // out. Cuts boot-time VA pressure and wine_unix_call rate for apps
  // that batch-create textures up front (audit M-PERF #2).
  void ensureMirror() override;

  // MANAGED mirror eviction, the wined3d evict_sysmem shape. A
  // D3DPOOL_MANAGED texture's host mirror (the wsi::aligned_malloc backing in
  // the calling process's scarce <4 GB space) is redundant once every level
  // has been uploaded: the Metal texture is the authoritative copy and
  // survives Reset. noteLevelUploaded records a level as GPU-resident and,
  // when all levels are covered, frees the mirror back to the device pool so
  // a 32-bit title reclaims that address space. ensureMirror re-allocates it
  // lazily on a later Lock; materializeLevelForLock re-downloads a level's
  // bytes from the Metal texture before a read-Lock hands the pointer out;
  // restoreMirrorForSource does the same before UpdateTexture reads the
  // mirror. A texture read back more than a small threshold keeps its mirror
  // (wined3d's download_count heuristic) to avoid re-download thrash.
  void noteLevelUploaded(uint32_t level) override;
  void materializeLevelForLock(uint32_t level) override;
  void restoreMirrorForSource();

  // Deferred MANAGED upload (see MTLD3D9CommonTexture). A 2D MANAGED texture is
  // fully dirty at create (m_needs_upload_mask = every level); a plain Unlock
  // clears its level's bit as it uploads eagerly, so the sweep only fires for
  // levels written but never individually Unlocked. deferManagedNoDirtyUpload
  // routes a NO_DIRTY_UPDATE Unlock away from the eager path so those bytes wait
  // in the mirror for the sweep / AddDirtyRect instead. AUTOGENMIPMAP is excluded:
  // its pre-draw mip regeneration reads the uploaded level 0, so that level must
  // keep uploading eagerly (its NO_DIRTY handling is a separate, untested corner).
  bool
  deferManagedNoDirtyUpload() const override {
    return m_pool == D3DPOOL_MANAGED && !(m_usage & D3DUSAGE_AUTOGENMIPMAP);
  }
  bool
  hasPendingManagedUpload() const override {
    return m_needs_upload_mask != 0;
  }
  void sweepManagedUpload() override;
  void evictManagedMirror() override;

  // CPU-side mirror of a SYSTEMMEM or MANAGED master texture, sourced by
  // UpdateTexture via mirrorBase()/mirrorOffset() (allocated in
  // buildLevelsAndMirror). Empty on DEFAULT-pool textures, which have no
  // mirror; UpdateTexture's caller must check pool() before consuming.
  void *
  mirrorBase() const {
    return m_mirrorBacking;
  }
  size_t
  mirrorOffset(UINT level) const {
    return level < m_mirrorOffsets.size() ? m_mirrorOffsets[level] : 0;
  }
  D3DPOOL
  pool() const {
    return m_pool;
  }
  DWORD
  usage() const {
    return m_usage;
  }
  UINT
  levelCount() const {
    return static_cast<UINT>(m_levels.size());
  }
  // Per-texture dirty-region tracking: wined3d
  // wined3d_texture_add_dirty_region records a single union region at
  // level-0 coordinates for the whole sub-resource set, then scales it
  // down per-level when uploads consume. dxmt mirrors that shape:
  // start fully dirty (a freshly-created texture has no GPU-side
  // content the consumer can trust), AddDirtyRect(NULL) re-marks full,
  // AddDirtyRect(rect) unions, and UpdateTexture / future consumers
  // read+clear via dirtyRectLevel0 / clearDirty.
  bool
  isDirty() const {
    return m_dirty_any;
  }
  RECT
  dirtyRectLevel0() const {
    return m_dirty_rect;
  }
  void
  clearDirty() {
    m_dirty_any = false;
  }
  // unionDirtyRect(NULL) marks the whole texture; otherwise unions the
  // supplied level-0 rect. Used by AddDirtyRect and by Lock/Unlock-side
  // bookkeeping in sub-E.
  void unionDirtyRect(const RECT *pRect);
  D3DFORMAT
  d3dFormat() const override {
    return m_format;
  }
  bool
  mipsDirty() const override {
    return m_mips_dirty.load(std::memory_order_acquire);
  }
  void
  clearMipsDirty() override {
    m_mips_dirty.store(false, std::memory_order_release);
  }
  // Flag the mip chain for regeneration. No-op if AUTOGENMIPMAP isn't
  // set: the device's pre-draw sweep only consumes the dirty bit for
  // textures that opted in. Same shape UnlockRect(0) uses at d3d9_texture
  // .cpp, factored out so StretchRect-as-dest can route through it
  // (wined3d device.c: `d3d9_texture_flag_auto_gen_mipmap`). Returns whether
  // it marked the chain so the device bumps its sweep epoch only when a real
  // AUTOGENMIPMAP texture went dirty.
  bool
  flagAutoGenDirty() {
    if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP))
      return false;
    m_mips_dirty.store(true, std::memory_order_release);
    return true;
  }
  // Forward to the ComObject<IDirect3DTexture9> base so Com<
  // MTLD3D9CommonTexture, false> pins the underlying refcount through
  // the same path the leaf already uses. Qualifying the ComObject
  // call avoids the recursive override.
  void
  AddRefPrivate() override {
    ComObject<IDirect3DTexture9>::AddRefPrivate();
  }
  void
  ReleasePrivate() override {
    ComObject<IDirect3DTexture9>::ReleasePrivate();
  }

private:
  MTLD3D9Device *m_device;
  // Always-populated retain on the live Metal texture handle:
  // a fresh Reference into m_dxmtTexture's current allocation.
  // Per-level surfaces hold their own independent retain on the same
  // NSObject so each surface keeps the allocation alive independently.
  WMT::Reference<WMT::Texture> m_textureRaw;
  // Always-populated wrapper for ctx.access lifetime tracking.
  // Chunk-emitcc lambdas capture this Rc<> so the underlying Metal
  // texture (and its backing buffer, for buffer-backed textures) is
  // retained through to GPU completion via the chunk ref_tracker.
  Rc<dxmt::Texture> m_dxmtTexture;
  // One Com<MTLD3D9Surface> per mip level. Eager creation rather than
  // lazy because GetSurfaceLevel must return the same surface object
  // across calls, and pre-creating sidesteps a lock on the lookup
  // path.
  std::vector<Com<MTLD3D9Surface, false>> m_levels;
  // Texture-wide Lock/GetDC coordination shared by every level surface (wined3d
  // resource.map_count). setSharedLockState points each level at this.
  D3D9SurfaceLockState m_shared_lock_state;
  // Per-level sysmem mirror for SYSTEMMEM/MANAGED/SCRATCH pools:
  // wsi::aligned_malloc wrapped in MTLBuffer. The mirror is CPU-only; UnlockRect
  // snapshots its bytes into the staging ring (a direct blit from the mirror
  // would race the encode thread). The MTLBuffer wrapper exists only for the
  // backing-pool registration contract, not as a GPU blit source.
  // m_mirrorOffsets[i] = byte offset of level i's first row.
  // Empty for D3DPOOL_DEFAULT or buffer-backed (texture-storage buffer IS lock target).
  WMT::Reference<WMT::Buffer> m_mirrorBuffer;
  void *m_mirrorBacking = nullptr;
  std::vector<size_t> m_mirrorOffsets;
  // D3D9Ex user-memory: m_mirrorBacking points at the app-owned packed
  // pixel storage rather than a dxmt allocation (no m_mirrorBuffer). The
  // dtor must not pool or free it; the app owns the lifetime.
  bool m_userMemory = false;

public:
  bool
  isUserMemory() const {
    return m_userMemory;
  }

private:
  // MANAGED mirror eviction state; see noteLevelUploaded. m_all_levels_mask
  // has one bit per mip level (set at ctor). m_uploaded_mask accumulates
  // uploaded levels and triggers dropMirror once it equals all_levels_mask.
  // m_mirror_stale_mask marks levels whose mirror bytes were freed and must
  // be re-downloaded from the Metal texture on the next read-Lock or
  // UpdateTexture. m_mirror_download_count pins the mirror once an app reads
  // it back repeatedly.
  uint32_t m_all_levels_mask = 0;
  uint32_t m_uploaded_mask = 0;
  uint32_t m_mirror_stale_mask = 0;
  uint32_t m_mirror_download_count = 0;
  // Deferred-upload bitset (see the sweepManagedUpload / hasPendingManagedUpload
  // block above): one bit per mip level that the app has written (or that create
  // / EvictManagedResources marked) but that has not been pushed to the Metal
  // texture. Seeded to every level at create for MANAGED, cleared per level as
  // it uploads. Independent of m_uploaded_mask, which drives mirror eviction.
  uint32_t m_needs_upload_mask = 0;
  void dropMirror();
  // Stage one mip level's [l, t, r, b) sub-rect (level-local coords) from the
  // sysmem mirror to the Metal texture through the device upload ring. Shared by
  // AddDirtyRect's region upload and the pre-draw sweep's full-level upload; the
  // same resource_offset_map_pointer / per-level scaling shape UpdateTexture
  // uses. No-op on a degenerate rect or an unallocated mirror.
  void stageMirrorLevel(uint32_t level, LONG l, LONG t, LONG r, LONG b);
  // AddDirtyRect consumer for a MANAGED sampled texture: push pRect (level-0
  // coords; NULL = whole surface) to every mip level, scaled down per level,
  // from the mirror. Restores an evicted mirror first so the read is valid.
  void stageDirtyRegionUpload(const RECT *pRect);
  DWORD m_usage;
  D3DPOOL m_pool;
  D3DFORMAT m_format;
  // Cached at ctor: per-level pitch needs m_width for ensureMirror().
  // m_levels[i] would be only other source (surfaces have no width accessor).
  UINT m_width = 0;
  UINT m_height = 0;
  DWORD m_priority = 0;
  // Atomic: the encode-thread draw walker reads the LOD clamp (commonTextureLod)
  // without the device lock while a calling-thread SetLOD may write it. Relaxed
  // suffices; a one-frame-stale clamp is harmless.
  std::atomic<DWORD> m_lod = {0u};
  D3DTEXTUREFILTERTYPE m_autoGenFilter = D3DTEXF_LINEAR;
  // Cached at ctor time so the per-bind sRGB-alias check doesn't fire a
  // wine_unix_call to query the parent Metal handle's format.
  WMTPixelFormat m_metalFormat = static_cast<WMTPixelFormat>(0);
  // Same exactly-once-drop pattern as MTLD3D9Surface::m_self_pinned;
  // see that header. Bound textures (m_textures[N] in the device,
  // m_levels surface m_container chain) hold private refs on top of
  // the ctor's self-pin; cycling pub through a Get/Release pair must
  // only drop the self-pin on the FIRST pub→0 transition, otherwise
  // each subsequent cycle over-decrements priv and tears down a
  // texture another holder still depends on.
  bool m_self_pinned = true;
  // AUTOGENMIPMAP lazy-flush bit. UnlockRect on level 0 sets this
  // true; the device's draw path scans bound textures and clears it
  // on flush (one blit encoder coalesced across all dirty bound
  // textures). Atomic so the compiler doesn't reorder w.r.t.
  // surrounding members; ordering is implicit since wine's main
  // thread runs both Lock/Unlock and draw.
  std::atomic<bool> m_mips_dirty{false};
  // Dirty-region tracking: see isDirty/dirtyRectLevel0/unionDirtyRect
  // in the public section. Defaults to (true, full level-0 extent) at
  // ctor: a freshly-created texture has no GPU-side content yet, so
  // the consumer's first upload should cover everything.
  bool m_dirty_any = true;
  RECT m_dirty_rect{};
  // Losable-resource accounting: see d3d9_surface.hpp's matching field.
  // CreateTexture's DEFAULT-pool path calls markLosable() before
  // AddRef; the dtor decrements. Other pools and per-level surfaces
  // never bump.
  bool m_isLosable = false;
  // Bytes reported to GetAvailableTextureMem while losable; see markLosable.
  int64_t m_losableBytes = 0;

public:
  void markLosable();

private:
  ComPrivateData m_privateData;
};

} // namespace dxmt
