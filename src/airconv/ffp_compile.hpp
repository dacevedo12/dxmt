#pragma once

#include "airconv_public.h"
#include "nt/air_builder.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace dxmt {

/* Snap an RGBA color destined for a LINEAR 8-bit UNORM render target to the
   nearest k/255 with round-half-to-even, so Metal's unorm-write conversion
   (which rounds an exact half away from zero) reproduces the same byte D3D/WARP
   does. rint() follows the current rounding mode (round-to-nearest-even): a
   channel landing exactly on k.5 (e.g. 76.5 -> 76) goes to the even k, and it
   moves off the ambiguous half onto an exact k/255 the unorm store round-trips
   unambiguously. For every off-half value the round is idempotent, so the snap
   only-improves and never regresses a value that already matched. Caller gates
   this on 8-bit unorm targets only: applying it to a float / HDR / sRGB
   attachment would quantise (or double-curve) and destroy precision. Shared by
   the generated fixed-function (ffp_compile.cpp) and the bytecode (dxso_compile.cpp)
   pixel epilogues. The DXBC path corrects the same divergence in its epilogue
   (pop_output_reg_fix_unorm, dxbc_converter_basicblock.cpp) with a constant
   half-delta subtract; rint is used here because it round-half-evens every k.5,
   where the delta only matches WARP at even k. */
inline llvm::Value *
emit_unorm8_snap(llvm::air::AIRBuilder &air, llvm::IRBuilder<> &builder, llvm::Value *color) {
  // ConstantFP::get returns a splat when the type is a vector, so one constant
  // serves the float4 outputs both epilogues assemble.
  auto *k255 = llvm::ConstantFP::get(color->getType(), 255.0);
  llvm::Value *sat = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, color);
  llvm::Value *rounded = air.CreateFPUnOp(llvm::air::AIRBuilder::rint, builder.CreateFMul(sat, k255));
  return builder.CreateFDiv(rounded, k255);
}

/* Generated fixed-function shaders. The vertex kind consumes the same
   IA layout argument the DXSO path uses (reg 0 = position, reg 1 =
   diffuse per the key contract in airconv_public.h) and the same
   auxiliary buffer bindings (clip planes at 3/4, viewport remap at 5,
   vertex_buffers table at 16); its uniforms block at buffer(0) carries
   the host-precomputed world*view*projection row matrix. The pixel
   kind is a COLOR0 passthrough until the stage-combiner milestones. */
void compile_ffp(
    const ::DXSO_SHADER_FFP_KEY_DATA *key, const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout,
    const ::DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args, int ps_fog_mode, bool ps_fog_coord_w, const char *name,
    llvm::LLVMContext &context, llvm::Module &module
);

} // namespace dxmt
