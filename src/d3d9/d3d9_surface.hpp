#pragma once

#include "Metal.hpp"
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"
#include "dxmt_texture.hpp"
#include "rc/util_rc_ptr.hpp"

#include <utility>

namespace dxmt {

class MTLD3D9Device;
class MTLD3D9Texture;

// Scaffold of IDirect3DSurface9: D3DSURFACE_DESC + container IUnknown*
// (device, texture, or swapchain). No MTLTexture yet.
// References: wined3d surface.c.
class MTLD3D9Surface final : public ComObject<IDirect3DSurface9> {
public:
  // selfPin=true: standalone surface, app-only owner; self-pin survives
  // Release until m_container is safe to drop. selfPin=false: sub-resource
  // owned by parent (texture/swapchain), stays alive via priv ref.
  // For lockable surfaces: buffer handle, CPU pointer, pitch for LockRect.
  // dxmtTexture: Rc<> wrapping MTLTexture for chunk lambdas to keep
  // allocation alive across calling→encode thread boundary.
  MTLD3D9Surface(
      MTLD3D9Device *device, const D3DSURFACE_DESC &desc, IUnknown *container, WMT::Reference<WMT::Texture> texture,
      uint32_t mipLevel, bool selfPin, WMTTextureType parentTextureType, WMT::Reference<WMT::Buffer> buffer = {},
      void *cpuPtr = nullptr, uint32_t pitch = 0, uint32_t arraySlice = 0, void *ownedBacking = nullptr,
      Rc<dxmt::Texture> dxmtTexture = nullptr, bool textureMipSurface = false
  );
  ~MTLD3D9Surface();

  // Internal accessors used by SetRenderTarget / Present blits / etc.
  // Not part of the IDirect3DSurface9 contract.
  WMT::Texture
  metalTexture() const {
    return m_texture;
  }
  // Lockable backing buffer + its row stride. Non-null only for SYSMEM /
  // SCRATCH / MANAGED surfaces: DEFAULT-pool surfaces have no host-
  // visible backing (m_buffer is zero-initialised). Readback paths
  // (GetRenderTargetData / GetFrontBufferData) prefer copyFromTexture:
  // toBuffer: over a texture-to-texture blit through the linear-texture
  // view because the latter has been observed to drop trailing rows on
  // virtualised Apple Silicon (GHA macos-26 runner): addressing the
  // buffer directly with explicit bytesPerRow sidesteps that path.
  WMT::Buffer
  metalBuffer() const {
    return m_buffer;
  }
  uint32_t
  pitch() const {
    return m_pitch;
  }
  // Raw access to the owning device: avoids an AddRef/Release pair
  // on the SetRenderTarget / SetDepthStencilSurface hot path that
  // only needs identity, not a public ref. Always non-null while the
  // surface is alive (the surface's own AddRef/Release pins the
  // container, which transitively keeps the device alive).
  MTLD3D9Device *
  deviceRaw() const {
    return m_device;
  }
  const D3DSURFACE_DESC &
  desc() const {
    return m_desc;
  }
  // Mip level this surface views into m_texture. 0 for standalone
  // surfaces (CreateRenderTarget, CreateDepthStencilSurface,
  // CreateOffscreenPlainSurface: m_texture is itself a single-level
  // allocation). For texture sub-resources the same Metal texture
  // handle is shared across N MTLD3D9Surface views, each with its
  // mipLevel field set to its index: render-pass attachments and
  // sampler bindings select the level from this field.
  uint32_t
  mipLevel() const {
    return m_mip_level;
  }
  // Array slice this surface views into m_texture. 0 for non-array
  // sources (CreateRenderTarget, plain CreateTexture mip levels). Cube
  // texture face surfaces set this to 0..5 to identify the face;
  // render-pass attachments and sampler bindings select the slice from
  // this field. Volume texture sub-resources will reuse the same slot
  // when they land.
  uint32_t
  arraySlice() const {
    return m_array_slice;
  }
  // Raw container pointer: same value GetContainer's QueryInterface
  // routes through. Callers that already know the COM-side type (e.g.
  // StretchRect's AUTOGENMIPMAP regen flag) downcast based on
  // IDirect3DBaseTexture9::GetType to avoid the QI Release pair.
  IUnknown *
  container() const {
    return m_container;
  }
  // wined3d device.c (StretchRect) + 2354 (rts_flag_auto_gen_mipmap)
  // both flag the destination/RT container's auto-gen mipmap dirty bit
  // after a successful op so the lazy regen sweep fires before the next
  // sample. Standalone surfaces and swapchain backbuffers fail the QI
  // and become no-ops; only Texture / CubeTexture containers route
  // through to MTLD3D9{Texture,CubeTexture}::flagAutoGenDirty (which
  // itself gates on D3DUSAGE_AUTOGENMIPMAP).
  void flagContainerAutoGenDirty();
  // Cached metal pixel format of the underlying texture (zero
  // wine_unix_call on the bind hot path). Mirrors metalTexture's
  // pixelFormat() value but reads from a member.
  WMTPixelFormat
  metalPixelFormat() const {
    return m_metalFormat;
  }
  // Chunk-emitcc draw lambdas capture this Rc<> to attach the surface as
  // a render target via ctx.access. Returns the parent texture's Rc<> for
  // per-level/per-face surfaces, the surface's own for standalone
  // allocations. May be null for purely sysmem surfaces: callers must
  // check.
  const Rc<dxmt::Texture> &
  dxmtTexture() const {
    return m_dxmtTexture;
  }

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

  // IDirect3DSurface9
  HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) override;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override;
  HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE UnlockRect() override;
  HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) override;
  HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override;

  // dxmt-internal accessors used by the parent texture's UnlockRect
  // hook to auto-mark the texture's dirty region with the lock rect,
  // matching wined3d texture.c (only top-level maps record
  // dirt, only non-READONLY locks). Returns the rect in pixel coords
  // at the surface's own level: caller scales to level-0 if needed.
  RECT
  lockedRect() const {
    return RECT{
        static_cast<LONG>(m_locked_x), static_cast<LONG>(m_locked_y), static_cast<LONG>(m_locked_x + m_locked_w),
        static_cast<LONG>(m_locked_y + m_locked_h)
    };
  }
  bool
  lockedReadOnly() const {
    return m_locked_readonly;
  }
  bool
  lockedNoDirtyUpdate() const {
    return m_locked_no_dirty_update;
  }

  // dxmt-internal: parent texture sets the mirror source after ctor so
  // UnlockRect can route to stageTextureUploadFromBuffer. Called once
  // per per-level surface in MTLD3D9Texture's buildLevelsAndMirror.
  void
  setMirrorSource(obj_handle_t buffer_handle, uint32_t level_offset) {
    m_mirror_src_buffer = buffer_handle;
    m_mirror_level_offset = level_offset;
  }

  // Lazy-mirror back-pointer: MTLD3D9Texture defers mirror allocation
  // until first LockRect. Surface-direct LockRect path drives alloc via
  // m_lazyMirrorParent; patchMirror called from ensureMirror().
  void
  setLazyMirrorParent(MTLD3D9Texture *parent) {
    m_lazyMirrorParent = parent;
  }
  void
  patchMirror(void *cpu_ptr, obj_handle_t buffer_handle, uint32_t level_offset, uint32_t pitch) {
    m_cpu_ptr = cpu_ptr;
    m_mirror_src_buffer = buffer_handle;
    m_mirror_level_offset = level_offset;
    m_pitch = pitch;
  }

  // Swap Metal backing in place (swapchain ResetForDeviceReset).
  // Preserves IDirect3DSurface9* identity; apps see current backbuffer
  // contents, not stale snapshot. New desc/texture replace backing.
  // Per-bind views resolved off m_dxmtTexture.
  void
  resetBacking(const D3DSURFACE_DESC &desc, WMT::Reference<WMT::Texture> texture, Rc<dxmt::Texture> dxmtTexture) {
    m_desc = desc;
    m_texture = std::move(texture);
    m_dxmtTexture = std::move(dxmtTexture);
    m_metalFormat = m_texture.pixelFormat();
  }

  // Swap Metal backing between surface objects (swapchain Present rotation, FLIP/FLIPEX/DISCARD).
  // Sibling backbuffer slots: contents shift per Present (0→N-1, 1→0, etc). Views carried by m_dxmtTexture.
  void
  SwapBacking(MTLD3D9Surface *other) {
    std::swap(m_texture, other->m_texture);
    std::swap(m_dxmtTexture, other->m_dxmtTexture);
  }

private:
  // Lifetime: device held by surface; first public AddRef→device AddRef, last Release→device Release.
  // Device-side bookkeeping (SetRenderTarget storing bound surfaces) uses private refs only, never public.
  // Ctor self-pins via AddRefPrivate; pin released at end of Release (if no other priv refs, destructs immediately).
  MTLD3D9Device *m_device;
  // Raw: the container (parent texture / swapchain / device) outlives
  // the surface by construction. Swapchain backbuffer surfaces will
  // store the chain here; CreateRenderTarget standalone surfaces will
  // store the device. wined3d returns E_NOINTERFACE when container is
  // null; we never construct a surface with null container, but the
  // GetContainer path defensively handles it.
  IUnknown *m_container;
  D3DSURFACE_DESC m_desc;
  // Lockable-only backing buffer; the texture below is a view into it.
  // Declared before m_texture so the buffer outlives the view at
  // destruction. Null for non-lockable surfaces.
  WMT::Reference<WMT::Buffer> m_buffer;
  WMT::Reference<WMT::Texture> m_texture;
  // Chunk-lambda capture handle. For per-level / per-face surfaces this
  // points at the parent texture's dxmt::Texture; for standalone surfaces
  // (RT, DS, OffscreenPlain, swapchain backbuffer) it owns the standalone
  // allocation. Null for purely sysmem surfaces: m_texture is the source
  // of truth for those.
  Rc<dxmt::Texture> m_dxmtTexture;
  uint32_t m_mip_level;
  uint32_t m_array_slice;
  bool m_self_pinned;
  DWORD m_priority = 0;
  ComPrivateData m_privateData;
  WMTPixelFormat m_metalFormat = static_cast<WMTPixelFormat>(0);
  // CPU pointer + pitch handed back from LockRect; both 0/null when
  // m_buffer is null.
  void *m_cpu_ptr = nullptr;
  uint32_t m_pitch = 0;
  bool m_locked = false;
  // True iff the parent container is a D3DRTYPE_TEXTURE (2D). Toggles
  // the relaxed double-Unlock contract: wined3d surface.c returns
  // D3D_OK for a double-unlock when the container is a 2D texture and
  // INVALIDCALL otherwise. Set via the ctor; default false covers
  // standalone surfaces, swapchain backbuffers, and cube-face surfaces
  // (wined3d does not relax cube: only D3DRTYPE_TEXTURE).
  bool m_is_texture_mip = false;
  // Per-Lock state read by UnlockRect. The dirty rect is stored in
  // pixel coords (whole-surface if the app passed pRect=NULL). The
  // readonly bit elides the MANAGED replaceRegion entirely: apps
  // that promise not to write must not get their data echoed back.
  bool m_locked_readonly = false;
  // D3DLOCK_NO_DIRTY_UPDATE: when set, the parent texture's UnlockRect
  // skips the implicit unionDirtyRect so apps that AddDirtyRect
  // manually after the Lock get exactly the region they passed, not
  // a superset including the auto-recorded lock rect. DXVK honours it
  // on every pool except DEFAULT (`d3d9_device.cpp`).
  bool m_locked_no_dirty_update = false;
  uint32_t m_locked_x = 0;
  uint32_t m_locked_y = 0;
  uint32_t m_locked_w = 0;
  uint32_t m_locked_h = 0;
  // Mirror-buffer upload source. Distinct from m_buffer (which marks
  // "surface storage IS this buffer, skip upload": the buffer-backed
  // path). When m_mirror_src_buffer is non-zero, MANAGED UnlockRect
  // records a buffer→texture blit using this handle directly, no host
  // memcpy. m_mirror_level_offset is the start of this level inside
  // the parent's mirror buffer; the dirty-rect offset is added on top
  // at Unlock time.
  obj_handle_t m_mirror_src_buffer = 0;
  uint32_t m_mirror_level_offset = 0;
  // Lazy-mirror parent: only set on per-level surfaces of a MANAGED/
  // SYSTEMMEM/SCRATCH MTLD3D9Texture whose mirror hasn't been alloc'd
  // yet. LockRect dispatches to ensureMirror() through this pointer
  // before the m_cpu_ptr null check. Null for swapchain backbuffer,
  // standalone RT/DS, OffscreenPlain, and DEFAULT-pool surfaces.
  // Lifetime: the parent texture's m_levels vector holds a private
  // ref on this surface, so the parent strictly outlives the surface.
  MTLD3D9Texture *m_lazyMirrorParent = nullptr;
  // Process-allocated backing for newBufferWithBytesNoCopy. dxmt
  // pre-allocates the storage via wsi::aligned_malloc and hands it to
  // Metal so the lockable host pointer always lives in the calling
  // process's <4 GB address space: without the placement, Metal can
  // return a high-memory pointer that 32-bit Windows games cannot
  // reach. Owned by this object; dtor frees via wsi::aligned_free.
  // Null when m_buffer is using a Metal-owned allocation (DEFAULT-pool
  // RTs, future Private paths).
  void *m_owned_backing = nullptr;
  // Losable-resource accounting. App-facing CreateRenderTarget /
  // CreateDepthStencilSurface / CreateOffscreenPlainSurface call
  // markLosable() right before AddRef; the leaf dtor decrements the
  // device's counter so Reset's "no app-held DEFAULT resources" gate
  // can read it. Implicit RT0 / auto-DS surfaces never call
  // markLosable(): they're device/swapchain-owned and shouldn't
  // count.
  bool m_isLosable = false;

public:
  void markLosable();
};

} // namespace dxmt
