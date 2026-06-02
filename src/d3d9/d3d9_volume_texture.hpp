#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"
#include "d3d9_common_texture.hpp"
#include "d3d9_volume.hpp"
#include "dxmt_texture.hpp"
#include "rc/util_rc_ptr.hpp"

#include <atomic>
#include <vector>

namespace dxmt {

class MTLD3D9Device;

// IDirect3DVolumeTexture9: 3D texture used for color-grading LUTs,
// volume rendering, and fluid-sim tables. WMTTextureType3D + per-level
// MTLD3D9Volume, CPU mirror as std::vector (no Metal-buffer wrapper).
// Compressed formats unsupported; mirror is pure linear (rowPitch×H×D).
class MTLD3D9VolumeTexture final : public ComObject<IDirect3DVolumeTexture9>, public MTLD3D9CommonTexture {
public:
  MTLD3D9VolumeTexture(
      MTLD3D9Device *device, UINT width, UINT height, UINT depth, UINT levels, DWORD usage, D3DFORMAT format,
      D3DPOOL pool, Rc<dxmt::Texture> texture
  );
  ~MTLD3D9VolumeTexture();

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

  // IDirect3DVolumeTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DVOLUME_DESC *pDesc) override;
  HRESULT STDMETHODCALLTYPE GetVolumeLevel(UINT Level, IDirect3DVolume9 **ppVolumeLevel) override;
  HRESULT STDMETHODCALLTYPE LockBox(UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE UnlockBox(UINT Level) override;
  HRESULT STDMETHODCALLTYPE AddDirtyBox(const D3DBOX *pDirtyBox) override;

  // MTLD3D9CommonTexture overrides: forwarded inline to the
  // ComObject<IDirect3DVolumeTexture9> base; same diamond pattern as
  // MTLD3D9Texture / MTLD3D9CubeTexture.
  WMT::Texture
  metalTexture() const override {
    return m_texture->current()->texture();
  }
  const Rc<dxmt::Texture> &
  dxmtTexture() const override {
    return m_texture;
  }
  MTLD3D9Device *
  deviceRaw() const override {
    return m_device;
  }
  D3DRESOURCETYPE
  commonTextureType() const override {
    return D3DRTYPE_VOLUMETEXTURE;
  }
  D3DPOOL
  commonTexturePool() const override {
    return m_pool;
  }
  uint32_t
  commonTextureLod() const override {
    return static_cast<uint32_t>(m_lod);
  }
  WMTPixelFormat
  metalPixelFormat() const override {
    return m_metalFormat;
  }
  D3DFORMAT
  d3dFormat() const override {
    return m_format;
  }
  bool
  mipsDirty() const override {
    return false;
  }
  void
  clearMipsDirty() override {}
  void
  AddRefPrivate() override {
    ComObject<IDirect3DVolumeTexture9>::AddRefPrivate();
  }
  void
  ReleasePrivate() override {
    ComObject<IDirect3DVolumeTexture9>::ReleasePrivate();
  }

  // Mirror accessors for UpdateTexture's volume branch.
  const uint8_t *
  mirrorBase() const {
    return m_mirror.empty() ? nullptr : m_mirror.data();
  }
  size_t
  mirrorOffset(uint32_t level) const {
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
  uint32_t
  levelCount() const {
    return m_level_count;
  }
  // Dirty-box tracking: see MTLD3D9Texture's header comment for the
  // shape. Volume uses D3DBOX (with Front/Back) instead of RECT.
  bool
  isDirty() const {
    return m_dirty_any;
  }
  D3DBOX
  dirtyBoxLevel0() const {
    return m_dirty_box;
  }
  void
  clearDirty() {
    m_dirty_any = false;
  }
  void unionDirtyBox(const D3DBOX *pBox);
  void markLosable();

private:
  // Push level i's mirror bytes to the GPU. Called by UnlockBox so the
  // MANAGED-pool semantics ("Lock to author, Unlock to publish") work
  // without an explicit UpdateTexture call. wined3d's
  // wined3d_texture_upload_data is the equivalent.
  void pushLevelToGpu(uint32_t level, const MTLD3D9Volume *vol);

  MTLD3D9Device *m_device;
  Rc<dxmt::Texture> m_texture;
  std::vector<Com<MTLD3D9Volume, false>> m_levels;
  std::vector<uint8_t> m_mirror;
  std::vector<size_t> m_mirrorOffsets;
  uint32_t m_level_count;
  DWORD m_usage;
  D3DPOOL m_pool;
  D3DFORMAT m_format;
  DWORD m_priority = 0;
  DWORD m_lod = 0;
  D3DTEXTUREFILTERTYPE m_autoGenFilter = D3DTEXF_LINEAR;
  WMTPixelFormat m_metalFormat = static_cast<WMTPixelFormat>(0);
  // Dirty-box union at level-0 extent. Default: fully dirty.
  bool m_dirty_any = true;
  D3DBOX m_dirty_box{};
  uint32_t m_width_l0 = 0;
  uint32_t m_height_l0 = 0;
  uint32_t m_depth_l0 = 0;
  bool m_self_pinned = true;
  // Losable-resource accounting: see d3d9_surface.hpp.
  bool m_isLosable = false;
  ComPrivateData m_privateData;
};

} // namespace dxmt
