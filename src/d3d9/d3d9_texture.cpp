#include "d3d9_texture.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_trace.hpp"
#include "wsi_platform.hpp"

#include <algorithm>

namespace dxmt {

// Shared per-level setup. Both ctors call this after stashing
// m_textureRaw + m_dxmtTexture (or just m_textureRaw + m_backingBuffer
// + m_backingPtr for the legacy path) so the surface array eager-
// creation and sysmem mirror logic only lives in one place.
static void
buildLevelsAndMirror(
    MTLD3D9Texture *self, MTLD3D9Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, WMT::Texture parentTex, WMT::Buffer backingBuffer, void *backingPtr, uint32_t bufferPitch,
    const Rc<dxmt::Texture> &dxmtTexture, WMT::Reference<WMT::Buffer> &mirrorBufferOut, void *&mirrorBackingOut,
    std::vector<size_t> &mirrorOffsets, std::vector<Com<MTLD3D9Surface, false>> &levelsOut,
    WMTPixelFormat &metalFormatOut, D3D9ViewCache &viewCacheOut
) {
  metalFormatOut = parentTex.pixelFormat();
  viewCacheOut.reset(parentTex, WMTTextureType2D, metalFormatOut);

  const bool buffer_backed = (backingBuffer != nullptr);
  const bool needs_mirror =
      !buffer_backed && (pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH);
  mirrorOffsets.resize(levels + 1u);
  size_t total_bytes = 0;
  for (UINT i = 0; i < levels; ++i) {
    UINT level_w = std::max<UINT>(1u, width >> i);
    UINT level_h = std::max<UINT>(1u, height >> i);
    mirrorOffsets[i] = total_bytes;
    total_bytes += static_cast<size_t>(D3DFormatRowPitch(format, level_w)) *
                   static_cast<size_t>(D3DFormatRowCount(format, level_h));
  }
  mirrorOffsets[levels] = total_bytes;
  // Mirror alloc is deferred to the first LockRect on any level
  // surface — see MTLD3D9Texture::ensureMirror. Apps that batch-create
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
    if (buffer_backed && i == 0) {
      // Workstream B level-0: surface aliases the backing buffer's
      // bytes; Lock returns the wsi::aligned_malloc'd pointer +
      // aligned pitch; Unlock fires m_buffer != nullptr gate and
      // skips stageTextureUpload.
      cpu_ptr = backingPtr;
      pitch = bufferPitch;
      level_buffer = WMT::Reference<WMT::Buffer>(backingBuffer);
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
        /*dxmtTexture=*/dxmtTexture
    );
    if (needs_mirror)
      level->setLazyMirrorParent(self);
    levelsOut.emplace_back(level);
  }
}

MTLD3D9Texture::MTLD3D9Texture(
    MTLD3D9Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    Rc<dxmt::Texture> texture, uint32_t bufferPitch
) :
    m_device(device),
    m_textureRaw(WMT::Reference<WMT::Texture>(texture->current()->texture())),
    m_dxmtTexture(std::move(texture)),
    m_usage(usage),
    m_pool(pool),
    m_format(format),
    m_width(width),
    m_height(height) {
  AddRefPrivate();
  // dxmt::Texture path is non-buffer-backed only — see header comment
  // for why the buffer-backed shape goes through the legacy ctor.
  (void)bufferPitch;
  // m_dirty_any = true by default-init; set the union rect to the full
  // level-0 extent so first consumer pass sees the whole texture as the
  // dirty region.
  m_dirty_rect = RECT{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
  buildLevelsAndMirror(
      this, m_device, width, height, levels, usage, format, pool, m_textureRaw,
      /*backingBuffer=*/{},
      /*backingPtr=*/nullptr,
      /*bufferPitch=*/0, m_dxmtTexture, m_mirrorBuffer, m_mirrorBacking, m_mirrorOffsets, m_levels, m_metalFormat,
      m_viewCache
  );
}

MTLD3D9Texture::MTLD3D9Texture(
    MTLD3D9Device *device, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    WMT::Reference<WMT::Texture> texture, WMT::Reference<WMT::Buffer> backingBuffer, void *backingPtr,
    uint32_t bufferPitch
) :
    m_device(device),
    m_textureRaw(std::move(texture)),
    m_dxmtTexture(nullptr),
    m_backingBuffer(std::move(backingBuffer)),
    m_backingPtr(backingPtr),
    m_backingBytes(static_cast<size_t>(bufferPitch) * height),
    m_usage(usage),
    m_pool(pool),
    m_format(format),
    m_width(width),
    m_height(height) {
  AddRefPrivate();
  m_dirty_rect = RECT{0, 0, static_cast<LONG>(m_width), static_cast<LONG>(m_height)};
  buildLevelsAndMirror(
      this, m_device, width, height, levels, usage, format, pool, m_textureRaw, m_backingBuffer, m_backingPtr,
      bufferPitch, m_dxmtTexture, m_mirrorBuffer, m_mirrorBacking, m_mirrorOffsets, m_levels, m_metalFormat, m_viewCache
  );
}

void
MTLD3D9Texture::ensureMirror() {
  // Idempotent — first call allocates, subsequent calls early-out.
  if (m_mirrorBacking != nullptr)
    return;
  // Pools that never need a mirror. DEFAULT-pool textures live entirely
  // on the GPU; the legacy buffer-backed path (Workstream B) aliases
  // level 0 onto a wsi::aligned_malloc'd buffer that IS the mirror.
  if (m_pool == D3DPOOL_DEFAULT)
    return;
  if (m_backingBuffer != nullptr)
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
    // Hazard-tracking default (Tracked) — see the eager-alloc note in
    // buildLevelsAndMirror's prior shape: UnlockRect blits this buffer
    // into the GPU texture; without the barrier a follow-on sample in
    // the same cmdbuf reads stale texels.
    binfo.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
    binfo.memory.set(m_mirrorBacking);
    m_mirrorBuffer = m_device->metalDevice().newBuffer(binfo);
    if (m_mirrorBuffer == nullptr) {
      wsi::aligned_free(m_mirrorBacking);
      m_mirrorBacking = nullptr;
      return;
    }
  }

  // Patch every level surface — fills in cpu_ptr, mirror_src_buffer,
  // mirror_level_offset, and pitch (computed per-level from m_format
  // and the dimension cached at ctor time).
  for (UINT i = 0; i < m_levels.size(); ++i) {
    void *level_ptr = static_cast<uint8_t *>(m_mirrorBacking) + m_mirrorOffsets[i];
    UINT level_w = std::max<UINT>(1u, m_width >> i);
    uint32_t pitch = D3DFormatRowPitch(m_format, level_w);
    m_levels[i]->patchMirror(level_ptr, m_mirrorBuffer.handle, static_cast<uint32_t>(m_mirrorOffsets[i]), pitch);
  }
}

MTLD3D9Texture::~MTLD3D9Texture() {
  // Tear down per-level surfaces first so the GPU stops sampling
  // before we drop the underlying allocations.
  m_levels.clear();
  // Donate the buffer-backed allocation to the device's shared
  // backing pool — same shape as MTLD3D9{Vertex,Index}Buffer's dtor.
  // The wsi pages are already pre-faulted from the first-time
  // memset, so the next CreateTexture / CreateVertexBuffer of the
  // same size skips both the newBuffer XPC and the page-fault cliff
  // (project_d3d9_placed_buffer_page_faults — ~ms-class for
  // streaming-sized textures on Rosetta x86_32). NFS:MW's in-game
  // texture streaming bursts (HUD blit encoder count max=9) are the
  // primary target. If the pool is full the entry drops as before.
  if (m_backingPtr || m_backingBuffer != nullptr) {
    m_device->releaseBufferBacking(std::move(m_backingBuffer), m_backingPtr, /*gpu_address=*/0, m_backingBytes);
    m_backingPtr = nullptr;
  }
  // Same pool donation for the lazy-allocated LockRect mirror.
  if (m_mirrorBacking || m_mirrorBuffer != nullptr) {
    const size_t mirror_bytes = m_mirrorOffsets.empty() ? 0u : m_mirrorOffsets.back();
    m_device->releaseBufferBacking(std::move(m_mirrorBuffer), m_mirrorBacking, /*gpu_address=*/0, mirror_bytes);
    m_mirrorBacking = nullptr;
  }
  if (m_isLosable)
    m_device->onLosableResourceDestroyed();
}

void
MTLD3D9Texture::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    m_device->onLosableResourceCreated();
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
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed();
    }
    m_device->Release();
    // Drop the ctor self-pin exactly once, matching MTLD3D9Surface.
    // Subsequent pub Get→Release cycles must NOT call ReleasePrivate
    // again — m_textures[N] / m_levels surface containers etc. may
    // still hold their own priv refs, and over-decrementing kills the
    // object out from under them.
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::QueryInterface(REFIID riid, void **ppvObject) {
  D9_TRACE("IDirect3DTexture9::QueryInterface");
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
  D9_TRACE("IDirect3DTexture9::GetDevice");
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9_TRACE("IDirect3DTexture9::SetPrivateData");
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9_TRACE("IDirect3DTexture9::GetPrivateData");
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::FreePrivateData(REFGUID refguid) {
  D9_TRACE("IDirect3DTexture9::FreePrivateData");
  HRESULT hr = m_privateData.setData(refguid, 0, nullptr);
  if (hr == S_FALSE)
    return D3DERR_NOTFOUND;
  return hr;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::SetPriority(DWORD PriorityNew) {
  D9_TRACE("IDirect3DTexture9::SetPriority");
  DWORD prev = m_priority;
  m_priority = PriorityNew;
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetPriority() {
  D9_TRACE("IDirect3DTexture9::GetPriority");
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9Texture::PreLoad() {
  D9_TRACE("IDirect3DTexture9::PreLoad");
  // Apple Silicon's unified memory makes residency hints a no-op.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9Texture::GetType() {
  D9_TRACE("IDirect3DTexture9::GetType");
  return D3DRTYPE_TEXTURE;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::SetLOD(DWORD LODNew) {
  D9_TRACE("IDirect3DTexture9::SetLOD");
  // Per D3D9: SetLOD only meaningful for D3DPOOL_MANAGED. For other
  // pools the runtime returns 0 and ignores the new value. wined3d
  // texture.c d3d9_texture_2d_SetLOD asserts this.
  if (m_pool != D3DPOOL_MANAGED)
    return 0;
  // CreateTexture rules out an empty m_levels, but the unsigned
  // subtraction is a footgun in case validation drift ever lets a
  // zero-level texture through.
  DWORD max_lod = m_levels.empty() ? 0u : static_cast<DWORD>(m_levels.size() - 1);
  DWORD prev = m_lod;
  m_lod = std::min(LODNew, max_lod);
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetLOD() {
  D9_TRACE("IDirect3DTexture9::GetLOD");
  return m_lod;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Texture::GetLevelCount() {
  D9_TRACE("IDirect3DTexture9::GetLevelCount");
  return static_cast<DWORD>(m_levels.size());
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
  D9_TRACE("IDirect3DTexture9::SetAutoGenFilterType");
  // wined3d texture.c d3d9_texture_2d_SetAutoGenFilterType: reject
  // D3DTEXF_NONE — the runtime requires a valid auto-gen filter, and
  // apps that test capability by trying NONE first expect
  // D3DERR_INVALIDCALL back.
  if (FilterType == D3DTEXF_NONE)
    return D3DERR_INVALIDCALL;
  m_autoGenFilter = FilterType;
  return D3D_OK;
}

D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9Texture::GetAutoGenFilterType() {
  D9_TRACE("IDirect3DTexture9::GetAutoGenFilterType");
  return m_autoGenFilter;
}

void STDMETHODCALLTYPE
MTLD3D9Texture::GenerateMipSubLevels() {
  D9_TRACE("IDirect3DTexture9::GenerateMipSubLevels");
  // No-op for single-level textures — generateMipmapsForTexture
  // requires mipmap_level_count > 1, and CreateTexture only allocates
  // a multi-level chain when AUTOGENMIPMAP is set OR the app asked
  // for explicit mips. App-explicit mips are filled by Lock/Unlock
  // per level, so calling generateMipmaps would overwrite that with
  // a downsample of level 0 — D3D9 spec says the runtime only auto-
  // generates for AUTOGENMIPMAP textures.
  if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP))
    return;
  if (m_levels.size() <= 1)
    return; // single-level allocation, nothing to fill
  m_device->generateMipmaps(m_textureRaw);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
  D9_TRACE("IDirect3DTexture9::GetLevelDesc");
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface9 **ppSurfaceLevel) {
  D9_TRACE("IDirect3DTexture9::GetSurfaceLevel");
  if (!ppSurfaceLevel)
    return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = nullptr;
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  // wined3d texture.c:364 — AUTOGENMIPMAP exposes only level 0.
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  *ppSurfaceLevel = ::dxmt::ref<IDirect3DSurface9>(m_levels[Level].ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::LockRect(UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  D9_TRACE("IDirect3DTexture9::LockRect");
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::UnlockRect(UINT Level) {
  D9_TRACE("IDirect3DTexture9::UnlockRect");
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  // Snapshot lock state BEFORE Unlock — surface clears it before return.
  // Per wined3d texture.c:1221: only top-level (level 0) non-READONLY
  // writes record dirt. AddDirtyRect(NULL) at create still covered the
  // whole texture; this hook narrows on subsequent Lock/Unlock cycles.
  bool record_dirty = (Level == 0) && !m_levels[Level]->lockedReadOnly();
  RECT lock_rect = m_levels[Level]->lockedRect();
  HRESULT hr = m_levels[Level]->UnlockRect();
  if (SUCCEEDED(hr) && record_dirty)
    unionDirtyRect(&lock_rect);
  // D3D9 spec: AUTOGENMIPMAP textures auto-regenerate the chain on
  // every Unlock(0). wined3d texture.c d3d9_texture_unmap mirrors
  // this. Eager generate (one cmdbuf per Unlock) was a per-frame
  // submission in NFS:MW's loading screen — hundreds of textures
  // unlocking back-to-back stalled CPU↔GPU at 1 fps. Lazy-flag now;
  // the device's draw path coalesces every dirty bound texture into
  // one blit encoder chained onto the open cmdbuf (no extra commit).
  // No m_levels.size() check: AUTOGENMIPMAP textures show 1 level to
  // the app (CreateTexture pins app_levels=1) but the Metal
  // allocation carries the full chain (metal_levels). A guard on
  // m_levels.size() > 1 would suppress the flag on every 2D
  // AUTOGENMIPMAP texture — the common path. generateMipmaps on a
  // legitimately 1-level Metal texture is a Metal no-op; the flush
  // pays only one encoder boundary either way.
  if (SUCCEEDED(hr) && Level == 0 && (m_usage & D3DUSAGE_AUTOGENMIPMAP))
    m_mips_dirty.store(true, std::memory_order_release);
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Texture::AddDirtyRect(const RECT *pDirtyRect) {
  D9_TRACE("IDirect3DTexture9::AddDirtyRect");
  // wined3d wined3d_texture_add_dirty_region: rect==NULL marks the
  // whole sub-resource set dirty; otherwise unions with the existing
  // dirty region. Storage is a single union RECT at level-0
  // coordinates — UpdateTexture (sub-E) scales it down per-level.
  unionDirtyRect(pDirtyRect);
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
