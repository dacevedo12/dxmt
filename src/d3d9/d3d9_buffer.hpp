#pragma once

#include "Metal.hpp"

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"
#include "d3d9_buffer_map.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_dynamic.hpp"

namespace dxmt {

class MTLD3D9Device;

// IDirect3DVertexBuffer9 backed by a dxmt::DynamicBuffer recycling wrapper
// (the same one d3d11 uses) whose allocation flavour is fixed at create
// time from the map mode (see d3d9_buffer_map.hpp). DIRECT (DEFAULT +
// DYNAMIC): a CpuPlaced allocation the app writes in place and the GPU
// reads; a plain Lock waits on the current allocation's last GPU read
// (write-after-read), and a DISCARD recycles a GPU-idle allocation from
// the FIFO. BUFFER (every other pool/usage): a GpuPrivate allocation plus
// a host mirror the app writes and Unlock copies the dirty range into, so
// a Lock never waits on the GPU. No sub-resources; standalone shape
// (self-pin in ctor, AddRef/Release pin device). References:
// d3d11_buffer.cpp / d3d11_context_imm.cpp (DynamicBuffer +
// MapDynamicBuffer), DXVK d3d9_common_buffer.cpp (allowDirectBufferMapping
// picks the write path).
class MTLD3D9VertexBuffer final : public ComObject<IDirect3DVertexBuffer9> {
public:
  MTLD3D9VertexBuffer(
      MTLD3D9Device *device, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool, void *host_ptr,
      Rc<dxmt::Buffer> dxmt_buffer
  );
  ~MTLD3D9VertexBuffer();

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

  // IDirect3DVertexBuffer9
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE Unlock() override;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC *pDesc) override;

  // Metal buffer the GPU reads: the DynamicBuffer's current allocation
  // buffer (a Lock(DISCARD) renames it via updateImmediateName). Both map
  // modes route through the wrapper now, so the arm no longer branches.
  WMT::Buffer
  metalBuffer() const {
    return m_dynamic->immediateName()->buffer();
  }
  // GPU virtual address of the buffer the GPU reads; the manual-fetch VS
  // variant pulls vertex data through this pointer via the [[buffer(16)]]
  // vertex_buffers table, not through a [[buffer(N)]] binding. Read from
  // the DynamicBuffer's current allocation in either map mode.
  uint64_t
  gpuAddress() const {
    return m_dynamic->immediateName()->gpuAddress();
  }
  D3D9BufferMapMode
  mapMode() const {
    return m_mapMode;
  }
  // Current DynamicBuffer allocation (either map mode). The draw path
  // freezes this (handle, gpu_address, and the Rc) from ONE read so binding
  // and the Vertex-stage read the fence tracker registers stay on one
  // allocation even after a later Lock(DISCARD) rename: BUFFER orders the
  // staged upload against it, DIRECT fences its in-place backing.
  Rc<dxmt::BufferAllocation>
  immediateAllocation() const {
    return m_dynamic->immediateName();
  }
  // Copy any pending dirty range from the host mirror into the Private
  // buffer through the device upload path. No-op in DIRECT mode or with
  // an empty dirty range. Called by the outer Unlock and by draw
  // recording for a bound dirty buffer.
  void flushDirty();
  // Raw access to the owning device: same rationale as
  // MTLD3D9Surface / MTLD3D9Texture: SetStreamSource's cross-device
  // check needs identity, not a public ref, on a hot path.
  MTLD3D9Device *
  deviceRaw() const {
    return m_device;
  }
  UINT
  size() const {
    return m_size;
  }
  // FVF the buffer was created with (0 for a non-FVF buffer). ProcessVertices
  // reads it to synthesise the destination layout when the caller passes no
  // output declaration, matching DXVK (dst->Desc()->FVF).
  DWORD
  fvf() const {
    return m_fvf;
  }
  // Stamped by BuildDrawCapture with the open chunk's seq whenever this
  // buffer is captured into a draw. A DIRECT plain-map Lock compares the
  // stamp against the device's signaled floor to decide whether the GPU
  // may still read the current in-place allocation; see lockSyncLastGpuUse
  // in d3d9_buffer.cpp. BUFFER mode never consults it (its Lock writes a
  // mirror, never the GPU-read backing).
  void
  markPendingGpuUse(uint64_t seq) {
    m_lastUseSeq = seq;
  }

private:
  MTLD3D9Device *m_device;
  // Storage model, fixed at create time from pool + usage.
  D3D9BufferMapMode m_mapMode;
  // The dxmt::Buffer the DynamicBuffer wraps, held only to anchor the raw
  // Buffer pointer m_dynamic keeps (same role as d3d11's
  // D3D11Buffer::buffer_ behind its dynamic_); the current allocation and
  // the GPU-idle recycle FIFO live in m_dynamic. Its current() allocation
  // is CpuPlaced (DIRECT) or GpuPrivate (BUFFER). Declared before m_dynamic
  // so it outlives the wrapper's raw pointer at teardown.
  Rc<dxmt::Buffer> m_dxmtBuffer;
  // The DynamicBuffer recycling wrapper (the same one d3d11 uses,
  // d3d11_buffer.cpp:123). Owns the current allocation name and a FIFO of
  // retired allocations a Lock(DISCARD) recycles once the GPU has passed
  // them. DIRECT: the app writes immediateMappedMemory() in place and draws
  // read the same allocation. BUFFER: the staged copy uploads into the
  // current name and draws read it. Used in both modes.
  Rc<dxmt::DynamicBuffer> m_dynamic;
  // BUFFER mode: byte span the app's Locks have written into the host
  // mirror and a subsequent Unlock or draw must copy into m_dynamic's
  // current allocation. Empty in DIRECT mode (no mirror).
  D3D9BufferRange m_dirtyRange;
  // BUFFER mode: nested Lock/Unlock depth; the upload fires on the outer
  // Unlock only.
  D3D9BufferLockCount m_lockCount;
  // BUFFER mode: the process-owned host mirror, never registered with
  // Metal; the dtor frees it directly. Null in DIRECT mode, where the
  // lockable pointer is the DynamicBuffer's immediateMappedMemory() and
  // rotates on Lock(DISCARD).
  void *m_hostPtr;
  UINT m_size;
  DWORD m_usage;
  DWORD m_fvf;
  D3DPOOL m_pool;
  // Last chunk seq a draw captured this buffer at; consulted by a DIRECT
  // plain-map Lock only.
  uint64_t m_lastUseSeq = 0;
  DWORD m_priority = 0;
  // Same exactly-once-drop pattern as MTLD3D9Surface / MTLD3D9Texture:
  // the ctor self-pin must be released only on the FIRST pub→0
  // transition, otherwise a Get/Release cycle on a slot-pinned buffer
  // (m_vertexBuffers[N]) over-decrements priv and destructs.
  bool m_self_pinned = true;
  // Losable-resource accounting: see d3d9_surface.hpp.
  bool m_isLosable = false;

public:
  void markLosable();

private:
  ComPrivateData m_privateData;
};

// IDirect3DIndexBuffer9: same lifetime / pool / storage shape as
// MTLD3D9VertexBuffer; the only meaningful differences are the
// D3DFORMAT (D3DFMT_INDEX16 / D3DFMT_INDEX32) instead of FVF, the
// resource type, and the descriptor struct.
class MTLD3D9IndexBuffer final : public ComObject<IDirect3DIndexBuffer9> {
public:
  MTLD3D9IndexBuffer(
      MTLD3D9Device *device, UINT size, DWORD usage, D3DFORMAT format, D3DPOOL pool, void *host_ptr,
      Rc<dxmt::Buffer> dxmt_buffer
  );
  ~MTLD3D9IndexBuffer();

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

  // IDirect3DIndexBuffer9
  HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void **ppbData, DWORD Flags) override;
  HRESULT STDMETHODCALLTYPE Unlock() override;
  HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC *pDesc) override;

  // See MTLD3D9VertexBuffer::metalBuffer.
  WMT::Buffer
  metalBuffer() const {
    return m_dynamic->immediateName()->buffer();
  }
  // Byte offset of the current allocation within metalBuffer(). Always 0:
  // each DynamicBuffer allocation is its own MTLBuffer starting at 0. Kept
  // for caller compatibility (BuildDrawCapture and the index fan-remap path).
  uint64_t
  currentOffset() const {
    return 0;
  }
  D3DFORMAT
  indexFormat() const {
    return m_format;
  }
  D3D9BufferMapMode
  mapMode() const {
    return m_mapMode;
  }
  // See MTLD3D9VertexBuffer::immediateAllocation.
  Rc<dxmt::BufferAllocation>
  immediateAllocation() const {
    return m_dynamic->immediateName();
  }
  // See MTLD3D9VertexBuffer::flushDirty.
  void flushDirty();
  // Host-mapped pointer to the current index data. DIRECT: the DynamicBuffer's
  // current in-place allocation (immediateMappedMemory, which rotates on
  // Lock(DISCARD)). BUFFER: the host mirror. The index fan-remap path in
  // d3d9_device.cpp reads the source indices through this pointer to remap
  // them; callers must null-check.
  const void *
  hostPointer() const {
    return m_mapMode == D3D9BufferMapMode::Direct ? m_dynamic->immediateMappedMemory() : m_hostPtr;
  }
  MTLD3D9Device *
  deviceRaw() const {
    return m_device;
  }
  UINT
  size() const {
    return m_size;
  }
  // See MTLD3D9VertexBuffer::markPendingGpuUse.
  void
  markPendingGpuUse(uint64_t seq) {
    m_lastUseSeq = seq;
  }

private:
  MTLD3D9Device *m_device;
  D3D9BufferMapMode m_mapMode;
  // See MTLD3D9VertexBuffer::m_dxmtBuffer / m_dynamic / m_hostPtr for the
  // per-map-mode lifecycle. m_dxmtBuffer is declared before m_dynamic so
  // it outlives the wrapper's raw Buffer pointer at teardown.
  Rc<dxmt::Buffer> m_dxmtBuffer;
  Rc<dxmt::DynamicBuffer> m_dynamic;
  void *m_hostPtr;
  D3D9BufferRange m_dirtyRange;
  D3D9BufferLockCount m_lockCount;
  UINT m_size;
  DWORD m_usage;
  D3DFORMAT m_format;
  D3DPOOL m_pool;
  // Last chunk seq a draw captured this buffer at; consulted by a DIRECT
  // plain-map Lock only.
  uint64_t m_lastUseSeq = 0;
  DWORD m_priority = 0;
  // See MTLD3D9VertexBuffer::m_self_pinned for the rationale.
  bool m_self_pinned = true;
  // Losable-resource accounting: see d3d9_surface.hpp.
  bool m_isLosable = false;

public:
  void markLosable();

private:
  ComPrivateData m_privateData;
};

} // namespace dxmt
