#include "d3d9_fvf.hpp"

namespace dxmt {

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
  // Mirror the wine dlls/d3d9 layer's vdecl_convert_fvf (the 1:1 app-facing
  // shape; wined3d's own convert_fvf_to_declaration differs on the XYZW gate
  // below). Single-stream lowering; D3D9's FVF
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
    // A projected-W position is mutually exclusive with blend weights, the
    // gate the dlls/d3d9 layer applies (vdecl_convert_fvf); wined3d's own
    // convert_fvf_to_declaration omits this !has_blend guard.
    else if (!has_blend && (fvf & D3DFVF_XYZW) == D3DFVF_XYZW)
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
    // Three-way, mirroring wined3d vertexdeclaration.c
    // convert_fvf_to_declaration: UBYTE4 when LASTBETA_UBYTE4 (or the
    // XYZB2 + LASTBETA_D3DCOLOR special case), D3DCOLOR when
    // LASTBETA_D3DCOLOR, otherwise FLOAT1 (e.g. plain XYZB5 with no
    // LASTBETA flag, where the trailing beta is a float index).
    BYTE type;
    if ((fvf & D3DFVF_LASTBETA_UBYTE4) || ((fvf & 0x000Eu) == D3DFVF_XYZB2 && (fvf & D3DFVF_LASTBETA_D3DCOLOR)))
      type = D3DDECLTYPE_UBYTE4;
    else if (fvf & D3DFVF_LASTBETA_D3DCOLOR)
      type = D3DDECLTYPE_D3DCOLOR;
    else
      type = D3DDECLTYPE_FLOAT1;
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

DWORD
derive_fvf_from_elements(const D3DVERTEXELEMENT9 *elements, size_t count) {
  // A lone position element in the canonical slot converts; anything else
  // (extra elements, a nonzero stream/offset/method/usage index, or any other
  // usage) reports 0. count == 2 is one vertex element plus D3DDECL_END.
  if (count != 2 || elements[0].Stream != 0 || elements[0].Offset != 0 ||
      elements[0].Method != D3DDECLMETHOD_DEFAULT || elements[0].UsageIndex != 0)
    return 0;
  if (elements[0].Type == D3DDECLTYPE_FLOAT3 && elements[0].Usage == D3DDECLUSAGE_POSITION)
    return D3DFVF_XYZ;
  if (elements[0].Type == D3DDECLTYPE_FLOAT4 && elements[0].Usage == D3DDECLUSAGE_POSITIONT)
    return D3DFVF_XYZRHW;
  return 0;
}

} // namespace dxmt
