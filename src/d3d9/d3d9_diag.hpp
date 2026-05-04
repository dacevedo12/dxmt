/*
 * Private diagnostic interface for dxmt's MTLD3D9Device.
 *
 * Exposed via QueryInterface on the device using the private
 * IID_IDxmtDiag9 GUID. Tests (tests/dx9/) probe this to assert
 * post-teardown invariants (m_losableResourceCount == 0, etc.)
 * that aren't observable through the public D3D9 surface.
 *
 * Apps never see this — the GUID is private to dxmt and is not
 * listed in any IID_PPV_ARGS_Helper / IID_*-exported table.
 *
 * Lifetime contract: the IDxmtDiag9 pointer returned via QI is
 * BORROWED. Callers MUST NOT AddRef/Release it; lifetime is
 * governed by the COM refcount on the IDirect3DDevice9 the
 * caller already holds. The diag interface deliberately does
 * NOT inherit IUnknown — it isn't a standalone COM object and
 * giving it an independent refcount would invite lifetime
 * confusion. The pre-existing IDirect3DDevice9 pointer keeps
 * the device alive for the duration of any diag use.
 */
#pragma once

#include "d3d9.h"

namespace dxmt {

struct IDxmtDiag9 {
  virtual UINT STDMETHODCALLTYPE GetLosableResourceCount() = 0;
};

// {D2C7B12B-D9D9-4D9D-8AAA-BBCCDDEE0001}
//
// Private dxmt diag UUID. Random fourth-version GUID; no app or
// upstream tooling enumerates QI tables for this value.
inline constexpr GUID IID_IDxmtDiag9 = {0xD2C7B12B, 0xD9D9, 0x4D9D, {0x8A, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x00, 0x01}};

} // namespace dxmt
