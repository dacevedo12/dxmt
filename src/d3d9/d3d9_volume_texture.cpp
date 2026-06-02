#include "d3d9_volume_texture.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"

#include <algorithm>
#include <chrono>

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
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::FreePrivateData(REFGUID refguid) {
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
  // Volume textures: LOD applies just like 2D; clamped by spec to
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
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) {
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
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->LockBox(pLockedVolume, pBox, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::UnlockBox(UINT Level) {
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  // Delegate to the per-level Volume, which drives the GPU push in its
  // own UnlockBox (flushLevelOnUnlock). That way an app filling the
  // texture through an IDirect3DVolume9 it got from GetVolumeLevel hits
  // the same upload path as one unlocking through the texture here;
  // previously the push lived only on this method, so a volume-level
  // unlock left the Metal texture empty.
  return m_levels[Level]->UnlockBox();
}

void
MTLD3D9VolumeTexture::flushLevelOnUnlock(uint32_t level, const MTLD3D9Volume *vol) {
  // MANAGED pool: push the just-written box to the GPU so subsequent
  // samples see the new content. Other pools have no GPU side
  // (SYSTEMMEM/SCRATCH) or no CPU master (DEFAULT; LockBox failed already
  // with a null m_cpu_ptr). Mirrors MTLD3D9Surface's per-surface upload
  // for 2D textures; wined3d d3d9_volume_unmap pushes on the same edge.
  if (m_pool == D3DPOOL_MANAGED && !vol->lockedReadOnly())
    pushLevelToGpu(level, vol);
  // Dirty-region auto-mark; wined3d texture.c top-level only.
  // D3DLOCK_NO_DIRTY_UPDATE suppresses the implicit auto-record so apps
  // that AddDirtyBox manually after the lock get exactly their explicit
  // region. DXVK honours it the same way.
  if (level == 0 && !vol->lockedReadOnly() && !vol->lockedNoDirtyUpdate()) {
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
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::AddDirtyBox(const D3DBOX *pDirtyBox) {
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
  // then add the x pixel offset). Uncompressed only; volumes never
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
  // Source slices are spaced by the full mip's slice pitch (row_pitch x
  // mip height), which exceeds the staged box height for a sub-box upload.
  uint32_t src_slice_pitch = row_pitch * d.Height;
  m_device->stageTextureUpload(
      tex, level, /*slice=*/0, origin, size, src, row_pitch, /*compressed=*/false, src_slice_pitch
  );
}

} // namespace dxmt
