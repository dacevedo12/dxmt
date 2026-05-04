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
struct DXSO_SHADER_PS_BUMP_ENV_DATA;

namespace dxmt {

// Opaque handle backing dxso_shader_t in airconv_public.h. Owns a
// frozen copy of the DXSO bytecode plus the header + metadata
// gathered by the create-time walk; later passes (signature
// extraction, AIR emission) read from here.
//
// Mirrors airconv's SM50Shader (sm50_shader_t in dxbc_converter.cpp)
// in role — initialize parses + analyses, destroy frees — but the
// payload is DXSO-shaped (DxsoShaderMetadata) rather than DXBC.
struct DxsoShader {
  std::vector<uint32_t> bytecode;
  DxsoHeader header;
  DxsoShaderMetadata metadata;
};

// Allocate a DxsoShader from a raw bytecode blob. Returns nullptr if
// the header is malformed or the iterator can't reach End — caller
// is responsible for surfacing the failure.
DxsoShader *dxso_shader_initialize(const void *bytecode, size_t bytecode_size);

void dxso_shader_destroy(DxsoShader *shader);

// Emit DXSO into an externally-owned LLVM module. Caller has already
// called initializeModule on the target. Same role for DXSO that
// dxbc::convertDXBC plays for DXBC; airconv_cli drives both through
// the same optimization + linkXxx + output flow.
//
// ia_layout (optional) opts the VS into manual vertex fetch: each
// dcl_<usage> v# whose register matches an IA element's `reg`
// becomes a buffer-indexed pull instead of a [[stage_in]] attribute,
// driven by a vertex_buffers table at [[buffer(16)]]. NULL ⇒ legacy
// stage_in path; PS ignores the layout. Mirrors the
// SM50_SHADER_IA_INPUT_LAYOUT_DATA → SignatureContext::ia_layout
// dispatch in dxbc_signature.cpp.
void compile_dxso(
    DxsoShader *shader, const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout,
    const ::DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args, const ::DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *ps_samp_layout,
    bool ps_point_sprite, float vs_point_size_override, const ::DXSO_SHADER_PS_BUMP_ENV_DATA *ps_bump_env,
    const char *name, llvm::LLVMContext &context, llvm::Module &module
);

// Backing for dxso_bitcode_t — owns the AIR metallib bytes produced
// by DXSOCompile. Same role as airconv's SM50CompiledBitcode (no
// extra reflection payload yet; that grows as opcode lowering lands).
// Storage is llvm::SmallVector<char, 0> so MetallibWriter can write
// directly through raw_svector_ostream — bytes.data()/size() still
// satisfy the C ABI's `const void *` + `size_t`.
struct DxsoBitcode {
  llvm::SmallVector<char, 0> bytes;
};

} // namespace dxmt
