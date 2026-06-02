#include "d3d9_buffer.hpp"

#include <cstring>

#include "d3d9_device.hpp"
#include "log/log.hpp"
#include "wsi_platform.hpp"

namespace dxmt {

namespace {

// Rotate the active backing of a DYNAMIC buffer on a DISCARD-Lock:
// retire the current entry, then pick a replacement (signaled retired
// entry > fresh allocation > GPU-stall-and-reuse oldest retired). The
// vertex / index buffer Locks are otherwise identical here: keeping
// the rotation in one place stops their bodies diverging when the
// rename-ring grows new policy. The device-private accesses
// (m_currentCmdSeq, m_completionEvent, m_cachedSignaled, the
// flush-commit sequence) are passed through from the buffer Lock,
// which has friend access to MTLD3D9Device.
template <typename ForceCommit, typename AllocFresh>
void
lockDiscardRotate(
    uint64_t current_cmd_seq, std::atomic<uint64_t> &cached_signaled, WMT::SharedEvent &completion_event,
    ForceCommit force_commit, WMT::Reference<WMT::Buffer> &active_buffer, void *&active_host, void *&active_owned,
    uint64_t &active_gpu, std::vector<BufferBackingEntry> &retired, AllocFresh alloc_fresh
) {
  // Retire the current active. last_used_seq is the open (not-yet-
  // submitted) cmdbuf's seq: the upper bound on any GPU reference
  // against the active backing. Submitted cmdbufs have signal_seq <
  // current_cmd_seq, so once cached_signaled >= current_cmd_seq, all
  // those cmdbufs have retired and the retired entry is safe to reuse.
  BufferBackingEntry pushed{};
  pushed.mtl_buffer = std::move(active_buffer);
  pushed.owned_backing = active_owned;
  pushed.host_ptr = active_host;
  pushed.gpu_address = active_gpu;
  pushed.last_used_seq = current_cmd_seq;
  retired.push_back(std::move(pushed));
  // Two-pass retire-pool walk. The cached signaled floor is refreshed
  // periodically off the calling thread by refreshSignaledAndTrimRings
  // (throttled to once per kRingRefreshGap=8 chunks). Reading it costs
  // one atomic load, vs ~50μs for a fresh signaledValue(), which goes
  // through wine_unix_call on every invocation. At 30 DISCARDs/frame,
  // an unconditional refresh on every call cost ~1.5ms/frame of pure
  // syscall overhead. Trust the cache first; only pay the unix_call
  // if no entry is reusable under it: that miss only happens during
  // warmup or a flush burst.
  uint64_t coherent = cached_signaled.load(std::memory_order_acquire);
  auto pick_reusable = [&]() -> bool {
    for (auto it = retired.begin(); it != retired.end(); ++it) {
      if (it->last_used_seq <= coherent) {
        active_buffer = std::move(it->mtl_buffer);
        active_owned = it->owned_backing;
        active_host = it->host_ptr;
        active_gpu = it->gpu_address;
        retired.erase(it);
        return true;
      }
    }
    return false;
  };
  if (pick_reusable())
    return;
  // Cache may be stale: force-refresh and retry once before falling
  // through to a fresh allocation.
  uint64_t fresh = completion_event.signaledValue();
  if (fresh > coherent) {
    cached_signaled.store(fresh, std::memory_order_release);
    coherent = fresh;
    if (pick_reusable())
      return;
  }
  // Allocate fresh (pool hit or newBuffer cold path).
  WMT::Reference<WMT::Buffer> fresh_buf{};
  uint64_t fresh_gpu = 0;
  void *fresh_host = nullptr;
  void *fresh_owned = nullptr;
  if (alloc_fresh(fresh_buf, fresh_gpu, fresh_host, fresh_owned)) {
    active_buffer = std::move(fresh_buf);
    active_gpu = fresh_gpu;
    active_host = fresh_host;
    active_owned = fresh_owned;
    return;
  }
  // OOM. Retired pool has entries (we just pushed one) but none are
  // signaled. Stall: force-submit any open work, wait for the oldest
  // retired entry's seq, then pop it as the new active. The prior
  // shape restored the MOST-RECENTLY-retired entry: last_used_seq ==
  // current_cmd_seq, the in-flight cmdbuf the GPU is still reading
  // from: racing the app's upcoming writes against the queued draw's
  // reads. Correctness over the OOM-only perf cliff.
  force_commit();
  auto &front = retired.front();
  uint64_t wait_seq = front.last_used_seq;
  completion_event.waitUntilSignaledValue(wait_seq, UINT64_MAX);
  uint64_t prev_cached = cached_signaled.load(std::memory_order_relaxed);
  if (wait_seq > prev_cached)
    cached_signaled.store(wait_seq, std::memory_order_release);
  active_buffer = std::move(front.mtl_buffer);
  active_owned = front.owned_backing;
  active_host = front.host_ptr;
  active_gpu = front.gpu_address;
  retired.erase(retired.begin());
}

} // namespace

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
  // Self-pin: same shape as MTLD3D9Surface / MTLD3D9Texture. The
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
  // newBuffer XPC + wsi::aligned_malloc + pre-fault memset: the
  // backing was already paid for at a previous buffer's create-time
  // or DISCARD-growth-time and pre-faulted then.
  if (m_device->acquireBufferBacking(m_size, out_buffer, out_gpu, out_host, out_owned))
    return true;
  // Cold path. Always use a fresh WMTBufferInfo for each newBuffer
  // call: reusing the same WMTBufferInfo aliases the second buffer's
  // storage onto the first. Pre-allocate the host backing so the
  // lockable host pointer is 32-bit-addressable; Metal can return a
  // high-memory pointer that 32-bit callers cannot reach.
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
    // Losable counter: decrement on pub→0, BEFORE m_device->Release
    // can destruct the device. See MTLD3D9Surface::Release for the
    // full rationale: Reset's counter check fires while bound
    // resources still have device priv refs, so the counter must
    // track app-pub-ref presence, not full destruct.
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed();
    }
    m_device->Release();
    // Drop the ctor self-pin exactly once: same shape as
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
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::FreePrivateData(REFGUID refguid) {
  HRESULT hr = m_privateData.setData(refguid, 0, nullptr);
  if (hr == S_FALSE)
    return D3DERR_NOTFOUND;
  return hr;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VertexBuffer::SetPriority(DWORD PriorityNew) {
  DWORD prev = m_priority;
  m_priority = PriorityNew;
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetPriority() {
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9VertexBuffer::PreLoad() {
  // Apple Silicon's unified memory makes residency hints a no-op.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetType() {
  return D3DRTYPE_VERTEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  if (!ppbData)
    return D3DERR_INVALIDCALL;
  *ppbData = nullptr;
  if (!m_hostPtr)
    return D3DERR_INVALIDCALL;
  // OffsetToLock=0, SizeToLock=0 means "entire buffer" per the D3D9
  // contract: wined3d buffer.c (wined3d_buffer_map). Otherwise
  // the [offset, offset+size) range must lie within the buffer.
  if (SizeToLock == 0) {
    if (OffsetToLock >= m_size)
      return D3DERR_INVALIDCALL;
  } else {
    uint64_t end = static_cast<uint64_t>(OffsetToLock) + static_cast<uint64_t>(SizeToLock);
    if (end > m_size)
      return D3DERR_INVALIDCALL;
  }
  // Flag normalisation (DXVK d3d9_device.cpp). DISCARD is
  // mutually exclusive with READONLY; the combination is INVALIDCALL.
  // DISCARD combined with NOOVERWRITE silently drops DISCARD (the
  // NOOVERWRITE promise is stronger). Same shape as
  // MTLD3D9Surface::LockRect; bringing buffer Lock into alignment.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY))
    return D3DERR_INVALIDCALL;
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  // D3DLOCK_DISCARD: retire active backing and allocate fresh or reuse
  // signaled retired entry; app promises previous contents unneeded.
  // D3DLOCK_NOOVERWRITE: app promises no GPU-read regions written;
  // active backing stays put.
  if ((Flags & D3DLOCK_DISCARD) && (m_usage & D3DUSAGE_DYNAMIC)) {
    lockDiscardRotate(
        m_device->m_currentCmdSeq, m_device->m_cachedSignaled, m_device->m_completionEvent,
        [this] { m_device->forceFlushAndCommit(); }, m_buffer, m_hostPtr, m_ownedBacking, m_gpuAddress,
        m_retiredBackings,
        [this](WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned) {
          return allocateFreshBacking(out_buffer, out_gpu, out_host, out_owned);
        }
    );
  }
  *ppbData = static_cast<char *>(m_hostPtr) + OffsetToLock;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Unlock() {
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
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
  // See MTLD3D9VertexBuffer::~MTLD3D9VertexBuffer: same shape.
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
  // See MTLD3D9VertexBuffer::allocateFreshBacking: same shape.
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
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  if (Flags & D3DSPD_IUNKNOWN)
    return m_privateData.setInterface(refguid, static_cast<const IUnknown *>(pData));
  return m_privateData.setData(refguid, static_cast<UINT>(SizeOfData), pData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  if (!pSizeOfData)
    return D3DERR_INVALIDCALL;
  UINT size = static_cast<UINT>(*pSizeOfData);
  HRESULT hr = m_privateData.getData(refguid, &size, pData);
  *pSizeOfData = size;
  return hr;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::FreePrivateData(REFGUID refguid) {
  HRESULT hr = m_privateData.setData(refguid, 0, nullptr);
  if (hr == S_FALSE)
    return D3DERR_NOTFOUND;
  return hr;
}

DWORD STDMETHODCALLTYPE
MTLD3D9IndexBuffer::SetPriority(DWORD PriorityNew) {
  DWORD prev = m_priority;
  m_priority = PriorityNew;
  return prev;
}

DWORD STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetPriority() {
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9IndexBuffer::PreLoad() {
  // Apple Silicon's unified memory makes residency hints a no-op.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetType() {
  return D3DRTYPE_INDEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  // Same shape as MTLD3D9VertexBuffer::Lock: see the rationale there
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
  // Flag normalisation: see MTLD3D9VertexBuffer::Lock for the rationale.
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_READONLY)) == (D3DLOCK_DISCARD | D3DLOCK_READONLY))
    return D3DERR_INVALIDCALL;
  if ((Flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE)) == (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE))
    Flags &= ~D3DLOCK_DISCARD;
  if ((Flags & D3DLOCK_DISCARD) && (m_usage & D3DUSAGE_DYNAMIC)) {
    lockDiscardRotate(
        m_device->m_currentCmdSeq, m_device->m_cachedSignaled, m_device->m_completionEvent,
        [this] { m_device->forceFlushAndCommit(); }, m_buffer, m_hostPtr, m_ownedBacking, m_gpuAddress,
        m_retiredBackings,
        [this](WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned) {
          return allocateFreshBacking(out_buffer, out_gpu, out_host, out_owned);
        }
    );
  }
  *ppbData = static_cast<char *>(m_hostPtr) + OffsetToLock;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Unlock() {
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
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
