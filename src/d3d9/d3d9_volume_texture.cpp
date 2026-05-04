#include "d3d9_volume_texture.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_trace.hpp"

#include <algorithm>

namespace dxmt {

MTLD3D9VolumeTexture::MTLD3D9VolumeTexture(
    MTLD3D9Device *device, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
    D3DPOOL pool, Rc<dxmt::Texture> texture
) :
    m_device(device),
    m_texture(std::move(texture)),
    m_level_count(levels),
    m_usage(usage),
    m_pool(pool),
    m_format(format) {
  AddRefPrivate();
  m_width_l0 = width;
  m_height_l0 = height;
  m_depth_l0 = depth;
  m_dirty_box = D3DBOX{0, 0, width, height, 0, depth};

  WMT::Texture parentTex = m_texture->current()->texture();
  m_metalFormat = parentTex.pixelFormat();
  m_viewCache.reset(parentTex, WMTTextureType3D, m_metalFormat);

  // Per-level mirror sizing. Volumes can't be block-compressed in
  // D3D9, so pitch math is plain (bpp × width × height × depth).
  const bool needs_mirror = (pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH);
  m_mirrorOffsets.resize(levels + 1u);
  size_t total_bytes = 0;
  for (UINT lvl = 0; lvl < levels; ++lvl) {
    UINT lw = std::max<UINT>(1u, width >> lvl);
    UINT lh = std::max<UINT>(1u, height >> lvl);
    UINT ld = std::max<UINT>(1u, depth >> lvl);
    m_mirrorOffsets[lvl] = total_bytes;
    total_bytes +=
        static_cast<size_t>(D3DFormatRowPitch(format, lw)) * static_cast<size_t>(lh) * static_cast<size_t>(ld);
  }
  m_mirrorOffsets[levels] = total_bytes;
  if (needs_mirror && total_bytes > 0)
    m_mirror.assign(total_bytes, 0u);

  m_levels.reserve(levels);
  for (UINT lvl = 0; lvl < levels; ++lvl) {
    UINT lw = std::max<UINT>(1u, width >> lvl);
    UINT lh = std::max<UINT>(1u, height >> lvl);
    UINT ld = std::max<UINT>(1u, depth >> lvl);

    D3DVOLUME_DESC desc{};
    desc.Format = format;
    desc.Type = D3DRTYPE_VOLUME;
    desc.Usage = usage;
    desc.Pool = pool;
    desc.Width = lw;
    desc.Height = lh;
    desc.Depth = ld;

    void *cpu_ptr = nullptr;
    uint32_t row_pitch = 0;
    uint32_t slice_pitch = 0;
    if (!m_mirror.empty()) {
      cpu_ptr = m_mirror.data() + m_mirrorOffsets[lvl];
      row_pitch = D3DFormatRowPitch(format, lw);
      slice_pitch = row_pitch * lh;
    }
    auto *vol = new MTLD3D9Volume(m_device, this, desc, lvl, cpu_ptr, row_pitch, slice_pitch);
    m_levels.emplace_back(vol);
  }
}

MTLD3D9VolumeTexture::~MTLD3D9VolumeTexture() {
  m_levels.clear();
  if (m_isLosable)
    m_device->onLosableResourceDestroyed();
}

void
MTLD3D9VolumeTexture::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    m_device->onLosableResourceCreated();
  }
}

ULONG STDMETHODCALLTYPE
MTLD3D9VolumeTexture::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9VolumeTexture::Release() {
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed();
    }
    m_device->Release();
    if (m_self_pinned) {
      m_self_pinned = false;
      ComObject<IDirect3DVolumeTexture9>::ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::QueryInterface(REFIID riid, void **ppvObject) {
  D9_TRACE("IDirect3DVolumeTexture9::QueryInterface");
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DBaseTexture9) ||
      riid == __uuidof(IDirect3DVolumeTexture9)) {
    *ppvObject = static_cast<IDirect3DVolumeTexture9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9_TRACE("IDirect3DVolumeTexture9::GetDevice");
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9_TRACE("IDirect3DVolumeTexture9::SetPrivateData");
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9_TRACE("IDirect3DVolumeTexture9::GetPrivateData");
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::FreePrivateData(REFGUID refguid) {
  D9_TRACE("IDirect3DVolumeTexture9::FreePrivateData");
  HRESULT hr = m_privateData.setData(refguid, 0, nullptr);
  return hr == S_FALSE ? D3DERR_NOTFOUND : hr;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetPriority(DWORD PriorityNew) {
  DWORD prev = m_priority;
  m_priority = PriorityNew;
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetPriority() {
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9VolumeTexture::PreLoad() {}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetType() {
  return D3DRTYPE_VOLUMETEXTURE;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetLOD(DWORD LODNew) {
  // Volume textures: LOD applies just like 2D — clamped by spec to
  // [0, level_count-1]. The bind path reads m_lod; FX stage / sampler
  // bias picks up the value from there.
  if (m_pool != D3DPOOL_MANAGED)
    return 0;
  DWORD prev = m_lod;
  m_lod = std::min<DWORD>(LODNew, m_level_count > 0 ? m_level_count - 1 : 0);
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetLOD() {
  return m_lod;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetLevelCount() {
  return m_level_count;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
  // wined3d texture.c d3d9_texture_3d_SetAutoGenFilterType: reject
  // D3DTEXF_NONE.
  if (FilterType == D3DTEXF_NONE)
    return D3DERR_INVALIDCALL;
  m_autoGenFilter = FilterType;
  return D3D_OK;
}

D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetAutoGenFilterType() {
  return m_autoGenFilter;
}

void STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GenerateMipSubLevels() {
  // Auto-mipgen on 3D textures: Metal's blit encoder supports
  // generateMipmapsForTexture on 3D textures. Defer the actual call
  // until UnlockBox(0) or an explicit trigger; for now this is a
  // bookkeeping no-op so apps that call it don't fail.
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) {
  D9_TRACE("IDirect3DVolumeTexture9::GetLevelDesc");
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) {
  D9_TRACE("IDirect3DVolumeTexture9::GetVolumeLevel");
  if (!ppVolumeLevel)
    return D3DERR_INVALIDCALL;
  *ppVolumeLevel = nullptr;
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  *ppVolumeLevel = ::dxmt::ref<IDirect3DVolume9>(m_levels[Level].ptr());
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) {
  D9_TRACE("IDirect3DVolumeTexture9::LockBox");
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->LockBox(pLockedVolume, pBox, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::UnlockBox(UINT Level) {
  D9_TRACE("IDirect3DVolumeTexture9::UnlockBox");
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  MTLD3D9Volume *vol = m_levels[Level].ptr();
  // Snapshot the locked-box bookkeeping BEFORE UnlockBox clears it,
  // so the GPU push can scope to the actual dirty region. wined3d
  // d3d9_volume_unmap walks the same path.
  HRESULT hr = vol->UnlockBox();
  if (FAILED(hr))
    return hr;
  // MANAGED pool: push the box to the GPU so subsequent samples see
  // the new content. Other pools either have no GPU side
  // (SYSTEMMEM/SCRATCH) or no CPU master (DEFAULT — LockBox would
  // have failed already since m_cpu_ptr is null).
  if (m_pool == D3DPOOL_MANAGED && !vol->lockedReadOnly())
    pushLevelToGpu(Level, vol);
  // Dirty-region auto-mark — wined3d texture.c:1221 top-level only.
  // vol's m_locked_* persist past UnlockBox (vol only clears m_locked
  // bool), so reading them here is safe until the next LockBox.
  if (Level == 0 && !vol->lockedReadOnly()) {
    D3DBOX box{
        vol->lockedX(),
        vol->lockedY(),
        vol->lockedX() + vol->lockedW(),
        vol->lockedY() + vol->lockedH(),
        vol->lockedZ(),
        vol->lockedZ() + vol->lockedD()
    };
    unionDirtyBox(&box);
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::AddDirtyBox(const D3DBOX *pDirtyBox) {
  D9_TRACE("IDirect3DVolumeTexture9::AddDirtyBox");
  unionDirtyBox(pDirtyBox);
  return D3D_OK;
}

void
MTLD3D9VolumeTexture::unionDirtyBox(const D3DBOX *pBox) {
  if (pBox == nullptr) {
    m_dirty_any = true;
    m_dirty_box = D3DBOX{0, 0, m_width_l0, m_height_l0, 0, m_depth_l0};
    return;
  }
  if (!m_dirty_any) {
    m_dirty_any = true;
    m_dirty_box = *pBox;
    return;
  }
  if (pBox->Left < m_dirty_box.Left)
    m_dirty_box.Left = pBox->Left;
  if (pBox->Top < m_dirty_box.Top)
    m_dirty_box.Top = pBox->Top;
  if (pBox->Front < m_dirty_box.Front)
    m_dirty_box.Front = pBox->Front;
  if (pBox->Right > m_dirty_box.Right)
    m_dirty_box.Right = pBox->Right;
  if (pBox->Bottom > m_dirty_box.Bottom)
    m_dirty_box.Bottom = pBox->Bottom;
  if (pBox->Back > m_dirty_box.Back)
    m_dirty_box.Back = pBox->Back;
}

void
MTLD3D9VolumeTexture::pushLevelToGpu(uint32_t level, const MTLD3D9Volume *vol) {
  if (m_mirror.empty())
    return;
  WMT::Texture tex = m_texture->current()->texture();
  if (tex == nullptr)
    return;

  D3DVOLUME_DESC d{};
  m_levels[level]->GetDesc(&d);
  uint32_t row_pitch = D3DFormatRowPitch(m_format, d.Width);
  if (row_pitch == 0)
    return;

  // Push only the locked box, not the whole level. Source pointer
  // walks from the mirror base + level offset + per-row/slice offset
  // (skip src_z slices × slice_pitch, then skip src_y rows × row_pitch,
  // then add the x pixel offset). Uncompressed only — volumes never
  // carry block-compressed pixels in D3D9.
  uint32_t bpp = D3DFormatBytesPerPixel(m_format);
  const uint8_t *base = m_mirror.data() + m_mirrorOffsets[level];
  size_t row_off = static_cast<size_t>(vol->lockedY()) * row_pitch;
  size_t slice_off = static_cast<size_t>(vol->lockedZ()) * row_pitch * d.Height;
  size_t col_off = static_cast<size_t>(vol->lockedX()) * bpp;
  const uint8_t *src = base + slice_off + row_off + col_off;

  WMTOrigin origin{};
  origin.x = vol->lockedX();
  origin.y = vol->lockedY();
  origin.z = vol->lockedZ();
  WMTSize size{};
  size.width = vol->lockedW();
  size.height = vol->lockedH();
  size.depth = vol->lockedD();
  m_device->stageTextureUpload(tex, level, /*slice=*/0, origin, size, src, row_pitch, /*compressed=*/false);
}

} // namespace dxmt
