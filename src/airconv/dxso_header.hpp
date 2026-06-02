#pragma once

#include "d3d9.h"

#include <cstdint>
#include <optional>

namespace dxmt {

// DXSO header parser: reads version token at byte_code[0] and classifies it.
// Used by Create{Vertex,Pixel}Shader to reject malformed bytecode at create
// time. Header-only, no-LLVM; airconv will re-use for DXSO→AIR translation.

enum class DxsoShaderKind : uint8_t {
  Vertex,
  Pixel,
};

struct DxsoHeader {
  DxsoShaderKind kind;
  uint8_t major;
  uint8_t minor;
};

// Layout of the version token (DXVK dxso_common.h):
//   bits 31..16: 0xFFFE (vertex) or 0xFFFF (pixel)
//   bits 15..8:  major version
//   bits 7..0:   minor version
inline constexpr uint32_t kDxsoVertexTag = 0xFFFE0000u;
inline constexpr uint32_t kDxsoPixelTag = 0xFFFF0000u;
inline constexpr uint32_t kDxsoTagMask = 0xFFFF0000u;
inline constexpr uint32_t kDxsoMajorMask = 0x0000FF00u;
inline constexpr uint32_t kDxsoMinorMask = 0x000000FFu;

inline std::optional<DxsoHeader>
parse_dxso_header(const uint32_t *byte_code, size_t dword_count) {
  // Header (DWORD 0) + at least an END token (last DWORD = 0x0000FFFF).
  if (!byte_code || dword_count < 2)
    return std::nullopt;

  uint32_t v = static_cast<uint32_t>(byte_code[0]);
  DxsoShaderKind kind;
  if ((v & kDxsoTagMask) == kDxsoVertexTag) {
    kind = DxsoShaderKind::Vertex;
  } else if ((v & kDxsoTagMask) == kDxsoPixelTag) {
    kind = DxsoShaderKind::Pixel;
  } else {
    return std::nullopt;
  }
  uint8_t major = static_cast<uint8_t>((v & kDxsoMajorMask) >> 8);
  uint8_t minor = static_cast<uint8_t>(v & kDxsoMinorMask);

  // Valid major versions per the D3D9 spec: 1, 2, 3. Major 0 / 4+ are
  // invalid (D3D10 SM4 uses DXBC, not DXSO; the cross-API confusion
  // between SM3 vs SM4 is a real-world bug source).
  if (major < 1 || major > 3)
    return std::nullopt;

  // Minor caps mirror DXVK d3d9_device.cpp: vs_1_x ≤ 1,
  // ps_1_x ≤ 4, SM3 minor must be 0. SM2 minor is **deliberately
  // unchecked**: the FXC compiler emits 2_x / 2_a / 2_b profile
  // markers in the minor byte that aren't documented as discrete
  // values, and DXVK skips the check rather than enumerate them. We
  // mirror that to avoid rejecting real-world FXC blobs.
  if (major == 1) {
    uint8_t cap = (kind == DxsoShaderKind::Vertex) ? 1 : 4;
    if (minor > cap)
      return std::nullopt;
  } else if (major == 3) {
    if (minor != 0)
      return std::nullopt;
  }
  // major == 2: minor unchecked.

  return DxsoHeader{kind, major, minor};
}

} // namespace dxmt
