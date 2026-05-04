#include "d3d9_surface.hpp"

#include "d3d9_device.hpp"
#include "d3d9_format.hpp"
#include "d3d9_tex_debug.hpp"
#include "d3d9_texture.hpp"
#include "d3d9_trace.hpp"
#include "wsi_platform.hpp"

#include <chrono>
#include <cstring>

namespace dxmt {

MTLD3D9Surface::MTLD3D9Surface(
    MTLD3D9Device *device, const D3DSURFACE_DESC &desc, IUnknown *container, WMT::Reference<WMT::Texture> texture,
    uint32_t mipLevel, bool selfPin, WMTTextureType parentTextureType, WMT::Reference<WMT::Buffer> buffer, void *cpuPtr,
    uint32_t pitch, uint32_t arraySlice, void *ownedBacking, Rc<dxmt::Texture> dxmtTexture
) :
    m_device(device),
    m_container(container),
    m_desc(desc),
    m_buffer(std::move(buffer)),
    m_texture(std::move(texture)),
    m_dxmtTexture(std::move(dxmtTexture)),
    m_mip_level(mipLevel),
    m_array_slice(arraySlice),
    m_self_pinned(selfPin),
    m_cpu_ptr(cpuPtr),
    m_pitch(pitch),
    m_owned_backing(ownedBacking) {
  if (m_self_pinned)
    AddRefPrivate();
  // Initialise the view cache against the parent's actual texture type.
  // Sub-resource surfaces (texture mip levels, cube faces) alias the
  // parent's MTLTexture handle — the cache's type drives newTextureView's
  // slice-count resolution (Cube → 6, 2D → 1). Hard-coding Type2D here
  // would silently miscompute slice_count for cube faces the moment any
  // non-default key (sRGB read/write, swizzle) routes through viewFor().
  if (m_texture != nullptr) {
    m_metalFormat = m_texture.pixelFormat();
    m_viewCache.reset(m_texture, parentTextureType, m_metalFormat);
  }
}

MTLD3D9Surface::~MTLD3D9Surface() {
  // Drop the texture-view and the underlying MTLBuffer first so the
  // GPU stops referencing the backing before we free it. WMT::Reference
  // destructors handle the release; explicit reset for ordering clarity.
  m_texture = WMT::Reference<WMT::Texture>{};
  m_buffer = WMT::Reference<WMT::Buffer>{};
  if (m_owned_backing)
    wsi::aligned_free(m_owned_backing);
  if (m_isLosable)
    m_device->onLosableResourceDestroyed();
}

void
MTLD3D9Surface::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    m_device->onLosableResourceCreated();
  }
}

ULONG STDMETHODCALLTYPE
MTLD3D9Surface::AddRef() {
  ULONG ref = ComObject::AddRef();
  // Pin the container, not the device, so the chain
  // surface→container→device stays correct for sub-resource surfaces
  // (mip level of a texture, swapchain backbuffer). For standalone
  // surfaces container == device, so the behaviour matches the prior
  // pattern.
  if (ref == 1)
    m_container->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Surface::Release() {
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    // Losable counter — decrement on pub→0 BEFORE we release the
    // container (which can synchronously destruct anything in the
    // chain, including m_device). Apps that hold a pub ref to a
    // DEFAULT-pool RT, bind it via SetRenderTarget(N, RT) which
    // takes a device priv ref, then Release(RT) and call Reset
    // need the counter to go to 0 at this pub→0 transition — the
    // resource is still alive via the device's bound-state priv
    // ref, but per the D3D9 spec it no longer counts as "app-held"
    // and Reset must proceed. Decrementing only in the dtor (the
    // prior shape) meant the counter stayed non-zero until the
    // device unbinds during Reset itself — which is too late
    // because Reset checks the counter BEFORE unbinding.
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed();
    }
    // Snapshot the self-pin state before releasing the container.
    // m_container->Release() can synchronously destruct the container,
    // whose Com<Surface,false> member release drops our priv ref. If we
    // were holding the last priv ref via something other than this
    // self-pin (we usually aren't, but the surface's full priv chain is
    // not enumerable here) reading m_self_pinned afterwards would be a
    // UAF.
    bool dropSelfPin = m_self_pinned;
    m_self_pinned = false;
    m_container->Release();
    if (dropSelfPin)
      ReleasePrivate();
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::QueryInterface(REFIID riid, void **ppvObject) {
  D9_TRACE("IDirect3DSurface9::QueryInterface");
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
  D9_TRACE("IDirect3DSurface9::GetDevice");
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9_TRACE("IDirect3DSurface9::SetPrivateData");
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9_TRACE("IDirect3DSurface9::GetPrivateData");
  // D3D9's contract: pSizeOfData is in/out, must be non-null. wined3d's
  // d3d9_resource_get_private_data rejects null up front; match that.
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  // DWORD vs UINT mismatch between D3D9's signature and ComPrivateData;
  // marshal via a stack temporary.
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::FreePrivateData(REFGUID refguid) {
  D9_TRACE("IDirect3DSurface9::FreePrivateData");
  // ComPrivateData has no dedicated free; setData with null/zero
  // erases. It returns S_FALSE when the GUID is missing — D3D9's
  // contract is D3DERR_NOTFOUND for that case (matches wined3d's
  // d3d9_resource_free_private_data behaviour).
  HRESULT hr = m_privateData.setData(refguid, 0, nullptr);
  if (hr == S_FALSE)
    return D3DERR_NOTFOUND;
  return hr;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Surface::SetPriority(DWORD PriorityNew) {
  D9_TRACE("IDirect3DSurface9::SetPriority");
  // Priority only matters for D3DPOOL_MANAGED resources (residency
  // hints to the runtime's eviction loop). Surfaces aren't usually
  // MANAGED, so the value is bookkeeping; wined3d preserves it.
  DWORD prev = m_priority;
  m_priority = PriorityNew;
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Surface::GetPriority() {
  D9_TRACE("IDirect3DSurface9::GetPriority");
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9Surface::PreLoad() {
  D9_TRACE("IDirect3DSurface9::PreLoad");
  // Hint to upload MANAGED contents to VRAM ahead of the next draw.
  // Apple Silicon's unified memory makes this a no-op; the texture
  // backing already resides in the GPU-accessible heap.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9Surface::GetType() {
  D9_TRACE("IDirect3DSurface9::GetType");
  return D3DRTYPE_SURFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetContainer(REFIID riid, void **ppContainer) {
  D9_TRACE("IDirect3DSurface9::GetContainer");
  if (!ppContainer)
    return D3DERR_INVALIDCALL;
  *ppContainer = nullptr;
  if (!m_container)
    return E_NOINTERFACE;
  return m_container->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDesc(D3DSURFACE_DESC *pDesc) {
  D9_TRACE("IDirect3DSurface9::GetDesc");
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  *pDesc = m_desc;
  return D3D_OK;
}

// LockRect — currently serves the buffer-backed (Workstream B) and
// MANAGED-mirror paths. DEFAULT-pool surfaces have no cpu_ptr today
// and fall out at the m_cpu_ptr null check below; the DEFAULT+DYNAMIC
// mapping-buffer + rename-ring path is queued separately (#314).
HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::LockRect(D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags) {
  D9_TRACE("IDirect3DSurface9::LockRect");
  D9_HOT_BUMP(texLockRect);
  // T0.3 latency histogram: per-LockRect caller-thread cost. The
  // counter above gives "how many" (sum), the histogram captures
  // distribution. Lazy-mirror first-touch pages and DEFAULT+DYNAMIC
  // rename-ring acquire are the variable-cost paths to watch.
  D9_HOT_SCOPE(lockCpuPin);
  // Diagnostic: one-shot per (Pool, Usage, Format, raw-Flags). Logs
  // the flags the app passed *before* DXVK-shaped normalization below
  // (so we can see whether the game is using DISCARD / NOOVERWRITE /
  // READONLY in earnest).
  D9_TEX_LOCK(m_desc.Pool, m_desc.Usage, m_desc.Format, Flags);
  if (!pLockedRect)
    return D3DERR_INVALIDCALL;
  pLockedRect->Pitch = 0;
  pLockedRect->pBits = nullptr;
  // Lazy-mirror parent owned: alloc the mirror buffer + patch this
  // (and all sibling level) surfaces' cpu_ptr/mirror_src_buffer/pitch
  // before the null check below. Idempotent — every Lock after the
  // first early-outs inside ensureMirror.
  if (!m_cpu_ptr && m_lazyMirrorParent)
    m_lazyMirrorParent->ensureMirror();
  if (!m_cpu_ptr)
    return D3DERR_INVALIDCALL;
  if (m_locked)
    return D3DERR_INVALIDCALL;
  // DXVK-d9vk LockImage validation gates (refs/d9vk/src/d3d9/d3d9_device.cpp:4460+).
  // Pure flag normalization: no behavioural change on the current
  // MANAGED-mirror path (DISCARD/NOOVERWRITE were already no-ops there
  // by spec). Matters once the DEFAULT+DYNAMIC mapping-buffer path
  // lands and consumes these flags.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY))
    return D3DERR_INVALIDCALL;
  // DXVK: "Games like Beyond Good and Evil break if [DONOTWAIT] doesn't
  // succeed." We don't honour it either; strip it so downstream code
  // can't surface it.
  Flags &= ~D3DLOCK_DONOTWAIT;
  // DISCARD + NOOVERWRITE: NOOVERWRITE wins.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  // Per-pixel offset math for partial-rect LockRects. Compressed
  // formats are addressed by 4×4 blocks; pRect's left/top must be
  // block-aligned (D3D9 contract) — wined3d texture.c d3d9_texture_2d
  // _lock_rect:142 enforces. We compute the row-byte and column-byte
  // skip from the format's row-pitch helpers instead of a flat bpp so
  // both DXT and uncompressed share the same math.
  const uint32_t row_pitch = D3DFormatRowPitch(m_desc.Format, m_desc.Width);
  if (row_pitch == 0 || m_pitch == 0)
    return D3DERR_INVALIDCALL;

  LONG x0 = 0;
  LONG y0 = 0;
  if (pRect) {
    if (pRect->left < 0 || pRect->top < 0 || pRect->right > static_cast<LONG>(m_desc.Width) ||
        pRect->bottom > static_cast<LONG>(m_desc.Height) || pRect->left >= pRect->right || pRect->top >= pRect->bottom)
      return D3DERR_INVALIDCALL;
    // Compressed-format block alignment — DXT/BCn surfaces are addressed
    // in 4x4 blocks, so pRect's left/top must be multiples of 4. The
    // byte-offset math below uses `x0 / block_h` rounding which would
    // silently truncate an unaligned rect to the previous block start
    // (apps would Lock at a different rect than they asked for and write
    // out-of-bounds rows). wined3d texture.c::d3d9_texture_2d_lock_rect
    // enforces the same gate. right/bottom may be at the surface extent
    // (which IS block-aligned by D3D9 contract on the create).
    if (IsCompressedFormat(m_desc.Format) && ((pRect->left & 3) || (pRect->top & 3)))
      return D3DERR_INVALIDCALL;
    x0 = pRect->left;
    y0 = pRect->top;
  }
  // DXVK d3d9_device.cpp:4515 — DISCARD is only meaningful on full-
  // extent DEFAULT-pool locks; drop it on partial-rect or non-DEFAULT.
  // DXVK comment: "DISCARD is not ignored for non-DYNAMIC unlike what
  // the docs say" — non-DYNAMIC DEFAULT keeps DISCARD; MANAGED /
  // SYSTEMMEM / SCRATCH strip it.
  const bool full_resource =
      !pRect || (pRect->left == 0 && pRect->top == 0 && static_cast<UINT>(pRect->right) >= m_desc.Width &&
                 static_cast<UINT>(pRect->bottom) >= m_desc.Height);
  if (!full_resource || m_desc.Pool != D3DPOOL_DEFAULT)
    Flags &= ~D3DLOCK_DISCARD;

  // Row-byte offset: y_in_blocks * pitch. For uncompressed, blocks
  // are 1×1 so this is just y * pitch. For DXT, y must be /4 first.
  const uint32_t block_h = IsCompressedFormat(m_desc.Format) ? 4u : 1u;
  const uint32_t row_offset = (static_cast<uint32_t>(y0) / block_h) * m_pitch;
  // Column-byte offset: same idea — bytes-per-block-column for
  // compressed, bytes-per-pixel for uncompressed. Derive from the
  // row pitch and texel-per-row count to avoid a parallel switch.
  const uint32_t cols_per_row = IsCompressedFormat(m_desc.Format) ? (m_desc.Width + 3u) / 4u : m_desc.Width;
  const uint32_t col_bytes = cols_per_row > 0 ? row_pitch / cols_per_row : 0u;
  const uint32_t col_offset = (static_cast<uint32_t>(x0) / block_h) * col_bytes;

  pLockedRect->Pitch = static_cast<INT>(m_pitch);
  pLockedRect->pBits = static_cast<uint8_t *>(m_cpu_ptr) + row_offset + col_offset;
  m_locked = true;
  // Remember the locked rect + READONLY hint so UnlockRect can do a
  // partial-extent replaceRegion (or skip it entirely on READONLY).
  // wined3d texture.c d3d9_surface_unmap pushes only the dirty rect.
  m_locked_readonly = (Flags & D3DLOCK_READONLY) != 0;
  if (pRect) {
    m_locked_x = static_cast<uint32_t>(pRect->left);
    m_locked_y = static_cast<uint32_t>(pRect->top);
    m_locked_w = static_cast<uint32_t>(pRect->right - pRect->left);
    m_locked_h = static_cast<uint32_t>(pRect->bottom - pRect->top);
  } else {
    m_locked_x = 0;
    m_locked_y = 0;
    m_locked_w = m_desc.Width;
    m_locked_h = m_desc.Height;
  }
  // D3DLOCK_NOSYSLOCK / D3DLOCK_NO_DIRTY_UPDATE: no-op on the MANAGED
  // sysmem-mirror path (NOSYSLOCK is a Win32-only critical-section
  // hint; NO_DIRTY_UPDATE matters once AddDirtyRect tracking lands).
  // DISCARD / NOOVERWRITE: gated above; the existing MANAGED upload
  // semantics already match the spec's effective meaning here. The
  // DEFAULT+DYNAMIC mapping-buffer path will consume these flags
  // (rename-ring on DISCARD, skip GPU-sync on NOOVERWRITE).
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::UnlockRect() {
  D9_TRACE("IDirect3DSurface9::UnlockRect");
  if (!m_locked)
    return D3DERR_INVALIDCALL;
  m_locked = false;
  // Buffer-backed surfaces (offscreen-plain, lockable RT/DS) live in
  // a Shared MTLBuffer the texture views into — writes are already
  // GPU-visible at memory level, no upload step.
  //
  // Texture-mirror surfaces (sub-resources of a MANAGED-pool texture)
  // need an explicit upload to push the sysmem mirror into the Metal
  // texture. wined3d texture.c d3d9_surface_unmap +
  // wined3d_texture_upload_data is the same shape: copy the mirror
  // row-by-row into the GPU resource. SYSTEMMEM/SCRATCH skip the push
  // — those pools never reach the GPU through this object (sysmem
  // mirror is the master; UpdateTexture is the path to a DEFAULT
  // mirror).
  //
  // The upload path stages through the device's m_uploadRing and a
  // chained blit encoder on the open cmdbuf. The previous
  // m_texture.replaceRegion shape implicitly fenced against in-flight
  // GPU reads of the same texture on Apple Silicon Shared storage —
  // NFS:MW's VP6 codec issues hundreds of LockRect / UnlockRect per
  // video frame and the EA-logo phase capped at 1 fps with
  // replaceRegion (project_d3d9_loading_perf_rosetta).
  if (m_buffer == nullptr && m_cpu_ptr != nullptr && m_texture != nullptr && m_desc.Pool == D3DPOOL_MANAGED &&
      !m_locked_readonly) {
    // Push only the dirty rect to GPU. wined3d does the same; copying
    // the full level extent on every Unlock burns wine syscall RTT for
    // games that lock 100s of textures during loading. The src pointer
    // must point at the (x0, y0) corner inside the mirror, not at the
    // surface origin — same offset math LockRect used. Compressed
    // formats are addressed by 4×4 blocks; row/col offsets divide
    // through the block size to stay aligned (D3D9 contract).
    const bool compressed = IsCompressedFormat(m_desc.Format);
    const uint32_t row_pitch = D3DFormatRowPitch(m_desc.Format, m_desc.Width);
    const uint32_t block_h = compressed ? 4u : 1u;
    const uint32_t cols_per_row = compressed ? (m_desc.Width + 3u) / 4u : m_desc.Width;
    const uint32_t col_bytes = cols_per_row > 0 ? row_pitch / cols_per_row : 0u;
    const uint32_t src_row_off = (m_locked_y / block_h) * m_pitch;
    const uint32_t src_col_off = (m_locked_x / block_h) * col_bytes;
    WMTOrigin origin{};
    origin.x = m_locked_x;
    origin.y = m_locked_y;
    origin.z = 0;
    WMTSize size{};
    size.width = m_locked_w;
    size.height = m_locked_h;
    size.depth = 1;
    auto t0 = std::chrono::steady_clock::now();
    // Always memcpy through m_uploadRing — the ring slice is the rename
    // ring's renaming step. The earlier mirror-buffer fast path (commit
    // history: stageTextureUploadFromBuffer with the mirror as direct
    // blit source) saved ~1 second/sec of CPU during NFS:MW loading by
    // skipping this memcpy, but it had a correctness bug: the chunk
    // lambda holds the mirror handle and the GPU reads its bytes at
    // *execution time*, not at lambda-post time. On Apple Silicon UMA
    // the mirror is the same physical memory the next LockRect's pBits
    // points at — so a Lock(0)/Unlock cycle that races a still-pending
    // upload chunk overwrites bytes the GPU is about to read. NFS:MW
    // is exclusively MANAGED Lock(flags=0) for textures (verified via
    // DXMT_D9_TEX_DEBUG against the 3D-garage scene); the race
    // manifests as 3D-world texture flicker. Memcpy-into-ring snapshots
    // the bytes at Unlock time so the mirror is safe to overwrite on
    // the next Lock. A per-surface rename ring on the mirror would
    // recover the perf without the race; that's a follow-on.
    (void)m_mirror_src_buffer;
    (void)m_mirror_level_offset;
    const void *src = static_cast<const uint8_t *>(m_cpu_ptr) + src_row_off + src_col_off;
    m_device->stageTextureUpload(m_texture, m_mip_level, m_array_slice, origin, size, src, m_pitch, compressed);
    auto t1 = std::chrono::steady_clock::now();
    D9_HOT_BUMP(texUnlockUpload);
    D9_HOT_ADD(texUnlockUploadMicros, std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
  }
  // Reset locked-rect bookkeeping so a subsequent LockRect with a
  // wider area doesn't accidentally inherit stale narrow bounds.
  m_locked_readonly = false;
  m_locked_x = m_locked_y = m_locked_w = m_locked_h = 0;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::GetDC(HDC *) {
  D9_TRACE("IDirect3DSurface9::GetDC");
  // GDI-compatible surface readback. Apple Silicon has no GDI; only
  // the LOCKABLE+GDI_COMPATIBLE backbuffer path could ever return a
  // real HDC, and dxmt does not currently expose one.
  return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Surface::ReleaseDC(HDC) {
  D9_TRACE("IDirect3DSurface9::ReleaseDC");
  return D3DERR_INVALIDCALL;
}

} // namespace dxmt
