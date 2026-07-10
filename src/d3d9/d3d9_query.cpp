#include "d3d9_query.hpp"

#include "d3d9_device.hpp"
#include "d3d9_query_contract.hpp"
#include "d3d9_stall.hpp"
#include "dxmt_command_queue.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace dxmt {

MTLD3D9Query::MTLD3D9Query(MTLD3D9Device *device, D3DQUERYTYPE type) : m_device(device), m_type(type) {
  AddRefPrivate();
}

MTLD3D9Query::~MTLD3D9Query() = default;

void
MTLD3D9Query::endOcclusionIfActive() {
  if (m_type != D3DQUERYTYPE_OCCLUSION || !m_visibility_query || m_ended)
    return;
  m_device->FlushDrawBatch();
  auto &queue = m_device->dxmtQueue();
  auto *chunk = queue.CurrentChunk();
  chunk->emitcc([query = m_visibility_query](ArgumentEncodingContext &ctx) mutable {
    ctx.endVisibilityResultQuery(std::move(query));
  });
  m_event_seq = queue.CurrentSeqId();
  m_ended = true;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Query::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9Query::Release() {
  // D3D9 Release-at-0 clamp: handed out at public 0 while self-pinned / bound
  // (DXVK clamps every device child); guard the underflow before the decrement.
  if (m_refCount.load() == 0)
    return 0;
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    // App is releasing the query without first issuing END. Synthesize
    // the End BEFORE the device public-refcount drop below, because
    // m_device may be torn down before this query's dtor runs (its
    // own priv ref to us only releases inside the device dtor); by
    // then m_device->FlushDrawBatch / dxmtQueue would UAF.
    {
      // endOcclusionIfActive mutates device draw-batch + queue state, unlike
      // the rest of this Release (atomic-only), so it must hold the device lock
      // under D3DCREATE_MULTITHREADED or it races a concurrent draw thread.
      // Scoped so the lock is dropped before the device pub-ref release below,
      // which on the last ref tears the device (and its lock) down.
      D9DeviceLock lock = m_device->LockDevice();
      endOcclusionIfActive();
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
MTLD3D9Query::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DQuery9)) {
    *ppvObject = static_cast<IDirect3DQuery9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::GetDevice(IDirect3DDevice9 **ppDevice) {
  D9DeviceLock lock = m_device->LockDevice();
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

D3DQUERYTYPE STDMETHODCALLTYPE
MTLD3D9Query::GetType() {
  D9DeviceLock lock = m_device->LockDevice();
  return m_type;
}

DWORD STDMETHODCALLTYPE
MTLD3D9Query::GetDataSize() {
  D9DeviceLock lock = m_device->LockDevice();
  // Pure per-type table, host-pinned in test_query.cpp. Getting these wrong
  // corrupts memory when the app passes a buffer sized to the documented type.
  return d3d9_query_data_size(m_type);
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::Issue(DWORD dwIssueFlags) {
  D9DeviceLock lock = m_device->LockDevice();
  d9NotePoll(g_d9stall.issue_count);
  // D3DISSUE_BEGIN starts a query (only OCCLUSION uses BEGIN; EVENT
  // and TIMESTAMP are END-only). D3DISSUE_END signals the GPU to
  // capture the current value. wined3d query.c d3d9_query_Issue
  // accepts both flags as a no-op on unsupported types.
  if (dwIssueFlags & D3DISSUE_BEGIN) {
    if (m_type == D3DQUERYTYPE_OCCLUSION) {
      // Begin-after-Begin: drain the in-flight query before starting a new one.
      // Without this, previous query leaks in pending_queries_ (seq_id_end stays
      // ~0uLL so erase-if never matches) and visibility slots burn.
      // d3d11 query.cpp documents the same hazard.
      endOcclusionIfActive();

      // Fresh VisibilityResultQuery on each Begin. Must FlushDrawBatch before emitcc:
      // d3d9 batches draws on calling-thread; FIFO becomes [pre-Begin batch → begin → subsequent batches].
      // If begin emitted before flush: [begin → end → draws-bundle] coalesces begin==end to same slot,
      // GPU counter never increments. d3d11 has no batching layer, so doesn't need this.
      m_device->FlushDrawBatch();
      m_visibility_query = new VisibilityResultQuery();
      auto &queue = m_device->dxmtQueue();
      auto *chunk = queue.CurrentChunk();
      chunk->emitcc([query = m_visibility_query](ArgumentEncodingContext &ctx) mutable {
        ctx.beginVisibilityResultQuery(std::move(query));
      });
    }
    // wined3d sets state = QUERY_BUILDING on any BEGIN regardless of type, so
    // GetData reports S_FALSE until the matching END. Only OCCLUSION opens a
    // GPU counter above; for the other types BEGIN is just the state move.
    m_began = true;
    m_ended = false;
  }
  if (dwIssueFlags & D3DISSUE_END) {
    if (m_type == D3DQUERYTYPE_OCCLUSION && m_visibility_query && !m_ended) {
      // Same drain shape as Begin; flush the in-window batch into
      // the chunk before the end lambda lands, otherwise the visibility
      // window collapses to zero draws.
      m_device->FlushDrawBatch();
      auto &queue = m_device->dxmtQueue();
      auto *chunk = queue.CurrentChunk();
      chunk->emitcc([query = m_visibility_query](ArgumentEncodingContext &ctx) mutable {
        ctx.endVisibilityResultQuery(std::move(query));
      });
      // Snapshot the chunk-in-flight seq id so GetData(FLUSH) knows
      // whether the issuing chunk has been committed yet. The
      // VisibilityResultQuery's getValue() handles the GPU-completion
      // check itself via the readback issue path.
      m_event_seq = queue.CurrentSeqId();
    }
    m_ended = true;
    if (m_type == D3DQUERYTYPE_EVENT) {
      // Snapshot the chunk-in-flight's seq id. All prior calling-thread
      // work landed on this chunk (or earlier); once cpu_coherent
      // advances past m_event_seq, the GPU has completed the work the
      // app is waiting on. dxmt's queue advances ready_for_encode on
      // CommitCurrentChunk and cpu_coherent on GPU-retire; matches the
      // DXVK DxvkGpuEvent + d3d11's EventQuery shape.
      m_event_seq = m_device->dxmtQueue().CurrentSeqId();
    }
    if (m_type == D3DQUERYTYPE_TIMESTAMP) {
      // Host-side approximation: monotonic ns since epoch. GetData
      // returns this raw value; TIMESTAMPFREQ reports 1 GHz so apps
      // computing (end - start) / freq see seconds-elapsed directly.
      m_timestamp_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
    }
  }
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9Query::GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
  D9DeviceLock lock = m_device->LockDevice();
  // Thin instrument wrapper: count the poll and record the API event once at
  // entry, and the S_FALSE (not-ready) return once at exit, so the six
  // not-ready return paths below need no per-site edits. The behaviour is
  // exactly getDataImpl's.
  d9NotePoll(g_d9stall.getdata_count);
  d9NoteApiEvent();
  HRESULT hr = getDataImpl(pData, dwSize, dwGetDataFlags);
  if (hr == S_FALSE)
    d9NotePoll(g_d9stall.getdata_false_count);
  return hr;
}

HRESULT
MTLD3D9Query::getDataImpl(void *pData, DWORD dwSize, DWORD dwGetDataFlags) {
  const DWORD data_size = GetDataSize();

  // wined3d query.c state machine, surfaced through dlls/d3d9 query.c GetData.
  // QUERY_BUILDING (BEGIN issued, END pending): the result window is still
  // open, so the value is not yet available: S_FALSE.
  if (m_began && !m_ended)
    return S_FALSE;

  // QUERY_CREATED (never issued END): wined3d returns INVALIDCALL, which the
  // d3d9 wrapper rewrites to S_OK after poisoning the caller buffer: zero the
  // whole span, then 0xdd the leading data_size bytes. An app polling a freshly
  // created query must observe S_OK, not a fatal INVALIDCALL.
  if (!m_ended) {
    if (pData)
      d3d9_poison_created_query(pData, dwSize, data_size);
    return S_OK;
  }

  // QUERY_SIGNALLED: END issued. Per-type readiness + result copy follow.
  // pData=NULL probes readiness; forward-commit EVENT/OCCLUSION regardless of
  // D3DGETDATA_FLUSH to prevent spin-loop deadlock.

  // EVENT queries are now backed by the queue's coherent-seq counter.
  // Commit the recorded chunk whenever it hasn't submitted yet (event_seq
  // still == CurrentSeqId), regardless of D3DGETDATA_FLUSH: an app that issues
  // an EVENT then immediately polls without the flag (and does no other GPU
  // work) would otherwise spin forever on a chunk that never submits. The
  // readiness model mirrors DXVK's DxvkGpuEvent::test; the unconditional submit
  // is dxmt's own (DXVK's forward-progress submit is FLUSH-gated, but its CS
  // thread submits independently so a non-FLUSH spin-poll cannot deadlock it).
  if (m_type == D3DQUERYTYPE_EVENT && m_event_seq != 0) {
    auto &queue = m_device->dxmtQueue();
    // Submit issuing chunk for forward progress regardless of FLUSH flag.
    // Apps polling without D3DGETDATA_FLUSH would deadlock: chunk never
    // submits, 100% CPU spin with encode/finish threads idle. D3D9 contract.
    if (queue.CurrentSeqId() == m_event_seq) {
      m_device->FlushDrawBatch();
      m_device->commitCurrentChunkTimed();
    }
    if (queue.CoherentSeqId() < m_event_seq) {
      // Caller polling readiness with no-buffer call: S_FALSE on
      // still-pending matches the per-spec contract.
      if (pData == nullptr || dwSize == 0)
        return S_FALSE;
      BOOL signaled = FALSE;
      std::memcpy(pData, &signaled, dwSize < sizeof(signaled) ? dwSize : sizeof(signaled));
      return S_FALSE;
    }
  }

  // OCCLUSION FLUSH + readiness gate: if the issuing chunk hasn't
  // been committed yet, commit it so the GPU has a chance to retire
  // the visibility-result readback. Without this an app that issues
  // an occlusion query and immediately polls; without doing any
  // other GPU work; would spin forever waiting for the chunk that
  // never committed. Same shape as the EVENT flush above. The
  // readiness probe (pData==null) returns S_FALSE when the query
  // isn't done: checked before the null-buffer short-circuit below.
  if (m_type == D3DQUERYTYPE_OCCLUSION && m_visibility_query) {
    auto &queue = m_device->dxmtQueue();
    // Same forward-progress fix as the EVENT path above: commit the issuing
    // chunk regardless of D3DGETDATA_FLUSH so a spin-poll without the flag
    // can't deadlock on a chunk that never submits. Flush first, like the
    // EVENT arm: draws queued AFTER Issue(END) (the standard cull loop keeps
    // drawing before it polls) are still pending with pod snapshots on the
    // current chunk. Committing without draining them retires that chunk while
    // those snapshots are live, and the encode thread reads freed ring memory
    // when it resolves the draws on the next chunk.
    if (queue.CurrentSeqId() == m_event_seq) {
      m_device->FlushDrawBatch();
      m_device->commitCurrentChunkTimed();
    }
    uint64_t probe = 0;
    if (!m_visibility_query->getValue(&probe)) {
      if (pData == nullptr || dwSize == 0)
        return S_FALSE;
    }
  }

  // Caller can pass pData=null + dwSize=0 to poll readiness without
  // copying the result. D3D_OK means "result is available". Real
  // backed queries would return S_FALSE while the GPU is still
  // running; the stub is always-ready for non-EVENT (we have no async
  // work).
  if (pData == nullptr || dwSize == 0)
    return D3D_OK;

  // Same per-type table as GetDataSize. Copy the smaller of dwSize
  // and the type's size; wined3d truncates if the app passes a
  // smaller buffer.
  switch (m_type) {
  case D3DQUERYTYPE_OCCLUSION: {
    // GPU-backed sample count via MTLVisibilityResultMode. The
    // VisibilityResultQuery accumulates the counter across all encoders the
    // begin/end straddles; getValue returns true once the issuing chunk's
    // readback has fired. wined3d stores the count as a uint64 and GetData
    // copies the full width (min(size, 8)); GetDataSize still reports the
    // documented sizeof(DWORD), so an app reading 8 bytes sees the high dword.
    uint64_t pixels64 = 0;
    if (m_visibility_query && !m_visibility_query->getValue(&pixels64)) {
      // Not ready yet. FLUSH already committed above; spin-poll is the app's
      // responsibility per the D3D9 contract. Leave the caller's buffer
      // untouched (wined3d and DXVK write only on the S_OK path): an app that
      // reuses last frame's pixel count on S_FALSE, a common conservative-cull
      // pattern, must not see a spurious 0. pData is non-null here (the null /
      // zero-size poll returned above).
      return S_FALSE;
    }
    std::memcpy(pData, &pixels64, std::min<DWORD>(dwSize, sizeof(pixels64)));
    return D3D_OK;
  }
  case D3DQUERYTYPE_EVENT: {
    BOOL signaled = TRUE;
    std::memcpy(pData, &signaled, dwSize < sizeof(signaled) ? dwSize : sizeof(signaled));
    return D3D_OK;
  }
  case D3DQUERYTYPE_TIMESTAMP: {
    // Host-side monotonic ns capture from Issue(END). Real GPU
    // timestamps via MTLCounterSampleBuffer are a follow-up; the
    // host-side delta is within a few-ms of GPU delta for typical
    // frame-paced work; apps use this for profiling overlays and
    // FPS counters which tolerate the imprecision.
    UINT64 ticks = m_timestamp_ns;
    std::memcpy(pData, &ticks, dwSize < sizeof(ticks) ? dwSize : sizeof(ticks));
    return D3D_OK;
  }
  case D3DQUERYTYPE_TIMESTAMPDISJOINT: {
    // Host steady_clock is monotonic + non-disjoint by construction;
    // FALSE always. Real GPU timestamps could report TRUE if the GPU
    // clock skipped (power-state transitions, etc.); not relevant on
    // the host-side path.
    BOOL disjoint = FALSE;
    std::memcpy(pData, &disjoint, dwSize < sizeof(disjoint) ? dwSize : sizeof(disjoint));
    return D3D_OK;
  }
  case D3DQUERYTYPE_TIMESTAMPFREQ: {
    // 1 GHz so TIMESTAMP values are interpretable as nanoseconds.
    // wined3d uses the same convention for software-timestamped paths.
    UINT64 freq = 1000000000ull;
    std::memcpy(pData, &freq, dwSize < sizeof(freq) ? dwSize : sizeof(freq));
    return D3D_OK;
  }
  default:
    return D3DERR_INVALIDCALL;
  }
}

} // namespace dxmt
