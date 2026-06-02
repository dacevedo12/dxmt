#include "d3d9_shader_scan.hpp"

#include "dxso_decoder.hpp"
#include "dxso_header.hpp"

namespace dxmt {

namespace {
// CreateVertexShader / CreatePixelShader carry no length, so the End
// token is the only delimiter. A blob with no reachable End would walk
// forever; cap at a size no SM1-3 shader approaches. Fully bounding a
// truncated blob would need a length-carrying decode context.
constexpr uint32_t kMaxScanDwords = 65536;
} // namespace

size_t
shader_bytecode_dword_count(const DWORD *byte_code) {
  if (!byte_code)
    return 0;
  const auto *words = reinterpret_cast<const uint32_t *>(byte_code);
  // Length comes from the canonical instruction walker, so it shares one
  // source of truth with the rest of the create path. A bespoke scan that
  // examined every DWORD is what let an operand aliasing the Comment
  // opcode (low 16 bits == 0xFFFE) be misread as a comment header and skip
  // past End into unmapped memory.
  auto header = parse_dxso_header(words, kMaxScanDwords);
  if (!header)
    return 0;
  DxsoBytecodeIter it(words, kMaxScanDwords, *header);
  DxsoInstruction ins{};
  while (it.next(ins)) {
    if (ins.opcode == DxsoOpcode::End)
      return ins.offset_dwords + 1;
  }
  return 0;
}

} // namespace dxmt
