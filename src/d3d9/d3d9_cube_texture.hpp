#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"
#include "d3d9_common_texture.hpp"

#include <bitset>
#include "d3d9_surface.hpp"
#include "dxmt_texture.hpp"
#include "rc/util_rc_ptr.hpp"

#include <atomic>
#include <vector>

namespace dxmt {

class MTLD3D9Device;

// IDirect3DCubeTexture9: one TextureCube (6 faces × N mips) with
// 6×N pre-created MTLD3D9Surface views. Faces share WMT::Texture;
// mipLevel+arraySlice select render-pass attachments/samplers.
// Non-DEFAULT pools: per-face/level sysmem mirror. MANAGED unlocks
// push data via replaceRegion. References: wined3d texture.c.
class MTLD3D9CubeTexture final : public ComObject<IDirect3DCubeTexture9>,
                                 public MTLD3D9CommonTexture,
                                 public D9LazyMirrorHost {
public:
  // Owns a dxmt::Texture wrapping the MTLTextureCube allocation.
  // Single ctor shape: cube textures don't take the buffer-backed
  // path (MTLBuffer.newTexture only supports Type2D), so the caller
  // always builds Texture(info, device) and the dxmt allocate() path
  // creates a fresh MTLTexture from the descriptor.
  MTLD3D9CubeTexture(
      MTLD3D9Device *device, UINT edgeLength, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool,
      Rc<dxmt::Texture> texture
  );
  ~MTLD3D9CubeTexture();

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

  // IDirect3DCubeTexture9
  HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT Level, D3DSURFACE_DESC *pDesc) override;
  HRESULT STDMETHODCALLTYPE
  GetCubeMapSurface(D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9 **ppCubeMapSurface) override;
  HRESULT STDMETHODCALLTYPE
  LockRect(D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level) override;
  HRESULT STDMETHODCALLTYPE AddDirtyRect(D3DCUBEMAP_FACES FaceType, const RECT *pDirtyRect) override;

  // MTLD3D9CommonTexture overrides; see d3d9_common_texture.hpp. The
  // forward to ComObject avoids the diamond-name clash between the
  // base's non-virtual AddRefPrivate and MTLD3D9CommonTexture's pure
  // virtual.
  WMT::Texture
  metalTexture() const override {
    return m_texture->current()->texture();
  }
  // See MTLD3D9Texture::dxmtTexture; chunk lambdas capture this Rc<>
  // to keep the cube alive across the calling-thread → encode-thread
  // boundary.
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
    return D3DRTYPE_CUBETEXTURE;
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
  // Lazy sysmem mirror, the MTLD3D9Texture model applied face-major:
  // subresource face*levelCount() + level. ensureMirror materialises
  // the backing on first Lock and patches every face surface;
  // noteLevelUploaded drops a fully-uploaded MANAGED mirror back to
  // the device pool; materializeLevelForLock re-downloads an evicted
  // face's bytes before a read-Lock; restoreMirrorForSource does the
  // same before UpdateTexture reads the mirror.
  void ensureMirror() override;
  void noteLevelUploaded(uint32_t subresource) override;
  void materializeLevelForLock(uint32_t subresource) override;
  void restoreMirrorForSource();
  void dropMirror();

  // Mirror accessors used by UpdateTexture to source from a
  // SYSTEMMEM/MANAGED cube's CPU-side mirror; the caller runs
  // restoreMirrorForSource first so an evicted mirror is rebuilt.
  // UpdateTexture routes through stageTextureUpload (CPU pointer +
  // staging-ring memcpy), the same path the 2D UpdateTexture uses.
  const uint8_t *
  mirrorBase() const {
    return static_cast<const uint8_t *>(m_mirrorBacking);
  }
  size_t
  mirrorOffset(uint32_t face, uint32_t level) const {
    const size_t idx = static_cast<size_t>(face) * m_level_count + level;
    return idx < m_mirrorOffsets.size() ? m_mirrorOffsets[idx] : 0;
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
  // Per-face dirty-region tracking; see MTLD3D9Texture's header
  // comment for the shape rationale. Cube AddDirtyRect's API surface
  // is per-face (the only diff vs 2D); the storage matches.
  bool
  isDirty(uint32_t face) const {
    return face < 6 && m_dirty_any[face];
  }
  RECT
  dirtyRectLevel0(uint32_t face) const {
    return face < 6 ? m_dirty_rect[face] : RECT{};
  }
  void
  clearDirty(uint32_t face) {
    if (face < 6)
      m_dirty_any[face] = false;
  }
  void unionDirtyRect(uint32_t face, const RECT *pRect);
  bool
  mipsDirty() const override {
    return m_mips_dirty.load(std::memory_order_acquire);
  }
  void
  clearMipsDirty() override {
    m_mips_dirty.store(false, std::memory_order_release);
  }
  // See MTLD3D9Texture::flagAutoGenDirty for rationale: StretchRect-
  // as-dest of an AUTOGENMIPMAP cube routes through here for the same
  // lazy-flag protocol UnlockRect uses. Returns whether it marked dirty.
  bool
  flagAutoGenDirty() {
    if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP))
      return false;
    m_mips_dirty.store(true, std::memory_order_release);
    return true;
  }
  void
  AddRefPrivate() override {
    ComObject<IDirect3DCubeTexture9>::AddRefPrivate();
  }
  void
  ReleasePrivate() override {
    ComObject<IDirect3DCubeTexture9>::ReleasePrivate();
  }

private:
  MTLD3D9Device *m_device;
  // dxmt::Texture wrapping the MTLTextureCube. Per-face surfaces hold
  // their own WMT::Reference<WMT::Texture> retains so the underlying
  // NSObject survives any single dxmt::Texture rename; see
  // MTLD3D9Texture's m_texture comment for the full lifetime rationale.
  Rc<dxmt::Texture> m_texture;
  // 6 × N face/level surface views. Layout is face-major: face 0
  // takes m_levels[0..N-1], face 1 takes m_levels[N..2N-1], etc.
  // Same eager-creation rationale as MTLD3D9Texture; GetCubeMapSurface
  // must return the same surface object across calls, and the lookup
  // is on the bind path so a vector lookup is cheaper than a
  // synchronization primitive.
  std::vector<Com<MTLD3D9Surface, false>> m_levels;
  // Texture-wide Lock/GetDC coordination shared by every face/level surface
  // (wined3d resource.map_count). Each surface is pointed at this in the ctor.
  D3D9SurfaceLockState m_shared_lock_state;
  // Sysmem mirror for non-DEFAULT pools, sized for 6 faces × N
  // levels. Layout matches m_levels: face-major, then level. Per-
  // face/level offset table; total bytes at m_mirrorOffsets.back().
  // Allocated lazily on first Lock (ensureMirror) and reclaimed for
  // MANAGED once every subresource is uploaded (dropMirror), the
  // MTLD3D9Texture model; the only delta is the 6x face dimension,
  // which pushes the per-subresource masks past 64 bits, hence the
  // bitsets. m_mirrorBuffer exists solely for the device backing
  // pool's registration contract; the GPU never reads it.
  void *m_mirrorBacking = nullptr;
  WMT::Reference<WMT::Buffer> m_mirrorBuffer;
  std::vector<size_t> m_mirrorOffsets;
  bool m_needs_mirror = false;
  std::bitset<128> m_uploadedSubres;
  std::bitset<128> m_staleSubres;
  uint32_t m_mirror_download_count = 0;
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
  // Same exactly-once-drop pattern as MTLD3D9Texture::m_self_pinned.
  bool m_self_pinned = true;
  // AUTOGENMIPMAP lazy-flush bit; see MTLD3D9Texture::m_mips_dirty.
  std::atomic<bool> m_mips_dirty{false};
  // Per-face dirty tracking. Each face has its own union RECT in
  // level-0 coordinates; defaults to (true, full edge×edge) so a
  // freshly-created cube uploads everything on first consume.
  bool m_dirty_any[6] = {true, true, true, true, true, true};
  RECT m_dirty_rect[6] = {};
  // Edge length cached at ctor for AddDirtyRect(NULL) to know the
  // level-0 extent without walking m_levels.
  uint32_t m_edge_length = 0;
  // Losable-resource accounting: see d3d9_surface.hpp.
  bool m_isLosable = false;
  int64_t m_losableBytes = 0;

public:
  void markLosable();

private:
  ComPrivateData m_privateData;
};

} // namespace dxmt
