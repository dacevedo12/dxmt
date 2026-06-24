#include "d3d9_surface.hpp"

#include "d3d9_cube_texture.hpp"
#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_private_data.hpp"
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
  if (ref == 1)
    m_device->AddRef();
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
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::FreePrivateData(REFGUID refguid) {
  return D3D9FreePrivateData(m_privateData, refguid);
}

DWORD STDMETHODCALLTYPE
MTLD3D9Surface::SetPriority(DWORD) {
  // d3d9_surface_SetPriority ignores priority unconditionally: a surface is
  // either a texture sub-resource (priority lives on the container) or a
  // standalone pool that cannot be MANAGED, so it always reports 0.
  return 0;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Surface::GetPriority() {
  return 0;
}

void STDMETHODCALLTYPE
MTLD3D9Surface::PreLoad() {
  // Hint to upload MANAGED contents to VRAM ahead of the next draw.
  // Apple Silicon's unified memory makes this a no-op; the texture
  // backing already resides in the GPU-accessible heap.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9Surface::GetType() {
  return D3DRTYPE_SURFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetContainer(REFIID riid, void **ppContainer) {
  if (!ppContainer)
    return D3DERR_INVALIDCALL;
  *ppContainer = nullptr;
  if (!m_container)
    return E_NOINTERFACE;
  return m_container->QueryInterface(riid, ppContainer);
}

void
MTLD3D9Surface::flagContainerAutoGenDirty() {
  if (!m_container)
    return;
  IDirect3DBaseTexture9 *base = nullptr;
  if (FAILED(m_container->QueryInterface(__uuidof(IDirect3DBaseTexture9), reinterpret_cast<void **>(&base))))
    return;
  switch (base->GetType()) {
  case D3DRTYPE_TEXTURE:
    static_cast<MTLD3D9Texture *>(static_cast<IDirect3DTexture9 *>(base))->flagAutoGenDirty();
    break;
  case D3DRTYPE_CUBETEXTURE:
    static_cast<MTLD3D9CubeTexture *>(static_cast<IDirect3DCubeTexture9 *>(base))->flagAutoGenDirty();
    break;
  default:
    break;
  }
  base->Release();
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDesc(D3DSURFACE_DESC *pDesc) {
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
  // (and all sibling level) surfaces' cpu_ptr/mirror_src_buffer/pitch
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
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY))
    return D3DERR_INVALIDCALL;
  // DXVK: "Games like Beyond Good and Evil break if [DONOTWAIT] doesn't
  // succeed." We don't honour it either; strip it so downstream code
  // can't surface it.
  Flags &= ~D3DLOCK_DONOTWAIT;
  // DISCARD + NOOVERWRITE: NOOVERWRITE wins.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  // Partial-rect LockRects derive the row-byte and column-byte skip from the
  // format's row-pitch helpers instead of a flat bpp, so both DXT (addressed in
  // 4x4 blocks) and uncompressed share the same offset math. The rect's
  // left/top reaching that math are block-aligned: the validation below either
  // accepted a block-aligned rect or nulled pRect (whole-surface lock).
  // The 'NULL' FOURCC placeholder has no real layout; CreateRenderTarget backs
  // a lockable one with a 4-bpp (BGRA8 dummy) mirror, so report the same pitch.
  const uint32_t row_pitch =
      IsNullFormat(m_desc.Format) ? (m_desc.Width * 4u) : D3DFormatRowPitch(m_desc.Format, m_desc.Width);
  if (row_pitch == 0 || m_pitch == 0)
    return D3DERR_INVALIDCALL;

  LONG x0 = 0;
  LONG y0 = 0;
  if (pRect) {
    if (IsCompressedFormat(m_desc.Format)) {
      // Compressed surfaces are addressed in 4x4 blocks. The box must be in
      // bounds, non-degenerate, and block-aligned (left/top block multiples,
      // right/bottom block multiples or the surface extent); the byte-offset
      // math below floors x0/y0 to the block, so an out-of-bounds or unaligned
      // rect would otherwise point past the mirror or truncate to the previous
      // block. The UINT casts mirror wined3d_box_set: a negative edge wraps
      // large and trips the bounds test, matching wined3d's check_box_dimensions.
      const UINT bl = static_cast<UINT>(pRect->left), bt = static_cast<UINT>(pRect->top);
      const UINT br = static_cast<UINT>(pRect->right), bb = static_cast<UINT>(pRect->bottom);
      const bool box_ok =
          !(bl >= br || br > m_desc.Width || bt >= bb || bb > m_desc.Height) &&
          !((bl & 3) || (bt & 3) || ((br & 3) && br != m_desc.Width) || ((bb & 3) && bb != m_desc.Height));
      if (!box_ok) {
        // A GPU-only (DEFAULT) DXTn surface rejects the invalid box; a CPU-pool
        // one degrades to a whole-surface lock so the following Unlock pushes a
        // block-aligned, in-bounds region rather than a wild sub-block. wined3d
        // forgives the box only for CPU-resident textures (device.c map: a
        // BLOCKS format with no CPU access fails, otherwise it proceeds).
        if (m_desc.Pool == D3DPOOL_DEFAULT)
          return D3DERR_INVALIDCALL;
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
    m_lazyMirrorParent->materializeLevelForLock(m_mip_level);
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

  // Row-byte offset: y_in_blocks * pitch. For uncompressed, blocks
  // are 1×1 so this is just y * pitch. For DXT, y must be /4 first.
  const uint32_t block_w = IsCompressedFormat(m_desc.Format) ? 4u : 1u;
  const uint32_t block_h = IsCompressedFormat(m_desc.Format) ? 4u : 1u;
  const uint32_t row_offset = (static_cast<uint32_t>(y0) / block_h) * m_pitch;
  // Column-byte offset: same idea: bytes-per-block-column for
  // compressed, bytes-per-pixel for uncompressed. Derive from the
  // row pitch and texel-per-row count to avoid a parallel switch.
  const uint32_t cols_per_row = IsCompressedFormat(m_desc.Format) ? (m_desc.Width + 3u) / 4u : m_desc.Width;
  const uint32_t col_bytes = cols_per_row > 0 ? row_pitch / cols_per_row : 0u;
  const uint32_t col_offset = (static_cast<uint32_t>(x0) / block_w) * col_bytes;

  pLockedRect->Pitch = static_cast<INT>(m_pitch);
  pLockedRect->pBits = static_cast<uint8_t *>(m_cpu_ptr) + row_offset + col_offset;
  m_locked = true;
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
  // wined3d surface.c softens the double-Unlock gate when the
  // container is a 2D texture; apps that drive a Lock/Unlock loop
  // across mip levels but forget to track whether a given level was
  // actually locked still see D3D_OK from the redundant Unlock. The
  // relaxation is *only* for D3DRTYPE_TEXTURE; standalone surfaces,
  // cube faces, and swapchain backbuffers stay strict.
  if (!m_locked)
    // wined3d returns D3D_OK for an unlock of an unmapped surface while a GDI DC
    // is open on the texture (WINED3D_TEXTURE_DC_IN_USE) or for a 2D-texture mip
    // double-unlock; otherwise it is INVALIDCALL.
    return (m_is_texture_mip || m_lock_state->dc_open) ? D3D_OK : D3DERR_INVALIDCALL;
  m_locked = false;
  if (m_lock_state->map_count > 0)
    --m_lock_state->map_count;
  // Buffer-backed surfaces (SYSTEMMEM/SCRATCH offscreen-plain): GPU-
  // visible, no upload step. Mirror-backed surfaces (MANAGED, DEFAULT
  // plain, DYNAMIC textures, lockable RTs): explicit upload to the
  // Metal texture via m_uploadRing.
  // 'NULL' placeholder: no upload (see LockRect; the 1x1 dummy texture can't
  // take a desc-sized region and the bytes are never read by the GPU).
  if (m_buffer == nullptr && m_cpu_ptr != nullptr && m_texture != nullptr &&
      (m_desc.Pool == D3DPOOL_MANAGED || m_desc.Pool == D3DPOOL_DEFAULT) && !m_locked_readonly &&
      !IsNullFormat(m_desc.Format) && m_locked_w > 0 && m_locked_h > 0) {
    // Push only the dirty rect to GPU. wined3d does the same; copying
    // the full level extent on every Unlock burns wine syscall RTT for
    // games that lock 100s of textures during loading. The src pointer
    // must point at the (x0, y0) corner inside the mirror, not at the
    // surface origin; same offset math LockRect used. Compressed
    // formats are addressed by 4×4 blocks; row/col offsets divide
    // through the block size to stay aligned (D3D9 contract).
    const bool compressed = IsCompressedFormat(m_desc.Format);
    const uint32_t row_pitch = D3DFormatRowPitch(m_desc.Format, m_desc.Width);
    const uint32_t block_w = compressed ? 4u : 1u;
    const uint32_t block_h = compressed ? 4u : 1u;
    const uint32_t cols_per_row = compressed ? (m_desc.Width + 3u) / 4u : m_desc.Width;
    const uint32_t col_bytes = cols_per_row > 0 ? row_pitch / cols_per_row : 0u;
    const uint32_t src_row_off = (m_locked_y / block_h) * m_pitch;
    const uint32_t src_col_off = (m_locked_x / block_w) * col_bytes;
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
    (void)m_mirror_src_buffer;
    (void)m_mirror_level_offset;
    const void *src = static_cast<const uint8_t *>(m_cpu_ptr) + src_row_off + src_col_off;
    m_device->stageTextureUpload(m_texture, m_dxmtTexture, m_mip_level, m_array_slice, origin, size, src, m_pitch, compressed);
    // The bytes are now snapshotted into the upload ring; the mirror is no
    // longer referenced. A MANAGED parent reclaims it once every level has
    // been uploaded (the level surfaces' cpu_ptr is nulled, so a later Lock
    // re-arms ensureMirror). Must run after the last m_cpu_ptr use above.
    if (m_lazyMirrorParent)
      m_lazyMirrorParent->noteLevelUploaded(m_mip_level);
  }
  // Reset locked-rect bookkeeping so a subsequent LockRect with a
  // wider area doesn't accidentally inherit stale narrow bounds.
  m_locked_readonly = false;
  m_locked_no_dirty_update = false;
  m_locked_x = m_locked_y = m_locked_w = m_locked_h = 0;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDC(HDC *phdc) {
#ifdef _WIN32
  // GDI text composition: some apps rasterize UI text into a sampled texture by
  // GetDC'ing the surface, drawing glyphs with GDI, then ReleaseDC. wined3d
  // (wined3d_texture_get_dc) and DXVK both back the DC with a bitmap created
  // directly over the surface's locked CPU bytes via D3DKMTCreateDCFromMemory,
  // so GDI paints into the exact memory LockRect exposes and UnlockRect uploads
  // to the Metal texture. Every coherence concern (download-on-lock,
  // upload-on-unlock, mirror residency) is delegated to the lock path instead
  // of a second hand-managed copy. The padded lock pitch satisfies GDI's
  // DWORD-aligned stride because the surface pitch is always 4-byte aligned.
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
#ifdef _WIN32
  if (m_gdi_dc == nullptr || hdc != m_gdi_dc)
    return D3DERR_INVALIDCALL;
  D3DKMT_DESTROYDCFROMMEMORY destroy{};
  destroy.hDc = m_gdi_dc;
  destroy.hBitmap = m_gdi_bitmap;
  d3dkmtDestroyDCFromMemory(&destroy);
  m_gdi_dc = nullptr;
  m_gdi_bitmap = nullptr;
  m_lock_state->dc_open = false;
  // UnlockRect uploads the GDI-modified bytes to the Metal texture (the text
  // quad samples them) through the same path a write-Lock uses, and releases
  // this surface's hold on the shared map_count.
  return UnlockRect();
#else
  (void)hdc;
  return D3DERR_INVALIDCALL;
#endif
}

} // namespace dxmt
