#pragma once

#include "dxso_header.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace dxmt {

// DXSO bytecode iterator: walks body tokens (after version DWORD, before END)
// and yields DxsoInstruction with decoded operands. ALU (Mov/Mad): dst, optional
// predicate, sources. Control-flow (If/Rep/Loop): sources only. Dcl: semantic+dst.
// Def/DefI/DefB: dst+typed literal. Relative addressing (`c[a0+N]`) handled inline.

// uint16_t storage: every defined opcode fits in 16 bits (the high
// values 0xFFFD/0xFFFE/0xFFFF saturate the field). DXVK uses uint32_t;
// switching to uint16_t in dxmt is purely a size choice; the token
// extraction below masks with 0xFFFF either way.
enum class DxsoOpcode : uint16_t {
  Nop = 0,
  Mov = 1,
  Add = 2,
  Sub = 3,
  Mad = 4,
  Mul = 5,
  Rcp = 6,
  Rsq = 7,
  Dp3 = 8,
  Dp4 = 9,
  Min = 10,
  Max = 11,
  Slt = 12,
  Sge = 13,
  Exp = 14,
  Log = 15,
  Lit = 16,
  Dst = 17,
  Lrp = 18,
  Frc = 19,
  M4x4 = 20,
  M4x3 = 21,
  M3x4 = 22,
  M3x3 = 23,
  M3x2 = 24,
  Call = 25,
  CallNz = 26,
  Loop = 27,
  Ret = 28,
  EndLoop = 29,
  Label = 30,
  Dcl = 31,
  Pow = 32,
  Crs = 33,
  Sgn = 34,
  Abs = 35,
  Nrm = 36,
  SinCos = 37,
  Rep = 38,
  EndRep = 39,
  If = 40,
  Ifc = 41,
  Else = 42,
  EndIf = 43,
  Break = 44,
  BreakC = 45,
  Mova = 46,
  DefB = 47,
  DefI = 48,

  TexCoord = 64,
  TexKill = 65,
  Tex = 66,
  TexBem = 67,
  TexBemL = 68,
  TexReg2Ar = 69,
  TexReg2Gb = 70,
  TexM3x2Pad = 71,
  TexM3x2Tex = 72,
  TexM3x3Pad = 73,
  TexM3x3Tex = 74,
  TexM3x3Spec = 76,
  TexM3x3VSpec = 77,
  ExpP = 78,
  LogP = 79,
  Cnd = 80,
  Def = 81,
  TexReg2Rgb = 82,
  TexDp3Tex = 83,
  TexM3x2Depth = 84,
  TexDp3 = 85,
  TexM3x3 = 86,
  TexDepth = 87,
  Cmp = 88,
  Bem = 89,
  Dp2Add = 90,
  DsX = 91,
  DsY = 92,
  TexLdd = 93,
  SetP = 94,
  TexLdl = 95,
  BreakP = 96,

  Phase = 0xFFFD,
  Comment = 0xFFFE,
  End = 0xFFFF,
};

enum class DxsoRegisterType : uint8_t {
  Temp = 0,  // r#
  Input = 1, // v#
  Const = 2, // c#
  Addr = 3,  // a# (VS) / t# Texture (PS)
  Texture = 3,
  RasterizerOut = 4, // oPos / oFog / oPts
  AttributeOut = 5,  // oD#
  TexcoordOut = 6,   // oT#
  Output = 6,        // o# (VS3.0+)
  ConstInt = 7,      // i#
  ColorOut = 8,      // oC#
  DepthOut = 9,      // oDepth
  Sampler = 10,      // s#
  Const2 = 11,       // c2048..c4095
  Const3 = 12,       // c4096..c6143
  Const4 = 13,       // c6144..c8191
  ConstBool = 14,    // b#
  Loop = 15,         // aL
  TempFloat16 = 16,
  MiscType = 17, // vPos / vFace
  Label = 18,
  Predicate = 19, // p#
  PixelTexcoord = 20,
};

// Modifier values 14/15 are unused; FXC doesn't emit them. The
// decoder static_casts the 4-bit field unchecked; callers that
// translate to AIR should treat anything past Not (13) as None.
enum class DxsoRegModifier : uint8_t {
  None = 0,
  Neg = 1,     // -r
  Bias = 2,    // r - 0.5
  BiasNeg = 3, // -(r - 0.5)
  Sign = 4,    // 2*r - 1
  SignNeg = 5,
  Comp = 6, // 1 - r
  X2 = 7,
  X2Neg = 8,
  Dz = 9,  // r / r.z
  Dw = 10, // r / r.w
  Abs = 11,
  AbsNeg = 12,
  Not = 13,
};

class DxsoRegMask {
public:
  constexpr DxsoRegMask() : m_mask(0xF) {}
  // Caller is responsible for passing only the 4-bit field; the
  // decoders below mask before constructing.
  constexpr explicit DxsoRegMask(uint8_t mask) : m_mask(mask) {}

  constexpr bool
  operator[](uint32_t i) const {
    return (m_mask & (1u << i)) != 0;
  }
  constexpr uint8_t
  raw() const {
    return m_mask;
  }

private:
  uint8_t m_mask;
};

// Per-component selector packed as four 2-bit lanes (low to high).
// Lane i picks component (0=x, 1=y, 2=z, 3=w) of the source register
// for output channel i.
class DxsoRegSwizzle {
public:
  constexpr DxsoRegSwizzle() : m_swizzle(0b11100100) {} // .xyzw identity
  constexpr explicit DxsoRegSwizzle(uint8_t swizzle) : m_swizzle(swizzle) {}

  constexpr uint32_t
  operator[](uint32_t i) const {
    return (m_swizzle >> (2u * i)) & 0x3u;
  }
  constexpr uint8_t
  raw() const {
    return m_swizzle;
  }

private:
  uint8_t m_swizzle;
};

struct DxsoBaseRegister {
  DxsoRegisterType type;
  uint16_t num; // bits 0..10 of token (max 2047)
};

// Relative-address suffix: when has_relative is set on a dst / src,
// the next DWORD in the bytecode is decoded as one of these. type +
// num pick the address register (a0 in VS, aL in PS-loop bodies);
// swizzle picks which component of it to add to the base register
// number. DXVK src/dxso/dxso_decoder.cpp::decodeRelativeRegister.
struct DxsoRelativeReg {
  DxsoBaseRegister base;
  DxsoRegSwizzle swizzle;
};

// Destination register: base + write mask + saturate + result-shift.
// Result shift is a 4-bit two's-complement value in bits 24..27 (0xF = -1,
// i.e. divide by 2), per DXVK src/dxso/dxso_decoder.cpp; range is -8..+7.
struct DxsoDstRegister {
  DxsoBaseRegister base;
  DxsoRegMask mask;
  bool saturate;
  int8_t shift;
  bool has_relative; // SM3.0+ on dst, SM2.0+ on src
  DxsoRelativeReg relative;
};

// Source register: base + per-component swizzle + arithmetic
// modifier. Modifier is applied at fetch (e.g. -r, abs(r), 1-r).
struct DxsoSrcRegister {
  DxsoBaseRegister base;
  DxsoRegSwizzle swizzle;
  DxsoRegModifier modifier;
  bool has_relative;
  DxsoRelativeReg relative;
};

// `dcl_<usage>` semantic: bound to an input / output / sampler
// register at the start of the shader body. Together with the dst
// register the dcl writes to, this is what tells the AIR emitter
// "v3 carries TEXCOORD0", "s0 is a 2D sampler", etc.
enum class DxsoUsage : uint8_t {
  Position = 0,
  BlendWeight = 1,
  BlendIndices = 2,
  Normal = 3,
  PointSize = 4,
  Texcoord = 5,
  Tangent = 6,
  Binormal = 7,
  TessFactor = 8,
  PositionT = 9,
  Color = 10,
  Fog = 11,
  Depth = 12,
  Sample = 13,
};

// Sampler dcls carry the texture dimensionality alongside the dst
// register: values mirror D3D9's D3DSAMPLER_TEXTURE_TYPE encoding
// (D3DSTT_2D / D3DSTT_CUBE / D3DSTT_VOLUME) shifted to byte 3 of the
// declaration token.
enum class DxsoTextureType : uint8_t {
  Unknown = 0,
  Texture2D = 2,
  TextureCube = 3,
  Texture3D = 4,
  // 2D sampler bound to a Metal depth-format texture (INTZ / DF24 /
  // DF16 / auto-DS reuse). Not a decoder-emitted value; the host's
  // DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA marks this when the bound
  // texture's Metal pixel format has a depth aspect. Drives the
  // depth2d<float> path in the signature emitter and the depth-sample
  // replication in CreateSample.
  Texture2DDepth = 5,
};

// Declaration token (body[0] of Dcl) layout:
//   bits  0..3   : usage
//   bits 16..19  : usage index (e.g. TEXCOORD3 → index = 3)
//   bits 27..30  : texture type (sampler dcls only; 0 otherwise)
// DXVK src/dxso/dxso_decoder.cpp.
struct DxsoDeclaration {
  DxsoUsage usage;
  uint8_t usage_index;
  DxsoTextureType texture_type;
};

inline DxsoDeclaration
dxso_decode_declaration(uint32_t token) {
  DxsoDeclaration d;
  d.usage = static_cast<DxsoUsage>(token & 0xFu);
  d.usage_index = static_cast<uint8_t>((token & 0x000F0000u) >> 16);
  d.texture_type = static_cast<DxsoTextureType>((token & 0x78000000u) >> 27);
  return d;
}

// `def c#, x, y, z, w`     → DxsoDefKind::Float32 (4 IEEE floats)
// `defi i#, x, y, z, w`    → DxsoDefKind::Int32   (4 signed ints)
// `defb b#, true|false`    → DxsoDefKind::Bool    (1 DWORD, 0/1)
// The literal is held in raw form; readers cast via the union, the
// kind tag picks the lane. Mirrors DXVK src/dxso/dxso_decoder.h
// DxsoDefinition (a union there too).
enum class DxsoDefKind : uint8_t {
  Float32,
  Int32,
  Bool,
};

union DxsoDefinitionPayload {
  float f32[4];
  int32_t i32[4];
  uint32_t u32[4];
};

struct DxsoDefinition {
  DxsoDefKind kind;
  DxsoDefinitionPayload payload;
};

// Register-type + register-number extraction. Type is split across
// two ranges in the token: bits 11..12 (high) and bits 28..30 (low),
// reassembled into a 5-bit value (0..31). DXVK src/dxso/
// dxso_decoder.cpp. The naive `(type << 28)` encoding silently
// drops types ≥ 8: hand-authored DXSO blobs must split the high bit
// out into bit 11 explicitly.
inline DxsoBaseRegister
dxso_decode_base_register(uint32_t token) {
  uint32_t type_bits = ((token & 0x00001800u) >> 8)     // bits 11..12 → 3..4
                       | ((token & 0x70000000u) >> 28); // bits 28..30 → 0..2
  return DxsoBaseRegister{static_cast<DxsoRegisterType>(type_bits), static_cast<uint16_t>(token & 0x7FFu)};
}

inline DxsoDstRegister
dxso_decode_dst_register(uint32_t token) {
  DxsoDstRegister r{};
  r.base = dxso_decode_base_register(token);
  r.mask = DxsoRegMask(static_cast<uint8_t>((token & 0x000F0000u) >> 16));
  r.saturate = (token & (1u << 20)) != 0;
  uint32_t shift_bits = (token & 0x0F000000u) >> 24;
  r.shift = static_cast<int8_t>((shift_bits & 0x7) - (shift_bits & 0x8));
  r.has_relative = (token & (1u << 13)) != 0;
  return r;
}

inline DxsoSrcRegister
dxso_decode_src_register(uint32_t token) {
  DxsoSrcRegister r{};
  r.base = dxso_decode_base_register(token);
  r.swizzle = DxsoRegSwizzle(static_cast<uint8_t>((token & 0x00FF0000u) >> 16));
  r.modifier = static_cast<DxsoRegModifier>((token & 0x0F000000u) >> 24);
  r.has_relative = (token & (1u << 13)) != 0;
  return r;
}

// Relative-address sub-register decoder. Same base format as the
// owning operand (type + num) plus a swizzle field that picks which
// component of e.g. a0 to use. The relative-addr token has no mask /
// modifier: those bits aren't meaningful for an address register.
inline DxsoRelativeReg
dxso_decode_relative(uint32_t token) {
  DxsoRelativeReg r;
  r.base = dxso_decode_base_register(token);
  r.swizzle = DxsoRegSwizzle(static_cast<uint8_t>((token & 0x00FF0000u) >> 16));
  return r;
}

// Maximum decoded source operands per instruction. Mirrors DXVK's
// DxsoMaxOperandCount (src/dxso/dxso_decoder.h): wide enough for
// every defined SM3 opcode (TexLdd needs 4 sources; nothing exceeds
// it).
constexpr uint32_t kDxsoMaxOperandCount = 8;

// Per-opcode 8-bit specifier carried in token bits 16..23. Per-opcode
// mapping mirrors DXVK src/dxso/dxso_decoder.h (DxsoOpcodeSpecificData):
//   - Tex (SM2+): DxsoTexLdMode { Regular=0, Project=1, Bias=2 }
//   - Ifc / BreakC / Setp: DxsoComparison { Reserved0..Always }
//   - everything else: ignored
enum class DxsoTexLdMode : uint8_t {
  Regular = 0,
  Project = 1,
  Bias = 2,
};

struct DxsoInstruction {
  DxsoOpcode opcode;
  // Offset of the opcode token (DWORD index from byte_code[0], i.e.
  // including the version DWORD). length_dwords counts the opcode
  // token itself plus all of its parameter / body tokens.
  uint32_t offset_dwords;
  uint32_t length_dwords;
  bool predicated;
  bool coissue;
  // Token bits 16..23 raw: opcode-specific selector (DxsoTexLdMode
  // for Tex, DxsoComparison for Ifc/BreakC/Setp, ignored otherwise).
  uint8_t specific_data;

  // Decoded operands: filled for standard (dst+predicate+sources) and
  // control-flow shapes (sources only). Dcl/Def/DefI/DefB/Comment/End/Phase have
  // has_dst=false/src_count=0; operand decoding (semantics, literals) is a follow-up.
  bool has_dst;
  DxsoDstRegister dst;
  bool has_predicate;
  DxsoSrcRegister predicate;
  uint32_t src_count;
  DxsoSrcRegister src[kDxsoMaxOperandCount];

  // Set by Dcl opcodes alongside dst (the register the semantic
  // binds to). For sampler dcls dcl.texture_type is set; for input
  // / output dcls it stays Unknown.
  bool has_dcl;
  DxsoDeclaration dcl;

  // Set by Def / DefI / DefB opcodes alongside dst (the constant
  // register that takes the literal). Kind picks float vs int vs
  // bool; payload is held in raw 32-bit lanes for either reading.
  bool has_def;
  DxsoDefinition def;
};

// Default-length table used for SM1.x. SM2.0+ takes the length from
// token bits 24..27; SM1.x has fixed lengths per opcode. Mirrors
// DXVK src/dxso/dxso_tables.cpp::DxsoGetDefaultOpcodeLength.
inline uint32_t
dxso_default_opcode_length(DxsoOpcode opcode) {
  switch (opcode) {
  case DxsoOpcode::Nop:
    return 0;
  case DxsoOpcode::Mov:
    return 2;
  case DxsoOpcode::Add:
    return 3;
  case DxsoOpcode::Sub:
    return 3;
  case DxsoOpcode::Mad:
    return 4;
  case DxsoOpcode::Mul:
    return 3;
  case DxsoOpcode::Rcp:
    return 2;
  case DxsoOpcode::Rsq:
    return 2;
  case DxsoOpcode::Dp3:
    return 3;
  case DxsoOpcode::Dp4:
    return 3;
  case DxsoOpcode::Min:
    return 3;
  case DxsoOpcode::Max:
    return 3;
  case DxsoOpcode::Slt:
    return 3;
  case DxsoOpcode::Sge:
    return 3;
  case DxsoOpcode::Exp:
    return 2;
  case DxsoOpcode::Log:
    return 2;
  case DxsoOpcode::Lit:
    return 2;
  case DxsoOpcode::Dst:
    return 3;
  case DxsoOpcode::Lrp:
    return 4;
  case DxsoOpcode::Frc:
    return 2;
  case DxsoOpcode::M4x4:
    return 3;
  case DxsoOpcode::M4x3:
    return 3;
  case DxsoOpcode::M3x4:
    return 3;
  case DxsoOpcode::M3x3:
    return 3;
  case DxsoOpcode::M3x2:
    return 3;
  case DxsoOpcode::Call:
    return 1;
  case DxsoOpcode::CallNz:
    return 2;
  case DxsoOpcode::Loop:
    return 2;
  case DxsoOpcode::Ret:
    return 0;
  case DxsoOpcode::EndLoop:
    return 0;
  case DxsoOpcode::Label:
    return 1;
  case DxsoOpcode::Dcl:
    return 2;
  case DxsoOpcode::Pow:
    return 3;
  case DxsoOpcode::Crs:
    return 3;
  case DxsoOpcode::Sgn:
    return 4;
  case DxsoOpcode::Abs:
    return 2;
  case DxsoOpcode::Nrm:
    return 2;
  case DxsoOpcode::SinCos:
    return 4;
  case DxsoOpcode::Rep:
    return 1;
  case DxsoOpcode::EndRep:
    return 0;
  case DxsoOpcode::If:
    return 1;
  case DxsoOpcode::Ifc:
    return 2;
  case DxsoOpcode::Else:
    return 0;
  case DxsoOpcode::EndIf:
    return 0;
  case DxsoOpcode::Break:
    return 0;
  case DxsoOpcode::BreakC:
    return 2;
  case DxsoOpcode::Mova:
    return 2;
  case DxsoOpcode::DefB:
    return 2;
  case DxsoOpcode::DefI:
    return 5;
  case DxsoOpcode::TexCoord:
    return 1;
  case DxsoOpcode::TexKill:
    return 1;
  case DxsoOpcode::Tex:
    return 1;
  case DxsoOpcode::TexBem:
    return 2;
  case DxsoOpcode::TexBemL:
    return 2;
  case DxsoOpcode::TexReg2Ar:
    return 2;
  case DxsoOpcode::TexReg2Gb:
    return 2;
  case DxsoOpcode::TexM3x2Pad:
    return 2;
  case DxsoOpcode::TexM3x2Tex:
    return 2;
  case DxsoOpcode::TexM3x3Pad:
    return 2;
  case DxsoOpcode::TexM3x3Tex:
    return 2;
  case DxsoOpcode::TexM3x3Spec:
    return 3;
  case DxsoOpcode::TexM3x3VSpec:
    return 2;
  case DxsoOpcode::ExpP:
    return 2;
  case DxsoOpcode::LogP:
    return 2;
  case DxsoOpcode::Cnd:
    return 4;
  case DxsoOpcode::Def:
    return 5;
  case DxsoOpcode::TexReg2Rgb:
    return 2;
  case DxsoOpcode::TexDp3Tex:
    return 2;
  case DxsoOpcode::TexM3x2Depth:
    return 2;
  case DxsoOpcode::TexDp3:
    return 2;
  case DxsoOpcode::TexM3x3:
    return 2;
  case DxsoOpcode::TexDepth:
    return 1;
  case DxsoOpcode::Cmp:
    return 4;
  case DxsoOpcode::Bem:
    return 3;
  case DxsoOpcode::Dp2Add:
    return 4;
  case DxsoOpcode::DsX:
    return 2;
  case DxsoOpcode::DsY:
    return 2;
  case DxsoOpcode::TexLdd:
    return 5;
  case DxsoOpcode::SetP:
    return 3;
  case DxsoOpcode::TexLdl:
    return 3;
  case DxsoOpcode::BreakP:
    return 2;
  default:
    return UINT32_MAX;
  }
}

// Decode per-opcode operand shape. major_version drives relative-addressing rules:
// SM3.0+ allows rel-addr on dst, SM2.0+ on src, not on predicate.
// Body shape depends on opcode group, not opcode-length encoding (DXVK dxso_decoder.cpp).
inline void
dxso_decode_operands(
    DxsoOpcode opcode, const uint32_t *body, uint32_t body_count, uint8_t major_version, DxsoInstruction &out
) {
  uint32_t cursor = 0;

  // Contract for has_relative on the decoded operands: true *only*
  // when the SM-version rule applies AND the rel-addr extra DWORD
  // was actually read. The token-level bit-13 flag is normalized
  // away here so AIR emit can rely on `has_relative` without re-
  // checking the major version.
  // The cursor < body_count guard on the rel-addr branch is the
  // truncated-bytecode safeguard: rather than read past the body
  // we drop the relative flag and leave the operand un-relativized.
  auto take_dst = [&]() {
    if (cursor >= body_count)
      return;
    out.has_dst = true;
    out.dst = dxso_decode_dst_register(body[cursor++]);
    if (out.dst.has_relative && major_version >= 3 && cursor < body_count)
      out.dst.relative = dxso_decode_relative(body[cursor++]);
    else
      out.dst.has_relative = false;
  };

  auto take_src = [&]() {
    if (cursor >= body_count)
      return;
    DxsoSrcRegister s = dxso_decode_src_register(body[cursor++]);
    if (s.has_relative) {
      if (major_version >= 2) {
        // SM2.0+ relative addressing carries an extra DWORD naming the
        // address register (a0 / aL) and its swizzle.
        if (cursor < body_count)
          s.relative = dxso_decode_relative(body[cursor++]);
        else
          s.has_relative = false; // truncated-bytecode safeguard
      } else {
        // SM1.x relative addressing is IMPLICIT a0.x: no extra DWORD.
        // DXVK's decodeGenericRegister pre-seeds relative = {Addr, 0,
        // IdentitySwizzle} and only overrides it via an extra token when
        // relativeAddressingUsesToken() is true (SM>=2 source); wined3d
        // likewise reads a0 directly for vs_1_1 `c[a0.x + N]`. Clearing
        // has_relative here (the old behaviour) made vs_1_1 matrix-
        // palette skinning read c[N] = bone 0 for every vertex; T-pose
        // with detached joints. Keep it relative against a0.x.
        s.relative.base.type = DxsoRegisterType::Addr;
        s.relative.base.num = 0;
        s.relative.swizzle = DxsoRegSwizzle(); // identity → component .x
      }
    }
    if (out.src_count < kDxsoMaxOperandCount)
      out.src[out.src_count++] = s;
  };

  auto take_predicate = [&]() {
    if (cursor >= body_count)
      return;
    out.has_predicate = true;
    out.predicate = dxso_decode_src_register(body[cursor++]);
    // Predicate registers don't get rel-addr extras (DXVK 240-243),
    // so normalize the flag so the contract holds for predicate too.
    out.predicate.has_relative = false;
  };

  switch (opcode) {
  case DxsoOpcode::If:
  case DxsoOpcode::Ifc:
  case DxsoOpcode::Rep:
  case DxsoOpcode::Loop:
  case DxsoOpcode::BreakC:
  case DxsoOpcode::BreakP:
    // Pure-source shape: every body DWORD is a source register.
    while (cursor < body_count)
      take_src();
    return;

  case DxsoOpcode::Dcl:
    // body[0] is the semantic token; body[1] is the destination
    // register the semantic binds to. No rel-addr; dcls don't use
    // it.
    if (body_count >= 2) {
      out.has_dcl = true;
      out.dcl = dxso_decode_declaration(body[0]);
      out.has_dst = true;
      out.dst = dxso_decode_dst_register(body[1]);
    }
    return;

  case DxsoOpcode::Def:
  case DxsoOpcode::DefI:
  case DxsoOpcode::DefB: {
    // body[0] is the destination constant register; the remaining
    // body DWORDs hold the literal: 4 for Def/DefI, 1 for DefB.
    if (body_count >= 1) {
      out.has_dst = true;
      out.dst = dxso_decode_dst_register(body[0]);
    }
    out.has_def = true;
    out.def.kind = (opcode == DxsoOpcode::Def)    ? DxsoDefKind::Float32
                   : (opcode == DxsoOpcode::DefI) ? DxsoDefKind::Int32
                                                  : DxsoDefKind::Bool;
    // Zero unset lanes so DefB's three high lanes don't leak prior
    // junk.
    for (uint32_t i = 0; i < 4; ++i)
      out.def.payload.u32[i] = 0;
    uint32_t lit_count = body_count > 1 ? body_count - 1 : 0;
    if (lit_count > 4)
      lit_count = 4;
    for (uint32_t i = 0; i < lit_count; ++i)
      out.def.payload.u32[i] = static_cast<uint32_t>(body[1 + i]);
    return;
  }

  case DxsoOpcode::Comment:
  case DxsoOpcode::Phase:
  case DxsoOpcode::End:
    return;

  default:
    // Standard ALU / texture shape: dst, optional predicate, then
    // sources. Each register slot may consume an extra DWORD if its
    // relative-addressing flag is set.
    take_dst();
    if (out.predicated)
      take_predicate();
    while (cursor < body_count)
      take_src();
    return;
  }
}

class DxsoBytecodeIter {
public:
  // byte_code is the full blob (header + body + END). dword_count is
  // the inclusive length returned by shader_bytecode_dword_count.
  // header.major picks the length-resolution rule (SM1 default-table
  // vs SM2+ encoded). The iterator starts past the version DWORD;
  // the first call to next() yields the first body instruction.
  DxsoBytecodeIter(const uint32_t *byte_code, uint32_t dword_count, const DxsoHeader &header) :
      m_code(byte_code),
      m_count(dword_count),
      m_pos(1),
      m_major(header.major),
      m_minor(header.minor) {}

  bool
  next(DxsoInstruction &out) {
    if (m_pos >= m_count)
      return false;
    uint32_t token = static_cast<uint32_t>(m_code[m_pos]);
    DxsoOpcode opcode = static_cast<DxsoOpcode>(token & 0xFFFFu);

    out = DxsoInstruction{};
    out.offset_dwords = m_pos;
    out.opcode = opcode;

    uint32_t body_count = 0;
    if (opcode == DxsoOpcode::Comment) {
      // Comment body length is in bits 16..30 (15-bit field).
      body_count = (token & 0x7FFF0000u) >> 16;
    } else if (opcode == DxsoOpcode::End) {
      out.length_dwords = 1;
      m_pos = m_count;
      return true;
    } else if (opcode == DxsoOpcode::Phase) {
      // Phase has no parameters and no encoded length.
      body_count = 0;
    } else {
      // Real opcode: predicate / coissue flags only carry meaning
      // here; End/Comment/Phase reuse those bits for their own
      // length fields.
      out.predicated = (token & (1u << 28)) != 0;
      out.coissue = (token & (1u << 30)) != 0;
      out.specific_data = static_cast<uint8_t>((token & 0x00FF0000u) >> 16);
      if (m_major >= 2) {
        // SM2.0+: parameter count is in bits 24..27.
        body_count = (token & 0x0F000000u) >> 24;
      } else {
        // SM1.x: look up the per-opcode default length.
        body_count = dxso_default_opcode_length(opcode);
        if (body_count == UINT32_MAX)
          return false;
        // SM1.4 packs the texture coordinate source into the
        // instruction stream rather than using a separate texcoord
        // declaration like SM1.1-1.3: Tex and TexCoord pick up one
        // extra DWORD. DXVK dxso_decoder.cpp.
        if (m_major == 1 && m_minor == 4 && (opcode == DxsoOpcode::Tex || opcode == DxsoOpcode::TexCoord))
          body_count += 1;
      }
    }

    uint32_t length = 1u + body_count;
    if (m_pos + length > m_count)
      return false;
    out.length_dwords = length;

    // Operand decoding. m_pos is the opcode-token index; body DWORDs
    // start at +1. Relative-addressing extras live inside body_count
    // already (DXVK's encoded length accounts for them): the
    // operand decoder consumes them as part of the per-slot read.
    if (body_count > 0)
      dxso_decode_operands(opcode, m_code + m_pos + 1, body_count, m_major, out);

    m_pos += length;
    return true;
  }

  // Convenience: count the iterator's instructions (excluding End
  // and Comment). std::nullopt distinguishes "ran off the end without
  // hitting End" (malformed) from a legitimately empty body.
  static std::optional<uint32_t>
  count_instructions(const uint32_t *byte_code, uint32_t dword_count, const DxsoHeader &header) {
    DxsoBytecodeIter it(byte_code, dword_count, header);
    DxsoInstruction ins{};
    uint32_t n = 0;
    while (it.next(ins)) {
      if (ins.opcode == DxsoOpcode::End)
        return n;
      if (ins.opcode == DxsoOpcode::Comment)
        continue;
      ++n;
    }
    return std::nullopt;
  }

private:
  const uint32_t *m_code;
  uint32_t m_count;
  uint32_t m_pos;
  uint8_t m_major;
  uint8_t m_minor;
};

// dcl_<usage> entry collected at create time: the semantic itself
// plus the destination register the semantic was bound to.
struct DxsoBoundDcl {
  DxsoDeclaration dcl;
  DxsoBaseRegister bound_to;
  // The dcl dst token's write mask. A partial mask on a PS input
  // (dcl_texcoord0 v0.xyz) means the undeclared components read the
  // input-register defaults (0 for x/y/z, 1 for w) instead of the
  // interpolated value.
  DxsoRegMask mask;
};

// def / defi / defb entry collected at create time: the typed
// literal payload plus the constant register that takes it.
struct DxsoBoundConst {
  DxsoDefinition def;
  DxsoBaseRegister bound_to;
};

// Per-shader metadata gathered at CreateShader time: validation
// (iterator must reach End) and AIR-emission starting point.
// uses_kill: shader executes texkill; AIR must opt out of early tests.
// uses_derivatives: implicit-gradient ops (Tex/TexCoord/TexBem/DsX/DsY);
// explicit-LOD samples (TexLdd/TexLdl) excluded.
struct DxsoShaderMetadata {
  uint32_t instruction_count = 0;
  // Shader-model major from the version token; lets hosts gate
  // version-scoped variants (the fixed fog blend is a pre-SM3
  // contract) without re-parsing the bytecode header.
  uint32_t major = 0;
  std::vector<DxsoBoundDcl> dcls;
  std::vector<DxsoBoundConst> consts;
  bool uses_kill = false;
  bool uses_derivatives = false;
  // VS only: true when the shader writes its own point size, either
  // via SM1/2 `mov oPts, …` (RasterizerOut[2]) or SM3 `dcl_psize o#`
  // followed by a write to that register. The host uses this to skip
  // D3DRS_POINTSIZE auto-injection: when a VS computes its own
  // point size, the runtime D3DRS_POINTSIZE value is ignored per
  // D3D9 spec. False on PS / VS that doesn't touch oPts.
  bool writes_point_size = false;
  // VS only: true when the shader writes the fog factor, via SM1/2
  // `mov oFog, …` or an SM3 `dcl_fog o#`. Gates the PS fixed-fog-blend
  // variant: no fog write means factor 1.0 and an identity blend. The
  // PS-side `dcl_fog v#` input must not set it, hence the Output-file
  // requirement in the dcl arm below.
  bool writes_fog = false;
  // PS only: bitmask of stages where the shader uses TexBem / TexBemL /
  // Bem. The host uses this to thread D3DTSS_BUMPENVMAT00/01/10/11 +
  // LSCALE/LOFFSET into the PS variant key (and the compile chain) only
  // when the shader actually consumes the bump-env constants: most PS
  // shaders never touch TexBem so the variant cache stays unchanged for
  // them. SM1.x-only opcodes; mask covers stages 0..7 to match dxmt's
  // m_textureStageStates[8] storage.
  uint32_t bem_stage_mask = 0;
  // PS only: true when the shader writes oC1 (ColorOut register 1).
  // Dual-source blending borrows oC1 as attachment 0's second color
  // index; a PSO with Source1 blend factors and no index(1) shader
  // output fails Metal pipeline creation, so the host must only take
  // the dual-source path when the bound PS actually exports oC1.
  bool writes_oc1 = false;
  // True when any source reads c# through relative addressing
  // (`c[a0.x + N]` / `c[aL + N]`). Relative reads bypass the def-baked
  // literal table in the compiler, so the host must write the def
  // values into the uploaded float CB where the app never Set the
  // register; broader than DXVK's needsConstantCopies trigger, same mechanism
  // (src/dxso/dxso_isgn.h + d3d9_device.cpp UploadConstantSet).
  bool uses_relative_const = false;
  // Bitmask of sampler registers the shader actually samples. A texture
  // bound to a register outside this mask is a stale binding the shader
  // never reads, which lets the host drop it instead of treating it as a
  // live hazard (the depth-stencil-also-bound-as-texture case). Derived
  // from the tex opcodes rather than the dcls because SM1.0-1.3 declares
  // no samplers at all. Set for both stages; the bits are DXSO register
  // numbers, so a vertex shader's s0..s3 are the four
  // D3DVERTEXTEXTURESAMPLER slots, not texture stages 0..3.
  uint32_t sampler_usage_mask = 0;
  // Float-constant footprint: one past the highest float const register the
  // shader statically addresses, counting `def c#` and non-relative `c#`
  // reads but NOT relative `c[a0 + N]` / `c[aL + N]` reads. Relative reads
  // can land on any register, so folding them in would make every
  // relative-addressing shader look like it needs the extended file. A
  // vertex-shader value past D3D9_MAX_VS_CONST_F (256) means the shader
  // references the software-VP-only extended constant space (c256..); the
  // host reads it to gate a draw that binds such a shader while the device
  // is in hardware vertex processing.
  uint32_t max_float_const_index = 0;
};

// Opcodes whose sample / gradient is implicit; i.e. the GPU needs
// to compute derivatives across the 2x2 quad to pick the LOD. Mirrors
// DXVK src/dxso/dxso_analysis.cpp; explicit-LOD samples
// (TexLdd / TexLdl) are excluded because they bring their own
// gradient and don't constrain divergent control flow.
inline bool
dxso_opcode_uses_derivatives(DxsoOpcode op) {
  switch (op) {
  case DxsoOpcode::DsX:
  case DxsoOpcode::DsY:
  case DxsoOpcode::Tex:
  case DxsoOpcode::TexCoord:
  case DxsoOpcode::TexBem:
  case DxsoOpcode::TexBemL:
  case DxsoOpcode::TexReg2Ar:
  case DxsoOpcode::TexReg2Gb:
  case DxsoOpcode::TexM3x2Pad:
  case DxsoOpcode::TexM3x2Tex:
  case DxsoOpcode::TexM3x3Pad:
  case DxsoOpcode::TexM3x3Tex:
  case DxsoOpcode::TexM3x3Spec:
  case DxsoOpcode::TexM3x3VSpec:
  case DxsoOpcode::TexReg2Rgb:
  case DxsoOpcode::TexDp3Tex:
  case DxsoOpcode::TexM3x2Depth:
  case DxsoOpcode::TexDp3:
  case DxsoOpcode::TexM3x3:
  case DxsoOpcode::TexDepth:
    return true;
  default:
    return false;
  }
}

// Opcodes that read a texture through a sampler. Narrower than the
// derivative set: it drops the gradient-only DsX / DsY and the
// address-arithmetic forms (TexCoord, TexM3x2Pad, TexM3x3Pad,
// TexM3x2Depth, TexDp3, TexM3x3, TexDepth), which compute into a
// register without sampling, and adds the explicit-LOD TexLdl / TexLdd.
inline bool
dxso_opcode_samples(DxsoOpcode op) {
  switch (op) {
  case DxsoOpcode::Tex:
  case DxsoOpcode::TexLdl:
  case DxsoOpcode::TexLdd:
  case DxsoOpcode::TexReg2Ar:
  case DxsoOpcode::TexReg2Gb:
  case DxsoOpcode::TexReg2Rgb:
  case DxsoOpcode::TexBem:
  case DxsoOpcode::TexBemL:
  case DxsoOpcode::TexDp3Tex:
  case DxsoOpcode::TexM3x2Tex:
  case DxsoOpcode::TexM3x3Tex:
  case DxsoOpcode::TexM3x3Spec:
  case DxsoOpcode::TexM3x3VSpec:
    return true;
  default:
    return false;
  }
}

// Resolve the sampler slot a sampling instruction reads. SM2+ carries
// it in src[1]; SM1.4 `texld r#, t#` uses the destination register
// number as the sampler index; SM1.0-1.3 `tex t#` leaves src_count 0
// and puts the slot in the destination Texture register. Returns
// UINT32_MAX when no slot can be derived.
inline uint32_t
dxso_sampling_slot(const DxsoInstruction &ins, const DxsoHeader &header) {
  if (ins.src_count >= 2 && ins.src[1].base.type == DxsoRegisterType::Sampler)
    return ins.src[1].base.num;
  if (ins.has_dst && header.major == 1 && header.minor >= 4 && ins.opcode == DxsoOpcode::Tex)
    return ins.dst.base.num;
  if (ins.has_dst && ins.dst.base.type == DxsoRegisterType::Texture)
    return ins.dst.base.num;
  return UINT32_MAX;
}

// The software / mixed vertex-processing float constant ceiling: an SWVP
// device exposes c0..c8191 where a hardware-VP device stops at c0..c255.
// Kept here so the airconv compile path can widen the walk cap to the
// hardware maximum without pulling in the d3d9 device header.
inline constexpr uint32_t kDxsoMaxVsFloatConstSWVP = 8192;

// Walk the (already header-validated) bytecode end-to-end. Returns
// nullopt if the iterator can't reach End; that means the bytecode
// is malformed in a way that the simple shader_bytecode_dword_count
// scan didn't catch (e.g. an SM1 opcode that isn't in the default-
// length table).
inline std::optional<DxsoShaderMetadata>
walk_dxso_shader(
    const uint32_t *byte_code, uint32_t dword_count, const DxsoHeader &header, uint32_t vs_float_const_cap = 256u
) {
  DxsoShaderMetadata md;
  md.major = header.major;
  DxsoBytecodeIter it(byte_code, dword_count, header);
  DxsoInstruction ins{};
  while (it.next(ins)) {
    if (ins.opcode == DxsoOpcode::End)
      return md;
    if (ins.opcode == DxsoOpcode::Comment)
      continue;
    md.instruction_count += 1;
    if (ins.has_dcl && ins.has_dst) {
      // In ps_3_0 every input register (v#) carries an explicit usage, so
      // dcl_position0 on a v# input is a genuine (and malformed) primary-position
      // declaration: fragment position reaches a pixel shader only through the
      // dedicated vPos (MiscType) register, so native's assembler rejects it (wine
      // d3d9 maps that to INVALIDCALL at CreatePixelShader). A non-zero index
      // (dcl_position1..) is a valid user-tagged interpolated input. This must NOT
      // fire below ps_3_0: ps_1_x / ps_2_0 inputs have no explicit semantic and
      // their usage bits default to 0, so the same token is an ordinary colour
      // input the runtime accepts.
      if (header.kind == DxsoShaderKind::Pixel && header.major >= 3 && ins.dcl.usage == DxsoUsage::Position &&
          ins.dcl.usage_index == 0 && ins.dst.base.type == DxsoRegisterType::Input)
        return std::nullopt;
      md.dcls.push_back({ins.dcl, ins.dst.base, ins.dst.mask});
    }
    if (ins.has_def && ins.has_dst) {
      // Reject a constant-register index past the shader-model limit; wine
      // d3d9 fails Create{Vertex,Pixel}Shader for these and the host
      // validation maps nullopt to D3DERR_INVALIDCALL. Float consts: 256 on a
      // hardware-VP device, 8192 on software/mixed VP (the caller passes its
      // register count as vs_float_const_cap), and 8 / 32 / 224 on PS SM1 / SM2 /
      // SM3. Integer and boolean consts: 16 on VS SM2+ and PS SM3, absent before.
      const bool is_vs = header.kind == DxsoShaderKind::Vertex;
      uint32_t const_cap;
      if (ins.def.kind == DxsoDefKind::Float32) {
        const_cap = is_vs ? vs_float_const_cap : ((header.major >= 3) ? 224u : (header.major >= 2) ? 32u : 8u);
      } else {
        // Integer/boolean consts (16 each where present): VS gained them in
        // SM2, PS in ps_2_1 (the ps_2_x / ps_2_b static-flow-control profiles,
        // minor >= 1) and SM3.
        const bool has_int_bool =
            is_vs ? (header.major >= 2)
                  : (header.major >= 3 || (header.major == 2 && header.minor >= 1));
        const_cap = has_int_bool ? 16u : 0u;
      }
      if (ins.dst.base.num >= const_cap)
        return std::nullopt;
      md.consts.push_back({ins.def, ins.dst.base});
      if (ins.def.kind == DxsoDefKind::Float32 && ins.dst.base.num + 1 > md.max_float_const_index)
        md.max_float_const_index = ins.dst.base.num + 1;
    }
    if (ins.opcode == DxsoOpcode::TexKill)
      md.uses_kill = true;
    if (dxso_opcode_uses_derivatives(ins.opcode))
      md.uses_derivatives = true;
    for (uint32_t i = 0; i < ins.src_count; ++i) {
      if (ins.src[i].base.type != DxsoRegisterType::Const)
        continue;
      // A relative read can touch any register, so it does not extend the
      // static footprint; a direct read of c# does (its register + 1).
      if (ins.src[i].has_relative)
        md.uses_relative_const = true;
      else if (ins.src[i].base.num + 1 > md.max_float_const_index)
        md.max_float_const_index = ins.src[i].base.num + 1;
    }
    // VS oPts detection. SM1/2 emits `mov oPts, …` as a write to
    // RasterizerOut[2]; SM3 declares `dcl_psize o#` first then writes
    // the bound o#: record the dcl side here (DxsoUsage::PointSize)
    // and the bytecode side via the dst register check. PS bytecode
    // has no RasterizerOut nor PointSize dcls, so the gates stay false.
    if (ins.has_dst && ins.dst.base.type == DxsoRegisterType::RasterizerOut && ins.dst.base.num == 2)
      md.writes_point_size = true;
    if (ins.has_dcl && ins.has_dst && ins.dcl.usage == DxsoUsage::PointSize)
      md.writes_point_size = true;
    // VS oFog detection, same two encodings as oPts: SM1/2 writes
    // RasterizerOut[1], SM3 declares `dcl_fog o#`. Unlike PointSize the
    // dcl arm must require the Output register file: PS bytecode
    // declares fog as an INPUT (`dcl_fog v#`) and would otherwise set
    // the flag on pixel shaders.
    if (ins.has_dst && ins.dst.base.type == DxsoRegisterType::RasterizerOut && ins.dst.base.num == 1)
      md.writes_fog = true;
    if (ins.has_dcl && ins.has_dst && ins.dcl.usage == DxsoUsage::Fog &&
        ins.dst.base.type == DxsoRegisterType::Output)
      md.writes_fog = true;
    // PS TexBem / TexBemL / Bem stage tracking. TexBem and TexBemL
    // encode the destination texture stage in ins.dst.base.num; Bem
    // does the same. Track the bitmask so the host knows whether to
    // thread D3DTSS_BUMPENV* constants into the PS compile chain.
    if (ins.has_dst &&
        (ins.opcode == DxsoOpcode::TexBem || ins.opcode == DxsoOpcode::TexBemL || ins.opcode == DxsoOpcode::Bem) &&
        ins.dst.base.num < 32)
      md.bem_stage_mask |= (1u << ins.dst.base.num);
    // PS oC1 detection for the dual-source-blending gate. ColorOut
    // only exists in PS bytecode, so no stage guard is needed.
    if (ins.has_dst && ins.dst.base.type == DxsoRegisterType::ColorOut && ins.dst.base.num == 1)
      md.writes_oc1 = true;
    if (dxso_opcode_samples(ins.opcode)) {
      const uint32_t slot = dxso_sampling_slot(ins, header);
      if (slot < 16)
        md.sampler_usage_mask |= (1u << slot);
    }
  }
  // Iterator returned false without seeing End; bytecode is
  // malformed in a way that shader_bytecode_dword_count's scan-for-
  // 0xFFFF didn't catch (e.g. SM1 opcode not in the default-length
  // table, or a body whose declared length runs off the end).
  return std::nullopt;
}

} // namespace dxmt
