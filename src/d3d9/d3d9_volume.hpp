#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"

namespace dxmt {

class MTLD3D9Device;
class MTLD3D9VolumeTexture;

// IDirect3DVolume9: single mip-level slice of IDirect3DVolumeTexture9.
// Mirrors MTLD3D9Surface for 2D textures: weak ref to parent, exposes
// LockBox/UnlockBox over CPU mirror. Parent creates per mip level (Com<,false>).
class MTLD3D9Volume final : public ComObject<IDirect3DVolume9> {
public:
  MTLD3D9Volume(
      MTLD3D9Device *device, MTLD3D9VolumeTexture *container, const D3DVOLUME_DESC &desc, uint32_t mipLevel,
      uint32_t rowPitch, uint32_t slicePitch
  );
  ~MTLD3D9Volume();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override;
  HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override;
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) override;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVOLUME_DESC *pDesc) override;
  HRESULT STDMETHODCALLTYPE LockBox(D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE UnlockBox() override;

  uint32_t
  mipLevel() const {
    return m_mipLevel;
  }
  // Locked-box bookkeeping read back by the parent volume texture
  // when UnlockBox needs to push the changed box to GPU. Whole-extent
  // if pBox==NULL at LockBox time; pixel coords stored in box units.
  // Cleared on UnlockBox so a follow-on read of m_locked_* doesn't
  // see stale bounds.
  uint32_t
  lockedX() const {
    return m_locked_x;
  }
  uint32_t
  lockedY() const {
    return m_locked_y;
  }
  uint32_t
  lockedZ() const {
    return m_locked_z;
  }
  uint32_t
  lockedW() const {
    return m_locked_w;
  }
  uint32_t
  lockedH() const {
    return m_locked_h;
  }
  uint32_t
  lockedD() const {
    return m_locked_d;
  }
  bool
  lockedReadOnly() const {
    return m_locked_readonly;
  }
  bool
  lockedNoDirtyUpdate() const {
    return m_locked_no_dirty_update;
  }

private:
  MTLD3D9Device *m_device;
  // Raw: parent container outlives the volume by construction. The
  // container holds a Com<MTLD3D9Volume, false> priv-pin via m_levels;
  // pub Get/Release routes through here to keep the container alive.
  MTLD3D9VolumeTexture *m_container;
  D3DVOLUME_DESC m_desc;
  uint32_t m_mipLevel;
  // CPU pointer into the parent's mirror: set at ctor time, stable for
  // the volume's lifetime. Null for D3DPOOL_DEFAULT volumes (no mirror).
  // Mirror view base for the open lock: LockBox maps the container's
  // mirror and holds the view until UnlockBox so pBits stays valid;
  // null while unlocked (the map bracket is a no-op on 64-bit).
  void *m_mapped_base = nullptr;
  uint32_t m_row_pitch;
  uint32_t m_slice_pitch;
  bool m_locked = false;
  // Locked-box bookkeeping for UnlockBox.
  bool m_locked_readonly = false;
  // D3DLOCK_NO_DIRTY_UPDATE: see MTLD3D9Surface::m_locked_no_dirty_update
  // for the rationale. Suppresses the parent texture's implicit
  // unionDirtyBox at UnlockBox time.
  bool m_locked_no_dirty_update = false;
  uint32_t m_locked_x = 0;
  uint32_t m_locked_y = 0;
  uint32_t m_locked_z = 0;
  uint32_t m_locked_w = 0;
  uint32_t m_locked_h = 0;
  uint32_t m_locked_d = 0;
  ComPrivateData m_privateData;
};

} // namespace dxmt
