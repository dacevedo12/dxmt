#include "d3d9_buffer.hpp"

#include <cstring>

#include "d3d9_device.hpp"
#include "d3d9_trace.hpp"
#include "log/log.hpp"
#include "wsi_platform.hpp"

namespace dxmt {

MTLD3D9VertexBuffer::MTLD3D9VertexBuffer(
    MTLD3D9Device *device, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool, WMT::Reference<WMT::Buffer> buffer,
    uint64_t gpu_address, void *host_ptr, void *owned_backing
) :
    m_device(device),
    m_buffer(std::move(buffer)),
    m_gpuAddress(gpu_address),
    m_hostPtr(host_ptr),
    m_ownedBacking(owned_backing),
    m_size(size),
    m_usage(usage),
    m_fvf(fvf),
    m_pool(pool) {
  // Self-pin — same shape as MTLD3D9Surface / MTLD3D9Texture. The
  // override Release path drops the device pin after ComObject::
  // Release has decremented public to 0; the self-pin keeps `this`
  // alive across that window.
  AddRefPrivate();
}

MTLD3D9VertexBuffer::~MTLD3D9VertexBuffer() {
  // Donate the active backing + every retired backing to the device's
  // shared pool. By dtor time the GPU drain (FlushDrawBatch +
  // CommitCurrentChunk + WaitCPUFence in any teardown path the device
  // owns) has ensured no in-flight cmdbuf still reads from these
  // regions, so they're safe to hand to the next caller. Same total
  // VRAM behaviour as the prior free-on-dtor path; only the timing of
  // the free changes (deferred until the pool fills past
  // kMaxBufferBackingPoolSize).
  m_device->releaseBufferBacking(std::move(m_buffer), m_ownedBacking, m_gpuAddress, m_size);
  for (auto &entry : m_retiredBackings) {
    m_device->releaseBufferBacking(std::move(entry.mtl_buffer), entry.owned_backing, entry.gpu_address, m_size);
  }
  m_retiredBackings.clear();
  if (m_isLosable)
    m_device->onLosableResourceDestroyed();
}

bool
MTLD3D9VertexBuffer::allocateFreshBacking(
    WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned
) {
  // Hot path: pull from the device-level pool first. Skips the
  // newBuffer XPC + wsi::aligned_malloc + pre-fault memset — the
  // backing was already paid for at a previous buffer's create-time
  // or DISCARD-growth-time and pre-faulted then.
  if (m_device->acquireBufferBacking(m_size, out_buffer, out_gpu, out_host, out_owned))
    return true;
  // Cold path. project_wmt_buffer_info_aliasing memory: fresh
  // WMTBufferInfo each newBuffer call. project_wow64_abi_gotchas
  // memory: pre-allocate the host backing so the lockable host
  // pointer is 32-bit-addressable; never let Metal pick a
  // high-memory pointer.
  void *backing = wsi::aligned_malloc(m_size, DXMT_PAGE_SIZE);
  if (!backing)
    return false;
  std::memset(backing, 0, m_size);
  WMTBufferInfo info{};
  info.length = m_size;
  info.options = WMTResourceStorageModeShared;
  info.memory.set(backing);
  WMT::Reference<WMT::Buffer> buf = m_device->m_metalDevice.newBuffer(info);
  if (buf == nullptr) {
    wsi::aligned_free(backing);
    return false;
  }
  out_buffer = std::move(buf);
  out_gpu = info.gpu_address;
  out_host = backing;
  out_owned = backing;
  return true;
}

void
MTLD3D9VertexBuffer::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    m_device->onLosableResourceCreated();
  }
}

ULONG STDMETHODCALLTYPE
MTLD3D9VertexBuffer::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Release() {
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    // Losable counter — decrement on pub→0, BEFORE m_device->Release
    // can destruct the device. See MTLD3D9Surface::Release for the
    // full rationale: Reset's counter check fires while bound
    // resources still have device priv refs, so the counter must
    // track app-pub-ref presence, not full destruct.
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed();
    }
    m_device->Release();
    // Drop the ctor self-pin exactly once — same shape as
    // MTLD3D9Surface / MTLD3D9Texture. Subsequent Get/Release cycles
    // on a slot-bound buffer must not call ReleasePrivate again
    // (m_vertexBuffers[N] holds its own priv ref).
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::QueryInterface(REFIID riid, void **ppvObject) {
  D9_TRACE("IDirect3DVertexBuffer9::QueryInterface");
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DVertexBuffer9)) {
    *ppvObject = static_cast<IDirect3DVertexBuffer9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9_TRACE("IDirect3DVertexBuffer9::GetDevice");
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9_TRACE("IDirect3DVertexBuffer9::SetPrivateData");
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9_TRACE("IDirect3DVertexBuffer9::GetPrivateData");
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::FreePrivateData(REFGUID refguid) {
  D9_TRACE("IDirect3DVertexBuffer9::FreePrivateData");
  HRESULT hr = m_privateData.setData(refguid, 0, nullptr);
  if (hr == S_FALSE)
    return D3DERR_NOTFOUND;
  return hr;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VertexBuffer::SetPriority(DWORD PriorityNew) {
  D9_TRACE("IDirect3DVertexBuffer9::SetPriority");
  DWORD prev = m_priority;
  m_priority = PriorityNew;
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetPriority() {
  D9_TRACE("IDirect3DVertexBuffer9::GetPriority");
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9VertexBuffer::PreLoad() {
  D9_TRACE("IDirect3DVertexBuffer9::PreLoad");
  // Apple Silicon's unified memory makes residency hints a no-op.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetType() {
  D9_TRACE("IDirect3DVertexBuffer9::GetType");
  return D3DRTYPE_VERTEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  D9_TRACE("IDirect3DVertexBuffer9::Lock");
  D9_HOT_BUMP(bufLock);
  if (!ppbData)
    return D3DERR_INVALIDCALL;
  *ppbData = nullptr;
  if (!m_hostPtr)
    return D3DERR_INVALIDCALL;
  // OffsetToLock=0, SizeToLock=0 means "entire buffer" per the D3D9
  // contract — wined3d buffer.c:746 (wined3d_buffer_map). Otherwise
  // the [offset, offset+size) range must lie within the buffer.
  if (SizeToLock == 0) {
    if (OffsetToLock >= m_size)
      return D3DERR_INVALIDCALL;
  } else {
    uint64_t end = static_cast<uint64_t>(OffsetToLock) + static_cast<uint64_t>(SizeToLock);
    if (end > m_size)
      return D3DERR_INVALIDCALL;
  }
  // Flag normalisation (DXVK d3d9_device.cpp:5517-5524). DISCARD is
  // mutually exclusive with READONLY; the combination is INVALIDCALL.
  // DISCARD combined with NOOVERWRITE silently drops DISCARD (the
  // NOOVERWRITE promise is stronger). Same shape as
  // MTLD3D9Surface::LockRect; bringing buffer Lock into alignment.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY))
    return D3DERR_INVALIDCALL;
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  // D3DLOCK_DISCARD: the app promises the previous contents are no
  // longer needed and wants to overwrite without waiting for prior
  // GPU consumption to finish. wined3d's cs_map_upload_bo (cs.c:3185)
  // hits adapter_alloc_bo on DISCARD — a *brand-new* BO; the old one
  // is refcounted and lives until prior cmdbuf references retire.
  // dxmt's port: retire the current active backing into
  // m_retiredBackings tagged with the current cmdbuf seq, then either
  // pop a retired entry whose last_used_seq is signaled (reuse) or
  // allocate a fresh MTLBuffer + host backing. Within-cmdbuf wrap is
  // physically impossible — we never write into a region the GPU is
  // still reading.
  //
  // D3DLOCK_NOOVERWRITE: app promises not to write any region the
  // GPU is currently reading; active backing stays put.
  //
  // No flag / NO_DIRTY_UPDATE / READONLY: active backing stays put.
  if ((Flags & D3DLOCK_DISCARD) && (m_usage & D3DUSAGE_DYNAMIC)) {
    // Retire the current active. last_used_seq is m_currentCmdSeq —
    // the seq of the open (not-yet-submitted) cmdbuf which is the
    // upper bound on any GPU reference against the active backing.
    // Submitted cmdbufs have signal_seq < m_currentCmdSeq, so once
    // m_cachedSignaled >= m_currentCmdSeq, all those cmdbufs have
    // retired and the retired entry is safe to reuse.
    DynamicBackingEntry retired{};
    retired.mtl_buffer = std::move(m_buffer);
    retired.owned_backing = m_ownedBacking;
    retired.host_ptr = m_hostPtr;
    retired.gpu_address = m_gpuAddress;
    retired.last_used_seq = m_device->m_currentCmdSeq;
    m_retiredBackings.push_back(std::move(retired));
    // Two-pass retire-pool walk. The cached signaled floor is
    // refreshed periodically off the calling thread by
    // refreshSignaledAndTrimRings (throttled to once per
    // kRingRefreshGap=8 chunks, see d3d9_device.cpp:6311). Reading
    // it costs one atomic load, vs ~50μs for a fresh
    // m_completionEvent.signaledValue() — which goes through
    // wine_unix_call on every invocation. At 30 DISCARDs/frame, the
    // unconditional refresh that used to live here cost ~1.5ms/frame
    // of pure syscall overhead. Trust the cache first; only pay the
    // unix_call if no entry is reusable under it — that miss only
    // happens during warmup or a flush burst.
    uint64_t coherent = m_device->m_cachedSignaled.load(std::memory_order_acquire);
    auto pick_reusable = [&]() -> bool {
      for (auto it = m_retiredBackings.begin(); it != m_retiredBackings.end(); ++it) {
        if (it->last_used_seq <= coherent) {
          m_buffer = std::move(it->mtl_buffer);
          m_ownedBacking = it->owned_backing;
          m_hostPtr = it->host_ptr;
          m_gpuAddress = it->gpu_address;
          m_retiredBackings.erase(it);
          return true;
        }
      }
      return false;
    };
    bool reused = pick_reusable();
    if (!reused) {
      // Cache may be stale — force-refresh and retry once before
      // falling through to a fresh allocation.
      uint64_t fresh = m_device->m_completionEvent.signaledValue();
      if (fresh > coherent) {
        m_device->m_cachedSignaled.store(fresh, std::memory_order_release);
        coherent = fresh;
        reused = pick_reusable();
      }
    }
    if (!reused) {
      // Allocate fresh (pool hit or newBuffer cold path).
      WMT::Reference<WMT::Buffer> fresh_buf{};
      uint64_t fresh_gpu = 0;
      void *fresh_host = nullptr;
      void *fresh_owned = nullptr;
      if (!allocateFreshBacking(fresh_buf, fresh_gpu, fresh_host, fresh_owned)) {
        // Allocation failed — restore the most-recently-retired entry
        // back into active position so the buffer stays usable. App
        // sees a successful Lock but on the OLD backing; this is the
        // same fall-through wined3d takes when alloc fails.
        auto &last = m_retiredBackings.back();
        m_buffer = std::move(last.mtl_buffer);
        m_ownedBacking = last.owned_backing;
        m_hostPtr = last.host_ptr;
        m_gpuAddress = last.gpu_address;
        m_retiredBackings.pop_back();
      } else {
        m_buffer = std::move(fresh_buf);
        m_gpuAddress = fresh_gpu;
        m_hostPtr = fresh_host;
        m_ownedBacking = fresh_owned;
      }
    }
  }
  *ppbData = static_cast<char *>(m_hostPtr) + OffsetToLock;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Unlock() {
  D9_TRACE("IDirect3DVertexBuffer9::Unlock");
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
  D9_TRACE("IDirect3DVertexBuffer9::GetDesc");
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  pDesc->Format = D3DFMT_VERTEXDATA;
  pDesc->Type = D3DRTYPE_VERTEXBUFFER;
  pDesc->Usage = m_usage;
  pDesc->Pool = m_pool;
  pDesc->Size = m_size;
  pDesc->FVF = m_fvf;
  return D3D_OK;
}

// ============================================================
// MTLD3D9IndexBuffer
// ============================================================

MTLD3D9IndexBuffer::MTLD3D9IndexBuffer(
    MTLD3D9Device *device, UINT size, DWORD usage, D3DFORMAT format, D3DPOOL pool, WMT::Reference<WMT::Buffer> buffer,
    uint64_t gpu_address, void *host_ptr, void *owned_backing
) :
    m_device(device),
    m_buffer(std::move(buffer)),
    m_hostPtr(host_ptr),
    m_ownedBacking(owned_backing),
    m_gpuAddress(gpu_address),
    m_size(size),
    m_usage(usage),
    m_format(format),
    m_pool(pool) {
  AddRefPrivate();
}

MTLD3D9IndexBuffer::~MTLD3D9IndexBuffer() {
  // See MTLD3D9VertexBuffer::~MTLD3D9VertexBuffer — same shape.
  m_device->releaseBufferBacking(std::move(m_buffer), m_ownedBacking, m_gpuAddress, m_size);
  for (auto &entry : m_retiredBackings) {
    m_device->releaseBufferBacking(std::move(entry.mtl_buffer), entry.owned_backing, entry.gpu_address, m_size);
  }
  m_retiredBackings.clear();
  if (m_isLosable)
    m_device->onLosableResourceDestroyed();
}

bool
MTLD3D9IndexBuffer::allocateFreshBacking(
    WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned
) {
  // See MTLD3D9VertexBuffer::allocateFreshBacking — same shape.
  if (m_device->acquireBufferBacking(m_size, out_buffer, out_gpu, out_host, out_owned))
    return true;
  void *backing = wsi::aligned_malloc(m_size, DXMT_PAGE_SIZE);
  if (!backing)
    return false;
  std::memset(backing, 0, m_size);
  WMTBufferInfo info{};
  info.length = m_size;
  info.options = WMTResourceStorageModeShared;
  info.memory.set(backing);
  WMT::Reference<WMT::Buffer> buf = m_device->m_metalDevice.newBuffer(info);
  if (buf == nullptr) {
    wsi::aligned_free(backing);
    return false;
  }
  out_buffer = std::move(buf);
  out_gpu = info.gpu_address;
  out_host = backing;
  out_owned = backing;
  return true;
}

void
MTLD3D9IndexBuffer::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    m_device->onLosableResourceCreated();
  }
}

ULONG STDMETHODCALLTYPE
MTLD3D9IndexBuffer::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Release() {
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed();
    }
    m_device->Release();
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::QueryInterface(REFIID riid, void **ppvObject) {
  D9_TRACE("IDirect3DIndexBuffer9::QueryInterface");
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DResource9) || riid == __uuidof(IDirect3DIndexBuffer9)) {
    *ppvObject = static_cast<IDirect3DIndexBuffer9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9_TRACE("IDirect3DIndexBuffer9::GetDevice");
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9_TRACE("IDirect3DIndexBuffer9::SetPrivateData");
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9_TRACE("IDirect3DIndexBuffer9::GetPrivateData");
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::FreePrivateData(REFGUID refguid) {
  D9_TRACE("IDirect3DIndexBuffer9::FreePrivateData");
  HRESULT hr = m_privateData.setData(refguid, 0, nullptr);
  if (hr == S_FALSE)
    return D3DERR_NOTFOUND;
  return hr;
}

DWORD STDMETHODCALLTYPE
MTLD3D9IndexBuffer::SetPriority(DWORD PriorityNew) {
  D9_TRACE("IDirect3DIndexBuffer9::SetPriority");
  DWORD prev = m_priority;
  m_priority = PriorityNew;
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetPriority() {
  D9_TRACE("IDirect3DIndexBuffer9::GetPriority");
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9IndexBuffer::PreLoad() {
  D9_TRACE("IDirect3DIndexBuffer9::PreLoad");
  // Apple Silicon's unified memory makes residency hints a no-op.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetType() {
  D9_TRACE("IDirect3DIndexBuffer9::GetType");
  return D3DRTYPE_INDEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  D9_TRACE("IDirect3DIndexBuffer9::Lock");
  D9_HOT_BUMP(bufLock);
  // Same shape as MTLD3D9VertexBuffer::Lock — see the rationale there
  // for DISCARD / NOOVERWRITE semantics.
  if (!ppbData)
    return D3DERR_INVALIDCALL;
  *ppbData = nullptr;
  if (!m_hostPtr)
    return D3DERR_INVALIDCALL;
  if (SizeToLock == 0) {
    if (OffsetToLock >= m_size)
      return D3DERR_INVALIDCALL;
  } else {
    uint64_t end = static_cast<uint64_t>(OffsetToLock) + static_cast<uint64_t>(SizeToLock);
    if (end > m_size)
      return D3DERR_INVALIDCALL;
  }
  // Flag normalisation — see MTLD3D9VertexBuffer::Lock for the rationale.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY))
    return D3DERR_INVALIDCALL;
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  // See MTLD3D9VertexBuffer::Lock for the wined3d-shaped retire-pool
  // rationale.
  if ((Flags & D3DLOCK_DISCARD) && (m_usage & D3DUSAGE_DYNAMIC)) {
    DynamicBackingEntry retired{};
    retired.mtl_buffer = std::move(m_buffer);
    retired.owned_backing = m_ownedBacking;
    retired.host_ptr = m_hostPtr;
    retired.gpu_address = m_gpuAddress;
    retired.last_used_seq = m_device->m_currentCmdSeq;
    m_retiredBackings.push_back(std::move(retired));
    // See MTLD3D9VertexBuffer::Lock — trust cached signaled first,
    // only force-refresh on miss.
    uint64_t coherent = m_device->m_cachedSignaled.load(std::memory_order_acquire);
    auto pick_reusable = [&]() -> bool {
      for (auto it = m_retiredBackings.begin(); it != m_retiredBackings.end(); ++it) {
        if (it->last_used_seq <= coherent) {
          m_buffer = std::move(it->mtl_buffer);
          m_ownedBacking = it->owned_backing;
          m_hostPtr = it->host_ptr;
          m_gpuAddress = it->gpu_address;
          m_retiredBackings.erase(it);
          return true;
        }
      }
      return false;
    };
    bool reused = pick_reusable();
    if (!reused) {
      uint64_t fresh = m_device->m_completionEvent.signaledValue();
      if (fresh > coherent) {
        m_device->m_cachedSignaled.store(fresh, std::memory_order_release);
        coherent = fresh;
        reused = pick_reusable();
      }
    }
    if (!reused) {
      WMT::Reference<WMT::Buffer> fresh_buf{};
      uint64_t fresh_gpu = 0;
      void *fresh_host = nullptr;
      void *fresh_owned = nullptr;
      if (!allocateFreshBacking(fresh_buf, fresh_gpu, fresh_host, fresh_owned)) {
        auto &last = m_retiredBackings.back();
        m_buffer = std::move(last.mtl_buffer);
        m_ownedBacking = last.owned_backing;
        m_hostPtr = last.host_ptr;
        m_gpuAddress = last.gpu_address;
        m_retiredBackings.pop_back();
      } else {
        m_buffer = std::move(fresh_buf);
        m_gpuAddress = fresh_gpu;
        m_hostPtr = fresh_host;
        m_ownedBacking = fresh_owned;
      }
    }
  }
  *ppbData = static_cast<char *>(m_hostPtr) + OffsetToLock;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Unlock() {
  D9_TRACE("IDirect3DIndexBuffer9::Unlock");
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
  D9_TRACE("IDirect3DIndexBuffer9::GetDesc");
  if (!pDesc)
    return D3DERR_INVALIDCALL;
  pDesc->Format = m_format;
  pDesc->Type = D3DRTYPE_INDEXBUFFER;
  pDesc->Usage = m_usage;
  pDesc->Pool = m_pool;
  pDesc->Size = m_size;
  return D3D_OK;
}

} // namespace dxmt
