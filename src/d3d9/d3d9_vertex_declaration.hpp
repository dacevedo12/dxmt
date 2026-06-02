#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"

#include <vector>

namespace dxmt {

class MTLD3D9Device;

// IDirect3DVertexDeclaration9: frozen D3DVERTEXELEMENT9 array +
// device back-pointer. IA descriptor built lazily at draw from (decl, VS) pair.
// Lifetime: self-pin via AddRefPrivate; Release drops once at first pub→0.
// Same m_self_pinned guard prevents over-decrement on Get/Release cycles.
// References: wined3d dlls/d3d9/vertexdeclaration.c.
class MTLD3D9VertexDeclaration final : public ComObject<IDirect3DVertexDeclaration9> {
public:
  MTLD3D9VertexDeclaration(MTLD3D9Device *device, const D3DVERTEXELEMENT9 *elements);
  // Internal-cache ctor; used by SetFVF for the device-owned cache of
  // FVF-derived declarations. The cache holds the only reference (a
  // private ref via Com<,false>) for the device's lifetime; without
  // disabling the ctor self-pin the priv refcount would never reach 0
  // and the decl would leak when the cache drops it. Public AddRef
  // does NOT bump device refcount in this mode (no user owner).
  MTLD3D9VertexDeclaration(MTLD3D9Device *device, const D3DVERTEXELEMENT9 *elements, bool selfPin);
  ~MTLD3D9VertexDeclaration();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override;

  HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
  HRESULT STDMETHODCALLTYPE GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) override;

  // Internal accessors; used by SetVertexDeclaration / future IA
  // descriptor lowering. Not part of the IDirect3DVertexDeclaration9
  // contract.
  MTLD3D9Device *
  deviceRaw() const {
    return m_device;
  }
  const D3DVERTEXELEMENT9 *
  elements() const {
    return m_elements.data();
  }
  UINT
  elementCount() const {
    return static_cast<UINT>(m_elements.size());
  }
  // The FVF this declaration corresponds to: the source FVF for a decl
  // SetFVF synthesized, 0 for one CreateVertexDeclaration built from
  // elements (D3D9 does not reverse a general element list to an FVF, so
  // GetFVF after SetVertexDeclaration reports 0). wined3d stores the same
  // fvf on the declaration; GetFVF reads the bound decl's value.
  DWORD
  fvf() const {
    return m_fvf;
  }
  void
  setFvf(DWORD fvf) {
    m_fvf = fvf;
  }

private:
  MTLD3D9Device *m_device;
  // Includes the D3DDECL_END terminator at the back, matching wined3d
  // dlls/d3d9/vertexdeclaration.c (element_count =
  // wined3d_element_count + 1). Apps reading via GetDeclaration with
  // a NULL out-array expect this count to include the terminator.
  std::vector<D3DVERTEXELEMENT9> m_elements;
  DWORD m_fvf = 0;
  bool m_self_pinned = true;
};

} // namespace dxmt
