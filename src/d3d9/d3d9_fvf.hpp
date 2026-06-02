#pragma once

#include "d3d9.h"

#include <vector>

namespace dxmt {

// Lower a single-stream D3D9 FVF dword into the D3DVERTEXELEMENT9 array
// CreateVertexDeclaration consumes. Apps that bind a FVF via SetFVF
// expect the runtime to synthesise the corresponding declaration:
// wined3d does this in vertexdeclaration.c convert_fvf_to_declaration
// (the reference shape this mirrors). Output array does NOT carry the
// D3DDECL_END terminator; callers append one before passing to
// CreateVertexDeclaration.
//
// Kept as a free function in its own TU (matching wined3d, where the
// conversion is likewise standalone) so the lowering can be exercised
// without a device: see tests/dx9/unit/test_fvf_decl.cpp.
void build_fvf_decl_elements(DWORD fvf, std::vector<D3DVERTEXELEMENT9> &out);

} // namespace dxmt
