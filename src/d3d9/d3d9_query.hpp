#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "dxmt_occlusion_query.hpp"
#include "rc/util_rc_ptr.hpp"

namespace dxmt {

class MTLD3D9Device;

// IDirect3DQuery9 — wined3d dlls/d3d9/query.c d3d9_query_init.
// D3D9 covers a dozen query types (OCCLUSION, EVENT, TIMESTAMP,
// VCACHE, RESOURCEMANAGER, etc.). The two that actually show up in
// real apps are OCCLUSION (visibility tests) and EVENT (fence-style
// sync); the rest are essentially unused.
//
// OCCLUSION queries the GPU's MTLVisibilityResultMode counter via
// dxmt's shared visibility-result infrastructure (the same path d3d11
// IMTLD3D11OcclusionQuery uses). EVENT is backed by the queue's
// coherent-seq watermark, TIMESTAMP by a host-side monotonic clock.
// The remaining D3D9 query types (VCACHE, RESOURCEMANAGER, PIPELINE-
// TIMINGS, INTERFACETIMINGS, VERTEXTIMINGS, PIXELTIMINGS, BANDWIDTH-
// TIMINGS, CACHEUTILIZATION) stay as stubs — apps essentially never
// use them in shipped titles.
class MTLD3D9Query final : public ComObject<IDirect3DQuery9> {
public:
  MTLD3D9Query(MTLD3D9Device *device, D3DQUERYTYPE type);
  ~MTLD3D9Query();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  D3DQUERYTYPE STDMETHODCALLTYPE GetType() override;
  DWORD STDMETHODCALLTYPE GetDataSize() override;
  HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) override;
  HRESULT STDMETHODCALLTYPE GetData(void *pData, DWORD dwSize, DWORD dwGetDataFlags) override;

private:
  // Drains an in-flight OCCLUSION visibility query — emits the same
  // endVisibilityResultQuery chunk lambda an explicit Issue(END) would,
  // matched 1:1 with the prior Begin so dxmt_context's pending_queries_
  // / active_visibility_query_count_ bookkeeping balances. No-op when
  // the query isn't OCCLUSION, isn't currently Begun, or has already
  // been Ended. Called from Issue(BEGIN) (Begin-after-Begin path) and
  // from the destructor (Release-before-End path).
  void endOcclusionIfActive();

  MTLD3D9Device *m_device;
  D3DQUERYTYPE m_type;
  // Tracks whether D3DISSUE_END has run — GetData before END is
  // INVALIDCALL per the D3D9 contract. wined3d query.c:75 enforces.
  bool m_ended = false;
  // EVENT-query GPU completion seq. Captured at Issue(D3DISSUE_END):
  // the queue's CurrentSeqId — the chunk-in-flight that all-prior work
  // up to the END landed in. GetData polls CoherentSeqId against this
  // value; when GPU-coherent ≥ captured, the event is signaled. Zero
  // before any END so the EVENT type defaults to "signaled" if the
  // app polls before issuing — matches the prior stub shape.
  uint64_t m_event_seq = 0;
  // TIMESTAMP-query host-side time capture. Real GPU timestamps via
  // MTLCounterSampleBuffer are an infrastructure follow-up; the host-
  // side approximation is a calling-thread monotonic-clock snapshot at
  // Issue(D3DISSUE_END), reported back in nanoseconds. Apps that use
  // timestamps for profiling (Fraps, Steam FPS overlay, in-game
  // counters) compute deltas — a host-side delta is within a few-ms
  // of the GPU delta for typical frame-paced work, vs. the zero-ticks
  // stub that made every elapsed measurement appear instantaneous.
  uint64_t m_timestamp_ns = 0;
  // OCCLUSION-query GPU-backed counter. Allocated fresh on each
  // Issue(D3DISSUE_BEGIN); the begin/end chunk lambdas pass it to
  // ArgumentEncodingContext::beginVisibilityResultQuery /
  // endVisibilityResultQuery, which carves out an offset in the
  // shared visibility result heap and switches the render encoder's
  // setVisibilityResultMode between Counting and Disabled. The query
  // accumulates fragment-pass counts on the GPU; getValue() returns
  // true once the issuing chunk's readback has run. Null when the
  // query has never been Begun (or has been read out post-End and
  // is awaiting the next Begin); the read path defaults to D3D_OK
  // with pixel count 0 in that case, matching wined3d's "no result
  // yet" shape. d3d11_query.cpp:230-297 is the literal model.
  Rc<VisibilityResultQuery> m_visibility_query;
  // Self-pin shape mirrors MTLD3D9StateBlock — keep `this` alive
  // across the public 1→0 transition long enough for the override
  // to drop the device pin safely.
  bool m_self_pinned = true;
};

} // namespace dxmt
