#include "d3d9_buffer.hpp"

#include <cstring>

#include "d3d9_device.hpp"
#include "d3d9_private_data.hpp"
#include "d3d9_resource_priority.hpp"
#include "d3d9_stall.hpp"
#include "log/log.hpp"
#include "wsi_platform.hpp"

namespace dxmt {

namespace {

// GPU-sync gate for a plain DIRECT-mode Lock (neither DISCARD nor
// NOOVERWRITE nor READONLY). A DIRECT buffer's lock pointer aliases the
// DynamicBuffer's current CpuPlaced allocation the GPU reads in place, so
// the returned pointer aliases memory queued draws may still read; DXVK's
// LockBuffer routes the same case through WaitForResource
// (d3d9_device.cpp). BUFFER-mode buffers never reach here: their Lock
// writes a host mirror, never the GPU-read backing. Returns false when
// D3DLOCK_DONOTWAIT is set and the current allocation's last GPU use
// hasn't retired (the caller answers D3DERR_WASSTILLDRAWING). last_use_seq
// comes from BuildDrawCapture's per-draw stamp of the current allocation;
// a DISCARD resets it to 0 because the recycled/fresh allocation is
// GPU-idle by the FIFO invariant. The common never-drawn-since-last-fence
// case costs one atomic load.
template <typename ForceCommit>
bool
lockSyncLastGpuUse(
    uint64_t last_use_seq, std::atomic<uint64_t> &cached_signaled, WMT::SharedEvent &completion_event, DWORD flags,
    ForceCommit force_commit
) {
  uint64_t coherent = cached_signaled.load(std::memory_order_acquire);
  if (last_use_seq <= coherent)
    return true;
  // The cached floor can lag; pay one signaledValue() refresh before
  // deciding to stall (same trust-the-cache-first shape as the DISCARD
  // recycle in DynamicBuffer::allocate).
  uint64_t fresh = completion_event.signaledValue();
  if (fresh > coherent) {
    cached_signaled.store(fresh, std::memory_order_release);
    if (last_use_seq <= fresh)
      return true;
  }
  // The stamped seq is NOT guaranteed to have a committed tail signal
  // behind it: FlushDrawBatch consumes seqs without signaling, and the
  // sync paths emit their signals into a chunk that can stay open
  // until the next Present. Waiting before committing would block the
  // only thread that can commit. Flush + commit unconditionally, and
  // do it on the DONOTWAIT arm too so spinning apps observe forward
  // progress: DXVK's WaitForResource (d3d9_device.cpp) flushes on both
  // arms the same way.
  force_commit();
  if (flags & D3DLOCK_DONOTWAIT)
    return false;
  completion_event.waitUntilSignaledValue(last_use_seq, UINT64_MAX);
  uint64_t prev = cached_signaled.load(std::memory_order_relaxed);
  if (last_use_seq > prev)
    cached_signaled.store(last_use_seq, std::memory_order_release);
  return true;
}

// Lock(DISCARD) rename shared by the vertex/index DIRECT and BUFFER arms:
// recycle a GPU-idle allocation (or mint one) and install it as the current
// name. Metal returns a null buffer on video-memory exhaustion, and a placed
// allocation keeps a non-null CPU pointer even then, so the plain immediate-
// memory null check downstream would not catch it. Reject the fresh
// allocation here, leaving the current name intact, so a null-backed name is
// never installed (a later draw would bind a null MTLBuffer) and Lock reports
// the failure instead of handing back a pointer into a dead allocation.
HRESULT
discardRenameDynamicBuffer(DynamicBuffer *dynamic, uint64_t current_seq, uint64_t coherent_seq) {
  auto fresh = dynamic->allocate(coherent_seq);
  if (!fresh.ptr() || !fresh->buffer())
    return D3DERR_OUTOFVIDEOMEMORY;
  dynamic->updateImmediateName(current_seq, std::move(fresh), 0, false);
  return D3D_OK;
}

} // namespace

MTLD3D9VertexBuffer::MTLD3D9VertexBuffer(
    MTLD3D9Device *device, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool, void *host_ptr,
    Rc<dxmt::Buffer> dxmt_buffer
) :
    m_device(device),
    m_mapMode(determine_buffer_map_mode(pool, usage)),
    m_dxmtBuffer(std::move(dxmt_buffer)),
    m_hostPtr(host_ptr),
    m_size(size),
    m_usage(usage),
    m_fvf(fvf),
    m_pool(pool) {
  // Wrap the underlying Buffer in the DynamicBuffer recycling wrapper (the
  // same one d3d11 uses, d3d11_buffer.cpp:123). m_dxmtBuffer anchors the
  // Buffer whose raw pointer the wrapper holds; the wrapper's initial name
  // is the Buffer's current() allocation set at create time. The allocation
  // flavour matches the map mode: CpuPlaced (DIRECT, the app writes it in
  // place and the GPU reads it) or GpuPrivate (BUFFER, uploaded from a
  // mirror). A Lock(DISCARD) recycles a GPU-idle allocation of this flavour.
  m_dynamic = new dxmt::DynamicBuffer(
      m_dxmtBuffer.ptr(),
      m_mapMode == D3D9BufferMapMode::Buffer ? BufferAllocationFlag::GpuPrivate : BufferAllocationFlag::CpuPlaced
  );
  // A non-DEFAULT staged buffer is dirty over its whole extent from
  // creation so its first bind or Unlock uploads the mirror; DEFAULT
  // contents are undefined until the app writes them (DXVK
  // d3d9_common_buffer.cpp). DIRECT is DEFAULT-only, so it never starts
  // dirty (and has no mirror).
  if (m_mapMode == D3D9BufferMapMode::Buffer && m_pool != D3DPOOL_DEFAULT)
    m_dirtyRange = {0, m_size};
  // Self-pin: same shape as MTLD3D9Surface / MTLD3D9Texture. The
  // override Release path drops the device pin after ComObject::
  // Release has decremented public to 0; the self-pin keeps `this`
  // alive across that window.
  AddRefPrivate();
}

MTLD3D9VertexBuffer::~MTLD3D9VertexBuffer() {
  // Every allocation the DynamicBuffer owns (the current name plus the FIFO
  // of retired ones) is completion-pinned by the ref tracker: a draw that
  // binds this buffer registers an access<> read against the frozen
  // allocation and pins the wrapper through the chunk's resolved pins
  // (BatchedDraw::resolved_vb_pins) until the GPU retires that chunk. So the
  // dtor runs only once no in-flight cmdbuf still reads them, in either map
  // mode. m_dynamic (declared after m_dxmtBuffer) destructs first and drops
  // those allocations; each BufferAllocation frees its own placed / private
  // backing. The BUFFER-mode host mirror was never registered with Metal
  // and the GPU never reads it, so free it directly; m_hostPtr is null in
  // DIRECT mode (the app wrote the placed allocation in place).
  if (m_hostPtr)
    wsi::aligned_free(m_hostPtr);
  if (m_isLosable)
    m_device->onLosableResourceDestroyed(m_size);
}

void
MTLD3D9VertexBuffer::flushDirty() {
  if (m_mapMode != D3D9BufferMapMode::Buffer || m_dirtyRange.empty())
    return;
  // Freeze the DynamicBuffer's current allocation as the copy destination
  // on the calling thread. A draw recording this flush freezes the same
  // immediateName() at BuildDrawCapture time, so the copy and the draw's
  // read land on one allocation across a later Lock(DISCARD) rename.
  m_device->stageBufferUpload(
      m_dynamic->immediateName(), m_dirtyRange.min, static_cast<const char *>(m_hostPtr) + m_dirtyRange.min,
      m_dirtyRange.max - m_dirtyRange.min
  );
  m_dirtyRange.clear();
}

void
MTLD3D9VertexBuffer::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    m_device->onLosableResourceCreated(m_size);
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
  // D3D9 Release-at-0 clamp: handed out at public 0 while self-pinned / bound,
  // so guard the underflow before the decrement (DXVK clamps every device
  // child; same shape as the surface/swapchain/texture clamps).
  if (m_refCount.load() == 0)
    return 0;
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    // Losable counter: decrement on pub→0, BEFORE m_device->Release
    // can destruct the device. See MTLD3D9Surface::Release for the
    // full rationale: Reset's counter check fires while bound
    // resources still have device priv refs, so the counter must
    // track app-pub-ref presence, not full destruct.
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed(m_size);
    }
    // The destructor releases the DynamicBuffer's Metal allocations (whose
    // dispose calls into the device's Metal device), so the device has to
    // outlive it. Drop the device pin LAST: capture it (ReleasePrivate frees
    // `this`), let the destructor run while the pin still keeps the device
    // alive, then release the pin, which may now free it.
    MTLD3D9Device *device = m_device;
    // Drop the ctor self-pin exactly once: same shape as
    // MTLD3D9Surface / MTLD3D9Texture. Subsequent Get/Release cycles
    // on a slot-bound buffer must not call ReleasePrivate again
    // (m_vertexBuffers[N] holds its own priv ref).
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
    device->Release();
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
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::FreePrivateData(REFGUID refguid) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9FreePrivateData(m_privateData, refguid);
}

DWORD STDMETHODCALLTYPE
MTLD3D9VertexBuffer::SetPriority(DWORD PriorityNew) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetResourcePriority(m_pool, m_priority, PriorityNew);
}

DWORD STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetPriority() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9VertexBuffer::PreLoad() {
  D9DeviceLock lock = m_device->LockDevice();
  // Apple Silicon's unified memory makes residency hints a no-op.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetType() {
  D9DeviceLock lock = m_device->LockDevice();
  return D3DRTYPE_VERTEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  D9StallLockTimer _lock_timer;
  if (!ppbData)
    return D3DERR_INVALIDCALL;
  *ppbData = nullptr;
  // Flag sanitisation (d3d9_buffer_map.hpp): drop the flags this
  // pool/usage does not honour before any of them take effect. No bounds
  // validation: the runtime neither clamps nor rejects OffsetToLock /
  // SizeToLock, the returned pointer is simply base + offset.
  // A lost device ignores DISCARD (see isDeviceLost); strip it before sanitize
  // so the rename path is never taken while lost.
  if (m_device->isDeviceLost())
    Flags &= ~D3DLOCK_DISCARD;
  Flags = sanitize_buffer_lock_flags(Flags, m_pool, m_usage, m_device->canOnlySWVP());
  if (m_mapMode == D3D9BufferMapMode::Direct) {
    // The lock pointer aliases the DynamicBuffer's current CpuPlaced
    // allocation the GPU reads in place. D3DLOCK_DISCARD renames it to a
    // GPU-idle allocation the FIFO recycles (or mints), so the app writes a
    // region no queued draw reads (sanitisation guarantees DEFAULT pool
    // here; DXVK discards any DEFAULT-pool buffer); the recycle floor and
    // the will_free_at retire are exactly the BUFFER-mode DISCARD shape
    // below. A plain map instead overwrites the CURRENT allocation the GPU
    // may still read, so it waits on that allocation's last captured GPU
    // use (the write-after-read gate); NOOVERWRITE / READONLY skip the wait
    // (the app promises no GPU-read region is written).
    if (Flags & D3DLOCK_DISCARD) {
      if (HRESULT hr = discardRenameDynamicBuffer(
              m_dynamic.ptr(), m_device->m_currentCmdSeq,
              m_device->m_cachedSignaled.load(std::memory_order_acquire)
          );
          FAILED(hr))
        return hr;
      // The renamed-in allocation is GPU-idle, so the pre-rename use stamp
      // no longer applies.
      m_lastUseSeq = 0;
    } else if (!(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
      if (!lockSyncLastGpuUse(
              m_lastUseSeq, m_device->m_cachedSignaled, m_device->m_completionEvent, Flags,
              [this] { m_device->forceFlushAndCommit(); }
          ))
        return D3DERR_WASSTILLDRAWING;
    }
  } else {
    // The lock pointer is the host mirror, disjoint from the GPU-read
    // allocation, so a Lock never waits and never returns WASSTILLDRAWING.
    // On DISCARD, install a fresh current name from the DynamicBuffer:
    // allocate() recycles a FIFO allocation the GPU has passed (will_free_at
    // <= the signaled floor) or mints one, and updateImmediateName retires
    // the old name tagged with the open cmdbuf's seq. The whole-buffer dirty
    // span conjoined below re-uploads the mirror into the fresh name, while
    // in-flight draws that froze the old allocation keep reading it
    // undisturbed: it stays alive on those draws' captured Rc plus the FIFO
    // entry and recycles only once provably GPU-idle. Without the fresh name
    // the re-upload clobbers the same allocation a queued draw still reads
    // (write-after-read tearing). Ports d3d11_context_imm.cpp
    // MapDynamicBuffer's WRITE_DISCARD arm (minus the view rename d3d9
    // buffers do not use). NOOVERWRITE / plain never rename: they write
    // disjoint mirror bytes no in-flight draw reads.
    if (!m_hostPtr)
      return D3DERR_INVALIDCALL;
    if (Flags & D3DLOCK_DISCARD) {
      if (HRESULT hr = discardRenameDynamicBuffer(
              m_dynamic.ptr(), m_device->m_currentCmdSeq,
              m_device->m_cachedSignaled.load(std::memory_order_acquire)
          );
          FAILED(hr))
        return hr;
    }
    // Track the dirty span the outer Unlock (or a bind) uploads into the
    // current allocation.
    if (buffer_lock_updates_dirty(Flags))
      m_dirtyRange.conjoin(buffer_lock_dirty_range(Flags, OffsetToLock, SizeToLock, m_size, m_pool, m_usage));
    m_lockCount.increment();
  }
  // Lockable base: DIRECT hands out the current in-place allocation's mapped
  // memory (a DISCARD rename above just advanced it); BUFFER hands out the
  // host mirror.
  void *base = m_mapMode == D3D9BufferMapMode::Direct ? m_dynamic->immediateMappedMemory() : m_hostPtr;
  if (!base)
    return D3DERR_INVALIDCALL;
  *ppbData = static_cast<char *>(base) + OffsetToLock;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Unlock() {
  D9DeviceLock lock = m_device->LockDevice();
  // DIRECT buffers were written in place; nothing to do. A BUFFER-mode
  // buffer uploads its dirty range on the outer Unlock only (D3D9 permits
  // nested locks).
  if (m_mapMode != D3D9BufferMapMode::Buffer)
    return D3D_OK;
  if (m_lockCount.decrement() != 0)
    return D3D_OK;
  flushDirty();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc) {
  D9DeviceLock lock = m_device->LockDevice();
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
    MTLD3D9Device *device, UINT size, DWORD usage, D3DFORMAT format, D3DPOOL pool, void *host_ptr,
    Rc<dxmt::Buffer> dxmt_buffer
) :
    m_device(device),
    m_mapMode(determine_buffer_map_mode(pool, usage)),
    m_dxmtBuffer(std::move(dxmt_buffer)),
    m_hostPtr(host_ptr),
    m_size(size),
    m_usage(usage),
    m_format(format),
    m_pool(pool) {
  // See MTLD3D9VertexBuffer's ctor: wrap the underlying Buffer in the
  // DynamicBuffer recycling wrapper with the map mode's allocation flavour
  // (CpuPlaced for DIRECT, GpuPrivate for BUFFER).
  m_dynamic = new dxmt::DynamicBuffer(
      m_dxmtBuffer.ptr(),
      m_mapMode == D3D9BufferMapMode::Buffer ? BufferAllocationFlag::GpuPrivate : BufferAllocationFlag::CpuPlaced
  );
  // See MTLD3D9VertexBuffer's ctor: a non-DEFAULT staged buffer starts
  // wholly dirty (DIRECT is DEFAULT-only and never starts dirty).
  if (m_mapMode == D3D9BufferMapMode::Buffer && m_pool != D3DPOOL_DEFAULT)
    m_dirtyRange = {0, m_size};
  AddRefPrivate();
}

MTLD3D9IndexBuffer::~MTLD3D9IndexBuffer() {
  // See MTLD3D9VertexBuffer::~MTLD3D9VertexBuffer: same shape. The
  // DynamicBuffer allocations release via m_dynamic (before m_dxmtBuffer);
  // only the BUFFER-mode host mirror is freed here, and m_hostPtr is null
  // in DIRECT mode.
  if (m_hostPtr)
    wsi::aligned_free(m_hostPtr);
  if (m_isLosable)
    m_device->onLosableResourceDestroyed(m_size);
}

void
MTLD3D9IndexBuffer::flushDirty() {
  if (m_mapMode != D3D9BufferMapMode::Buffer || m_dirtyRange.empty())
    return;
  // See MTLD3D9VertexBuffer::flushDirty: freeze the current allocation as
  // the copy destination on the calling thread.
  m_device->stageBufferUpload(
      m_dynamic->immediateName(), m_dirtyRange.min, static_cast<const char *>(m_hostPtr) + m_dirtyRange.min,
      m_dirtyRange.max - m_dirtyRange.min
  );
  m_dirtyRange.clear();
}

void
MTLD3D9IndexBuffer::markLosable() {
  if (!m_isLosable) {
    m_isLosable = true;
    m_device->onLosableResourceCreated(m_size);
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
  // D3D9 Release-at-0 clamp (see MTLD3D9VertexBuffer::Release).
  if (m_refCount.load() == 0)
    return 0;
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    if (m_isLosable) {
      m_isLosable = false;
      m_device->onLosableResourceDestroyed(m_size);
    }
    // The destructor releases the DynamicBuffer's Metal allocations, so the
    // device has to outlive it (see MTLD3D9VertexBuffer::Release): drop the
    // device pin last, after the destructor has run.
    MTLD3D9Device *device = m_device;
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
    device->Release();
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
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::SetPrivateData(REFGUID refguid, const void *pData, DWORD SizeOfData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetPrivateData(m_privateData, refguid, pData, SizeOfData, Flags);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9GetPrivateData(m_privateData, refguid, pData, pSizeOfData);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::FreePrivateData(REFGUID refguid) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9FreePrivateData(m_privateData, refguid);
}

DWORD STDMETHODCALLTYPE
MTLD3D9IndexBuffer::SetPriority(DWORD PriorityNew) {
  D9DeviceLock lock = m_device->LockDevice();
  return D3D9SetResourcePriority(m_pool, m_priority, PriorityNew);
}

DWORD STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetPriority() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_priority;
}

void STDMETHODCALLTYPE
MTLD3D9IndexBuffer::PreLoad() {
  D9DeviceLock lock = m_device->LockDevice();
  // Apple Silicon's unified memory makes residency hints a no-op.
}

D3DRESOURCETYPE STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetType() {
  D9DeviceLock lock = m_device->LockDevice();
  return D3DRTYPE_INDEXBUFFER;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) {
  D9DeviceLock lock = m_device->LockDevice();
  D9StallLockTimer _lock_timer;
  // Same shape as MTLD3D9VertexBuffer::Lock: see the rationale there
  // for the flag sanitisation, DISCARD / NOOVERWRITE semantics, the
  // plain-map GPU sync, and the mode-aware lockable base.
  if (!ppbData)
    return D3DERR_INVALIDCALL;
  *ppbData = nullptr;
  // A lost device ignores DISCARD (see isDeviceLost); strip it before sanitize
  // so the rename path is never taken while lost.
  if (m_device->isDeviceLost())
    Flags &= ~D3DLOCK_DISCARD;
  Flags = sanitize_buffer_lock_flags(Flags, m_pool, m_usage, m_device->canOnlySWVP());
  if (m_mapMode == D3D9BufferMapMode::Direct) {
    if (Flags & D3DLOCK_DISCARD) {
      if (HRESULT hr = discardRenameDynamicBuffer(
              m_dynamic.ptr(), m_device->m_currentCmdSeq,
              m_device->m_cachedSignaled.load(std::memory_order_acquire)
          );
          FAILED(hr))
        return hr;
      // See MTLD3D9VertexBuffer::Lock: the renamed-in allocation is GPU-idle.
      m_lastUseSeq = 0;
    } else if (!(Flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY))) {
      if (!lockSyncLastGpuUse(
              m_lastUseSeq, m_device->m_cachedSignaled, m_device->m_completionEvent, Flags,
              [this] { m_device->forceFlushAndCommit(); }
          ))
        return D3DERR_WASSTILLDRAWING;
    }
  } else {
    // On DISCARD, install a fresh current name from the DynamicBuffer; see
    // MTLD3D9VertexBuffer::Lock for the recycle / write-after-read
    // rationale and the old-allocation lifetime.
    if (!m_hostPtr)
      return D3DERR_INVALIDCALL;
    if (Flags & D3DLOCK_DISCARD) {
      if (HRESULT hr = discardRenameDynamicBuffer(
              m_dynamic.ptr(), m_device->m_currentCmdSeq,
              m_device->m_cachedSignaled.load(std::memory_order_acquire)
          );
          FAILED(hr))
        return hr;
    }
    if (buffer_lock_updates_dirty(Flags))
      m_dirtyRange.conjoin(buffer_lock_dirty_range(Flags, OffsetToLock, SizeToLock, m_size, m_pool, m_usage));
    m_lockCount.increment();
  }
  void *base = m_mapMode == D3D9BufferMapMode::Direct ? m_dynamic->immediateMappedMemory() : m_hostPtr;
  if (!base)
    return D3DERR_INVALIDCALL;
  *ppbData = static_cast<char *>(base) + OffsetToLock;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Unlock() {
  D9DeviceLock lock = m_device->LockDevice();
  // See MTLD3D9VertexBuffer::Unlock.
  if (m_mapMode != D3D9BufferMapMode::Buffer)
    return D3D_OK;
  if (m_lockCount.decrement() != 0)
    return D3D_OK;
  flushDirty();
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc) {
  D9DeviceLock lock = m_device->LockDevice();
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
