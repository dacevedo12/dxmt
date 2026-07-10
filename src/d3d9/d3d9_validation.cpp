#include "d3d9_validation.hpp"

namespace dxmt {

HRESULT
validate_stream_source_freq(UINT StreamNumber, UINT Setting) {
  const bool indexed = (Setting & D3DSTREAMSOURCE_INDEXEDDATA) != 0;
  const bool instanced = (Setting & D3DSTREAMSOURCE_INSTANCEDATA) != 0;
  if (Setting == 0)
    return D3DERR_INVALIDCALL;
  if (indexed && instanced)
    return D3DERR_INVALIDCALL;
  if (StreamNumber == 0 && instanced)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}

HRESULT
validate_vertex_elements(const D3DVERTEXELEMENT9 *elements) {
  // Bound the scan before dereferencing so a decl the app forgot to terminate
  // with D3DDECL_END (0xFF stream) does not read one element past the cap
  // (MAXD3DDECLLENGTH). The short-circuit order matters: i < 64 must come first.
  for (size_t i = 0; i < 64 && elements[i].Stream != 0xFF; ++i) {
    if (elements[i].Type >= D3DDECLTYPE_UNUSED)
      return E_FAIL;
    // wined3d clamps each element's required alignment to min(byte_count, 4)
    // and rejects an unaligned offset (vertexdeclaration.c). Every D3DDECLTYPE
    // is at least 4 bytes wide, so the alignment is always 4: an offset that
    // is not 4-byte aligned is invalid.
    if (elements[i].Offset & 3u)
      return E_FAIL;
  }
  return D3D_OK;
}

} // namespace dxmt
