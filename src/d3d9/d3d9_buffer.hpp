#pragma once

#include "Metal.hpp"

#include <vector>
#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "com/com_private_data.hpp"
#include "d3d9.h"

namespace dxmt {

class MTLD3D9Device;

// IDirect3DVertexBuffer9 backed by a single WMT::Buffer. No sub-
// resource children — the lifetime is simpler than MTLD3D9Texture
// (standalone-surface shape: self-pin in ctor, AddRef/Release pin
// the device).
//
// References (vtable shape, Lock/Unlock semantics, pool/usage gating):
// wined3d dlls/d3d9/buffer.c d3d9_vertexbuffer_*. MGL has no GL→Metal
// equivalent — Metal buffers are first-class and the storage-mode
// mapping is the same one we already use for the CPU pools in the
// surface paths (Shared on Apple Silicon's unified memory).
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
  // newBuffer time — the manual-fetch VS variant pulls vertex data
  // through this pointer via the [[buffer(16)]] vertex_buffers table,
  // not through a [[buffer(N)]] binding. Same shape as
  // dxmt_buffer.cpp gpuAddress_.
  uint64_t
  gpuAddress() const {
    return m_gpuAddress;
  }
  // Raw access to the owning device — same rationale as
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

private:
  MTLD3D9Device *m_device;
  // Active backing — the one a Lock returns and the GPU reads on the
  // next draw. For static buffers (no D3DUSAGE_DYNAMIC) this is the
  // only backing for the lifetime of the buffer; m_retiredBackings
  // stays empty. For DYNAMIC buffers, DISCARD-Lock rotates: the
  // current active is pushed into m_retiredBackings tagged with the
  // device's current m_currentCmdSeq, and either a reusable retired
  // entry (last_used_seq <= signaled coherent_id) is popped back as
  // the new active, or a fresh MTLBuffer + host backing is allocated.
  // This is wined3d's wined3d_cs_map_upload_bo + adapter_alloc_bo
  // shape (cs.c:3185-3193): a brand-new BO per DISCARD with the old
  // one alive through prior cmdbuf references — within-cmdbuf wrap
  // is physically impossible because we never write into a region
  // the GPU is still reading.
  WMT::Reference<WMT::Buffer> m_buffer;
  uint64_t m_gpuAddress;
  // Host-mapped pointer for Shared / Managed storage modes; null for
  // Private (D3DPOOL_DEFAULT static buffers — those need a staging
  // upload path that lands later).
  void *m_hostPtr;
  // Process-allocated backing for newBufferWithBytesNoCopy. dxmt
  // pre-allocates the storage via wsi::aligned_malloc and hands it
  // to Metal so the lockable host pointer always lives in the
  // calling process's <4 GB address space — without the placement,
  // Metal returns a high-memory pointer that 32-bit Windows games
  // cannot reach (project_wow64_abi_gotchas memory). Owned by this
  // object; dtor frees via wsi::aligned_free.
  void *m_ownedBacking = nullptr;
  // Retire pool — see m_buffer comment for the wined3d-shaped
  // lifecycle. Each entry owns its own WMT::Reference<WMT::Buffer>
  // and wsi::aligned_malloc'd backing. last_used_seq is set at the
  // moment the entry was retired (== m_device->m_currentCmdSeq at
  // that point), which is a conservative upper bound on the latest
  // cmdbuf that could still hold a GPU read against this region.
  // Reusable once m_device->m_cachedSignaled >= last_used_seq.
  struct DynamicBackingEntry {
    WMT::Reference<WMT::Buffer> mtl_buffer;
    void *owned_backing;
    void *host_ptr;
    uint64_t gpu_address;
    uint64_t last_used_seq;
  };
  std::vector<DynamicBackingEntry> m_retiredBackings;
  // Allocate a fresh MTLBuffer + wsi backing of m_size bytes; returns
  // false on OOM. Used both during DISCARD-Lock (fresh path) and on
  // construction (initial active backing).
  bool
  allocateFreshBacking(WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned);
  UINT m_size;
  DWORD m_usage;
  DWORD m_fvf;
  D3DPOOL m_pool;
  DWORD m_priority = 0;
  // Same exactly-once-drop pattern as MTLD3D9Surface / MTLD3D9Texture:
  // the ctor self-pin must be released only on the FIRST pub→0
  // transition, otherwise a Get/Release cycle on a slot-pinned buffer
  // (m_vertexBuffers[N]) over-decrements priv and destructs.
  bool m_self_pinned = true;
  // Losable-resource accounting — see d3d9_surface.hpp.
  bool m_isLosable = false;

public:
  void markLosable();

private:
  ComPrivateData m_privateData;
};

// IDirect3DIndexBuffer9 — same lifetime / pool / storage shape as
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
  // 0 under the per-DISCARD-fresh-backing design — each backing is
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

private:
  MTLD3D9Device *m_device;
  // See MTLD3D9VertexBuffer::m_buffer / m_retiredBackings for the
  // wined3d-shaped per-DISCARD-fresh-backing lifecycle.
  WMT::Reference<WMT::Buffer> m_buffer;
  void *m_hostPtr;
  void *m_ownedBacking = nullptr;
  uint64_t m_gpuAddress = 0;
  struct DynamicBackingEntry {
    WMT::Reference<WMT::Buffer> mtl_buffer;
    void *owned_backing;
    void *host_ptr;
    uint64_t gpu_address;
    uint64_t last_used_seq;
  };
  std::vector<DynamicBackingEntry> m_retiredBackings;
  bool
  allocateFreshBacking(WMT::Reference<WMT::Buffer> &out_buffer, uint64_t &out_gpu, void *&out_host, void *&out_owned);
  UINT m_size;
  DWORD m_usage;
  D3DFORMAT m_format;
  D3DPOOL m_pool;
  DWORD m_priority = 0;
  // See MTLD3D9VertexBuffer::m_self_pinned for the rationale.
  bool m_self_pinned = true;
  // Losable-resource accounting — see d3d9_surface.hpp.
  bool m_isLosable = false;

public:
  void markLosable();

private:
  ComPrivateData m_privateData;
};

} // namespace dxmt
