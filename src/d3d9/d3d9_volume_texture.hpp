#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"
#include "d3d9_common_texture.hpp"
#include "d3d9_mem.hpp"
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
// Block-compressed (DXTn) volumes have no Metal 3D texture, so they exist
// only as a mirror-backed SCRATCH staging copy with a null m_texture; the
// mirror is sized in block rows (rowPitch × RowCount × depth).
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

  // Push a just-unlocked level's mirror contents to the GPU (MANAGED) and
  // auto-mark the dirty box. Called from MTLD3D9Volume::UnlockBox so the
  // upload fires whichever unlock interface the app uses (texture-level
  // UnlockBox delegates to the Volume).
  void flushLevelOnUnlock(uint32_t level, const MTLD3D9Volume *vol);

  // MTLD3D9CommonTexture overrides: forwarded inline to the
  // ComObject<IDirect3DVolumeTexture9> base; same diamond pattern as
  // MTLD3D9Texture / MTLD3D9CubeTexture.
  WMT::Texture
  metalTexture() const override {
    // Null for a mirror-only (block-compressed SCRATCH) volume: it has no Metal
    // 3D texture. Such a resource is never bound, so the bind path never asks.
    return m_texture != nullptr ? m_texture->current()->texture() : WMT::Texture{};
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
  uint32_t
  commonTextureLod() const override {
    return static_cast<uint32_t>(m_lod.load(std::memory_order_relaxed));
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

  // Mirror map brackets for LockBox / UnlockBox and UpdateTexture's
  // volume branch: every reader or writer of mirror bytes maps a view
  // for the duration of the access and unmaps after (no-ops on
  // 64-bit, where the pointer is permanently valid). Nesting is
  // refcounted so an UpdateTexture during an open lock stays mapped.
  uint8_t *
  mapMirror() {
    if (!m_mirror)
      return nullptr;
    if (m_mirrorMapRefs == 0)
      m_mirror.Map();
    uint8_t *ptr = static_cast<uint8_t *>(m_mirror.Ptr());
    // A failed view keeps the count at zero so the next attempt
    // retries the mapping instead of trusting a null pointer.
    if (ptr)
      ++m_mirrorMapRefs;
    return ptr;
  }
  void
  unmapMirror() {
    if (!m_mirror || m_mirrorMapRefs == 0)
      return;
    if (--m_mirrorMapRefs == 0)
      m_mirror.Unmap();
  }
  bool
  hasMirror() const {
    return static_cast<bool>(m_mirror);
  }
  // Convenience for LockBox: map and return the level's slice base.
  uint8_t *
  mapMirrorLevel(uint32_t level) {
    uint8_t *base = mapMirror();
    return base ? base + m_mirrorOffsets[level] : nullptr;
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
  // Mirror bytes live in the device's mirror allocator: transient
  // views on a 32-bit guest (mapped for the duration of a lock or
  // copy through mapMirror / unmapMirror), a plain allocation whose
  // brackets are no-ops on 64-bit. Never registered with Metal; the
  // GPU side is fed by staged uploads.
  D3D9Memory m_mirror;
  uint32_t m_mirrorMapRefs = 0;
  std::vector<size_t> m_mirrorOffsets;
  uint32_t m_level_count;
  DWORD m_usage;
  D3DPOOL m_pool;
  D3DFORMAT m_format;
  DWORD m_priority = 0;
  // Atomic: the encode-thread draw walker reads the LOD clamp (commonTextureLod)
  // without the device lock while a calling-thread SetLOD may write it. Relaxed
  // suffices; a one-frame-stale clamp is harmless.
  std::atomic<DWORD> m_lod = {0u};
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
  int64_t m_losableBytes = 0;
  ComPrivateData m_privateData;
};

} // namespace dxmt
