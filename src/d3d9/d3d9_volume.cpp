#include "d3d9_volume.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_private_data.hpp"
#include "d3d9_volume_texture.hpp"

namespace dxmt {

MTLD3D9Volume::MTLD3D9Volume(
    MTLD3D9Device *device, MTLD3D9VolumeTexture *container, const D3DVOLUME_DESC &desc, uint32_t mipLevel, void *cpuPtr,
    uint32_t rowPitch, uint32_t slicePitch
) :
    m_device(device),
    m_container(container),
    m_desc(desc),
    m_mipLevel(mipLevel),
    m_cpu_ptr(cpuPtr),
    m_row_pitch(rowPitch),
    m_slice_pitch(slicePitch) {
  // Standalone Volume objects don't exist outside their container.
  // CreateOffscreenPlainSurface-style direct-volume creation is not
  // supported by D3D9. So we never self-pin; the parent's m_levels
  // Com<,false> is the only private ref.
}

MTLD3D9Volume::~MTLD3D9Volume() = default;

ULONG STDMETHODCALLTYPE
MTLD3D9Volume::AddRef() {
  // A volume is always a sub-resource of its volume texture (never
  // standalone), so it shares the parent's public counter:
  // get_refcount(volume) == get_refcount(volume texture), per the D3D9
  // sub-resource contract (DXVK D3D9Subresource). The parent owns this
  // volume, so the whole body delegates and never touches `this` afterwards.
  return static_cast<IDirect3DVolumeTexture9 *>(m_container)->AddRef();
}

ULONG STDMETHODCALLTYPE
MTLD3D9Volume::Release() {
  return static_cast<IDirect3DVolumeTexture9 *>(m_container)->Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVolume9)) {
    *ppvObject = static_cast<IDirect3DVolume9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::FreePrivateData(REFGUID refguid) {
  return D3D9FreePrivateData(m_privateData, refguid);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetContainer(REFIID riid, void **ppContainer) {
  if (!ppContainer)
    return D3DERR_INVALIDCALL;
  *ppContainer = nullptr;
  return static_cast<IDirect3DVolumeTexture9 *>(m_container)->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetDesc(D3DVOLUME_DESC *pDesc) {
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  *pDesc = m_desc;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) {
  if (!pLockedVolume)
    return D3DERR_INVALIDCALL;
  pLockedVolume->RowPitch = 0;
  pLockedVolume->SlicePitch = 0;
  pLockedVolume->pBits = nullptr;
  if (!m_cpu_ptr)
    return D3DERR_INVALIDCALL;
  if (m_locked)
    return D3DERR_INVALIDCALL;

  // DXVK-shaped lock flag normalisation: same as MTLD3D9Surface's
  // LockRect: DISCARD+READONLY combo is invalid; DISCARD on non-DEFAULT
  // pools is silently dropped; DONOTWAIT is silently dropped.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY))
    return D3DERR_INVALIDCALL;
  Flags &= ~D3DLOCK_DONOTWAIT;
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  if (m_desc.Pool != D3DPOOL_DEFAULT)
    Flags &= ~D3DLOCK_DISCARD;

  uint32_t x0 = 0, y0 = 0, z0 = 0;
  uint32_t w = m_desc.Width, h = m_desc.Height, d = m_desc.Depth;
  if (pBox) {
    if (pBox->Right <= pBox->Left || pBox->Bottom <= pBox->Top || pBox->Back <= pBox->Front ||
        pBox->Right > m_desc.Width || pBox->Bottom > m_desc.Height || pBox->Back > m_desc.Depth)
      return D3DERR_INVALIDCALL;
    // Block-compressed (DXTn) volumes require block-aligned box edges. Unlike
    // 2D surfaces, volumes enforce this on EVERY pool (wined3d forgives a
    // misaligned box only for buffers and 2D textures; a 3D texture never gets
    // that escape), so the gate is unconditional. Depth (Front/Back) is not
    // block compressed. right/bottom may sit at the volume extent.
    if (IsCompressedFormat(m_desc.Format) &&
        ((pBox->Left & 3) || (pBox->Top & 3) ||
         ((pBox->Right & 3) && pBox->Right != m_desc.Width) ||
         ((pBox->Bottom & 3) && pBox->Bottom != m_desc.Height)))
      return D3DERR_INVALIDCALL;
    x0 = pBox->Left;
    y0 = pBox->Top;
    z0 = pBox->Front;
    w = pBox->Right - pBox->Left;
    h = pBox->Bottom - pBox->Top;
    d = pBox->Back - pBox->Front;
  }
  // Byte offset of the locked box's first byte into the mirror. Block-
  // compressed volumes are addressed in 4x4 blocks: the row offset steps by
  // block rows and the column offset by bytes-per-block-column. Depth carries
  // no block compression (block depth is 1), so z0 indexes whole slices.
  // D3DFormatRowPitch(format, block_w) yields the bytes per block column for
  // DXTn and the bytes-per-pixel for plain formats, so both share one path.
  // The block gate above keeps x0/y0 block-aligned for DXTn, so the divisions
  // are exact.
  const uint32_t block_w = IsCompressedFormat(m_desc.Format) ? 4u : 1u;
  const uint32_t block_h = IsCompressedFormat(m_desc.Format) ? 4u : 1u;
  const uint32_t col_step = D3DFormatRowPitch(m_desc.Format, block_w);
  uint8_t *base = static_cast<uint8_t *>(m_cpu_ptr);
  size_t offset = static_cast<size_t>(z0) * m_slice_pitch +
                  (static_cast<size_t>(y0) / block_h) * m_row_pitch +
                  (static_cast<size_t>(x0) / block_w) * col_step;
  pLockedVolume->RowPitch = static_cast<INT>(m_row_pitch);
  pLockedVolume->SlicePitch = static_cast<INT>(m_slice_pitch);
  pLockedVolume->pBits = base + offset;

  m_locked = true;
  m_locked_readonly = (Flags & D3DLOCK_READONLY) != 0;
  m_locked_no_dirty_update = (Flags & D3DLOCK_NO_DIRTY_UPDATE) != 0;
  m_locked_x = x0;
  m_locked_y = y0;
  m_locked_z = z0;
  m_locked_w = w;
  m_locked_h = h;
  m_locked_d = d;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::UnlockBox() {
  if (!m_locked)
    return D3DERR_INVALIDCALL;
  m_locked = false;
  // Push the just-written contents to the GPU texture. The container reads
  // our persisted m_locked_* bounds to scope the upload. Driving it here
  // (rather than only from the texture-level UnlockBox) means a MANAGED
  // volume filled through an IDirect3DVolume9 from GetVolumeLevel is
  // actually uploaded instead of left empty; mirrors MTLD3D9Surface for 2D.
  m_container->flushLevelOnUnlock(m_mipLevel, this);
  return D3D_OK;
}

} // namespace dxmt
