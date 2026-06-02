#pragma once

#include "d3d9.h"

namespace dxmt {

// Validate a SetStreamSourceFreq(StreamNumber, Setting) request against
// the D3D9 contract (reference: Wine d3d9 visual.c stream_test). Returns
// D3D_OK when the (stream, setting) pair is legal, D3DERR_INVALIDCALL
// otherwise. The caller handles the StreamNumber upper-bound check (a
// runtime array-size concern, not part of this contract).
//
// Native D3D9 accepts a plain frequency other than 1 (no flags) and
// accepts INSTANCEDATA with a zero divider; both return D3D_OK and
// round-trip through GetStreamSourceFreq; the GPU step rate is clamped
// to >= 1 at draw time. Only three combinations are rejected: a zero
// setting, both flags set at once, and INSTANCEDATA on stream 0.
// Checked host-native in tests/dx9/unit/test_stream_source_freq.cpp.
HRESULT validate_stream_source_freq(UINT StreamNumber, UINT Setting);

// Validate a CreateVertexDeclaration element array. D3DDECLTYPE_UNUSED
// (17) is legal only as the D3DDECL_END terminator; any non-terminator
// element whose Type is >= UNUSED makes CreateVertexDeclaration return
// E_FAIL (reference: Wine d3d9 test_unused_declaration_type, wined3d
// vertexdeclaration.c). Returns D3D_OK for a well-formed array. The
// array must be D3DDECL_END-terminated (Stream == 0xFF); a defensive
// 64-element cap bounds a malformed input.
// Checked host-native in tests/dx9/unit/test_vertex_decl_validation.cpp.
HRESULT validate_vertex_elements(const D3DVERTEXELEMENT9 *elements);

} // namespace dxmt
