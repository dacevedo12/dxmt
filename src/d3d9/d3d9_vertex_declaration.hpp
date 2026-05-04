#pragma once

#include "com/com_object.hpp"
#include "com/com_pointer.hpp"
#include "d3d9.h"

#include <vector>

namespace dxmt {

class MTLD3D9Device;

// IDirect3DVertexDeclaration9 — a frozen copy of the D3DVERTEXELEMENT9
// array passed to CreateVertexDeclaration, plus a back-pointer to the
// device. No Metal handles yet; the input-assembler descriptor that
// the rasterizer eventually needs is built lazily from these elements
// when a draw resolves the (decl, vertex shader) pair.
//
// References (vtable shape, GetDeclaration semantics): wined3d
// dlls/d3d9/vertexdeclaration.c. MGL has nothing analogous — Metal
// expresses vertex layout entirely via MTLVertexDescriptor at PSO
// build time, not as a standalone object.
//
// Lifetime mirrors the standalone-resource pattern (MTLD3D9Surface
// CreateRenderTarget shape):
//   - Decl holds raw MTLD3D9Device*; first AddRef bumps device, last
//     Release drops it.
//   - Ctor self-pin via AddRefPrivate; Release drops it exactly once
//     on the first pub→0 — same m_self_pinned guard the
//     surface/texture/buffer subclasses use, so a Get/Release cycle
//     on a slot-bound decl (when SetVertexDeclaration arrives) does
//     not over-decrement priv.
class MTLD3D9VertexDeclaration final : public ComObject<IDirect3DVertexDeclaration9> {
public:
  MTLD3D9VertexDeclaration(MTLD3D9Device *device, const D3DVERTEXELEMENT9 *elements);
  // Internal-cache ctor — used by SetFVF for the device-owned cache of
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

  // Internal accessors — used by SetVertexDeclaration / future IA
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

private:
  MTLD3D9Device *m_device;
  // Includes the D3DDECL_END terminator at the back, matching wined3d
  // dlls/d3d9/vertexdeclaration.c:386 (element_count =
  // wined3d_element_count + 1). Apps reading via GetDeclaration with
  // a NULL out-array expect this count to include the terminator.
  std::vector<D3DVERTEXELEMENT9> m_elements;
  bool m_self_pinned = true;
};

// Lower a single-stream D3D9 FVF dword into the D3DVERTEXELEMENT9 array
// CreateVertexDeclaration consumes. Apps that bind a FVF via SetFVF
// expect the runtime to synthesise the corresponding declaration —
// wined3d does this in vertexdeclaration.c convert_fvf_to_declaration
// (the reference shape this mirrors). Output array does NOT carry the
// D3DDECL_END terminator; callers append one before passing to
// CreateVertexDeclaration.
void build_fvf_decl_elements(DWORD fvf, std::vector<D3DVERTEXELEMENT9> &out);

} // namespace dxmt
