#include "d3d9_vertex_declaration.hpp"

#include "d3d9_device.hpp"

#include <cstring>

namespace dxmt {

namespace {
// Walk to (and include) the D3DDECL_END terminator. wined3d
// dlls/d3d9/vertexdeclaration.c uses convert_to_wined3d_declaration
// to count, but the simpler shape that matches GetDeclaration's
// observable contract is "scan for the entry whose Stream==0xFF, count
// includes that entry". wined3d checks Stream==0xff only on the
// terminator; Type is ignored there. dxmt previously required both
// Stream==0xFF AND Type==UNUSED, which walked past a malformed
// terminator (Stream==0xFF with Type!=UNUSED) straight to the
// 64-element defensive cap.
size_t
count_with_terminator(const D3DVERTEXELEMENT9 *elements) {
  size_t n = 0;
  for (;; ++n) {
    if (elements[n].Stream == 0xFF)
      return n + 1;
    // Defensive cap: D3D9 spec puts the maximum element count
    // (excluding the terminator) at 64. Past that, the input is
    // malformed; return what we have rather than walking off the
    // page.
    if (n >= 64)
      return n;
  }
}
} // namespace

MTLD3D9VertexDeclaration::MTLD3D9VertexDeclaration(MTLD3D9Device *device, const D3DVERTEXELEMENT9 *elements) :
    MTLD3D9VertexDeclaration(device, elements, /*selfPin=*/true) {}

MTLD3D9VertexDeclaration::MTLD3D9VertexDeclaration(
    MTLD3D9Device *device, const D3DVERTEXELEMENT9 *elements, bool selfPin
) :
    m_device(device) {
  size_t n = count_with_terminator(elements);
  m_elements.assign(elements, elements + n);
  // Self-pin matches the surface/texture/buffer pattern. Release's
  // m_self_pinned guard drops it exactly once on the first public→0
  // transition. Internal-cache callers pass selfPin=false because the
  // cache holds the only ref (priv via Com<,false>) and a public ref
  // never exists.
  m_self_pinned = selfPin;
  if (selfPin)
    AddRefPrivate();
}

MTLD3D9VertexDeclaration::~MTLD3D9VertexDeclaration() = default;

ULONG STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::AddRef() {
  ULONG ref = ComObject::AddRef();
  if (ref == 1)
    m_device->AddRef();
  return ref;
}

ULONG STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::Release() {
  ULONG ref = ComObject::Release();
  if (ref == 0) {
    m_device->Release();
    if (m_self_pinned) {
      m_self_pinned = false;
      ReleasePrivate();
    }
  }
  return ref;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;

  if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DVertexDeclaration9)) {
    *ppvObject = static_cast<IDirect3DVertexDeclaration9 *>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::GetDevice(IDirect3DDevice9 **ppDevice) {
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) {
  // wined3d dlls/d3d9/vertexdeclaration.c; pElement is allowed
  // to be NULL; callers do that to query the count first. pNumElements
  // is required.
  if (!pNumElements)
    return D3DERR_INVALIDCALL;
  *pNumElements = static_cast<UINT>(m_elements.size());
  if (pElement)
    std::memcpy(pElement, m_elements.data(), m_elements.size() * sizeof(D3DVERTEXELEMENT9));
  return D3D_OK;
}

} // namespace dxmt
