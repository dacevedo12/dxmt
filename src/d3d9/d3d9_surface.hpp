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

// Shared Lock/GetDC bookkeeping for all surfaces of one texture (wined3d's
// resource.map_count + per-texture DC exclusion). See MTLD3D9Surface.
struct D3D9SurfaceLockState {
  uint32_t map_count = 0;
  bool dc_open = false;
};

class MTLD3D9Device;
class MTLD3D9Texture;

// Lazy-mirror protocol between a level surface and its container
// texture: the container defers sysmem-mirror allocation until first
// LockRect, re-downloads an evicted level's bytes before a MANAGED
// lock, and drops the mirror once every level has been uploaded. 2D
// and cube containers implement it; the surface addresses its own
// level through the subresource index fixed at creation (mip for 2D,
// face-major for cubes).
struct D9LazyMirrorHost {
  virtual void ensureMirror() = 0;
  virtual void materializeLevelForLock(uint32_t subresource) = 0;
  virtual void noteLevelUploaded(uint32_t subresource) = 0;
  // A NO_DIRTY_UPDATE Unlock of a MANAGED sub-resource records no dirty region,
  // so per wined3d + DXVK the written bytes stay in the sysmem mirror until a
  // later AddDirtyRect / EvictManagedResources / plain Unlock marks them; the
  // GPU copy is not refreshed. Only the 2D texture host has that deferred
  // re-upload path (its pre-draw managed sweep + AddDirtyRect eager-upload)
  // today, so it returns true; the cube host has none, so it keeps the eager
  // upload-on-unlock and returns false here (unchanged behaviour).
  virtual bool
  deferManagedNoDirtyUpload() const {
    return false;
  }

protected:
  ~D9LazyMirrorHost() = default;
};

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
      Rc<dxmt::Texture> dxmtTexture = nullptr, bool textureMipSurface = false,
      IDirect3DBaseTexture9 *baseTexture = nullptr
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
  void *
  cpuPtr() const {
    return m_cpu_ptr;
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
  // True when this surface is a sub-resource of a texture (GetSurfaceLevel
  // or a cube face), false for standalone surfaces (CreateRenderTarget,
  // CreateDepthStencilSurface, CreateOffscreenPlainSurface, swapchain
  // backbuffer). ColorFill rejects a non-render-target texture sub-resource.
  bool
  isTextureSubresource() const {
    return m_baseTexture != nullptr;
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
  // Union a lock rect into the parent texture's level-0 dirty region (2D:
  // whole-texture, cube: per-face via m_array_slice) through the same
  // container QI the auto-gen flag uses. Lets a direct surface unlock feed
  // UpdateTexture's dirty-region tracking the way the texture wrapper does.
  void flagContainerDirtyRegion(const RECT *rect);
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

  // dxmt-internal accessor used by UnlockRect to auto-mark the parent
  // texture's dirty region with the lock rect, matching wined3d texture.c
  // (only top-level maps record dirt, only non-READONLY locks). Returns the
  // rect in pixel coords at the surface's own level; only level 0 records, so
  // no scaling is needed.
  RECT
  lockedRect() const {
    return RECT{
        static_cast<LONG>(m_locked_x), static_cast<LONG>(m_locked_y), static_cast<LONG>(m_locked_x + m_locked_w),
        static_cast<LONG>(m_locked_y + m_locked_h)
    };
  }
  bool
  locked() const {
    return m_locked;
  }
  bool
  lockedReadOnly() const {
    return m_locked_readonly;
  }
  bool
  lockedNoDirtyUpdate() const {
    return m_locked_no_dirty_update;
  }

  // Lazy-mirror back-pointer: the container defers mirror allocation
  // until first LockRect. Surface-direct LockRect path drives alloc via
  // m_lazyMirrorParent; patchMirror called from ensureMirror().
  void
  setLazyMirrorParent(D9LazyMirrorHost *parent, uint32_t subresource) {
    m_lazyMirrorParent = parent;
    m_lazy_subresource = subresource;
  }
  // Point this surface at its parent texture's shared Lock/GetDC state so all
  // sibling surfaces coordinate texture-wide (see D3D9SurfaceLockState). Called
  // by the owning texture for each level/face surface it creates.
  void
  setSharedLockState(D3D9SurfaceLockState *state) {
    if (state)
      m_lock_state = state;
  }
  void
  patchMirror(void *cpu_ptr, uint32_t pitch) {
    m_cpu_ptr = cpu_ptr;
    m_pitch = pitch;
  }
  // Inverse of patchMirror for MANAGED mirror eviction (MTLD3D9Texture::
  // dropMirror): null the CPU pointer so the next LockRect re-arms the mirror
  // through m_lazyMirrorParent->ensureMirror(). The lazy back-pointer stays
  // set; ensureMirror re-patches every level on the re-allocation.
  void
  clearMirrorPatch() {
    m_cpu_ptr = nullptr;
  }

  // Swap Metal backing in place (swapchain ResetForDeviceReset).
  // Preserves IDirect3DSurface9* identity; apps see current backbuffer
  // contents, not stale snapshot. New desc/texture replace backing.
  // Per-bind views resolved off m_dxmtTexture.
  // Reset orphaned this backbuffer: the chain no longer owns it, so
  // GetContainer identity falls back to the device (E_NOINTERFACE for the
  // swapchain, the device still answers), while the desc and contents stay
  // the pre-Reset ones. Refs already pin the device, so this is only the
  // identity swap; wine's d3d9ex tests pin the contract.
  void detachContainer();

  void
  resetBacking(const D3DSURFACE_DESC &desc, WMT::Reference<WMT::Texture> texture, Rc<dxmt::Texture> dxmtTexture) {
    m_desc = desc;
    m_texture = std::move(texture);
    m_dxmtTexture = std::move(dxmtTexture);
    m_metalFormat = m_texture.pixelFormat();
  }
  // Swap the lockable host mirror across a swapchain Reset: free the old backing
  // and adopt the new one (or null to make a no-longer-lockable backbuffer
  // GPU-only). Defined out-of-line to keep wsi out of this header.
  void resetLockableMirror(void *cpuPtr, uint32_t pitch, void *ownedBacking);

  // Materialise the lazy sysmem mirror a texture-level surface defers
  // until first Lock (the same dispatch LockRect performs); a no-op
  // for surfaces that already carry a backing or have none to defer.
  void ensureHostMirror();

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
  // The parent base texture when this surface is a texture / cube mip-level
  // sub-resource, else null. When set, AddRef/Release delegate entirely to it
  // so the level shares the parent's public refcount (the D3D9 sub-resource
  // contract, DXVK D3D9Subresource); when null the surface is standalone or
  // the implicit backbuffer and pins the device on its own 0<->1 edge.
  // Separate from m_container (GetContainer identity) so the backbuffer can
  // report the swapchain while pinning the device.
  IDirect3DBaseTexture9 *m_baseTexture;
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
  ComPrivateData m_privateData;
  WMTPixelFormat m_metalFormat = static_cast<WMTPixelFormat>(0);
  // CPU pointer + pitch handed back from LockRect; both 0/null when
  // m_buffer is null.
  void *m_cpu_ptr = nullptr;
  uint32_t m_pitch = 0;
  bool m_locked = false;
  // Texture-wide Lock/GetDC coordination (wined3d resource.map_count model).
  // map_count counts every active Lock and GetDC across ALL sibling surfaces of
  // a texture; dc_open marks that one of them holds a GDI DC. GetDC requires
  // map_count == 0 (nothing mapped anywhere on the texture); LockRect on a
  // sibling is allowed while another is locked but not while a DC is open. A
  // standalone surface points at its own m_own_lock_state; 2D-texture and cube
  // surfaces are pointed at the parent's shared instance via setSharedLockState.
  D3D9SurfaceLockState m_own_lock_state;
  D3D9SurfaceLockState *m_lock_state = &m_own_lock_state;
#ifdef _WIN32
  // GDI text composition (GetDC/ReleaseDC): apps rasterize UI text into a
  // sampled texture via GDI. GetDC locks the surface and hands GDI a DC created
  // directly over the locked bytes (D3DKMTCreateDCFromMemory), so GDI paints
  // into the same memory LockRect exposes and UnlockRect uploads. Matches
  // wined3d's get_dc and DXVK's D3D9Surface::GetDC. These handles are live only
  // between a GetDC and its matching ReleaseDC.
  HDC m_gdi_dc = nullptr;
  HANDLE m_gdi_bitmap = nullptr;
#endif
  // True iff the parent container is a D3DRTYPE_TEXTURE (2D). Toggles
  // the relaxed double-Unlock contract: a redundant unlock of a mapped-once
  // 2D-texture mip returns D3D_OK (DXVK/wined3d gate on the container type).
  // Set via the ctor; default false covers standalone surfaces, swapchain
  // backbuffers, and cube-face surfaces (only D3DRTYPE_TEXTURE relaxes).
  bool m_is_texture_mip = false;
  // Latches the first successful Lock and is never cleared. Native D3D9
  // forgives only a redundant unlock (mapped at least once); an Unlock of a
  // never-mapped surface is INVALIDCALL even for a texture mip. UnlockRect
  // reads this alongside m_is_texture_mip to match native on both cases.
  bool m_ever_locked = false;
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
  // Lazy-mirror parent: only set on per-level surfaces of a MANAGED/
  // SYSTEMMEM/SCRATCH MTLD3D9Texture whose mirror hasn't been alloc'd
  // yet. LockRect dispatches to ensureMirror() through this pointer
  // before the m_cpu_ptr null check. Null for swapchain backbuffer,
  // standalone RT/DS, OffscreenPlain, and DEFAULT-pool surfaces.
  // Lifetime: the parent texture's m_levels vector holds a private
  // ref on this surface, so the parent strictly outlives the surface.
  D9LazyMirrorHost *m_lazyMirrorParent = nullptr;
  uint32_t m_lazy_subresource = 0;
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
  int64_t m_losableBytes = 0;
  // Implicit-surface loss accounting (swapchain backbuffers, auto depth-
  // stencil). Unlike m_isLosable (counted at create for app-created DEFAULT
  // resources), an implicit surface counts in the device's loss gate only
  // while the app holds a public reference: markImplicitLosable() sets this
  // one-shot flag and the public 0<->1 refcount edge drives the counter.
  // Native fails a non-Ex Reset while an app-held backbuffer / auto-DS is
  // alive; this is the pub-edge form of that. The two flags never co-occur.
  bool m_isImplicitLosable = false;

public:
  void markLosable();
  void markImplicitLosable();
  // D3D9Ex CreateRenderTargetEx / CreateDepthStencilSurfaceEx carry extra
  // informational Usage bits (RESTRICTED_CONTENT, the shared-resource
  // restrictions) that the fixed non-Ex create signature cannot thread in.
  // The Ex method ORs them onto the base RT/DS usage before the surface
  // reaches the app; only GetDesc reads them (dxmt does not enforce content
  // protection).
  void
  addDescUsage(DWORD usage) {
    m_desc.Usage |= usage;
  }
};

} // namespace dxmt
