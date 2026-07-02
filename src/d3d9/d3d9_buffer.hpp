#pragma once

#include "Metal.hpp"

#include <vector>
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"
#include "d3d9_buffer_map.hpp"
#include "dxmt_buffer.hpp"

namespace dxmt {

class MTLD3D9Device;

// One retired backing in a DIRECT-mode buffer's rename ring. last_used_seq
// is m_device->m_currentCmdSeq at retire time: a conservative upper
// bound on the latest cmdbuf that could still hold a GPU read against
// this region. Reusable once m_device->m_cachedSignaled >= last_used_seq.
// Shared between MTLD3D9VertexBuffer / MTLD3D9IndexBuffer so the
// rename-ring helper (lockDiscardRotate in d3d9_buffer.cpp) can name
// one type.
struct BufferBackingEntry {
  WMT::Reference<WMT::Buffer> mtl_buffer;
  void *owned_backing;
  void *host_ptr;
  uint64_t gpu_address;
  uint64_t last_used_seq;
};

// IDirect3DVertexBuffer9 backed by a per-map-mode storage model (see
// d3d9_buffer_map.hpp). DIRECT (DEFAULT + DYNAMIC): the app writes a
// placed Shared MTLBuffer the GPU reads in place, with a DISCARD rename
// ring. BUFFER (every other pool/usage): the app writes a host mirror
// and Unlock copies the dirty range into a GPU-only Private buffer, so a
// Lock never waits on the GPU. No sub-resources; standalone shape
// (self-pin in ctor, AddRef/Release pin device). References: wined3d
// buffer.c, DXVK d3d9_common_buffer.cpp.
class MTLD3D9VertexBuffer final : public ComObject<IDirect3DVertexBuffer9> {
public:
  MTLD3D9VertexBuffer(
      MTLD3D9Device *device, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool, WMT::Reference<WMT::Buffer> buffer,
      uint64_t gpu_address, void *host_ptr, void *owned_backing, Rc<dxmt::Buffer> dxmt_buffer
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

  // Metal buffer the GPU reads. DIRECT: the placed Shared backing.
  // BUFFER: the tracked Private allocation's buffer (never renames in
  // this mode, so current() is stable).
  WMT::Buffer
  metalBuffer() const {
    if (m_mapMode == D3D9BufferMapMode::Buffer)
      return m_dxmtBuffer->current()->buffer();
    return m_buffer;
  }
  // GPU virtual address of the buffer the GPU reads; the manual-fetch VS
  // variant pulls vertex data through this pointer via the [[buffer(16)]]
  // vertex_buffers table, not through a [[buffer(N)]] binding. BUFFER
  // mode reads it from the tracked allocation's current storage.
  uint64_t
  gpuAddress() const {
    if (m_mapMode == D3D9BufferMapMode::Buffer)
      return m_dxmtBuffer->current()->gpuAddress();
    return m_gpuAddress;
  }
  D3D9BufferMapMode
  mapMode() const {
    return m_mapMode;
  }
  // Tracked Private allocation for a BUFFER-mode buffer; null for DIRECT.
  // The draw path registers a Vertex-stage read access against it so the
  // fence tracker orders the staged upload copy ahead of the draw.
  const Rc<dxmt::Buffer> &
  dxmtBuffer() const {
    return m_dxmtBuffer;
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
  // Stamped by BuildDrawCapture with the open chunk's seq whenever this
  // DIRECT-mode buffer is captured into a draw. Lock's plain-map path
  // compares the stamp against the device's signaled floor to decide
  // whether the GPU may still read the active backing; see
  // lockSyncLastGpuUse in d3d9_buffer.cpp. BUFFER mode never consults it
  // (its Lock never touches the GPU-read backing).
  void
  markPendingGpuUse(uint64_t seq) {
    m_lastUseSeq = seq;
  }

private:
  MTLD3D9Device *m_device;
  // Storage model, fixed at create time from pool + usage.
  D3D9BufferMapMode m_mapMode;
  // DIRECT mode: active backing a Lock returns and the GPU reads next
  // draw. Static buffers keep only this one; DYNAMIC buffers rotate it on
  // a DISCARD-Lock, pushing the current into m_retiredBackings tagged with
  // m_currentCmdSeq and popping a reusable retired entry or allocating a
  // fresh MTLBuffer + host backing. Wined3d-shaped: brand-new BO per
  // DISCARD, old alive through prior cmdbuf references. Null in BUFFER
  // mode.
  WMT::Reference<WMT::Buffer> m_buffer;
  uint64_t m_gpuAddress = 0;
  // DIRECT mode: process-allocated backing handed to Metal via
  // newBufferWithBytesNoCopy. dxmt pre-allocates the storage via
  // wsi::aligned_malloc so the lockable host pointer always lives in the
  // calling process's <4 GB address space; without the placement, Metal
  // can return a high-memory pointer that 32-bit Windows games cannot
  // reach. Owned by this object; dtor frees it via releaseBufferBacking.
  // Null in BUFFER mode.
  void *m_ownedBacking = nullptr;
  // DIRECT mode retire pool: see m_buffer for the wined3d-shaped
  // lifecycle. Each entry owns its own WMT::Reference<WMT::Buffer> and
  // wsi::aligned_malloc'd backing. Empty in BUFFER mode.
  std::vector<BufferBackingEntry> m_retiredBackings;
  // BUFFER mode: GPU-only Private allocation the staged copy uploads into
  // and draws read; tracked, so the fence tracker orders the copy against
  // draws via the read/write access() calls. Null in DIRECT mode.
  Rc<dxmt::Buffer> m_dxmtBuffer;
  // BUFFER mode: byte span the app's Locks have written into the host
  // mirror and a subsequent Unlock or draw must copy into m_dxmtBuffer.
  D3D9BufferRange m_dirtyRange;
  // BUFFER mode: nested Lock/Unlock depth; the upload fires on the outer
  // Unlock only.
  D3D9BufferLockCount m_lockCount;
  // Lockable host pointer. DIRECT: the placed backing Metal also reads
  // (aliases m_ownedBacking). BUFFER: the process-owned mirror, never
  // registered with Metal; the dtor frees it directly.
  void *m_hostPtr;
  // Allocate a fresh MTLBuffer + wsi backing of m_size bytes (DIRECT
  // mode); returns false on OOM. Used both during DISCARD-Lock (fresh
  // path) and on construction (initial active backing).
  bool
  allocateFreshBacking(WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned);
  UINT m_size;
  DWORD m_usage;
  DWORD m_fvf;
  D3DPOOL m_pool;
  // Last chunk seq a draw captured this buffer at; DIRECT mode only.
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
      MTLD3D9Device *device, UINT size, DWORD usage, D3DFORMAT format, D3DPOOL pool, WMT::Reference<WMT::Buffer> buffer,
      uint64_t gpu_address, void *host_ptr, void *owned_backing, Rc<dxmt::Buffer> dxmt_buffer
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
    if (m_mapMode == D3D9BufferMapMode::Buffer)
      return m_dxmtBuffer->current()->buffer();
    return m_buffer;
  }
  // Byte offset of the active backing within metalBuffer(). Always 0:
  // DIRECT backings are each their own MTLBuffer that starts at 0, and
  // BUFFER mode has one Private allocation at 0. Kept for caller
  // compatibility (BuildDrawCapture and the index fan-remap path).
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
  // See MTLD3D9VertexBuffer::dxmtBuffer.
  const Rc<dxmt::Buffer> &
  dxmtBuffer() const {
    return m_dxmtBuffer;
  }
  // See MTLD3D9VertexBuffer::flushDirty.
  void flushDirty();
  // Host-mapped pointer to the current index data. DIRECT: the placed
  // backing's current rename-ring slot. BUFFER: the host mirror. The
  // index fan-remap path in d3d9_device.cpp reads the source indices
  // through this pointer to remap them; callers must null-check.
  const void *
  hostPointer() const {
    return m_hostPtr;
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
  // See MTLD3D9VertexBuffer::m_buffer / m_retiredBackings / m_dxmtBuffer
  // for the per-map-mode lifecycle.
  WMT::Reference<WMT::Buffer> m_buffer;
  void *m_hostPtr;
  void *m_ownedBacking = nullptr;
  uint64_t m_gpuAddress = 0;
  std::vector<BufferBackingEntry> m_retiredBackings;
  Rc<dxmt::Buffer> m_dxmtBuffer;
  D3D9BufferRange m_dirtyRange;
  D3D9BufferLockCount m_lockCount;
  bool
  allocateFreshBacking(WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned);
  UINT m_size;
  DWORD m_usage;
  D3DFORMAT m_format;
  D3DPOOL m_pool;
  // Last chunk seq a draw captured this buffer at; DIRECT mode only.
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
