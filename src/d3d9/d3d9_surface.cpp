#include "d3d9_surface.hpp"

#include "d3d9_cube_texture.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_image_lock.hpp"
#include "d3d9_private_data.hpp"
#include "d3d9_stall.hpp"
#include "d3d9_texture.hpp"
#include "wsi_platform.hpp"

#include <chrono>
#include <cstring>

namespace dxmt {

#ifdef _WIN32
namespace {
// GetDC backs a GDI device context with a bitmap created directly over the
// surface's locked CPU bytes (D3DKMTCreateDCFromMemory), the wined3d
// (wined3d_texture_get_dc) and DXVK (D3D9Surface::GetDC) shape. The toolchain
// ships no DDK header for it and the import lib may not carry the symbol, so
// declare the stable kernel-thunk ABI here and resolve the two entry points
// from gdi32 at runtime (wine exports both, mapped to win32u NtGdiDdDDI*).
struct D3DKMT_CREATEDCFROMMEMORY {
  void *pMemory;
  D3DFORMAT Format; // D3DDDIFORMAT, numerically a D3DFORMAT for these formats
  UINT Width;
  UINT Height;
  UINT Pitch;
  HDC hDeviceDc;
  PALETTEENTRY *pColorTable;
  HDC hDc;
  HANDLE hBitmap;
};
struct D3DKMT_DESTROYDCFROMMEMORY {
  HDC hDc;
  HANDLE hBitmap;
};

LONG
d3dkmtCreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *arg) {
  using Fn = LONG(WINAPI *)(D3DKMT_CREATEDCFROMMEMORY *);
  static Fn fn =
      reinterpret_cast<Fn>(::GetProcAddress(::GetModuleHandleA("gdi32.dll"), "D3DKMTCreateDCFromMemory"));
  return fn ? fn(arg) : -1;
}

LONG
d3dkmtDestroyDCFromMemory(const D3DKMT_DESTROYDCFROMMEMORY *arg) {
  using Fn = LONG(WINAPI *)(const D3DKMT_DESTROYDCFROMMEMORY *);
  static Fn fn =
      reinterpret_cast<Fn>(::GetProcAddress(::GetModuleHandleA("gdi32.dll"), "D3DKMTDestroyDCFromMemory"));
  return fn ? fn(arg) : -1;
}
} // namespace
#endif

MTLD3D9Surface::MTLD3D9Surface(
    MTLD3D9Device *device, const D3DSURFACE_DESC &desc, IUnknown *container, WMT::Reference<WMT::Texture> texture,
    uint32_t mipLevel, bool selfPin, WMTTextureType parentTextureType, WMT::Reference<WMT::Buffer> buffer, void *cpuPtr,
    uint32_t pitch, uint32_t arraySlice, void *ownedBacking, Rc<dxmt::Texture> dxmtTexture, bool textureMipSurface,
    IDirect3DBaseTexture9 *baseTexture
) :
    m_device(device),
    m_container(container),
    m_baseTexture(baseTexture),
    m_desc(desc),
    m_buffer(std::move(buffer)),
    m_texture(std::move(texture)),
    m_dxmtTexture(std::move(dxmtTexture)),
    m_mip_level(mipLevel),
    m_array_slice(arraySlice),
    m_self_pinned(selfPin),
    m_cpu_ptr(cpuPtr),
    m_pitch(pitch),
    m_is_texture_mip(textureMipSurface),
    m_owned_backing(ownedBacking) {
  if (m_self_pinned)
    AddRefPrivate();
  // parentTextureType records what kind of texture this surface is a
  // sub-resource of (Cube → 6 faces, 2D → 1 slice). Per-bind views are
  // now resolved off m_dxmtTexture via dxmt::Texture::createView, which
  // already knows the parent's type, so the surface only needs to cache
  // the metal pixel format here.
  (void)parentTextureType;
  if (m_texture != nullptr)
    m_metalFormat = m_texture.pixelFormat();
}

MTLD3D9Surface::~MTLD3D9Surface() {
#ifdef _WIN32
  // A GDI DC left open at destruction (a GetDC with no matching ReleaseDC):
  // tear it down through the same kernel thunk that created it (see GetDC).
  // The DC's bitmap aliases the surface's CPU bytes, so destroy it before the
  // backing it points at is freed below.
  if (m_gdi_dc) {
    D3DKMT_DESTROYDCFROMMEMORY destroy{};
    destroy.hDc = m_gdi_dc;
    destroy.hBitmap = m_gdi_bitmap;
    d3dkmtDestroyDCFromMemory(&destroy);
  }
#endif
  // Drop the texture-view and the underlying MTLBuffer first so the
  // GPU stops referencing the backing before we free it. WMT::Reference
  // destructors handle the release; explicit reset for ordering clarity.
  m_texture = WMT::Reference<WMT::Texture>{};
  m_buffer = WMT::Reference<WMT::Buffer>{};
  if (m_owned_backing)
    wsi::aligned_free(m_owned_backing);
  if (m_isLosable)
    m_device->onLosableResourceDestroyed(m_losableBytes);
}

void
MTLD3D9Surface::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    // Reported bytes for GetAvailableTextureMem; see MTLD3D9Texture. Depth and
    // NULL formats have no packed bpp, so fall back to a 4-byte estimate.
    uint32_t pitch = D3DFormatLockPitch(m_desc.Format, m_desc.Width);
    if (pitch == 0)
      pitch = m_desc.Width * 4u;
    uint32_t rows = D3DFormatRowCount(m_desc.Format, m_desc.Height);
    if (rows == 0)
      rows = m_desc.Height;
    m_losableBytes = static_cast<int64_t>(pitch) * static_cast<int64_t>(rows);
    m_device->onLosableResourceCreated(m_losableBytes);
  }
}

void
MTLD3D9Surface::markImplicitLosable() {
  // One-shot flag; the counting happens on the public refcount edge in AddRef /
  // Release, not here. See m_isImplicitLosable.
  m_isImplicitLosable = true;
}

void
MTLD3D9Surface::ensureHostMirror() {
  if (!m_cpu_ptr && m_lazyMirrorParent)
    m_lazyMirrorParent->ensureMirror();
}

void
MTLD3D9Surface::resetLockableMirror(void *cpuPtr, uint32_t pitch, void *ownedBacking) {
  if (m_owned_backing)
    wsi::aligned_free(m_owned_backing);
  m_owned_backing = ownedBacking;
  m_cpu_ptr = cpuPtr;
  m_pitch = pitch;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Surface::AddRef() {
  // Texture / cube mip-level surface: share the parent texture's public
  // counter so get_refcount(level) == get_refcount(parent), per the D3D9
  // sub-resource contract (DXVK D3D9Subresource). The parent owns this level,
  // so the whole body delegates and never touches `this` afterwards.
  if (m_baseTexture)
    return m_baseTexture->AddRef();
  // Standalone surface (CreateRenderTarget / DepthStencil / Offscreen, the
  // auto-DS) and the implicit backbuffer pin the DEVICE on their public 0->1
  // edge, like every DXVK D3D9DeviceChild. m_container is GetContainer
  // identity only: the backbuffer's container is the swapchain, but its ref
  // pins the device.
  ULONG ref = ComObject::AddRef();
  if (ref == 1) {
    m_device->AddRef();
    // Implicit surface (backbuffer / auto-DS): count in the device's loss gate
    // while the app holds a public ref, so a non-Ex Reset fails until the app
    // releases it. Bytes 0: implicit resources stay out of the memory report.
    if (m_isImplicitLosable)
      m_device->onLosableResourceCreated();
  }
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Surface::Release() {
  // Sub-resource: delegate the whole public Release to the parent texture.
  // The parent can destruct synchronously inside this call (its m_levels
  // clears and deletes `this`), so the result must come from the delegated
  // call and `this` must not be read afterwards.
  if (m_baseTexture)
    return m_baseTexture->Release();
  // Standalone / implicit surface: D3D9 clamps Release-at-0. These are handed
  // out at public refcount 0 and kept alive by a private ref, and an app may
  // Release a surface that lives with the device; guard the underflow before
  // the decrement so a redundant Release-at-0 neither wraps the counter nor
  // re-runs the device unpin.
  if (m_refCount.load() == 0)
    return 0;
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    // Losable counter: decrement on pub->0 BEFORE the device unpin. Reset
    // checks the counter before unbinding, so it must track app public-ref
    // presence, not full destruct (per D3D9 spec).
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed(m_losableBytes);
    }
    // Implicit surface: mirror the pub-ref edge. The flag persists (the app can
    // re-acquire the same object through GetBackBuffer /
    // GetDepthStencilSurface), so only the count toggles here, not the flag.
    if (m_isImplicitLosable)
      m_device->onLosableResourceDestroyed();
    // device->Release() can synchronously destruct the device. A standalone
    // self-pinned surface survives that teardown via its self-pin (dropped just
    // below); the implicit backbuffer (no self-pin) is instead deleted inside
    // this call via its private drop, so nothing afterwards reads `this`, only
    // the captured device and the local ref. Capture the device first since the
    // call may free `this`.
    MTLD3D9Device *device = m_device;
    bool dropSelfPin = m_self_pinned;
    m_self_pinned = false;
    device->Release();
    if (dropSelfPin)
      ReleasePrivate();
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DSurface9)) {
    *ppvObject = static_cast<IDirect3DSurface9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::FreePrivateData(REFGUID refguid) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9FreePrivateData(m_privateData, refguid);
}

DWORD STDMETHODCALLTYPE
MTLD3D9Surface::SetPriority(DWORD) {
  D9DeviceLock lock = m_device->LockDevice();
  // d3d9_surface_SetPriority ignores priority unconditionally: a surface is
  // either a texture sub-resource (priority lives on the container) or a
  // standalone pool that cannot be MANAGED, so it always reports 0.
  return 0;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Surface::GetPriority() {
  D9DeviceLock lock = m_device->LockDevice();
  return 0;
}

void STDMETHODCALLTYPE
MTLD3D9Surface::PreLoad() {
  D9DeviceLock lock = m_device->LockDevice();
  // Hint to upload MANAGED contents to VRAM ahead of the next draw.
  // Apple Silicon's unified memory makes this a no-op; the texture
  // backing already resides in the GPU-accessible heap.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9Surface::GetType() {
  D9DeviceLock lock = m_device->LockDevice();
  return D3DRTYPE_SURFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetContainer(REFIID riid, void **ppContainer) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppContainer)
    return D3DERR_INVALIDCALL;
  *ppContainer = nullptr;
  if (!m_container)
    return E_NOINTERFACE;
  return m_container->QueryInterface(riid, ppContainer);
}

void
MTLD3D9Surface::detachContainer() {
  m_container = static_cast<IUnknown *>(static_cast<IDirect3DDevice9Ex *>(m_device));
}

void
MTLD3D9Surface::flagContainerAutoGenDirty() {
  if (!m_container)
    return;
  IDirect3DBaseTexture9 *base = nullptr;
  if (FAILED(m_container->QueryInterface(__uuidof(IDirect3DBaseTexture9), reinterpret_cast<void **>(&base))))
    return;
  bool marked = false;
  switch (base->GetType()) {
  case D3DRTYPE_TEXTURE:
    marked = static_cast<MTLD3D9Texture *>(static_cast<IDirect3DTexture9 *>(base))->flagAutoGenDirty();
    break;
  case D3DRTYPE_CUBETEXTURE:
    marked = static_cast<MTLD3D9CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(base))->flagAutoGenDirty();
    break;
  default:
    break;
  }
  base->Release();
  // Move the device's pre-draw sweep epoch so the next draw regenerates this
  // texture's mips before sampling them (the mip-gen op lands ahead of the
  // draws in the op stream).
  if (marked)
    m_device->markAutogenMipsDirty();
}

void
MTLD3D9Surface::flagContainerDirtyRegion(const RECT *rect) {
  if (!m_container)
    return;
  IDirect3DBaseTexture9 *base = nullptr;
  if (FAILED(m_container->QueryInterface(__uuidof(IDirect3DBaseTexture9), reinterpret_cast<void **>(&base))))
    return;
  switch (base->GetType()) {
  case D3DRTYPE_TEXTURE:
    static_cast<MTLD3D9Texture *>(static_cast<IDirect3DTexture9 *>(base))->unionDirtyRect(rect);
    break;
  case D3DRTYPE_CUBETEXTURE:
    // m_array_slice is the cube face for a face-level surface; the cube tracks
    // one dirty region per face.
    static_cast<MTLD3D9CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(base))->unionDirtyRect(m_array_slice, rect);
    break;
  default:
    break;
  }
  base->Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDesc(D3DSURFACE_DESC *pDesc) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  *pDesc = m_desc;
  return D3D_OK;
}

// LockRect: serves the buffer-backed path, the MANAGED
// sysmem-mirror path, DEFAULT-pool off-screen plain surfaces (host mirror
// from CreateOffscreenPlainSurface), and DYNAMIC DEFAULT-pool textures
// (lazy sysmem mirror via m_lazyMirrorParent->ensureMirror; see
// d3d9_texture.cpp). MSDN: off-screen plain and DYNAMIC resources are
// always lockable. A non-DYNAMIC DEFAULT texture surface carries no
// cpu_ptr and correctly falls out at the null check below.
HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  D9StallLockTimer _lock_timer;
  // T0.3 latency histogram: per-LockRect caller-thread cost. The
  // counter above gives "how many" (sum), the histogram captures
  // distribution. Lazy-mirror first-touch pages (incl. DYNAMIC DEFAULT
  // textures' first lock) are the variable-cost path to watch.
  if (!pLockedRect)
    return D3DERR_INVALIDCALL;
  // The caller's struct is written only on success: every INVALIDCALL return
  // below leaves it untouched, so an app that pre-seeds pBits/Pitch (and locks
  // an already-locked surface) reads back its own values. wined3d and DXVK
  // both write Pitch/pBits on the success path only.
  // Lazy-mirror parent owned: alloc the mirror buffer + patch this
  // (and all sibling level) surfaces' cpu_ptr/pitch
  // before the null check below. Idempotent; every Lock after the
  // first early-outs inside ensureMirror.
  if (!m_cpu_ptr && m_lazyMirrorParent)
    m_lazyMirrorParent->ensureMirror();
  if (!m_cpu_ptr)
    return D3DERR_INVALIDCALL;
  if (m_locked)
    return D3DERR_INVALIDCALL;
  // A GDI DC open on any sibling surface of this texture blocks LockRect
  // texture-wide (wined3d: GetDC takes a map, and the map_count gate rejects).
  // GetDC's own internal lock runs before it sets dc_open, so it is not blocked.
  if (m_lock_state->dc_open)
    return D3DERR_INVALIDCALL;
  // DXVK d3d9_device.cpp+ LockImage validation gates.
  // Pure flag normalization: no behavioural change on the sysmem-mirror
  // path (MANAGED + DYNAMIC DEFAULT). DISCARD/NOOVERWRITE are no-ops there
  // by spec; per-frame DISCARD re-lock safety comes from UnlockRect's
  // upload-ring snapshot, not from honouring the flag here.
  // The contradictory DISCARD+READONLY combo is rejected only on DEFAULT pool;
  // the CPU pools drop DISCARD anyway, so the pair is harmless there (DXVK
  // d3d9_device.cpp).
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY) &&
      m_desc.Pool == D3DPOOL_DEFAULT)
    return D3DERR_INVALIDCALL;
  // DXVK: "Games like Beyond Good and Evil break if [DONOTWAIT] doesn't
  // succeed." We don't honour it either; strip it so downstream code
  // can't surface it.
  Flags &= ~D3DLOCK_DONOTWAIT;
  // DISCARD + NOOVERWRITE: NOOVERWRITE wins.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  // DOC-DIV (cosmetic): DXVK also strips DISCARD when the device is lost
  // (d3d9_device.cpp LockImage); the buffer path does the same
  // (d3d9_buffer.cpp). Images do not, so a DISCARD lock of a lockable DEFAULT
  // RT / plain surface on a lost device skips the mirror readback below where
  // DXVK would refresh it. The contents are undefined during device loss either
  // way, so the only difference is which junk the app reads. Left as-is.
  // Partial-rect LockRects derive the row-byte and column-byte skip from the
  // format's row-pitch helpers instead of a flat bpp, so both DXT (addressed in
  // 4x4 blocks) and uncompressed share the same offset math. The left/top
  // reaching that math are block-floored: the validation below either accepted a
  // block-aligned rect, or (CPU-pool forgiveness) floored the requested corner
  // to the block grid while nulling pRect so the bookkeeping stays whole-surface.
  // The 'NULL' FOURCC placeholder has no real layout; CreateRenderTarget backs
  // a lockable one with a 4-bpp (BGRA8 dummy) mirror, so report the same pitch.
  const uint32_t row_pitch =
      IsNullFormat(m_desc.Format) ? (m_desc.Width * 4u) : D3DFormatRowPitch(m_desc.Format, m_desc.Width);
  if (row_pitch == 0 || m_pitch == 0)
    return D3DERR_INVALIDCALL;

  LONG x0 = 0;
  LONG y0 = 0;
  if (pRect) {
    // 3Dc joins the block rules HERE only: rect validation follows the real
    // 4x4 blocks (wined3d flags the formats compressed for this check) while
    // the addressing below keeps their linear one-byte fiction.
    if (IsCompressedFormat(m_desc.Format) || Is3DcFormat(m_desc.Format)) {
      // Compressed / 3Dc surfaces are addressed in 4x4 blocks: the box must be
      // in bounds, non-degenerate and block-aligned. image_box_dimensions_valid
      // ports wined3d's check_box_dimensions; 3Dc joins the block rules here even
      // though its byte offset below stays a linear one-byte fiction. The UINT
      // casts mirror wined3d_box_set: a negative edge wraps large and trips the
      // bounds test.
      const bool box_ok = image_box_dimensions_valid(
          static_cast<uint32_t>(pRect->left), static_cast<uint32_t>(pRect->top), static_cast<uint32_t>(pRect->right),
          static_cast<uint32_t>(pRect->bottom), 0, 1, m_desc.Width, m_desc.Height, 1, 4, 4
      );
      if (!box_ok) {
        // A GPU-only (DEFAULT) DXTn surface rejects the invalid box. A CPU-pool
        // one is forgiven: wined3d (resource_offset_map_pointer) and DXVK
        // (CalcImageLockOffset) do not collapse the pointer to the level origin,
        // they floor the requested corner to the block grid and hand pBits back
        // there, then upload the whole level. Reproduce that returned pointer:
        // floor left/top to the 4x4 block, clamped into the level so a wild rect
        // cannot walk pBits off the mirror, and null pRect so the bookkeeping and
        // Unlock upload below stay whole-surface, block-aligned and in bounds.
        if (m_desc.Pool == D3DPOOL_DEFAULT)
          return D3DERR_INVALIDCALL;
        x0 = std::max<LONG>(0, std::min<LONG>(pRect->left & ~3, static_cast<LONG>(m_desc.Width)));
        y0 = std::max<LONG>(0, std::min<LONG>(pRect->top & ~3, static_cast<LONG>(m_desc.Height)));
        pRect = nullptr;
      } else {
        x0 = pRect->left;
        y0 = pRect->top;
      }
    } else {
      // Uncompressed CPU-resident surfaces accept any rect, including out-of-
      // bounds, inverted, or negative ones: D3D9 hands back a pointer whose byte
      // offset wraps in unsigned arithmetic. wined3d's map path does no box
      // validation; DXVK block-checks compressed formats only. The Unlock upload
      // region is clamped to the surface extent so the GPU blit stays in bounds.
      x0 = pRect->left;
      y0 = pRect->top;
    }
  }
  // DXVK d3d9_device.cpp: DISCARD is only meaningful on full-
  // extent DEFAULT-pool locks; drop it on partial-rect or non-DEFAULT.
  // DXVK comment: "DISCARD is not ignored for non-DYNAMIC unlike what
  // the docs say": non-DYNAMIC DEFAULT keeps DISCARD; MANAGED /
  // SYSTEMMEM / SCRATCH strip it.
  const bool full_resource =
      !pRect || (pRect->left == 0 && pRect->top == 0 && static_cast<UINT>(pRect->right) >= m_desc.Width &&
                 static_cast<UINT>(pRect->bottom) >= m_desc.Height);
  if (!full_resource || m_desc.Pool != D3DPOOL_DEFAULT)
    Flags &= ~D3DLOCK_DISCARD;
  // Re-materialize an evicted MANAGED mirror level (freed after upload to
  // reclaim 32-bit address space) before the pointer goes out: ensureMirror
  // above re-allocated a blank backing, so the bytes are re-downloaded from the
  // Metal texture (the wined3d sysmem-reload shape). MANAGED always reloads:
  // DISCARD was stripped above (it is DEFAULT-only), so there is no
  // whole-level-overwrite hint that would let the download be skipped.
  if (m_lazyMirrorParent && m_desc.Pool == D3DPOOL_MANAGED)
    m_lazyMirrorParent->materializeLevelForLock(m_lazy_subresource);
  // GPU-authoritative mirror surfaces: the contents live in the Metal
  // texture (a render pass, ColorFill, StretchRect or UpdateSurface is the
  // writer), so the host mirror is refreshed before the pointer goes out
  // unless the caller discards. This covers lockable render targets AND
  // DEFAULT-pool offscreen-plain surfaces (Usage 0): both are DEFAULT-pool
  // and buffer-less (the texture is the truth, not an aliased backing
  // buffer). DYNAMIC textures stream CPU->GPU and keep the mirror
  // authoritative, so they skip the download. Tested after the DISCARD
  // normalization above so a partial-rect DISCARD still reads back, matching
  // DXVK's lock order; wined3d maps the same sysmem download.
  // A 'NULL' placeholder keeps a 1x1 dummy Metal texture behind a desc-sized
  // mirror; its bytes are never GPU-meaningful (the slot is write-skipped), and
  // a readback would copy the desc extent out of the 1x1 texture's bounds.
  if (m_buffer == nullptr && m_texture != nullptr && m_desc.Pool == D3DPOOL_DEFAULT &&
      !(m_desc.Usage & D3DUSAGE_DYNAMIC) && !(Flags & D3DLOCK_DISCARD) && !IsNullFormat(m_desc.Format))
    m_device->readbackSurfaceMirror(this);

  // Byte offset of the (block-floored) locked corner into the mirror, the
  // wined3d resource_offset_map_pointer math shared with UnlockRect. For
  // uncompressed this is y*pitch + x*bpp; for DXT the divide floors x/y to the
  // 4x4 block. 3Dc keeps its linear one-byte fiction (compressed=false).
  const uint32_t lock_offset = image_lock_block_offset(
      static_cast<uint32_t>(x0), static_cast<uint32_t>(y0), m_pitch, row_pitch, m_desc.Width,
      IsCompressedFormat(m_desc.Format)
  );
  pLockedRect->Pitch = static_cast<INT>(m_pitch);
  pLockedRect->pBits = static_cast<uint8_t *>(m_cpu_ptr) + lock_offset;
  m_locked = true;
  m_ever_locked = true;
  ++m_lock_state->map_count;
  // Remember the locked rect + READONLY hint so UnlockRect can do a
  // partial-extent replaceRegion (or skip it entirely on READONLY).
  // wined3d texture.c d3d9_surface_unmap pushes only the dirty rect.
  m_locked_readonly = (Flags & D3DLOCK_READONLY) != 0;
  m_locked_no_dirty_update = (Flags & D3DLOCK_NO_DIRTY_UPDATE) != 0;
  if (pRect) {
    // The returned pBits above carries the raw (possibly out-of-bounds) rect
    // offset, but the dirty/upload rect must stay within the surface extent so
    // the Unlock GPU blit never reads or writes past the texture. wined3d
    // intersects the dirty region with the level extent the same way. An
    // inverted or fully out-of-range rect collapses to a zero-area region,
    // which UnlockRect skips.
    const LONG cw = static_cast<LONG>(m_desc.Width);
    const LONG ch = static_cast<LONG>(m_desc.Height);
    const LONG lx = std::max<LONG>(0, std::min<LONG>(pRect->left, cw));
    const LONG ly = std::max<LONG>(0, std::min<LONG>(pRect->top, ch));
    const LONG rx = std::max<LONG>(0, std::min<LONG>(pRect->right, cw));
    const LONG ry = std::max<LONG>(0, std::min<LONG>(pRect->bottom, ch));
    m_locked_x = static_cast<uint32_t>(lx);
    m_locked_y = static_cast<uint32_t>(ly);
    m_locked_w = static_cast<uint32_t>(rx > lx ? rx - lx : 0);
    m_locked_h = static_cast<uint32_t>(ry > ly ? ry - ly : 0);
  } else {
    m_locked_x = 0;
    m_locked_y = 0;
    m_locked_w = m_desc.Width;
    m_locked_h = m_desc.Height;
  }
  // D3DLOCK_NOSYSLOCK / D3DLOCK_NO_DIRTY_UPDATE: no-op on the sysmem-mirror
  // path (NOSYSLOCK is a Win32-only critical-section hint; NO_DIRTY_UPDATE
  // matters once AddDirtyRect tracking lands). DISCARD / NOOVERWRITE: the
  // upload-on-Unlock + upload-ring snapshot already match the spec's
  // effective meaning for both MANAGED and DYNAMIC DEFAULT: a per-frame
  // DISCARD re-lock is safe because the prior contents were snapshotted
  // into the ring at Unlock.
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::UnlockRect() {
  D9DeviceLock lock = m_device->LockDevice();
  // Unlock of a surface that is not currently mapped. DXVK and wined3d forgive
  // it for a D3DRTYPE_TEXTURE container (m_is_texture_mip) and reject it with
  // INVALIDCALL otherwise. Native D3D9 draws a finer line the references do not:
  // it forgives only a *redundant* unlock (the level was mapped at least once),
  // and still returns INVALIDCALL for a texture mip that was never mapped (e.g.
  // an Unlock after a failed Lock). m_ever_locked latches the first successful
  // Lock so both cases match native. The open-GDI-DC arm is the ReleaseDC path:
  // its internal Unlock releases a still-open DC and must not report failure.
  // Cube faces soften their own entry point in MTLD3D9CubeTexture::UnlockRect;
  // standalone surfaces and swapchain backbuffers stay strict.
  if (!m_locked)
    return (m_is_texture_mip && m_ever_locked) || m_lock_state->dc_open ? D3D_OK : D3DERR_INVALIDCALL;
  m_locked = false;
  if (m_lock_state->map_count > 0)
    --m_lock_state->map_count;
  // Record the parent texture's level-0 dirty region + AUTOGENMIPMAP flag here
  // so the GetSurfaceLevel / GetDC entry point (a direct surface unlock, not
  // routed through the texture wrapper) tracks them too; wined3d records both
  // via the shared texture regardless of entry point (surface.c UnlockRect /
  // ReleaseDC). Only the top-most level records dirt (wined3d texture.c);
  // READONLY / NO_DIRTY_UPDATE suppress the implicit record. Captured before the
  // reset below and before the upload block's 3Dc full-level rewrite.
  const bool record_dirty = m_mip_level == 0 && !m_locked_readonly && !m_locked_no_dirty_update;
  const RECT dirty_rect = lockedRect();
  // Buffer-backed surfaces (SYSTEMMEM/SCRATCH offscreen-plain): GPU-
  // visible, no upload step. Mirror-backed surfaces (MANAGED, DEFAULT
  // plain, DYNAMIC textures, lockable RTs): explicit upload to the
  // Metal texture via m_uploadRing.
  // 'NULL' placeholder: no upload (see LockRect; the 1x1 dummy texture can't
  // take a desc-sized region and the bytes are never read by the GPU).
  // A NO_DIRTY_UPDATE lock on a MANAGED 2D texture defers: it records no dirty
  // region, so the bytes stay in the mirror until a later AddDirtyRect /
  // EvictManagedResources / plain Unlock (consumed by the pre-draw managed
  // sweep), matching wined3d + DXVK. DEFAULT / DYNAMIC ignore NO_DIRTY_UPDATE
  // (they always upload), and the cube host has no deferred path, so both keep
  // the eager upload below (deferManagedNoDirtyUpload() gates on the 2D host).
  const bool defer_no_dirty = m_locked_no_dirty_update && m_desc.Pool == D3DPOOL_MANAGED && m_lazyMirrorParent &&
                              m_lazyMirrorParent->deferManagedNoDirtyUpload();
  if (m_buffer == nullptr && m_cpu_ptr != nullptr && m_texture != nullptr &&
      (m_desc.Pool == D3DPOOL_MANAGED || m_desc.Pool == D3DPOOL_DEFAULT) && !m_locked_readonly && !defer_no_dirty &&
      !IsNullFormat(m_desc.Format) && m_locked_w > 0 && m_locked_h > 0) {
    // 3Dc: the fiction's partial rects have no block mapping; push the
    // whole level (stageTextureUpload converts the layout, the mirror
    // start is the block stream).
    if (Is3DcFormat(m_desc.Format)) {
      m_locked_x = 0;
      m_locked_y = 0;
      m_locked_w = m_desc.Width;
      m_locked_h = m_desc.Height;
    }
    // Push only the dirty rect to GPU. wined3d does the same; copying
    // the full level extent on every Unlock burns wine syscall RTT for
    // games that lock 100s of textures during loading. The src pointer
    // must point at the (x, y) corner inside the mirror, not at the
    // surface origin; the same resource_offset_map_pointer math LockRect
    // used (image_lock_block_offset), block-floored for DXTn.
    const bool compressed = IsCompressedFormat(m_desc.Format);
    const uint32_t row_pitch = D3DFormatRowPitch(m_desc.Format, m_desc.Width);
    const uint32_t src_offset =
        image_lock_block_offset(m_locked_x, m_locked_y, m_pitch, row_pitch, m_desc.Width, compressed);
    WMTOrigin origin{};
    origin.x = m_locked_x;
    origin.y = m_locked_y;
    origin.z = 0;
    WMTSize size{};
    size.width = m_locked_w;
    size.height = m_locked_h;
    size.depth = 1;
    // Always memcpy through m_uploadRing to snapshot bytes at Unlock time.
    // Direct blit would race: GPU reads mirror bytes at execution time,
    // but next Lock could overwrite them (Apple Silicon UMA). Per-surface
    // rename ring on mirror would recover perf; follow-on work.
    const void *src = static_cast<const uint8_t *>(m_cpu_ptr) + src_offset;
    m_device->stageTextureUpload(m_texture, m_dxmtTexture, m_mip_level, m_array_slice, origin, size, src, m_pitch, compressed);
    // The bytes are now snapshotted into the upload ring; the mirror is no
    // longer referenced. A MANAGED parent reclaims it once every level has
    // been uploaded (the level surfaces' cpu_ptr is nulled, so a later Lock
    // re-arms ensureMirror). Must run after the last m_cpu_ptr use above.
    if (m_lazyMirrorParent)
      m_lazyMirrorParent->noteLevelUploaded(m_lazy_subresource);
  }
  // Feed the parent texture's dirty-region + auto-gen tracking (see the capture
  // above). Runs for every pool, including a SYSTEMMEM source whose dirty region
  // UpdateTexture consumes; flagContainerAutoGenDirty is a no-op unless the
  // container opted into AUTOGENMIPMAP. Both no-op on a standalone surface whose
  // container is the device or swapchain rather than a texture.
  if (record_dirty)
    flagContainerDirtyRegion(&dirty_rect);
  if (m_mip_level == 0)
    flagContainerAutoGenDirty();
  // Reset locked-rect bookkeeping so a subsequent LockRect with a
  // wider area doesn't accidentally inherit stale narrow bounds.
  m_locked_readonly = false;
  m_locked_no_dirty_update = false;
  m_locked_x = m_locked_y = m_locked_w = m_locked_h = 0;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDC(HDC *phdc) {
  D9DeviceLock lock = m_device->LockDevice();
#ifdef _WIN32
  // GDI text composition: some apps rasterize UI text into a sampled texture by
  // GetDC'ing the surface, drawing glyphs with GDI, then ReleaseDC. wined3d
  // (wined3d_texture_get_dc) and DXVK both back the DC with a bitmap created
  // directly over the surface's locked CPU bytes via D3DKMTCreateDCFromMemory,
  // so GDI paints into the exact memory LockRect exposes and UnlockRect uploads
  // to the Metal texture. Every coherence concern (download-on-lock,
  // upload-on-unlock, mirror residency) is delegated to the lock path instead
  // of a second hand-managed copy. The lock pitch of every mirror-backed
  // surface GetDC can target (texture mips, offscreen plains, lockable RTs)
  // is the 4-byte-aligned D3DFormatLockPitch, which satisfies GDI's
  // DWORD-aligned stride requirement.
  // Leave *phdc untouched on every failure path: D3D9 only writes it on success
  // (a failed GetDC must not clobber the caller's HDC). Matches DXVK.
  if (!phdc)
    return D3DERR_INVALIDCALL;
  if (m_gdi_dc != nullptr) // a DC is already open on this surface
    return D3DERR_INVALIDCALL;
  if (!IsGetDCCompatibleFormat(m_desc.Format))
    return D3DERR_INVALIDCALL;
  // wined3d: GetDC requires the whole texture to be unmapped (resource.map_count
  // == 0). A DC is exclusive per texture and cannot be taken while any sibling
  // surface is locked or already holds a DC.
  if (m_lock_state->map_count > 0)
    return D3DERR_INVALIDCALL;

  D3DLOCKED_RECT locked{};
  HRESULT hr = LockRect(&locked, nullptr, 0);
  if (FAILED(hr))
    return hr;

  D3DKMT_CREATEDCFROMMEMORY create{};
  create.pMemory = locked.pBits;
  create.Format = m_desc.Format;
  create.Width = m_desc.Width;
  create.Height = m_desc.Height;
  create.Pitch = static_cast<UINT>(locked.Pitch);
  create.hDeviceDc = ::CreateCompatibleDC(nullptr);
  create.pColorTable = nullptr;
  // The output bitmap/DC own the memory mapping; the device DC is only needed
  // for the call and is freed regardless of outcome (DXVK does the same).
  LONG status = d3dkmtCreateDCFromMemory(&create);
  if (create.hDeviceDc)
    ::DeleteDC(create.hDeviceDc);
  if (status != 0 || create.hDc == nullptr) {
    UnlockRect();
    return D3DERR_INVALIDCALL;
  }
  m_gdi_dc = create.hDc;
  m_gdi_bitmap = create.hBitmap;
  m_lock_state->dc_open = true;
  *phdc = m_gdi_dc;
  return D3D_OK;
#else
  (void)phdc;
  return D3DERR_INVALIDCALL;
#endif
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::ReleaseDC(HDC hdc) {
  D9DeviceLock lock = m_device->LockDevice();
#ifdef _WIN32
  if (m_gdi_dc == nullptr || hdc != m_gdi_dc)
    return D3DERR_INVALIDCALL;
  D3DKMT_DESTROYDCFROMMEMORY destroy{};
  destroy.hDc = m_gdi_dc;
  destroy.hBitmap = m_gdi_bitmap;
  d3dkmtDestroyDCFromMemory(&destroy);
  m_gdi_dc = nullptr;
  m_gdi_bitmap = nullptr;
  // UnlockRect uploads the GDI-modified bytes to the Metal texture (the text
  // quad samples them) through the same path a write-Lock uses, and releases
  // this surface's hold on the shared map_count. Clear dc_open only after it
  // returns: if the app already unlocked the surface itself, the internal Unlock
  // takes the not-mapped path, whose D3D_OK arm is the still-open DC.
  HRESULT hr = UnlockRect();
  m_lock_state->dc_open = false;
  return hr;
#else
  (void)hdc;
  return D3DERR_INVALIDCALL;
#endif
}

} // namespace dxmt
