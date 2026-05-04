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

// Scaffold of IDirect3DSurface9. Holds the D3DSURFACE_DESC the
// runtime exposes plus a parent IUnknown* for GetContainer (device,
// texture, or swapchain depending on origin). No MTLTexture yet —
// the Metal-backing fields land alongside CreateRenderTarget,
// CreateDepthStencilSurface, and the swapchain's backbuffer surface
// in follow-up commits.
//
// References (vtable shape, GetContainer / GetDesc / Get*Type
// semantics, refcount obligations): wined3d dlls/d3d9/surface.c. MGL
// will be the reference once we wire the Metal pixel-format lowering
// in CreateRenderTarget.
class MTLD3D9Surface final : public ComObject<IDirect3DSurface9> {
public:
  // selfPin = true (the standalone case — CreateRenderTarget,
  // CreateDepthStencilSurface, CreateOffscreenPlainSurface): the
  // surface's only owner is the app's public ref. The ctor takes a
  // private self-pin so the override Release path can call
  // m_container->Release safely after ComObject::Release has dropped
  // pub=0 — the self-pin keeps `this` alive. Released at the end of
  // Release.
  //
  // selfPin = false (sub-resource case — texture mip levels, future
  // cube/volume surfaces, and the swapchain backbuffer): a container
  // already owns the surface via a private ref (Com<MTLD3D9Surface,
  // false>). No self-pin: when public hits 0 the surface stays alive
  // because the container's priv ref keeps it; the container's
  // destructor / Reset path drops the priv ref to actually destroy
  // it. Skipping the self-pin avoids a leak when the app never
  // retrieves the sub-surface (e.g. a mip level of a texture that's
  // released without GetSurfaceLevel).
  // For lockable surfaces, also pass the underlying buffer + its CPU
  // pointer + the pitch used at view-creation time. LockRect returns
  // (cpuPtr + offset, pitch) without copying.
  //
  // parentTextureType describes the Metal texture type of the parent
  // `texture` handle, not the D3D9 surface itself (Cube-face surfaces
  // alias a Cube parent, etc.). Retained on the signature for the
  // callers that already compute it; per-bind views are now derived off
  // dxmtTexture via dxmt::Texture::createView, which carries the
  // parent's type, so the surface itself no longer keys a view cache on
  // it.
  // dxmtTexture: Rc<dxmt::Texture> wrapping the underlying MTLTexture
  // for chunk-emitcc lambdas to capture (so ctx.access can attach the
  // surface as an RT / sample source from the encode thread, keeping
  // the allocation + its views alive to GPU completion). Per-level /
  // per-face surfaces receive their parent texture's Rc<>; standalone
  // surfaces (CreateRenderTarget, CreateDepthStencilSurface,
  // CreateOffscreenPlainSurface, swapchain backbuffer) build a fresh
  // dxmt::Texture wrapping `texture`.
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
  // SCRATCH / MANAGED surfaces — DEFAULT-pool surfaces have no host-
  // visible backing (m_buffer is zero-initialised). Readback paths
  // (GetRenderTargetData / GetFrontBufferData) prefer copyFromTexture:
  // toBuffer: over a texture-to-texture blit through the linear-texture
  // view because the latter has been observed to drop trailing rows on
  // virtualised Apple Silicon (GHA macos-26 runner) — addressing the
  // buffer directly with explicit bytesPerRow sidesteps that path.
  WMT::Buffer
  metalBuffer() const {
    return m_buffer;
  }
  uint32_t
  pitch() const {
    return m_pitch;
  }
  // Raw access to the owning device — avoids an AddRef/Release pair
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
  // CreateOffscreenPlainSurface — m_texture is itself a single-level
  // allocation). For texture sub-resources the same Metal texture
  // handle is shared across N MTLD3D9Surface views, each with its
  // mipLevel field set to its index — render-pass attachments and
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
  // Raw container pointer — same value GetContainer's QueryInterface
  // routes through. Callers that already know the COM-side type (e.g.
  // StretchRect's AUTOGENMIPMAP regen flag) downcast based on
  // IDirect3DBaseTexture9::GetType to avoid the QI Release pair.
  IUnknown *
  container() const {
    return m_container;
  }
  // wined3d device.c:2066 (StretchRect) + 2354 (rts_flag_auto_gen_mipmap)
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
  // allocations. May be null for purely sysmem surfaces — callers must
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
  // matching wined3d texture.c:1221-1224 (only top-level maps record
  // dirt, only non-READONLY locks). Returns the rect in pixel coords
  // at the surface's own level — caller scales to level-0 if needed.
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

  // dxmt-internal: lazy-mirror back-pointer + patch hook. The
  // MTLD3D9Texture builds per-level surfaces eagerly so GetSurfaceLevel
  // returns a stable IDirect3DSurface9*, but the underlying mirror
  // memory (wsi::aligned_malloc + Metal newBuffer thunk) is deferred
  // until first LockRect. Texture sets m_lazyMirrorParent so the
  // surface-direct LockRect path (apps that hold the surface via
  // GetSurfaceLevel and skip the texture-level LockRect) can drive
  // the alloc. patchMirror is called from MTLD3D9Texture::ensureMirror
  // once the buffer exists to fill in this level's view of it.
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

  // dxmt-internal: swap the Metal backing in place. Used by the
  // swapchain's ResetForDeviceReset to preserve IDirect3DSurface9*
  // identity across Reset — wined3d swapchain.c::wined3d_swapchain_reset
  // and DXVK D3D9SwapChainEx::Reset both keep their backbuffer
  // surfaces' object identity stable, just swapping in the new
  // texture/format/extent. Apps that called GetBackBuffer before
  // Reset and held the IDirect3DSurface9* get the *current*
  // backbuffer contents after Reset, not a stale snapshot. The new
  // desc replaces m_desc so GetDesc returns the new dimensions /
  // format. The new texture handle + dxmt::Texture replace the
  // backing. Intended only for DEFAULT-pool surfaces with no mirror
  // buffer; m_buffer / m_cpu_ptr / m_owned_backing are not touched (the
  // swapchain backbuffer has none). Per-bind views are resolved off
  // m_dxmtTexture now, so swapping it in is all the rebind needs.
  void
  resetBacking(const D3DSURFACE_DESC &desc, WMT::Reference<WMT::Texture> texture, Rc<dxmt::Texture> dxmtTexture) {
    m_desc = desc;
    m_texture = std::move(texture);
    m_dxmtTexture = std::move(dxmtTexture);
    m_metalFormat = m_texture.pixelFormat();
  }

  // Swap the Metal backing between two surface objects without changing
  // either object's identity. DXVK's D3D9Subresource::Swap shape — used
  // by the swapchain's Present rotation under SwapEffect=FLIP / FLIPEX /
  // DISCARD so app-held IDirect3DSurface9* from prior GetBackBuffer(i)
  // calls keep referring to the same slot index, with the slot's contents
  // shifting one position per Present (slot 0 becomes the new slot N-1,
  // slot 1 becomes the new slot 0, etc).
  //
  // Both surfaces are assumed to be sibling swapchain backbuffers:
  // identical desc / format / non-lockable / no mirror buffer. Per-bind
  // views live on the dxmt::Texture now, so swapping m_dxmtTexture (and
  // the raw m_texture handle) carries the view state with it.
  void
  SwapBacking(MTLD3D9Surface *other) {
    std::swap(m_texture, other->m_texture);
    std::swap(m_dxmtTexture, other->m_dxmtTexture);
  }

private:
  // Lifetime contract — same shape as MTLD3D9SwapChain (see its header
  // for the full rationale):
  //   - Surface holds raw MTLD3D9Device*. First public AddRef bumps
  //     the device's refcount, last public Release drops it. Future
  //     device-side bookkeeping (SetRenderTarget storing bound
  //     surfaces) takes private refs only, never public.
  //   - The constructor calls AddRefPrivate as a self-pin so the
  //     surface survives its own last public Release long enough for
  //     the override to call m_device->Release safely. The self-pin
  //     is dropped at the end of the override; if no other private
  //     ref is held (the app-only-owned case), the surface destructs
  //     immediately. If a chain or texture also holds an
  //     AddRefPrivate (the backbuffer/sub-surface case), it stays
  //     alive until that pin is released.
  // The cached m_desc / m_texture / m_dxmtTexture / m_metalFormat are
  // refreshed by resetBacking() on the swapchain's Reset path, so a
  // backbuffer surface held across a resolution change reports its
  // new dimensions / format and resolves views against the new
  // texture.
  MTLD3D9Device *m_device;
  // Raw — the container (parent texture / swapchain / device) outlives
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
  // allocation. Null for purely sysmem surfaces — m_texture is the source
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
  // the relaxed double-Unlock contract — wined3d surface.c:284 returns
  // D3D_OK for a double-unlock when the container is a 2D texture and
  // INVALIDCALL otherwise. Set via the ctor; default false covers
  // standalone surfaces, swapchain backbuffers, and cube-face surfaces
  // (wined3d does not relax cube — only D3DRTYPE_TEXTURE).
  bool m_is_texture_mip = false;
  // Per-Lock state read by UnlockRect. The dirty rect is stored in
  // pixel coords (whole-surface if the app passed pRect=NULL). The
  // readonly bit elides the MANAGED replaceRegion entirely — apps
  // that promise not to write must not get their data echoed back.
  bool m_locked_readonly = false;
  // D3DLOCK_NO_DIRTY_UPDATE — when set, the parent texture's UnlockRect
  // skips the implicit unionDirtyRect so apps that AddDirtyRect
  // manually after the Lock get exactly the region they passed, not
  // a superset including the auto-recorded lock rect. DXVK honours it
  // on every pool except DEFAULT (`d3d9_device.cpp:5219`).
  bool m_locked_no_dirty_update = false;
  uint32_t m_locked_x = 0;
  uint32_t m_locked_y = 0;
  uint32_t m_locked_w = 0;
  uint32_t m_locked_h = 0;
  // Mirror-buffer upload source. Distinct from m_buffer (which marks
  // "surface storage IS this buffer, skip upload" — the buffer-backed
  // path). When m_mirror_src_buffer is non-zero, MANAGED UnlockRect
  // records a buffer→texture blit using this handle directly, no host
  // memcpy. m_mirror_level_offset is the start of this level inside
  // the parent's mirror buffer; the dirty-rect offset is added on top
  // at Unlock time.
  obj_handle_t m_mirror_src_buffer = 0;
  uint32_t m_mirror_level_offset = 0;
  // Lazy-mirror parent — only set on per-level surfaces of a MANAGED/
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
  // process's <4 GB address space — without the placement, Metal can
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
  // markLosable() — they're device/swapchain-owned and shouldn't
  // count.
  bool m_isLosable = false;

public:
  void markLosable();
};

} // namespace dxmt
