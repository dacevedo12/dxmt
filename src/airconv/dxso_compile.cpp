#include "dxso_compile.hpp"
#include "ffp_compile.hpp"

#include "air_operations.hpp"
#include "air_signature.hpp"
#include "air_type.hpp"
#include "airconv_context.hpp"
#include "airconv_public.h"
#include "metallib_writer.hpp"
#include "nt/air_builder.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>

namespace dxmt {

// TODO: d3d9 should route CreateVertexShader/CreatePixelShader through
// DXSOInitialize for validation and walk instead of duplicating.
// Translate a complete SM1-3 shader into the externally-owned Module,
// mirroring dxbc::convertDXBC's role in the optimization pipeline.
void
compile_dxso(
    DxsoShader *shader, const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout,
    const ::DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args, const ::DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *ps_samp_layout,
    bool ps_point_sprite, bool vs_inject_point_size, int ps_fog_mode, bool ps_fog_coord_w, const char *name,
    llvm::LLVMContext &context, llvm::Module &module
) {
  using namespace llvm;
  const bool is_vertex = shader->header.kind == DxsoShaderKind::Vertex;
  // Fog blend is a pre-SM3 contract; ps_3_0 computes fog itself. mode -1
  // means the host passed no fog arg; 0 is vertex fog (oFog factor),
  // 1/2/3 are the LINEAR/EXP/EXP2 table modes computed from depth here.
  const bool emit_fog_blend = !is_vertex && ps_fog_mode >= 0 && shader->header.major < 3;
  const bool fog_is_table =
      emit_fog_blend && ps_fog_mode >= DXSO_PS_FOG_MODE_LINEAR && ps_fog_mode <= DXSO_PS_FOG_MODE_EXP2;
  // Manual fetch is VS-only; even if the host hands a layout for a PS
  // (it shouldn't), the lowering below would have nothing to do.
  const bool manual_fetch = is_vertex && ia_layout != nullptr;
  // Pre-transformed (POSITIONT / XYZRHW) draw: the host marks the layout so
  // the VS epilogue remaps the window-space position to clip space instead
  // of passing it through. wined3d/DXVK run a fixed-function pre-transform
  // path for these; dxmt injects the same remap into the VS position write.
  const bool position_transformed = is_vertex && ia_layout != nullptr && ia_layout->position_transformed;
  // Size of the vertex float constant file the host will bind: 256 on a
  // hardware-VP device, up to 8192 on a software / mixed-VP device. It sets
  // both the direct-read ceiling and the relative-index clamp so a reladdr
  // `c[a0.x]` can reach the extended file the host uploads. Zero (an older
  // host, or a pixel shader) falls back to the hardware ceiling.
  const uint32_t vs_float_const_count =
      (is_vertex && ia_layout != nullptr && ia_layout->vs_float_const_count) ? ia_layout->vs_float_const_count : 256u;
  // Alpha test is PS-only and only relevant when the host explicitly
  // opts in; passing nullptr (or D3DCMP_ALWAYS) means no test snippet
  // is emitted, so the unspecialised PS keeps its current shape.
  const bool emit_alpha_test = !is_vertex && ps_args != nullptr && ps_args->alpha_test_func != 8 /* D3DCMP_ALWAYS */;
  // Dual-source blending: only the host knows the active blend factors,
  // so it flags this when SRC1 factors are bound. oC0/oC1 then become
  // the two color indices of attachment 0 instead of two attachments.
  const bool dual_source = !is_vertex && ps_args != nullptr && ps_args->dual_source_blending != 0;
  // D3DRS_MULTISAMPLEMASK: PS-only, host-gated on a maskable multisample
  // target with a non-all-ones mask. Emits a [[sample_mask]] coverage output
  // that ANDs the app mask (blob tail, uint32 index 29) into hardware
  // coverage. SM1-3 pixel shaders never emit their own coverage, so the tail
  // value is always the final mask; passing 0 keeps the current PS shape.
  const bool emit_sample_mask = !is_vertex && ps_args != nullptr && ps_args->emit_sample_mask != 0;
  // D3DSHADE_FLAT holds color-usage inputs at the provoking vertex for every
  // shader model, SM3 included. DXVK flat-decorates every PS colour input with
  // no version gate and keys it purely off D3DRS_SHADEMODE; the wine shademode
  // SM3 row is todo_wine (wine's own GL-backend miss, not native) and its
  // expected values are the flat colours, so native flat-shades SM3 too.
  const bool ps_flat_colors = !is_vertex && ps_args != nullptr && ps_args->flat_shading != 0;

  // air.version / air.language_version: AGX rejects PS metallibs
  // missing these. Mirrors dxbc::setup_metal_version's
  // SM50_SHADER_METAL_320 arm.
  auto u32_md = [&](uint32_t v) { return ConstantAsMetadata::get(ConstantInt::get(context, APInt{32, v})); };
  module.setTargetTriple("air64-apple-macosx15.0.0");
  module.getOrInsertNamedMetadata("air.version")->addOperand(MDTuple::get(context, {u32_md(2), u32_md(7), u32_md(0)}));
  module.getOrInsertNamedMetadata("air.language_version")
      ->addOperand(MDTuple::get(context, {MDString::get(context, "Metal"), u32_md(3), u32_md(2), u32_md(0)}));

  air::FunctionSignatureBuilder sig;

  // VS inputs: one [[stage_in]] attribute per dcl_<usage> v#: the
  // attribute index is the v# register number, host vertex descriptor
  // binds against it. PS inputs: InputFragmentStageIn keyed on a
  // user-name, and the matching VS output uses the same string. v#
  // (Input register file) and t# (Texture register file, SM2 / SM 1.4
  // PS) live in separate input arrays so the body's load_src can
  // route by register file directly.
  std::array<int, 16> input_arg_idx;
  input_arg_idx.fill(-1);
  // Per-v# dcl write mask. A partial mask (dcl_texcoord0 v0.xyz) makes
  // the undeclared components read the input-register defaults, 0 for
  // x/y/z and 1 for w, rather than whatever the interpolant carries.
  std::array<uint8_t, 16> ps_input_mask;
  ps_input_mask.fill(0xF);
  // Parallel to input_arg_idx: true when v<N> was dcl'd as TEXCOORD.
  // Drives the POINTSPRITEENABLE per-input substitution at tex_inputs
  // load time; SM3 PS reads texcoords via v# (input_arg_idx) rather
  // than t# (ps_tex_arg_idx), so the substitution has to look at both
  // register files. Stays all-false for SM1.x (legacy-PS reroutes
  // every v# to COLOR via the legacy_ps branch above) and for SM3
  // shaders that don't bind any texcoords.
  std::array<bool, 16> ps_v_is_texcoord{};
  // PS Texture-register-file inputs (t0..t7). Used by SM2 PS and SM 1.4
  // PS as the texcoord-input register file; SM3 PS reads texcoords
  // through v# instead and leaves this array fully -1.
  std::array<int, 8> ps_tex_arg_idx;
  ps_tex_arg_idx.fill(-1);
  // Per-v# index into ia_layout->elements when manual_fetch is on; -1
  // means the v# slot zero-fills like an undeclared input does today.
  std::array<int, 16> ia_element_idx;
  ia_element_idx.fill(-1);

  // PS signature head: InputPosition lands first so it occupies struct
  // field 0, even when no dcl_position v# is present. AGX rejects PS
  // metallibs that interleave [[position]] with user varyings; the
  // mismatched field order surfaces as
  // XPC_ERROR_CONNECTION_INTERRUPTED at PSO link time with no metallib
  // diagnostic. The arg index is captured so a SM3 `dcl_position v#`
  // can reuse it for v# loading instead of double-defining.
  int ps_position_arg_idx = -1;
  // COLOR1 stage-in arg (dcl'd or stub): the specular-alpha fog source
  // reads its alpha in the fog epilogue.
  int ps_color1_arg_idx = -1;
  // Set of PS user-input names already claimed by an explicit dcl;
  // used below to avoid emitting a duplicate stub for the same name.
  std::array<bool, 8> ps_texcoord_used{};
  std::array<bool, 2> ps_color_used{};
  bool ps_fog_used = false;
  // Pre-transformed (POSITIONT) passthrough varyings. A SM3 PS reading a
  // pre-transformed draw's extra decl semantics (blendweight / normal /
  // tangent / binormal / depth, plus the harmless blendindices) links them by
  // name to the fixed-function VS outputs (ffp_compile.cpp). Track which the
  // shader dcl'd at index 0 so the stub tail defines exactly the names the
  // shader itself did not, keeping every FFP-VS user output claimed by a PS
  // input (the AGX pipeline-link rule).
  bool ps_blendweight_used = false;
  bool ps_blendindices_used = false;
  bool ps_normal_used = false;
  bool ps_tangent_used = false;
  bool ps_binormal_used = false;
  bool ps_depth_used = false;
  // FOG0 stage-in arg, dcl'd or stub: the fixed fog blend epilogue
  // reads it whichever way it was defined.
  int ps_fog_arg_idx = -1;
  if (!is_vertex) {
    ps_position_arg_idx = (int)sig.DefineInput(
        air::InputPosition{
            .interpolation = air::Interpolation::center_no_perspective,
        }
    );
  }

  // vFace (DXSO MiscType register 1) -> [[front_facing]]. Defined right
  // after [[position]]: both are built-in inputs that must precede the
  // user varyings and the texture/sampler args (built-in inputs must
  // precede user varyings on AGX, or PSO link fails): and only when the
  // shader actually reads vFace, so a
  // PS that doesn't use it keeps a byte-identical signature. vPos (MiscType
  // register 0) needs no new input; it reuses ps_position_arg_idx.
  int ps_vface_arg_idx = -1;
  if (!is_vertex) {
    DxsoBytecodeIter vfscan(shader->bytecode.data(), static_cast<uint32_t>(shader->bytecode.size()), shader->header);
    DxsoInstruction vfins{};
    bool uses_vface = false;
    while (!uses_vface && vfscan.next(vfins)) {
      if (vfins.opcode == DxsoOpcode::End)
        break;
      for (uint32_t s = 0; s < vfins.src_count; ++s)
        if (vfins.src[s].base.type == DxsoRegisterType::MiscType && vfins.src[s].base.num == 1) {
          uses_vface = true;
          break;
        }
    }
    if (uses_vface)
      ps_vface_arg_idx = (int)sig.DefineInput(air::InputFrontFacing{});
  }

  // SM 1.0..1.3 PS, SM 2.0 PS and SM 1.4 PS: input semantics from
  // register file, not dcl token. SM 3.0 PS: semantic on dcl token.
  // (Mirrors DXVK dxso_compiler.cpp)
  bool legacy_ps = !is_vertex && shader->header.major < 3;
  bool ps_t_is_stage_in =
      !is_vertex && (shader->header.major >= 2 || (shader->header.major == 1 && shader->header.minor == 4));
  for (const auto &d : shader->metadata.dcls) {
    if (is_vertex) {
      if (d.bound_to.type != DxsoRegisterType::Input || d.bound_to.num >= input_arg_idx.size())
        continue;
      if (manual_fetch) {
        // Look up the IA element whose `reg` matches this v#. The d3d9
        // caller resolves (decl semantic, VS dcl) → reg before
        // populating the layout, so a matched element is the
        // expected case; an unmatched dcl falls through to zero-fill,
        // which mirrors the "undcl'd input is zero" DXSO contract.
        for (uint32_t k = 0; k < ia_layout->num_elements; ++k) {
          if (ia_layout->elements[k].reg == d.bound_to.num) {
            ia_element_idx[d.bound_to.num] = (int)k;
            break;
          }
        }
        continue;
      }
      input_arg_idx[d.bound_to.num] = (int)sig.DefineInput(
          air::InputVertexStageIn{
              .attribute = d.bound_to.num,
              .type = air::InputAttributeComponentType::Float,
              .name = "in" + std::to_string(d.bound_to.num),
          }
      );
      continue;
    }

    // PS path. Two register files reach the fragment shader as
    // interpolated inputs: Input (v#) and Texture (t#). SM3 PS only
    // declares v# and routes everything (Color, Texcoord, Fog,
    // Position) by dcl.usage. SM2 PS and SM 1.4 PS declare both v#
    // and t# but leave dcl.usage unreliable; register-file is the
    // truth. SM 1.0..1.3 PS doesn't dcl t# (and we don't bind it as
    // a stage-in below); the t# slots come from the `tex t#` opcode.
    bool is_v = d.bound_to.type == DxsoRegisterType::Input;
    bool is_t = d.bound_to.type == DxsoRegisterType::Texture;
    if (!is_v && !is_t)
      continue;
    if (is_v && d.bound_to.num >= input_arg_idx.size())
      continue;
    if (is_t && (!ps_t_is_stage_in || d.bound_to.num >= ps_tex_arg_idx.size()))
      continue;

    DxsoUsage effective_usage = d.dcl.usage;
    uint32_t effective_index = d.dcl.usage_index;
    if (legacy_ps) {
      effective_usage = is_t ? DxsoUsage::Texcoord : DxsoUsage::Color;
      effective_index = d.bound_to.num;
    }

    const char *base = nullptr;
    switch (effective_usage) {
    case DxsoUsage::Position:
      // SM3 PS dcl_position v# carries SV_Position. Reuse the
      // InputPosition emitted at the signature head: a second
      // DefineInput would put two [[position]] fields in the function.
      // legacy_ps never reaches this (effective_usage was rewritten
      // to Color/Texcoord based on register file).
      if (is_v)
        input_arg_idx[d.bound_to.num] = ps_position_arg_idx;
      continue;
    case DxsoUsage::Color:
      base = "COLOR";
      break;
    case DxsoUsage::Texcoord:
      base = "TEXCOORD";
      break;
    case DxsoUsage::Fog:
      base = "FOG";
      break;
    // Pre-transformed (POSITIONT) passthrough semantics. Only SM3 PS reaches
    // here with these usages (legacy_ps rewrites effective_usage to Color /
    // Texcoord by register file), and only a pre-transformed FFP VS actually
    // produces them; the names match the FFP VS OutputVertex user() names.
    case DxsoUsage::BlendWeight:
      base = "BLENDWEIGHT";
      break;
    case DxsoUsage::BlendIndices:
      base = "BLENDINDICES";
      break;
    case DxsoUsage::Normal:
      base = "NORMAL";
      break;
    case DxsoUsage::Tangent:
      base = "TANGENT";
      break;
    case DxsoUsage::Binormal:
      base = "BINORMAL";
      break;
    case DxsoUsage::Depth:
      base = "DEPTH";
      break;
    default:
      break; // TessFactor/Sample/etc; skip
    }
    if (!base)
      continue;
    int arg_idx = (int)sig.DefineInput(
        air::InputFragmentStageIn{
            .user = std::string(base) + std::to_string(effective_index),
            .type = air::msl_float4,
            .interpolation = ps_flat_colors && effective_usage == DxsoUsage::Color
                                 ? air::Interpolation::flat
                                 : air::Interpolation::center_perspective,
            .pull_mode = false,
        }
    );
    if (is_v) {
      input_arg_idx[d.bound_to.num] = arg_idx;
      ps_input_mask[d.bound_to.num] = d.mask.raw();
      if (effective_usage == DxsoUsage::Texcoord)
        ps_v_is_texcoord[d.bound_to.num] = true;
    } else
      ps_tex_arg_idx[d.bound_to.num] = arg_idx;
    if (effective_usage == DxsoUsage::Color && effective_index < ps_color_used.size()) {
      ps_color_used[effective_index] = true;
      if (effective_index == 1)
        ps_color1_arg_idx = arg_idx;
    } else if (effective_usage == DxsoUsage::Texcoord && effective_index < ps_texcoord_used.size())
      ps_texcoord_used[effective_index] = true;
    else if (effective_usage == DxsoUsage::Fog && effective_index == 0) {
      ps_fog_used = true;
      ps_fog_arg_idx = arg_idx;
    } else if (effective_index == 0) {
      // Pre-transformed passthrough semantics claimed at index 0; the FFP VS
      // only produces index 0, so a higher index links against nothing and is
      // left to the stub tail. The default arm covers the usages routed above.
      switch (effective_usage) {
      case DxsoUsage::BlendWeight:
        ps_blendweight_used = true;
        break;
      case DxsoUsage::BlendIndices:
        ps_blendindices_used = true;
        break;
      case DxsoUsage::Normal:
        ps_normal_used = true;
        break;
      case DxsoUsage::Tangent:
        ps_tangent_used = true;
        break;
      case DxsoUsage::Binormal:
        ps_binormal_used = true;
        break;
      case DxsoUsage::Depth:
        ps_depth_used = true;
        break;
      default:
        break;
      }
    }
  }

  // PS signature tail: define stubs for COLOR0..1, TEXCOORD0..7, FOG0
  // so AGX's PSO link finds matching PS inputs for all possible VS outputs,
  // preventing XPC_ERROR_CONNECTION_INTERRUPTED at link time.
  if (!is_vertex) {
    auto define_stub = [&](const std::string &user) {
      // Color stubs share the flat rule: the SM1 register-file binding
      // and the specular-alpha fog source read them, so they must
      // interpolate the way a declared color would.
      bool flat = ps_flat_colors && user.rfind("COLOR", 0) == 0;
      return (int)sig.DefineInput(
          air::InputFragmentStageIn{
              .user = user,
              .type = air::msl_float4,
              .interpolation = flat ? air::Interpolation::flat : air::Interpolation::center_perspective,
              .pull_mode = false,
          }
      );
    };
    // SM 1.x PS (1.4 included) has no dcl tokens; the dcls walk above
    // never populates input_arg_idx[] or ps_tex_arg_idx[]. The body
    // still reads v0/v1 (= COLOR0/1) and t0..t7 (= TEXCOORD0..7) by
    // register-file convention; without binding those slots to the
    // stage-in args defined here, every v#/t# read returns
    // ConstantAggregateZero and any "modulate by vertex color" PS
    // produces solid black. A ps_1_1 shader that does "mul r0, tex t0,
    // v0" produces 0 when v0 reads as zero; this binding prevents that
    // silent collapse.
    bool sm1_legacy_ps = shader->header.major == 1;
    for (int i = 0; i < 2; ++i) {
      if (!ps_color_used[i]) {
        int arg_idx = define_stub("COLOR" + std::to_string(i));
        if (sm1_legacy_ps && i < (int)input_arg_idx.size())
          input_arg_idx[i] = arg_idx;
        if (i == 1)
          ps_color1_arg_idx = arg_idx;
      }
    }
    for (int i = 0; i < 8; ++i) {
      if (!ps_texcoord_used[i]) {
        int arg_idx = define_stub("TEXCOORD" + std::to_string(i));
        if (sm1_legacy_ps && i < (int)ps_tex_arg_idx.size())
          ps_tex_arg_idx[i] = arg_idx;
      }
    }
    if (!ps_fog_used)
      ps_fog_arg_idx = define_stub("FOG0");
    // Pre-transformed passthrough stubs: a pre-transformed FFP VS emits every
    // one of these user outputs, so any PS it pairs with must claim all of
    // them by name or the PSO link fails (XPC_ERROR). Stub the names the
    // shader did not declare itself; an ordinary draw leaves these reading NaN,
    // which the body never touches. A PS that does declare one of these but
    // pairs with a non-pre-transformed VS (which never produces them) reads
    // NaN in place of the zero it read before; that pairing had no correct
    // source of the attribute either way, so no valid case regresses. Names
    // match the FFP VS OutputVertex user() names (ffp_compile.cpp).
    if (!ps_blendweight_used)
      define_stub("BLENDWEIGHT0");
    if (!ps_blendindices_used)
      define_stub("BLENDINDICES0");
    if (!ps_normal_used)
      define_stub("NORMAL0");
    if (!ps_tangent_used)
      define_stub("TANGENT0");
    if (!ps_binormal_used)
      define_stub("BINORMAL0");
    if (!ps_depth_used)
      define_stub("DEPTH0");
  }

  // PS point-sprite [[point_coord]] input. When ps_point_sprite is on
  // (host has bound a point-list primitive with D3DRS_POINTSPRITEENABLE),
  // every TEXCOORD<N> stage_in read at tex_inputs init time gets
  // substituted with float4(point_coord.xy, 0, 1). Replacing all 8 texcoord
  // slots unconditionally (no per-stage D3DTSS_TEXCOORDINDEX gating) is the
  // D3D9 POINTSPRITEENABLE contract. The z/w = (0, 1) is deliberate: DXVK and
  // wined3d fill (0, 0), but w = 1 keeps a projected texcoord read on a
  // point-sprite coord a harmless divide by one, not a divide by zero.
  int ps_point_coord_arg_idx = -1;
  if (!is_vertex && ps_point_sprite) {
    ps_point_coord_arg_idx = (int)sig.DefineInput(air::InputPointCoord{});
  }

  // Manual-fetch VS extras: vertex_buffers table at [[buffer(16)]]
  // (struct-array of {device char *base, i32 stride, i32 length}, one
  // entry per active stream slot, indexed by popcount of the slot mask
  // below the element's slot bit), plus the [[vertex_id]] /
  // [[base_vertex]] system inputs the prologue uses to compute the
  // per-element byte offset. Mirrors pull_vertex_input in
  // dxbc_converter_basicblock.cpp (which sources the same struct
  // layout via AirType::_dxmt_vertex_buffer_entry).
  uint32_t vbuf_table_arg_idx = 0;
  uint32_t vid_arg_idx = 0;
  uint32_t base_vertex_arg_idx = 0;
  uint32_t instance_id_arg_idx = 0;
  uint32_t base_instance_arg_idx = 0;
  if (manual_fetch) {
    // array_size = 1 (not 0): xcrun metal emits `air.location_index,
    // i32 N, i32 1` for non-array bindings; AGX reads the second integer
    // as a binding count and rejects 0 at PSO link with
    // XPC_ERROR_CONNECTION_INTERRUPTED, even though the metallib writer
    // accepts it without complaint. Same fix applies to every
    // ArgumentBindingBuffer site below.
    vbuf_table_arg_idx = sig.DefineInput(
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
    vid_arg_idx = sig.DefineInput(air::InputVertexID{});
    base_vertex_arg_idx = sig.DefineInput(air::InputBaseVertex{});
    instance_id_arg_idx = sig.DefineInput(air::InputInstanceID{});
    base_instance_arg_idx = sig.DefineInput(air::InputBaseInstance{});
  }

  // Constant register files: c#, i#, b#. Split per-file (not DXVK's
  // single struct) to match existing dxmt convention. Host binds:
  // buffer(0)=float4 c[], buffer(1)=int4 i[], buffer(2)=uint b[].
  // Covers the HWVP profiles (SM1 through SM3); SWVP's enlarged constant
  // register files are not covered.
  uint32_t cb_arg_idx = sig.DefineInput(
      air::ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = 0,
          .array_size = 1,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .type = air::msl_float4,
          .arg_name = "c",
          .raster_order_group = {},
      }
  );
  // Integer constant buffer: i0..i15. Read by Rep / Loop for the
  // count / init / stride payload and by load_src for general
  // ConstInt operands (rare outside loops). Sized for 16 entries
  // unconditionally; D3D9 caps i# at 16 across all SM2/SM3
  // profiles.
  uint32_t ic_arg_idx = sig.DefineInput(
      air::ArgumentBindingBuffer{
          .buffer_size = {},
          .location_index = 1,
          .array_size = 1,
          .memory_access = air::MemoryAccess::read,
          .address_space = air::AddressSpace::constant,
          .type = air::msl_int4,
          .arg_name = "i",
          .raster_order_group = {},
      }
  );
  // Bool bitmask: single uint32 packing b0..b15 with bit i = b#i.
  // Matches DXVK's bConsts[1] layout (src/d3d9/d3d9_constant_set.h)
  // so that the SetXShaderConstantB host-side packing path is a
  // bit-OR / bit-AND, no per-bool dispatch.
  uint32_t bc_arg_idx = sig.DefineInput(
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

  // VS clip planes: host packs enabled planes consecutively.
  // Disabled planes write 0.0 (GPU clips if < 0, so 0.0 passes).
  // PS does not use these bindings; slots 3/4 free for fragment stage.
  uint32_t clip_planes_arg_idx = ~0u;
  uint32_t clip_count_arg_idx = ~0u;
  if (is_vertex) {
    clip_planes_arg_idx = sig.DefineInput(
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
    clip_count_arg_idx = sig.DefineInput(
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
  }

  // Pre-transform viewport remap: two float4 (invExtent, invOffset) packed by
  // the host from the live D3D9 viewport. Only present for POSITIONT draws so
  // ordinary VSes keep their current binding layout.
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

  // Point-size uniform (float4 = size, min, max) at VS buffer 6. Only the
  // injecting point-size variant declares it, so ordinary VSes keep their
  // binding layout; the host binds it for exactly those draws. Feeding the
  // size through the uniform is what lets one variant serve every size
  // (DXVK d3d9_fixed_function.cpp GetPointSizeInfoVS reads the same block).
  uint32_t point_size_arg_idx = ~0u;
  if (is_vertex && vs_inject_point_size) {
    point_size_arg_idx = sig.DefineInput(
        air::ArgumentBindingBuffer{
            .buffer_size = {},
            .location_index = 6,
            .array_size = 1,
            .memory_access = air::MemoryAccess::read,
            .address_space = air::AddressSpace::constant,
            .type = air::msl_float4,
            .arg_name = "point_size",
            .raster_order_group = {},
        }
    );
  }

  // PS texture+sampler bindings: one pair per sampled s# from pre-scan.
  // Binding ABI only; Tex lowering in follow-up. Keep unknown samplers
  // out of both signature and opcode lowering to avoid unused args.
  std::array<int, 16> tex_arg_idx{};
  tex_arg_idx.fill(-1);
  std::array<int, 16> samp_arg_idx{};
  samp_arg_idx.fill(-1);
  std::array<DxsoTextureType, 16> samp_kind{};
  samp_kind.fill(DxsoTextureType::Unknown);
  // Per-slot hardware-PCF flag: flips SM2+ sample to sample_compare.
  // SM1.x texm3x*/texbem sample depth2d raw (bound as env/bump maps).
  std::array<bool, 16> samp_compare{};
  std::array<bool, 16> samp_fetch4{};
  std::array<bool, 16> samp_fetch4_broken{};
  std::array<bool, 16> samp_fetch4_rep{};
  std::array<bool, 16> samp_depth_r001{};
  std::array<uint8_t, 16> samp_snorm2{};
  // Texture/sampler bindings run for BOTH stages: vertex texture fetch (VTF,
  // SM3.0) lets a VS sample a texture (e.g. a displacement map driving vertex
  // deformation). The dcl carries the sampler kind on
  // the VS side (no host ps_samp_layout); the texldl/texldd handler below
  // emits the sample once these args exist.
  {
    for (const auto &d : shader->metadata.dcls) {
      if (d.bound_to.type == DxsoRegisterType::Sampler && d.bound_to.num < samp_kind.size())
        samp_kind[d.bound_to.num] = d.dcl.texture_type;
    }
    bool used[16] = {};
    DxsoBytecodeIter scan(shader->bytecode.data(), static_cast<uint32_t>(shader->bytecode.size()), shader->header);
    DxsoInstruction tins{};
    while (scan.next(tins)) {
      if (tins.opcode == DxsoOpcode::End)
        break;
      // Missing an opcode here starves its arm of the texture argument
      // and the instruction is silently dropped at the tex_arg_idx
      // guard, so the opcode set and the slot derivation are shared with
      // the metadata walk (dxso_decoder.hpp) rather than duplicated.
      if (!dxso_opcode_samples(tins.opcode))
        continue;
      const uint32_t slot = dxso_sampling_slot(tins, shader->header);
      if (slot < 16)
        used[slot] = true;
    }
    // Host layout is authoritative: engine's actual bound texture type
    // may differ from shader dcl (e.g., declare 2D but bind Cube for env-map).
    // Fallback to shader's dcl when no host layout; preserves airconv_cli.
    if (ps_samp_layout) {
      for (uint32_t i = 0; i < 16; ++i) {
        if (!used[i])
          continue;
        switch (ps_samp_layout->kinds[i]) {
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D:
          samp_kind[i] = DxsoTextureType::Texture2D;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_CUBE:
          samp_kind[i] = DxsoTextureType::TextureCube;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_3D:
          samp_kind[i] = DxsoTextureType::Texture3D;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH:
          samp_kind[i] = DxsoTextureType::Texture2DDepth;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_COMPARE:
          // Same Metal kind (depth2d); the Tex arm emits sample_compare
          // instead of sample, and the host pairs this with a LessEqual
          // compareFunction sampler.
          samp_kind[i] = DxsoTextureType::Texture2DDepth;
          samp_compare[i] = true;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4:
          // FETCH4: plain 2D bind, but the Tex arm gathers the red
          // channel of the four neighbours instead of sampling.
          samp_kind[i] = DxsoTextureType::Texture2D;
          samp_fetch4[i] = true;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_SNORM2_8:
          samp_kind[i] = DxsoTextureType::Texture2D;
          samp_snorm2[i] = 8;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_SNORM2_16:
          samp_kind[i] = DxsoTextureType::Texture2D;
          samp_snorm2[i] = 16;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_FETCH4:
          // Raw-depth FETCH4: depth2d bind, the Tex arm gathers the
          // four neighbouring depth values instead of sampling.
          samp_kind[i] = DxsoTextureType::Texture2DDepth;
          samp_fetch4[i] = true;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_DF:
          // DF raw depth: red channel only, (d, 0, 0, 1).
          samp_kind[i] = DxsoTextureType::Texture2DDepth;
          samp_depth_r001[i] = true;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4_BROKEN:
          samp_kind[i] = DxsoTextureType::Texture2D;
          samp_fetch4_broken[i] = true;
          break;
        case DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4_REPLICATE:
          samp_kind[i] = DxsoTextureType::Texture2D;
          samp_fetch4_rep[i] = true;
          break;
        default:
          // UNKNOWN; host didn't pin (e.g. stage with no bound
          // texture at PSO build time, or a partial-binding shape).
          // Leave samp_kind[i] alone: the shader's own dcl is the
          // next-best signal, and the SM 1.x default-to-2D arm below
          // catches the remaining Unknown case.
          break;
        }
      }
    }
    // Fallback for SM 1.x sampler slots the host didn't pin: keep the
    // historical Texture2D default. SM1 (1.4 included) has no dcl
    // sampler tokens; wined3d glsl_shader.c
    // shader_glsl_get_sample_function and DXVK src/dxso/dxso_compiler.cpp
    // both assume 2D when the bytecode itself can't disambiguate. Without
    // either source, samp_kind[i] stays Unknown → the binding loop below
    // `continue`s → tex_arg_idx[i] stays -1 → the Tex opcode short-
    // circuits and r0 sees the raw TEXCOORD<n> stage_in instead of the
    // sampled texel (UV-as-color gradient).
    if (shader->header.major == 1) {
      for (uint32_t i = 0; i < 16; ++i) {
        if (used[i] && samp_kind[i] == DxsoTextureType::Unknown)
          samp_kind[i] = DxsoTextureType::Texture2D;
      }
    }
    for (uint32_t i = 0; i < 16; ++i) {
      if (!used[i])
        continue;
      air::TextureKind kind;
      switch (samp_kind[i]) {
      case DxsoTextureType::Texture2D:
        kind = air::TextureKind::texture_2d;
        break;
      case DxsoTextureType::TextureCube:
        kind = air::TextureKind::texture_cube;
        break;
      case DxsoTextureType::Texture3D:
        kind = air::TextureKind::texture_3d;
        break;
      case DxsoTextureType::Texture2DDepth:
        kind = air::TextureKind::depth_2d;
        break;
      default:
        // Unknown sampler (no dcl); leave unbound; the case arm will
        // short-circuit on tex_arg_idx == -1.
        continue;
      }
      // array_size = 1 (not 0): AGX rejects 0 with
      // XPC_ERROR_CONNECTION_INTERRUPTED at pipeline link, even though
      // the metallib writer accepts it without complaint. xcrun metal
      // emits `air.location_index, i32 N, i32 1` for non-array bindings.
      tex_arg_idx[i] = (int)sig.DefineInput(
          air::ArgumentBindingTexture{
              .location_index = i,
              .array_size = 1,
              .memory_access = air::MemoryAccess::sample,
              .type =
                  air::MSLTexture{
                      .component_type = air::msl_float,
                      .memory_access = air::MemoryAccess::sample,
                      .resource_kind = kind,
                      .resource_kind_logical = kind,
                  },
              .arg_name = "t" + std::to_string(i),
              .raster_order_group = {},
          }
      );
      samp_arg_idx[i] = (int)sig.DefineInput(
          air::ArgumentBindingSampler{
              .location_index = i,
              .array_size = 1,
              .arg_name = "s" + std::to_string(i),
          }
      );
    }
  }

  // VS varying outputs: pre-scan SM≤2 for oD#/oT# writes, SM3 reads dcl.
  // SM1/SM2 semantics positional; SM3 uses dcl. OutputPosition first.
  std::array<int, 2> oD_arg_idx{};
  oD_arg_idx.fill(-1);
  std::array<int, 8> oT_arg_idx{};
  oT_arg_idx.fill(-1);
  int oFog_arg_idx = -1;
  // VS oPts (point size): SM1.x via mov oPts, SM3 via dcl_psize o#.
  // Without plumb-through, D3DPT_POINTLIST defaults to 1.0.
  // vs_inject_point_size forces the output for a point draw whose
  // bytecode never writes oPts; the size then rides the uniform.
  bool oPts_used = false;
  if (is_vertex && vs_inject_point_size)
    oPts_used = true;
  int oPts_arg_idx = -1;
  // SM3 dcl_psize routes here via oN_is_pointsize[N] aliasing, mirroring
  // the oN_is_position pattern below. Tracked separately from
  // oN_arg_idx so the SM3 varying-output emit loop skips PointSize
  // (it's a non-varying output that needs scalar emission, not float4).
  std::array<bool, 16> oN_is_pointsize{};
  // PS [[color(N)]] outputs: SM2+ shaders may write up to 4 RTs.
  // SM1.x has no ColorOut register file; the output is r0 implicitly,
  // and that path stays gated through the SM1.x lowering work.
  std::array<int, 4> oC_arg_idx{};
  oC_arg_idx.fill(-1);
  int oDepth_arg_idx = -1;
  // [[sample_mask]] coverage output field index (D3DRS_MULTISAMPLEMASK); -1
  // when the mask is inert (all-ones or single-sample), so no output is added.
  int cov_arg_idx = -1;
  // SM3 VS outputs: `dcl_<usage> o#` registers, dcl-driven semantics.
  // Output # is the dcl's bound_to.num; usage gives the AIR user name.
  // o#'s register file decodes as TexcoordOut/Output (=6); same
  // numeric value, distinct meaning. -1 unbound, ≥ 0 is the
  // OutputVertex arg index. oN_is_position aliases the slot to oPos
  // for Position dcls so the existing field-0 plumbing covers them.
  std::array<int, 16> oN_arg_idx{};
  oN_arg_idx.fill(-1);
  std::array<bool, 16> oN_is_position{};
  std::array<Value *, 16> oN_slot{};
  bool sm12_vs_varyings = is_vertex && shader->header.major <= 2;
  bool sm3_vs_outputs = is_vertex && shader->header.major == 3;
  uint32_t clip_dist_field_idx = ~0u;
  if (is_vertex) {
    sig.DefineOutput(air::OutputPosition{.type = air::msl_float4});
    // 8-element clip-distance array: every VS unconditionally writes
    // the array even when D3DRS_CLIPPLANEENABLE is 0, because the host
    // packs only enabled planes consecutively and signals "no plane is
    // enabled" by setting clip_count to 0; the per-lane select below
    // then short-circuits to 0.0, which the GPU treats as "not
    // clipped". This avoids per-draw VS variants keyed on
    // CLIPPLANEENABLE; DXVK uses the same shape via a spec constant
    // (src/dxso/dxso_compiler.cpp) for the same reason.
    clip_dist_field_idx = sig.DefineOutput(air::OutputClipDistance{.count = 8});
    if (sm12_vs_varyings) {
      bool oD_used[2] = {};
      bool oT_used[8] = {};
      bool oFog_used = false;
      DxsoBytecodeIter scan(shader->bytecode.data(), static_cast<uint32_t>(shader->bytecode.size()), shader->header);
      DxsoInstruction sins{};
      while (scan.next(sins)) {
        if (sins.opcode == DxsoOpcode::End)
          break;
        if (!sins.has_dst)
          continue;
        if (sins.dst.base.type == DxsoRegisterType::AttributeOut && sins.dst.base.num < 2)
          oD_used[sins.dst.base.num] = true;
        else if (sins.dst.base.type == DxsoRegisterType::TexcoordOut && sins.dst.base.num < 8)
          oT_used[sins.dst.base.num] = true;
        else if (sins.dst.base.type == DxsoRegisterType::RasterizerOut && sins.dst.base.num == 1)
          oFog_used = true;
        else if (sins.dst.base.type == DxsoRegisterType::RasterizerOut && sins.dst.base.num == 2)
          oPts_used = true;
      }
      // The full legacy varying set is defined regardless of what the
      // shader writes. The PS side always claims COLOR0..1, TEXCOORD0..7
      // and FOG0 (its stub tail), and Metal delivers NaN for a claimed
      // input the vertex function does not produce, so a sparse VS fed
      // NaN into every unwritten varying a pixel shader read. Unwritten
      // slots now carry their D3D9 default seeds instead.
      (void)oD_used;
      (void)oT_used;
      (void)oFog_used;
      for (int i = 0; i < 2; ++i) {
        oD_arg_idx[i] = (int)sig.DefineOutput(
            air::OutputVertex{
                .user = "COLOR" + std::to_string(i),
                .type = air::msl_float4,
            }
        );
      }
      for (int i = 0; i < 8; ++i) {
        oT_arg_idx[i] = (int)sig.DefineOutput(
            air::OutputVertex{
                .user = "TEXCOORD" + std::to_string(i),
                .type = air::msl_float4,
            }
        );
      }
      oFog_arg_idx = (int)sig.DefineOutput(
          air::OutputVertex{
              .user = "FOG0",
              .type = air::msl_float4,
          }
      );
      if (oPts_used)
        oPts_arg_idx = (int)sig.DefineOutput(air::OutputPointSize{});
    } else if (sm3_vs_outputs) {
      // SM3 VS: walk dcls, emit one OutputVertex per `dcl_<usage> o#`.
      // Position routes to the oPos slot (already defined above);
      // PointSize routes to the AIR [[point_size]] output via
      // oN_is_pointsize aliasing (mirroring oN_is_position). Color /
      // Texcoord / Fog become user-named varyings whose name matches
      // the PS InputFragmentStageIn naming convention so linkage works.
      bool vs3_sem_color[2] = {};
      bool vs3_sem_texcoord[8] = {};
      bool vs3_sem_fog = false;
      for (const auto &d : shader->metadata.dcls) {
        if (d.bound_to.type != DxsoRegisterType::Output || d.bound_to.num >= oN_arg_idx.size())
          continue;
        if (d.dcl.usage == DxsoUsage::Position) {
          oN_is_position[d.bound_to.num] = true;
          continue;
        }
        if (d.dcl.usage == DxsoUsage::PointSize) {
          oN_is_pointsize[d.bound_to.num] = true;
          oPts_used = true;
          continue;
        }
        const char *base = nullptr;
        switch (d.dcl.usage) {
        case DxsoUsage::Color:
          base = "COLOR";
          break;
        case DxsoUsage::Texcoord:
          base = "TEXCOORD";
          break;
        case DxsoUsage::Fog:
          base = "FOG";
          break;
        default:
          break; // Depth / etc; TODO
        }
        if (!base)
          continue;
        // Track which of the PS-stub interpolants (COLOR0..1,
        // TEXCOORD0..7, FOG0) the shader covers itself; semantics beyond
        // that set (a matched SM3 pair may use COLOR2+ and declare the
        // input on the PS side) are emitted as-is, an unclaimed vertex
        // output links fine.
        if (d.dcl.usage == DxsoUsage::Color && d.dcl.usage_index < 2)
          vs3_sem_color[d.dcl.usage_index] = true;
        else if (d.dcl.usage == DxsoUsage::Texcoord && d.dcl.usage_index < 8)
          vs3_sem_texcoord[d.dcl.usage_index] = true;
        else if (d.dcl.usage == DxsoUsage::Fog && d.dcl.usage_index == 0)
          vs3_sem_fog = true;
        oN_arg_idx[d.bound_to.num] = (int)sig.DefineOutput(
            air::OutputVertex{
                .user = std::string(base) + std::to_string(d.dcl.usage_index),
                .type = air::msl_float4,
            }
        );
      }
      // Stub the legacy interpolants the shader does not declare, the
      // same full-set contract the SM1/2 path and the PS side keep:
      // Metal reads NaN from a claimed input with no vertex output, so
      // every slot must exist. Stubs ride the oD/oT/oFog machinery and
      // deliver their default seeds.
      for (uint32_t i = 0; i < 2; ++i)
        if (!vs3_sem_color[i])
          oD_arg_idx[i] = (int)sig.DefineOutput(
              air::OutputVertex{
                  .user = "COLOR" + std::to_string(i),
                  .type = air::msl_float4,
              }
          );
      for (uint32_t i = 0; i < 8; ++i)
        if (!vs3_sem_texcoord[i])
          oT_arg_idx[i] = (int)sig.DefineOutput(
              air::OutputVertex{
                  .user = "TEXCOORD" + std::to_string(i),
                  .type = air::msl_float4,
              }
          );
      if (!vs3_sem_fog)
        oFog_arg_idx = (int)sig.DefineOutput(
            air::OutputVertex{
                .user = "FOG0",
                .type = air::msl_float4,
            }
        );
      if (oPts_used)
        oPts_arg_idx = (int)sig.DefineOutput(air::OutputPointSize{});
    }
  } else {
    // Pre-scan PS body for ColorOut writes so we only define
    // [[color(N)]] outputs for the slots the shader actually uses;
    // a shader that touches only oC2 should produce a function
    // returning just that RT, not three unused zero-filled ones.
    // FXC always writes oC0, so the empty-shader fallback below keeps
    // the no-write degenerate case rendering transparent black
    // instead of skipping the function output entirely.
    bool oC_used[4] = {};
    bool oDepth_used = false;
    DxsoBytecodeIter scan(shader->bytecode.data(), static_cast<uint32_t>(shader->bytecode.size()), shader->header);
    DxsoInstruction sins{};
    while (scan.next(sins)) {
      if (sins.opcode == DxsoOpcode::End)
        break;
      if (!sins.has_dst)
        continue;
      if (sins.dst.base.type == DxsoRegisterType::ColorOut && sins.dst.base.num < 4)
        oC_used[sins.dst.base.num] = true;
      else if (sins.dst.base.type == DxsoRegisterType::DepthOut)
        oDepth_used = true;
      // SM 1.x TexDepth / TexM3x2Depth encode the destination as a
      // Temp / Texture register, not DepthOut; but the opcode
      // semantically writes [[depth]]. Treat their presence as a
      // depth-output declaration too so the OutputDepth signature
      // entry + slot get allocated. wined3d glsl_shader.c +
      // :6563 emit `gl_FragDepth = …` for both.
      else if (sins.opcode == DxsoOpcode::TexDepth || sins.opcode == DxsoOpcode::TexM3x2Depth)
        oDepth_used = true;
    }
    bool any = false;
    for (int i = 0; i < 4; ++i)
      any = any || oC_used[i];
    if (!any)
      oC_used[0] = true;
    for (int i = 0; i < 4; ++i) {
      if (!oC_used[i])
        continue;
      // Dual-source blending borrows oC1 as attachment 0's second color
      // index; only oC0/oC1 are addressable in that mode (mirrors
      // dxbc_signature.cpp's reg > 1 guard). Higher slots are dropped.
      if (dual_source && i > 1)
        continue;
      oC_arg_idx[i] = (int)sig.DefineOutput(
          air::OutputRenderTarget{
              .dual_source_blending = dual_source,
              .index = (uint32_t)i,
              .type = air::msl_float4,
          }
      );
    }
    if (oDepth_used)
      oDepth_arg_idx = (int)sig.DefineOutput(
          air::OutputDepth{
              .depth_argument = air::DepthArgument::any,
          }
      );
    // D3DRS_MULTISAMPLEMASK coverage output: the epilogue ANDs the app mask
    // into hardware coverage. Added last so it never shifts the oC / oDepth
    // field indices the retval assembly below relies on.
    if (emit_sample_mask)
      cov_arg_idx = (int)sig.DefineOutput(air::OutputCoverageMask{});
  }
  auto [fn, fn_md] = sig.CreateFunction(name, context, module, 0, false);

  IRBuilder<> builder(BasicBlock::Create(context, "entry", fn));

  // AIRBuilder for high-level texture / sampler ops: same wrapper
  // dxbc_converter uses. Texture sample lowerings call
  // through this so the air.sample.* intrinsic emission stays in one
  // place. Debug stream is null'd; AIR diagnostics aren't surfaced
  // through the DXSO path.
  raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(builder, nulldbg);

  // Temp register file r0..r31: one alloca, zero-init to avoid undef.
  // D3D9 PS leaves unwritten lanes as default; undef causes per-frame
  // flicker on alpha-blended output. MTL_SHADER_VALIDATION masks this
  // by zeroing undef reads; we do it unconditionally.
  auto *float4Ty = FixedVectorType::get(Type::getFloatTy(context), 4);
  auto *int4Ty = FixedVectorType::get(Type::getInt32Ty(context), 4);
  auto *tempArrTy = ArrayType::get(float4Ty, 32);
  auto *temps = builder.CreateAlloca(tempArrTy, nullptr, "r");
  builder.CreateStore(ConstantAggregateZero::get(tempArrTy), temps);

  // VS address register a0. <4 x i32> alloca, zero-seeded; Mova
  // writes the rounded float-to-int result here, and load_src reads
  // it back when a Const operand carries a relative-address suffix.
  Value *a0_slot = nullptr;
  if (is_vertex) {
    a0_slot = builder.CreateAlloca(int4Ty, nullptr, "a0");
    builder.CreateStore(ConstantAggregateZero::get(int4Ty), a0_slot);
  }

  // Loop iterator aL; single i32. Loop sets it to the initial value
  // from the integer constant; EndLoop steps it by the stride. Const
  // reads with relative.base.type == Loop pick it up directly. SM3
  // allows aL in both VS and PS, so allocate unconditionally
  // (DXVK's emitGetOperandPtr for Loop doesn't gate on stage either).
  auto *aL_slot = builder.CreateAlloca(Type::getInt32Ty(context), nullptr, "aL");
  builder.CreateStore(builder.getInt32(0), aL_slot);

  // Predicate register p0; <4 x i1>, one bool per lane. Setp writes
  // it from a lane-wise compare; predicated instructions read it
  // through store_dst and gate per-lane writes accordingly.
  auto *bool4Ty = FixedVectorType::get(Type::getInt1Ty(context), 4);
  auto *p0_slot = builder.CreateAlloca(bool4Ty, nullptr, "p0");
  builder.CreateStore(ConstantAggregateZero::get(bool4Ty), p0_slot);

  // VS oPos slot. Pre-seeded with (0,0,0,1) so a shader that never
  // writes oPos still produces a clip-safe Position; zero-w divides
  // to NaN at clip and the draw rasterizes nothing.
  auto *zero = ConstantFP::get(Type::getFloatTy(context), 0.0);
  auto *one = ConstantFP::get(Type::getFloatTy(context), 1.0);
  Value *out_slot = nullptr;
  if (is_vertex) {
    out_slot = builder.CreateAlloca(float4Ty, nullptr, "oPos");
    builder.CreateStore(ConstantVector::get({zero, zero, zero, one}), out_slot);
  }
  // PS oC# slots: one alloca per render target the pre-scan
  // discovered. Each is pre-seeded with transparent black so the
  // never-written degenerate case still produces a defined RT
  // sample.
  std::array<Value *, 4> oC_slot{};
  Value *oDepth_slot = nullptr;
  if (!is_vertex) {
    auto *zero4 = ConstantAggregateZero::get(float4Ty);
    for (int i = 0; i < 4; ++i) {
      if (oC_arg_idx[i] < 0)
        continue;
      oC_slot[i] = builder.CreateAlloca(float4Ty, nullptr, ("oC" + std::to_string(i)).c_str());
      builder.CreateStore(zero4, oC_slot[i]);
    }
    if (oDepth_arg_idx >= 0) {
      // <4 x float> alloca so store_dst's float4 plumbing can write
      // through it uniformly; epilogue extracts lane 0 for the actual
      // OutputDepth scalar.
      oDepth_slot = builder.CreateAlloca(float4Ty, nullptr, "oDepth");
      builder.CreateStore(zero4, oDepth_slot);
    }
  }

  // VS varying-output slots: one alloca per oD#/oT# the pre-scan
  // discovered. Pre-seeded with zero so a partial-mask write still
  // produces a defined varying for the lanes the shader didn't touch.
  std::array<Value *, 2> oD_slot{};
  std::array<Value *, 8> oT_slot{};
  Value *oFog_slot = nullptr;
  // oPts (point size): float4 alloca, extract lane 0 for [[point_size]].
  // The injecting variant seeds the slot from the uniform's size lane so a
  // point draw whose bytecode never writes oPts rasterises at the
  // render-state size; a shader that writes its own oPts overwrites this.
  // Bytecode-only oPts (a non-injecting point/other draw) keeps the 1.0
  // default seed.
  Value *oPts_slot = nullptr;
  if (is_vertex && oPts_arg_idx >= 0) {
    oPts_slot = builder.CreateAlloca(float4Ty, nullptr, "oPts");
    Value *seed_splat;
    if (vs_inject_point_size && point_size_arg_idx != ~0u) {
      Value *u = builder.CreateLoad(float4Ty, fn->getArg(point_size_arg_idx));
      Value *size = builder.CreateExtractElement(u, builder.getInt32(0));
      seed_splat = builder.CreateVectorSplat(4, size);
    } else {
      Value *one = ConstantFP::get(builder.getFloatTy(), 1.0f);
      seed_splat = ConstantVector::getSplat(llvm::ElementCount::getFixed(4), cast<llvm::Constant>(one));
    }
    builder.CreateStore(seed_splat, oPts_slot);
  }
  if (sm12_vs_varyings) {
    auto *zero4 = ConstantAggregateZero::get(float4Ty);
    // Unwritten varying lanes carry D3D9's colour defaults, not zero: diffuse
    // seeds (1, 1, 1, 1) and texcoord lanes (0, 0, 0, 1), so a shader that
    // writes only some lanes (mov oD0.x) leaves those defaults in the rest.
    // Specular seeds (0, 0, 0, 0): the fixed-function fog can read specular
    // alpha as its fog factor, so an unwritten specular must fog fully (w = 0),
    // matching wined3d. The wine uninitialized-varyings tests accept either
    // alpha on the texcoord lanes (vendor-dependent), so those keep DXVK's 1.
    auto splat4 = [&](float x, float y, float z, float w) -> Constant * {
      Constant *lanes[4] = {
          ConstantFP::get(builder.getFloatTy(), x), ConstantFP::get(builder.getFloatTy(), y),
          ConstantFP::get(builder.getFloatTy(), z), ConstantFP::get(builder.getFloatTy(), w)
      };
      return ConstantVector::get(lanes);
    };
    Constant *diffuse_default = splat4(1.0f, 1.0f, 1.0f, 1.0f);
    Constant *specular_default = splat4(0.0f, 0.0f, 0.0f, 0.0f);
    Constant *alpha1_default = splat4(0.0f, 0.0f, 0.0f, 1.0f);
    for (int i = 0; i < 2; ++i) {
      if (oD_arg_idx[i] < 0)
        continue;
      oD_slot[i] = builder.CreateAlloca(float4Ty, nullptr, ("oD" + std::to_string(i)).c_str());
      builder.CreateStore(i == 0 ? diffuse_default : specular_default, oD_slot[i]);
    }
    for (int i = 0; i < 8; ++i) {
      if (oT_arg_idx[i] < 0)
        continue;
      oT_slot[i] = builder.CreateAlloca(float4Ty, nullptr, ("oT" + std::to_string(i)).c_str());
      builder.CreateStore(alpha1_default, oT_slot[i]);
    }
    if (oFog_arg_idx >= 0) {
      oFog_slot = builder.CreateAlloca(float4Ty, nullptr, "oFog");
      builder.CreateStore(zero4, oFog_slot);
    }
  } else if (sm3_vs_outputs) {
    auto *zero4 = ConstantAggregateZero::get(float4Ty);
    for (int i = 0; i < 16; ++i) {
      // Position aliases oPos (already (0,0,0,1)-seeded); PointSize
      // aliases oPts_slot (1.0-seeded above). Other varyings get their
      // own zero-seeded alloca. Undcl'd o# stays null and silently
      // swallows any spurious write.
      if (oN_is_position[i]) {
        oN_slot[i] = out_slot;
      } else if (oN_is_pointsize[i]) {
        oN_slot[i] = oPts_slot;
      } else if (oN_arg_idx[i] >= 0) {
        oN_slot[i] = builder.CreateAlloca(float4Ty, nullptr, ("o" + std::to_string(i)).c_str());
        builder.CreateStore(zero4, oN_slot[i]);
      }
    }
    // Stub interpolants (legacy semantics the shader never declares):
    // zero-seeded slots so the epilogue has something to load. SM3
    // keeps zero seeds, native SM3 defaults are hardware-chaotic and
    // wine's tests accept zero alpha there; the D3D9 white-diffuse
    // convention is a SM1/2 behavior.
    for (int i = 0; i < 2; ++i)
      if (oD_arg_idx[i] >= 0) {
        oD_slot[i] = builder.CreateAlloca(float4Ty, nullptr, ("oD" + std::to_string(i) + ".stub").c_str());
        builder.CreateStore(zero4, oD_slot[i]);
      }
    for (int i = 0; i < 8; ++i)
      if (oT_arg_idx[i] >= 0) {
        oT_slot[i] = builder.CreateAlloca(float4Ty, nullptr, ("oT" + std::to_string(i) + ".stub").c_str());
        builder.CreateStore(zero4, oT_slot[i]);
      }
    if (oFog_arg_idx >= 0) {
      oFog_slot = builder.CreateAlloca(float4Ty, nullptr, "oFog.stub");
      builder.CreateStore(zero4, oFog_slot);
    }
  }

  // Pre-compute the point-sprite substitution value once if requested.
  // Used to override both v# (SM3 PS dcl_texcoord) and t# (SM2/SM1.4
  // PS texture-register-file) reads. D3D9 POINTSPRITEENABLE replaces
  // all PS texcoord inputs uniformly regardless of D3DTSS_TEXCOORDINDEX.
  Value *point_sprite_v = nullptr;
  if (!is_vertex && ps_point_sprite && ps_point_coord_arg_idx >= 0) {
    auto *pc2 = fn->getArg(ps_point_coord_arg_idx);
    Value *px = builder.CreateExtractElement(pc2, builder.getInt32(0));
    Value *py = builder.CreateExtractElement(pc2, builder.getInt32(1));
    Value *v4 = UndefValue::get(float4Ty);
    v4 = builder.CreateInsertElement(v4, px, builder.getInt32(0));
    v4 = builder.CreateInsertElement(v4, py, builder.getInt32(1));
    v4 = builder.CreateInsertElement(v4, ConstantFP::get(Type::getFloatTy(context), 0.0f), builder.getInt32(2));
    v4 = builder.CreateInsertElement(v4, ConstantFP::get(Type::getFloatTy(context), 1.0f), builder.getInt32(3));
    point_sprite_v = v4;
  }

  // Input register file v0..v15. Allocas + an upfront copy from the
  // function's stage_in args (legacy path) or buffer-pulled fetch
  // (manual_fetch), so load_src reads through GEP+load like the temp
  // file does. Inputs the shader didn't declare get
  // ConstantAggregateZero (matches DXSO "undeclared inputs are zero").
  auto *inputArrTy = ArrayType::get(float4Ty, 16);
  auto *inputs = builder.CreateAlloca(inputArrTy, nullptr, "v");

  // air::AirType is constructed lazily; only the manual-fetch path
  // needs it (for _dxmt_vertex_buffer_entry and the AIRBuilderContext
  // that pull_vec4_from_addr expects).
  std::optional<air::AirType> air_types_storage;
  auto get_air_types = [&]() -> air::AirType & {
    if (!air_types_storage)
      air_types_storage.emplace(context);
    return *air_types_storage;
  };

  for (uint32_t i = 0; i < 16; ++i) {
    Value *src = nullptr;
    if (manual_fetch && ia_element_idx[i] >= 0) {
      const auto &element = ia_layout->elements[ia_element_idx[i]];
      auto &types = get_air_types();
      auto *vbuf_table = builder.CreateBitCast(
          fn->getArg(vbuf_table_arg_idx),
          types._dxmt_vertex_buffer_entry->getPointerTo((uint32_t)air::AddressSpace::constant)
      );
      // popcount of slot bits below this element's slot; same packing
      // dxbc_converter_basicblock.cpp uses so the host can
      // produce one shared layout for both pipelines.
      unsigned int shift = 32u - element.slot;
      unsigned int vbuf_entry_index = element.slot ? __builtin_popcount((ia_layout->slot_mask << shift) >> shift) : 0u;
      auto *vbuf_entry = builder.CreateLoad(
          types._dxmt_vertex_buffer_entry,
          builder.CreateConstGEP1_32(types._dxmt_vertex_buffer_entry, vbuf_table, vbuf_entry_index)
      );
      auto *base_addr = builder.CreateExtractValue(vbuf_entry, {0});
      auto *stride = builder.CreateExtractValue(vbuf_entry, {1});
      // [[vertex_id]] is post-resolution vertex number for both draw modes.
      // [[base_vertex]] declared for dcl side-effects only; MUST NOT add: would
      // double-add and overflow buffer (verified smoke_draw_indexed P2).
      Value *index;
      if (element.step_function) {
        Value *instance_id = fn->getArg(instance_id_arg_idx);
        Value *base_instance = fn->getArg(base_instance_arg_idx);
        if (element.step_rate) {
          index =
              builder.CreateAdd(base_instance, builder.CreateUDiv(instance_id, builder.getInt32(element.step_rate)));
        } else {
          index = base_instance;
        }
      } else {
        index = fn->getArg(vid_arg_idx);
      }
      // base_vertex_arg_idx names the [[base_vertex]] system input
      // declared on the signature above. Keeping the dcl alive (even
      // unread here) leaves room for a follow-up lowering; e.g.,
      // recovering the raw 0-indexed vertex number for stream-output
      // emulation; without re-shuffling input indices.
      (void)base_vertex_arg_idx;
      auto *byte_offset =
          builder.CreateAdd(builder.CreateMul(stride, index), builder.getInt32(element.aligned_byte_offset));
      air::AIRBuilderContext abctx{
          .llvm = context,
          .module = module,
          .builder = builder,
          .types = types,
          .air = air,
      };
      auto result =
          air::pull_vec4_from_addr((air::MTLAttributeFormat)element.format, base_addr, byte_offset).build(abctx);
      if (auto err = result.takeError()) {
        // Unsupported MTLAttributeFormat (gaps in pull_vec4_from_addr_checked).
        // Fail open: zero-fill so the rest of the shader still
        // compiles. A later commit either widens the format coverage
        // or rejects the layout up front.
        llvm::consumeError(std::move(err));
        src = ConstantAggregateZero::get(float4Ty);
      } else {
        src = result.get();
        if (src->getType() == int4Ty) {
          // Non-normalized integer formats must be presented as floats.
          // D3D9 spec: UBYTE4 5 → v.x = 5.0, not int bits.
          // Bone-index path depends on this; bitcast would collapse to 0.
          src = builder.CreateSIToFP(src, float4Ty);
        }
      }
    } else if (input_arg_idx[i] >= 0) {
      // POINTSPRITEENABLE substitution: when v<N> was dcl'd as
      // TEXCOORD (SM3 PS path), override the stage_in read with the
      // point-sprite UV vec4. v<N> dcl'd as COLOR is left alone.
      if (point_sprite_v && i < ps_v_is_texcoord.size() && ps_v_is_texcoord[i])
        src = point_sprite_v;
      else {
        src = fn->getArg(input_arg_idx[i]);
        // Partial dcl mask: overwrite undeclared lanes with the input
        // register defaults instead of the raw interpolant.
        if (!is_vertex && i < ps_input_mask.size() && ps_input_mask[i] != 0xF) {
          for (uint32_t lane = 0; lane < 4; ++lane) {
            if (ps_input_mask[i] & (1u << lane))
              continue;
            src = builder.CreateInsertElement(
                src, ConstantFP::get(Type::getFloatTy(context), lane == 3 ? 1.0f : 0.0f), builder.getInt32(lane)
            );
          }
        }
      }
    } else {
      src = ConstantAggregateZero::get(float4Ty);
    }
    auto *slot = builder.CreateGEP(inputArrTy, inputs, {builder.getInt32(0), builder.getInt32(i)});
    builder.CreateStore(src, slot);
  }

  // PS t# inputs (t0..t7): allocated unconditionally. SM2/SM1.4 pre-load
  // from stage-in; SM1.0..1.3 populated at runtime by tex opcode.
  auto *texInputArrTy = ArrayType::get(float4Ty, 8);
  Value *tex_inputs = nullptr;
  if (!is_vertex) {
    tex_inputs = builder.CreateAlloca(texInputArrTy, nullptr, "t");
    for (uint32_t i = 0; i < 8; ++i) {
      Value *src;
      if (point_sprite_v) {
        // POINTSPRITEENABLE substitution for the t# (SM2 / SM 1.4 PS)
        // register file. Override regardless of whether the slot would
        // otherwise have a stage_in binding; apps may declare zero
        // texcoord inputs but still sample with the implicit point
        // sprite UV, which D3DRS_POINTSPRITEENABLE injects.
        src = point_sprite_v;
      } else if (ps_tex_arg_idx[i] >= 0) {
        src = fn->getArg(ps_tex_arg_idx[i]);
      } else {
        src = ConstantAggregateZero::get(float4Ty);
      }
      auto *slot = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(i)});
      builder.CreateStore(src, slot);
    }
  }

  // Operand helpers: closures over the entry-block builder + the
  // register files. Each helper returns nullptr when the operand
  // shape isn't covered yet (Input/Const/sampler/...); the per-opcode
  // arms treat that as "skip this instruction" rather than fail.
  // D3DSAMP_MIPMAPLODBIAS for a PS sampler, host-written into the
  // bool-constant buffer tail (float at u32 index 8 + slot). Metal
  // samplers carry no LOD bias so every sample site applies it, the
  // same way the d3d11 path reads it from its argument buffer. Every
  // pixel-shader sample pays the biased form and the uniform load even
  // when the app leaves the bias at zero (a numeric no-op): skipping it
  // would take a compile-time variant per bias-in-use mask, and forking
  // PSOs is costlier than the always-on bias operand. Not implemented
  // for vertex samplers; their buffer(2) carries no bias table.
  auto load_samp_bias = [&](uint32_t slot) -> Value * {
    if (is_vertex)
      return nullptr;
    Value *bias_ptr =
        builder.CreateGEP(builder.getInt32Ty(), fn->getArg(bc_arg_idx), builder.getInt32(8 + (int)slot));
    Value *bias_raw = builder.CreateLoad(builder.getInt32Ty(), bias_ptr);
    return builder.CreateBitCast(bias_raw, builder.getFloatTy());
  };
  // Shared SM1.x projected-texturing divide: coord / coord.w, gated on the
  // per-stage PROJECTED bit in the runtime bool-constant blob at index 27
  // (bit = stage). The runtime select leaves a non-projected stage untouched,
  // so a w==0 texcoord on a non-projected app never divides. coord is a 4-lane
  // texcoord; callers gate by opcode/version and pass their stage slot.
  auto apply_projected = [&](Value *coord, uint32_t slot) -> Value * {
    Value *proj_ptr = builder.CreateGEP(builder.getInt32Ty(), fn->getArg(bc_arg_idx), builder.getInt32(27));
    Value *proj_mask = builder.CreateLoad(builder.getInt32Ty(), proj_ptr);
    Value *proj_bit =
        builder.CreateICmpNE(builder.CreateAnd(proj_mask, builder.getInt32(1u << slot)), builder.getInt32(0));
    Value *w = builder.CreateExtractElement(coord, builder.getInt32(3));
    Value *w_splat = builder.CreateVectorSplat(4, w);
    return builder.CreateSelect(proj_bit, builder.CreateFDiv(coord, w_splat), coord);
  };
  auto load_src = [&](const DxsoSrcRegister &src) -> Value * {
    Value *v = nullptr;
    switch (src.base.type) {
    case DxsoRegisterType::Temp: {
      auto *gep = builder.CreateGEP(tempArrTy, temps, {builder.getInt32(0), builder.getInt32(src.base.num)});
      v = builder.CreateLoad(float4Ty, gep);
      break;
    }
    case DxsoRegisterType::Input: {
      if (src.base.num >= 16)
        return nullptr;
      Value *in_idx = builder.getInt32(src.base.num);
      // SM3 dynamic input indexing: v#[aL] in ps_3_0 (and vs_3_0). The
      // inputs already live in an indexable register-file array, so the
      // relative read is the same add-and-clamp the Const arm does; aL
      // is the only legal index register for the input file.
      if (src.has_relative) {
        if (src.relative.base.type != DxsoRegisterType::Loop)
          return nullptr;
        Value *off = builder.CreateLoad(Type::getInt32Ty(context), aL_slot);
        in_idx = builder.CreateAdd(in_idx, off);
        in_idx = air.CreateIntBinOp(llvm::air::AIRBuilder::max, in_idx, builder.getInt32(0), /*Signed=*/true);
        in_idx = air.CreateIntBinOp(llvm::air::AIRBuilder::min, in_idx, builder.getInt32(15), /*Signed=*/true);
      }
      auto *gep = builder.CreateGEP(inputArrTy, inputs, {builder.getInt32(0), in_idx});
      v = builder.CreateLoad(float4Ty, gep);
      break;
    }
    case DxsoRegisterType::Texture: {
      // PS-only: SM2 / SM 1.4 reads interpolated TEXCOORD<n> through
      // the t# register file; SM 1.0..1.3 reads the same slot but it
      // was populated by an earlier `tex t#` instruction. VS doesn't
      // own a Texture-file source. tex_inputs is allocated whenever
      // !is_vertex regardless of SM (zero-init covers both SM3-PS-
      // never-uses and SM 1.0..1.3-not-yet-populated cases).
      if (is_vertex || src.base.num >= 8 || !tex_inputs)
        return nullptr;
      auto *gep = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(src.base.num)});
      v = builder.CreateLoad(float4Ty, gep);
      break;
    }
    case DxsoRegisterType::Const: {
      // def-baked Float32 literal first; codegen-time constant beats
      // a runtime load. A relative read must skip the def table: the
      // operand's base register having a def says nothing about the
      // register actually addressed at runtime (DXVK
      // src/dxso/dxso_compiler.cpp emitRegisterLoadRaw only takes the
      // def id when no relative operand is present); the host re-
      // applies def values into the uploaded CB for that case. Int /
      // Bool defs share the Const switch arm here as a per-DefKind
      // dispatch only because the decoder gives each its own
      // DxsoRegisterType (ConstInt / ConstBool); those types are
      // handled by the dedicated arms below.
      // ps_1_x clamps float constants to [-1, 1] (DXVK
      // emitClampBoundReplicant; wined3d shader.c). Bake the clamp
      // into the literal; the runtime load clamps below.
      bool ps1x_const_clamp = !is_vertex && shader->header.major == 1;
      const DxsoBoundConst *match = nullptr;
      if (!src.has_relative) {
        for (const auto &c : shader->metadata.consts) {
          if (c.bound_to.type == src.base.type && c.bound_to.num == src.base.num) {
            match = &c;
            break;
          }
        }
      }
      if (match && match->def.kind == DxsoDefKind::Float32) {
        auto *fTy = Type::getFloatTy(context);
        auto bake = [&](float x) -> Constant * {
          if (ps1x_const_clamp)
            x = std::clamp(x, -1.0f, 1.0f);
          return ConstantFP::get(fTy, x);
        };
        Constant *lanes[4] = {
            bake(match->def.payload.f32[0]),
            bake(match->def.payload.f32[1]),
            bake(match->def.payload.f32[2]),
            bake(match->def.payload.f32[3]),
        };
        v = ConstantVector::get(lanes);
        break;
      }
      if (match)
        return nullptr; // mismatched def kind on a Float register;
                        // malformed bytecode, skip.
      // Runtime-set: load from the Float CB at slot src.base.num,
      // optionally plus the indexed component of a0 when the operand
      // carries a relative-address suffix (`c[a0.x + N]`). vs_2_0/3_0
      // caps c# at 255, ps_2_0/3_0 at 223; the host binds a CB sized to
      // each ceiling. Out-of-range registers come from malformed
      // bytecode; skip rather than emit an OOB read on the host CB.
      if (src.base.num >= (is_vertex ? vs_float_const_count : 224u))
        return nullptr;
      auto *cbPtr = fn->getArg(cb_arg_idx);
      Value *idx = builder.getInt32(src.base.num);
      if (src.has_relative) {
        Value *off = nullptr;
        if (src.relative.base.type == DxsoRegisterType::Addr && src.relative.base.num == 0 && a0_slot) {
          auto *a0v = builder.CreateLoad(int4Ty, a0_slot);
          off = builder.CreateExtractElement(a0v, builder.getInt32(src.relative.swizzle[0]));
        } else if (src.relative.base.type == DxsoRegisterType::Loop) {
          off = builder.CreateLoad(Type::getInt32Ty(context), aL_slot);
        } else {
          return nullptr;
        }
        idx = builder.CreateAdd(idx, off);
        // Clamp the relative index into the declared Float CB register
        // file. a0/aL are signed and runtime-computed, so a malformed
        // shader can drive idx negative or past the register file.
        // DXVK src/dxso/dxso_compiler.cpp emitArrayIndex leaves the same
        // index unclamped and leans on Vulkan robustBufferAccess to
        // fold an OOB load to 0 (see d3d9_device.cpp "rely on robustness
        // to return 0 on OOB reads"); Metal has no robustness mode, so an
        // unclamped GEP would fault or read stray GPU memory outside the
        // CB allocation. The register file caps at vs c# (255 hardware-VP,
        // up to 8191 software-VP) / ps c# 223; clamp to [0, cap] via signed
        // air.max/air.min so the GEP stays inside the bound allocation.
        const uint32_t cb_max_index = (is_vertex ? vs_float_const_count : 224u) - 1u;
        idx = air.CreateIntBinOp(llvm::air::AIRBuilder::max, idx, builder.getInt32(0), /*Signed=*/true);
        idx = air.CreateIntBinOp(
            llvm::air::AIRBuilder::min, idx, builder.getInt32(cb_max_index), /*Signed=*/true
        );
      }
      auto *gep = builder.CreateGEP(float4Ty, cbPtr, idx);
      v = builder.CreateLoad(float4Ty, gep);
      if (ps1x_const_clamp) {
        auto clampSplat = [&](double x) -> Constant * {
          return ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(Type::getFloatTy(context), x));
        };
        v = air.CreateFPBinOp(
            llvm::air::AIRBuilder::fmax, air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, v, clampSplat(1.0)),
            clampSplat(-1.0)
        );
      }
      break;
    }
    case DxsoRegisterType::ConstInt: {
      // i# as a generic operand. DXVK src/dxso/dxso_compiler.cpp
      // returns the raw int4; our load_src contract is <4 x float>, so
      // we sint-to-fp the four lanes after loading and let downstream
      // swizzle/modifier handling treat it as any other float source.
      // FXC almost always uses i# only for `loop`/`rep` counts (which
      // bypass load_src entirely); a `mov r0, i0` does appear in
      // hand-written shaders though, and is the symptom that motivated
      // this arm.
      if (src.base.num >= 16)
        return nullptr;
      const DxsoBoundConst *match = nullptr;
      for (const auto &c : shader->metadata.consts) {
        if (c.bound_to.type == DxsoRegisterType::ConstInt && c.bound_to.num == src.base.num &&
            c.def.kind == DxsoDefKind::Int32) {
          match = &c;
          break;
        }
      }
      Value *as_int = nullptr;
      if (match) {
        Constant *lanes[4] = {
            builder.getInt32(match->def.payload.i32[0]),
            builder.getInt32(match->def.payload.i32[1]),
            builder.getInt32(match->def.payload.i32[2]),
            builder.getInt32(match->def.payload.i32[3]),
        };
        as_int = ConstantVector::get(lanes);
      } else {
        auto *icPtr = fn->getArg(ic_arg_idx);
        auto *gep = builder.CreateGEP(int4Ty, icPtr, builder.getInt32(src.base.num));
        as_int = builder.CreateLoad(int4Ty, gep);
      }
      v = builder.CreateSIToFP(as_int, float4Ty);
      break;
    }
    case DxsoRegisterType::ConstBool: {
      // b# as a generic operand. The dedicated `If b#` handler reads
      // the bit directly into an i1; here we lift the same bit into a
      // {0.0, 1.0} float and splat across all four lanes so a `mul r0,
      // r0, b0` gates the multiply on the bool. Def-baked literal
      // first; otherwise sample the bitmask binding (one uint, bit i =
      // b#i) shared with the If handler.
      if (src.base.num >= 16)
        return nullptr;
      auto *fTy = Type::getFloatTy(context);
      auto *one = ConstantFP::get(fTy, 1.0);
      auto *zero = ConstantFP::get(fTy, 0.0);
      Value *flt = nullptr;
      const DxsoBoundConst *match = nullptr;
      for (const auto &c : shader->metadata.consts) {
        if (c.bound_to.type == DxsoRegisterType::ConstBool && c.bound_to.num == src.base.num &&
            c.def.kind == DxsoDefKind::Bool) {
          match = &c;
          break;
        }
      }
      if (match) {
        flt = match->def.payload.u32[0] != 0 ? one : zero;
      } else {
        auto *u32Ty = Type::getInt32Ty(context);
        auto *bcPtr = fn->getArg(bc_arg_idx);
        Value *bits = builder.CreateLoad(u32Ty, bcPtr);
        Value *mask = builder.getInt32(1u << src.base.num);
        Value *bit = builder.CreateAnd(bits, mask);
        Value *cond = builder.CreateICmpNE(bit, builder.getInt32(0));
        flt = builder.CreateSelect(cond, one, zero);
      }
      v = builder.CreateVectorSplat(4, flt);
      break;
    }
    case DxsoRegisterType::MiscType: {
      // PS-only: vPos = [[position]].xy; vFace: true->+1, false->-1.
      // dxmt pins winding=Clockwise (no Y-flip), so mapping is direct;
      // don't 'correct' to Vulkan/DXVK pattern (opposite reason).
      if (is_vertex)
        return nullptr;
      if (src.base.num == 0 && ps_position_arg_idx >= 0) {
        // D3D9 vPos is the integer pixel coordinate; [[position]]
        // interpolates at the pixel center. Shift xy back by 0.5 and
        // leave zw as delivered (DXVK src/dxso/dxso_compiler.cpp
        // stores FragCoord - (0.5, 0.5, 0, 0) into vPos).
        auto *fTy = Type::getFloatTy(context);
        Constant *half_xy[4] = {
            ConstantFP::get(fTy, 0.5),
            ConstantFP::get(fTy, 0.5),
            ConstantFP::get(fTy, 0.0),
            ConstantFP::get(fTy, 0.0),
        };
        v = builder.CreateFSub(fn->getArg(ps_position_arg_idx), ConstantVector::get(half_xy));
        break;
      }
      if (src.base.num == 1 && ps_vface_arg_idx >= 0) {
        auto *fTy = Type::getFloatTy(context);
        Value *ff = fn->getArg(ps_vface_arg_idx); // i1 [[front_facing]]
        Value *sel = builder.CreateSelect(ff, ConstantFP::get(fTy, 1.0), ConstantFP::get(fTy, -1.0));
        v = builder.CreateVectorSplat(4, sel);
        break;
      }
      return nullptr;
    }
    default:
      return nullptr;
    }
    // PRE-swizzle modifiers: Dz/Dw divide RAW register z/w before swizzle.
    // Post-swizzle divide is wrong: projected texcoords become ±inf.
    if (src.modifier == DxsoRegModifier::Dz || src.modifier == DxsoRegModifier::Dw) {
      uint32_t lane_idx = (src.modifier == DxsoRegModifier::Dz) ? 2u : 3u;
      Value *lane = builder.CreateExtractElement(v, builder.getInt32(lane_idx));
      v = builder.CreateFDiv(v, builder.CreateVectorSplat(4, lane));
    }
    if (src.swizzle.raw() != 0b11100100u) {
      int mask[4] = {(int)src.swizzle[0], (int)src.swizzle[1], (int)src.swizzle[2], (int)src.swizzle[3]};
      v = builder.CreateShuffleVector(v, v, ArrayRef<int>(mask, 4));
    }
    if (src.modifier == DxsoRegModifier::None)
      return v;
    // POST-swizzle modifiers: fetch-time arithmetic on the swizzled
    // float4 (DXVK emitSrcOperandPostSwizzleModifiers). Dz/Dw were
    // already applied pre-swizzle above. Not is the bool-register
    // inversion (D3D9 spec restricts the modifier to b# / predicate
    // registers, where the value is 0.0 or 1.0); we implement it as a
    // logical "non-zero ⇒ 0, zero ⇒ 1" select so the IR reads cleanly
    // for downstream passes.
    auto kSplat = [&](double x) -> Constant * {
      return ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(Type::getFloatTy(context), x));
    };
    auto fabs = [&](Value *x) { return air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, x); };
    switch (src.modifier) {
    case DxsoRegModifier::Neg:
      v = builder.CreateFNeg(v);
      break;
    case DxsoRegModifier::Abs:
      v = fabs(v);
      break;
    case DxsoRegModifier::AbsNeg:
      v = builder.CreateFNeg(fabs(v));
      break;
    case DxsoRegModifier::Comp:
      v = builder.CreateFSub(kSplat(1.0), v);
      break;
    case DxsoRegModifier::X2:
      v = builder.CreateFMul(v, kSplat(2.0));
      break;
    case DxsoRegModifier::X2Neg:
      v = builder.CreateFNeg(builder.CreateFMul(v, kSplat(2.0)));
      break;
    case DxsoRegModifier::Bias:
      v = builder.CreateFSub(v, kSplat(0.5));
      break;
    case DxsoRegModifier::BiasNeg:
      v = builder.CreateFNeg(builder.CreateFSub(v, kSplat(0.5)));
      break;
    case DxsoRegModifier::Sign:
      v = builder.CreateFSub(builder.CreateFMul(v, kSplat(2.0)), kSplat(1.0));
      break;
    case DxsoRegModifier::SignNeg:
      v = builder.CreateFNeg(builder.CreateFSub(builder.CreateFMul(v, kSplat(2.0)), kSplat(1.0)));
      break;
    case DxsoRegModifier::Dz:
    case DxsoRegModifier::Dw:
      // Already applied pre-swizzle above (DXVK splits these out of the
      // post-swizzle modifier path).
      break;
    case DxsoRegModifier::Not: {
      // bool-register inversion. v ∈ {0.0, 1.0} per D3D9 spec; pick
      // 1.0 when v is zero and 0.0 otherwise so non-zero non-one
      // inputs (which the spec disallows but a misencoded shader can
      // emit) still produce a defined result.
      Value *zero = kSplat(0.0);
      Value *one = kSplat(1.0);
      Value *isZero = builder.CreateFCmpOEQ(v, zero);
      v = builder.CreateSelect(isZero, one, zero);
      break;
    }
    default:
      return nullptr; // unreachable; enum values past Not are masked
                      // out by the decoder per dxso_decoder.hpp
    }
    return v;
  };

  // Declared up front so store_dst can read its predicate fields.
  // Each iteration of the lowering loop overwrites it via it.next(ins).
  DxsoInstruction ins{};
  auto store_dst = [&](const DxsoDstRegister &dst, Value *value) {
    if (!value)
      return;
    // DXSO destination modifiers per DXVK
    // src/dxso/dxso_compiler.h (emitDstStore): result-shift
    // first as a power-of-two multiply (4-bit two's-complement shift, range
    // -8..+7), then saturate as a [0,1] clamp. Both apply to the
    // full <4 x float> before mask blending.
    if (dst.shift != 0) {
      float scale = dst.shift < 0 ? 1.0f / static_cast<float>(1 << -dst.shift) : static_cast<float>(1 << dst.shift);
      auto *fTy = Type::getFloatTy(context);
      Constant *splat = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(fTy, scale));
      value = builder.CreateFMul(value, splat);
    }
    // A write to oFog (RasterizerOut[1]) is force-saturated regardless of
    // the dst saturate bit; the fog factor is defined on [0,1] and the
    // FFP blend clamps it. DXVK emitDstStore (dxso_compiler.h)
    // forces saturate for RasterOutFog for the same reason.
    bool force_saturate = is_vertex && dst.base.type == DxsoRegisterType::RasterizerOut && dst.base.num == 1;
    if (dst.saturate || force_saturate)
      value = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, value);
    // Relative-addressed destinations (a vs_3_0 `mov o[aL + N]` writing an
    // output array inside a loop) are not supported: the switch below
    // indexes the static base register, so an indexed write lands on the
    // base slot every iteration. Warn once rather than silently writing the
    // wrong output register.
    if (dst.has_relative) {
      static std::atomic<bool> warned_dstrel{false};
      if (!warned_dstrel.exchange(true, std::memory_order_relaxed))
        llvm::errs() << "dxso: relative-addressed destination write is unimplemented; writes the base register\n";
    }
    Value *slot = nullptr;
    switch (dst.base.type) {
    case DxsoRegisterType::Temp:
      slot = builder.CreateGEP(tempArrTy, temps, {builder.getInt32(0), builder.getInt32(dst.base.num)});
      break;
    case DxsoRegisterType::RasterizerOut:
      if (is_vertex && dst.base.num == 0)
        slot = out_slot;
      else if (sm12_vs_varyings && dst.base.num == 1)
        slot = oFog_slot;
      else if (is_vertex && dst.base.num == 2 && oPts_slot)
        slot = oPts_slot;
      break;
    case DxsoRegisterType::ColorOut:
      if (!is_vertex && dst.base.num < 4)
        slot = oC_slot[dst.base.num];
      break;
    case DxsoRegisterType::DepthOut:
      // PS-only; D3D9 only writes the .x lane to oDepth, but the slot
      // is float4 so the rest of store_dst's plumbing is uniform.
      if (!is_vertex)
        slot = oDepth_slot;
      break;
    case DxsoRegisterType::AttributeOut:
      // Only SM ≤ 2 routes through the positional COLOR mapping.
      // SM3's `o#` (Output, alias for TexcoordOut numerically) has
      // its own dcl-driven path landing in a follow-up.
      if (sm12_vs_varyings && dst.base.num < 2)
        slot = oD_slot[dst.base.num];
      break;
    case DxsoRegisterType::TexcoordOut:
      // SM ≤ 2: positional oT# → TEXCOORD<n>. SM3: same numeric file
      // (Output=6) but dcl-driven, so route via oN_slot built from
      // the dcl walk above.
      if (sm12_vs_varyings && dst.base.num < 8)
        slot = oT_slot[dst.base.num];
      else if (sm3_vs_outputs && dst.base.num < 16)
        slot = oN_slot[dst.base.num];
      break;
    case DxsoRegisterType::Texture:
      // SM 1.x PS t# is both a sampler index and a register slot:
      // tex / texcoord / texreg2* write back into the same t<n> slot
      // that subsequent operands read through the Texture src arm
      // above. wined3d glsl_shader.c routes these through ffp_texcoord
      // implicitly. The Addr / Texture enum value (3) is shared with
      // VS a#, but VS writes go through Mova rather than store_dst.
      if (!is_vertex && tex_inputs && dst.base.num < 8)
        slot = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(dst.base.num)});
      break;
    default:
      break;
    }
    if (!slot)
      return;
    // Predicated execution: per-lane gate on p0. The predicate's
    // swizzle picks which p0 lane drives each result lane; modifier
    // Not inverts it (DXVK src/dxso/dxso_compiler.cpp
    // emitPredicateOp + emitPredicateLoad).
    Value *pmask = nullptr;
    if (ins.has_predicate && ins.predicate.base.type == DxsoRegisterType::Predicate && ins.predicate.base.num == 0) {
      pmask = builder.CreateLoad(bool4Ty, p0_slot);
      if (ins.predicate.swizzle.raw() != 0b11100100u) {
        int sw[4] = {
            (int)ins.predicate.swizzle[0], (int)ins.predicate.swizzle[1], (int)ins.predicate.swizzle[2],
            (int)ins.predicate.swizzle[3]
        };
        pmask = builder.CreateShuffleVector(pmask, pmask, ArrayRef<int>(sw, 4));
      }
      if (ins.predicate.modifier == DxsoRegModifier::Not)
        pmask = builder.CreateNot(pmask);
    }
    if (dst.mask.raw() == 0xF && !pmask) {
      builder.CreateStore(value, slot);
      return;
    }
    auto *cur = builder.CreateLoad(float4Ty, slot);
    Value *to_write = pmask ? builder.CreateSelect(pmask, value, cur) : value;
    Value *blended = cur;
    for (uint32_t i = 0; i < 4; ++i) {
      if (dst.mask[i]) {
        auto *e = builder.CreateExtractElement(to_write, builder.getInt32(i));
        blended = builder.CreateInsertElement(blended, e, builder.getInt32(i));
      }
    }
    builder.CreateStore(blended, slot);
  };

  // Two-source arithmetic helper: load both operands, run `op`, store
  // the result. nullptr from load_src (unsupported source shape) is
  // treated as skip; same model the per-opcode arms had inline.
  auto fold_binary = [&](const DxsoInstruction &ins, auto op) {
    if (!ins.has_dst || ins.src_count < 2)
      return;
    Value *a = load_src(ins.src[0]);
    Value *b = load_src(ins.src[1]);
    if (a && b)
      store_dst(ins.dst, op(a, b));
  };

  // One-source variant for Mov / Rcp / Rsq / Frc / Abs / Exp / Log.
  auto fold_unary = [&](const DxsoInstruction &ins, auto op) {
    if (!ins.has_dst || ins.src_count < 1)
      return;
    Value *a = load_src(ins.src[0]);
    if (a)
      store_dst(ins.dst, op(a));
  };

  // Three-source variant for Cmp / Lrp / Mad-style ternary lowerings.
  auto fold_ternary = [&](const DxsoInstruction &ins, auto op) {
    if (!ins.has_dst || ins.src_count < 3)
      return;
    Value *a = load_src(ins.src[0]);
    Value *b = load_src(ins.src[1]);
    Value *c = load_src(ins.src[2]);
    if (a && b && c)
      store_dst(ins.dst, op(a, b, c));
  };

  // float4 splat constant: uniqued by LLVM, no IR cost per use.
  auto v4splat = [&](double x) -> Constant * {
    return ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(Type::getFloatTy(context), x));
  };

  // Scalar dot product of the first n lanes of a and b (n=3 or 4).
  // For n=3 the inputs get shuffled down to a 3-vec first so lane 3;
  // which may be poison when the temp wasn't initialized; never
  // reaches the multiply. Mirrors DXVK src/dxso/dxso_compiler.cpp
  // emitDot. Used by Dp3 / Dp4 and by the M*x* matrix multiplies.
  auto compute_dot = [&](Value *a, Value *b, int n) -> Value * {
    if (n == 3) {
      int xyz[3] = {0, 1, 2};
      a = builder.CreateShuffleVector(a, a, ArrayRef<int>(xyz, 3));
      b = builder.CreateShuffleVector(b, b, ArrayRef<int>(xyz, 3));
    }
    Value *prod = builder.CreateFMul(a, b);
    Value *sum = builder.CreateExtractElement(prod, builder.getInt32(0));
    for (int i = 1; i < n; ++i)
      sum = builder.CreateFAdd(sum, builder.CreateExtractElement(prod, builder.getInt32(i)));
    return sum;
  };

  // Walk the body: the iterator drives the per-opcode lowering dispatch in the
  // switch below. Anything we don't recognize is silently skipped.
  // DWORD is `typedef uint32_t DWORD` in this codebase
  // (windows_base.h), so DxsoBytecodeIter's `const DWORD *`
  // accepts a `const uint32_t *` directly with no cast.
  DxsoBytecodeIter it(shader->bytecode.data(), static_cast<uint32_t>(shader->bytecode.size()), shader->header);
  // CFG stack for structured control flow. else_bb is allocated up
  // front as the false-edge target; if the source has no `else`, it
  // ends up as a single uncond br to merge_bb.
  struct IfBlock {
    BasicBlock *else_bb;
    BasicBlock *merge_bb;
    bool saw_else;
  };
  std::vector<IfBlock> cf_stack;
  // Rep / Loop frames. EndRep and EndLoop share the same close shape;
  // the only difference is that Loop also steps aL each iteration and
  // (for SM3 nesting) saves/restores aL across the inner scope.
  // Counts and stride are LLVM Values rather than constants so def-
  // baked (defi i#) and runtime-set (SetXShaderConstantI) i# both
  // flow through the same loop emitter.
  struct LoopFrame {
    Value *counter_slot;
    Value *aL_backup_slot; // null for Rep; no aL save/restore
    Value *total_count;    // i32, dynamic
    Value *aL_stride;      // i32, dynamic; ignored for Rep
    BasicBlock *header_bb;
    BasicBlock *latch_bb;
    BasicBlock *merge_bb;
  };
  std::vector<LoopFrame> loop_stack;
  while (it.next(ins)) {
    switch (ins.opcode) {
    case DxsoOpcode::End:
    case DxsoOpcode::Comment:
    case DxsoOpcode::Nop:
    case DxsoOpcode::Phase:
    case DxsoOpcode::Dcl:
    case DxsoOpcode::Def:
    case DxsoOpcode::DefI:
    case DxsoOpcode::DefB:
      break;
    case DxsoOpcode::Mov:
      // vs_1_x uses mov a0.x (no mova before SM2). Floor to int (wined3d),
      // store masked. SM2+ illegal mov-to-a0; FXC emits mova instead.
      if (is_vertex && shader->header.major == 1 && a0_slot && ins.has_dst && ins.src_count >= 1 &&
          ins.dst.base.type == DxsoRegisterType::Addr && ins.dst.base.num == 0) {
        if (Value *src = load_src(ins.src[0])) {
          Value *floored = air.CreateFPUnOp(llvm::air::AIRBuilder::floor, src);
          Value *as_int = builder.CreateFPToSI(floored, int4Ty);
          Value *cur = builder.CreateLoad(int4Ty, a0_slot);
          for (uint32_t i = 0; i < 4; ++i) {
            if (!ins.dst.mask[i])
              continue;
            Value *lane = builder.CreateExtractElement(as_int, builder.getInt32(i));
            cur = builder.CreateInsertElement(cur, lane, builder.getInt32(i));
          }
          builder.CreateStore(cur, a0_slot);
        }
        break;
      }
      fold_unary(ins, [&](Value *a) { return a; });
      break;
    case DxsoOpcode::Mova: {
      // mova a0.<mask>, src: round to nearest even, convert to int,
      // store into a0. SM1.x predates the `mova` opcode and a plain
      // `mov` to a0 floors instead of rounds (DXVK
      // src/dxso/dxso_compiler.cpp), but Mova is SM2+ only,
      // so always round. Anything other than the VS address register
      // as the destination is malformed bytecode.
      if (!a0_slot || !ins.has_dst || ins.src_count < 1 || ins.dst.base.type != DxsoRegisterType::Addr ||
          ins.dst.base.num != 0)
        break;
      Value *src = load_src(ins.src[0]);
      if (!src)
        break;
      Value *rounded = air.CreateFPUnOp(llvm::air::AIRBuilder::rint, src);
      Value *as_int = builder.CreateFPToSI(rounded, int4Ty);
      Value *cur = builder.CreateLoad(int4Ty, a0_slot);
      for (uint32_t i = 0; i < 4; ++i) {
        if (!ins.dst.mask[i])
          continue;
        Value *lane = builder.CreateExtractElement(as_int, builder.getInt32(i));
        cur = builder.CreateInsertElement(cur, lane, builder.getInt32(i));
      }
      builder.CreateStore(cur, a0_slot);
      break;
    }
    case DxsoOpcode::Abs:
      fold_unary(ins, [&](Value *a) { return air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, a); });
      break;
    case DxsoOpcode::Frc:
      // frc(x) = x - floor(x). Matches the D3D9 reference behavior on
      // negative inputs ([0, 1) result regardless of sign).
      fold_unary(ins, [&](Value *a) {
        return builder.CreateFSub(a, air.CreateFPUnOp(llvm::air::AIRBuilder::floor, a));
      });
      break;
    case DxsoOpcode::Rcp:
      // 1.0 / x, capped at FLT_MAX so 1/0 stays finite the way D3D9
      // HW behaves (DXVK src/dxso/dxso_compiler.cpp NMin under its
      // default-on d3d9FloatEmulation). AGX has a fast-recip primitive
      // InstCombine doesn't emit; if the AIR builder's air.recip ever
      // shows lower-error codegen, swap to it here.
      fold_unary(ins, [&](Value *a) {
        auto *one = builder.CreateVectorSplat(4, ConstantFP::get(Type::getFloatTy(context), 1.0));
        Value *r = builder.CreateFDiv(one, a);
        return air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, r, v4splat(std::numeric_limits<float>::max()), /*FastVariant=*/false);
      });
      break;
    case DxsoOpcode::Rsq:
      // 1.0 / sqrt(|x|). Negative inputs are clamped to abs to match
      // D3D9: DXVK src/dxso/dxso_compiler.cpp (inversesqrt(abs))
      // sqrt() on a negative would return NaN. rsqrt(0) caps at
      // FLT_MAX, same float-emulation contract as Rcp.
      fold_unary(ins, [&](Value *a) {
        Value *r = air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, a));
        return air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, r, v4splat(std::numeric_limits<float>::max()), /*FastVariant=*/false);
      });
      break;
    case DxsoOpcode::Exp:
      // D3D9 Exp / Log are base-2 (one of the FFP unfortunate names).
      // Overflow caps at FLT_MAX rather than +inf (DXVK
      // src/dxso/dxso_compiler.cpp NMin under d3d9FloatEmulation).
      fold_unary(ins, [&](Value *a) {
        Value *r = air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, a);
        return air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, r, v4splat(std::numeric_limits<float>::max()), /*FastVariant=*/false);
      });
      break;
    case DxsoOpcode::ExpP:
      // SM<2 ExpP: special four-lane result (not per-lane exp2).
      // .x=2^floor(s), .y=frac(s), .z=2^s, .w=1. Read .y for bone-index.
      // Both shapes share the Exp FLT_MAX overflow cap.
      if (shader->header.major < 2) {
        if (Value *src = load_src(ins.src[0])) {
          auto *fTy = Type::getFloatTy(context);
          Value *s = builder.CreateExtractElement(src, builder.getInt32(0));
          Value *fl = air.CreateFPUnOp(llvm::air::AIRBuilder::floor, s);
          Value *r = PoisonValue::get(float4Ty);
          r = builder.CreateInsertElement(r, air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, fl), builder.getInt32(0));
          r = builder.CreateInsertElement(r, builder.CreateFSub(s, fl), builder.getInt32(1));
          r = builder.CreateInsertElement(r, air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, s), builder.getInt32(2));
          r = builder.CreateInsertElement(r, ConstantFP::get(fTy, 1.0), builder.getInt32(3));
          r = air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, r, v4splat(std::numeric_limits<float>::max()), /*FastVariant=*/false);
          store_dst(ins.dst, r);
        }
      } else {
        fold_unary(ins, [&](Value *a) {
          Value *r = air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, a);
          return air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, r, v4splat(std::numeric_limits<float>::max()), /*FastVariant=*/false);
        });
      }
      break;
    case DxsoOpcode::Log:
    case DxsoOpcode::LogP:
      // D3D9 log is log2(|x|). LogP is the partial-precision alias;
      // DXVK src/dxso/dxso_compiler.cpp collapses both to the
      // same opcode. log2(0) floors at -FLT_MAX rather than -inf
      // (DXVK NMax under d3d9FloatEmulation).
      fold_unary(ins, [&](Value *a) {
        Value *r = air.CreateFPUnOp(llvm::air::AIRBuilder::log2, air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, a));
        return air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, r, v4splat(-std::numeric_limits<float>::max()), /*FastVariant=*/false);
      });
      break;
    case DxsoOpcode::Pow:
      // D3D9 pow(a, b) = 2^(b * log2(|a|)). Base is implicitly abs'd;
      // air.fast_pow on a negative base with non-integer exponent
      // returns NaN, which diverges. pow(x, 0) must be exactly 1.0
      // even for x = 0 / inf / NaN; fast_pow gives NaN there, so a
      // zero-exponent select picks 1.0 lane-wise (DXVK
      // src/dxso/dxso_compiler.cpp does the same under its default-on
      // d3d9FloatEmulation). air.fast_pow rather than llvm.pow because
      // AGX's pipeline compiler rejects llvm.* intrinsics in metallib
      // bodies as unrecognized opcodes
      // (XPC_ERROR_CONNECTION_INTERRUPTED at link, no diagnostic at
      // metallib-write).
      fold_binary(ins, [&](Value *a, Value *b) {
        Value *r = air.CreateFPBinOp(llvm::air::AIRBuilder::pow, air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, a), b);
        Value *exp_is_zero = builder.CreateFCmpOEQ(b, v4splat(0.0));
        return builder.CreateSelect(exp_is_zero, v4splat(1.0), r);
      });
      break;
    case DxsoOpcode::DsX:
    case DxsoOpcode::DsY: {
      // SM3+ explicit gradient: dst = ddx(src) or ddy(src). Quad
      // derivatives are PS-only. Mirrors DXVK
      // src/dxso/dxso_compiler.cpp (opDpdx / opDpdy).
      if (is_vertex)
        break;
      Value *a = load_src(ins.src[0]);
      if (!a)
        break;
      store_dst(ins.dst, air.CreateDerivative(a, ins.opcode == DxsoOpcode::DsY));
      break;
    }
    case DxsoOpcode::Sgn: {
      // dst = sign(src): +1 if src > 0, -1 if src < 0, 0 if src == 0.
      // DXVK src/dxso/dxso_compiler.cpp emits OpFSign; same
      // tri-state result. Metal AIR has no direct sign intrinsic,
      // so unfold to a fcmp/select pair on the <4 x float>. The
      // sgn's SM2 src[1]/src[2] scratch operands are ignored here, the same
      // as DXVK (its Sgn arm is opFSign on src0 only); their post-op contents
      // are undefined per the D3D9 spec.
      if (!ins.has_dst || ins.src_count < 1)
        break;
      Value *a = load_src(ins.src[0]);
      if (!a)
        break;
      auto *zero4 = ConstantAggregateZero::get(float4Ty);
      auto *one4 = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(Type::getFloatTy(context), 1.0));
      auto *neg1_4 =
          ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(Type::getFloatTy(context), -1.0));
      Value *gt = builder.CreateFCmpOGT(a, zero4);
      Value *lt = builder.CreateFCmpOLT(a, zero4);
      Value *result = builder.CreateSelect(gt, one4, builder.CreateSelect(lt, neg1_4, zero4));
      store_dst(ins.dst, result);
      break;
    }
    case DxsoOpcode::Dst: {
      // FFP distance vector helper.
      // dst.x = 1
      // dst.y = src0.y * src1.y
      // dst.z = src0.z
      // dst.w = src1.w
      // Mirrors DXVK src/dxso/dxso_compiler.cpp. Used by FFP
      // attenuation lighting; rare in modern shaders but FXC still
      // emits it under fixed-function expansion.
      if (ins.src_count < 2)
        break;
      Value *a = load_src(ins.src[0]);
      Value *b = load_src(ins.src[1]);
      if (!a || !b)
        break;
      auto *fTy = Type::getFloatTy(context);
      auto *one_f = ConstantFP::get(fTy, 1.0);
      Value *ay = builder.CreateExtractElement(a, builder.getInt32(1));
      Value *by = builder.CreateExtractElement(b, builder.getInt32(1));
      Value *az = builder.CreateExtractElement(a, builder.getInt32(2));
      Value *bw = builder.CreateExtractElement(b, builder.getInt32(3));
      Value *result = ConstantAggregateZero::get(float4Ty);
      result = builder.CreateInsertElement(result, one_f, builder.getInt32(0));
      result = builder.CreateInsertElement(result, builder.CreateFMul(ay, by), builder.getInt32(1));
      result = builder.CreateInsertElement(result, az, builder.getInt32(2));
      result = builder.CreateInsertElement(result, bw, builder.getInt32(3));
      store_dst(ins.dst, result);
      break;
    }
    case DxsoOpcode::Lit: {
      // FFP lighting: dst=(1, max(src.x,0), (src.x>0 && src.y>0) ?
      // pow(max(src.y,0), clamp(src.w,-127.9961,127.9961)) : 0, 1). The x/y
      // gates are strict: at a zero operand real hardware returns 0, not the
      // pow(0,0)=1 a >= gate would select (matches the FFP specular gate).
      if (ins.src_count < 1)
        break;
      Value *a = load_src(ins.src[0]);
      if (!a)
        break;
      auto *fTy = Type::getFloatTy(context);
      auto *zero_f = ConstantFP::get(fTy, 0.0);
      auto *one_f = ConstantFP::get(fTy, 1.0);
      Value *sx = builder.CreateExtractElement(a, builder.getInt32(0));
      Value *sy = builder.CreateExtractElement(a, builder.getInt32(1));
      Value *sw = builder.CreateExtractElement(a, builder.getInt32(3));
      auto *pmax_f = ConstantFP::get(fTy, 127.9961f);
      auto *pmin_f = ConstantFP::get(fTy, -127.9961f);
      Value *p = air.CreateFPBinOp(
          llvm::air::AIRBuilder::fmin, air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, sw, pmin_f), pmax_f
      );
      Value *y_lane = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, sx, zero_f);
      Value *base = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, sy, zero_f);
      Value *z_pow = air.CreateFPBinOp(llvm::air::AIRBuilder::pow, base, p);
      Value *xgt = builder.CreateFCmpOGT(sx, zero_f);
      Value *ygt = builder.CreateFCmpOGT(sy, zero_f);
      Value *cond = builder.CreateAnd(xgt, ygt);
      Value *z_lane = builder.CreateSelect(cond, z_pow, zero_f);
      Value *result = ConstantAggregateZero::get(float4Ty);
      result = builder.CreateInsertElement(result, one_f, builder.getInt32(0));
      result = builder.CreateInsertElement(result, y_lane, builder.getInt32(1));
      result = builder.CreateInsertElement(result, z_lane, builder.getInt32(2));
      result = builder.CreateInsertElement(result, one_f, builder.getInt32(3));
      store_dst(ins.dst, result);
      break;
    }
    case DxsoOpcode::Crs: {
      // 3D cross: dst.x = a.y*b.z - a.z*b.y, .y = a.z*b.x - a.x*b.z,
      // .z = a.x*b.y - a.y*b.x. dst.w is don't-care (mask blends it
      // away). Mirrors DXVK src/dxso/dxso_compiler.cpp: same
      // (a.yzx * b.zxy - a.zxy * b.yzx) shuffle shape.
      if (!ins.has_dst || ins.src_count < 2)
        break;
      Value *a = load_src(ins.src[0]);
      Value *b = load_src(ins.src[1]);
      if (!a || !b)
        break;
      int yzxw[4] = {1, 2, 0, 3};
      int zxyw[4] = {2, 0, 1, 3};
      Value *a_yzx = builder.CreateShuffleVector(a, a, ArrayRef<int>(yzxw, 4));
      Value *b_zxy = builder.CreateShuffleVector(b, b, ArrayRef<int>(zxyw, 4));
      Value *a_zxy = builder.CreateShuffleVector(a, a, ArrayRef<int>(zxyw, 4));
      Value *b_yzx = builder.CreateShuffleVector(b, b, ArrayRef<int>(yzxw, 4));
      Value *result = builder.CreateFSub(builder.CreateFMul(a_yzx, b_zxy), builder.CreateFMul(a_zxy, b_yzx));
      store_dst(ins.dst, result);
      break;
    }
    case DxsoOpcode::SinCos: {
      // dst.x = cos(src.x), dst.y = sin(src.x). DXVK
      // src/dxso/dxso_compiler.cpp; same shape; the SM2-only
      // src[1]/src[2] sincos approximation tables are ignored both
      // there and here (modern HW computes sin/cos directly). The
      // dst mask is applied by store_dst, so the typical .xy /
      // .x / .y mask trims the broadcast vector to written lanes.
      if (!ins.has_dst || ins.src_count < 1)
        break;
      Value *a = load_src(ins.src[0]);
      if (!a)
        break;
      Value *ax = builder.CreateExtractElement(a, builder.getInt32(0));
      Value *cosx = air.CreateFPUnOp(llvm::air::AIRBuilder::cos, ax);
      Value *sinx = air.CreateFPUnOp(llvm::air::AIRBuilder::sin, ax);
      Value *result = ConstantAggregateZero::get(float4Ty);
      result = builder.CreateInsertElement(result, cosx, builder.getInt32(0));
      result = builder.CreateInsertElement(result, sinx, builder.getInt32(1));
      store_dst(ins.dst, result);
      break;
    }
    case DxsoOpcode::Dp2Add: {
      // Scalar: dot2(src[0].xy, src[1].xy) + src[2].x. Broadcast to
      // the dst's masked lanes. DXVK src/dxso/dxso_compiler.cpp;
      // same shape: emitDot on .xy + FAdd of src[2].x.
      if (!ins.has_dst || ins.src_count < 3)
        break;
      Value *a = load_src(ins.src[0]);
      Value *b = load_src(ins.src[1]);
      Value *c = load_src(ins.src[2]);
      if (!a || !b || !c)
        break;
      Value *dot2 = compute_dot(a, b, 2);
      Value *cx = builder.CreateExtractElement(c, builder.getInt32(0));
      Value *result = builder.CreateFAdd(dot2, cx);
      store_dst(ins.dst, builder.CreateVectorSplat(4, result));
      break;
    }
    case DxsoOpcode::Nrm: {
      // 3D normalize: r = a / sqrt(a.x*a.x + a.y*a.y + a.z*a.z),
      // broadcast to all dst lanes (dst mask trims). DXVK
      // src/dxso/dxso_compiler.cpp; same shape: rsqrt(dot3),
      // multiply src * splat. rsqrt(0) caps at FLT_MAX so a zero
      // vector normalizes to 0 rather than NaN (DXVK NMin under its
      // default-on d3d9FloatEmulation).
      if (!ins.has_dst || ins.src_count < 1)
        break;
      Value *a = load_src(ins.src[0]);
      if (!a)
        break;
      Value *dot3 = compute_dot(a, a, 3);
      Value *rcp_len = air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, dot3);
      rcp_len = air.CreateFPBinOp(
          llvm::air::AIRBuilder::fmin, rcp_len,
          ConstantFP::get(Type::getFloatTy(context), std::numeric_limits<float>::max()),
          /*FastVariant=*/false
      );
      Value *splat = builder.CreateVectorSplat(4, rcp_len);
      store_dst(ins.dst, builder.CreateFMul(a, splat));
      break;
    }
    case DxsoOpcode::Add:
      fold_binary(ins, [&](Value *a, Value *b) { return builder.CreateFAdd(a, b); });
      break;
    case DxsoOpcode::Sub:
      fold_binary(ins, [&](Value *a, Value *b) { return builder.CreateFSub(a, b); });
      break;
    case DxsoOpcode::Mul:
      fold_binary(ins, [&](Value *a, Value *b) { return builder.CreateFMul(a, b); });
      break;
    case DxsoOpcode::Min:
      fold_binary(ins, [&](Value *a, Value *b) { return air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, a, b); });
      break;
    case DxsoOpcode::Max:
      fold_binary(ins, [&](Value *a, Value *b) { return air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, a, b); });
      break;
    case DxsoOpcode::Slt:
      // (a < b) ? 1.0 : 0.0, lane-wise. Ordered compare; NaN inputs
      // pick the 0.0 side, matching DXVK's opFOrdLessThan.
      fold_binary(ins, [&](Value *a, Value *b) {
        return builder.CreateSelect(builder.CreateFCmpOLT(a, b), v4splat(1.0), v4splat(0.0));
      });
      break;
    case DxsoOpcode::Sge:
      // (a >= b) ? 1.0 : 0.0. Ordered compare; same NaN behavior as
      // Slt. DXVK uses opFOrdGreaterThanEqual.
      fold_binary(ins, [&](Value *a, Value *b) {
        return builder.CreateSelect(builder.CreateFCmpOGE(a, b), v4splat(1.0), v4splat(0.0));
      });
      break;
    case DxsoOpcode::Cmp:
      // (src0 >= 0) ? src1 : src2, lane-wise. NaN goes to the false
      // (src2) branch via the ordered compare. FXC only emits Cmp in
      // PS bytecode; the arm is harmless if a hand-authored VS
      // shader encodes it. DXVK src/dxso/dxso_compiler.cpp.
      fold_ternary(ins, [&](Value *a, Value *b, Value *c) {
        return builder.CreateSelect(builder.CreateFCmpOGE(a, v4splat(0.0)), b, c);
      });
      break;
    case DxsoOpcode::Cnd: {
      // SM1.x conditional: (src0 > 0.5) ? src1 : src2.
      // wined3d glsl_shader.c shader_glsl_cnd: for a co-issued cnd in
      // ps_1_0..1_3 whose write mask is not alpha-only, the instruction
      // degenerates to an unconditional move of src1 (a vendor co-issue quirk
      // the wine tests pin; DXVK lacks it). Otherwise the plain select.
      const bool pre_ps14 = shader->header.major == 1 && shader->header.minor < 4;
      if (ins.coissue && pre_ps14 && ins.dst.mask.raw() != 0x8u) {
        fold_ternary(ins, [&](Value *, Value *b, Value *) { return b; });
      } else if (pre_ps14) {
        // Before ps_1_4 the comparison is scalar: one lane drives every
        // written component. wined3d reads src0 through WRITEMASK_0 for
        // exactly this version range and only ps_1_4 onwards compares per
        // component. DXVK compares lane-wise at every model.
        fold_ternary(ins, [&](Value *a, Value *b, Value *c) {
          Value *lane = builder.CreateExtractElement(a, builder.getInt32(0));
          Value *gt = builder.CreateFCmpOGT(lane, ConstantFP::get(Type::getFloatTy(context), 0.5));
          return builder.CreateSelect(gt, b, c);
        });
      } else {
        fold_ternary(ins, [&](Value *a, Value *b, Value *c) {
          return builder.CreateSelect(builder.CreateFCmpOGT(a, v4splat(0.5)), b, c);
        });
      }
      break;
    }
    case DxsoOpcode::Lrp:
      // src0 * (src1 - src2) + src2; the standard mix/lerp. DXVK
      // src/dxso/dxso_compiler.cpp emitMix derives the same
      // arithmetic via mad(src0, src1 - src2, src2).
      fold_ternary(ins, [&](Value *s0, Value *s1, Value *s2) {
        return builder.CreateFAdd(builder.CreateFMul(s0, builder.CreateFSub(s1, s2)), s2);
      });
      break;
    case DxsoOpcode::Mad:
      if (ins.has_dst && ins.src_count >= 3) {
        Value *a = load_src(ins.src[0]);
        Value *b = load_src(ins.src[1]);
        Value *c = load_src(ins.src[2]);
        if (a && b && c)
          store_dst(ins.dst, builder.CreateFAdd(builder.CreateFMul(a, b), c));
      }
      break;
    case DxsoOpcode::Dp3:
    case DxsoOpcode::Dp4:
      // dp{3,4} broadcasts the dot to all four dst lanes; store_dst's
      // mask blend then trims to the writemask the shader requested.
      if (ins.has_dst && ins.src_count >= 2) {
        Value *a = load_src(ins.src[0]);
        Value *b = load_src(ins.src[1]);
        if (a && b) {
          int n = (ins.opcode == DxsoOpcode::Dp3) ? 3 : 4;
          store_dst(ins.dst, builder.CreateVectorSplat(4, compute_dot(a, b, n)));
        }
      }
      break;
    case DxsoOpcode::M4x4:
    case DxsoOpcode::M4x3:
    case DxsoOpcode::M3x4:
    case DxsoOpcode::M3x3:
    case DxsoOpcode::M3x2: {
      // M<N>x<M> dst, vec, mat: N-element dot of `vec` against M
      // consecutive matrix rows starting at src1.base.num. dst.lane[i]
      // = dot(vec, mat[i]). Mirrors DXVK src/dxso/dxso_compiler.cpp
      // emitMatrixAlu; that walks src1.id.num the same
      // way to pick up the next row.
      if (!ins.has_dst || ins.src_count < 2)
        break;
      int dotCount = 0;
      int compCount = 0;
      switch (ins.opcode) {
      case DxsoOpcode::M4x4:
        dotCount = 4;
        compCount = 4;
        break;
      case DxsoOpcode::M4x3:
        dotCount = 4;
        compCount = 3;
        break;
      case DxsoOpcode::M3x4:
        dotCount = 3;
        compCount = 4;
        break;
      case DxsoOpcode::M3x3:
        dotCount = 3;
        compCount = 3;
        break;
      case DxsoOpcode::M3x2:
        dotCount = 3;
        compCount = 2;
        break;
      default:
        break;
      }
      // Trim the dst mask to the first compCount set lanes; DXVK
      // src/dxso/dxso_compiler.cpp. m4x3 r0.xyzw still writes
      // only three rows; lane 3 of the dst is preserved instead of
      // being stomped to zero. The i-th dot lands at the i-th set
      // mask lane so a mask like .yzw routes dot0→y, dot1→z, dot2→w.
      int target_lane[4] = {-1, -1, -1, -1};
      uint8_t trimmed = 0;
      int kept = 0;
      for (int i = 0; i < 4 && kept < compCount; ++i) {
        if (ins.dst.mask[i]) {
          target_lane[kept++] = i;
          trimmed |= static_cast<uint8_t>(1u << i);
        }
      }
      if (kept == 0)
        break;
      Value *v = load_src(ins.src[0]);
      if (!v)
        break;
      Value *result = ConstantAggregateZero::get(float4Ty);
      bool ok = true;
      for (int i = 0; i < kept; ++i) {
        DxsoSrcRegister row = ins.src[1];
        row.base.num = static_cast<uint16_t>(row.base.num + i);
        Value *r = load_src(row);
        if (!r) {
          ok = false;
          break;
        }
        Value *d = compute_dot(v, r, dotCount);
        result = builder.CreateInsertElement(result, d, builder.getInt32(target_lane[i]));
      }
      if (!ok)
        break;
      DxsoDstRegister tdst = ins.dst;
      tdst.mask = DxsoRegMask(trimmed);
      store_dst(tdst, result);
      break;
    }
    case DxsoOpcode::Tex:
    case DxsoOpcode::TexLdl:
    case DxsoOpcode::TexLdd: {
      // Tex opcode variants: SM1.0-1.3 implicit t<n>,
      // SM1.4 texld src, SM2+ texld/texldl/texldd with sampler src[1].
      // VTF: a VS may sample via texldl/texldd (SM3.0 vertex texture fetch);
      // the SM<2 implicit-tex form is pixel-only, so still gate that branch.
      if (!ins.has_dst)
        break;
      bool is_grad = ins.opcode == DxsoOpcode::TexLdd;
      uint32_t slot;
      Value *coord4;
      bool sampler_operand = false;
      if (ins.opcode == DxsoOpcode::Tex && shader->header.major < 2) {
        slot = ins.dst.base.num;
        if (slot >= 16 || tex_arg_idx[slot] < 0)
          break;
        if (shader->header.minor == 4) {
          if (ins.src_count < 1)
            break;
          coord4 = load_src(ins.src[0]);
        } else {
          if (!tex_inputs || slot >= 8)
            break;
          auto *gep = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(slot)});
          coord4 = builder.CreateLoad(float4Ty, gep);
        }
      } else {
        uint32_t need_srcs = is_grad ? 4u : 2u;
        if (ins.src_count < need_srcs)
          break;
        if (ins.src[1].base.type != DxsoRegisterType::Sampler)
          break;
        slot = ins.src[1].base.num;
        if (slot >= 16 || tex_arg_idx[slot] < 0)
          break;
        coord4 = load_src(ins.src[0]);
        sampler_operand = true;
      }
      if (!coord4)
        break;
      // SM1.0-1.3 implicit projected texturing (D3DTTFF_PROJECTED): the
      // pre-1.4 pixel shader divides the texcoord by its w at a PROJECTED
      // stage (see apply_projected). DXVK gates the same divide on a
      // per-sampler projected spec constant. Cube samples are excluded (a
      // direction vector is not projected); SM1.4 and SM2+ do not take this
      // path, and TexBem/TexBemL project in their own arm. Projecting coord4
      // here, before the shuffle, also feeds the depth-compare reference
      // extracted below.
      if (!is_vertex && ins.opcode == DxsoOpcode::Tex && shader->header.major < 2 && shader->header.minor < 4 &&
          samp_kind[slot] != DxsoTextureType::TextureCube)
        coord4 = apply_projected(coord4, slot);
      // Coord shape follows the dcl'd texture type: 2D reads xy, Cube
      // reads xyz (Metal's cube sampler takes a direction vector, not
      // a face/uv pair), 3D reads xyz. TexLdl reads lane 3 as the LOD
      // regardless of the texture type; DXVK
      // src/dxso/dxso_compiler.cpp does the same composite-extract
      // on w independent of dimensions.
      int xy[2] = {0, 1};
      int xyz[3] = {0, 1, 2};
      Value *coord;
      llvm::air::Texture::ResourceKind air_kind;
      if (samp_kind[slot] == DxsoTextureType::Texture2D || samp_kind[slot] == DxsoTextureType::Texture2DDepth) {
        coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xy, 2));
        air_kind = samp_kind[slot] == DxsoTextureType::Texture2DDepth ? llvm::air::Texture::depth_2d
                                                                      : llvm::air::Texture::texture_2d;
      } else if (samp_kind[slot] == DxsoTextureType::TextureCube) {
        coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xyz, 3));
        air_kind = llvm::air::Texture::texturecube;
      } else {
        // Texture3D; the only remaining kind the bind loop emits
        // args for.
        coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xyz, 3));
        air_kind = llvm::air::Texture::texture3d;
      }
      Value *tex_handle = fn->getArg(tex_arg_idx[slot]);
      Value *samp_handle = fn->getArg(samp_arg_idx[slot]);
      const int32_t no_offset[3] = {0, 0, 0};
      llvm::air::Texture tex_desc{
          .kind = air_kind,
          .sample_type = llvm::air::Texture::sample_float,
          .memory_access = llvm::air::Texture::access_sample,
      };
      // SM2+ texld carries an opcode-specific mode in the token's
      // bits 16..23: Regular (0), Project (1), Bias (2). DXVK
      // src/dxso/dxso_compiler.cpp. Project divides the
      // coord by w before sampling; Bias passes w as a LOD bias.
      // TexLdl/TexLdd ignore the mode; they always carry an explicit
      // LOD or explicit gradients, so Project/Bias don't apply.
      auto mode = static_cast<DxsoTexLdMode>(ins.specific_data);
      bool is_proj = ins.opcode == DxsoOpcode::Tex && mode == DxsoTexLdMode::Project;
      bool is_bias = ins.opcode == DxsoOpcode::Tex && mode == DxsoTexLdMode::Bias;
      Value *samp_lod_bias = load_samp_bias(slot);
      if (is_proj) {
        Value *w = builder.CreateExtractElement(coord4, builder.getInt32(3));
        Value *w_splat = builder.CreateVectorSplat(cast<FixedVectorType>(coord->getType())->getNumElements(), w);
        coord = builder.CreateFDiv(coord, w_splat);
      }
      Value *texel = nullptr;
      if (samp_fetch4_broken[slot] && !is_proj) {
        // FETCH4 armed on a format outside the single-channel set:
        // the plain sample forms read zero on the vendor hardware and
        // only the projected form (handled by falling through) takes
        // the normal sample.
        texel = Constant::getNullValue(float4Ty);
      } else if (
          samp_fetch4[slot] && !samp_compare[slot] &&
          (ins.opcode == DxsoOpcode::Tex || ins.opcode == DxsoOpcode::TexLdl || ins.opcode == DxsoOpcode::TexLdd)
      ) {
        // AMD FETCH4: gather the red channel of the four neighbours in
        // the funny D3D9 order (B, R, G, A). Nudge the coordinate by
        // half a texel so the gather footprint matches the point-sample
        // texel the app addressed (DXVK dxso_compiler.cpp does the
        // same). The explicit-LOD and gradient forms gather too, and
        // like DXVK's the gather ignores the lod and gradient inputs:
        // the hack is level-0 point sampling by construction.
        Value *fw = air.CreateTextureQuery(tex_desc, tex_handle, llvm::air::Texture::Query::width, builder.getInt32(0));
        Value *fh =
            air.CreateTextureQuery(tex_desc, tex_handle, llvm::air::Texture::Query::height, builder.getInt32(0));
        // Just under half a texel: an exact half lands sample points on
        // texel corners and the gather flips to the wrong quad (DXVK
        // biases by 1 - 1/256 for the same grid effect).
        Value *inv_w = builder.CreateFDiv(
            ConstantFP::get(Type::getFloatTy(context), 0.498046875f),
            builder.CreateUIToFP(fw, Type::getFloatTy(context))
        );
        Value *inv_h = builder.CreateFDiv(
            ConstantFP::get(Type::getFloatTy(context), 0.498046875f),
            builder.CreateUIToFP(fh, Type::getFloatTy(context))
        );
        Value *nudge = UndefValue::get(coord->getType());
        nudge = builder.CreateInsertElement(nudge, inv_w, builder.getInt32(0));
        nudge = builder.CreateInsertElement(nudge, inv_h, builder.getInt32(1));
        Value *g_coord = builder.CreateFAdd(coord, nudge);
        const int32_t g_off[3] = {0, 0, 0};
        auto [g, g_res] = air.CreateGather(
            tex_desc, tex_handle, samp_handle, g_coord, /*ArrayIndex=*/nullptr, g_off, builder.getInt32(0)
        );
        (void)g_res;
        int swz[4] = {2, 0, 1, 3};
        texel = builder.CreateShuffleVector(g, g, ArrayRef<int>(swz, 4));
      } else if (samp_compare[slot]) {
        // Hardware PCF: sample_compare(coord.xy, ref) returns the filtered
        // 0/1 comparison of ref vs the stored depth. The reference is the
        // projected depth: coord.z, divided by w for a projective texldp
        // (the is_proj block above already divided coord.xy by w; the ref
        // needs the same divide). D3D9 HW shadow maps sample at LOD 0, so
        // use the sample_level{0} overload (matches the DXIL
        // SampleCmpLevelZero path). The scalar result is splatted to
        // xyzw below, same as the raw-depth path.
        Value *ref = builder.CreateExtractElement(coord4, builder.getInt32(2));
        if (is_proj) {
          Value *w = builder.CreateExtractElement(coord4, builder.getInt32(3));
          ref = builder.CreateFDiv(ref, w);
        }
        auto [t, residency] = air.CreateSampleCmp(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, ref, no_offset, llvm::air::sample_level{air.getFloat(0)}
        );
        (void)residency;
        texel = t;
      } else if (is_grad) {
        // ddx in src[2], ddy in src[3]; shuffle each to the coord
        // shape (DXVK src/dxso/dxso_compiler.cpp derives the
        // gradient mask from sampler.dimensions).
        Value *ddx4 = load_src(ins.src[2]);
        Value *ddy4 = load_src(ins.src[3]);
        if (!ddx4 || !ddy4)
          break;
        Value *ddx;
        Value *ddy;
        if (samp_kind[slot] == DxsoTextureType::Texture2D) {
          ddx = builder.CreateShuffleVector(ddx4, ddx4, ArrayRef<int>(xy, 2));
          ddy = builder.CreateShuffleVector(ddy4, ddy4, ArrayRef<int>(xy, 2));
        } else {
          ddx = builder.CreateShuffleVector(ddx4, ddx4, ArrayRef<int>(xyz, 3));
          ddy = builder.CreateShuffleVector(ddy4, ddy4, ArrayRef<int>(xyz, 3));
        }
        if (samp_lod_bias) {
          // The sampler bias shifts the gradient-derived LOD; scaling
          // both gradient vectors by 2^bias is the same shift.
          Value *scale = air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, samp_lod_bias);
          Value *splat =
              builder.CreateVectorSplat(cast<FixedVectorType>(ddx->getType())->getNumElements(), scale);
          ddx = builder.CreateFMul(ddx, splat);
          ddy = builder.CreateFMul(ddy, splat);
        }
        auto [t, residency] = air.CreateSampleGrad(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, ddx, ddy, no_offset
        );
        (void)residency;
        texel = t;
      } else if (ins.opcode == DxsoOpcode::TexLdl) {
        Value *lod = builder.CreateExtractElement(coord4, builder.getInt32(3));
        if (samp_lod_bias)
          lod = builder.CreateFAdd(lod, samp_lod_bias);
        auto [t, residency] = air.CreateSample(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_level{lod}
        );
        (void)residency;
        texel = t;
      } else if (is_bias) {
        Value *bias = builder.CreateExtractElement(coord4, builder.getInt32(3));
        if (samp_lod_bias)
          bias = builder.CreateFAdd(bias, samp_lod_bias);
        auto [t, residency] = air.CreateSample(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{bias}
        );
        (void)residency;
        texel = t;
      } else if (samp_lod_bias) {
        auto [t, residency] = air.CreateSample(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{samp_lod_bias}
        );
        (void)residency;
        texel = t;
      } else {
        auto [t, residency] = air.CreateSample(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, no_offset
        );
        (void)residency; // D3D9 has no residency feedback; discard.
        texel = t;
      }
      // FETCH4 on a block-compressed format: replicate the sampled red
      // to all four lanes instead of gathering across the block.
      if (samp_fetch4_rep[slot] && texel && texel->getType() == float4Ty) {
        Value *r = builder.CreateExtractElement(texel, builder.getInt32(0));
        texel = builder.CreateVectorSplat(4, r);
      }
      // Two-channel signed formats: rescale Metal's snorm result to the D3D9
      // convention (divide by 2^(n-1), see the kind's contract) and force the
      // missing z and w channels to one.
      if (samp_snorm2[slot] != 0 && texel->getType() == float4Ty) {
        double s = samp_snorm2[slot] == 8 ? 127.0 / 128.0 : 32767.0 / 32768.0;
        Value *scaled =
            builder.CreateFMul(texel, builder.CreateVectorSplat(4, ConstantFP::get(Type::getFloatTy(context), s)));
        Value *one = ConstantFP::get(Type::getFloatTy(context), 1.0);
        scaled = builder.CreateInsertElement(scaled, one, builder.getInt32(2));
        texel = builder.CreateInsertElement(scaled, one, builder.getInt32(3));
      }
      // depth_2d sample (and sample_compare) returns a scalar float;
      // the rest of the DXSO pipeline operates on float4. Splat to
      // all four lanes per the NVIDIA/ATI INTZ / hardware-PCF
      // contract: `texld r#, t#, s#` against a depth texture
      // returns the shadow factor / depth value replicated in
      // r#.xyzw, which is what shadow-modulating shaders rely on
      // when they read r#.x as the shadow factor. Without this,
      // sampling Depth32Float_Stencil8 as MSL texture2d<float>
      // leaves r#.yzw undefined and the visible result is per-pixel
      // brightness variation on every shaded surface; most visible
      // on alpha-blended geometry like smoke / particles.
      if (samp_kind[slot] == DxsoTextureType::Texture2DDepth && texel && !texel->getType()->isVectorTy()) {
        // The FETCH4 gather is already a four-lane value; only the
        // scalar depth sample expands. INTZ replicates to all lanes,
        // the DF formats read (d, 0, 0, 1); DXVK expresses the same
        // split as its RRRR vs R001 view swizzles.
        if (samp_depth_r001[slot]) {
          Value *v = Constant::getNullValue(float4Ty);
          v = builder.CreateInsertElement(v, texel, builder.getInt32(0));
          texel = builder.CreateInsertElement(v, ConstantFP::get(Type::getFloatTy(context), 1.0), builder.getInt32(3));
        } else {
          texel = builder.CreateVectorSplat(4, texel);
        }
      }
      // SM2+ texld applies the sampler operand's swizzle to the fetched
      // texel (DXVK src/dxso/dxso_compiler.cpp swizzles the sample
      // result by ctx.src[1].swizzle; SM<2 uses identity). load_src
      // never sees the sampler operand, so the swizzle lands here,
      // after the depth splat so depth reads swizzle uniformly.
      if (sampler_operand && texel && ins.src[1].swizzle.raw() != 0b11100100u) {
        int sw[4] = {
            (int)ins.src[1].swizzle[0], (int)ins.src[1].swizzle[1], (int)ins.src[1].swizzle[2],
            (int)ins.src[1].swizzle[3]
        };
        texel = builder.CreateShuffleVector(texel, texel, ArrayRef<int>(sw, 4));
      }
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::TexKill: {
      // texkill <reg>: discard the fragment if any tested lane of the
      // source is < 0. The source and the tested lanes split on
      // different model boundaries (DXVK src/dxso/dxso_compiler.cpp,
      // wined3d shader_glsl_texkill):
      //  - source: SM 2.0+ and SM 1.4 read the dst slot; SM 1.0-1.3
      //    read the interpolated TEXCOORD<dst.num> through the
      //    PixelTexcoord register file.
      //  - lanes: every SM 1.x model tests xyz, because the phase
      //    kills w; the dst writemask only starts selecting at SM 2.0.
      // Early-Z opt-out: AIR's metallib linker detects
      // discard_fragment and disables early fragment tests on its
      // own, and the DXSO path never opts in (only DXBC does, via
      // dxbc_converter_cfg.cpp), so kill-bearing PS already runs
      // late. No explicit ExecutionModeEarlyFragment-style negation
      // needed here.
      if (is_vertex)
        break;
      bool sm14_or_later = shader->header.major >= 2 || (shader->header.major == 1 && shader->header.minor == 4);
      Value *coord4 = nullptr;
      bool test_lane[4] = {false, false, false, false};
      if (sm14_or_later) {
        // Mirror the dst slot as a src; identity swizzle, no
        // modifier. has_relative / relative are propagated verbatim
        // per DXVK src/dxso/dxso_compiler.cpp; SM3 dst-relative
        // texkill works end-to-end once load_src grows relative-
        // addressing for non-Const operand types.
        DxsoSrcRegister src{};
        src.base = ins.dst.base;
        src.has_relative = ins.dst.has_relative;
        src.relative = ins.dst.relative;
        coord4 = load_src(src);
        if (!coord4)
          break;
        // SM 1.4 reads the destination register but still tests only xyz,
        // like the earlier models: the writemask starts applying at SM2,
        // where FXC never emits a partial one but hand-written shaders
        // rely on it.
        if (shader->header.major < 2) {
          test_lane[0] = test_lane[1] = test_lane[2] = true;
        } else {
          for (uint32_t i = 0; i < 4; ++i)
            test_lane[i] = ins.dst.mask[i];
        }
      } else {
        // SM 1.0-1.3 form. Source is the interpolated TEXCOORD
        // <dst.num> register, which our PS path already pre-loads
        // into tex_inputs at function entry (the same slot t# texld
        // would read for SM 1.4 / 2.0+). The writemask is ignored;
        // only xyz contribute (DXVK lines 3079-3087 collapse to a
        // 3-component reg before the < 0 test).
        if (!tex_inputs || ins.dst.base.num >= 8)
          break;
        auto *gep =
            builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(ins.dst.base.num)});
        coord4 = builder.CreateLoad(float4Ty, gep);
        test_lane[0] = test_lane[1] = test_lane[2] = true;
      }
      Value *zero_f = ConstantFP::get(Type::getFloatTy(context), 0.0);
      Value *any_neg = nullptr;
      for (uint32_t i = 0; i < 4; ++i) {
        if (!test_lane[i])
          continue;
        Value *lane = builder.CreateExtractElement(coord4, builder.getInt32(i));
        Value *lt = builder.CreateFCmpOLT(lane, zero_f);
        any_neg = any_neg ? builder.CreateOr(any_neg, lt) : lt;
      }
      if (!any_neg)
        break;
      auto *kill_bb = BasicBlock::Create(context, "texkill", fn);
      auto *cont_bb = BasicBlock::Create(context, "texkill.cont", fn);
      builder.CreateCondBr(any_neg, kill_bb, cont_bb);
      builder.SetInsertPoint(kill_bb);
      air.CreateDiscard();
      builder.CreateBr(cont_bb);
      builder.SetInsertPoint(cont_bb);
      break;
    }
    case DxsoOpcode::TexCoord: {
      // SM 1.0-1.3 `texcoord t<n>` (no source): clamp the interpolated
      // TEXCOORD<n> to [0,1], force w to 1.0 and write back to t<n>.
      // SM 1.4 `texcrd dst, src`: copy src to dst (no clamp, no
      // w-force); src swizzle and Dz/Dw modifier are lowered by
      // load_src. DXVK src/dxso/dxso_compiler.cpp emitTexCoord;
      // wined3d glsl_shader.c shader_glsl_texcoord:6432-6471.
      if (is_vertex || !ins.has_dst)
        break;
      bool sm14 = shader->header.major == 1 && shader->header.minor == 4;
      Value *coord4 = nullptr;
      if (sm14) {
        if (ins.src_count < 1)
          break;
        coord4 = load_src(ins.src[0]);
      } else {
        if (!tex_inputs || ins.dst.base.num >= 8)
          break;
        auto *gep =
            builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(ins.dst.base.num)});
        Value *raw = builder.CreateLoad(float4Ty, gep);
        coord4 = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, raw);
        coord4 =
            builder.CreateInsertElement(coord4, ConstantFP::get(Type::getFloatTy(context), 1.0), builder.getInt32(3));
      }
      store_dst(ins.dst, coord4);
      break;
    }
    case DxsoOpcode::TexDp3:
    case DxsoOpcode::TexDp3Tex: {
      // SM 1.x: 3-component dot product of the texcoord input at
      // tex_inputs[dst.num] (interpolated TEXCOORD<n>) and src[0].
      // TexDp3 stores the scalar dot product splat to dst (no sampling
      // happens). TexDp3Tex feeds the scalar as a 1D coord into a
      // sampler[dst.num] sample, lookup result lands in dst.
      // DXVK dxso_compiler.cpp.
      if (is_vertex || !ins.has_dst || ins.src_count < 1 || !tex_inputs)
        break;
      uint32_t slot = ins.dst.base.num;
      if (slot >= 8)
        break;
      Value *src4 = load_src(ins.src[0]);
      if (!src4)
        break;
      auto *gep = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(slot)});
      Value *coord4 = builder.CreateLoad(float4Ty, gep);
      int xyz[3] = {0, 1, 2};
      Value *coord3 = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xyz, 3));
      Value *src3 = builder.CreateShuffleVector(src4, src4, ArrayRef<int>(xyz, 3));
      Value *dot = compute_dot(coord3, src3, 3);
      if (ins.opcode == DxsoOpcode::TexDp3) {
        // Splat the dot result to all 4 lanes; store_dst applies the
        // writemask. Mirrors the existing Dp3 / Dp4 opcode pattern.
        store_dst(ins.dst, builder.CreateVectorSplat(4, dot));
        break;
      }
      // TexDp3Tex: sample at sampler[slot] using `dot` as the texcoord.
      if (tex_arg_idx[slot] < 0)
        break;
      Value *sample_coord;
      llvm::air::Texture::ResourceKind air_kind;
      if (samp_kind[slot] == DxsoTextureType::Texture2D || samp_kind[slot] == DxsoTextureType::Texture2DDepth) {
        // 1D coord under a 2D sampler: pack as (dot, 0) per DXVK
        // shape (coord3 indices[0]=dot, [1]=[2]=[3]=0).
        auto *float2Ty = FixedVectorType::get(Type::getFloatTy(context), 2);
        Value *c = UndefValue::get(float2Ty);
        c = builder.CreateInsertElement(c, dot, builder.getInt32(0));
        c = builder.CreateInsertElement(c, ConstantFP::get(Type::getFloatTy(context), 0.0f), builder.getInt32(1));
        sample_coord = c;
        air_kind = samp_kind[slot] == DxsoTextureType::Texture2DDepth ? llvm::air::Texture::depth_2d
                                                                      : llvm::air::Texture::texture_2d;
      } else if (samp_kind[slot] == DxsoTextureType::TextureCube) {
        auto *float3Ty = FixedVectorType::get(Type::getFloatTy(context), 3);
        Value *c = UndefValue::get(float3Ty);
        c = builder.CreateInsertElement(c, dot, builder.getInt32(0));
        c = builder.CreateInsertElement(c, ConstantFP::get(Type::getFloatTy(context), 0.0f), builder.getInt32(1));
        c = builder.CreateInsertElement(c, ConstantFP::get(Type::getFloatTy(context), 0.0f), builder.getInt32(2));
        sample_coord = c;
        air_kind = llvm::air::Texture::texturecube;
      } else {
        auto *float3Ty = FixedVectorType::get(Type::getFloatTy(context), 3);
        Value *c = UndefValue::get(float3Ty);
        c = builder.CreateInsertElement(c, dot, builder.getInt32(0));
        c = builder.CreateInsertElement(c, ConstantFP::get(Type::getFloatTy(context), 0.0f), builder.getInt32(1));
        c = builder.CreateInsertElement(c, ConstantFP::get(Type::getFloatTy(context), 0.0f), builder.getInt32(2));
        sample_coord = c;
        air_kind = llvm::air::Texture::texture3d;
      }
      Value *tex_handle = fn->getArg(tex_arg_idx[slot]);
      Value *samp_handle = fn->getArg(samp_arg_idx[slot]);
      const int32_t no_offset[3] = {0, 0, 0};
      llvm::air::Texture tex_desc{
          .kind = air_kind,
          .sample_type = llvm::air::Texture::sample_float,
          .memory_access = llvm::air::Texture::access_sample,
      };
      Value *legacy_bias = load_samp_bias(slot);
      auto [texel, residency] = legacy_bias
                                    ? air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, sample_coord,
                                          /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{legacy_bias}
                                      )
                                    : air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, sample_coord,
                                          /*ArrayIndex=*/nullptr, no_offset
                                      );
      (void)residency;
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::TexReg2Rgb: {
      // SM 1.x dependent texture read: sample 2D/3D/cube texture at
      // sampler <dst.num> using src.rgb as the coord. DXVK
      // dxso_compiler.cpp swizzles (0,1,2,2); for 2D only
      // .rg is used, for 3D/cube all three. wined3d glsl_shader.c
      // shader_glsl_texreg2rgb:6832.
      if (is_vertex || !ins.has_dst || ins.src_count < 1)
        break;
      uint32_t slot = ins.dst.base.num;
      if (slot >= 16 || tex_arg_idx[slot] < 0)
        break;
      Value *src4 = load_src(ins.src[0]);
      if (!src4)
        break;
      int xy[2] = {0, 1};
      int xyz[3] = {0, 1, 2};
      Value *coord;
      llvm::air::Texture::ResourceKind air_kind;
      if (samp_kind[slot] == DxsoTextureType::Texture2D || samp_kind[slot] == DxsoTextureType::Texture2DDepth) {
        coord = builder.CreateShuffleVector(src4, src4, ArrayRef<int>(xy, 2));
        air_kind = samp_kind[slot] == DxsoTextureType::Texture2DDepth ? llvm::air::Texture::depth_2d
                                                                      : llvm::air::Texture::texture_2d;
      } else if (samp_kind[slot] == DxsoTextureType::TextureCube) {
        coord = builder.CreateShuffleVector(src4, src4, ArrayRef<int>(xyz, 3));
        air_kind = llvm::air::Texture::texturecube;
      } else {
        coord = builder.CreateShuffleVector(src4, src4, ArrayRef<int>(xyz, 3));
        air_kind = llvm::air::Texture::texture3d;
      }
      Value *tex_handle = fn->getArg(tex_arg_idx[slot]);
      Value *samp_handle = fn->getArg(samp_arg_idx[slot]);
      const int32_t no_offset[3] = {0, 0, 0};
      llvm::air::Texture tex_desc{
          .kind = air_kind,
          .sample_type = llvm::air::Texture::sample_float,
          .memory_access = llvm::air::Texture::access_sample,
      };
      Value *legacy_bias = load_samp_bias(slot);
      auto [texel, residency] = legacy_bias
                                    ? air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord,
                                          /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{legacy_bias}
                                      )
                                    : air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord,
                                          /*ArrayIndex=*/nullptr, no_offset
                                      );
      (void)residency;
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::TexBem:
    case DxsoOpcode::TexBemL:
    case DxsoOpcode::Bem: {
      // SM 1.0..1.3 bump-environment mapping. Perturbs the destination
      // stage's interpolated texcoord by a 2x2 matrix applied to the
      // previous stage's bump-map sample, then (TexBem/TexBemL) samples
      // the destination stage. Bem does the math only; no sampling.
      // DXVK dxso_compiler.cpp (emitBem) + 2790-2803 (sample
      // wiring) + 2968-2987 (TexBemL luminance).
      //
      //   src0 = tc[dst.num] (interpolated TEXCOORD<dst.num>)
      //   src1 = n (typically t<dst.num-1>'s post-sample register)
      //   dst.u = src0.x + bm[0][0]*n.x + bm[1][0]*n.y
      //   dst.v = src0.y + bm[0][1]*n.x + bm[1][1]*n.y
      //   TexBem  : sample dst.num at (dst.u, dst.v), store texel.
      //   TexBemL : same as TexBem, then result *= clamp(n.z * lscale +
      //             loffset, 0, 1). (n.z = the bump-map source register's z
      //             per wined3d shader_glsl_texbem; DXVK instead reads the
      //             post-sample result's z. dxmt follows wined3d here.)
      //   Bem     : store (dst.u, dst.v, 0, 0) to dst, no sample.
      //
      // The bump-env matrix + luminance scale/offset are read from the
      // shared PS uniform tail per stage (see load_bem below); the host
      // writes every stage's D3DTSS_BUMPENV* there each draw, defaulting to
      // zero for an unset stage (no perturbation), so no arg is threaded.
      if (is_vertex || !ins.has_dst || ins.src_count < 1 || !tex_inputs)
        break;
      uint32_t slot = ins.dst.base.num;
      if (slot >= 8)
        break;
      // Operand shapes differ by opcode: TexBem / TexBemL carry one
      // source (the bump-map sample) and take the base from the
      // interpolated TEXCOORD<dst.num> implicitly, while the ps_1_4
      // bem is fully explicit (dst.rg = src0.rg + matrix * src1.rg).
      // Loads go through load_src so SM 1.x source modifiers apply.
      const bool is_bem = ins.opcode == DxsoOpcode::Bem;
      Value *n4 = load_src(is_bem ? ins.src[1] : ins.src[0]);
      if (!n4)
        break;
      Value *tc4;
      if (is_bem) {
        // Explicit base register; bem never projects.
        tc4 = load_src(ins.src[0]);
        if (!tc4)
          break;
      } else {
        // Interpolated TEXCOORD<dst.num>, pre-loaded into tex_inputs at
        // function entry; the SM 1.0..1.3 implicit-source convention
        // used by Tex, TexBem*, and the TexM3x* family. DXVK projects
        // the texcoord BEFORE the bump perturbation, gated on the
        // per-sampler PROJECTED bit; the bump sample n is not
        // projected.
        auto *gep_tc = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(slot)});
        tc4 = builder.CreateLoad(float4Ty, gep_tc);
        tc4 = apply_projected(tc4, slot);
      }
      Value *tc_x = builder.CreateExtractElement(tc4, builder.getInt32(0));
      Value *tc_y = builder.CreateExtractElement(tc4, builder.getInt32(1));
      Value *n_x = builder.CreateExtractElement(n4, builder.getInt32(0));
      Value *n_y = builder.CreateExtractElement(n4, builder.getInt32(1));
      auto *fT = Type::getFloatTy(context);
      // Per-stage bump-env constants ride the shared PS uniform tail
      // (buffer(2), six floats per stage from uint32 index 32: bm00, bm01,
      // bm10, bm11, lscale, loffset), host-written from the D3DTSS_BUMPENV*
      // state per draw. Reading them at runtime keeps one variant per module
      // instead of baking a per-value matrix, so an app that animates
      // bump-env stops churning cold PSO links. The perturbation math is
      // unchanged, so a fixed matrix samples identically. DXVK reads the same
      // block from its D3D9SharedPS uniform (dxso_compiler.cpp emitBem).
      auto *u32Ty = Type::getInt32Ty(context);
      const uint32_t bem_base = 32u + slot * 6u;
      auto load_bem = [&](uint32_t lane) -> Value * {
        Value *p = builder.CreateGEP(u32Ty, fn->getArg(bc_arg_idx), builder.getInt32((int)(bem_base + lane)));
        return builder.CreateBitCast(builder.CreateLoad(u32Ty, p), fT);
      };
      Value *bm00 = load_bem(0), *bm01 = load_bem(1), *bm10 = load_bem(2), *bm11 = load_bem(3);
      // perturbed.x = tc.x + bm00 * n.x + bm10 * n.y
      Value *u =
          builder.CreateFAdd(tc_x, builder.CreateFAdd(builder.CreateFMul(bm00, n_x), builder.CreateFMul(bm10, n_y)));
      // perturbed.y = tc.y + bm01 * n.x + bm11 * n.y
      Value *v =
          builder.CreateFAdd(tc_y, builder.CreateFAdd(builder.CreateFMul(bm01, n_x), builder.CreateFMul(bm11, n_y)));
      // Bem: math-only, no sample. Store (u, v, 0, 0) to dst per DXVK
      // shape (the math-only output keeps the lower two lanes; the
      // writemask filters which actually land).
      if (ins.opcode == DxsoOpcode::Bem) {
        Value *out = UndefValue::get(float4Ty);
        out = builder.CreateInsertElement(out, u, builder.getInt32(0));
        out = builder.CreateInsertElement(out, v, builder.getInt32(1));
        out = builder.CreateInsertElement(out, ConstantFP::get(fT, 0.0f), builder.getInt32(2));
        out = builder.CreateInsertElement(out, ConstantFP::get(fT, 0.0f), builder.getInt32(3));
        store_dst(ins.dst, out);
        break;
      }
      // TexBem / TexBemL: sample at sampler[slot] with (u, v).
      if (tex_arg_idx[slot] < 0)
        break;
      auto *float2Ty = FixedVectorType::get(fT, 2);
      Value *coord2 = UndefValue::get(float2Ty);
      coord2 = builder.CreateInsertElement(coord2, u, builder.getInt32(0));
      coord2 = builder.CreateInsertElement(coord2, v, builder.getInt32(1));
      llvm::air::Texture::ResourceKind air_kind = samp_kind[slot] == DxsoTextureType::Texture2DDepth
                                                      ? llvm::air::Texture::depth_2d
                                                      : llvm::air::Texture::texture_2d;
      Value *tex_handle = fn->getArg(tex_arg_idx[slot]);
      Value *samp_handle = fn->getArg(samp_arg_idx[slot]);
      const int32_t no_offset[3] = {0, 0, 0};
      llvm::air::Texture tex_desc{
          .kind = air_kind,
          .sample_type = llvm::air::Texture::sample_float,
          .memory_access = llvm::air::Texture::access_sample,
      };
      Value *legacy_bias = load_samp_bias(slot);
      auto [texel, residency] = legacy_bias
                                    ? air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord2,
                                          /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{legacy_bias}
                                      )
                                    : air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord2,
                                          /*ArrayIndex=*/nullptr, no_offset
                                      );
      (void)residency;
      // TexBemL: scale by clamp(n.z * lscale + loffset, 0, 1) where
      // n.z is the BUMP-MAP sample's z, the source register loaded
      // above; the perturbed destination sample only receives the
      // scale. Reading the destination z instead saturated the factor
      // to one on typical content, which wine's bump rows pin against
      // the source-z form (the same source the generated combiner's
      // luminance arm reads).
      if (ins.opcode == DxsoOpcode::TexBemL) {
        // lscale / loffset ride the same shared PS uniform tail as the matrix
        // (lanes 4 and 5 of the stage's six-float block).
        Value *lscale = load_bem(4);
        Value *loffset = load_bem(5);
        Value *n_z = builder.CreateExtractElement(n4, builder.getInt32(2));
        Value *lum = builder.CreateFAdd(builder.CreateFMul(n_z, lscale), loffset);
        lum = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, lum);
        Value *lum_splat = builder.CreateVectorSplat(4, lum);
        texel = builder.CreateFMul(texel, lum_splat);
      }
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::TexM3x3:
    case DxsoOpcode::TexM3x3Tex:
    case DxsoOpcode::TexM3x2Tex: {
      // SM 1.x texture-coordinate matrix multiply with optional sample.
      // DXVK dxso_compiler.cpp. Each opcode consumes a matrix
      // assembled from the previous (count-1) texcoord input registers;
      // the matching TexM3x{2,3}Pad ops are bytecode-side markers
      // (no codegen, handled as no-op cases above) whose dst register
      // indices identify the rows. The destination's own register
      // (dst.num) supplies the final row.
      //
      //   TexM3x2Tex (count=2): rows from tex_inputs[dst.num-1, dst.num],
      //                         coord = float4(d0, d1, 0, 0), sample 2D.
      //   TexM3x3Tex (count=3): rows from tex_inputs[dst.num-2..dst.num],
      //                         coord = float4(d0, d1, d2, 0), sample
      //                         per the bound texture kind (cube / 3D).
      //   TexM3x3    (count=3): same matrix, but store the dots
      //                         directly without sampling; used to feed
      //                         a follow-up TexM3x3Spec / TexM3x3VSpec.
      // PS 1.x environment-map shaders commonly use TexM3x3Tex to
      // look up tangent-space cube reflections; silently
      // dropping these ops produces black-where-shiny on reflective
      // surfaces.
      if (is_vertex || !ins.has_dst || ins.src_count < 1 || !tex_inputs)
        break;
      uint32_t slot = ins.dst.base.num;
      const uint32_t count = (ins.opcode == DxsoOpcode::TexM3x2Tex) ? 2u : 3u;
      if (slot >= 8 || slot + 1 < count)
        break;
      Value *n4 = load_src(ins.src[0]);
      if (!n4)
        break;
      int xyz[3] = {0, 1, 2};
      Value *n = builder.CreateShuffleVector(n4, n4, ArrayRef<int>(xyz, 3));
      Value *dots[3] = {
          ConstantFP::get(Type::getFloatTy(context), 0.0f), ConstantFP::get(Type::getFloatTy(context), 0.0f),
          ConstantFP::get(Type::getFloatTy(context), 0.0f)
      };
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t row_slot = slot - (count - 1) + i;
        auto *gep = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(row_slot)});
        Value *row4 = builder.CreateLoad(float4Ty, gep);
        Value *row = builder.CreateShuffleVector(row4, row4, ArrayRef<int>(xyz, 3));
        dots[i] = compute_dot(row, n, 3);
      }
      // Pack dots into a float4 (z = 0 for count==2, w always 0).
      Value *coord4 = UndefValue::get(float4Ty);
      coord4 = builder.CreateInsertElement(coord4, dots[0], builder.getInt32(0));
      coord4 = builder.CreateInsertElement(coord4, dots[1], builder.getInt32(1));
      coord4 = builder.CreateInsertElement(coord4, dots[2], builder.getInt32(2));
      coord4 =
          builder.CreateInsertElement(coord4, ConstantFP::get(Type::getFloatTy(context), 0.0f), builder.getInt32(3));
      // TexM3x3 stores the matrix dots without sampling; finishes here.
      if (ins.opcode == DxsoOpcode::TexM3x3) {
        store_dst(ins.dst, coord4);
        break;
      }
      if (tex_arg_idx[slot] < 0)
        break;
      int xy_only[2] = {0, 1};
      Value *coord;
      llvm::air::Texture::ResourceKind air_kind;
      if (samp_kind[slot] == DxsoTextureType::Texture2D || samp_kind[slot] == DxsoTextureType::Texture2DDepth) {
        coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xy_only, 2));
        air_kind = samp_kind[slot] == DxsoTextureType::Texture2DDepth ? llvm::air::Texture::depth_2d
                                                                      : llvm::air::Texture::texture_2d;
      } else if (samp_kind[slot] == DxsoTextureType::TextureCube) {
        coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xyz, 3));
        air_kind = llvm::air::Texture::texturecube;
      } else {
        coord = builder.CreateShuffleVector(coord4, coord4, ArrayRef<int>(xyz, 3));
        air_kind = llvm::air::Texture::texture3d;
      }
      Value *tex_handle = fn->getArg(tex_arg_idx[slot]);
      Value *samp_handle = fn->getArg(samp_arg_idx[slot]);
      const int32_t no_offset[3] = {0, 0, 0};
      llvm::air::Texture tex_desc{
          .kind = air_kind,
          .sample_type = llvm::air::Texture::sample_float,
          .memory_access = llvm::air::Texture::access_sample,
      };
      Value *legacy_bias = load_samp_bias(slot);
      auto [texel, residency] = legacy_bias
                                    ? air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord,
                                          /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{legacy_bias}
                                      )
                                    : air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord,
                                          /*ArrayIndex=*/nullptr, no_offset
                                      );
      (void)residency;
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::TexM3x3Spec:
    case DxsoOpcode::TexM3x3VSpec: {
      // SM 1.x reflection cube-map sample. DXVK dxso_compiler.cpp:
      // 2755-2786. Computes a 3×3 matrix from the prior 2 + current
      // texcoord input registers, dots src[0] (tangent) against each
      // row to get the surface normal, builds an eye ray (from src[1]
      // for Spec, from .w of the same 3 texcoord inputs for VSpec),
      // then samples the bound texture (typically a cube) at
      // -reflect(normalize(eyeRay), normalize(normal)). Used for SM1.x
      // environment-mapped specular highlights and reflections common
      // in shaders of that era.
      if (is_vertex || !ins.has_dst || !tex_inputs)
        break;
      const uint32_t count = 3;
      uint32_t slot = ins.dst.base.num;
      if (slot >= 8 || slot + 1 < count)
        break;
      bool is_vspec = ins.opcode == DxsoOpcode::TexM3x3VSpec;
      if (!is_vspec && ins.src_count < 2)
        break;
      if (is_vspec && ins.src_count < 1)
        break;
      Value *n4 = load_src(ins.src[0]);
      if (!n4)
        break;
      int xyz[3] = {0, 1, 2};
      auto *float3Ty = FixedVectorType::get(Type::getFloatTy(context), 3);
      Value *n = builder.CreateShuffleVector(n4, n4, ArrayRef<int>(xyz, 3));
      // Dot each row of the texcoord matrix against src[0] → surface normal.
      Value *normal = UndefValue::get(float3Ty);
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t row_slot = slot - (count - 1) + i;
        auto *gep = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(row_slot)});
        Value *row4 = builder.CreateLoad(float4Ty, gep);
        Value *row = builder.CreateShuffleVector(row4, row4, ArrayRef<int>(xyz, 3));
        Value *d = compute_dot(row, n, 3);
        normal = builder.CreateInsertElement(normal, d, builder.getInt32(i));
      }
      // Eye ray: VSpec sources the .w of each row's texcoord input;
      // Spec sources the .xyz of src[1].
      Value *eye3 = UndefValue::get(float3Ty);
      if (is_vspec) {
        for (uint32_t i = 0; i < count; ++i) {
          uint32_t row_slot = slot - (count - 1) + i;
          auto *gep = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(row_slot)});
          Value *row4 = builder.CreateLoad(float4Ty, gep);
          Value *w = builder.CreateExtractElement(row4, builder.getInt32(3));
          eye3 = builder.CreateInsertElement(eye3, w, builder.getInt32(i));
        }
      } else {
        Value *src1_4 = load_src(ins.src[1]);
        if (!src1_4)
          break;
        eye3 = builder.CreateShuffleVector(src1_4, src1_4, ArrayRef<int>(xyz, 3));
      }
      // normalize(v) = v * rsqrt(dot(v, v)); same shape as the
      // existing Nrm opcode case.
      auto normalize3 = [&](Value *v) -> Value * {
        Value *dot = compute_dot(v, v, 3);
        Value *rcp_len = air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, dot);
        Value *splat = builder.CreateVectorSplat(3, rcp_len);
        return builder.CreateFMul(v, splat);
      };
      Value *eye_n = normalize3(eye3);
      Value *normal_n = normalize3(normal);
      // reflect(I, N) = I - 2 * dot(N, I) * N. Per GLSL/HLSL convention.
      Value *dot_NI = compute_dot(normal_n, eye_n, 3);
      Value *two = ConstantFP::get(Type::getFloatTy(context), 2.0f);
      Value *two_dot = builder.CreateFMul(dot_NI, two);
      Value *splat_2dot = builder.CreateVectorSplat(3, two_dot);
      Value *scaled_n = builder.CreateFMul(normal_n, splat_2dot);
      Value *reflection = builder.CreateFSub(eye_n, scaled_n);
      // DXVK negates the reflection vector before sampling
      // (dxso_compiler.cpp). Matches the cubemap-sample convention
      // games of this era expect.
      Value *neg_reflection = builder.CreateFNeg(reflection);
      if (tex_arg_idx[slot] < 0)
        break;
      Value *coord;
      llvm::air::Texture::ResourceKind air_kind;
      if (samp_kind[slot] == DxsoTextureType::TextureCube) {
        coord = neg_reflection;
        air_kind = llvm::air::Texture::texturecube;
      } else if (samp_kind[slot] == DxsoTextureType::Texture3D) {
        coord = neg_reflection;
        air_kind = llvm::air::Texture::texture3d;
      } else {
        // 2D fallback: extract xy. Apps binding a 2D texture to a
        // TexM3x3Spec/VSpec sampler is unusual but valid; we degrade
        // gracefully rather than failing the compile.
        int xy[2] = {0, 1};
        coord = builder.CreateShuffleVector(neg_reflection, neg_reflection, ArrayRef<int>(xy, 2));
        air_kind = samp_kind[slot] == DxsoTextureType::Texture2DDepth ? llvm::air::Texture::depth_2d
                                                                      : llvm::air::Texture::texture_2d;
      }
      Value *tex_handle = fn->getArg(tex_arg_idx[slot]);
      Value *samp_handle = fn->getArg(samp_arg_idx[slot]);
      const int32_t no_offset[3] = {0, 0, 0};
      llvm::air::Texture tex_desc{
          .kind = air_kind,
          .sample_type = llvm::air::Texture::sample_float,
          .memory_access = llvm::air::Texture::access_sample,
      };
      Value *legacy_bias = load_samp_bias(slot);
      auto [texel, residency] = legacy_bias
                                    ? air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord,
                                          /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{legacy_bias}
                                      )
                                    : air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord,
                                          /*ArrayIndex=*/nullptr, no_offset
                                      );
      (void)residency;
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::TexReg2Ar:
    case DxsoOpcode::TexReg2Gb: {
      // SM 1.x dependent texture read: sample 2D texture at sampler
      // <dst.num> using two channels of src[0] as the UV coordinate,
      // and write the resulting texel to t<dst.num>. TexReg2Ar uses
      // .wx (alpha → u, red → v); TexReg2Gb uses .yz (green → u,
      // blue → v). wined3d glsl_shader.c shader_glsl_texreg2ar:6792 /
      // shader_glsl_texreg2gb:6812.
      if (is_vertex || !ins.has_dst || ins.src_count < 1)
        break;
      uint32_t slot = ins.dst.base.num;
      if (slot >= 16 || tex_arg_idx[slot] < 0)
        break;
      // These reads sample two colour channels of a 2D texture; the arm
      // hard-codes a texture_2d sample. A slot the host pinned as a cube, 3D
      // or depth texture declares a mismatched sampler argument (an invalid
      // metallib) and is not a valid target for these ops anyway, so skip it
      // with a warn. Texture2D and Unknown both declare a texture_2d arg that
      // matches the sample below.
      if (samp_kind[slot] == DxsoTextureType::TextureCube || samp_kind[slot] == DxsoTextureType::Texture3D ||
          samp_kind[slot] == DxsoTextureType::Texture2DDepth) {
        static std::atomic<bool> warned_texreg2{false};
        if (!warned_texreg2.exchange(true, std::memory_order_relaxed))
          llvm::errs() << "dxso: texreg2ar/gb on a non-2D-colour sampler is unsupported\n";
        break;
      }
      Value *src4 = load_src(ins.src[0]);
      if (!src4)
        break;
      uint32_t lane_u = (ins.opcode == DxsoOpcode::TexReg2Ar) ? 3 : 1;
      uint32_t lane_v = (ins.opcode == DxsoOpcode::TexReg2Ar) ? 0 : 2;
      auto *float2Ty = VectorType::get(Type::getFloatTy(context), ElementCount::getFixed(2));
      Value *coord = UndefValue::get(float2Ty);
      coord = builder.CreateInsertElement(
          coord, builder.CreateExtractElement(src4, builder.getInt32(lane_u)), builder.getInt32(0)
      );
      coord = builder.CreateInsertElement(
          coord, builder.CreateExtractElement(src4, builder.getInt32(lane_v)), builder.getInt32(1)
      );
      Value *tex_handle = fn->getArg(tex_arg_idx[slot]);
      Value *samp_handle = fn->getArg(samp_arg_idx[slot]);
      const int32_t no_offset[3] = {0, 0, 0};
      llvm::air::Texture tex_desc{
          .kind = llvm::air::Texture::texture_2d,
          .sample_type = llvm::air::Texture::sample_float,
          .memory_access = llvm::air::Texture::access_sample,
      };
      Value *legacy_bias = load_samp_bias(slot);
      auto [texel, residency] = legacy_bias
                                    ? air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord,
                                          /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{legacy_bias}
                                      )
                                    : air.CreateSample(
                                          tex_desc, tex_handle, samp_handle, coord,
                                          /*ArrayIndex=*/nullptr, no_offset
                                      );
      (void)residency;
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::If: {
      // SM2+ `if b#` reads a bool register and `if p#` reads a
      // predicate. DXVK src/dxso/dxso_compiler.cpp.
      // Predicate p0.<comp> reads through p0_slot. Bool reads
      // first try the def-baked literal table (defb b#, true|false),
      // then fall back to a runtime load from the b# bitmask
      // binding (DXVK src/d3d9/d3d9_constant_set.h bConsts[1]:
      // bit i = b#i).
      if (ins.src_count < 1)
        break;
      const auto &s = ins.src[0];
      Value *cond = nullptr;
      if (s.base.type == DxsoRegisterType::ConstBool) {
        for (const auto &c : shader->metadata.consts) {
          if (c.bound_to.type == DxsoRegisterType::ConstBool && c.bound_to.num == s.base.num &&
              c.def.kind == DxsoDefKind::Bool) {
            bool taken = c.def.payload.u32[0] != 0;
            if (s.modifier == DxsoRegModifier::Not)
              taken = !taken;
            cond = builder.getInt1(taken);
            break;
          }
        }
        if (!cond && s.base.num < 16) {
          auto *u32Ty = Type::getInt32Ty(context);
          auto *bcPtr = fn->getArg(bc_arg_idx);
          Value *bits = builder.CreateLoad(u32Ty, bcPtr);
          Value *mask = builder.getInt32(1u << s.base.num);
          Value *masked = builder.CreateAnd(bits, mask);
          cond = builder.CreateICmpNE(masked, builder.getInt32(0));
          if (s.modifier == DxsoRegModifier::Not)
            cond = builder.CreateNot(cond);
        }
      } else if (s.base.type == DxsoRegisterType::Predicate && s.base.num == 0) {
        Value *pmask = builder.CreateLoad(bool4Ty, p0_slot);
        cond = builder.CreateExtractElement(pmask, builder.getInt32(s.swizzle[0]));
        if (s.modifier == DxsoRegModifier::Not)
          cond = builder.CreateNot(cond);
      }
      // An undecodable condition must not skip the frame push: the matching
      // Else/EndIf would otherwise pop the ENCLOSING if and mis-structure the
      // CFG. Fall back to an always-taken branch so the block stays paired.
      if (!cond)
        cond = builder.getInt1(true);
      auto *then_bb = BasicBlock::Create(context, "if.then", fn);
      auto *else_bb = BasicBlock::Create(context, "if.else", fn);
      auto *merge_bb = BasicBlock::Create(context, "if.end", fn);
      builder.CreateCondBr(cond, then_bb, else_bb);
      builder.SetInsertPoint(then_bb);
      cf_stack.push_back({else_bb, merge_bb, false});
      break;
    }
    case DxsoOpcode::Ifc: {
      // Structured control flow. DXVK src/dxso/dxso_compiler.cpp.
      // Pre-create both true and false BBs so that an `if` without an
      // `else` still has a fall-through edge into the merge block.
      if (ins.src_count < 2)
        break;
      Value *a = load_src(ins.src[0]);
      Value *b = load_src(ins.src[1]);
      if (!a || !b)
        break;
      Value *ax = builder.CreateExtractElement(a, builder.getInt32(0));
      Value *bx = builder.CreateExtractElement(b, builder.getInt32(0));
      // DxsoComparison: 1=GT, 2=EQ, 3=GE, 4=LT, 5=NE, 6=LE. NotEqual
      // is unordered per DXVK src/dxso/dxso_compiler.cpp; the
      // rest are ordered.
      Value *cond = nullptr;
      switch (ins.specific_data) {
      case 1:
        cond = builder.CreateFCmpOGT(ax, bx);
        break;
      case 2:
        cond = builder.CreateFCmpOEQ(ax, bx);
        break;
      case 3:
        cond = builder.CreateFCmpOGE(ax, bx);
        break;
      case 4:
        cond = builder.CreateFCmpOLT(ax, bx);
        break;
      case 5:
        cond = builder.CreateFCmpUNE(ax, bx);
        break;
      case 6:
        cond = builder.CreateFCmpOLE(ax, bx);
        break;
      default:
        break;
      }
      // An unknown comparison must not skip the frame push: the matching
      // Else/EndIf would otherwise pop the ENCLOSING if and mis-structure the
      // CFG. Fall back to an always-taken branch so the block stays paired.
      if (!cond)
        cond = builder.getInt1(true);
      auto *then_bb = BasicBlock::Create(context, "if.then", fn);
      auto *else_bb = BasicBlock::Create(context, "if.else", fn);
      auto *merge_bb = BasicBlock::Create(context, "if.end", fn);
      builder.CreateCondBr(cond, then_bb, else_bb);
      builder.SetInsertPoint(then_bb);
      cf_stack.push_back({else_bb, merge_bb, false});
      break;
    }
    case DxsoOpcode::Else: {
      if (cf_stack.empty())
        break;
      IfBlock &b = cf_stack.back();
      if (b.saw_else)
        break;
      builder.CreateBr(b.merge_bb);
      builder.SetInsertPoint(b.else_bb);
      b.saw_else = true;
      break;
    }
    case DxsoOpcode::EndIf: {
      if (cf_stack.empty())
        break;
      IfBlock b = cf_stack.back();
      cf_stack.pop_back();
      builder.CreateBr(b.merge_bb);
      if (!b.saw_else) {
        // No else arm; close the empty else_bb with a fall-through
        // before switching to the merge block.
        builder.SetInsertPoint(b.else_bb);
        builder.CreateBr(b.merge_bb);
      }
      builder.SetInsertPoint(b.merge_bb);
      break;
    }
    case DxsoOpcode::Rep:
    case DxsoOpcode::Loop: {
      // Rep src0 (i#.x = count): DXVK src/dxso/dxso_compiler.cpp.
      // Loop aL, src1 (i#.x = count, .y = init aL, .z = stride);
      // DXVK src/dxso/dxso_compiler.cpp. Counts can come from a
      // def-baked literal (defi i#, ...) or from the runtime i#
      // binding ([[buffer(1)]]); the same emitter handles both since
      // LoopFrame carries Values, not int32_t constants.
      bool is_loop = ins.opcode == DxsoOpcode::Loop;
      if (ins.src_count < (is_loop ? 2u : 1u))
        break;
      const auto &s = ins.src[is_loop ? 1u : 0u];
      auto *i32Ty = Type::getInt32Ty(context);
      auto *int4Ty = FixedVectorType::get(i32Ty, 4);
      // A malformed count source (not an i# register, or an i# number past
      // the 16-register file) still opens a block that a matching
      // EndRep/EndLoop will close, so force-push a degenerate zero-count
      // frame exactly the way the If/Ifc undecodable-condition arms above
      // force-push an always-taken branch. Skipping the push instead would
      // let the EndRep pop the ENCLOSING loop and mis-nest the CFG. count
      // stays 0 so the header's ULT(cur, 0) skips the body. FXC always emits
      // an in-range i# count; this path is reachable only from hand-crafted
      // bytecode.
      Value *count = builder.getInt32(0);
      Value *init_aL = builder.getInt32(0);
      Value *stride = builder.getInt32(0);
      if (s.base.type == DxsoRegisterType::ConstInt && s.base.num < 16) {
        const DxsoBoundConst *match = nullptr;
        for (const auto &c : shader->metadata.consts) {
          if (c.bound_to.type == DxsoRegisterType::ConstInt && c.bound_to.num == s.base.num &&
              c.def.kind == DxsoDefKind::Int32) {
            match = &c;
            break;
          }
        }
        if (match) {
          count = builder.getInt32(match->def.payload.i32[0]);
          if (is_loop) {
            init_aL = builder.getInt32(match->def.payload.i32[1]);
            stride = builder.getInt32(match->def.payload.i32[2]);
          }
        } else {
          // Runtime i# read. The binding is `int4 *i` at slot 1; GEP
          // by reg num then load the lane the loop emitter needs.
          auto *icPtr = fn->getArg(ic_arg_idx);
          Value *idx = builder.getInt32(s.base.num);
          auto *gep = builder.CreateGEP(int4Ty, icPtr, idx);
          Value *vec = builder.CreateLoad(int4Ty, gep);
          count = builder.CreateExtractElement(vec, builder.getInt32(0));
          if (is_loop) {
            init_aL = builder.CreateExtractElement(vec, builder.getInt32(1));
            stride = builder.CreateExtractElement(vec, builder.getInt32(2));
          }
        }
      }
      auto *header_bb = BasicBlock::Create(context, "loop.header", fn);
      auto *body_bb = BasicBlock::Create(context, "loop.body", fn);
      auto *latch_bb = BasicBlock::Create(context, "loop.latch", fn);
      auto *merge_bb = BasicBlock::Create(context, "loop.end", fn);
      // The counter and aL backup are function-scoped stack slots, so they
      // belong in the entry block: one lexical slot per Loop suffices (the
      // store below re-initialises it on each dynamic entry). Emitting the
      // alloca at the loop site instead would re-run it every outer iteration
      // of a nested loop and keep it out of mem2reg's entry-block promotion.
      IRBuilder<> entry_builder(&fn->getEntryBlock(), fn->getEntryBlock().begin());
      Value *counter = entry_builder.CreateAlloca(i32Ty, nullptr, "loop.i");
      builder.CreateStore(builder.getInt32(0), counter);
      Value *aL_backup = nullptr;
      if (is_loop) {
        // Save the outer-scope aL before overwriting with init so a
        // nested Loop can restore on EndLoop. DXVK
        // src/dxso/dxso_compiler.cpp / 2500-2507.
        aL_backup = entry_builder.CreateAlloca(i32Ty, nullptr, "aL.backup");
        Value *outer = builder.CreateLoad(i32Ty, aL_slot);
        builder.CreateStore(outer, aL_backup);
        builder.CreateStore(init_aL, aL_slot);
      }
      builder.CreateBr(header_bb);
      builder.SetInsertPoint(header_bb);
      Value *cur = builder.CreateLoad(i32Ty, counter);
      // Unsigned compare: hardware reads the i# count as a uint, so a
      // negative count means "iterate until the shader's own break", not
      // zero iterations (vkd3d's d3dbc rep tests pin this against native;
      // a shader relying on it carries its own limiter, as on hardware).
      Value *cond = builder.CreateICmpULT(cur, count);
      builder.CreateCondBr(cond, body_bb, merge_bb);
      builder.SetInsertPoint(body_bb);
      loop_stack.push_back({counter, aL_backup, count, stride, header_bb, latch_bb, merge_bb});
      break;
    }
    case DxsoOpcode::SetP: {
      // setp_<cmp> p0.<mask>, src0, src1; lane-wise compare, masked
      // store into p0. DXVK src/dxso/dxso_compiler.cpp. NotEqual
      // is unordered; the rest are ordered.
      if (!ins.has_dst || ins.src_count < 2)
        break;
      if (ins.dst.base.type != DxsoRegisterType::Predicate || ins.dst.base.num != 0)
        break;
      Value *a = load_src(ins.src[0]);
      Value *b = load_src(ins.src[1]);
      if (!a || !b)
        break;
      Value *cmp = nullptr;
      switch (ins.specific_data) {
      case 1:
        cmp = builder.CreateFCmpOGT(a, b);
        break;
      case 2:
        cmp = builder.CreateFCmpOEQ(a, b);
        break;
      case 3:
        cmp = builder.CreateFCmpOGE(a, b);
        break;
      case 4:
        cmp = builder.CreateFCmpOLT(a, b);
        break;
      case 5:
        cmp = builder.CreateFCmpUNE(a, b);
        break;
      case 6:
        cmp = builder.CreateFCmpOLE(a, b);
        break;
      default:
        break;
      }
      if (!cmp)
        break;
      Value *cur = builder.CreateLoad(bool4Ty, p0_slot);
      for (uint32_t i = 0; i < 4; ++i) {
        if (!ins.dst.mask[i])
          continue;
        Value *lane = builder.CreateExtractElement(cmp, builder.getInt32(i));
        cur = builder.CreateInsertElement(cur, lane, builder.getInt32(i));
      }
      builder.CreateStore(cur, p0_slot);
      break;
    }
    case DxsoOpcode::Break:
    case DxsoOpcode::BreakC: {
      // Early loop exit. DXVK src/dxso/dxso_compiler.cpp / 2559.
      // Break unconditionally jumps to the enclosing loop's merge BB; BreakC
      // wraps the jump in a comparison whose true edge takes the break and whose
      // false edge continues the body. Decode the condition first and create the
      // continuation block only once a terminator is guaranteed: a malformed
      // BreakC then bails as a no-op (builder stays in the open body block)
      // rather than stranding an unterminated, unreferenced block that fails the
      // LLVM verifier. Same fail-open shape the If/Ifc decoders use.
      if (loop_stack.empty())
        break;
      BasicBlock *merge_bb = loop_stack.back().merge_bb;
      Value *cond = nullptr;
      if (ins.opcode != DxsoOpcode::Break) {
        if (ins.src_count < 2)
          break;
        Value *a = load_src(ins.src[0]);
        Value *b = load_src(ins.src[1]);
        if (!a || !b)
          break;
        Value *ax = builder.CreateExtractElement(a, builder.getInt32(0));
        Value *bx = builder.CreateExtractElement(b, builder.getInt32(0));
        switch (ins.specific_data) {
        case 1:
          cond = builder.CreateFCmpOGT(ax, bx);
          break;
        case 2:
          cond = builder.CreateFCmpOEQ(ax, bx);
          break;
        case 3:
          cond = builder.CreateFCmpOGE(ax, bx);
          break;
        case 4:
          cond = builder.CreateFCmpOLT(ax, bx);
          break;
        case 5:
          cond = builder.CreateFCmpUNE(ax, bx);
          break;
        case 6:
          cond = builder.CreateFCmpOLE(ax, bx);
          break;
        default:
          break;
        }
        if (!cond)
          break;
      }
      auto *next_bb = BasicBlock::Create(context, "break.cont", fn);
      if (cond)
        builder.CreateCondBr(cond, merge_bb, next_bb);
      else
        builder.CreateBr(merge_bb);
      builder.SetInsertPoint(next_bb);
      break;
    }
    case DxsoOpcode::BreakP: {
      // Predicate-driven early loop exit: break when the swizzle-selected
      // p0 lane, after the source modifier, is set. wined3d lowers breakp
      // as a conditional break off the x-swizzled source lane
      // (glsl_shader.c shader_glsl_conditional_op); DXVK carries no breakp
      // arm, so wined3d is the reference. Same open-block discipline as
      // Break above.
      if (loop_stack.empty())
        break;
      if (ins.src_count < 1 || ins.src[0].base.type != DxsoRegisterType::Predicate || ins.src[0].base.num != 0)
        break;
      BasicBlock *merge_bb = loop_stack.back().merge_bb;
      auto *next_bb = BasicBlock::Create(context, "breakp.cont", fn);
      Value *pmask = builder.CreateLoad(bool4Ty, p0_slot);
      Value *cond = builder.CreateExtractElement(pmask, builder.getInt32((int)ins.src[0].swizzle[0]));
      if (ins.src[0].modifier == DxsoRegModifier::Not)
        cond = builder.CreateNot(cond);
      builder.CreateCondBr(cond, merge_bb, next_bb);
      builder.SetInsertPoint(next_bb);
      break;
    }
    // SM 1.x TexM3x{2,3}Pad: literal no-ops. Dependent ops read via
    // register-file lookup, not preserved state. Explicit case avoids spurious warnings.
    case DxsoOpcode::TexDepth: {
      // SM 1.4 TexDepth: depth = clamp(r.x / r.y, 0, 1). The divisor is
      // used as-is: vkd3d's d3dbc texdepth test pins the plain division
      // against native (0.75 / 1.5 must give 0.5; wined3d's GLSL clamps
      // the divisor to 1.0, which native does not do). Division by zero
      // is documented to yield 1.0 and the saturate realises that for
      // the +inf case; hardware itself disagrees on 0 / 0.
      if (is_vertex || !ins.has_dst || oDepth_arg_idx < 0 || !oDepth_slot)
        break;
      auto *gep_r = builder.CreateGEP(tempArrTy, temps, {builder.getInt32(0), builder.getInt32(ins.dst.base.num)});
      Value *r4 = builder.CreateLoad(float4Ty, gep_r);
      Value *rx = builder.CreateExtractElement(r4, builder.getInt32(0));
      Value *ry = builder.CreateExtractElement(r4, builder.getInt32(1));
      Value *depth = builder.CreateFDiv(rx, ry);
      depth = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, depth);
      // oDepth_slot is a float4; lane 0 is what the epilogue extracts
      // for [[depth]]. Splat for stable contents on the unused lanes.
      builder.CreateStore(builder.CreateVectorSplat(4, depth), oDepth_slot);
      break;
    }
    case DxsoOpcode::TexM3x2Depth: {
      // SM 1.3 TexM3x2Depth: re-derive rows, dots against src[0], emit depth.
      // depth = (row1·src == 0) ? 1 : clamp((row0·src)/(row1·src), 0, 1).
      if (is_vertex || !ins.has_dst || ins.src_count < 1 || !tex_inputs || oDepth_arg_idx < 0 || !oDepth_slot)
        break;
      uint32_t slot = ins.dst.base.num;
      if (slot < 1 || slot >= 8)
        break;
      Value *src4 = load_src(ins.src[0]);
      if (!src4)
        break;
      int xyz[3] = {0, 1, 2};
      Value *src3 = builder.CreateShuffleVector(src4, src4, ArrayRef<int>(xyz, 3));
      auto loadRow = [&](uint32_t row_slot) -> Value * {
        auto *gep = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(row_slot)});
        Value *row4 = builder.CreateLoad(float4Ty, gep);
        return builder.CreateShuffleVector(row4, row4, ArrayRef<int>(xyz, 3));
      };
      Value *tmp_x = compute_dot(loadRow(slot - 1), src3, 3);
      Value *tmp_y = compute_dot(loadRow(slot), src3, 3);
      auto *fT = Type::getFloatTy(context);
      Value *zero_f = ConstantFP::get(fT, 0.0f);
      Value *one_f = ConstantFP::get(fT, 1.0f);
      Value *q = builder.CreateFDiv(tmp_x, tmp_y);
      Value *q_sat = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, q);
      Value *y_is_zero = builder.CreateFCmpOEQ(tmp_y, zero_f);
      Value *depth = builder.CreateSelect(y_is_zero, one_f, q_sat);
      builder.CreateStore(builder.CreateVectorSplat(4, depth), oDepth_slot);
      break;
    }
    case DxsoOpcode::TexM3x2Pad:
    case DxsoOpcode::TexM3x3Pad:
      break;
    case DxsoOpcode::EndRep:
    case DxsoOpcode::EndLoop: {
      if (loop_stack.empty())
        break;
      LoopFrame f = loop_stack.back();
      loop_stack.pop_back();
      builder.CreateBr(f.latch_bb);
      builder.SetInsertPoint(f.latch_bb);
      auto *i32Ty = Type::getInt32Ty(context);
      Value *cur = builder.CreateLoad(i32Ty, f.counter_slot);
      Value *next = builder.CreateAdd(cur, builder.getInt32(1));
      builder.CreateStore(next, f.counter_slot);
      if (f.aL_backup_slot) {
        // Loop (not Rep); step aL by stride. Stride may be zero
        // (def-baked or runtime); the unconditional add is fine
        // either way and keeps the IR shape uniform.
        Value *aL = builder.CreateLoad(i32Ty, aL_slot);
        Value *aL_next = builder.CreateAdd(aL, f.aL_stride);
        builder.CreateStore(aL_next, aL_slot);
      }
      builder.CreateBr(f.header_bb);
      builder.SetInsertPoint(f.merge_bb);
      if (f.aL_backup_slot) {
        Value *outer = builder.CreateLoad(i32Ty, f.aL_backup_slot);
        builder.CreateStore(outer, aL_slot);
      }
      break;
    }
    case DxsoOpcode::Label: {
      // Subroutines (call / label / ret) are not implemented, matching
      // DXVK. Stop lowering at the first label so the subroutine bodies
      // that follow it do not execute inline after the main body and
      // clobber its result (the calls to them were already dropped by the
      // default arm). Warn once so a shader that relies on subroutines
      // self-diagnoses instead of rendering subtly wrong.
      static std::atomic<bool> warned_label{false};
      if (!warned_label.exchange(true, std::memory_order_relaxed))
        llvm::errs() << "dxso: subroutines unimplemented; truncating shader at first label\n";
      break;
    }
    default: {
      // Silent fall-through is how "almost-correct shader" symptoms
      // reach the pipeline: an unhandled op is dropped, the resulting
      // metallib compiles fine, the draw runs, output is partially
      // wrong. Emit a one-shot warn per opcode value (low 7 bits
      // cover the entire SM 1.x / 2.x / 3.x table 0..96; the special
      // Phase / Comment / End values 0xFFFD..0xFFFF are caught by
      // their own cases above).
      uint32_t op_val = static_cast<uint32_t>(ins.opcode);
      if (op_val < 128) {
        static std::atomic<uint64_t> warned_lo{0};
        static std::atomic<uint64_t> warned_hi{0};
        auto &slot = (op_val < 64) ? warned_lo : warned_hi;
        uint64_t bit = 1ull << (op_val & 63);
        uint64_t prev = slot.fetch_or(bit, std::memory_order_relaxed);
        if (!(prev & bit)) {
          llvm::errs() << "dxso: unhandled opcode " << op_val << " in " << (is_vertex ? "vs" : "ps") << " shader\n";
        }
      }
      break;
    }
    }
    if (ins.opcode == DxsoOpcode::End || ins.opcode == DxsoOpcode::Label)
      break;
  }

  // SM 1.x PS has no oC# register file; `r0` is the implicit pixel
  // output (D3D9 SDK ps_1_x reference; wined3d glsl_shader.c
  // emits the same `oC0 = R0` copy at the GLSL backend). The body
  // writes `mul r0, ...` through store_dst's Temp arm (temps[0]); the
  // retval assembly below loads from oC_slot[0] which is zero-init
  // unless we copy here. Without this every ps_1_x PS returns
  // (0,0,0,0) regardless of body; the output register must be
  // explicitly seeded from temps[0] after input_arg_idx wiring.
  if (!is_vertex && shader->header.major < 2 && oC_arg_idx[0] >= 0) {
    auto *r0_gep = builder.CreateGEP(tempArrTy, temps, {builder.getInt32(0), builder.getInt32(0)});
    Value *r0 = builder.CreateLoad(float4Ty, r0_gep);
    // The ps_1_x colour output clamps to [0, 1]: register values run
    // wider in flight, but the value leaving the shader saturates
    // (wine's ps1-sampler rows pin 1.2 to one and -0.3 to zero).
    r0 = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, r0);
    builder.CreateStore(r0, oC_slot[0]);
  }

  // D3D9 fog blend: oC0.rgb = mix(fog_color, oC0.rgb, saturate(fog)).
  // Vertex fog reads the factor from the FOG0 varying (VS oFog); the
  // table modes derive it per fragment from the window-space depth
  // (see below). The colour sits in the bool-constant buffer tail,
  // host-written from D3DRS_FOGCOLOR per draw, with FOGSTART/FOGEND/
  // FOGDENSITY following it. Fog precedes the alpha test and blends
  // rgb only, matching wined3d's generated PS epilogue.
  if (emit_fog_blend && oC_arg_idx[0] >= 0 && ps_fog_arg_idx >= 0) {
    auto *fTy = Type::getFloatTy(context);
    auto *u32Ty = Type::getInt32Ty(context);
    auto *bcPtrEarly = fn->getArg(bc_arg_idx);
    // Read a float from the bool-constant blob at the given uint32 index
    // (byte offset = idx * 4). The host packs the table-fog params at
    // index 24/25/26 (FOGSTART/FOGEND/FOGDENSITY), right after the LOD
    // bias array.
    auto load_blob_f = [&](uint32_t idx) -> Value * {
      Value *p = builder.CreateGEP(u32Ty, bcPtrEarly, builder.getInt32((int)idx));
      return builder.CreateBitCast(builder.CreateLoad(u32Ty, p), fTy);
    };
    Value *factor = nullptr;
    if (ps_fog_mode == DXSO_PS_FOG_MODE_SPECULAR_ALPHA && ps_color1_arg_idx >= 0) {
      Value *spec_in = fn->getArg(ps_color1_arg_idx);
      factor = builder.CreateExtractElement(spec_in, builder.getInt32(3));
    } else if (!fog_is_table) {
      Value *fog_in = fn->getArg(ps_fog_arg_idx);
      factor = builder.CreateExtractElement(fog_in, builder.getInt32(0));
    } else {
      // Table-fog coordinate: eye-space w (the reciprocal of Metal's
      // interpolated 1/w) for a typical perspective projection, the
      // vertex-output Z varying otherwise; the host derives the choice from
      // the projection matrix per wined3d's contract and passes it on the
      // fog argument.
      Value *pos = fn->getArg(ps_position_arg_idx);
      Value *depth;
      if (ps_fog_coord_w) {
        Value *pw = builder.CreateExtractElement(pos, builder.getInt32(3));
        depth = builder.CreateFDiv(ConstantFP::get(fTy, 1.0), pw);
      } else {
        // Vertex-output Z: the interpolated FOG0.y varying the VS wrote
        // (oPos.z / clip_pos.z), not the fragment [[position]].z. The device
        // Z is post-perspective and carries the rasterizer depth bias on
        // Apple GPUs; wined3d fogs the ortho / pre-transformed path against
        // gl_Position.z / ec_pos.z (glsl_shader.c), which this varying is.
        depth = builder.CreateExtractElement(fn->getArg(ps_fog_arg_idx), builder.getInt32(1));
      }
      Value *fog_start = load_blob_f(24);
      Value *fog_end = load_blob_f(25);
      Value *fog_density = load_blob_f(26);
      if (ps_fog_mode == DXSO_PS_FOG_MODE_LINEAR) {
        // (end - depth) / (end - start), matching wined3d's LINEAR and
        // DXVK's (end - d) * fogScale (fogScale = 1/(end-start)).
        Value *num = builder.CreateFSub(fog_end, depth);
        Value *den = builder.CreateFSub(fog_end, fog_start);
        factor = builder.CreateFDiv(num, den);
      } else {
        // wined3d/DXVK use exp(); air exposes exp2, so fold log2(e) into
        // the exponent: exp(x) = exp2(x * LOG2E).
        constexpr double kLog2E = 1.4426950408889634;
        Value *log2e = ConstantFP::get(fTy, kLog2E);
        Value *dd = builder.CreateFMul(depth, fog_density);
        Value *exponent;
        if (ps_fog_mode == DXSO_PS_FOG_MODE_EXP2) {
          // EXP2: exp(-(density*depth)^2).
          exponent = builder.CreateFMul(dd, dd);
        } else {
          // EXP: exp(-density*depth).
          exponent = dd;
        }
        Value *neg = builder.CreateFNeg(builder.CreateFMul(exponent, log2e));
        factor = air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, neg);
      }
    }
    factor = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, factor);
    Value *out_color = builder.CreateLoad(float4Ty, oC_slot[0]);
    Value *fogged = out_color;
    for (uint32_t lane = 0; lane < 3; ++lane) {
      Value *cPtr = builder.CreateGEP(u32Ty, bcPtrEarly, builder.getInt32(4 + lane));
      Value *cBits = builder.CreateLoad(u32Ty, cPtr);
      Value *fog_c = builder.CreateBitCast(cBits, fTy);
      Value *c = builder.CreateExtractElement(out_color, builder.getInt32(lane));
      // mix(fog_c, c, factor) = fog_c + (c - fog_c) * factor
      Value *blended = builder.CreateFAdd(fog_c, builder.CreateFMul(builder.CreateFSub(c, fog_c), factor));
      fogged = builder.CreateInsertElement(fogged, blended, builder.getInt32(lane));
    }
    builder.CreateStore(fogged, oC_slot[0]);
  }

  // D3D9 alpha test, lowered to discard_fragment per wined3d's GLSL
  // backend (dlls/wined3d/glsl_shader.c shader_glsl_generate_alpha_
  // test): "alpha_func is the PASS condition"; we compare oC0.a
  // against ALPHAREF/255.0 with the D3DCMP_* predicate, then discard
  // when the test fails. Runs after the body has finished writing
  // oC0 and before retval assembly so an alpha-failing fragment never
  // makes it into the PSO output. NEVER ⇒ unconditional discard;
  // ALWAYS is filtered out by emit_alpha_test above.
  if (emit_alpha_test && oC_arg_idx[0] >= 0) {
    auto *cont_bb = BasicBlock::Create(context, "alpha_test.cont", fn);
    auto *kill_bb = BasicBlock::Create(context, "alpha_test.kill", fn);
    if (ps_args->alpha_test_func == 1 /* D3DCMP_NEVER */) {
      builder.CreateBr(kill_bb);
    } else {
      Value *out_color = builder.CreateLoad(float4Ty, oC_slot[0]);
      Value *alpha = builder.CreateExtractElement(out_color, builder.getInt32(3));
      // The ref rides the shared PS uniform tail (buffer(2) uint32 index 28),
      // host-written as D3DRS_ALPHAREF / 255 per draw, so distinct refs share
      // one metallib instead of baking a per-value immediate. The compare
      // itself is unchanged, so a fixed ref discards identically.
      auto *u32Ty = Type::getInt32Ty(context);
      Value *ref_ptr = builder.CreateGEP(u32Ty, fn->getArg(bc_arg_idx), builder.getInt32(28));
      Value *ref = builder.CreateBitCast(builder.CreateLoad(u32Ty, ref_ptr), Type::getFloatTy(context));
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
        pass = builder.CreateFCmpUNE(alpha, ref);
        break;
      case 7 /* D3DCMP_GREATEREQUAL */:
        pass = builder.CreateFCmpOGE(alpha, ref);
        break;
      default:
        // Invalid compare func kills every fragment (both refs' DecodeCompareOp
        // default is NEVER); the device normalizes garbage before keying, so
        // this is the airconv-direct safety net.
        pass = ConstantInt::getFalse(context);
        break;
      }
      // pass=true → continue; pass=false → discard.
      builder.CreateCondBr(pass, cont_bb, kill_bb);
    }
    builder.SetInsertPoint(kill_bb);
    air.CreateDiscard();
    builder.CreateBr(cont_bb);
    builder.SetInsertPoint(cont_bb);
  }

  auto *retTy = fn->getReturnType();
  if (retTy->isVoidTy()) {
    builder.CreateRetVoid();
  } else {
    Value *retval = UndefValue::get(retTy);
    if (is_vertex) {
      Value *pos = builder.CreateLoad(float4Ty, out_slot);
      if (position_transformed) {
        // Window-space (POSITIONT/XYZRHW) -> clip space, then the perspective
        // rhw setup, matching wined3d/DXVK's fixed-function pre-transform path.
        // invExtent/invOffset are the host-packed viewport remap (location 5);
        // the rhw = 1/w divide (guarding w==0) scales xyz and replaces w so the
        // GPU's perspective divide lands xy at the screen position.
        auto *posFTy = Type::getFloatTy(context);
        auto *vpPtr = fn->getArg(vp_remap_arg_idx);
        auto *invExtent = builder.CreateLoad(float4Ty, vpPtr);
        auto *invOffGep = builder.CreateGEP(float4Ty, vpPtr, builder.getInt32(1));
        auto *invOffset = builder.CreateLoad(float4Ty, invOffGep);
        pos = builder.CreateFAdd(builder.CreateFMul(pos, invExtent), invOffset);
        Value *w = builder.CreateExtractElement(pos, builder.getInt32(3));
        Value *isZero = builder.CreateFCmpOEQ(w, ConstantFP::get(posFTy, 0.0));
        Value *rhw = builder.CreateSelect(
            isZero, ConstantFP::get(posFTy, 1.0), builder.CreateFDiv(ConstantFP::get(posFTy, 1.0), w)
        );
        pos = builder.CreateFMul(pos, builder.CreateVectorSplat(4, rhw));
        pos = builder.CreateInsertElement(pos, rhw, builder.getInt32(3));
      }
      retval = builder.CreateInsertValue(retval, pos, {0});
      // User clip planes: emit clip_distance[i] = (i < clip_count)
      //   ? dot(planes[i], pos) : 0.0
      // for the 8 plane slots. The host packs enabled planes
      // consecutively into the buffer; clip_count is the popcount of
      // D3DRS_CLIPPLANEENABLE. dot is computed lane-wise rather than
      // through an intrinsic so the IR stays at the same level the
      // rest of compile_dxso uses (LLVM optimisation passes coalesce
      // the four FMul + three FAdd into a single fdot).
      auto *fTy = Type::getFloatTy(context);
      auto *i32Ty = Type::getInt32Ty(context);
      auto *cdArrTy = ArrayType::get(fTy, 8);
      Value *cdArr = UndefValue::get(cdArrTy);
      auto *cpPtr = fn->getArg(clip_planes_arg_idx);
      auto *ccPtr = fn->getArg(clip_count_arg_idx);
      Value *count = builder.CreateLoad(i32Ty, ccPtr);
      Value *zero_f = ConstantFP::get(fTy, 0.0);
      for (uint32_t i = 0; i < 8; ++i) {
        auto *gep = builder.CreateGEP(float4Ty, cpPtr, builder.getInt32(i));
        Value *plane = builder.CreateLoad(float4Ty, gep);
        Value *prod = builder.CreateFMul(plane, pos);
        Value *x = builder.CreateExtractElement(prod, builder.getInt32(0));
        Value *y = builder.CreateExtractElement(prod, builder.getInt32(1));
        Value *z = builder.CreateExtractElement(prod, builder.getInt32(2));
        Value *w = builder.CreateExtractElement(prod, builder.getInt32(3));
        Value *xy = builder.CreateFAdd(x, y);
        Value *zw = builder.CreateFAdd(z, w);
        Value *dot = builder.CreateFAdd(xy, zw);
        Value *enabled = builder.CreateICmpULT(builder.getInt32(i), count);
        Value *lane = builder.CreateSelect(enabled, dot, zero_f);
        cdArr = builder.CreateInsertValue(cdArr, lane, {i});
      }
      retval = builder.CreateInsertValue(retval, cdArr, {clip_dist_field_idx});
    } else {
      // PS: each oC# slot lands at the field DefineOutput assigned
      // in pre-scan order. oC_arg_idx[N] gives the struct index. An oC# whose
      // attachment bit is set in the unorm-snap mask (the host resolved a LINEAR
      // 8-bit unorm target for it) is snapped to the nearest k/255 with
      // round-half-to-even so Metal's unorm write matches WARP's byte; float /
      // HDR / sRGB attachments leave their bit clear and keep full precision.
      const uint32_t unorm_snap = ps_args != nullptr ? ps_args->unorm_output_reg_mask : 0u;
      for (int i = 0; i < 4; ++i) {
        if (oC_arg_idx[i] < 0)
          continue;
        Value *v = builder.CreateLoad(float4Ty, oC_slot[i]);
        if ((unorm_snap >> i) & 1u)
          v = emit_unorm8_snap(air, builder, v);
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oC_arg_idx[i]});
      }
      if (oDepth_arg_idx >= 0) {
        auto *v4 = builder.CreateLoad(float4Ty, oDepth_slot);
        Value *d = builder.CreateExtractElement(v4, builder.getInt32(0));
        retval = builder.CreateInsertValue(retval, d, {(unsigned)oDepth_arg_idx});
      }
      if (cov_arg_idx >= 0) {
        // D3DRS_MULTISAMPLEMASK: the app mask is the coverage bitmask itself
        // (blob tail uint32 index 29, so no bitcast). SM1-3 PS emits no
        // coverage of its own, so this is the whole [[sample_mask]] output.
        Value *mask_ptr = builder.CreateGEP(builder.getInt32Ty(), fn->getArg(bc_arg_idx), builder.getInt32(29));
        Value *mask_u32 = builder.CreateLoad(builder.getInt32Ty(), mask_ptr);
        retval = builder.CreateInsertValue(retval, mask_u32, {(unsigned)cov_arg_idx});
      }
    }
    if (sm12_vs_varyings) {
      for (int i = 0; i < 2; ++i) {
        if (oD_arg_idx[i] < 0)
          continue;
        Value *v = builder.CreateLoad(float4Ty, oD_slot[i]);
        // The dedicated color outputs clamp to [0, 1] below shader
        // model 3 (DXVK saturates at the same point; wine's
        // color-clamping rows pin the boundary).
        v = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, v);
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oD_arg_idx[i]});
      }
      for (int i = 0; i < 8; ++i) {
        if (oT_arg_idx[i] < 0)
          continue;
        auto *v = builder.CreateLoad(float4Ty, oT_slot[i]);
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oT_arg_idx[i]});
      }
      if (oFog_arg_idx >= 0) {
        Value *v = builder.CreateLoad(float4Ty, oFog_slot);
        // FOG0.y carries the table (pixel) fog coordinate: the vertex-output
        // Z (oPos.z), read by a pre-SM3 pixel shader's fog epilogue for the
        // non-w (ortho / pre-transformed) path. Mirrors wined3d's VS_FOG_Z =
        // gl_Position.z (glsl_shader.c). FOG0.x stays the vertex-fog factor.
        Value *oPosZ = builder.CreateExtractElement(builder.CreateLoad(float4Ty, out_slot), builder.getInt32(2));
        v = builder.CreateInsertElement(v, oPosZ, builder.getInt32(1));
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oFog_arg_idx});
      }
    } else if (sm3_vs_outputs) {
      // Position aliases out_slot and is already covered at field 0;
      // only varyings (oN_arg_idx >= 0) need a struct insert. PointSize
      // (oN_is_pointsize) is emitted via the oPts_arg_idx block below
      // since it's a scalar, not a float4.
      for (int i = 0; i < 16; ++i) {
        if (oN_arg_idx[i] < 0)
          continue;
        auto *v = builder.CreateLoad(float4Ty, oN_slot[i]);
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oN_arg_idx[i]});
      }
      // Stub interpolants ride the oD/oT/oFog arrays (see the SM3
      // signature tail); insert their seeded slots too.
      for (int i = 0; i < 2; ++i) {
        if (oD_arg_idx[i] < 0)
          continue;
        auto *v = builder.CreateLoad(float4Ty, oD_slot[i]);
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oD_arg_idx[i]});
      }
      for (int i = 0; i < 8; ++i) {
        if (oT_arg_idx[i] < 0)
          continue;
        auto *v = builder.CreateLoad(float4Ty, oT_slot[i]);
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oT_arg_idx[i]});
      }
      if (oFog_arg_idx >= 0) {
        auto *v = builder.CreateLoad(float4Ty, oFog_slot);
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oFog_arg_idx});
      }
    }
    if (oPts_arg_idx >= 0 && oPts_slot) {
      // [[point_size]] is a scalar float: extract lane 0 from the
      // float4 storage slot (store_dst's plumbing writes the same
      // value to all four lanes for a scalar write like `mov oPts, c0.x`).
      auto *v4 = builder.CreateLoad(float4Ty, oPts_slot);
      Value *ps = builder.CreateExtractElement(v4, builder.getInt32(0));
      if (vs_inject_point_size && point_size_arg_idx != ~0u) {
        // clamp(size-or-shader-oPts, min, max) against the D3DRS_POINTSIZE_MIN
        // / _MAX bounds the uniform carries, matching the point-size clamp in
        // DXVK's emitLinkerOutputSetup (GetPointSizeInfoVS). The
        // seed above already sourced the render-state size, so a shader that
        // never writes oPts clamps that; one that does clamps its own value.
        Value *u = builder.CreateLoad(float4Ty, fn->getArg(point_size_arg_idx));
        Value *mn = builder.CreateExtractElement(u, builder.getInt32(1));
        Value *mx = builder.CreateExtractElement(u, builder.getInt32(2));
        ps = air.CreateFPBinOp(llvm::air::AIRBuilder::fmax, ps, mn);
        ps = air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, ps, mx);
      }
      retval = builder.CreateInsertValue(retval, ps, {(unsigned)oPts_arg_idx});
    }
    builder.CreateRet(retval);
  }

  module.getOrInsertNamedMetadata(is_vertex ? "air.vertex" : "air.fragment")->addOperand(fn_md);
}

DxsoShader *
dxso_shader_initialize(const void *bytecode, size_t bytecode_size) {
  if (!bytecode || bytecode_size < sizeof(uint32_t) || bytecode_size % sizeof(uint32_t) != 0)
    return nullptr;

  const uint32_t *words = static_cast<const uint32_t *>(bytecode);
  uint32_t word_count = static_cast<uint32_t>(bytecode_size / sizeof(uint32_t));

  auto header = parse_dxso_header(words, word_count);
  if (!header)
    return nullptr;

  // Compile-time walk: the create-time walk in d3d9.dll already applied the
  // per-device float ceiling (256 on hardware-VP, rejecting a def c256), so
  // any bytecode that reaches here is already valid for its device. Widen the
  // cap to the software-VP maximum so a legitimately-created c256+ vertex
  // shader is not rejected a second time (which would drop it to a null
  // function and render black).
  auto metadata = walk_dxso_shader(words, word_count, *header, kDxsoMaxVsFloatConstSWVP);
  if (!metadata)
    return nullptr;

  auto *shader = new (std::nothrow) DxsoShader();
  if (!shader)
    return nullptr;
  shader->bytecode.assign(words, words + word_count);
  shader->header = *header;
  shader->metadata = std::move(*metadata);
  return shader;
}

void
dxso_shader_destroy(DxsoShader *shader) {
  delete shader;
}

} // namespace dxmt

extern "C" {

AIRCONV_API int
DXSOInitialize(const void *pBytecode, size_t BytecodeSize, dxso_shader_t *ppShader) {
  if (!ppShader)
    return -1;
  // sm50_ptr64_t is a 64-bit-fixed handle that's `void *` on 64-bit
  // builds and a wrapper struct with a `void *` constructor on 32-bit.
  // Both let us assign from a raw pointer through the constructor;
  // clearing it on early-out uses the same path.
  *ppShader = (void *)nullptr;
  auto *shader = dxmt::dxso_shader_initialize(pBytecode, BytecodeSize);
  if (!shader)
    return -1;
  *ppShader = (void *)shader;
  return 0;
}

AIRCONV_API void
DXSODestroy(dxso_shader_t pShader) {
  // Mirrors SM50Destroy (dxbc_converter.cpp): C-style cast
  // back to the implementation type. On 32-bit this routes through
  // sm50_ptr64_t::operator uint64_t().
  delete (dxmt::DxsoShader *)pShader;
}

AIRCONV_API int
DXSOCompile(
    dxso_shader_t pShader, struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName,
    dxso_bitcode_t *ppBitcode
) {
  using namespace llvm;
  if (!ppBitcode)
    return -1;
  *ppBitcode = (void *)nullptr;
  if (!FunctionName)
    return -1;

  // Walk the argument chain. Recognised arg types: IA layout (VS),
  // PSO alpha-test (PS), PS sampler-kind layout (PS), PS point-sprite
  // (PS). Unknown types are silently skipped; same forgiveness
  // contract SM50 uses.
  DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args = nullptr;
  DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *ps_samp_layout = nullptr;
  bool ps_point_sprite = false;
  int ps_fog_mode = -1; // -1 = no fog arg in the chain
  bool ps_fog_coord_w = false;
  bool vs_inject_point_size = false;
  DXSO_SHADER_FFP_KEY_DATA *ffp_key = nullptr;
  for (auto *arg = pArgs; arg; arg = (DXSO_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next) {
    switch (arg->type) {
    case DXSO_SHADER_IA_INPUT_LAYOUT:
      ia_layout = (DXSO_SHADER_IA_INPUT_LAYOUT_DATA *)arg;
      break;
    case DXSO_SHADER_PSO_PIXEL_SHADER:
      ps_args = (DXSO_SHADER_PSO_PIXEL_SHADER_DATA *)arg;
      break;
    case DXSO_SHADER_PS_SAMPLER_LAYOUT:
      ps_samp_layout = (DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *)arg;
      break;
    case DXSO_SHADER_PS_POINT_SPRITE:
      ps_point_sprite = true;
      break;
    case DXSO_SHADER_VS_POINT_SIZE:
      vs_inject_point_size = true;
      break;
    case DXSO_SHADER_PS_BUMP_ENV:
      // Reserved: the TexBem bump-env constants now ride the shared PS
      // uniform tail (buffer(2)), so no arg carries them.
      break;
    case DXSO_SHADER_PS_FOG:
      ps_fog_mode = (int)((DXSO_SHADER_PS_FOG_DATA *)arg)->mode;
      ps_fog_coord_w = ((DXSO_SHADER_PS_FOG_DATA *)arg)->coord_is_w != 0;
      break;
    case DXSO_SHADER_FFP_KEY:
      ffp_key = (DXSO_SHADER_FFP_KEY_DATA *)arg;
      break;
    case DXSO_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
  }
  // A fixed-function request carries no bytecode: the key + IA layout
  // fully describe the generated shader. Everything else requires the
  // parsed shader handle.
  if (!pShader && !ffp_key)
    return -1;

  LLVMContext context;
  context.setOpaquePointers(false);
  auto module = std::make_unique<Module>("dxso.air", context);
  dxmt::initializeModule(*module);
  if (ffp_key)
    dxmt::compile_ffp(ffp_key, ia_layout, ps_args, ps_fog_mode, ps_fog_coord_w, FunctionName, context, *module);
  else
    dxmt::compile_dxso(
        (dxmt::DxsoShader *)pShader, ia_layout, ps_args, ps_samp_layout, ps_point_sprite, vs_inject_point_size,
        ps_fog_mode, ps_fog_coord_w, FunctionName, context, *module
    );

  auto *compiled = new (std::nothrow) dxmt::DxsoBitcode();
  if (!compiled)
    return -1;
  llvm::raw_svector_ostream os(compiled->bytes);
  dxmt::metallib::MetallibWriter writer;
  writer.Write(*module, os);

  // Env-gated metallib dump: point DXMT_AIRCONV_DUMP at a directory
  // and every DXSOCompile call drops a {kind}_{hash}.metallib there.
  // Disassemble with `xcrun air-objdump -d <file>` to inspect what
  // AGX actually receives. Only meant for triage of XPC link
  // failures; off by default.
  if (const char *dump_dir = std::getenv("DXMT_AIRCONV_DUMP")) {
    if (dump_dir[0]) {
      static std::atomic<uint64_t> counter{0};
      uint64_t n = counter.fetch_add(1, std::memory_order_relaxed);
      std::string path = std::string(dump_dir) + "/" + FunctionName + "_" + std::to_string(n) + ".metallib";
      if (FILE *fp = std::fopen(path.c_str(), "wb")) {
        std::fwrite(compiled->bytes.data(), 1, compiled->bytes.size(), fp);
        std::fclose(fp);
      }
    }
  }

  *ppBitcode = (void *)compiled;
  return 0;
}

AIRCONV_API void
DXSOGetCompiledBitcode(dxso_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData) {
  auto *bc = (dxmt::DxsoBitcode *)pBitcode;
  if (!bc || !pData)
    return;
  pData->Data = bc->bytes.data();
  pData->Size = bc->bytes.size();
}

AIRCONV_API void
DXSODestroyBitcode(dxso_bitcode_t pBitcode) {
  delete (dxmt::DxsoBitcode *)pBitcode;
}

} // extern "C"
