#include "d3d9_volume.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_image_lock.hpp"
#include "d3d9_private_data.hpp"
#include "d3d9_stall.hpp"
#include "d3d9_volume_texture.hpp"

namespace dxmt {

MTLD3D9Volume::MTLD3D9Volume(
    MTLD3D9Device *device, MTLD3D9VolumeTexture *container, const D3DVOLUME_DESC &desc, uint32_t mipLevel,
    uint32_t rowPitch, uint32_t slicePitch
) :
    m_device(device),
    m_container(container),
    m_desc(desc),
    m_mipLevel(mipLevel),
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
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::FreePrivateData(REFGUID refguid) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9FreePrivateData(m_privateData, refguid);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetContainer(REFIID riid, void **ppContainer) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppContainer)
    return D3DERR_INVALIDCALL;
  *ppContainer = nullptr;
  return static_cast<IDirect3DVolumeTexture9 *>(m_container)->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::GetDesc(D3DVOLUME_DESC *pDesc) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  *pDesc = m_desc;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Volume::LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  D9StallLockTimer _lock_timer;
  if (!pLockedVolume)
    return D3DERR_INVALIDCALL;
  pLockedVolume->RowPitch = 0;
  pLockedVolume->SlicePitch = 0;
  pLockedVolume->pBits = nullptr;
  if (!m_container->hasMirror())
    return D3DERR_INVALIDCALL;
  if (m_locked)
    return D3DERR_INVALIDCALL;

  // DXVK-shaped lock flag normalisation: same as MTLD3D9Surface's
  // LockRect: the DISCARD+READONLY combo is invalid on DEFAULT pool only (the
  // CPU pools drop DISCARD anyway); DISCARD on non-DEFAULT pools is silently
  // dropped; DONOTWAIT is silently dropped.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY) &&
      m_desc.Pool == D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  Flags &= ~D3DLOCK_DONOTWAIT;
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  if (m_desc.Pool != D3DPOOL_DEFAULT)
    Flags &= ~D3DLOCK_DISCARD;
  // DOC-DIV (cosmetic): like MTLD3D9Surface::LockRect, image locks do not strip
  // DISCARD on a lost device the way DXVK LockImage and the buffer path do;
  // contents are undefined during device loss, so it is unobservable.

  // Block-addressed formats: DXTn are 4x4, packed-YUV (YUY2/UYVY) are 2x1.
  const uint32_t block_w = D3DFormatBlockWidth(m_desc.Format);
  const uint32_t block_h = D3DFormatBlockHeight(m_desc.Format);
  uint32_t x0 = 0, y0 = 0, z0 = 0;
  uint32_t w = m_desc.Width, h = m_desc.Height, d = m_desc.Depth;
  if (pBox) {
    // image_box_dimensions_valid ports wined3d's check_box_dimensions: OOB /
    // inverted / block-misaligned -> reject. Unlike 2D surfaces, a volume
    // (TEXTURE_3D) is never forgiven a misaligned box on any pool (wined3d
    // forgives only buffers and 2D textures), so the gate is unconditional.
    // Depth (Front/Back) carries no block compression; right/bottom may sit at
    // the volume extent.
    if (!image_box_dimensions_valid(
            pBox->Left, pBox->Top, pBox->Right, pBox->Bottom, pBox->Front, pBox->Back, m_desc.Width, m_desc.Height,
            m_desc.Depth, block_w, block_h
        ))
      return D3DERR_INVALIDCALL;
    x0 = pBox->Left;
    y0 = pBox->Top;
    z0 = pBox->Front;
    w = pBox->Right - pBox->Left;
    h = pBox->Bottom - pBox->Top;
    d = pBox->Back - pBox->Front;
  }
  // Byte offset of the locked box's first byte into the mirror. Block-
  // addressed volumes step the row offset by block rows and the column offset
  // by bytes-per-block-column. Depth carries no block compression (block depth
  // is 1), so z0 indexes whole slices. D3DFormatRowPitch(format, block_w)
  // yields the bytes per block column for DXTn / packed-YUV and the bytes-per-
  // pixel for plain formats, so all share one path. The block gate above keeps
  // x0/y0 block-aligned, so the divisions are exact.
  const uint32_t col_step = D3DFormatRowPitch(m_desc.Format, block_w);
  // Map the container mirror for the lock's lifetime; UnlockBox drops
  // the view after the GPU push has staged the bytes.
  m_mapped_base = m_container->mapMirrorLevel(m_mipLevel);
  if (!m_mapped_base)
    return D3DERR_INVALIDCALL;
  uint8_t *base = static_cast<uint8_t *>(m_mapped_base);
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
  D9DeviceLock lock = m_device->LockDevice();
  if (!m_locked)
    return D3DERR_INVALIDCALL;
  m_locked = false;
  // Push the just-written contents to the GPU texture. The container reads
  // our persisted m_locked_* bounds to scope the upload. Driving it here
  // (rather than only from the texture-level UnlockBox) means a MANAGED
  // volume filled through an IDirect3DVolume9 from GetVolumeLevel is
  // actually uploaded instead of left empty; mirrors MTLD3D9Surface for 2D.
  m_container->flushLevelOnUnlock(m_mipLevel, this);
  // The push staged its bytes synchronously; the lock's mirror view can go.
  m_container->unmapMirror();
  m_mapped_base = nullptr;
  return D3D_OK;
}

} // namespace dxmt
