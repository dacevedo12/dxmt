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

// IDirect3DTexture9 owns one Metal texture allocation with N mip
// levels and pre-creates one MTLD3D9Surface per level. Each level
// surface shares the parent's WMT::Texture handle (Metal NSObject
// retain semantics) and stores its level index — render-pass
// attachments and sampler bindings select the level from the
// surface's mipLevel(). GetSurfaceLevel hands out a public ref on
// the cached level surface (D3D9 contract: same Level returns the
// same object across calls).
//
// Lifetime mirrors the wined3d-shaped pattern already used by
// MTLD3D9Surface and MTLD3D9SwapChain:
//   - Texture holds raw MTLD3D9Device*; first public AddRef bumps
//     device, last public Release drops it.
//   - Constructor self-pins via AddRefPrivate so the texture
//     survives its own last public Release long enough to drop the
//     device pin.
//   - Each mip-level surface holds an AddRefPrivate on the texture
//     (the surface's m_container points back here). Surfaces in the
//     m_levels vector are stored as Com<MTLD3D9Surface>; that's the
//     private ref. App-visible refs come from GetSurfaceLevel which
//     calls AddRef on the level surface.
//
// References (vtable shape, validation order, autogenmip semantics):
// wined3d dlls/d3d9/texture.c d3d9_texture_2d_*. MGL has nothing 2D-
// texture-specific to add — the format lowering already happens in
// d3d9_format and the per-level surface allocation is a Metal
// concept (mipmap_level_count on WMTTextureInfo).
class MTLD3D9Texture final : public ComObject<IDirect3DTexture9>, public MTLD3D9CommonTexture {
public:
  // Two ctor shapes, picked at the device-side creation site:
  //
  //   - Phase-3.0 wrapper: Rc<dxmt::Texture>. The dxmt::Texture has
  //     already been built via Texture(info, device) + allocate() and
  //     owns the underlying MTLTexture. Used for the regular (non-
  //     buffer-backed) creation path. Phase 3.5 chunk-emitcc lambdas
  //     can capture m_dxmtTexture for ctx.access lifetime tracking.
  //
  //   - Legacy buffer-backed (Workstream B): pre-existing WMT handles
  //     plus the i386 wsi::aligned_malloc'd backing pointer. The
  //     dxmt::Texture buffer-backed branch can't service this path
  //     yet — its wsi::aligned_malloc only fires under __i386__ which
  //     is the unix-side build (always x86_64 on macOS), not the
  //     calling Windows process. Without that allocation the placement
  //     buffer's mapped pointer is null and LockRect hands back NULL
  //     pBits, the i386 game memcpys to it, and ucrtbase faults
  //     (project_d3d9_buffer_backed_wsi_alloc — see commit message).
  //     The Windows-side d3d9 code does the wsi::aligned_malloc
  //     itself (32-bit-addressable) and passes the pointer through
  //     here. m_dxmtTexture stays null on this path; a Phase-3.5
  //     sample bind for a buffer-backed texture must use the raw
  //     WMT::Texture handle via ctx.encodeRenderCommand rather than
  //     ctx.access.
  //
  // bufferPitch is ignored on the dxmt::Texture path.
  MTLD3D9Texture(
      MTLD3D9Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
      Rc<dxmt::Texture> texture, uint32_t bufferPitch = 0
  );
  MTLD3D9Texture(
      MTLD3D9Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
      WMT::Reference<WMT::Texture> texture, WMT::Reference<WMT::Buffer> backingBuffer, void *backingPtr,
      uint32_t bufferPitch
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

  // MTLD3D9CommonTexture overrides — see d3d9_common_texture.hpp.
  // Definitions are inline so the per-bind virtual dispatch lands on
  // a non-virtual ComObject tail call rather than a separate function
  // call.
  WMT::Texture
  metalTexture() const override {
    return m_textureRaw;
  }
  // Internal: the Rc<> handle so Phase 3.5+ chunk lambdas (and ctx.access
  // call sites) can capture it and keep the texture alive across the
  // calling-thread → encode-thread boundary. Returns by const reference.
  // May be null on the legacy buffer-backed path — Phase 3.5 callers
  // must check before using ctx.access; raw WMT::Texture sampler binds
  // via ctx.encodeRenderCommand always work.
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
  D3DPOOL
  commonTexturePool() const override {
    return m_pool;
  }
  WMT::Texture
  viewFor(const D3D9ViewKey &key) override {
    return m_viewCache.viewFor(key);
  }
  WMTPixelFormat
  metalPixelFormat() const override {
    return m_metalFormat;
  }
  // Lazy mirror allocator — buildLevelsAndMirror computes the per-level
  // offsets eagerly but defers the wsi::aligned_malloc + Metal newBuffer
  // thunk until the first LockRect on any level surface. Called from
  // both MTLD3D9Texture::LockRect (the IDirect3DTexture9 path) and
  // MTLD3D9Surface::LockRect (the GetSurfaceLevel-direct path). Safe to
  // call repeatedly; the first call allocates and patches every level
  // surface's m_cpu_ptr + m_mirror_src_buffer, subsequent calls early-
  // out. Cuts boot-time VA pressure and wine_unix_call rate for apps
  // that batch-create textures up front (audit M-PERF #2).
  void ensureMirror();

  // Mirror accessors used by UpdateTexture to source from a SYSTEMMEM
  // or MANAGED master texture's CPU-side mirror buffer (allocated in
  // buildLevelsAndMirror). Empty WMT::Buffer / 0 offset on DEFAULT-pool
  // textures which have no mirror. UpdateTexture's caller must check
  // pool() before consuming.
  WMT::Buffer
  mirrorBuffer() const {
    return m_mirrorBuffer;
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
  // Per-texture dirty-region tracking — wined3d
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
  // Always-populated retain on the live Metal texture handle. On the
  // dxmt::Texture path it's a fresh Reference into m_dxmtTexture's
  // current allocation; on the legacy buffer-backed path the device
  // hands it in directly. Per-level surfaces hold their own
  // independent retain on the same NSObject (see Phase 3.0 commit
  // for the lifetime rationale).
  WMT::Reference<WMT::Texture> m_textureRaw;
  // Phase 3.0+ wrapper for ctx.access lifetime tracking. Null on the
  // legacy buffer-backed path; populated for everything else.
  Rc<dxmt::Texture> m_dxmtTexture;
  // Per-d3d9-texture view cache for sRGB / swizzle / sub-range
  // aliasing. Initialised from m_textureRaw in the ctor; default-key
  // viewFor() returns that texture without creating a Metal view.
  D3D9ViewCache m_viewCache;
  // One Com<MTLD3D9Surface> per mip level. Eager creation rather than
  // lazy because GetSurfaceLevel must return the same surface object
  // across calls, and pre-creating sidesteps a lock on the lookup
  // path.
  std::vector<Com<MTLD3D9Surface, false>> m_levels;
  // Per-level sysmem mirror for SYSTEMMEM/MANAGED/SCRATCH pools. The
  // memory lives in a process-allocated wsi::aligned_malloc backing
  // wrapped by a Shared MTLBuffer — that lets UnlockRect record a
  // direct buffer→texture blit on the open cmdbuf instead of memcpying
  // the bytes into the staging ring first. NFS:MW's loading screen
  // hammered the mirror→ring memcpy at >1 second/sec total
  // (project_d3d9_loading_perf_rosetta); this is the fix.
  // m_mirrorOffsets[i] is the byte offset of level i's first row,
  // m_mirrorOffsets[m_levels.size()] is total bytes. Empty / null for
  // D3DPOOL_DEFAULT (no sysmem master) and for the Workstream B
  // buffer-backed path (the texture-storage buffer IS the lock target).
  WMT::Reference<WMT::Buffer> m_mirrorBuffer;
  void *m_mirrorBacking = nullptr;
  std::vector<size_t> m_mirrorOffsets;
  // Legacy buffer-backed path only — see ctor comment. The
  // wsi::aligned_malloc backing for newBufferWithBytesNoCopy must
  // outlive the level-0 surface that aliases it; the i386 process
  // address space delivers the 32-bit-addressable pBits the game
  // expects from LockRect. Both null on the dxmt::Texture path.
  WMT::Reference<WMT::Buffer> m_backingBuffer;
  void *m_backingPtr = nullptr;
  // Total bytes of m_backingPtr (backingPitch × Height for the
  // buffer-backed path). Cached at ctor so dtor can hand the backing
  // back to the device pool keyed by exact size — same shape as the
  // VB/IB pool keys MTLD3D9{Vertex,Index}Buffer use.
  size_t m_backingBytes = 0;
  DWORD m_usage;
  D3DPOOL m_pool;
  D3DFORMAT m_format;
  // Cached at ctor for ensureMirror() — needs the per-level pitch
  // D3DFormatRowPitch(m_format, max(1, m_width >> level)) and m_levels[i]
  // would otherwise be the only source (the texture has no width
  // accessor on m_levels' surfaces from the parent's side).
  UINT m_width = 0;
  UINT m_height = 0;
  DWORD m_priority = 0;
  DWORD m_lod = 0;
  D3DTEXTUREFILTERTYPE m_autoGenFilter = D3DTEXF_LINEAR;
  // Cached at ctor time so per-bind viewFor() calls don't fire a
  // wine_unix_call to query the parent Metal handle.
  WMTPixelFormat m_metalFormat = static_cast<WMTPixelFormat>(0);
  // Same exactly-once-drop pattern as MTLD3D9Surface::m_self_pinned —
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
  // Dirty-region tracking — see isDirty/dirtyRectLevel0/unionDirtyRect
  // in the public section. Defaults to (true, full level-0 extent) at
  // ctor — a freshly-created texture has no GPU-side content yet, so
  // the consumer's first upload should cover everything.
  bool m_dirty_any = true;
  RECT m_dirty_rect{};
  // Losable-resource accounting — see d3d9_surface.hpp's matching field.
  // CreateTexture's DEFAULT-pool path calls markLosable() before
  // AddRef; the dtor decrements. Other pools and per-level surfaces
  // never bump.
  bool m_isLosable = false;

public:
  void markLosable();

private:
  ComPrivateData m_privateData;
};

} // namespace dxmt
