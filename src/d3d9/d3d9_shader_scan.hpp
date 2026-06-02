#pragma once

#include "d3d9.h"

#include <cstddef>

namespace dxmt {

// Total length, in DWORDs, of a D3D9 shader bytecode blob: the version
// token through the End token inclusive. Returns 0 for a malformed blob
// (bad version token, or no reachable End). CreateVertexShader /
// CreatePixelShader carry no length, so this delimits the bytecode to
// copy and walk. Kept in its own translation unit so it links against
// only the header-only DXSO decoder, which keeps it host-native testable.
size_t shader_bytecode_dword_count(const DWORD *byte_code);

} // namespace dxmt
