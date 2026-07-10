/*
 * Private diagnostic interface for MTLD3D9Device (tests only, not visible to apps).
 * Lifetime: pointer is BORROWED; callers must NOT AddRef/Release.
 * Does not inherit IUnknown; lifetime governed by IDirect3DDevice9 refcount.
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
