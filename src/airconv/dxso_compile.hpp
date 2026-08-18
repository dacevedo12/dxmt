#pragma once

#include "dxso_decoder.hpp"
#include "dxso_header.hpp"

#include "llvm/ADT/SmallVector.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace llvm {
class LLVMContext;
class Module;
} // namespace llvm

struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA;
struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA;
struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA;

namespace dxmt {

// Opaque handle backing dxso_shader_t in airconv_public.h. Owns a
// frozen copy of the DXSO bytecode plus the header + metadata
// gathered by the create-time walk; later passes (signature
// extraction, AIR emission) read from here.
//
// Mirrors airconv's SM50Shader (sm50_shader_t in dxbc_converter.cpp)
// in role: initialize parses + analyses, destroy frees; but the
// payload is DXSO-shaped (DxsoShaderMetadata) rather than DXBC.
struct DxsoShader {
  std::vector<uint32_t> bytecode;
  DxsoHeader header;
  DxsoShaderMetadata metadata;
};

// Allocate a DxsoShader from a raw bytecode blob. Returns nullptr if
// the header is malformed or the iterator can't reach End: caller
// is responsible for surfacing the failure.
DxsoShader *dxso_shader_initialize(const void *bytecode, size_t bytecode_size);

void dxso_shader_destroy(DxsoShader *shader);

// Emit DXSO into externally-owned LLVM module (caller calls initializeModule).
// ia_layout enables manual VS vertex fetch (buffer-indexed pull at [[buffer(16)]]);
// NULL uses legacy [[stage_in]]. PS ignores layout. Mirrors dxbc_signature.cpp.
void compile_dxso(
    DxsoShader *shader, const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout,
    const ::DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args, const ::DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *ps_samp_layout,
    bool ps_point_sprite, bool vs_inject_point_size, int ps_fog_mode, bool ps_fog_coord_w, const char *name,
    llvm::LLVMContext &context, llvm::Module &module
);

// Backing for dxso_bitcode_t: owns the AIR metallib bytes produced
// by DXSOCompile. Same role as airconv's SM50CompiledBitcode (no
// extra reflection payload yet; that grows as opcode lowering lands).
// Storage is llvm::SmallVector<char, 0> so MetallibWriter can write
// directly through raw_svector_ostream: bytes.data()/size() still
// satisfy the C ABI's `const void *` + `size_t`.
struct DxsoBitcode {
  llvm::SmallVector<char, 0> bytes;
};

} // namespace dxmt
