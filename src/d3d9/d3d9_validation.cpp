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
  for (size_t i = 0; elements[i].Stream != 0xFF && i < 64; ++i) {
    if (elements[i].Type >= D3DDECLTYPE_UNUSED)
      return E_FAIL;
  }
  return D3D_OK;
}

} // namespace dxmt
