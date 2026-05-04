#include "d3d9_vertex_declaration.hpp"

#include "d3d9_device.hpp"
#include "d3d9_trace.hpp"

#include <cstring>

namespace dxmt {

namespace {
// Walk to (and include) the D3DDECL_END terminator. wined3d
// dlls/d3d9/vertexdeclaration.c uses convert_to_wined3d_declaration
// to count, but the simpler shape that matches GetDeclaration's
// observable contract is "scan for the entry whose Stream==0xFF and
// Stream==0xFF, count includes that entry". wined3d
// vertexdeclaration.c:329 checks Stream==0xff only — Type ignored on
// the terminator. dxmt previously required both Stream==0xFF AND
// Type==UNUSED, which walked past malformed terminators (Stream=0xFF
// with Type!=UNUSED, e.g. uninitialized memory after a copy bug)
// straight to the 64-element defensive cap.
size_t
count_with_terminator(const D3DVERTEXELEMENT9 *elements) {
  size_t n = 0;
  for (;; ++n) {
    if (elements[n].Stream == 0xFF)
      return n + 1;
    // Defensive cap — D3D9 spec puts the maximum element count
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
  D9_TRACE("IDirect3DVertexDeclaration9::QueryInterface");
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
  D9_TRACE("IDirect3DVertexDeclaration9::GetDevice");
  if (!ppDevice)
    return D3DERR_INVALIDCALL;
  *ppDevice = ::dxmt::ref(static_cast<IDirect3DDevice9 *>(m_device));
  return D3D_OK;
}

HRESULT STDMETHODCALLTYPE
MTLD3D9VertexDeclaration::GetDeclaration(D3DVERTEXELEMENT9 *pElement, UINT *pNumElements) {
  D9_TRACE("IDirect3DVertexDeclaration9::GetDeclaration");
  // wined3d dlls/d3d9/vertexdeclaration.c:267 — pElement is allowed
  // to be NULL; callers do that to query the count first. pNumElements
  // is required.
  if (!pNumElements)
    return D3DERR_INVALIDCALL;
  *pNumElements = static_cast<UINT>(m_elements.size());
  if (pElement)
    std::memcpy(pElement, m_elements.data(), m_elements.size() * sizeof(D3DVERTEXELEMENT9));
  return D3D_OK;
}

namespace {
// One element-size lookup per D3DDECLTYPE used by the FVF lowering.
// wined3d uses a per-format byte-size table in utils.c; ours covers
// only the types convert_fvf_to_declaration emits.
WORD
fvf_decl_type_size(BYTE type) {
  switch (type) {
  case D3DDECLTYPE_FLOAT1:
    return 4;
  case D3DDECLTYPE_FLOAT2:
    return 8;
  case D3DDECLTYPE_FLOAT3:
    return 12;
  case D3DDECLTYPE_FLOAT4:
    return 16;
  case D3DDECLTYPE_D3DCOLOR:
    return 4;
  case D3DDECLTYPE_UBYTE4:
    return 4;
  default:
    return 0;
  }
}

void
fvf_append_element(std::vector<D3DVERTEXELEMENT9> &out, WORD &offset, BYTE type, BYTE usage, BYTE usage_index) {
  D3DVERTEXELEMENT9 e{};
  e.Stream = 0;
  e.Offset = offset;
  e.Type = type;
  e.Method = D3DDECLMETHOD_DEFAULT;
  e.Usage = usage;
  e.UsageIndex = usage_index;
  out.push_back(e);
  offset = static_cast<WORD>(offset + fvf_decl_type_size(type));
}
} // namespace

void
build_fvf_decl_elements(DWORD fvf, std::vector<D3DVERTEXELEMENT9> &out) {
  // Mirror wined3d dlls/wined3d/vertexdeclaration.c
  // convert_fvf_to_declaration. Single-stream lowering — D3D9's FVF
  // dword carries no stream index, so every emitted element binds to
  // stream 0. Callers that need multi-stream layouts must use
  // CreateVertexDeclaration with an explicit element array.
  out.clear();
  WORD offset = 0;

  bool has_pos = (fvf & D3DFVF_POSITION_MASK) != 0;
  // XYZB1..XYZB5 sit in the low nibble above XYZRHW (0x004); B5 = 0x00E.
  bool has_blend = (fvf & 0x000Eu) > D3DFVF_XYZRHW;
  bool has_blend_idx = has_blend && (((fvf & 0x000Eu) == D3DFVF_XYZB5) || (fvf & D3DFVF_LASTBETA_D3DCOLOR) ||
                                     (fvf & D3DFVF_LASTBETA_UBYTE4));

  unsigned int num_blends = 1u + (((fvf & 0x000Eu) - D3DFVF_XYZB1) >> 1);
  if (has_blend_idx && num_blends > 0)
    --num_blends;

  if (has_pos) {
    if (!has_blend && (fvf & D3DFVF_XYZRHW) == D3DFVF_XYZRHW)
      fvf_append_element(out, offset, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0);
    else if ((fvf & D3DFVF_XYZW) == D3DFVF_XYZW)
      fvf_append_element(out, offset, D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0);
    else
      fvf_append_element(out, offset, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
  }

  if (has_blend && num_blends > 0) {
    BYTE type = D3DDECLTYPE_FLOAT1;
    if ((fvf & 0x000Eu) == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR))
      type = D3DDECLTYPE_D3DCOLOR;
    else
      switch (num_blends) {
      case 1:
        type = D3DDECLTYPE_FLOAT1;
        break;
      case 2:
        type = D3DDECLTYPE_FLOAT2;
        break;
      case 3:
        type = D3DDECLTYPE_FLOAT3;
        break;
      case 4:
        type = D3DDECLTYPE_FLOAT4;
        break;
      }
    fvf_append_element(out, offset, type, D3DDECLUSAGE_BLENDWEIGHT, 0);
  }

  if (has_blend_idx) {
    BYTE type = D3DDECLTYPE_D3DCOLOR;
    if ((fvf & D3DFVF_LASTBETA_UBYTE4) || ((fvf & 0x000Eu) == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR)))
      type = D3DDECLTYPE_UBYTE4;
    fvf_append_element(out, offset, type, D3DDECLUSAGE_BLENDINDICES, 0);
  }

  if (fvf & D3DFVF_NORMAL)
    fvf_append_element(out, offset, D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
  if (fvf & D3DFVF_PSIZE)
    fvf_append_element(out, offset, D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
  if (fvf & D3DFVF_DIFFUSE)
    fvf_append_element(out, offset, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
  if (fvf & D3DFVF_SPECULAR)
    fvf_append_element(out, offset, D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);

  unsigned int num_textures = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
  unsigned int texcoords = (fvf & 0xffff0000u) >> 16;
  for (unsigned int idx = 0; idx < num_textures; ++idx) {
    BYTE type;
    switch ((texcoords >> (idx * 2)) & 0x3u) {
    case D3DFVF_TEXTUREFORMAT1:
      type = D3DDECLTYPE_FLOAT1;
      break;
    case D3DFVF_TEXTUREFORMAT3:
      type = D3DDECLTYPE_FLOAT3;
      break;
    case D3DFVF_TEXTUREFORMAT4:
      type = D3DDECLTYPE_FLOAT4;
      break;
    case D3DFVF_TEXTUREFORMAT2:
    default:
      type = D3DDECLTYPE_FLOAT2;
      break;
    }
    fvf_append_element(out, offset, type, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(idx));
  }
}

} // namespace dxmt
