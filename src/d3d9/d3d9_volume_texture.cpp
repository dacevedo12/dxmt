#include "d3d9_volume_texture.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_image_lock.hpp"
#include "d3d9_private_data.hpp"
#include "d3d9_resource_priority.hpp"

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

  // Block-compressed (DXTn) volumes have no Metal 3D texture (Apple Silicon has
  // no 3D BC pixel format); they are mirror-only SCRATCH staging. m_texture is
  // null there and m_metalFormat stays Invalid, which is fine: the resource is
  // never sampled, and LockBox/UnlockBox work off the host mirror alone.
  if (m_texture != nullptr) {
    WMT::Texture parentTex = m_texture->current()->texture();
    m_metalFormat = parentTex.pixelFormat();
  }

  // Per-level mirror sizing in block rows: a slice is RowPitch x RowCount
  // bytes, where RowCount is ceil(height/4) for the block-compressed (DXTn)
  // volume formats D3D9 allows and plain height otherwise.
  // DEFAULT volumes are GPU-only and unlockable, except DYNAMIC ones: per MSDN
  // a DYNAMIC texture is lockable regardless of pool, so it gets the same
  // sysmem mirror + upload-on-unlock as the CPU pools (matches the 2D path in
  // buildLevelsAndMirror). Volumes are never RT/DS, so no usage guard is needed.
  const bool needs_mirror = (pool == D3DPOOL_MANAGED || pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH ||
                             (pool == D3DPOOL_DEFAULT && (usage & D3DUSAGE_DYNAMIC)));
  m_mirrorOffsets.resize(levels + 1u);
  size_t total_bytes = 0;
  for (UINT lvl = 0; lvl < levels; ++lvl) {
    UINT lw = std::max<UINT>(1u, width >> lvl);
    UINT lh = std::max<UINT>(1u, height >> lvl);
    UINT ld = std::max<UINT>(1u, depth >> lvl);
    m_mirrorOffsets[lvl] = total_bytes;
    // 4-byte-aligned row stride, the pitch LockBox reports; slice stride is
    // that times the block-row count. See d3d9_texture.cpp
    // buildLevelsAndMirror.
    total_bytes += static_cast<size_t>(D3DFormatLockPitch(format, lw)) *
                   static_cast<size_t>(D3DFormatRowCount(format, lh)) * static_cast<size_t>(ld);
  }
  m_mirrorOffsets[levels] = total_bytes;
  if (needs_mirror && total_bytes > 0)
    m_mirror = device->mirrorAllocator().Alloc(static_cast<uint32_t>(total_bytes));

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

    uint32_t row_pitch = 0;
    uint32_t slice_pitch = 0;
    if (m_mirror) {
      // The volume derives its lock pointer through the container's
      // map bracket per LockBox; only the strides are fixed here.
      row_pitch = D3DFormatLockPitch(format, lw);
      slice_pitch = row_pitch * D3DFormatRowCount(format, lh);
    }
    auto *vol = new MTLD3D9Volume(m_device, this, desc, lvl, row_pitch, slice_pitch);
    m_levels.emplace_back(vol);
  }
}

MTLD3D9VolumeTexture::~MTLD3D9VolumeTexture() {
  m_levels.clear();
  if (m_isLosable)
    m_device->onLosableResourceDestroyed(m_losableBytes);
}

void
MTLD3D9VolumeTexture::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    // Level-0 slice bytes times depth; see MTLD3D9Texture::markLosable.
    m_losableBytes = static_cast<int64_t>(D3DFormatLockPitch(m_format, m_width_l0)) *
                     static_cast<int64_t>(D3DFormatRowCount(m_format, m_height_l0)) *
                     static_cast<int64_t>(m_depth_l0);
    m_device->onLosableResourceCreated(m_losableBytes);
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
  // D3D9 Release-at-0 clamp (hand-folded: multiply-inherits, so
  // ComObjectClamp cannot wrap it). Also bounds a volume's delegated Release
  // against the shared counter.
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
      ComObject<IDirect3DVolumeTexture9>::ReleasePrivate();
    }
    device->Release();
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
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::FreePrivateData(REFGUID refguid) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9FreePrivateData(m_privateData, refguid);
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetPriority(DWORD PriorityNew) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetResourcePriority(m_pool, m_priority, PriorityNew);
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetPriority() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9VolumeTexture::PreLoad() {
  D9DeviceLock lock = m_device->LockDevice();
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetType() {
  D9DeviceLock lock = m_device->LockDevice();
  return D3DRTYPE_VOLUMETEXTURE;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetLOD(DWORD LODNew) {
  D9DeviceLock lock = m_device->LockDevice();
  // Volume textures: LOD applies just like 2D; clamped by spec to
  // [0, level_count-1]. The bind path reads m_lod; FX stage / sampler
  // bias picks up the value from there.
  if (m_pool != D3DPOOL_MANAGED)
    return 0;
  DWORD prev = m_lod.load(std::memory_order_relaxed);
  m_lod.store(std::min<DWORD>(LODNew, m_level_count > 0 ? m_level_count - 1 : 0), std::memory_order_relaxed);
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetLOD() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_lod.load(std::memory_order_relaxed);
}

DWORD STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetLevelCount() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_level_count;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType) {
  D9DeviceLock lock = m_device->LockDevice();
  // wined3d texture.c d3d9_texture_3d_SetAutoGenFilterType: reject
  // D3DTEXF_NONE.
  if (FilterType == D3DTEXF_NONE)
    return D3DERR_INVALIDCALL;
  m_autoGenFilter = FilterType;
  return D3D_OK;
}

D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetAutoGenFilterType() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_autoGenFilter;
}

void STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GenerateMipSubLevels() {
  D9DeviceLock lock = m_device->LockDevice();
  // A volume texture can never carry AUTOGENMIPMAP: CreateVolumeTexture rejects
  // the usage (validate_texture_create), matching both refs, so no auto-generated
  // 3D chain is ever constructed. This entry point stays a no-op that returns
  // cleanly for an app that calls it on an explicit-mip volume, rather than
  // failing; there is nothing to regenerate.
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) {
  D9DeviceLock lock = m_device->LockDevice();
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
  D9DeviceLock lock = m_device->LockDevice();
  if (Level >= m_levels.size())
    return D3DERR_INVALIDCALL;
  return m_levels[Level]->LockBox(pLockedVolume, pBox, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VolumeTexture::UnlockBox(UINT Level) {
  D9DeviceLock lock = m_device->LockDevice();
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
  // Pools with both a CPU mirror and a GPU texture push the just-written
  // box so subsequent samples see the new content: MANAGED, and the
  // DYNAMIC DEFAULT volumes that get a lockable mirror in the ctor.
  // SYSTEMMEM/SCRATCH have no GPU side; non-DYNAMIC DEFAULT has no mirror
  // (LockBox already failed on the null m_cpu_ptr). Mirrors MTLD3D9Surface's
  // per-surface upload for 2D textures; wined3d d3d9_volume_unmap pushes on
  // the same edge.
  if ((m_pool == D3DPOOL_MANAGED || (m_pool == D3DPOOL_DEFAULT && (m_usage & D3DUSAGE_DYNAMIC))) &&
      !vol->lockedReadOnly())
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
  D9DeviceLock lock = m_device->LockDevice();
  // wined3d validates the dirty box against the level-0 extent
  // (check_box_dimensions: OOB / inverted / block-misaligned -> INVALIDCALL); a
  // NULL box is "whole volume" and always valid. A volume (TEXTURE_3D) is never
  // forgiven a misaligned box, matching the LockBox gate. DXVK shares the old
  // laxness and the consumer clamps, so this is HR-correctness only.
  if (pDirtyBox &&
      !image_box_dimensions_valid(
          pDirtyBox->Left, pDirtyBox->Top, pDirtyBox->Right, pDirtyBox->Bottom, pDirtyBox->Front, pDirtyBox->Back,
          m_width_l0, m_height_l0, m_depth_l0, D3DFormatBlockWidth(m_format), D3DFormatBlockHeight(m_format)
      ))
    return D3DERR_INVALIDCALL;
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
  if (!m_mirror)
    return;
  // Mirror-only block-compressed SCRATCH volumes have no GPU texture; there is
  // nothing to push. The caller's pool gate never reaches them, but guard
  // locally so the safety is not caller-dependent.
  if (m_texture == nullptr)
    return;
  WMT::Texture tex = m_texture->current()->texture();
  if (tex == nullptr)
    return;

  D3DVOLUME_DESC d{};
  m_levels[level]->GetDesc(&d);
  // Aligned row stride to match the mirror layout; the row / slice offsets and
  // the staging src pitch below all derive from it. The per-column step stays
  // tight (bpp) at col_off. See buildLevelsAndMirror.
  uint32_t row_pitch = D3DFormatLockPitch(m_format, d.Width);
  if (row_pitch == 0)
    return;

  // Push only the locked box, not the whole level. Source pointer walks
  // from the mirror base + level offset + per-row/slice offset (skip src_z
  // slices x slice_pitch, then skip src_y rows x row_pitch, then add the x
  // pixel offset). The per-slice stride is block rows to match the mirror
  // sizing; the row/column offsets stay pixel-based (block-aligned partial-box
  // uploads of DXTn volumes are a separate gap, not reached by full-box uploads).
  uint32_t bpp = D3DFormatBytesPerPixel(m_format);
  // The unlock bracket usually holds the mapping already; the local
  // bracket keeps this correct for any future caller outside a lock.
  const uint8_t *mirror_base = mapMirror();
  if (!mirror_base)
    return;
  const uint8_t *base = mirror_base + m_mirrorOffsets[level];
  size_t row_off = static_cast<size_t>(vol->lockedY()) * row_pitch;
  size_t slice_off = static_cast<size_t>(vol->lockedZ()) * row_pitch * D3DFormatRowCount(m_format, d.Height);
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
  // Source slices are spaced by the full mip's slice pitch (row_pitch x the
  // mip's block-row count), which exceeds the staged box height for a sub-box upload.
  uint32_t src_slice_pitch = row_pitch * D3DFormatRowCount(m_format, d.Height);
  m_device->stageTextureUpload(
      tex, m_texture, level, /*slice=*/0, origin, size, src, row_pitch, /*compressed=*/false, src_slice_pitch
  );
  unmapMirror();
}

} // namespace dxmt
