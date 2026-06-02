#include "d3d9_cube_texture.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"

#include <algorithm>

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
  // type; the cube only caches its metal pixel format here.
  WMT::Texture parentTex = m_texture->current()->texture();
  m_metalFormat = parentTex.pixelFormat();

  // Sysmem mirror for non-DEFAULT pools. Layout matches m_levels:
  // face-major, then level. m_mirrorOffsets[face*levels + lvl] is the
  // byte offset of that face/level's first row.
  const bool needs_mirror = (pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH);
  const size_t total_subresources = static_cast<size_t>(6) * levels;
  m_mirrorOffsets.resize(total_subresources + 1u);
  size_t total_bytes = 0;
  size_t idx = 0;
  for (uint32_t face = 0; face < 6; ++face) {
    for (uint32_t lvl = 0; lvl < levels; ++lvl) {
      UINT level_dim = std::max<UINT>(1u, edgeLength >> lvl);
      m_mirrorOffsets[idx++] = total_bytes;
      total_bytes += static_cast<size_t>(D3DFormatRowPitch(format, level_dim)) *
                     static_cast<size_t>(D3DFormatRowCount(format, level_dim));
    }
  }
  m_mirrorOffsets[total_subresources] = total_bytes;
  if (needs_mirror && total_bytes > 0)
    m_mirror.assign(total_bytes, 0u);

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

      void *cpu_ptr = nullptr;
      uint32_t pitch = 0;
      if (!m_mirror.empty()) {
        cpu_ptr = m_mirror.data() + m_mirrorOffsets[idx];
        pitch = D3DFormatRowPitch(format, level_dim);
      }
      ++idx;

      auto *level = new MTLD3D9Surface(
          m_device, desc,
          /*container=*/static_cast<IDirect3DBaseTexture9 *>(this),
          WMT::Reference<WMT::Texture>(parentTex), // independent retain on the cube NSObject
          /*mipLevel=*/lvl,
          /*selfPin=*/false,
          /*parentTextureType=*/WMTTextureTypeCube,
          /*buffer=*/{},
          /*cpuPtr=*/cpu_ptr,
          /*pitch=*/pitch,
          /*arraySlice=*/face,
          /*ownedBacking=*/nullptr,
          /*dxmtTexture=*/m_texture
      );
      m_levels.emplace_back(level);
    }
  }
}

MTLD3D9CubeTexture::~MTLD3D9CubeTexture() {
  m_levels.clear();
  if (m_isLosable)
    m_device->onLosableResourceDestroyed();
}

void
MTLD3D9CubeTexture::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    m_device->onLosableResourceCreated();
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
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed();
    }
    m_device->Release();
    if (m_self_pinned) {
      m_self_pinned = false;
      ComObject<IDirect3DCubeTexture9>::ReleasePrivate();
    }
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
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::FreePrivateData(REFGUID refguid) {
  HRESULT hr = m_privateData.setData(refguid, 0, nullptr);
  if (hr == S_FALSE)
    return D3DERR_NOTFOUND;
  return hr;
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetPriority(DWORD PriorityNew) {
  DWORD prev = m_priority;
  m_priority = PriorityNew;
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetPriority() {
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9CubeTexture::PreLoad() {}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetType() {
  return D3DRTYPE_CUBETEXTURE;
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetLOD(DWORD LODNew) {
  if (m_pool != D3DPOOL_MANAGED)
    return 0;
  DWORD max_lod = m_level_count > 0 ? m_level_count - 1 : 0u;
  DWORD prev = m_lod;
  m_lod = std::min(LODNew, max_lod);
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetLOD() {
  return m_lod;
}

DWORD STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetLevelCount() {
  return m_level_count;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
  // wined3d texture.c d3d9_texture_cube_SetAutoGenFilterType: reject
  // D3DTEXF_NONE.
  if (FilterType == D3DTEXF_NONE)
    return D3DERR_INVALIDCALL;
  m_autoGenFilter = FilterType;
  return D3D_OK;
}

D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetAutoGenFilterType() {
  return m_autoGenFilter;
}

void STDMETHODCALLTYPE
MTLD3D9CubeTexture::GenerateMipSubLevels() {
  // generateMipmapsForTexture on a cube texture handle covers all 6
  // faces in a single call. Same gating rationale as MTLD3D9Texture.
  // Only AUTOGENMIPMAP textures opted into auto-regeneration; an
  // explicit-mip cube would have its hand-uploaded levels overwritten
  // by a downsample of face-level-0.
  if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP))
    return;
  if (m_level_count <= 1)
    return;
  m_device->generateMipmaps(m_texture->current()->texture());
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) {
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  if (Level >= m_level_count)
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) {
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
  if (static_cast<uint32_t>(FaceType) >= 6 || Level >= m_level_count)
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  uint32_t idx = static_cast<uint32_t>(FaceType) * m_level_count + Level;
  return m_levels[idx]->LockRect(pLockedRect, pRect, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) {
  if (static_cast<uint32_t>(FaceType) >= 6 || Level >= m_level_count)
    return D3DERR_INVALIDCALL;
  if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && Level != 0)
    return D3DERR_INVALIDCALL;
  uint32_t idx = static_cast<uint32_t>(FaceType) * m_level_count + Level;
  // Snapshot lock state before Unlock clears it; auto-mark dirty per
  // wined3d texture.c (top-level non-READONLY only).
  bool record_dirty = (Level == 0) && !m_levels[idx]->lockedReadOnly() && !m_levels[idx]->lockedNoDirtyUpdate();
  RECT lock_rect = m_levels[idx]->lockedRect();
  HRESULT hr = m_levels[idx]->UnlockRect();
  if (SUCCEEDED(hr) && record_dirty)
    unionDirtyRect(static_cast<uint32_t>(FaceType), &lock_rect);
  // AUTOGENMIPMAP cube: lazy-flag, same rationale as the 2D path. The
  // device's draw-time flush coalesces across faces: a six-face
  // upload sequence triggers a single generateMipmaps on the cube
  // handle (one blit covers all faces), no longer one per face.
  if (SUCCEEDED(hr) && Level == 0 && (m_usage & D3DUSAGE_AUTOGENMIPMAP) && m_level_count > 1)
    m_mips_dirty.store(true, std::memory_order_release);
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9CubeTexture::AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect) {
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
