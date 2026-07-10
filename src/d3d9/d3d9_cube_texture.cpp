#include "d3d9_cube_texture.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_image_lock.hpp"
#include "d3d9_private_data.hpp"
#include "d3d9_resource_priority.hpp"
#include "wsi_platform.hpp"

#include <algorithm>
#include <cstring>

namespace dxmt {

MTLD3D9CubeTexture::MTLD3D9CubeTexture(
    MTLD3D9Device *device, UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
    Rc<dxmt::Texture> texture
) :
    m_device(device),
    m_texture(std::move(texture)),
    m_level_count(levels),
    m_usage(usage),
    m_pool(pool),
    m_format(format) {
  AddRefPrivate();
  m_edge_length = edgeLength;
  for (uint32_t f = 0; f < 6; ++f)
    m_dirty_rect[f] = RECT{0, 0, static_cast<LONG>(edgeLength), static_cast<LONG>(edgeLength)};

  // Per-bind sample views (sRGB / swizzle) are resolved off m_texture
  // via dxmt::Texture::createView, which carries the parent's TextureCube
  // type; the cube only caches its metal pixel format here. A texture-less
  // mirror (packed-YUV SCRATCH) has no parent and is CPU-only.
  WMT::Texture parentTex = m_texture ? m_texture->current()->texture() : WMT::Texture();
  m_metalFormat = parentTex ? parentTex.pixelFormat() : WMTPixelFormatInvalid;

  // Sysmem mirror for non-DEFAULT pools. Layout matches m_levels:
  // face-major, then level. m_mirrorOffsets[face*levels + lvl] is the
  // byte offset of that face/level's first row. A D3DUSAGE_DYNAMIC DEFAULT
  // cube is lockable every frame the same as a DYNAMIC 2D texture, so it gets
  // the same sysmem mirror; RT/DS DEFAULT cubes stay GPU-only.
  const bool needs_mirror =
      pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH ||
      (pool == D3DPOOL_DEFAULT && ((usage & D3DUSAGE_DYNAMIC) || Is3DcFormat(format)) &&
       !(usage & D3DUSAGE_RENDERTARGET) && !(usage & D3DUSAGE_DEPTHSTENCIL));
  const size_t total_subresources = static_cast<size_t>(6) * levels;
  m_mirrorOffsets.resize(total_subresources + 1u);
  size_t total_bytes = 0;
  size_t idx = 0;
  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t lvl = 0; lvl < levels; ++lvl) {
      UINT level_dim = std::max<UINT>(1u, edgeLength >> lvl);
      m_mirrorOffsets[idx++] = total_bytes;
      // 4-byte-aligned mirror stride, the pitch LockRect reports; see
      // d3d9_texture.cpp buildLevelsAndMirror.
      total_bytes += static_cast<size_t>(D3DFormatLockPitch(format, level_dim)) *
                     static_cast<size_t>(D3DFormatRowCount(format, level_dim));
    }
    // Each cube face's mip chain starts on a 16-byte boundary within the
    // subresource backing: the D3D9 runtime aligns the per-face offset so a
    // LockRect on face N level 0 lands at the aligned position (wined3d sizes
    // its sub-resources the same way). Without this the reported pBits drift
    // 8 to 40 bytes off the expected face base.
    total_bytes = (total_bytes + 15u) & ~static_cast<size_t>(15u);
  }
  // 3Dc mirror pad: the last subresource (face 5, smallest level) is sized by
  // the linear fiction but stageTextureUpload reads its BC-real footprint; pad
  // the total so that read stays in bounds. Same shape as d3d9_texture.cpp
  // buildLevelsAndMirror; the per-face 16-byte alignment already covers the
  // interior faces' tail overreads.
  if (Is3DcFormat(format) && total_subresources > 0) {
    UINT last_dim = std::max<UINT>(1u, edgeLength >> (levels - 1));
    size_t bc_footprint = static_cast<size_t>(D3DFormatMetalTransferPitch(format, last_dim)) *
                          static_cast<size_t>(D3DFormatMetalTransferRows(format, last_dim));
    size_t min_total = m_mirrorOffsets[total_subresources - 1] + bc_footprint;
    if (min_total > total_bytes)
      total_bytes = min_total;
  }
  m_mirrorOffsets[total_subresources] = total_bytes;
  m_needs_mirror = needs_mirror && total_bytes > 0;

  m_levels.reserve(total_subresources);
  idx = 0;
  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t lvl = 0; lvl < levels; ++lvl) {
      UINT level_dim = std::max<UINT>(1u, edgeLength >> lvl);

      D3DSURFACE_DESC desc{};
      desc.Format = format;
      desc.Type = D3DRTYPE_SURFACE;
      desc.Usage = usage;
      desc.Pool = pool;
      desc.MultiSampleType = D3DMULTISAMPLE_NONE;
      desc.MultiSampleQuality = 0;
      desc.Width = level_dim;
      desc.Height = level_dim;


      auto *level = new MTLD3D9Surface(
          m_device, desc,
          /*container=*/static_cast<IDirect3DBaseTexture9 *>(this),
          WMT::Reference<WMT::Texture>(parentTex), // independent retain on the cube NSObject
          /*mipLevel=*/lvl,
          /*selfPin=*/false,
          /*parentTextureType=*/WMTTextureTypeCube,
          /*buffer=*/{},
          /*cpuPtr=*/nullptr,
          /*pitch=*/0,
          /*arraySlice=*/face,
          /*ownedBacking=*/nullptr,
          /*dxmtTexture=*/m_texture,
          /*textureMipSurface=*/false,
          // Sub-resource: the cube face shares this cube texture's public refcount.
          /*baseTexture=*/static_cast<IDirect3DBaseTexture9 *>(this)
      );
      // Mirror pointers arrive lazily: ensureMirror patches cpu_ptr and
      // pitch on first Lock, the MTLD3D9Texture shape.
      if (m_needs_mirror)
        level->setLazyMirrorParent(this, static_cast<uint32_t>(idx));
      ++idx;
      m_levels.emplace_back(level);
    }
  }
  // Point every face/level surface at this cube texture's shared Lock/GetDC
  // state so a GetDC or LockRect coordinates texture-wide (wined3d
  // resource.map_count); two faces can be locked at once but a DC is exclusive.
  for (auto &level : m_levels)
    level->setSharedLockState(&m_shared_lock_state);
}

MTLD3D9CubeTexture::~MTLD3D9CubeTexture() {
  // Tear down face surfaces first so the GPU stops sampling before
  // the backing drops; then donate the mirror to the device pool
  // (safe against in-flight work, the GPU never reads the mirror).
  m_levels.clear();
  if (m_mirrorBacking || m_mirrorBuffer != nullptr) {
    const size_t mirror_bytes = m_mirrorOffsets.empty() ? 0u : m_mirrorOffsets.back();
    m_device->releaseBufferBacking(std::move(m_mirrorBuffer), m_mirrorBacking, /*gpu_address=*/0, mirror_bytes);
    m_mirrorBacking = nullptr;
  }
  if (m_isLosable)
    m_device->onLosableResourceDestroyed(m_losableBytes);
}

void
MTLD3D9CubeTexture::ensureMirror() {
  // Idempotent: first call allocates, subsequent calls early-out.
  if (m_mirrorBacking != nullptr || !m_needs_mirror)
    return;
  if (m_mirrorOffsets.empty())
    return;
  const size_t total_bytes = m_mirrorOffsets.back();
  if (total_bytes == 0)
    return;

  // Device backing pool first, then aligned_malloc + a Shared Metal
  // registration for the pool contract; MTLD3D9Texture::ensureMirror
  // documents the shape. The GPU never reads this buffer.
  uint64_t mirror_gpu_addr = 0;
  void *mirror_host = nullptr;
  if (!m_device->acquireBufferBacking(total_bytes, m_mirrorBuffer, mirror_gpu_addr, mirror_host, m_mirrorBacking)) {
    m_mirrorBacking = wsi::aligned_malloc(total_bytes, DXMT_PAGE_SIZE);
    if (!m_mirrorBacking)
      return;
    std::memset(m_mirrorBacking, 0, total_bytes);

    WMTBufferInfo binfo{};
    binfo.length = total_bytes;
    binfo.options = (WMTResourceOptions)(WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared);
    binfo.memory.set(m_mirrorBacking);
    m_mirrorBuffer = m_device->metalDevice().newBuffer(binfo);
    if (m_mirrorBuffer == nullptr) {
      wsi::aligned_free(m_mirrorBacking);
      m_mirrorBacking = nullptr;
      return;
    }
  }

  // Patch every face surface: cpu_ptr plus the pitch computed from the
  // subresource's mip dimension.
  for (size_t i = 0; i < m_levels.size(); ++i) {
    void *level_ptr = static_cast<uint8_t *>(m_mirrorBacking) + m_mirrorOffsets[i];
    UINT level_dim = std::max<UINT>(1u, m_edge_length >> (i % m_level_count));
    m_levels[i]->patchMirror(level_ptr, D3DFormatLockPitch(m_format, level_dim));
  }
}

// Same read-back threshold as MTLD3D9Texture: an app that reads a
// MANAGED cube back repeatedly keeps its mirror resident rather than
// re-downloading on every read (wined3d's download_count guard).
static constexpr uint32_t kMirrorReadEvictThreshold = 4;

void
MTLD3D9CubeTexture::dropMirror() {
  if (m_mirrorBacking == nullptr)
    return;
  // The face surfaces share one backing and may hold concurrent locks;
  // freeing under a live pBits would dangle it. Skip; the next
  // write-Unlock retries.
  for (auto &level : m_levels)
    if (level->locked())
      return;
  // The GPU never samples the mirror buffer, so releasing it cannot
  // race in-flight work. Null every face surface's cpu_ptr so the next
  // Lock re-arms ensureMirror, then hand the backing to the device
  // pool.
  for (auto &level : m_levels)
    level->clearMirrorPatch();
  const size_t mirror_bytes = m_mirrorOffsets.empty() ? 0u : m_mirrorOffsets.back();
  m_device->releaseBufferBacking(std::move(m_mirrorBuffer), m_mirrorBacking, /*gpu_address=*/0, mirror_bytes);
  m_mirrorBacking = nullptr;
  m_staleSubres.set();
  m_uploadedSubres.reset();
}

void
MTLD3D9CubeTexture::noteLevelUploaded(uint32_t subresource) {
  // Only MANAGED reclaims: SYSTEMMEM/SCRATCH mirrors are the CPU
  // master with no GPU copy to restore from, and DEFAULT/DYNAMIC
  // stream every frame. Same threshold discipline as MTLD3D9Texture:
  // a cube read back repeatedly keeps its mirror resident.
  if (m_pool != D3DPOOL_MANAGED || m_mirrorBacking == nullptr)
    return;
  if (subresource < m_uploadedSubres.size()) {
    m_uploadedSubres.set(subresource);
    m_staleSubres.reset(subresource);
  }
  if (m_uploadedSubres.count() == m_levels.size() && m_mirror_download_count <= kMirrorReadEvictThreshold)
    dropMirror();
}

void
MTLD3D9CubeTexture::materializeLevelForLock(uint32_t subresource) {
  if (subresource >= m_staleSubres.size() || !m_staleSubres.test(subresource))
    return;
  // ensureMirror (run by the surface before this) re-allocated a blank
  // backing; download the face's bytes back from the Metal texture so
  // the Lock hands out the real contents.
  if (m_mirrorBacking != nullptr && subresource < m_levels.size()) {
    m_device->readbackSurfaceMirror(m_levels[subresource].ptr());
    ++m_mirror_download_count;
  }
  m_staleSubres.reset(subresource);
}

void
MTLD3D9CubeTexture::restoreMirrorForSource() {
  // UpdateTexture reads mirrorBase() directly; re-materialize every
  // evicted face before it does.
  if (m_staleSubres.none())
    return;
  ensureMirror();
  for (size_t i = 0; i < m_levels.size() && i < m_staleSubres.size(); ++i) {
    if (m_staleSubres.test(i))
      m_device->readbackSurfaceMirror(m_levels[i].ptr());
  }
  m_staleSubres.reset();
  ++m_mirror_download_count;
}

void
MTLD3D9CubeTexture::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    // Six faces of level-0 bytes; see MTLD3D9Texture::markLosable.
    m_losableBytes = 6 * static_cast<int64_t>(D3DFormatLockPitch(m_format, m_edge_length)) *
                     static_cast<int64_t>(D3DFormatRowCount(m_format, m_edge_length));
    m_device->onLosableResourceCreated(m_losableBytes);
  }
}

ULONG STDMETHODCALLTYPE
MTLD3D9CubeTexture::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9CubeTexture::Release() {
  // D3D9 Release-at-0 clamp (hand-folded: multiply-inherits, so
  // ComObjectClamp cannot wrap it). Also bounds a cube face's delegated
  // Release against the shared counter.
  if (m_refCount.load() == 0)
    return 0;
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed(m_losableBytes);
    }
    // Drop the device pin LAST: the destructor returns the mirror backing to the
    // device pool, so the device must outlive it. Capture the pin, run the
    // destructor via ReleasePrivate while it still keeps the device alive, then
    // release the pin (matching MTLD3D9Texture::Release).
    MTLD3D9Device *device = m_device;
    if (m_self_pinned) {
      m_self_pinned = false;
      ComObject<IDirect3DCubeTexture9>::ReleasePrivate();
    }
    device->Release();
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9) ||
      riid == __uuidof(IDirect3DCubeTexture9)) {
    *ppvObject = static_cast<IDirect3DCubeTexture9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::FreePrivateData(REFGUID refguid) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9FreePrivateData(m_privateData, refguid);
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetPriority(DWORD PriorityNew) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetResourcePriority(m_pool, m_priority, PriorityNew);
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetPriority() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9CubeTexture::PreLoad() {
  D9DeviceLock lock = m_device->LockDevice();
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetType() {
  D9DeviceLock lock = m_device->LockDevice();
  return D3DRTYPE_CUBETEXTURE;
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetLOD(DWORD LODNew) {
  D9DeviceLock lock = m_device->LockDevice();
  if (m_pool != D3DPOOL_MANAGED)
    return 0;
  DWORD max_lod = m_level_count > 0 ? m_level_count - 1 : 0u;
  DWORD prev = m_lod.load(std::memory_order_relaxed);
  m_lod.store(std::min(LODNew, max_lod), std::memory_order_relaxed);
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetLOD() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_lod.load(std::memory_order_relaxed);
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetLevelCount() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_level_count;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
  D9DeviceLock lock = m_device->LockDevice();
  // wined3d texture.c d3d9_texture_cube_SetAutoGenFilterType: reject
  // D3DTEXF_NONE.
  if (FilterType == D3DTEXF_NONE)
    return D3DERR_INVALIDCALL;
  m_autoGenFilter = FilterType;
  return D3D_OK;
}

D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetAutoGenFilterType() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_autoGenFilter;
}

void STDMETHODCALLTYPE
MTLD3D9CubeTexture::GenerateMipSubLevels() {
  D9DeviceLock lock = m_device->LockDevice();
  // generateMipmapsForTexture on a cube texture handle covers all 6 faces in a
  // single call. Same gating rationale as MTLD3D9Texture: only AUTOGENMIPMAP
  // cubes auto-regenerate (an explicit-mip cube fills faces by hand and must not
  // be overwritten by a downsample of face-level-0), the app-visible level count
  // is 1 so a guard on it would never fire, and generateMipmaps no-ops on a
  // single-level Metal chain, so gate on AUTOGENMIPMAP alone.
  if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP))
    return;
  // A texture-less cube (a packed-YUV SCRATCH placeholder carries no Metal
  // chain) has nothing to regenerate. The 2D path rides generateMipmaps'
  // null-handle guard through its cached reference; the cube dereferences
  // m_texture directly, so guard it here to keep that invariant.
  if (!m_texture)
    return;
  // Same dirty coordination as the 2D path: skip when face level 0 is unchanged
  // and clear the bit after, so the explicit call and the pre-draw sweep do not
  // both regenerate. Refs: wined3d gen_auto_mipmap, DXVK NeedsMipGen.
  if (!mipsDirty())
    return;
  m_device->generateMipmaps(m_texture->current()->texture(), m_texture);
  clearMipsDirty();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  if (Level >= m_level_count)
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppCubeMapSurface)
    return D3DERR_INVALIDCALL;
  *ppCubeMapSurface = nullptr;
  // D3DCUBEMAP_FACES is a signed enum; a negative value would pass
  // the signed `>= 6` check and then overflow on the cast-to-uint
  // index math below. Compare as unsigned to catch both bounds.
  if (static_cast<uint32_t>(FaceType) >= 6 || Level >= m_level_count)
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  uint32_t idx = static_cast<uint32_t>(FaceType) * m_level_count + Level;
  *ppCubeMapSurface = ::dxmt::ref<IDirect3DSurface9>(m_levels[idx].ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::LockRect(
    D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags
) {
  D9DeviceLock lock = m_device->LockDevice();
  if (static_cast<uint32_t>(FaceType) >= 6 || Level >= m_level_count)
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  uint32_t idx = static_cast<uint32_t>(FaceType) * m_level_count + Level;
  return m_levels[idx]->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
  D9DeviceLock lock = m_device->LockDevice();
  if (static_cast<uint32_t>(FaceType) >= 6 || Level >= m_level_count)
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  uint32_t idx = static_cast<uint32_t>(FaceType) * m_level_count + Level;
  // Windows softens a redundant Unlock made through the cube-texture entry
  // point (returns D3D_OK), unlike the surface-level Unlock reached via
  // GetCubeMapSurface, which stays strict. wined3d marks this leniency todo_wine.
  if (!m_levels[idx]->locked())
    return D3D_OK;
  // Dirty-region (per face) + AUTOGENMIPMAP recording lives in
  // MTLD3D9Surface::UnlockRect so the GetCubeMapSurface / GetDC entry point (a
  // direct surface unlock, not routed through this wrapper) records them too.
  // The device's draw-time flush still coalesces a six-face upload into a single
  // generateMipmaps on the cube handle. wined3d records both via the shared
  // texture regardless of entry point.
  return m_levels[idx]->UnlockRect();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect) {
  D9DeviceLock lock = m_device->LockDevice();
  // wined3d wined3d_texture_add_dirty_region rejects a layer past the face count
  // first, then validates the rect against the level-0 extent per face
  // (check_box_dimensions: OOB / inverted / block-misaligned -> INVALIDCALL); a
  // NULL rect is "whole face" and always valid. DXVK shares the old laxness and
  // the consumer clamps, so this is HR-correctness only.
  if (static_cast<uint32_t>(FaceType) >= 6)
    return D3DERR_INVALIDCALL;
  if (pDirtyRect &&
      !image_box_dimensions_valid(
          static_cast<uint32_t>(pDirtyRect->left), static_cast<uint32_t>(pDirtyRect->top),
          static_cast<uint32_t>(pDirtyRect->right), static_cast<uint32_t>(pDirtyRect->bottom), 0, 1, m_edge_length,
          m_edge_length, 1, D3DFormatBlockWidth(m_format), D3DFormatBlockHeight(m_format)
      ))
    return D3DERR_INVALIDCALL;
  unionDirtyRect(static_cast<uint32_t>(FaceType), pDirtyRect);
  return D3D_OK;
}

void
MTLD3D9CubeTexture::unionDirtyRect(uint32_t face, const RECT *pRect) {
  if (face >= 6)
    return;
  if (pRect == nullptr) {
    m_dirty_any[face] = true;
    m_dirty_rect[face] = RECT{0, 0, static_cast<LONG>(m_edge_length), static_cast<LONG>(m_edge_length)};
    return;
  }
  if (!m_dirty_any[face]) {
    m_dirty_any[face] = true;
    m_dirty_rect[face] = *pRect;
    return;
  }
  RECT &r = m_dirty_rect[face];
  if (pRect->left < r.left)
    r.left = pRect->left;
  if (pRect->top < r.top)
    r.top = pRect->top;
  if (pRect->right > r.right)
    r.right = pRect->right;
  if (pRect->bottom > r.bottom)
    r.bottom = pRect->bottom;
}

} // namespace dxmt
