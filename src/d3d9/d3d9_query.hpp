#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"
#include "dxmt_occlusion_query.hpp"
#include "rc/util_rc_ptr.hpp"

namespace dxmt {

class MTLD3D9Device;

// IDirect3DQuery9: only OCCLUSION and EVENT are used in practice.
// OCCLUSION uses MTLVisibilityResultMode; EVENT is backed by
// queue coherent-seq watermark; other types stub out.
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
  // Drains an in-flight OCCLUSION visibility query: emits the same
  // endVisibilityResultQuery chunk lambda an explicit Issue(END) would,
  // matched 1:1 with the prior Begin so dxmt_context's pending_queries_
  // / active_visibility_query_count_ bookkeeping balances. No-op when
  // the query isn't OCCLUSION, isn't currently Begun, or has already
  // been Ended. Called from Issue(BEGIN) (Begin-after-Begin path) and
  // from the destructor (Release-before-End path).
  void endOcclusionIfActive();

  MTLD3D9Device *m_device;
  D3DQUERYTYPE m_type;
  // Tracks whether D3DISSUE_END has run; GetData before END is
  // INVALIDCALL per the D3D9 contract. wined3d query.c enforces.
  bool m_ended = false;
  // EVENT-query GPU completion seq. Captured at Issue(D3DISSUE_END):
  // the queue's CurrentSeqId: the chunk-in-flight that all-prior work
  // up to the END landed in. GetData polls CoherentSeqId against this
  // value; when GPU-coherent ≥ captured, the event is signaled. Zero
  // before any END so the EVENT type defaults to "signaled" if the
  // app polls before issuing; matches the prior stub shape.
  uint64_t m_event_seq = 0;
  // TIMESTAMP-query host-side time capture. Real GPU timestamps via
  // MTLCounterSampleBuffer are an infrastructure follow-up; the host-
  // side approximation is a calling-thread monotonic-clock snapshot at
  // Issue(D3DISSUE_END), reported back in nanoseconds. Apps that use
  // timestamps for profiling (Fraps, Steam FPS overlay, in-game
  // counters) compute deltas: a host-side delta is within a few-ms
  // of the GPU delta for typical frame-paced work, vs. the zero-ticks
  // stub that made every elapsed measurement appear instantaneous.
  uint64_t m_timestamp_ns = 0;
  // OCCLUSION-query GPU counter allocated at Issue(BEGIN).
  // beginVisibilityResultQuery/endVisibilityResultQuery carve out
  // heap offset and manage encoder's setVisibilityResultMode.
  // Null until Begin; read returns 0 in that case.
  Rc<VisibilityResultQuery> m_visibility_query;
  // Self-pin shape mirrors MTLD3D9StateBlock: keep `this` alive
  // across the public 1→0 transition long enough for the override
  // to drop the device pin safely.
  bool m_self_pinned = true;
};

} // namespace dxmt
