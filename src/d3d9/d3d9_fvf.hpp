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
// without a device.
void build_fvf_decl_elements(DWORD fvf, std::vector<D3DVERTEXELEMENT9> &out);

// The FVF a GetFVF on an app-created declaration reports. D3D9 converts only
// the two single-element position layouts ([FLOAT3 POSITION] to XYZ, [FLOAT4
// POSITIONT] to XYZRHW) and forces every other declaration to 0. wined3d never
// derives an FVF here (GetFVF reads a stored 0) and DXVK maps each element and
// over-reports, so neither is the model: this mirrors the Windows behaviour
// test_fvf_decl_conversion pins. count includes the D3DDECL_END terminator.
DWORD derive_fvf_from_elements(const D3DVERTEXELEMENT9 *elements, size_t count);

} // namespace dxmt
