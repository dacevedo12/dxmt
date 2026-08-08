#include "d3d9_buffer.hpp"

#include "d3d9_device.hpp"
#include "d3d9_private_data.hpp"
#include "d3d9_resource_priority.hpp"
#include "log/log.hpp"
#include "wsi_platform.hpp"

namespace dxmt {

namespace {

// Refresh-time rename shared by the vertex and index buffers: recycle a
// GPU-idle allocation (or mint one) and install it as the current
// name. Metal returns a null buffer on video-memory exhaustion, and a placed
// allocation keeps a non-null CPU pointer even then, so the plain immediate-
// memory null check downstream would not catch it. Reject the fresh
// allocation here, leaving the current name intact, so a null-backed name is
// never installed (a later draw would bind a null MTLBuffer) and Lock reports
// the failure instead of handing back a pointer into a dead allocation.
//
// coherent_seq must mean the GPU has completed through that seq, which is what
// m_completionEvent reports and what the upload rings already recycle their
// host blocks on. The refresh overwrites the recycled allocation with the CPU,
// so a reader draw that is encoded but not yet retired would be torn.
HRESULT
renameDynamicBuffer(MTLD3D9Device *device, DynamicBuffer *dynamic, uint64_t current_seq, uint64_t coherent_seq) {
  bool minted_fresh = false;
  auto fresh = dynamic->allocate(coherent_seq, &minted_fresh);
  if (!fresh.ptr() || !fresh->buffer())
    return D3DERR_OUTOFVIDEOMEMORY;
  dxmt::perf::addFrameCounter(
      device->frameStats(), minted_fresh ? &dxmt::FrameStatistics::frame_dynamic_rename_fresh_count
                                         : &dxmt::FrameStatistics::frame_dynamic_rename_recycled_count
  );
  // Only a mint adds to the footprint the next commit has to relieve; a recycle
  // reuses memory already accounted for.
  if (minted_fresh)
    device->noteDynamicRenameBytes(fresh->length());
  dynamic->updateImmediateName(current_seq, std::move(fresh), 0, false);
  return D3D_OK;
}

} // namespace

MTLD3D9VertexBuffer::MTLD3D9VertexBuffer(
    MTLD3D9Device *device, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool, void *host_ptr, Rc<dxmt::Buffer> dxmt_buffer
) :
    m_device(device),
    m_dxmtBuffer(std::move(dxmt_buffer)),
    m_hostPtr(host_ptr),
    m_size(size),
    m_usage(usage),
    m_fvf(fvf),
    m_pool(pool) {
  // Wrap the underlying Buffer in the DynamicBuffer recycling wrapper (the same
  // one d3d11 uses, d3d11_buffer.cpp). m_dxmtBuffer anchors the Buffer
  // whose raw pointer the wrapper holds; the wrapper's initial name is the
  // Buffer's current() allocation set at create time. That allocation is
  // CPU-writable, so a refresh stores the mirror into it without a staging
  // block (d3d11 dynamic buffers use the same class), but it is Metal-owned and
  // never handed out: nothing the application touches is ever the allocation a
  // draw reads (d3d9_buffer_map.hpp). A Lock(DISCARD) recycles a GPU-idle one.
  // Every generation must carry this class, including the create-time one, or a
  // store into an allocation that cannot be written is silently dropped.
  m_dynamic = new dxmt::DynamicBuffer(m_dxmtBuffer.ptr(), BufferAllocationFlag::CpuWriteCombined);
  // A non-DEFAULT buffer is dirty over its whole extent from creation so its
  // first bind or Unlock uploads the mirror; DEFAULT contents are undefined
  // until the app writes them (DXVK d3d9_common_buffer.cpp).
  if (m_pool != D3DPOOL_DEFAULT)
    m_dirty = true;
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
  // dtor runs only once no in-flight cmdbuf still reads them. m_dynamic
  // (declared after m_dxmtBuffer) destructs first and drops those
  // allocations; each BufferAllocation frees its own private backing. The
  // host mirror was never registered with Metal and the GPU never reads it,
  // so free it directly.
  if (m_hostPtr)
    wsi::aligned_free(m_hostPtr);
  if (m_isLosable)
    m_device->onLosableResourceDestroyed(m_size);
}

void
MTLD3D9VertexBuffer::refreshWholeMirror() {
  if (!m_dirty)
    return;
  // Rename before storing. An earlier draw in this chunk may already be
  // recorded against the current allocation, and overwriting it in place would
  // hand that draw contents it was never recorded with. A fresh allocation
  // leaves the old one intact for as long as those draws pin it, and the FIFO
  // recycles it once the GPU has provably passed them. This is the Metal
  // dynamic-data idiom.
  //
  // The whole mirror goes, not a tracked span: the application does not
  // truthfully report what it wrote, and may still be writing.
  if (FAILED(renameDynamicBuffer(
          m_device, m_dynamic.ptr(), m_device->m_currentCmdSeq,
          m_device->m_cachedSignaled.load(std::memory_order_acquire)
      ))) {
    // Video memory is exhausted, so the cache keeps the previous generation and
    // the draw reads one refresh of stale geometry. Say so once: the failure is
    // otherwise invisible, and stale vertices look like a rendering bug rather
    // than an allocation one. Calling thread under the device lock.
    static bool warned = false;
    if (!warned) {
      warned = true;
      Logger::warn("d3d9: out of video memory refreshing a dynamic buffer; the draw reads stale contents");
    }
    return;
  }
  // The name installed above is GPU-idle by construction, so it takes the
  // mirror directly, the same shape d3d11's dynamic Unmap uses
  // (d3d11_context_imm.cpp). Charged to the staging axis: this is the whole
  // calling-thread cost of bringing the cache up to date, and the frontend
  // total is only meaningful while every such copy lands on one of its terms.
  // Where the allocation has no address this process can reach, the write
  // crosses to the unix side instead of being a local memcpy, so the cost
  // tracks refresh COUNT as much as bytes.
  {
    dxmt::perf::ScopedFrameDuration _upload_timer(
        m_device->frameStats(), &dxmt::FrameStatistics::frame_staging_upload_interval
    );
    dxmt::perf::addFrameCounter(m_device->frameStats(), &dxmt::FrameStatistics::frame_staging_upload_bytes, m_size);
    m_dynamic->immediateName()->updateContents(0, m_hostPtr, m_size, m_dynamic->immediateSuballocation());
  }
  dxmt::perf::addFrameCounter(m_device->frameStats(), &dxmt::FrameStatistics::frame_buffer_refresh_bytes, m_size);
  dxmt::perf::addFrameCounter(m_device->frameStats(), &dxmt::FrameStatistics::frame_buffer_refresh_count, 1);
  // Still mapped means the application can write more before the next draw
  // without telling us, so the cache cannot be declared current yet.
  m_dirty = m_writeLocked;
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
    // Losable counter: decrement on pub->0, BEFORE m_device->Release
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
  dxmt::perf::ScopedFrameDurationCounted _lock_timer(
      m_device->frameStats(), &dxmt::FrameStatistics::frame_resource_lock_interval,
      &dxmt::FrameStatistics::frame_resource_lock_count
  );
  if (!ppbData)
    return D3DERR_INVALIDCALL;
  *ppbData = nullptr;
  // Flag sanitisation (d3d9_buffer_map.hpp): drop the flags this
  // pool/usage does not honour before any of them take effect. No bounds
  // validation: the runtime neither clamps nor rejects OffsetToLock /
  // SizeToLock, the returned pointer is simply base + offset.
  Flags = sanitize_buffer_lock_flags(Flags, m_pool, m_usage, m_device->canOnlySWVP());
  // The lock pointer is the host mirror, disjoint from the GPU-read
  // allocation, so a Lock never waits and never returns WASSTILLDRAWING.
  // DISCARD therefore needs nothing done here. Its purpose is to let a driver
  // hand back fresh storage rather than stall, and neither half applies: the
  // application never touches the allocation, and the refresh that carries the
  // mirror across renames unconditionally, so a name installed here would be
  // retired by that rename without a single draw ever reading it.
  if (!m_hostPtr)
    return D3DERR_INVALIDCALL;
  // A write lock stales the cache and nothing finer is recorded: the
  // application does not truthfully report which bytes it wrote, and may still
  // be writing after Unlock (d3d9_buffer_map.hpp).
  if (buffer_lock_updates_dirty(Flags)) {
    m_dirty = true;
    m_writeLocked = true;
  }
  m_lockCount.increment();
  *ppbData = static_cast<char *>(m_hostPtr) + OffsetToLock;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexBuffer::Unlock() {
  D9DeviceLock lock = m_device->LockDevice();
  // Nothing uploads here, for any pool. The draw that reads the buffer
  // refreshes it, because an application may still be writing through the
  // pointer it was handed: a lock is advisory about its extent and about its
  // lifetime both, so anything finalised at Unlock can already be stale.
  if (m_lockCount.decrement() == 0)
    m_writeLocked = false;
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
    m_dxmtBuffer(std::move(dxmt_buffer)),
    m_hostPtr(host_ptr),
    m_size(size),
    m_usage(usage),
    m_format(format),
    m_pool(pool) {
  // See MTLD3D9VertexBuffer's ctor: wrap the underlying Buffer in the
  // DynamicBuffer recycling wrapper over a CPU-writable, Metal-owned
  // allocation.
  m_dynamic = new dxmt::DynamicBuffer(m_dxmtBuffer.ptr(), BufferAllocationFlag::CpuWriteCombined);
  // See MTLD3D9VertexBuffer's ctor: a non-DEFAULT buffer starts wholly dirty.
  if (m_pool != D3DPOOL_DEFAULT)
    m_dirty = true;
  AddRefPrivate();
}

MTLD3D9IndexBuffer::~MTLD3D9IndexBuffer() {
  // See MTLD3D9VertexBuffer::~MTLD3D9VertexBuffer: same shape. The
  // DynamicBuffer allocations release via m_dynamic (before m_dxmtBuffer);
  // only the host mirror is freed here.
  if (m_hostPtr)
    wsi::aligned_free(m_hostPtr);
  if (m_isLosable)
    m_device->onLosableResourceDestroyed(m_size);
}

void
MTLD3D9IndexBuffer::refreshWholeMirror() {
  if (!m_dirty)
    return;
  // See MTLD3D9VertexBuffer::refreshWholeMirror: rename first, then the whole
  // mirror rather than a span the application described.
  if (FAILED(renameDynamicBuffer(
          m_device, m_dynamic.ptr(), m_device->m_currentCmdSeq,
          m_device->m_cachedSignaled.load(std::memory_order_acquire)
      ))) {
    // Video memory is exhausted, so the cache keeps the previous generation and
    // the draw reads one refresh of stale geometry. Say so once: the failure is
    // otherwise invisible, and stale vertices look like a rendering bug rather
    // than an allocation one. Calling thread under the device lock.
    static bool warned = false;
    if (!warned) {
      warned = true;
      Logger::warn("d3d9: out of video memory refreshing a dynamic buffer; the draw reads stale contents");
    }
    return;
  }
  // See MTLD3D9VertexBuffer::refreshWholeMirror for why the store goes straight
  // into the fresh name and why it is charged to the staging axis.
  {
    dxmt::perf::ScopedFrameDuration _upload_timer(
        m_device->frameStats(), &dxmt::FrameStatistics::frame_staging_upload_interval
    );
    dxmt::perf::addFrameCounter(m_device->frameStats(), &dxmt::FrameStatistics::frame_staging_upload_bytes, m_size);
    m_dynamic->immediateName()->updateContents(0, m_hostPtr, m_size, m_dynamic->immediateSuballocation());
  }
  dxmt::perf::addFrameCounter(m_device->frameStats(), &dxmt::FrameStatistics::frame_buffer_refresh_bytes, m_size);
  dxmt::perf::addFrameCounter(m_device->frameStats(), &dxmt::FrameStatistics::frame_buffer_refresh_count, 1);
  // Still mapped means the application can write more before the next draw
  // without telling us, so the cache cannot be declared current yet.
  m_dirty = m_writeLocked;
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
  dxmt::perf::ScopedFrameDurationCounted _lock_timer(
      m_device->frameStats(), &dxmt::FrameStatistics::frame_resource_lock_interval,
      &dxmt::FrameStatistics::frame_resource_lock_count
  );
  // Same shape as MTLD3D9VertexBuffer::Lock: see the rationale there
  // for the flag sanitisation, DISCARD / NOOVERWRITE semantics, the
  // plain-map GPU sync, and the mode-aware lockable base.
  if (!ppbData)
    return D3DERR_INVALIDCALL;
  *ppbData = nullptr;
  Flags = sanitize_buffer_lock_flags(Flags, m_pool, m_usage, m_device->canOnlySWVP());
  // See MTLD3D9VertexBuffer::Lock for why DISCARD needs nothing done here.
  if (!m_hostPtr)
    return D3DERR_INVALIDCALL;
  // A write lock stales the cache and nothing finer is recorded: the
  // application does not truthfully report which bytes it wrote, and may still
  // be writing after Unlock (d3d9_buffer_map.hpp).
  if (buffer_lock_updates_dirty(Flags)) {
    m_dirty = true;
    m_writeLocked = true;
  }
  m_lockCount.increment();
  *ppbData = static_cast<char *>(m_hostPtr) + OffsetToLock;
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9IndexBuffer::Unlock() {
  D9DeviceLock lock = m_device->LockDevice();
  // See MTLD3D9VertexBuffer::Unlock: the draw refreshes, not this.
  if (m_lockCount.decrement() == 0)
    m_writeLocked = false;
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
