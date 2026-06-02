#pragma once

#include "Metal.hpp"

#include <vector>
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"

namespace dxmt {

class MTLD3D9Device;

// One retired backing in a DYNAMIC buffer's rename ring. last_used_seq
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

// IDirect3DVertexBuffer9 backed by WMT::Buffer. No sub-resources;
// standalone shape (self-pin in ctor, AddRef/Release pin device).
// References: wined3d buffer.c.
class MTLD3D9VertexBuffer final : public ComObject<IDirect3DVertexBuffer9> {
public:
  MTLD3D9VertexBuffer(
      MTLD3D9Device *device, UINT size, DWORD usage, DWORD fvf, D3DPOOL pool, WMT::Reference<WMT::Buffer> buffer,
      uint64_t gpu_address, void *host_ptr, void *owned_backing
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

  WMT::Buffer
  metalBuffer() const {
    return m_buffer;
  }
  // GPU virtual address captured from WMTBufferInfo.gpu_address at
  // newBuffer time; the manual-fetch VS variant pulls vertex data
  // through this pointer via the [[buffer(16)]] vertex_buffers table,
  // not through a [[buffer(N)]] binding. Same shape as
  // dxmt_buffer.cpp gpuAddress_.
  uint64_t
  gpuAddress() const {
    return m_gpuAddress;
  }
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
  // Stamped by BuildDrawCapture with the open chunk's seq whenever
  // this buffer is captured into a draw. Lock's plain-map path
  // compares the stamp against the device's signaled floor to decide
  // whether the GPU may still read the active backing; see
  // lockSyncLastGpuUse in d3d9_buffer.cpp.
  void
  markPendingGpuUse(uint64_t seq) {
    m_lastUseSeq = seq;
  }

private:
  MTLD3D9Device *m_device;
  // Active backing: one a Lock returns and GPU reads next draw.
  // Static buffers: only backing; m_retiredBackings empty. DYNAMIC
  // buffers: DISCARD-Lock rotates; current pushed into m_retiredBackings
  // tagged with m_currentCmdSeq; reusable retired entry popped back, or
  // fresh MTLBuffer+host backing allocated. Wined3d-shaped: brand-new
  // BO per DISCARD, old alive through prior cmdbuf references.
  WMT::Reference<WMT::Buffer> m_buffer;
  uint64_t m_gpuAddress;
  // Host-mapped pointer for Shared / Managed storage modes; null for
  // Private (D3DPOOL_DEFAULT static buffers, those need a staging
  // upload path that lands later).
  void *m_hostPtr;
  // Process-allocated backing for newBufferWithBytesNoCopy. dxmt
  // pre-allocates the storage via wsi::aligned_malloc and hands it
  // to Metal so the lockable host pointer always lives in the
  // calling process's <4 GB address space; without the placement,
  // Metal can return a high-memory pointer that 32-bit Windows games
  // cannot reach. Owned by this object; dtor frees via wsi::aligned_free.
  void *m_ownedBacking = nullptr;
  // Retire pool: see m_buffer comment for the wined3d-shaped
  // lifecycle. Each entry owns its own WMT::Reference<WMT::Buffer>
  // and wsi::aligned_malloc'd backing.
  std::vector<BufferBackingEntry> m_retiredBackings;
  // Allocate a fresh MTLBuffer + wsi backing of m_size bytes; returns
  // false on OOM. Used both during DISCARD-Lock (fresh path) and on
  // construction (initial active backing).
  bool
  allocateFreshBacking(WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned);
  UINT m_size;
  DWORD m_usage;
  DWORD m_fvf;
  D3DPOOL m_pool;
  // Last chunk seq a draw captured this buffer at; see markPendingGpuUse.
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
      uint64_t gpu_address, void *host_ptr, void *owned_backing
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

  WMT::Buffer
  metalBuffer() const {
    return m_buffer;
  }
  // Byte offset of the active backing within metalBuffer(). Always
  // 0 under the per-DISCARD-fresh-backing design; each backing is
  // its own MTLBuffer that starts at 0. Kept for caller compatibility
  // (BuildDrawCapture, drawCommonInScene fan path).
  uint64_t
  currentOffset() const {
    return 0;
  }
  D3DFORMAT
  indexFormat() const {
    return m_format;
  }
  // Host-mapped pointer to the CURRENT rename-ring slot; null for
  // pool/usage combinations that have no host backing (a future
  // D3DPOOL_DEFAULT static IB without a sysmem mirror). The fan-
  // emulation path in drawCommonInScene reads the source indices
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
  // See MTLD3D9VertexBuffer::m_buffer / m_retiredBackings for the
  // wined3d-shaped per-DISCARD-fresh-backing lifecycle.
  WMT::Reference<WMT::Buffer> m_buffer;
  void *m_hostPtr;
  void *m_ownedBacking = nullptr;
  uint64_t m_gpuAddress = 0;
  std::vector<BufferBackingEntry> m_retiredBackings;
  bool
  allocateFreshBacking(WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned);
  UINT m_size;
  DWORD m_usage;
  D3DFORMAT m_format;
  D3DPOOL m_pool;
  // Last chunk seq a draw captured this buffer at; see markPendingGpuUse.
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
