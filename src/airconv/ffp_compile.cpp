#include "ffp_compile.hpp"
#include "air_operations.hpp"
#include "air_signature.hpp"
#include "air_type.hpp"
#include "nt/air_builder.hpp"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <optional>
#include <string>
#include <vector>

namespace dxmt {

using namespace llvm;

namespace {

Constant *
splat4(LLVMContext &context, float x, float y, float z, float w) {
  Constant *lanes[4] = {
      ConstantFP::get(Type::getFloatTy(context), x), ConstantFP::get(Type::getFloatTy(context), y),
      ConstantFP::get(Type::getFloatTy(context), z), ConstantFP::get(Type::getFloatTy(context), w)
  };
  return ConstantVector::get(lanes);
}

// air.version / air.language_version / target triple: AGX rejects
// metallibs missing these; same values compile_dxso stamps.
void
stamp_metal_version(LLVMContext &context, Module &module) {
  auto u32_md = [&](uint32_t v) { return ConstantAsMetadata::get(ConstantInt::get(context, APInt{32, v})); };
  module.setTargetTriple("air64-apple-macosx15.0.0");
  module.getOrInsertNamedMetadata("air.version")->addOperand(MDTuple::get(context, {u32_md(2), u32_md(7), u32_md(0)}));
  module.getOrInsertNamedMetadata("air.language_version")
      ->addOperand(MDTuple::get(context, {MDString::get(context, "Metal"), u32_md(3), u32_md(2), u32_md(0)}));
}

/* The generated vertex function. Same manual-fetch model as the DXSO
   VS (no stage-in): position and diffuse come from the vertex_buffers
   argument table by the layout's offsets, the position goes through
   either the window->clip remap (POSITIONT layouts) or the uniforms
   block's world*view*projection row matrix, and the full legacy
   varying set is emitted so a bytecode pixel shader's claimed inputs
   are always fed (the same contract compile_dxso keeps). */
void
compile_ffp_vs(
    const ::DXSO_SHADER_FFP_KEY_DATA *key, const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout, const char *name,
    LLVMContext &context, Module &module
) {
  air::FunctionSignatureBuilder sig;
  const bool position_transformed = ia_layout && ia_layout->position_transformed;

  // Signature shape mirrors the DXSO manual-fetch VS exactly (argument
  // order and the full system-input set included): AGX's pipeline
  // linker is untolerant of shapes xcrun never emits, and the DXSO
  // order is the one proven against it. array_size = 1, not 0: AGX
  // reads the metadata's second integer as a binding count and rejects
  // 0 at PSO link.
  uint32_t vbuf_table_arg_idx = sig.DefineInput(
      air::ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = 16,
          .array_size = 1,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .type = air::msl_uint,
          .arg_name = "vertex_buffers",
          .raster_order_group = {},
      }
  );
  uint32_t vid_arg_idx = sig.DefineInput(air::InputVertexID{});
  uint32_t base_vertex_arg_idx = sig.DefineInput(air::InputBaseVertex{});
  uint32_t instance_id_arg_idx = sig.DefineInput(air::InputInstanceID{});
  uint32_t base_instance_arg_idx = sig.DefineInput(air::InputBaseInstance{});
  (void)base_vertex_arg_idx;
  (void)instance_id_arg_idx;
  (void)base_instance_arg_idx;
  uint32_t uniforms_arg_idx = sig.DefineInput(
      air::ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = 0,
          .array_size = 1,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .type = air::msl_float4,
          .arg_name = "ffp_uniforms",
          .raster_order_group = {},
      }
  );
  uint32_t clip_planes_arg_idx = sig.DefineInput(
      air::ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = 3,
          .array_size = 1,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .type = air::msl_float4,
          .arg_name = "clip_planes",
          .raster_order_group = {},
      }
  );
  uint32_t clip_count_arg_idx = sig.DefineInput(
      air::ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = 4,
          .array_size = 1,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .type = air::msl_uint,
          .arg_name = "clip_count",
          .raster_order_group = {},
      }
  );
  uint32_t vp_remap_arg_idx = ~0u;
  if (position_transformed) {
    vp_remap_arg_idx = sig.DefineInput(
        air::ArgumentBindingBuffer{
            .buffer_size = {},
            .location_index = 5,
            .array_size = 1,
            .memory_access = air::MemoryAccess::read,
            .address_space = air::AddressSpace::constant,
            .type = air::msl_float4,
            .arg_name = "vp_remap",
            .raster_order_group = {},
        }
    );
  }

  sig.DefineOutput(air::OutputPosition{.type = air::msl_float4});
  uint32_t clip_dist_field_idx = sig.DefineOutput(air::OutputClipDistance{.count = 8});
  // The full legacy varying set, matching the bytecode-PS stub tail
  // (COLOR0..1, TEXCOORD0..7, FOG0) so any pixel shader links.
  std::array<uint32_t, 2> oD_arg_idx{};
  std::array<uint32_t, 8> oT_arg_idx{};
  for (int i = 0; i < 2; ++i) {
    oD_arg_idx[i] = sig.DefineOutput(
        air::OutputVertex{
            .user = "COLOR" + std::to_string(i),
            .type = air::msl_float4,
        }
    );
  }
  for (int i = 0; i < 8; ++i) {
    oT_arg_idx[i] = sig.DefineOutput(
        air::OutputVertex{
            .user = "TEXCOORD" + std::to_string(i),
            .type = air::msl_float4,
        }
    );
  }
  uint32_t oFog_arg_idx = sig.DefineOutput(
      air::OutputVertex{
          .user = "FOG0",
          .type = air::msl_float4,
      }
  );
  // Pre-transformed passthrough varyings. A POSITIONT draw runs the
  // fixed-function vertex generator, yet its bytecode pixel shader can read
  // arbitrary decl semantics by name (blendweight / normal / tangent /
  // binormal / depth, plus the harmless blendindices). Emit one user-named
  // output per such usage so SM3's name linkage carries the raw attribute
  // through; the names match the DXSO PS stage-in user() names compile_dxso
  // defines. Gated on position_transformed, so an ordinary FFP draw keeps its
  // exact signature and every non-pre-transformed consumer stays byte-for-byte
  // unchanged. Index order matches the write site and the PS stub tail.
  static constexpr const char *kPassthroughUser[6] = {
      "BLENDWEIGHT0", "BLENDINDICES0", "NORMAL0", "TANGENT0", "BINORMAL0", "DEPTH0",
  };
  std::array<uint32_t, 6> passthrough_arg_idx{};
  passthrough_arg_idx.fill(~0u);
  if (position_transformed) {
    for (int i = 0; i < 6; ++i) {
      passthrough_arg_idx[i] = sig.DefineOutput(
          air::OutputVertex{
              .user = kPassthroughUser[i],
              .type = air::msl_float4,
          }
      );
    }
  }
  uint32_t oPts_arg_idx = ~0u;
  if (key->point_size != 0)
    oPts_arg_idx = sig.DefineOutput(air::OutputPointSize{});

  auto [fn, fn_md] = sig.CreateFunction(name, context, module, 0, false);
  IRBuilder<> builder(BasicBlock::Create(context, "entry", fn));
  raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(builder, nulldbg);

  auto *float4Ty = FixedVectorType::get(Type::getFloatTy(context), 4);
  auto *int4Ty = FixedVectorType::get(Type::getInt32Ty(context), 4);
  auto *fTy = Type::getFloatTy(context);
  air::AirType types(context);
  air::AIRBuilderContext abctx{
      .llvm = context,
      .module = module,
      .builder = builder,
      .types = types,
      .air = air,
  };

  // Fetch one element from the vertex_buffers table by the layout's
  // {slot, offset, format}; the entry index packing (popcount of slot
  // bits below the element's slot) matches the host's shared layout.
  auto fetch_element = [&](const DXSO_IA_INPUT_ELEMENT &element) -> Value * {
    auto *vbuf_table = builder.CreateBitCast(
        fn->getArg(vbuf_table_arg_idx),
        types._dxmt_vertex_buffer_entry->getPointerTo((uint32_t)air::AddressSpace::constant)
    );
    unsigned int shift = 32u - element.slot;
    unsigned int vbuf_entry_index = element.slot ? __builtin_popcount((ia_layout->slot_mask << shift) >> shift) : 0u;
    auto *vbuf_entry = builder.CreateLoad(
        types._dxmt_vertex_buffer_entry,
        builder.CreateConstGEP1_32(types._dxmt_vertex_buffer_entry, vbuf_table, vbuf_entry_index)
    );
    auto *base_addr = builder.CreateExtractValue(vbuf_entry, {0});
    auto *stride = builder.CreateExtractValue(vbuf_entry, {1});
    // [[vertex_id]] is the post-resolution vertex number; base_vertex
    // must not be added on top (dxso_compile carries the same note).
    Value *index = fn->getArg(vid_arg_idx);
    auto *byte_offset =
        builder.CreateAdd(builder.CreateMul(stride, index), builder.getInt32(element.aligned_byte_offset));
    auto result =
        air::pull_vec4_from_addr((air::MTLAttributeFormat)element.format, base_addr, byte_offset).build(abctx);
    if (auto err = result.takeError()) {
      llvm::consumeError(std::move(err));
      return ConstantAggregateZero::get(float4Ty);
    }
    Value *v = result.get();
    if (v->getType() == int4Ty)
      v = builder.CreateSIToFP(v, float4Ty);
    return v;
  };

  const DXSO_IA_INPUT_ELEMENT *pos_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *diffuse_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *texcoord0_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *specular_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *normal_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *blendweight_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *psize_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *texcoord_elements[8] = {};
  // Pre-transformed passthrough elements (regs 14..18 from ffp_input_register):
  // only consumed on the position_transformed path below, gathered here for a
  // uniform sweep of the layout.
  const DXSO_IA_INPUT_ELEMENT *tangent_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *binormal_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *depth_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *fog_element = nullptr;
  const DXSO_IA_INPUT_ELEMENT *blendindices_element = nullptr;
  if (ia_layout) {
    for (uint32_t i = 0; i < ia_layout->num_elements; ++i) {
      if (ia_layout->elements[i].reg == 0)
        pos_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 1)
        diffuse_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 2)
        texcoord0_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 3)
        specular_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 4)
        normal_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg >= 5 && ia_layout->elements[i].reg <= 11)
        texcoord_elements[ia_layout->elements[i].reg - 4] = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 12)
        blendweight_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 13)
        psize_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 14)
        tangent_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 15)
        binormal_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 16)
        depth_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 17)
        fog_element = &ia_layout->elements[i];
      else if (ia_layout->elements[i].reg == 18)
        blendindices_element = &ia_layout->elements[i];
    }
  }

  texcoord_elements[0] = texcoord0_element;
  Value *pos = pos_element ? fetch_element(*pos_element) : Constant::getNullValue(float4Ty);
  // D3DRS_VERTEXBLEND weight scalars, hoisted so the clip position and the
  // shared eye-space block below weight the same matrices identically.
  // Index i in [0, blend_count] holds matrix i's weight; the last is one
  // minus the declared sum (the wined3d vertex pipe shape). A missing weight
  // element reads zero, leaving the vertex on the last matrix. A
  // pre-transformed position never blends (the host forces vertex_blend 0).
  uint32_t blend_count = 0;
  std::vector<Value *> blend_weights;
  if (!position_transformed && key->vertex_blend != 0) {
    blend_count = key->vertex_blend > 3 ? 3u : key->vertex_blend;
    Value *weights = blendweight_element ? fetch_element(*blendweight_element) : Constant::getNullValue(float4Ty);
    Value *w_last = ConstantFP::get(fTy, 1.0);
    blend_weights.resize(blend_count + 1);
    for (uint32_t i = 0; i < blend_count; ++i) {
      blend_weights[i] = builder.CreateExtractElement(weights, builder.getInt32(i));
      w_last = builder.CreateFSub(w_last, blend_weights[i]);
    }
    blend_weights[blend_count] = w_last;
  }
  // The position the clip planes dot against. A POSITIONT draw defines its
  // clip planes in the same window space its vertices arrive in, so it dots
  // the raw pre-remap position (matches wined3d's transformed branch and DXVK
  // under the identity view POSITIONT implies); the world*VP path below resets
  // this to the clip-space position, where the host-packed (VP)^-1 plane lands.
  Value *clip_pos = pos;
  if (position_transformed) {
    // Window-space -> clip space + the rhw perspective setup; the same
    // remap compile_dxso appends for POSITIONT layouts.
    auto *vpPtr = fn->getArg(vp_remap_arg_idx);
    auto *invExtent = builder.CreateLoad(float4Ty, vpPtr);
    auto *invOffGep = builder.CreateGEP(float4Ty, vpPtr, builder.getInt32(1));
    auto *invOffset = builder.CreateLoad(float4Ty, invOffGep);
    pos = builder.CreateFAdd(builder.CreateFMul(pos, invExtent), invOffset);
    Value *w = builder.CreateExtractElement(pos, builder.getInt32(3));
    Value *isZero = builder.CreateFCmpOEQ(w, ConstantFP::get(fTy, 0.0));
    Value *rhw =
        builder.CreateSelect(isZero, ConstantFP::get(fTy, 1.0), builder.CreateFDiv(ConstantFP::get(fTy, 1.0), w));
    pos = builder.CreateFMul(pos, builder.CreateVectorSplat(4, rhw));
    pos = builder.CreateInsertElement(pos, rhw, builder.getInt32(3));
  } else {
    // Model space through the uniforms' world*view*projection rows:
    // out = v.x*row0 + v.y*row1 + v.z*row2 + row3 (v.w forced to 1; a
    // float3 position format already pads w = 1 but a float4 element
    // carrying garbage w must not skew the transform).
    auto *uPtr = fn->getArg(uniforms_arg_idx);
    auto mul_rows = [&](uint32_t base) -> Value * {
      Value *rows[4];
      for (uint32_t r = 0; r < 4; ++r)
        rows[r] = builder.CreateLoad(float4Ty, builder.CreateGEP(float4Ty, uPtr, builder.getInt32((int)(base + r))));
      Value *acc = rows[3];
      for (uint32_t c = 0; c < 3; ++c) {
        Value *lane = builder.CreateExtractElement(pos, builder.getInt32(c));
        acc = builder.CreateFAdd(acc, builder.CreateFMul(builder.CreateVectorSplat(4, lane), rows[c]));
      }
      return acc;
    };
    if (key->vertex_blend != 0) {
      // D3DRS_VERTEXBLEND: the position blends across world matrices
      // 0..blend_count with the hoisted per-matrix weights (the wined3d
      // vertex pipe shape); each matrix's world*view*projection is
      // host-folded at float4 0 (matrix 0) and float4 104 + 4*(i-1).
      Value *acc = nullptr;
      for (uint32_t i = 0; i <= blend_count; ++i) {
        Value *term = builder.CreateFMul(
            mul_rows(i == 0 ? 0u : 104u + (i - 1) * 4u), builder.CreateVectorSplat(4, blend_weights[i])
        );
        acc = acc ? builder.CreateFAdd(acc, term) : term;
      }
      pos = acc;
    } else {
      pos = mul_rows(0);
    }
    clip_pos = pos;
  }

  // Shared eye-space derivation (wined3d glsl_shader.c, DXVK
  // d3d9_fixed_function.cpp): every eye-space consumer reads the SAME eye
  // position and normal. Under D3DRS_VERTEXBLEND the vertex rides a
  // weighted blend of the world matrices, so both references blend the eye
  // position and normal across the same matrices the clip position uses;
  // deriving them once here keeps lighting, vertex fog, texgen and point
  // scale consistent instead of shading as if fully on world matrix 0.
  auto *uEye = fn->getArg(uniforms_arg_idx);
  auto load_u = [&](uint32_t idx) {
    return builder.CreateLoad(float4Ty, builder.CreateGEP(float4Ty, uEye, builder.getInt32((int)idx)));
  };
  auto dot4 = [&](Value *a, Value *b) {
    Value *p = builder.CreateFMul(a, b);
    Value *x = builder.CreateExtractElement(p, builder.getInt32(0));
    Value *y = builder.CreateExtractElement(p, builder.getInt32(1));
    Value *z = builder.CreateExtractElement(p, builder.getInt32(2));
    Value *w = builder.CreateExtractElement(p, builder.getInt32(3));
    return builder.CreateFAdd(builder.CreateFAdd(x, y), builder.CreateFAdd(z, w));
  };
  auto dot3 = [&](Value *a, Value *b) {
    Value *p = builder.CreateFMul(a, b);
    Value *x = builder.CreateExtractElement(p, builder.getInt32(0));
    Value *y = builder.CreateExtractElement(p, builder.getInt32(1));
    Value *z = builder.CreateExtractElement(p, builder.getInt32(2));
    return builder.CreateFAdd(builder.CreateFAdd(x, y), z);
  };
  auto vec3 = [&](Value *x, Value *y, Value *z) {
    Value *v = UndefValue::get(float4Ty);
    v = builder.CreateInsertElement(v, x, builder.getInt32(0));
    v = builder.CreateInsertElement(v, y, builder.getInt32(1));
    v = builder.CreateInsertElement(v, z, builder.getInt32(2));
    v = builder.CreateInsertElement(v, ConstantFP::get(fTy, 0.0), builder.getInt32(3));
    return v;
  };
  auto normalize3 = [&](Value *v) {
    // Zero-length guard mirrors wined3d's ffp_normalize: a degenerate
    // normal, a light coincident with the vertex, or a cancelled half-vector
    // must stay zero, not turn NaN via rsqrt(0).
    Value *len2 = dot3(v, v);
    Value *is0 = builder.CreateFCmpOEQ(len2, ConstantFP::get(fTy, 0.0));
    Value *inv = air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, len2);
    return builder.CreateSelect(is0, v, builder.CreateFMul(v, builder.CreateVectorSplat(4, inv)));
  };
  // NORMALIZENORMALS drives both the lighting normal (key bit 3) and the
  // generated-texcoord normal (texgen bit 24); they encode the same render
  // state, so either being set means the shared eye normal is normalized.
  const bool normalize_normals = (key->lighting_key & 8u) != 0 || (key->texgen_key & (1u << 24)) != 0;
  const bool wants_eye_space =
      pos_element && ((key->lighting_key & 1u) != 0 ||
                      (key->fog_vertex_mode >= 1 && key->fog_vertex_mode <= 3) || key->point_scale != 0 ||
                      ((key->texgen_key & 0xFFFFFFu) != 0 && !position_transformed));
  Value *eye_pos = nullptr;
  Value *eye_normal = Constant::getNullValue(float4Ty);
  if (wants_eye_space) {
    // Force the model position w to 1 before the eye-space dots: a FLOAT4
    // position decl can carry garbage w that would skew the dot4s (wined3d
    // builds vec4(pos.xyz, 1) for every eye-space use). A FLOAT3 decl
    // already pads w to 1 in the fetch.
    Value *pos_w1 =
        builder.CreateInsertElement(fetch_element(*pos_element), ConstantFP::get(fTy, 1.0), builder.getInt32(3));
    Value *n_w0 = nullptr;
    if (normal_element) {
      n_w0 = fetch_element(*normal_element);
      n_w0 = builder.CreateInsertElement(n_w0, ConstantFP::get(fTy, 0.0), builder.getInt32(3));
    }
    if (blend_count != 0) {
      // Blend the eye position and normal across the world matrices with
      // the same weights the clip position uses; gating on blend_count (not
      // the raw key) keeps this arm in lockstep with the weight population
      // above, so it never reads empty weights for a pre-transformed draw.
      // The normal blend stays on plain per-matrix world*view (both
      // references, no inverse-transpose in the blend arm). Matrix 0's
      // columns ride float4 6/7/4, the extra matrices' float4 116 + 3*(i-1).
      Value *ep = nullptr, *en = nullptr;
      for (uint32_t i = 0; i <= blend_count; ++i) {
        Value *cx, *cy, *cz;
        if (i == 0) {
          cx = load_u(6);
          cy = load_u(7);
          cz = load_u(4);
        } else {
          uint32_t b = 116u + (i - 1) * 3u;
          cx = load_u(b);
          cy = load_u(b + 1);
          cz = load_u(b + 2);
        }
        Value *wsplat = builder.CreateVectorSplat(4, blend_weights[i]);
        Value *ep_i = vec3(dot4(pos_w1, cx), dot4(pos_w1, cy), dot4(pos_w1, cz));
        ep = ep ? builder.CreateFAdd(ep, builder.CreateFMul(ep_i, wsplat)) : builder.CreateFMul(ep_i, wsplat);
        if (n_w0) {
          Value *en_i = vec3(dot4(n_w0, cx), dot4(n_w0, cy), dot4(n_w0, cz));
          en = en ? builder.CreateFAdd(en, builder.CreateFMul(en_i, wsplat)) : builder.CreateFMul(en_i, wsplat);
        }
      }
      eye_pos = ep;
      if (en)
        eye_normal = en;
    } else {
      eye_pos = vec3(dot4(pos_w1, load_u(6)), dot4(pos_w1, load_u(7)), dot4(pos_w1, load_u(4)));
      if (n_w0)
        // Inverse-transpose of world*view for the non-blend normal (float4
        // 125..127 = the rows of inverse(WV)); plain WV would skew lighting
        // under non-uniform scale, which NORMALIZENORMALS cannot repair.
        eye_normal = vec3(dot4(n_w0, load_u(125)), dot4(n_w0, load_u(126)), dot4(n_w0, load_u(127)));
    }
    if (normalize_normals)
      eye_normal = normalize3(eye_normal);
  }

  // Unlit diffuse default keyed on the DECL (wined3d utils.c): a bound
  // diffuse element feeds through, a declared-but-unbound diffuse renders
  // the zero attribute default (0,0,0,0), and a decl with no diffuse at all
  // takes the D3D9 opaque-white no-color default. The lit path overwrites
  // this with the accumulation, so only the unlit passthrough sees it.
  Value *diffuse;
  if (key->has_diffuse && diffuse_element)
    diffuse = fetch_element(*diffuse_element);
  else if (key->decl_has_diffuse)
    diffuse = splat4(context, 0.f, 0.f, 0.f, 0.f);
  else
    diffuse = splat4(context, 1.f, 1.f, 1.f, 1.f);

  // Vertex fog factor: the shared eye-space depth against the
  // D3DRS_FOGSTART / FOGEND / FOGDENSITY params (float4 5 in the uniforms
  // block). Same formulas as the table-fog pixel path; D3DFOG_EXP 1,
  // EXP2 2, LINEAR 3.
  Value *fog_factor = nullptr;
  // Modes 1..3 are the distance-fog formulas (EXP/EXP2/LINEAR); mode 4 is the
  // specular-alpha source, handled after the specular output is finalized.
  if (key->fog_vertex_mode >= 1 && key->fog_vertex_mode <= 3 && pos_element) {
    Value *fogp = load_u(5);
    // View-space depth is the shared eye position's z (dotting the model
    // position against the world*view z column is exactly eye_pos.z), so
    // vertex-blend fog rides the same blended depth as lighting. Under
    // D3DRS_RANGEFOGENABLE the fog coordinate is the radial eye-space
    // distance length(eye_pos.xyz) instead, so an object at the screen edge
    // fogs by its true distance rather than its z (wined3d
    // WINED3D_FFP_VS_FOG_RANGE = length(ec_pos.xyz); DXVK RangeFog). The eye
    // position is already computed above for lighting; range fog only forks
    // the coordinate, the EXP/EXP2/LINEAR formulas below are unchanged.
    Value *depth;
    if (key->range_fog) {
      depth = air.CreateFPUnOp(llvm::air::AIRBuilder::sqrt, dot3(eye_pos, eye_pos));
    } else {
      // The planar fog coordinate is the eye-space depth MAGNITUDE, not the
      // signed z: a primitive spanning the eye plane must fog by distance, so
      // a vertex behind the camera (negative view z) fogs the same as one the
      // same distance in front (wined3d glsl_shader.c abs(ec_pos.z), DXVK
      // d3d9_fixed_function.cpp opFAbs). The range-fog branch above already
      // takes a non-negative length, so only the planar coordinate needs it.
      depth = air.CreateFPUnOp(
          llvm::air::AIRBuilder::fabs, builder.CreateExtractElement(eye_pos, builder.getInt32(2))
      );
    }
    Value *fog_start = builder.CreateExtractElement(fogp, builder.getInt32(0));
    Value *fog_end = builder.CreateExtractElement(fogp, builder.getInt32(1));
    Value *fog_density = builder.CreateExtractElement(fogp, builder.getInt32(2));
    if (key->fog_vertex_mode == 3) {
      // D3DFOG_LINEAR is (end - depth) / (end - start). When start == end the
      // range collapses and D3D9 fogs the whole primitive instead of dividing
      // by zero (wined3d zeroes the vertex-fog scale). This is vertex fog only;
      // a nonzero fog_vertex_mode already implies FOGTABLEMODE is NONE.
      Value *num = builder.CreateFSub(fog_end, depth);
      Value *den = builder.CreateFSub(fog_end, fog_start);
      Value *collapsed = builder.CreateFCmpOEQ(fog_start, fog_end);
      fog_factor = builder.CreateSelect(
          collapsed, ConstantFP::get(fTy, 0.0), builder.CreateFDiv(num, den)
      );
    } else {
      constexpr double kLog2E = 1.4426950408889634;
      Value *dd = builder.CreateFMul(depth, fog_density);
      Value *exponent = key->fog_vertex_mode == 2 ? builder.CreateFMul(dd, dd) : dd;
      Value *neg = builder.CreateFNeg(builder.CreateFMul(exponent, ConstantFP::get(fTy, kLog2E)));
      fog_factor = air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, neg);
    }
    fog_factor = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, fog_factor);
  }

  Value *retval = UndefValue::get(fn->getReturnType());
  retval = builder.CreateInsertValue(retval, pos, {0});
  // Clip distances: dot(plane, clip_pos) for the host-packed enabled planes,
  // 0.0 (pass) beyond clip_count. D3D9 defines world*VP fixed-function clip
  // planes in world space (MSDN + DXVK d3d9_fixed_function.cpp); the host
  // pre-multiplies each plane by (View*Projection)^-1 before packing it, so
  // this dot against the clip-space position equals the world-space dot. A
  // POSITIONT draw instead clips in window space against the raw pre-remap
  // position (clip_pos), which pairs with the raw plane the host packs there.
  {
    auto *cdArrTy = ArrayType::get(fTy, 8);
    Value *cdArr = UndefValue::get(cdArrTy);
    auto *cpPtr = fn->getArg(clip_planes_arg_idx);
    auto *ccPtr = fn->getArg(clip_count_arg_idx);
    Value *count = builder.CreateLoad(Type::getInt32Ty(context), ccPtr);
    Value *zero_f = ConstantFP::get(fTy, 0.0);
    for (uint32_t i = 0; i < 8; ++i) {
      auto *gep = builder.CreateGEP(float4Ty, cpPtr, builder.getInt32(i));
      Value *plane = builder.CreateLoad(float4Ty, gep);
      Value *prod = builder.CreateFMul(plane, clip_pos);
      Value *x = builder.CreateExtractElement(prod, builder.getInt32(0));
      Value *y = builder.CreateExtractElement(prod, builder.getInt32(1));
      Value *z = builder.CreateExtractElement(prod, builder.getInt32(2));
      Value *w = builder.CreateExtractElement(prod, builder.getInt32(3));
      Value *dot = builder.CreateFAdd(builder.CreateFAdd(x, y), builder.CreateFAdd(z, w));
      Value *enabled = builder.CreateICmpULT(builder.getInt32(i), count);
      cdArr = builder.CreateInsertValue(cdArr, builder.CreateSelect(enabled, dot, zero_f), {i});
    }
    retval = builder.CreateInsertValue(retval, cdArr, {clip_dist_field_idx});
  }
  Value *specular =
      (key->has_specular && specular_element) ? fetch_element(*specular_element) : splat4(context, 0.f, 0.f, 0.f, 1.f);
  if ((key->lighting_key & 1u) && pos_element) {
    // Fixed-function vertex lighting (wined3d's equations): the shared
    // eye-space position and normal derived above, the material and light
    // array from the uniforms block (float4 10 on), accumulation over the
    // host-packed enabled lights.
    Value *mat_diffuse = load_u(10), *mat_ambient = load_u(11), *mat_specular = load_u(12), *mat_emissive = load_u(13);
    Value *mat_misc = load_u(14);
    Value *mat_power = builder.CreateExtractElement(mat_misc, builder.getInt32(0));
    Value *light_count_f = builder.CreateExtractElement(mat_misc, builder.getInt32(1));
    Value *global_ambient = load_u(15);
    // Material source selectors (COLORVERTEX): 0 material, 1 diffuse
    // input, 2 specular input. A selected input the layout does not
    // feed reads zero here, not the white passthrough default: wined3d
    // zero-fills every unbound attribute (context_gl.c), and a color
    // declared on an unbound stream keeps its selector.
    auto pick_source = [&](uint32_t sel, Value *mat_val) -> Value * {
      if (sel == 1)
        return (key->has_diffuse && diffuse_element) ? diffuse : Constant::getNullValue(float4Ty);
      if (sel == 2)
        return (key->has_specular && specular_element) ? specular : Constant::getNullValue(float4Ty);
      return mat_val;
    };
    uint32_t src_bits = (key->lighting_key >> 8) & 0xFF;
    Value *src_diffuse = pick_source(src_bits & 3, mat_diffuse);
    Value *src_specular = pick_source((src_bits >> 2) & 3, mat_specular);
    Value *src_ambient = pick_source((src_bits >> 4) & 3, mat_ambient);
    Value *src_emissive = pick_source((src_bits >> 6) & 3, mat_emissive);
    Value *acc_diffuse = builder.CreateFAdd(src_emissive, builder.CreateFMul(src_ambient, global_ambient));
    Value *acc_specular = Constant::getNullValue(float4Ty);
    for (uint32_t li = 0; li < 8; ++li) {
      Value *active = builder.CreateFCmpOLT(ConstantFP::get(fTy, (double)li), light_count_f);
      uint32_t base = 16 + li * 7;
      Value *l_diffuse = load_u(base), *l_specular = load_u(base + 1), *l_ambient = load_u(base + 2);
      Value *l_pos = load_u(base + 3), *l_dir = load_u(base + 4), *l_att = load_u(base + 5);
      Value *l_spot = load_u(base + 6);
      Value *l_type = builder.CreateExtractElement(l_dir, builder.getInt32(3));
      Value *is_directional = builder.CreateFCmpOEQ(l_type, ConstantFP::get(fTy, 3.0));
      Value *to_light = builder.CreateFSub(l_pos, eye_pos);
      Value *dist2 = dot3(to_light, to_light);
      Value *dist = air.CreateFPUnOp(llvm::air::AIRBuilder::sqrt, dist2);
      Value *L_point = normalize3(to_light);
      Value *L_dirl = normalize3(builder.CreateFNeg(l_dir));
      Value *L = builder.CreateSelect(is_directional, L_dirl, L_point);
      Value *a0 = builder.CreateExtractElement(l_att, builder.getInt32(0));
      Value *a1 = builder.CreateExtractElement(l_att, builder.getInt32(1));
      Value *a2 = builder.CreateExtractElement(l_att, builder.getInt32(2));
      Value *att_den =
          builder.CreateFAdd(a0, builder.CreateFAdd(builder.CreateFMul(a1, dist), builder.CreateFMul(a2, dist2)));
      Value *att_point = builder.CreateFDiv(ConstantFP::get(fTy, 1.0), att_den);
      Value *range = builder.CreateExtractElement(l_pos, builder.getInt32(3));
      Value *in_range = builder.CreateFCmpOLE(dist, range);
      att_point = builder.CreateSelect(in_range, att_point, ConstantFP::get(fTy, 0.0));
      Value *att = builder.CreateSelect(is_directional, ConstantFP::get(fTy, 1.0), att_point);
      // Spot cone: smooth falloff between the inner and outer cosines
      // (the host pre-halves and pre-cosines theta and phi).
      Value *is_spot = builder.CreateFCmpOEQ(l_type, ConstantFP::get(fTy, 2.0));
      Value *cos_angle = dot3(builder.CreateFNeg(L), normalize3(l_dir));
      Value *cos_theta = builder.CreateExtractElement(l_spot, builder.getInt32(0));
      Value *cos_phi = builder.CreateExtractElement(l_spot, builder.getInt32(1));
      Value *falloff = builder.CreateExtractElement(l_att, builder.getInt32(3));
      Value *spot_t =
          builder.CreateFDiv(builder.CreateFSub(cos_angle, cos_phi), builder.CreateFSub(cos_theta, cos_phi));
      spot_t = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, spot_t);
      Value *spot_pow = air.CreateFPBinOp(llvm::air::AIRBuilder::pow, spot_t, falloff);
      Value *inside = builder.CreateFCmpOGE(cos_angle, cos_theta);
      Value *spot_f = builder.CreateSelect(inside, ConstantFP::get(fTy, 1.0), spot_pow);
      // Outside the outer cone the falloff is zero, not pow(0, falloff): a
      // Falloff of 0 makes pow(0, 0) evaluate to 1 (or NaN under fast-math),
      // which would light the whole scene through the spot. Both references
      // force zero past the outer cone; do the same after the pow so the
      // theta==phi divide-by-zero cannot leak a NaN either.
      Value *outside = builder.CreateFCmpOLE(cos_angle, cos_phi);
      spot_f = builder.CreateSelect(outside, ConstantFP::get(fTy, 0.0), spot_f);
      att = builder.CreateSelect(is_spot, builder.CreateFMul(att, spot_f), att);
      // Clamp the diffuse dot to [0,1]: an unnormalized normal (length > 1
      // with NORMALIZENORMALS off) would otherwise over-brighten before the
      // final saturate. Both references clamp the diffuse dot; the specular
      // dot stays fmax-only (wined3d leaves it unclamped, and it is primary).
      Value *ndotl = air.CreateFPBinOp(
          llvm::air::AIRBuilder::fmin,
          air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, dot3(eye_normal, L), ConstantFP::get(fTy, 0.0)),
          ConstantFP::get(fTy, 1.0)
      );
      Value *att4 = builder.CreateVectorSplat(4, builder.CreateSelect(active, att, ConstantFP::get(fTy, 0.0)));
      Value *contrib = builder.CreateFMul(
          builder.CreateFAdd(
              builder.CreateFMul(src_ambient, l_ambient),
              builder.CreateFMul(builder.CreateFMul(src_diffuse, l_diffuse), builder.CreateVectorSplat(4, ndotl))
          ),
          att4
      );
      acc_diffuse = builder.CreateFAdd(acc_diffuse, contrib);
      if (key->lighting_key & 4u) {
        // Blinn half-vector against the viewer direction: the local
        // viewer normalizes toward the eye, otherwise the fixed -z.
        Value *view_dir = (key->lighting_key & 16u)
                              ? normalize3(builder.CreateFNeg(eye_pos))
                              : vec3(ConstantFP::get(fTy, 0.0), ConstantFP::get(fTy, 0.0), ConstantFP::get(fTy, -1.0));
        Value *half_v = normalize3(builder.CreateFAdd(L, view_dir));
        Value *ndoth =
            air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, dot3(eye_normal, half_v), ConstantFP::get(fTy, 0.0));
        Value *spec_pow = air.CreateFPBinOp(llvm::air::AIRBuilder::pow, ndoth, mat_power);
        // The specular term only applies when the light hits the front
        // face and the half-vector does; a zero power would otherwise
        // contribute full white through pow(x, 0). wined3d glsl_shader.c
        // gates on both dots the same way.
        Value *lit = builder.CreateAnd(
            builder.CreateFCmpOGT(ndotl, ConstantFP::get(fTy, 0.0)),
            builder.CreateFCmpOGT(ndoth, ConstantFP::get(fTy, 0.0))
        );
        spec_pow = builder.CreateSelect(lit, spec_pow, ConstantFP::get(fTy, 0.0));
        Value *scontrib = builder.CreateFMul(
            builder.CreateFMul(builder.CreateFMul(src_specular, l_specular), builder.CreateVectorSplat(4, spec_pow)),
            att4
        );
        acc_specular = builder.CreateFAdd(acc_specular, scontrib);
      }
    }
    auto saturate4v = [&](Value *v) {
      Value *lo = Constant::getNullValue(float4Ty);
      Value *hi = splat4(context, 1.f, 1.f, 1.f, 1.f);
      return air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, v, lo), hi);
    };
    // The lit alpha clamps like the color accumulators; a vertex color
    // above one must not leak an alpha beyond one (wined3d and DXVK
    // both saturate it).
    Value *lit_alpha = builder.CreateExtractElement(saturate4v(src_diffuse), builder.getInt32(3));
    diffuse = saturate4v(acc_diffuse);
    diffuse = builder.CreateInsertElement(diffuse, lit_alpha, builder.getInt32(3));
    // Only replace the specular output with the computed accumulation when
    // SPECULARENABLE is on. With it off both references pass the vertex
    // specular (COLOR1) through unchanged; overwriting with the zero
    // accumulation would black out any combiner stage reading D3DTA_SPECULAR.
    if (key->lighting_key & 4u)
      specular = saturate4v(acc_specular);
  }
  retval = builder.CreateInsertValue(retval, diffuse, {oD_arg_idx[0]});
  retval = builder.CreateInsertValue(retval, specular, {oD_arg_idx[1]});
  // Fog mode 4: the fog factor is the vertex specular alpha, emitted on the
  // smooth oFog varying (written below) so it interpolates independently of the
  // flat-shaded specular color. The distance modes set fog_factor above.
  if (key->fog_vertex_mode == 4)
    fog_factor = builder.CreateExtractElement(specular, builder.getInt32(3));
  // Texcoord outputs: forwarded per the mask (texcoord 0 keeps its
  // original key bit), optionally through the stage's texture matrix
  // with the count and projection flags. The transform pads the input
  // to (u, v, 1, 1) for the affine 2D case. A texgen stage replaces
  // the fetch with an eye-space generated coordinate (wined3d
  // glsl_shader.c): the eye position takes the affine world*view
  // columns (a projective modelview w stays a marked corner, as does
  // a generated stage colliding with a passthrough consumer of the
  // same varying slot).
  Value *tg_eye_pos = nullptr;
  Value *tg_normal = nullptr;
  auto tg_dot3 = [&](Value *a, Value *b) {
    Value *p = builder.CreateFMul(a, b);
    Value *x = builder.CreateExtractElement(p, builder.getInt32(0));
    Value *y = builder.CreateExtractElement(p, builder.getInt32(1));
    Value *z = builder.CreateExtractElement(p, builder.getInt32(2));
    return builder.CreateFAdd(builder.CreateFAdd(x, y), z);
  };
  // The zero-length guard mirrors wined3d's ffp_normalize; a missing
  // normal must stay zero instead of turning NaN.
  auto tg_normalize3 = [&](Value *v) {
    Value *len2 = tg_dot3(v, v);
    Value *is0 = builder.CreateFCmpOEQ(len2, ConstantFP::get(fTy, 0.0));
    Value *inv = air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, len2);
    return builder.CreateSelect(is0, v, builder.CreateFMul(v, builder.CreateVectorSplat(4, inv)));
  };
  auto tg_reflect = [&](Value *in, Value *n) {
    Value *d = tg_dot3(n, in);
    Value *two_d = builder.CreateFAdd(d, d);
    return builder.CreateFSub(in, builder.CreateFMul(n, builder.CreateVectorSplat(4, two_d)));
  };
  if ((key->texgen_key & 0xFFFFFFu) != 0 && !position_transformed && pos_element) {
    // Generated texture coordinates read the same shared eye-space position
    // and normal the lighting path uses (wined3d derives the eye normal
    // once for both); the normal already carries the inverse-transpose and
    // the NORMALIZENORMALS normalization from the shared block above.
    tg_eye_pos = eye_pos;
    tg_normal = eye_normal;
  }
  for (int i = 0; i < 8; ++i) {
    uint32_t set = (key->texcoord_index_key >> (i * 3)) & 7u;
    bool declared = set == 0 ? (key->has_texcoord0 != 0) : ((key->texcoord_mask >> set) & 1) != 0;
    uint32_t tg = (key->texgen_key >> (i * 3)) & 7u;
    Value *v = nullptr;
    if (tg != 0 && tg_eye_pos) {
      Value *one = ConstantFP::get(fTy, 1.0);
      switch (tg) {
      case 1: /* D3DTSS_TCI_CAMERASPACENORMAL */
        v = builder.CreateInsertElement(tg_normal, one, builder.getInt32(3));
        break;
      case 2: /* D3DTSS_TCI_CAMERASPACEPOSITION */
        v = builder.CreateInsertElement(tg_eye_pos, one, builder.getInt32(3));
        break;
      case 3: /* D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR */
        v = builder.CreateInsertElement(tg_reflect(tg_normalize3(tg_eye_pos), tg_normal), one, builder.getInt32(3));
        break;
      case 4: {
        /* D3DTSS_TCI_SPHEREMAP; the r.z - 1 form is wined3d's. */
        Value *r = tg_reflect(tg_normalize3(tg_eye_pos), tg_normal);
        Value *rx = builder.CreateExtractElement(r, builder.getInt32(0));
        Value *ry = builder.CreateExtractElement(r, builder.getInt32(1));
        Value *rz1 = builder.CreateFSub(builder.CreateExtractElement(r, builder.getInt32(2)), one);
        Value *m2 = builder.CreateFAdd(
            builder.CreateFAdd(builder.CreateFMul(rx, rx), builder.CreateFMul(ry, ry)), builder.CreateFMul(rz1, rz1)
        );
        Value *m = builder.CreateFMul(ConstantFP::get(fTy, 2.0), air.CreateFPUnOp(llvm::air::AIRBuilder::sqrt, m2));
        Value *half_c = ConstantFP::get(fTy, 0.5);
        Value *u = builder.CreateFAdd(builder.CreateFDiv(rx, m), half_c);
        Value *sv = builder.CreateFAdd(builder.CreateFDiv(ry, m), half_c);
        v = UndefValue::get(float4Ty);
        v = builder.CreateInsertElement(v, u, builder.getInt32(0));
        v = builder.CreateInsertElement(v, sv, builder.getInt32(1));
        v = builder.CreateInsertElement(v, ConstantFP::get(fTy, 0.0), builder.getInt32(2));
        v = builder.CreateInsertElement(v, one, builder.getInt32(3));
        break;
      }
      default:
        break;
      }
    } else if (declared && texcoord_elements[set])
      v = fetch_element(*texcoord_elements[set]);
    uint32_t tf = (key->texcoord_transform_key >> (i * 4)) & 0xF;
    if (v && (tf & 1u)) {
      // The host bakes the count, projection and attribute-width
      // semantics into the matrix (wined3d utils.c
      // compute_texture_matrix); the shader side is a plain row-vector
      // multiply of the fetched coordinate, whose missing components
      // already read (0, 0, 1) from the vertex fetch. The projective
      // divide happens at the fragment sample.
      auto *uPtr2 = fn->getArg(uniforms_arg_idx);
      Value *acc = nullptr;
      for (uint32_t r = 0; r < 4; ++r) {
        Value *row =
            builder.CreateLoad(float4Ty, builder.CreateGEP(float4Ty, uPtr2, builder.getInt32((int)(72 + i * 4 + r))));
        Value *lane = builder.CreateExtractElement(v, builder.getInt32(r));
        Value *term = builder.CreateFMul(builder.CreateVectorSplat(4, lane), row);
        acc = acc ? builder.CreateFAdd(acc, term) : term;
      }
      v = acc;
    }
    if (!v)
      v = splat4(context, 0.f, 0.f, 0.f, 1.f);
    retval = builder.CreateInsertValue(retval, v, {oT_arg_idx[i]});
  }
  // FOG0 carries the vertex-fog value in x and the table (pixel) fog coordinate
  // in y. A pre-transformed draw with a fog decl attribute forwards that raw
  // attribute WHOLE (all lanes, wined3d pretransformed passthrough) so the SM3
  // PS reads it via dcl_fog v#; the coordinate lane is not layered on, or it
  // would corrupt the green channel that path samples. Every other draw carries
  // the computed factor / zero in x and the vertex-output Z (clip_pos.z) in y,
  // which the PS fog epilogue reads for the non-w (ortho / pre-transformed)
  // path. clip_pos.z is the clip-space Z for a world*view*projection draw and
  // the raw window-space input Z for a POSITIONT draw, matching wined3d's
  // ffp_varying_fogcoord = gl_Position.z (ortho) / ec_pos.z (transformed) in
  // glsl_shader.c. That varying keeps the coordinate off the post-perspective
  // device Z and clear of the rasterizer depth bias.
  Value *fog_out;
  if (position_transformed && fog_element) {
    fog_out = fetch_element(*fog_element);
  } else {
    fog_out = fog_factor ? builder.CreateVectorSplat(4, fog_factor) : Constant::getNullValue(float4Ty);
    Value *fog_coord = builder.CreateExtractElement(clip_pos, builder.getInt32(2));
    fog_out = builder.CreateInsertElement(fog_out, fog_coord, builder.getInt32(1));
  }
  retval = builder.CreateInsertValue(retval, fog_out, {oFog_arg_idx});
  // Pre-transformed passthrough writes: forward each fetched decl attribute to
  // the matching user-named output defined above. Index order matches
  // kPassthroughUser (BLENDWEIGHT0, BLENDINDICES0, NORMAL0, TANGENT0,
  // BINORMAL0, DEPTH0). An unbound usage writes the zero attribute default,
  // the same seed an undeclared varying carries.
  if (position_transformed) {
    const DXSO_IA_INPUT_ELEMENT *passthrough_element[6] = {
        blendweight_element, blendindices_element, normal_element,
        tangent_element,     binormal_element,     depth_element,
    };
    for (int i = 0; i < 6; ++i) {
      Value *v = passthrough_element[i] ? fetch_element(*passthrough_element[i]) : Constant::getNullValue(float4Ty);
      retval = builder.CreateInsertValue(retval, v, {passthrough_arg_idx[i]});
    }
  }
  if (oPts_arg_idx != ~0u) {
    Value *point_size;
    if (key->point_scale != 0 && pos_element) {
      // D3DRS_POINTSCALEENABLE: size scales by the viewport height over
      // sqrt(A + B*d + C*d*d) with d the shared eye-space distance, then
      // the min/max clamp applies (wined3d's process_vertices shape).
      Value *dist2 = dot3(eye_pos, eye_pos);
      Value *dist = air.CreateFPUnOp(llvm::air::AIRBuilder::sqrt, dist2);
      Value *scale_abc = load_u(8);
      Value *size_mm = load_u(9);
      Value *a = builder.CreateExtractElement(scale_abc, builder.getInt32(0));
      Value *b = builder.CreateExtractElement(scale_abc, builder.getInt32(1));
      Value *c = builder.CreateExtractElement(scale_abc, builder.getInt32(2));
      Value *vph = builder.CreateExtractElement(scale_abc, builder.getInt32(3));
      Value *denom =
          builder.CreateFAdd(a, builder.CreateFAdd(builder.CreateFMul(b, dist), builder.CreateFMul(c, dist2)));
      Value *inv = air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, denom);
      Value *base = builder.CreateExtractElement(size_mm, builder.getInt32(0));
      if (key->point_size_per_vertex != 0)
        // A declared PSIZE attribute supplies the base size the attenuation
        // scales (wined3d's per-vertex arm); an unbound PSIZE stream, which the
        // ia_layout carries no element for, reads as 1 (AMD / WARP).
        base = psize_element ? builder.CreateExtractElement(fetch_element(*psize_element), builder.getInt32(0))
                             : ConstantFP::get(fTy, 1.0);
      point_size = builder.CreateFMul(builder.CreateFMul(base, vph), inv);
      Value *mn = builder.CreateExtractElement(size_mm, builder.getInt32(1));
      Value *mx = builder.CreateExtractElement(size_mm, builder.getInt32(2));
      point_size = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, point_size, mn);
      point_size = air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, point_size, mx);
    } else if (key->point_size_per_vertex != 0) {
      // Per-vertex D3DDECLUSAGE_PSIZE: the attribute is the size, clamped by the
      // bounds the uniforms carry. A PSIZE declared on an unfed stream carries
      // no layout element; D3D9 then reads the missing size as 1 (AMD / WARP),
      // not the render-state size.
      auto *uPtr = fn->getArg(uniforms_arg_idx);
      Value *size_mm = builder.CreateLoad(float4Ty, builder.CreateGEP(float4Ty, uPtr, builder.getInt32(9)));
      point_size = psize_element ? builder.CreateExtractElement(fetch_element(*psize_element), builder.getInt32(0))
                                 : ConstantFP::get(fTy, 1.0);
      point_size = air.CreateFPBinOp(
          llvm::air::AIRBuilder::fmax, point_size, builder.CreateExtractElement(size_mm, builder.getInt32(1))
      );
      point_size = air.CreateFPBinOp(
          llvm::air::AIRBuilder::fmin, point_size, builder.CreateExtractElement(size_mm, builder.getInt32(2))
      );
    } else {
      // Plain render-state size: read D3DRS_POINTSIZE from the uniform
      // (float4 9 lane 0) and clamp it against the same min/max the scale
      // arm uses, so one generated variant serves every point size rather
      // than baking the value into the key.
      auto *uPtr = fn->getArg(uniforms_arg_idx);
      Value *size_mm = builder.CreateLoad(float4Ty, builder.CreateGEP(float4Ty, uPtr, builder.getInt32(9)));
      point_size = builder.CreateExtractElement(size_mm, builder.getInt32(0));
      point_size = air.CreateFPBinOp(
          llvm::air::AIRBuilder::fmax, point_size, builder.CreateExtractElement(size_mm, builder.getInt32(1))
      );
      point_size = air.CreateFPBinOp(
          llvm::air::AIRBuilder::fmin, point_size, builder.CreateExtractElement(size_mm, builder.getInt32(2))
      );
    }
    retval = builder.CreateInsertValue(retval, point_size, {oPts_arg_idx});
  }
  builder.CreateRet(retval);

  module.getOrInsertNamedMetadata("air.vertex")->addOperand(fn_md);
}

/* The generated pixel function: interpolated COLOR0 straight to the
   colour attachment. Texturing and the stage combiners join in the
   following milestones; alpha test will ride the same variant axis
   the bytecode pixel shaders use rather than a fork here. */
void
compile_ffp_ps(
    const ::DXSO_SHADER_FFP_KEY_DATA *key, const ::DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args, int ps_fog_mode,
    bool ps_fog_coord_w, const char *name, LLVMContext &context, Module &module
) {
  (void)key;
  const bool emit_alpha_test = ps_args != nullptr && ps_args->alpha_test_func != 8 /* D3DCMP_ALWAYS */;
  // D3DRS_MULTISAMPLEMASK: the host set this only on a maskable multisample
  // target with a non-all-ones mask, so emit the [[sample_mask]] coverage
  // output and AND the mask word (blob tail, uint32 index 29) into hardware
  // coverage. Same bounded-gate / uniform-value split the alpha ref uses.
  const bool emit_sample_mask = key->emit_sample_mask != 0;
  air::FunctionSignatureBuilder sig;
  // AGX links a fragment function only when every VS user output is
  // claimed by name and [[position]] leads the stage-in args (the
  // pipeline-link trap notes); claim the same full legacy tail the
  // bytecode pixel shaders do, since either shader of the pair may be
  // generated while the other came from bytecode.
  uint32_t position_arg_idx = sig.DefineInput(
      air::InputPosition{
          .interpolation = air::Interpolation::center_no_perspective,
      }
  );
  uint32_t color0_arg_idx = 0;
  uint32_t color1_arg_idx = 0;
  for (int i = 0; i < 2; ++i) {
    uint32_t idx = sig.DefineInput(
        air::InputFragmentStageIn{
            .user = "COLOR" + std::to_string(i),
            .type = air::msl_float4,
            // D3DSHADE_FLAT holds the colors at the first vertex's
            // value; Metal's flat interpolation uses the same
            // provoking vertex.
            .interpolation = key->flat_shading != 0 ? air::Interpolation::flat : air::Interpolation::center_perspective,
            .pull_mode = false,
        }
    );
    if (i == 0)
      color0_arg_idx = idx;
    else
      color1_arg_idx = idx;
  }
  uint32_t texcoord_arg_idx[8] = {};
  for (int i = 0; i < 8; ++i) {
    texcoord_arg_idx[i] = sig.DefineInput(
        air::InputFragmentStageIn{
            .user = "TEXCOORD" + std::to_string(i),
            .type = air::msl_float4,
            .interpolation = air::Interpolation::center_perspective,
            .pull_mode = false,
        }
    );
  }
  uint32_t texcoord0_arg_idx = texcoord_arg_idx[0];
  uint32_t fog0_arg_idx = sig.DefineInput(
      air::InputFragmentStageIn{
          .user = "FOG0",
          .type = air::msl_float4,
          .interpolation = air::Interpolation::center_perspective,
          .pull_mode = false,
      }
  );
  // Point sprites: [[point_coord]] replaces the texcoord sample
  // coordinate; the same unconditional substitution the bytecode PS
  // variant applies (D3D9 replaces all texcoord reads uniformly).
  uint32_t point_coord_arg_idx = ~0u;
  if (key->point_sprite != 0)
    point_coord_arg_idx = sig.DefineInput(air::InputPointCoord{});
  // The fog blend reads its color and table params from the packed
  // bool-constant blob the host binds at fragment buffer 2 for every
  // draw (same layout contract as the bytecode pixel path); the
  // combiner also reads the per-sampler D3DSAMP_MIPMAPLODBIAS floats
  // from its tail, and the alpha test reads the normalised ref from it,
  // so the blob binds in any of those modes.
  uint32_t bc_arg_idx = ~0u;
  if (ps_fog_mode >= 0 || key->tex0_mode == 2 || emit_alpha_test || emit_sample_mask) {
    bc_arg_idx = sig.DefineInput(
        air::ArgumentBindingBuffer{
            .buffer_size = {},
            .location_index = 2,
            .array_size = 1,
            .memory_access = air::MemoryAccess::read,
            .address_space = air::AddressSpace::constant,
            .type = air::msl_uint,
            .arg_name = "b",
            .raster_order_group = {},
        }
    );
  }
  // Stage-0 texturing: texture and sampler bind at index 0 like a
  // bytecode PS's s0, sampled at TEXCOORD0 and modulated with the
  // diffuse (color and alpha; the combiner table refines per-op in a
  // later milestone). Bindings only exist in this mode so the plain
  // pass-through variant keeps its minimal signature.
  // Combiner mode: one texture+sampler binding per stage that carries a
  // bound texture, and the constants buffer (texture factor at float4 0,
  // per-stage D3DTA_CONSTANT colors at 1..8) at fragment buffer 0.
  int stage_tex_arg_idx[8];
  int stage_samp_arg_idx[8];
  for (int i = 0; i < 8; ++i)
    stage_tex_arg_idx[i] = stage_samp_arg_idx[i] = -1;
  uint32_t ps_consts_arg_idx = ~0u;
  if (key->tex0_mode == 2) {
    for (int i = 0; i < 8; ++i) {
      if (!(key->stages[i][0] & (1u << 16)))
        continue;
      // The host-resolved sampler kind picks the binding's texture
      // dimensionality; cube and volume textures on a combiner stage
      // sample with a three-component coordinate below.
      uint32_t kind_i = (key->sampler_kind_key >> (i * 4)) & 0xFu;
      air::TextureKind res_kind = kind_i == 3   ? air::TextureKind::texture_cube
                                  : kind_i == 4 ? air::TextureKind::texture_3d
                                                : air::TextureKind::texture_2d;
      stage_tex_arg_idx[i] = (int)sig.DefineInput(
          air::ArgumentBindingTexture{
              .location_index = (uint32_t)i,
              .array_size = 1,
              .memory_access = air::MemoryAccess::sample,
              .type =
                  air::MSLTexture{
                      .component_type = air::msl_float,
                      .memory_access = air::MemoryAccess::sample,
                      .resource_kind = res_kind,
                      .resource_kind_logical = res_kind,
                  },
              .arg_name = "t" + std::to_string(i),
              .raster_order_group = {},
          }
      );
      stage_samp_arg_idx[i] = (int)sig.DefineInput(
          air::ArgumentBindingSampler{
              .location_index = (uint32_t)i,
              .array_size = 1,
              .arg_name = "s" + std::to_string(i),
          }
      );
    }
    ps_consts_arg_idx = sig.DefineInput(
        air::ArgumentBindingBuffer{
            .buffer_size = {},
            .location_index = 0,
            .array_size = 1,
            .memory_access = air::MemoryAccess::read,
            .address_space = air::AddressSpace::constant,
            .type = air::msl_float4,
            .arg_name = "ffp_ps_consts",
            .raster_order_group = {},
        }
    );
  }
  int tex0_arg_idx = -1;
  int samp0_arg_idx = -1;
  if (key->tex0_mode == 1) {
    tex0_arg_idx = (int)sig.DefineInput(
        air::ArgumentBindingTexture{
            .location_index = 0,
            .array_size = 1,
            .memory_access = air::MemoryAccess::sample,
            .type =
                air::MSLTexture{
                    .component_type = air::msl_float,
                    .memory_access = air::MemoryAccess::sample,
                    .resource_kind = air::TextureKind::texture_2d,
                    .resource_kind_logical = air::TextureKind::texture_2d,
                },
            .arg_name = "t0",
            .raster_order_group = {},
        }
    );
    samp0_arg_idx = (int)sig.DefineInput(
        air::ArgumentBindingSampler{
            .location_index = 0,
            .array_size = 1,
            .arg_name = "s0",
        }
    );
  }
  uint32_t rt0_arg_idx = sig.DefineOutput(
      air::OutputRenderTarget{
          .dual_source_blending = false,
          .index = 0,
          .type = air::msl_float4,
      }
  );
  // D3DRS_MULTISAMPLEMASK coverage output: the [[sample_mask]] uint field the
  // epilogue ANDs the app mask into. The FFP PS declares no coverage of its
  // own, so the tail mask is always the final value (mirrors the bytecode PS).
  uint32_t cov_arg_idx = ~0u;
  if (emit_sample_mask)
    cov_arg_idx = sig.DefineOutput(air::OutputCoverageMask{});

  auto [fn, fn_md] = sig.CreateFunction(name, context, module, 0, false);
  IRBuilder<> builder(BasicBlock::Create(context, "entry", fn));
  raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(builder, nulldbg);

  Value *color = fn->getArg(color0_arg_idx);
  if (key->tex0_mode == 2) {
    // The texture-stage combiner chain (DXVK d3d9_fixed_function.cpp's
    // DoOp table). current starts as the interpolated diffuse; each
    // enabled stage combines into current (or the temp register when
    // D3DTSS_RESULTARG says so), with color and alpha evaluated
    // separately unless identical. Stage 0 ends the chain on
    // D3DTOP_DISABLE (color op 1).
    auto *float4Ty2 = FixedVectorType::get(Type::getFloatTy(context), 4);
    Value *diffuse_in = fn->getArg(color0_arg_idx);
    Value *specular_in = fn->getArg(color1_arg_idx);
    Value *current = diffuse_in;
    Value *temp_reg = Constant::getNullValue(float4Ty2);
    auto *cPtr = fn->getArg(ps_consts_arg_idx);
    Value *tfactor = builder.CreateLoad(float4Ty2, cPtr);
    auto splat_f = [&](float v) {
      return ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(Type::getFloatTy(context), v));
    };
    auto saturate4 = [&](Value *v) {
      return air.CreateFPBinOp(
          llvm::air::AIRBuilder::fmin, air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, v, splat_f(0.f)), splat_f(1.f)
      );
    };
    auto alpha_rep = [&](Value *v) {
      int wwww[4] = {3, 3, 3, 3};
      return builder.CreateShuffleVector(v, v, ArrayRef<int>(wwww, 4));
    };
    Value *prev_tex = nullptr;
    uint32_t prev_color_op = 0;
    for (int i = 0; i < 8; ++i) {
      uint32_t w0 = key->stages[i][0];
      uint32_t color_op = w0 & 0xFF;
      uint32_t alpha_op = (w0 >> 8) & 0xFF;
      if (color_op == 1 /* D3DTOP_DISABLE */)
        break;
      Value *texture_val = nullptr;
      if (stage_tex_arg_idx[i] >= 0) {
        uint32_t kind_i = (key->sampler_kind_key >> (i * 4)) & 0xFu;
        const bool coord3 = kind_i == 3 || kind_i == 4;
        llvm::air::Texture tex_desc{
            .kind = kind_i == 3   ? llvm::air::Texture::texture_cube
                    : kind_i == 4 ? llvm::air::Texture::texture_3d
                                  : llvm::air::Texture::texture_2d,
            .sample_type = llvm::air::Texture::sample_float,
            .memory_access = llvm::air::Texture::access_sample,
        };
        Value *coord;
        // A point sprite substitutes its generated [0,1]^2 coordinate for the
        // stage's texcoord. Cube and volume stages keep the varying here: a
        // sprite has no third coordinate, so this degenerate pairing (a
        // point-sprite draw sampling a cube/volume in the fixed function)
        // stays on the interpolated set. Both references substitute (s,t,0);
        // documented divergence for a corner no shipping content exercises.
        if (point_coord_arg_idx != ~0u && !coord3) {
          coord = fn->getArg(point_coord_arg_idx);
        } else {
          Value *coord4 = fn->getArg(texcoord_arg_idx[i]);
          if ((w0 & (1u << 18)) && !coord3) {
            // D3DTTFF_PROJECTED: the vertex stage copied the divisor
            // into w; projective sampling divides here. Cube and
            // volume stages never project (wined3d glsl_shader.c
            // drops proj for cube lookups).
            Value *w = builder.CreateExtractElement(coord4, builder.getInt32(3));
            coord4 = builder.CreateFDiv(coord4, builder.CreateVectorSplat(4, w));
          }
          if (coord3) {
            int xyz[3] = {0, 1, 2};
            coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xyz, 3));
          } else {
            int xy[2] = {0, 1};
            coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xy, 2));
          }
        }
        if (i > 0 && (prev_color_op == 22 || prev_color_op == 23) && prev_tex) {
          // D3DTOP_BUMPENVMAP on the stage below shifts this stage's
          // sample coordinate by the previous stage's signed sample
          // pushed through that stage's 2x2 bump matrix (wined3d
          // glsl_shader.c; the lane order matches the TexBem arm in
          // dxso_compile.cpp). With a projected coordinate the divide
          // above already happened, which is the same result as
          // wined3d scaling the displacement by w before its TXP.
          Value *mat = builder.CreateLoad(float4Ty2, builder.CreateGEP(float4Ty2, cPtr, builder.getInt32(9 + (i - 1))));
          Value *nx = builder.CreateExtractElement(prev_tex, builder.getInt32(0));
          Value *ny = builder.CreateExtractElement(prev_tex, builder.getInt32(1));
          Value *du = builder.CreateFAdd(
              builder.CreateFMul(builder.CreateExtractElement(mat, builder.getInt32(0)), nx),
              builder.CreateFMul(builder.CreateExtractElement(mat, builder.getInt32(2)), ny)
          );
          Value *dv = builder.CreateFAdd(
              builder.CreateFMul(builder.CreateExtractElement(mat, builder.getInt32(1)), nx),
              builder.CreateFMul(builder.CreateExtractElement(mat, builder.getInt32(3)), ny)
          );
          Value *cx = builder.CreateFAdd(builder.CreateExtractElement(coord, builder.getInt32(0)), du);
          Value *cy = builder.CreateFAdd(builder.CreateExtractElement(coord, builder.getInt32(1)), dv);
          coord = builder.CreateInsertElement(coord, cx, builder.getInt32(0));
          coord = builder.CreateInsertElement(coord, cy, builder.getInt32(1));
        }
        const int32_t no_off[3] = {0, 0, 0};
        if (kind_i == 12 /* FETCH4 armed on an incompatible format */) {
          texture_val = Constant::getNullValue(float4Ty2);
        } else if (kind_i == 7 /* FETCH4 */) {
          // AMD FETCH4: gather the red channel of the four neighbours
          // in the D3D9 order (B, R, G, A), nudged by half a texel so
          // the footprint matches the point-sample texel the stage
          // addressed; the same shape as the bytecode Tex arm.
          Value *tex_h = fn->getArg(stage_tex_arg_idx[i]);
          Value *fw = air.CreateTextureQuery(tex_desc, tex_h, llvm::air::Texture::Query::width, builder.getInt32(0));
          Value *fh = air.CreateTextureQuery(tex_desc, tex_h, llvm::air::Texture::Query::height, builder.getInt32(0));
          auto *sfTy = Type::getFloatTy(context);
          // Just under half a texel, the same corner-avoidance bias the
          // bytecode arm and DXVK apply.
          Value *inv_w = builder.CreateFDiv(ConstantFP::get(sfTy, 0.498046875f), builder.CreateUIToFP(fw, sfTy));
          Value *inv_h = builder.CreateFDiv(ConstantFP::get(sfTy, 0.498046875f), builder.CreateUIToFP(fh, sfTy));
          Value *nudge = UndefValue::get(coord->getType());
          nudge = builder.CreateInsertElement(nudge, inv_w, builder.getInt32(0));
          nudge = builder.CreateInsertElement(nudge, inv_h, builder.getInt32(1));
          Value *g_coord = builder.CreateFAdd(coord, nudge);
          auto [g, g_res] = air.CreateGather(
              tex_desc, tex_h, fn->getArg(stage_samp_arg_idx[i]), g_coord, /*ArrayIndex=*/nullptr, no_off,
              builder.getInt32(0)
          );
          (void)g_res;
          int swz[4] = {2, 0, 1, 3};
          texture_val = builder.CreateShuffleVector(g, g, ArrayRef<int>(swz, 4));
        } else {
          // D3DSAMP_MIPMAPLODBIAS applies at every sample site (Metal
          // samplers carry no bias); always-on like the bytecode path,
          // a numeric no-op at the zero default. The FETCH4 arm stays
          // unbiased: the gather is level-0 point sampling by
          // construction.
          Value *bias_ptr = builder.CreateGEP(builder.getInt32Ty(), fn->getArg(bc_arg_idx), builder.getInt32(8 + i));
          Value *bias =
              builder.CreateBitCast(builder.CreateLoad(builder.getInt32Ty(), bias_ptr), Type::getFloatTy(context));
          auto [texel, res] = air.CreateSample(
              tex_desc, fn->getArg(stage_tex_arg_idx[i]), fn->getArg(stage_samp_arg_idx[i]), coord,
              /*ArrayIndex=*/nullptr, no_off, llvm::air::sample_bias{bias}
          );
          (void)res;
          texture_val = texel;
        }
        if (kind_i == 13 && texture_val) {
          // FETCH4 on a block-compressed format replicates the sampled
          // red instead of gathering across the block.
          Value *r = builder.CreateExtractElement(texture_val, builder.getInt32(0));
          texture_val = builder.CreateVectorSplat(4, r);
        }
        if (kind_i == 8 || kind_i == 9) {
          // Two-channel signed formats: rescale to the D3D9 conversion
          // (divide by 2^(n-1)) and force the missing z and w to one,
          // the same fixup the bytecode path applies after its sample.
          double s = kind_i == 8 ? 127.0 / 128.0 : 32767.0 / 32768.0;
          auto *sfTy = Type::getFloatTy(context);
          Value *scaled = builder.CreateFMul(texture_val, builder.CreateVectorSplat(4, ConstantFP::get(sfTy, s)));
          Value *one_f = ConstantFP::get(sfTy, 1.0);
          scaled = builder.CreateInsertElement(scaled, one_f, builder.getInt32(2));
          texture_val = builder.CreateInsertElement(scaled, one_f, builder.getInt32(3));
        }
        if (i > 0 && prev_color_op == 23 && prev_tex) {
          // D3DTOP_BUMPENVMAPLUMINANCE also scales this stage's sample
          // by the previous sample's z through the stage's luminance
          // scale and offset, saturated.
          Value *lum =
              builder.CreateLoad(float4Ty2, builder.CreateGEP(float4Ty2, cPtr, builder.getInt32(17 + (i - 1))));
          Value *f = builder.CreateFAdd(
              builder.CreateFMul(
                  builder.CreateExtractElement(prev_tex, builder.getInt32(2)),
                  builder.CreateExtractElement(lum, builder.getInt32(0))
              ),
              builder.CreateExtractElement(lum, builder.getInt32(1))
          );
          auto *sfTy = Type::getFloatTy(context);
          f = air.CreateFPBinOp(
              llvm::air::AIRBuilder::fmin,
              air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, f, ConstantFP::get(sfTy, 0.0)), ConstantFP::get(sfTy, 1.0)
          );
          texture_val = builder.CreateFMul(texture_val, builder.CreateVectorSplat(4, f));
        }
      }
      auto get_arg = [&](uint32_t arg) -> Value * {
        Value *reg = splat_f(1.f);
        switch (arg & 0xF /* D3DTA_SELECTMASK */) {
        case 0 /* DIFFUSE */:
          reg = diffuse_in;
          break;
        case 1 /* CURRENT */:
          reg = current;
          break;
        case 2 /* TEXTURE */:
          // An unbound stage samples opaque black per the unbound-texture
          // contract the bytecode path keeps.
          reg =
              texture_val
                  ? texture_val
                  : ConstantVector::get(
                        {ConstantFP::get(Type::getFloatTy(context), 0.), ConstantFP::get(Type::getFloatTy(context), 0.),
                         ConstantFP::get(Type::getFloatTy(context), 0.), ConstantFP::get(Type::getFloatTy(context), 1.)}
                    );
          break;
        case 3 /* TFACTOR */:
          reg = tfactor;
          break;
        case 4 /* SPECULAR */:
          reg = specular_in;
          break;
        case 5 /* TEMP */:
          reg = temp_reg;
          break;
        case 6 /* CONSTANT */: {
          Value *p = builder.CreateGEP(float4Ty2, cPtr, builder.getInt32(1 + i));
          reg = builder.CreateLoad(float4Ty2, p);
          break;
        }
        default:
          break;
        }
        if (arg & 0x10 /* D3DTA_COMPLEMENT */)
          reg = builder.CreateFSub(splat_f(1.f), reg);
        if (arg & 0x20 /* D3DTA_ALPHAREPLICATE */)
          reg = alpha_rep(reg);
        return reg;
      };
      auto do_op = [&](uint32_t op, Value *dst, Value *a0, Value *a1, Value *a2) -> Value * {
        switch (op) {
        case 2 /* SELECTARG1 */:
          return a1;
        case 3 /* SELECTARG2 */:
          return a2;
        case 4 /* MODULATE */:
          return builder.CreateFMul(a1, a2);
        case 5 /* MODULATE2X */:
          return saturate4(builder.CreateFMul(builder.CreateFMul(a1, a2), splat_f(2.f)));
        case 6 /* MODULATE4X */:
          return saturate4(builder.CreateFMul(builder.CreateFMul(a1, a2), splat_f(4.f)));
        case 7 /* ADD */:
          return saturate4(builder.CreateFAdd(a1, a2));
        case 8 /* ADDSIGNED */:
          return saturate4(builder.CreateFAdd(a1, builder.CreateFSub(a2, splat_f(0.5f))));
        case 9 /* ADDSIGNED2X */:
          return saturate4(
              builder.CreateFMul(builder.CreateFAdd(a1, builder.CreateFSub(a2, splat_f(0.5f))), splat_f(2.f))
          );
        case 10 /* SUBTRACT */:
          return saturate4(builder.CreateFSub(a1, a2));
        case 11 /* ADDSMOOTH */:
          return saturate4(builder.CreateFAdd(a1, builder.CreateFMul(builder.CreateFSub(splat_f(1.f), a1), a2)));
        case 12 /* BLENDDIFFUSEALPHA */: {
          Value *f = alpha_rep(diffuse_in);
          return builder.CreateFAdd(builder.CreateFMul(a1, f),
                                    builder.CreateFMul(a2, builder.CreateFSub(splat_f(1.f), f)));
        }
        case 13 /* BLENDTEXTUREALPHA */: {
          Value *f = alpha_rep(get_arg(2));
          return builder.CreateFAdd(builder.CreateFMul(a1, f),
                                    builder.CreateFMul(a2, builder.CreateFSub(splat_f(1.f), f)));
        }
        case 14 /* BLENDFACTORALPHA */: {
          Value *f = alpha_rep(tfactor);
          return builder.CreateFAdd(builder.CreateFMul(a1, f),
                                    builder.CreateFMul(a2, builder.CreateFSub(splat_f(1.f), f)));
        }
        case 15 /* BLENDTEXTUREALPHAPM */: {
          Value *f = builder.CreateFSub(splat_f(1.f), alpha_rep(get_arg(2)));
          return saturate4(builder.CreateFAdd(a1, builder.CreateFMul(a2, f)));
        }
        case 16 /* BLENDCURRENTALPHA */: {
          Value *f = alpha_rep(current);
          return builder.CreateFAdd(builder.CreateFMul(a1, f),
                                    builder.CreateFMul(a2, builder.CreateFSub(splat_f(1.f), f)));
        }
        case 18 /* MODULATEALPHA_ADDCOLOR */:
          return saturate4(builder.CreateFAdd(a1, builder.CreateFMul(alpha_rep(a1), a2)));
        case 19 /* MODULATECOLOR_ADDALPHA */:
          return saturate4(builder.CreateFAdd(builder.CreateFMul(a1, a2), alpha_rep(a1)));
        case 20 /* MODULATEINVALPHA_ADDCOLOR */:
          return saturate4(
              builder.CreateFAdd(a1, builder.CreateFMul(builder.CreateFSub(splat_f(1.f), alpha_rep(a1)), a2))
          );
        case 21 /* MODULATEINVCOLOR_ADDALPHA */:
          return saturate4(
              builder.CreateFAdd(builder.CreateFMul(builder.CreateFSub(splat_f(1.f), a1), a2), alpha_rep(a1))
          );
        case 24 /* DOTPRODUCT3 */: {
          Value *b1 = builder.CreateFSub(a1, splat_f(0.5f));
          Value *b2 = builder.CreateFSub(a2, splat_f(0.5f));
          Value *prod = builder.CreateFMul(b1, b2);
          Value *x = builder.CreateExtractElement(prod, builder.getInt32(0));
          Value *y = builder.CreateExtractElement(prod, builder.getInt32(1));
          Value *z = builder.CreateExtractElement(prod, builder.getInt32(2));
          Value *dot = builder.CreateFMul(builder.CreateFAdd(builder.CreateFAdd(x, y), z),
                                          ConstantFP::get(Type::getFloatTy(context), 4.0));
          Value *v = builder.CreateVectorSplat(4, dot);
          return saturate4(v);
        }
        case 25 /* MULTIPLYADD */:
          return saturate4(builder.CreateFAdd(a0, builder.CreateFMul(a1, a2)));
        case 26 /* LERP */: {
          return builder.CreateFAdd(builder.CreateFMul(a1, a0),
                                    builder.CreateFMul(a2, builder.CreateFSub(splat_f(1.f), a0)));
        }
        case 22 /* BUMPENVMAP */:
        case 23 /* BUMPENVMAPLUMINANCE */:
          // The perturbation applies at the next stage's sample; the
          // combiner slot leaves the running register untouched
          // (wined3d writes nothing for these two ops).
          return dst;
        case 17 /* PREMODULATE */:
          // wined3d leaves the running register untouched for the
          // legacy pre-modulate op; keep the same no-op shape.
          return dst;
        default:
          // Anything unknown passes arg1 through.
          return a1;
        }
      };
      uint32_t c1 = key->stages[i][1], c2 = key->stages[i][2];
      Value *ca1 = get_arg(c1 & 0xFF), *ca2 = get_arg((c1 >> 8) & 0xFF), *ca0 = get_arg((c1 >> 16) & 0xFF);
      Value *dst_in = (w0 & (1u << 17)) ? temp_reg : current;
      Value *color_res = do_op(color_op, dst_in, ca0, ca1, ca2);
      Value *result;
      // D3DTOP_BUMPENVMAP(LUMINANCE) perturb only the next stage's sample
      // coordinate; the stage writes neither color nor alpha to the running
      // register, so both lanes stay dst_in even when D3DTSS_ALPHAOP is set.
      // wined3d guards both the color and the alpha fragment op out for these
      // ops (glsl_shader.c); DXVK misses the alpha guard, so follow wined3d.
      // A disabled alpha op must leave the running register alpha untouched
      // (wined3d writes the .xyz mask only; DXVK keeps dst.w), so it takes the
      // compose path below where the alpha lane comes from dst_in; only the
      // identical-op case may reuse the color result whole.
      if (color_op == 22 /* BUMPENVMAP */ || color_op == 23 /* BUMPENVMAPLUMINANCE */) {
        result = dst_in;
      } else if (alpha_op == color_op && c1 == c2) {
        result = color_res;
      } else {
        Value *aa1 = get_arg(c2 & 0xFF), *aa2 = get_arg((c2 >> 8) & 0xFF), *aa0 = get_arg((c2 >> 16) & 0xFF);
        Value *alpha_res = alpha_op == 1 ? dst_in : do_op(alpha_op, dst_in, aa0, aa1, aa2);
        int cccw[4] = {0, 1, 2, 4 + 3};
        result = builder.CreateShuffleVector(color_res, alpha_res, ArrayRef<int>(cccw, 4));
      }
      if (w0 & (1u << 17))
        temp_reg = result;
      else
        current = result;
      prev_tex = texture_val;
      prev_color_op = color_op;
    }
    color = current;
  } else if (key->tex0_mode == 1) {
    auto *float4Ty = FixedVectorType::get(Type::getFloatTy(context), 4);
    llvm::air::Texture tex_desc{
        .kind = llvm::air::Texture::texture_2d,
        .sample_type = llvm::air::Texture::sample_float,
        .memory_access = llvm::air::Texture::access_sample,
    };
    Value *coord;
    if (point_coord_arg_idx != ~0u) {
      coord = fn->getArg(point_coord_arg_idx);
    } else {
      Value *coord4 = fn->getArg(texcoord0_arg_idx);
      int xy[2] = {0, 1};
      coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xy, 2));
    }
    const int32_t no_offset[3] = {0, 0, 0};
    auto [texel, residency] = air.CreateSample(
        tex_desc, fn->getArg(tex0_arg_idx), fn->getArg(samp0_arg_idx), coord,
        /*ArrayIndex=*/nullptr, no_offset
    );
    (void)residency;
    (void)float4Ty;
    color = builder.CreateFMul(color, texel);
  }

  auto *fTy = Type::getFloatTy(context);
  auto *u32Ty = Type::getInt32Ty(context);
  if (key->lighting_key & 4u) {
    // D3DRS_SPECULARENABLE adds the interpolated specular after the
    // combiner chain and before fog (the fixed-function output stage).
    auto *f4 = FixedVectorType::get(fTy, 4);
    Value *spec = fn->getArg(color1_arg_idx);
    Constant *lanes[4] = {
        ConstantFP::get(fTy, 1.0), ConstantFP::get(fTy, 1.0), ConstantFP::get(fTy, 1.0), ConstantFP::get(fTy, 0.0)
    };
    Value *rgb_mask = ConstantVector::get(lanes);
    color = builder.CreateFAdd(color, builder.CreateFMul(spec, rgb_mask));
    (void)f4;
  }
  if (ps_fog_mode >= 0) {
    // Same blend the bytecode epilogue emits: factor from the FOG0
    // varying (vertex fog) or from clip-space depth and the blob's
    // table params, then mix toward the blob's fog color.
    auto *bcPtr = fn->getArg(bc_arg_idx);
    auto load_blob_f = [&](uint32_t idx) -> Value * {
      Value *p = builder.CreateGEP(u32Ty, bcPtr, builder.getInt32((int)idx));
      return builder.CreateBitCast(builder.CreateLoad(u32Ty, p), fTy);
    };
    Value *factor = nullptr;
    if (ps_fog_mode == DXSO_PS_FOG_MODE_SPECULAR_ALPHA) {
      factor = builder.CreateExtractElement(fn->getArg(color1_arg_idx), builder.getInt32(3));
    } else if (ps_fog_mode == 0) {
      factor = builder.CreateExtractElement(fn->getArg(fog0_arg_idx), builder.getInt32(0));
    } else {
      // Same coordinate contract as the bytecode epilogue: eye-space w
      // for a typical perspective projection, the vertex-output Z varying
      // otherwise.
      Value *pos = fn->getArg(position_arg_idx);
      Value *depth;
      if (ps_fog_coord_w) {
        Value *pw = builder.CreateExtractElement(pos, builder.getInt32(3));
        depth = builder.CreateFDiv(ConstantFP::get(fTy, 1.0), pw);
      } else {
        // Vertex-output Z: the interpolated FOG0.y varying the VS wrote
        // (clip_pos.z), not the fragment [[position]].z. The device Z is
        // post-perspective and carries the rasterizer depth bias on Apple
        // GPUs; wined3d fogs the ortho / pre-transformed path against
        // gl_Position.z / ec_pos.z (glsl_shader.c), which this varying is.
        depth = builder.CreateExtractElement(fn->getArg(fog0_arg_idx), builder.getInt32(1));
      }
      Value *fog_start = load_blob_f(24);
      Value *fog_end = load_blob_f(25);
      Value *fog_density = load_blob_f(26);
      if (ps_fog_mode == DXSO_PS_FOG_MODE_LINEAR) {
        Value *num = builder.CreateFSub(fog_end, depth);
        Value *den = builder.CreateFSub(fog_end, fog_start);
        factor = builder.CreateFDiv(num, den);
      } else {
        constexpr double kLog2E = 1.4426950408889634;
        Value *dd = builder.CreateFMul(depth, fog_density);
        Value *exponent = ps_fog_mode == DXSO_PS_FOG_MODE_EXP2 ? builder.CreateFMul(dd, dd) : dd;
        Value *neg = builder.CreateFNeg(builder.CreateFMul(exponent, ConstantFP::get(fTy, kLog2E)));
        factor = air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, neg);
      }
    }
    factor = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, factor);
    Value *fogged = color;
    for (uint32_t lane = 0; lane < 3; ++lane) {
      Value *fog_c = load_blob_f(4 + lane);
      Value *c = builder.CreateExtractElement(color, builder.getInt32(lane));
      Value *blended = builder.CreateFAdd(fog_c, builder.CreateFMul(builder.CreateFSub(c, fog_c), factor));
      fogged = builder.CreateInsertElement(fogged, blended, builder.getInt32(lane));
    }
    color = fogged;
  }

  if (emit_alpha_test) {
    // Same discard contract as the bytecode epilogue: compare the
    // output alpha against ALPHAREF / 255 with the D3DCMP_* predicate
    // and discard the fragment when the test fails.
    auto *cont_bb = BasicBlock::Create(context, "alpha_test.cont", fn);
    auto *kill_bb = BasicBlock::Create(context, "alpha_test.kill", fn);
    if (ps_args->alpha_test_func == 1 /* D3DCMP_NEVER */) {
      builder.CreateBr(kill_bb);
    } else {
      Value *alpha = builder.CreateExtractElement(color, builder.getInt32(3));
      // The ref rides the shared PS uniform tail (buffer(2) uint32 index 28),
      // host-written as D3DRS_ALPHAREF / 255 per draw, matching the bytecode
      // path; the compare is unchanged so a fixed ref discards identically.
      auto *u32Ty = Type::getInt32Ty(context);
      Value *ref_ptr = builder.CreateGEP(u32Ty, fn->getArg(bc_arg_idx), builder.getInt32(28));
      Value *ref = builder.CreateBitCast(builder.CreateLoad(u32Ty, ref_ptr), fTy);
      Value *pass = nullptr;
      switch (ps_args->alpha_test_func) {
      case 2 /* D3DCMP_LESS */:
        pass = builder.CreateFCmpOLT(alpha, ref);
        break;
      case 3 /* D3DCMP_EQUAL */:
        pass = builder.CreateFCmpOEQ(alpha, ref);
        break;
      case 4 /* D3DCMP_LESSEQUAL */:
        pass = builder.CreateFCmpOLE(alpha, ref);
        break;
      case 5 /* D3DCMP_GREATER */:
        pass = builder.CreateFCmpOGT(alpha, ref);
        break;
      case 6 /* D3DCMP_NOTEQUAL */:
        // Unordered: a NaN alpha must pass NOTEQUAL, matching the bytecode
        // alpha-test epilogue (dxso_compile.cpp) and wined3d.
        pass = builder.CreateFCmpUNE(alpha, ref);
        break;
      case 7 /* D3DCMP_GREATEREQUAL */:
        pass = builder.CreateFCmpOGE(alpha, ref);
        break;
      default:
        // An out-of-range compare func kills every fragment, matching both
        // refs' DecodeCompareOp default (NEVER). The d3d9 device already
        // normalizes garbage to D3DCMP_NEVER before keying, so this is the
        // safety net for the airconv-direct path, not the common case.
        pass = ConstantInt::getFalse(context);
        break;
      }
      builder.CreateCondBr(pass, cont_bb, kill_bb);
    }
    builder.SetInsertPoint(kill_bb);
    air.CreateDiscard();
    builder.CreateBr(cont_bb);
    builder.SetInsertPoint(cont_bb);
  }

  // The generated combiner only ever writes rt0, so bit 0 of the mask gates it:
  // an 8-bit unorm rt0 gets the round-to-even snap; a float/HDR/sRGB rt0 leaves
  // the mask clear and keeps full precision.
  if (ps_args != nullptr && (ps_args->unorm_output_reg_mask & 1u))
    color = emit_unorm8_snap(air, builder, color);
  Value *retval = UndefValue::get(fn->getReturnType());
  retval = builder.CreateInsertValue(retval, color, {rt0_arg_idx});
  if (cov_arg_idx != ~0u) {
    // D3DRS_MULTISAMPLEMASK: AND the app mask (blob tail uint32 index 29,
    // the coverage bitmask itself, so no bitcast) into hardware coverage.
    // The FFP PS has no coverage input to fold with, so the mask is the whole
    // [[sample_mask]] output (matches wined3d/DXVK's unconditional apply).
    Value *mask_ptr = builder.CreateGEP(u32Ty, fn->getArg(bc_arg_idx), builder.getInt32(29));
    Value *mask_u32 = builder.CreateLoad(u32Ty, mask_ptr);
    retval = builder.CreateInsertValue(retval, mask_u32, {cov_arg_idx});
  }
  builder.CreateRet(retval);

  module.getOrInsertNamedMetadata("air.fragment")->addOperand(fn_md);
}

} // namespace

void
compile_ffp(
    const ::DXSO_SHADER_FFP_KEY_DATA *key, const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout,
    const ::DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args, int ps_fog_mode, bool ps_fog_coord_w, const char *name,
    LLVMContext &context, Module &module
) {
  stamp_metal_version(context, module);
  if (key->kind == 0)
    compile_ffp_vs(key, ia_layout, name, context, module);
  else
    compile_ffp_ps(key, ps_args, ps_fog_mode, ps_fog_coord_w, name, context, module);
}

} // namespace dxmt
