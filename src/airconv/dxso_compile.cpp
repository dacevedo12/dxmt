#include "dxso_compile.hpp"

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

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <string>

namespace dxmt {

// TODO: d3d9's CreateVertexShader / CreatePixelShader still call
// parse_dxso_header + walk_dxso_shader directly to validate the blob
// before constructing the COM wrapper. Once DXSOCompile lands and
// d3d9 routes through DXSOInitialize for the validation+walk, the
// MTLD3D9*Shader subclasses should hold a dxso_shader_t handle
// instead of their current bytecode + metadata copy — single source
// of truth, no double walk. Tracked here so it doesn't get lost.
// Emit the placeholder DXSO entry into an externally-owned Module.
// Same role dxbc::convertDXBC plays for DXBC; airconv_cli drives both
// through the same optimization + linkXxx + output flow.
void
compile_dxso(
    DxsoShader *shader, const ::DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout,
    const ::DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args, const ::DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *ps_samp_layout,
    bool ps_point_sprite, float vs_point_size_override, const ::DXSO_SHADER_PS_BUMP_ENV_DATA *ps_bump_env,
    const char *name, llvm::LLVMContext &context, llvm::Module &module
) {
  using namespace llvm;
  const bool is_vertex = shader->header.kind == DxsoShaderKind::Vertex;
  // Manual fetch is VS-only — even if the host hands a layout for a PS
  // (it shouldn't), the lowering below would have nothing to do.
  const bool manual_fetch = is_vertex && ia_layout != nullptr;
  // Alpha test is PS-only and only relevant when the host explicitly
  // opts in — passing nullptr (or D3DCMP_ALWAYS) means no test snippet
  // is emitted, so the unspecialised PS keeps its current shape.
  const bool emit_alpha_test = !is_vertex && ps_args != nullptr && ps_args->alpha_test_func != 8 /* D3DCMP_ALWAYS */;

  // air.version / air.language_version: AGX rejects PS metallibs
  // missing these. Mirrors dxbc::setup_metal_version's
  // SM50_SHADER_METAL_320 arm.
  auto u32_md = [&](uint32_t v) { return ConstantAsMetadata::get(ConstantInt::get(context, APInt{32, v})); };
  module.setTargetTriple("air64-apple-macosx15.0.0");
  module.getOrInsertNamedMetadata("air.version")->addOperand(MDTuple::get(context, {u32_md(2), u32_md(7), u32_md(0)}));
  module.getOrInsertNamedMetadata("air.language_version")
      ->addOperand(MDTuple::get(context, {MDString::get(context, "Metal"), u32_md(3), u32_md(2), u32_md(0)}));

  air::FunctionSignatureBuilder sig;

  // VS inputs: one [[stage_in]] attribute per dcl_<usage> v# — the
  // attribute index is the v# register number, host vertex descriptor
  // binds against it. PS inputs: InputFragmentStageIn keyed on a
  // user-name, and the matching VS output uses the same string. v#
  // (Input register file) and t# (Texture register file, SM2 / SM 1.4
  // PS) live in separate input arrays so the body's load_src can
  // route by register file directly.
  std::array<int, 16> input_arg_idx;
  input_arg_idx.fill(-1);
  // Parallel to input_arg_idx: true when v<N> was dcl'd as TEXCOORD.
  // Drives the POINTSPRITEENABLE per-input substitution at tex_inputs
  // load time — SM3 PS reads texcoords via v# (input_arg_idx) rather
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

  // PS signature head — InputPosition lands first so it occupies struct
  // field 0, even when no dcl_position v# is present. AGX rejects PS
  // metallibs that interleave [[position]] with user varyings; the
  // mismatched field order surfaces as
  // XPC_ERROR_CONNECTION_INTERRUPTED at PSO link time with no metallib
  // diagnostic. The arg index is captured so a SM3 `dcl_position v#`
  // can reuse it for v# loading instead of double-defining.
  int ps_position_arg_idx = -1;
  // Set of PS user-input names already claimed by an explicit dcl —
  // used below to avoid emitting a duplicate stub for the same name.
  std::array<bool, 8> ps_texcoord_used{};
  std::array<bool, 2> ps_color_used{};
  bool ps_fog_used = false;
  if (!is_vertex) {
    ps_position_arg_idx = (int)sig.DefineInput(
        air::InputPosition{
            .interpolation = air::Interpolation::center_no_perspective,
        }
    );
  }

  // SM 1.0..1.3 PS, SM 2.0 PS and SM 1.4 PS all share the same
  // legacy-PS linkage rule: input semantics come from the register
  // file, not from the dcl token's usage field. FXC pins every dcl on
  // SM<3 PS to usage=POSITION/0, so reading dcl.usage as truth lands
  // every t# / v# on [[position]]. SM 1.0..1.3 has no t# stage-in —
  // those slots get filled by the `tex t#` opcode at runtime — but
  // SM 1.4 and SM 2.0 do, and SM 3.0 carries semantic on the dcl
  // token directly. (Mirrors DXVK src/dxso/dxso_compiler.cpp:1744-1754
  // emitDcl.)
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
    // and t# but leave dcl.usage unreliable — register-file is the
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
      // InputPosition emitted at the signature head — a second
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
    default:
      break; // BlendWeight/Normal/etc — skip
    }
    if (!base)
      continue;
    int arg_idx = (int)sig.DefineInput(
        air::InputFragmentStageIn{
            .user = std::string(base) + std::to_string(effective_index),
            .type = air::msl_float4,
            .interpolation = air::Interpolation::center_perspective,
            .pull_mode = false,
        }
    );
    if (is_v) {
      input_arg_idx[d.bound_to.num] = arg_idx;
      if (effective_usage == DxsoUsage::Texcoord)
        ps_v_is_texcoord[d.bound_to.num] = true;
    } else
      ps_tex_arg_idx[d.bound_to.num] = arg_idx;
    if (effective_usage == DxsoUsage::Color && effective_index < ps_color_used.size())
      ps_color_used[effective_index] = true;
    else if (effective_usage == DxsoUsage::Texcoord && effective_index < ps_texcoord_used.size())
      ps_texcoord_used[effective_index] = true;
    else if (effective_usage == DxsoUsage::Fog && effective_index == 0)
      ps_fog_used = true;
  }

  // PS signature tail — claim every varying name a paired VS could
  // produce so AGX's PSO link finds a matching PS input for each VS
  // output. Without these stubs, a VS that writes oT5 against a PS
  // that only dcls v0..v3 fails with XPC_ERROR_CONNECTION_INTERRUPTED
  // at link time. The set is the SM1/2/3 maximum: COLOR0..1
  // (from oD#), TEXCOORD0..7 (from oT#), FOG0 (from RasterizerOut[1]).
  // The arg indices aren't tracked into input_arg_idx — the body
  // never reads them, they exist only so their user-names appear in
  // the function signature.
  if (!is_vertex) {
    auto define_stub = [&](const std::string &user) {
      return (int)sig.DefineInput(
          air::InputFragmentStageIn{
              .user = user,
              .type = air::msl_float4,
              .interpolation = air::Interpolation::center_perspective,
              .pull_mode = false,
          }
      );
    };
    // SM 1.0..1.3 PS has no dcl tokens — the dcls walk above never
    // populates input_arg_idx[] or ps_tex_arg_idx[]. The body still
    // reads v0/v1 (= COLOR0/1) and t0..t7 (= TEXCOORD0..7) by register-
    // file convention; without binding those slots to the stage-in
    // args defined here, every v#/t# read returns ConstantAggregateZero
    // and any "modulate by vertex color" PS produces solid black.
    // Drove the NFS:MW black-screen — a ps_1_1 shader that does
    // "mul r0, tex t0, v0" produces 0 because v0 reads as zero.
    bool sm1_legacy_ps = shader->header.major == 1 && shader->header.minor < 4;
    for (int i = 0; i < 2; ++i) {
      if (!ps_color_used[i]) {
        int arg_idx = define_stub("COLOR" + std::to_string(i));
        if (sm1_legacy_ps && i < (int)input_arg_idx.size())
          input_arg_idx[i] = arg_idx;
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
      define_stub("FOG0");
  }

  // PS point-sprite [[point_coord]] input. When ps_point_sprite is on
  // (host has bound a point-list primitive with D3DRS_POINTSPRITEENABLE),
  // every TEXCOORD<N> stage_in read at tex_inputs init time gets
  // substituted with float4(point_coord.xy, 0, 1). The substitution is
  // unconditional across all 8 texcoord slots per D3D9 spec (no per-
  // stage D3DTSS_TEXCOORDINDEX gating for POINTSPRITEENABLE).
  int ps_point_coord_arg_idx = -1;
  if (!is_vertex && ps_point_sprite) {
    ps_point_coord_arg_idx = (int)sig.DefineInput(air::InputPointCoord{});
  }

  // Manual-fetch VS extras — vertex_buffers table at [[buffer(16)]]
  // (struct-array of {device char *base, i32 stride, i32 length}, one
  // entry per active stream slot, indexed by popcount of the slot mask
  // below the element's slot bit), plus the [[vertex_id]] /
  // [[base_vertex]] system inputs the prologue uses to compute the
  // per-element byte offset. Mirrors pull_vertex_input in
  // dxbc_converter_basicblock.cpp:375 (which sources the same struct
  // layout via AirType::_dxmt_vertex_buffer_entry).
  uint32_t vbuf_table_arg_idx = 0;
  uint32_t vid_arg_idx = 0;
  uint32_t base_vertex_arg_idx = 0;
  uint32_t instance_id_arg_idx = 0;
  uint32_t base_instance_arg_idx = 0;
  if (manual_fetch) {
    // array_size = 1 (not 0) — same rule the texture/sampler bindings
    // follow per project_agx_pipeline_link_traps trap #3. xcrun metal's
    // air.location_index for non-array buffer bindings emits
    // <location, 1>; AGX reads the second integer as a binding count
    // and rejects 0 at PSO link with XPC_ERROR_CONNECTION_INTERRUPTED.
    // Same fix applies to every ArgumentBindingBuffer site below.
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

  // Constant register files — c#, i#, b#. DXVK packs all three into
  // one binding via a struct (src/dxso/dxso_compiler.cpp:264
  // emitDclConstantBuffer). dxmt splits each register file across
  // its own [[buffer(N)]] binding to match the pre-existing dxmt
  // convention (the `c` binding was already its own slot). A DXVK-
  // shaped single struct is expressible through air's
  // ArgumentBindingIndirectBuffer but would be a wider refactor of
  // both signature emission and host upload; the split shape pays
  // an extra setVertex/Fragment Buffer call per draw which is
  // negligible compared to the per-draw newBuffer cost we already
  // eat for c.
  //
  // Host contract — drawCommonInScene must bind, per shader stage:
  //   * [[buffer(0)]] : float4 * — c0..c255 (VS) or c0..c223 (PS)
  //   * [[buffer(1)]] : int4 *   — i0..i15
  //   * [[buffer(2)]] : uint *   — single uint32 packing b0..b15 (bit i = b#i)
  //
  // SetVertexShaderConstant{F,I,B} / SetPixelShaderConstant{F,I,B}
  // populate the storage on the host; def-baked literals (def c#,
  // defi i#, defb b#) still take precedence in load_src / If /
  // Rep / Loop and resolve at codegen time.
  //
  // Coverage: vs_2_0/3_0 (c0..c255, i0..i15, b0..b15) and
  // ps_2_0/3_0 (c0..c223, i0..i15, b0..b15). SWVP (vs_3_0 with
  // 8192 float4s) and ps_1_x (c0..c7) need their own limit
  // handling and aren't covered in this commit.
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
  // Integer constant buffer — i0..i15. Read by Rep / Loop for the
  // count / init / stride payload and by load_src for general
  // ConstInt operands (rare outside loops). Sized for 16 entries
  // unconditionally — D3D9 caps i# at 16 across all SM2/SM3
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
  // Bool bitmask — single uint32 packing b0..b15 with bit i = b#i.
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

  // VS-only user clip plane bindings. Host pre-packs the enabled
  // planes consecutively (DXVK shape, src/d3d9/d3d9_device.cpp:6220
  // UpdateClipPlanes) so the shader's "i < clip_plane_count" gate
  // matches the host write order — apps may toggle planes individually
  // through D3DRS_CLIPPLANEENABLE, but the shader sees them packed.
  //
  // Disabled planes write 0.0 into clip_distance[i]; the GPU clip rule
  // is "clip if any clip_distance < 0", and 0.0 passes the test, so
  // disabled planes are no-ops. PS does not consume these bindings —
  // the slots stay free at locations 3/4 for the fragment stage.
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

  // PS texture + sampler bindings — one [[texture(N)]] / [[sampler(N)]]
  // pair per `dcl_<type> s#` the shader actually reads via Tex. The
  // pre-scan over Tex usages keeps the signature minimal: a sampler
  // dcl'd but never sampled produces no binding, so unused
  // [[texture]] slots don't bloat the PSO.
  //
  // Coverage in this commit is the binding ABI only — Tex opcode
  // lowering lands in a follow-up. The bindings appear in the
  // metallib in advance so the host upload path can be staged
  // independently.
  //
  // Texture-type filter: 2D, Cube and 3D are bound. Samplers with no
  // dcl (Unknown) produce no binding here AND will be a no-op in the
  // Tex opcode lowering — keep the two halves in lockstep. Widening
  // this gate without a matching opcode-lowering arm produces
  // signature args the body never reads (legal but useless).
  // Host contract: [[texture(N)]] / [[sampler(N)]] at slot N for each
  // PS sampler N the shader actually samples.
  std::array<int, 16> tex_arg_idx{};
  tex_arg_idx.fill(-1);
  std::array<int, 16> samp_arg_idx{};
  samp_arg_idx.fill(-1);
  std::array<DxsoTextureType, 16> samp_kind{};
  samp_kind.fill(DxsoTextureType::Unknown);
  if (!is_vertex) {
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
      // SM2+: texld / texldl / texldd dst, coord, sampler[, ddx, ddy].
      // Sampler is in src[1] for all three. SM1.x: tex t# — sampler is
      // implicit, slot matches dst.base.num. The SM1.x decoder leaves
      // src_count=0 and dst.base.type=Texture.
      bool is_sampling =
          tins.opcode == DxsoOpcode::Tex || tins.opcode == DxsoOpcode::TexLdl || tins.opcode == DxsoOpcode::TexLdd;
      if (!is_sampling)
        continue;
      uint32_t slot = UINT32_MAX;
      if (tins.src_count >= 2 && tins.src[1].base.type == DxsoRegisterType::Sampler)
        slot = tins.src[1].base.num;
      else if (tins.has_dst && tins.dst.base.type == DxsoRegisterType::Texture)
        slot = tins.dst.base.num;
      if (slot < 16)
        used[slot] = true;
    }
    // Host-provided per-stage sampler-kind layout (DXSO_SHADER_PS_SAMPLER_LAYOUT).
    // Authoritative when present: the host has the actual bound texture
    // type and Metal's PSO is built from this — the sampler in the
    // function signature must match what setFragmentTexture hands in.
    //
    // We previously honoured this only for shader slots the bytecode left
    // Unknown (SM 1.0..1.3 PS has no `dcl_2d` / `dcl_cube` / `dcl_volume`
    // tokens), thinking SM 1.4+/2.0+ dcl-bearing shaders would always be
    // bound to matching texture types. NFS:MW disproved this: smoke /
    // particle SM 1.4+ PS declare `dcl_2d s0` but the engine binds a
    // TextureCube to the slot for an env-map effect. Trusting the dcl
    // produced a 2D sampler in MSL; Metal saw a Cube view bound to a 2D
    // sampler and failed validation, with the visible effect being
    // single-texel "flake" patterns plus per-frame flicker (the precise
    // symptom MTL_SHADER_VALIDATION masks). wined3d's
    // shader_glsl_get_sample_function patches the GLSL sampler arity off
    // the actual bound view, same shape.
    //
    // Keeping the shader's dcl as the fallback when no host layout is
    // supplied preserves the airconv_cli path.
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
        default:
          // UNKNOWN — host didn't pin (e.g. stage with no bound
          // texture at PSO build time, or a partial-binding shape).
          // Leave samp_kind[i] alone: the shader's own dcl is the
          // next-best signal, and the SM 1.x default-to-2D arm below
          // catches the remaining Unknown case.
          break;
        }
      }
    }
    // Fallback for SM 1.0..1.3 sampler slots the host didn't pin: keep the
    // historical Texture2D default. wined3d glsl_shader.c
    // shader_glsl_get_sample_function and DXVK src/dxso/dxso_compiler.cpp
    // both assume 2D when the bytecode itself can't disambiguate. Without
    // either source, samp_kind[i] stays Unknown → the binding loop below
    // `continue`s → tex_arg_idx[i] stays -1 → the Tex opcode short-
    // circuits and r0 sees the raw TEXCOORD<n> stage_in instead of the
    // sampled texel (UV-as-color gradient).
    if (shader->header.major == 1 && shader->header.minor < 4) {
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
        // Unknown sampler (no dcl) — leave unbound; the case arm will
        // short-circuit on tex_arg_idx == -1.
        continue;
      }
      // array_size = 1 (not 0) — AGX rejects 0 with
      // XPC_ERROR_CONNECTION_INTERRUPTED at pipeline link, even though
      // the metallib writer accepts it without complaint. xcrun metal
      // emits `air.location_index, i32 N, i32 1` for non-array
      // bindings. See project_agx_pipeline_link_traps.
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

  // VS varying outputs — oD0/oD1 (AttributeOut) and oT0..oT7
  // (TexcoordOut). SM1/SM2 don't dcl their outputs, so the semantic
  // is positional: oDn = COLOR<n>, oTn = TEXCOORD<n>. Pre-scan the
  // bytecode for dst writes to discover which outputs the shader
  // produces; sparse-define so the PS doesn't see varyings the VS
  // never writes.
  //
  // SM3 (vs_3_0) uses register file Output=6 — same numeric value as
  // TexcoordOut — with dcl_<usage> on each o# carrying the semantic.
  // Treating SM3 `o#` writes as TEXCOORD<n> would silently mislabel
  // COLOR / FOG / PSIZE outputs, so the pre-scan is gated to SM ≤ 2.
  // SM3 dcl-driven output naming lands in its own commit.
  //
  // OutputPosition is defined first so it stays at struct field 0;
  // varyings get fields 1..N in DefineOutput order.
  std::array<int, 2> oD_arg_idx{};
  oD_arg_idx.fill(-1);
  std::array<int, 8> oT_arg_idx{};
  oT_arg_idx.fill(-1);
  int oFog_arg_idx = -1;
  // VS oPts (RasterizerOut[2], point size). SM1.x VS writes via
  // `mov oPts, <value>`; SM3 VS declares `dcl_psize o#` and writes via
  // the declared o<N>. Pre-scan tracks both paths; epilogue stores the
  // scalar to the AIR [[point_size]] output. Without this plumb-through
  // every D3DPT_POINTLIST draw renders at size 1.0 (the AIR default)
  // regardless of what the VS computed.
  //
  // vs_point_size_override > 0 forces oPts_used true even for VSes that
  // don't touch oPts — the slot below is seeded with the override value
  // (D3DRS_POINTSIZE) instead of 1.0, and since the bytecode never
  // stores to the slot, the override propagates to AIR. The host gates
  // the override on POINTLIST + non-writing VS so the variant cache
  // doesn't explode on no-op draws.
  bool oPts_used = false;
  if (is_vertex && vs_point_size_override > 0.0f)
    oPts_used = true;
  int oPts_arg_idx = -1;
  // SM3 dcl_psize routes here via oN_is_pointsize[N] aliasing, mirroring
  // the oN_is_position pattern below. Tracked separately from
  // oN_arg_idx so the SM3 varying-output emit loop skips PointSize
  // (it's a non-varying output that needs scalar emission, not float4).
  std::array<bool, 16> oN_is_pointsize{};
  // PS [[color(N)]] outputs — SM2+ shaders may write up to 4 RTs.
  // SM1.x has no ColorOut register file; the output is r0 implicitly,
  // and that path stays gated through the SM1.x lowering work.
  std::array<int, 4> oC_arg_idx{};
  oC_arg_idx.fill(-1);
  int oDepth_arg_idx = -1;
  // SM3 VS outputs — `dcl_<usage> o#` registers, dcl-driven semantics.
  // Output # is the dcl's bound_to.num; usage gives the AIR user name.
  // o#'s register file decodes as TexcoordOut/Output (=6) — same
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
    // 8-element clip-distance array — every VS unconditionally writes
    // the array even when D3DRS_CLIPPLANEENABLE is 0, because the host
    // packs only enabled planes consecutively and signals "no plane is
    // enabled" by setting clip_count to 0; the per-lane select below
    // then short-circuits to 0.0, which the GPU treats as "not
    // clipped". This avoids per-draw VS variants keyed on
    // CLIPPLANEENABLE — DXVK uses the same shape via a spec constant
    // (src/dxso/dxso_compiler.cpp:3603) for the same reason.
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
      for (int i = 0; i < 2; ++i) {
        if (oD_used[i])
          oD_arg_idx[i] = (int)sig.DefineOutput(
              air::OutputVertex{
                  .user = "COLOR" + std::to_string(i),
                  .type = air::msl_float4,
              }
          );
      }
      for (int i = 0; i < 8; ++i) {
        if (oT_used[i])
          oT_arg_idx[i] = (int)sig.DefineOutput(
              air::OutputVertex{
                  .user = "TEXCOORD" + std::to_string(i),
                  .type = air::msl_float4,
              }
          );
      }
      if (oFog_used)
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
          break; // Depth / etc — TODO
        }
        if (!base)
          continue;
        oN_arg_idx[d.bound_to.num] = (int)sig.DefineOutput(
            air::OutputVertex{
                .user = std::string(base) + std::to_string(d.dcl.usage_index),
                .type = air::msl_float4,
            }
        );
      }
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
      // Temp / Texture register, not DepthOut — but the opcode
      // semantically writes [[depth]]. Treat their presence as a
      // depth-output declaration too so the OutputDepth signature
      // entry + slot get allocated. wined3d glsl_shader.c:6542 +
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
      if (oC_used[i])
        oC_arg_idx[i] = (int)sig.DefineOutput(
            air::OutputRenderTarget{
                .dual_source_blending = false,
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
  }
  auto [fn, fn_md] = sig.CreateFunction(name, context, module, 0, false);

  IRBuilder<> builder(BasicBlock::Create(context, "entry", fn));

  // AIRBuilder for high-level texture / sampler ops — same wrapper
  // dxbc_converter uses (line 484). Texture sample lowerings call
  // through this so the air.sample.* intrinsic emission stays in one
  // place. Debug stream is null'd; AIR diagnostics aren't surfaced
  // through the DXSO path.
  raw_null_ostream nulldbg{};
  llvm::air::AIRBuilder air(builder, nulldbg);

  // Temp register file r0..r31 (SM3 ceiling — SM2/SM1 use a strict
  // subset). One alloca holds the whole array so per-instruction
  // lowerings just GEP in and load/store. mem2reg promotes them out
  // of memory in the optimization pipeline.
  //
  // Zero-init the whole array: D3D9 PS shaders commonly write only
  // a subset of r0's lanes before the implicit r0→oC0 epilogue, and
  // SM 1.x lets `mov r0.rgb, t0` leave r0.a as the default. Without
  // an initial store, mem2reg promotes the uninitialized load to
  // `undef`; AIR codegen leaves it as such and Metal's hardware can
  // materialise undef as any value — typically a stale GPU register
  // from the prior dispatch. The visible symptom is per-frame
  // variation on the unwritten lanes — for NFS:MW's smoke shader
  // (writes r0.rgb, leaves r0.a unwritten), the undefined alpha
  // flickers between frames, and SRCALPHA / INVSRCALPHA blending
  // turns that into the "flakes-plus-flicker" pattern. DXVK
  // initialises every temp pointer to constvec4f32(0,0,0,0) on first
  // emit (dxso_compiler.cpp:1158) for the same reason.
  // MTL_SHADER_VALIDATION masks this by replacing undef reads with
  // zeros — exactly what we now do unconditionally.
  auto *float4Ty = FixedVectorType::get(Type::getFloatTy(context), 4);
  auto *int4Ty = FixedVectorType::get(Type::getInt32Ty(context), 4);
  auto *tempArrTy = ArrayType::get(float4Ty, 32);
  auto *temps = builder.CreateAlloca(tempArrTy, nullptr, "r");
  builder.CreateStore(ConstantAggregateZero::get(tempArrTy), temps);

  // VS address register a0. <4 x i32> alloca, zero-seeded — Mova
  // writes the rounded float-to-int result here, and load_src reads
  // it back when a Const operand carries a relative-address suffix.
  Value *a0_slot = nullptr;
  if (is_vertex) {
    a0_slot = builder.CreateAlloca(int4Ty, nullptr, "a0");
    builder.CreateStore(ConstantAggregateZero::get(int4Ty), a0_slot);
  }

  // Loop iterator aL — single i32. Loop sets it to the initial value
  // from the integer constant; EndLoop steps it by the stride. Const
  // reads with relative.base.type == Loop pick it up directly. SM3
  // allows aL in both VS and PS, so allocate unconditionally
  // (DXVK's emitGetOperandPtr for Loop doesn't gate on stage either).
  auto *aL_slot = builder.CreateAlloca(Type::getInt32Ty(context), nullptr, "aL");
  builder.CreateStore(builder.getInt32(0), aL_slot);

  // Predicate register p0 — <4 x i1>, one bool per lane. Setp writes
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
  // PS oC# slots — one alloca per render target the pre-scan
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

  // VS varying-output slots — one alloca per oD#/oT# the pre-scan
  // discovered. Pre-seeded with zero so a partial-mask write still
  // produces a defined varying for the lanes the shader didn't touch.
  std::array<Value *, 2> oD_slot{};
  std::array<Value *, 8> oT_slot{};
  Value *oFog_slot = nullptr;
  // oPts (point size) — float4 alloca for uniform store_dst plumbing;
  // epilogue extracts lane 0 for the actual [[point_size]] scalar.
  // Default seed 1.0 so a shader that defines OutputPointSize but
  // never writes oPts still produces visible points. When the host
  // requested D3DRS_POINTSIZE auto-injection (override > 0), seed
  // with the override value instead — the bytecode never writes oPts
  // in that path (the override is gated on writes_point_size == false
  // on the device side), so the seed propagates to AIR directly.
  // Mirrors the oDepth pattern at :846 + DXVK dxso_compiler.cpp:3530.
  Value *oPts_slot = nullptr;
  if (is_vertex && oPts_arg_idx >= 0) {
    float seed = vs_point_size_override > 0.0f ? vs_point_size_override : 1.0f;
    Value *seed_const = ConstantFP::get(builder.getFloatTy(), seed);
    Value *seed_splat = ConstantVector::getSplat(llvm::ElementCount::getFixed(4), cast<llvm::Constant>(seed_const));
    oPts_slot = builder.CreateAlloca(float4Ty, nullptr, "oPts");
    builder.CreateStore(seed_splat, oPts_slot);
  }
  if (sm12_vs_varyings) {
    auto *zero4 = ConstantAggregateZero::get(float4Ty);
    for (int i = 0; i < 2; ++i) {
      if (oD_arg_idx[i] < 0)
        continue;
      oD_slot[i] = builder.CreateAlloca(float4Ty, nullptr, ("oD" + std::to_string(i)).c_str());
      builder.CreateStore(zero4, oD_slot[i]);
    }
    for (int i = 0; i < 8; ++i) {
      if (oT_arg_idx[i] < 0)
        continue;
      oT_slot[i] = builder.CreateAlloca(float4Ty, nullptr, ("oT" + std::to_string(i)).c_str());
      builder.CreateStore(zero4, oT_slot[i]);
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

  // air::AirType is constructed lazily — only the manual-fetch path
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
      // popcount of slot bits below this element's slot — same packing
      // dxbc_converter_basicblock.cpp:418-420 uses so the host can
      // produce one shared layout for both pipelines.
      unsigned int shift = 32u - element.slot;
      unsigned int vbuf_entry_index = element.slot ? __builtin_popcount((ia_layout->slot_mask << shift) >> shift) : 0u;
      auto *vbuf_entry = builder.CreateLoad(
          types._dxmt_vertex_buffer_entry,
          builder.CreateConstGEP1_32(types._dxmt_vertex_buffer_entry, vbuf_table, vbuf_entry_index)
      );
      auto *base_addr = builder.CreateExtractValue(vbuf_entry, {0});
      auto *stride = builder.CreateExtractValue(vbuf_entry, {1});
      // [[vertex_id]] in Metal is the post-resolution buffer-space
      // vertex number for both shapes — vertexStart + i for
      // drawPrimitives and index_value + baseVertex for
      // drawIndexedPrimitives. SM50's pull_vertex_input uses the same
      // value (vertex_id_with_base = function arg) at
      // dxbc_converter_basicblock.cpp:409. [[base_vertex]] is bound
      // for the dcl side-effects but adding it here would double-add
      // and walk past the end of the vertex buffer — verified
      // empirically via smoke_draw_indexed (P2 BaseVertexIndex=1 with
      // a dummy slot 0).
      //
      // Per-instance streams (D3DSTREAMSOURCE_INSTANCEDATA) replace
      // vertex_id with base_instance + instance_id/divisor. step_rate
      // == 0 freezes the read at base_instance — DXVK's "never step"
      // semantics. Mirrors dxbc_converter_basicblock.cpp:393.
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
      // unread here) leaves room for a follow-up lowering — e.g.,
      // recovering the raw 0-indexed vertex number for stream-output
      // emulation — without re-shuffling input indices.
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
          // pull_vec4_from_addr returns int4 for non-normalised
          // integer formats (UChar4 / Char4 / Short4 / UShort4 etc.).
          // D3D9 spec presents these to v# as integer-valued floats:
          // a D3DDECLTYPE_UBYTE4 byte of 5 reaches the shader as
          // v.x = 5.0, not the bit pattern of int(5). The bone-index
          // path (mova a0, v_blendindices then m4x4 r, v_pos,
          // c[a0.x + base]) depends on this — bitcasting walks the
          // int bits into denormals and mova rounds them all to 0,
          // collapsing every vertex onto bone 0. wined3d preserves
          // int-as-float by using glVertexAttribPointer with
          // normalize=GL_FALSE for SM<4 (context_gl.c:4925-4934);
          // we lower the same conversion at IA fetch time. SIToFP
          // is correct for both Z- and S-extended sources because
          // the int32 already carries the signed representation.
          src = builder.CreateSIToFP(src, float4Ty);
        }
      }
    } else if (input_arg_idx[i] >= 0) {
      // POINTSPRITEENABLE substitution: when v<N> was dcl'd as
      // TEXCOORD (SM3 PS path), override the stage_in read with the
      // point-sprite UV vec4. v<N> dcl'd as COLOR is left alone.
      if (point_sprite_v && i < ps_v_is_texcoord.size() && ps_v_is_texcoord[i])
        src = point_sprite_v;
      else
        src = fn->getArg(input_arg_idx[i]);
    } else {
      src = ConstantAggregateZero::get(float4Ty);
    }
    auto *slot = builder.CreateGEP(inputArrTy, inputs, {builder.getInt32(0), builder.getInt32(i)});
    builder.CreateStore(src, slot);
  }

  // PS Texture-register-file inputs (t0..t7). Allocated unconditionally
  // so the load_src closure has a valid pointer to GEP into; VS and
  // SM3 PS leave every ps_tex_arg_idx[] slot at -1 and the alloca just
  // zero-fills (matching the "undeclared input is zero" DXSO contract
  // for v#). For SM2 PS / SM 1.4 PS, slots that the dcl walk above
  // bound get pre-loaded from the corresponding fragment stage-in arg.
  // SM 1.0..1.3 t# is populated at runtime by the `tex t#` lowering
  // (a future store_dst arm — that opcode shape isn't yet covered);
  // the upfront load is zero-fill only and the opcode overwrites it.
  auto *texInputArrTy = ArrayType::get(float4Ty, 8);
  Value *tex_inputs = nullptr;
  if (!is_vertex) {
    tex_inputs = builder.CreateAlloca(texInputArrTy, nullptr, "t");
    for (uint32_t i = 0; i < 8; ++i) {
      Value *src;
      if (point_sprite_v) {
        // POINTSPRITEENABLE substitution for the t# (SM2 / SM 1.4 PS)
        // register file. Override regardless of whether the slot would
        // otherwise have a stage_in binding — apps may declare zero
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

  // Operand helpers — closures over the entry-block builder + the
  // register files. Each helper returns nullptr when the operand
  // shape isn't covered yet (Input/Const/sampler/...); the per-opcode
  // arms treat that as "skip this instruction" rather than fail.
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
      auto *gep = builder.CreateGEP(inputArrTy, inputs, {builder.getInt32(0), builder.getInt32(src.base.num)});
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
      // def-baked Float32 literal first — codegen-time constant beats
      // a runtime load. Int / Bool defs share the Const switch arm
      // here as a per-DefKind dispatch only because the decoder gives
      // each its own DxsoRegisterType (ConstInt / ConstBool); those
      // types are handled by the dedicated arms below.
      const DxsoBoundConst *match = nullptr;
      for (const auto &c : shader->metadata.consts) {
        if (c.bound_to.type == src.base.type && c.bound_to.num == src.base.num) {
          match = &c;
          break;
        }
      }
      if (match && match->def.kind == DxsoDefKind::Float32) {
        auto *fTy = Type::getFloatTy(context);
        Constant *lanes[4] = {
            ConstantFP::get(fTy, match->def.payload.f32[0]),
            ConstantFP::get(fTy, match->def.payload.f32[1]),
            ConstantFP::get(fTy, match->def.payload.f32[2]),
            ConstantFP::get(fTy, match->def.payload.f32[3]),
        };
        v = ConstantVector::get(lanes);
        break;
      }
      if (match)
        return nullptr; // mismatched def kind on a Float register —
                        // malformed bytecode, skip.
      // Runtime-set: load from the Float CB at slot src.base.num,
      // optionally plus the indexed component of a0 when the operand
      // carries a relative-address suffix (`c[a0.x + N]`). vs_2_0/3_0
      // caps c# at 255 and ps_2_0/3_0 at 223; clamp to the VS
      // ceiling. Out-of-range registers come from malformed
      // bytecode — skip rather than emit an OOB read on the host CB.
      if (src.base.num >= 256)
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
      }
      auto *gep = builder.CreateGEP(float4Ty, cbPtr, idx);
      v = builder.CreateLoad(float4Ty, gep);
      break;
    }
    case DxsoRegisterType::ConstInt: {
      // i# as a generic operand. DXVK src/dxso/dxso_compiler.cpp:902
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
    default:
      return nullptr;
    }
    if (src.swizzle.raw() != 0b11100100u) {
      int mask[4] = {(int)src.swizzle[0], (int)src.swizzle[1], (int)src.swizzle[2], (int)src.swizzle[3]};
      v = builder.CreateShuffleVector(v, v, ArrayRef<int>(mask, 4));
    }
    if (src.modifier == DxsoRegModifier::None)
      return v;
    // Source modifier — fetch-time arithmetic on the post-swizzle
    // float4. Dz/Dw divide all four lanes by the z/w of the swizzled
    // vector; DXVK src/dxso/dxso_compiler.cpp:1700 applies them after
    // swizzle for the same reason — the spec defines them on the
    // operand the consuming opcode actually sees. Not is the bool-
    // register inversion (D3D9 spec restricts the modifier to b# /
    // predicate registers, where the value is 0.0 or 1.0); we
    // implement it as a logical "non-zero ⇒ 0, zero ⇒ 1" select so
    // the IR reads cleanly for downstream passes.
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
    case DxsoRegModifier::Dz: {
      Value *zlane = builder.CreateExtractElement(v, builder.getInt32(2));
      v = builder.CreateFDiv(v, builder.CreateVectorSplat(4, zlane));
      break;
    }
    case DxsoRegModifier::Dw: {
      Value *wlane = builder.CreateExtractElement(v, builder.getInt32(3));
      v = builder.CreateFDiv(v, builder.CreateVectorSplat(4, wlane));
      break;
    }
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
      return nullptr; // unreachable — enum values past Not are masked
                      // out by the decoder per dxso_decoder.hpp:155
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
    // src/dxso/dxso_compiler.h:495-534 (emitDstStore): result-shift
    // first as a power-of-two multiply (sign-magnitude shift, range
    // -8..+7), then saturate as a [0,1] clamp. Both apply to the
    // full <4 x float> before mask blending.
    if (dst.shift != 0) {
      float scale = dst.shift < 0 ? 1.0f / static_cast<float>(1 << -dst.shift) : static_cast<float>(1 << dst.shift);
      auto *fTy = Type::getFloatTy(context);
      Constant *splat = ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(fTy, scale));
      value = builder.CreateFMul(value, splat);
    }
    if (dst.saturate)
      value = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, value);
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
    // Not inverts it (DXVK src/dxso/dxso_compiler.cpp:2305-2321
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
  // treated as skip — same model the per-opcode arms had inline.
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

  // float4 splat constant — uniqued by LLVM, no IR cost per use.
  auto v4splat = [&](double x) -> Constant * {
    return ConstantVector::getSplat(ElementCount::getFixed(4), ConstantFP::get(Type::getFloatTy(context), x));
  };

  // Scalar dot product of the first n lanes of a and b (n=3 or 4).
  // For n=3 the inputs get shuffled down to a 3-vec first so lane 3 —
  // which may be poison when the temp wasn't initialized — never
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

  // Walk the body. No opcodes are lowered yet — the iterator + switch
  // are the spine real lowerings hang off. Anything we don't recognize
  // is silently skipped so the output stays a valid zero-filled
  // placeholder until per-opcode commits land.
  // DWORD is `typedef uint32_t DWORD` in this codebase
  // (windows_base.h:104), so DxsoBytecodeIter's `const DWORD *`
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
    Value *aL_backup_slot; // null for Rep — no aL save/restore
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
      fold_unary(ins, [&](Value *a) { return a; });
      break;
    case DxsoOpcode::Mova: {
      // mova a0.<mask>, src — round to nearest even, convert to int,
      // store into a0. SM1.x predates the `mova` opcode and a plain
      // `mov` to a0 floors instead of rounds (DXVK
      // src/dxso/dxso_compiler.cpp:1850-1855), but Mova is SM2+ only,
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
      // 1.0 / x. AGX has a fast-recip primitive InstCombine doesn't
      // emit; if the AIR builder's air.recip ever shows lower-error
      // codegen, swap to it here.
      fold_unary(ins, [&](Value *a) {
        auto *one = builder.CreateVectorSplat(4, ConstantFP::get(Type::getFloatTy(context), 1.0));
        return builder.CreateFDiv(one, a);
      });
      break;
    case DxsoOpcode::Rsq:
      // 1.0 / sqrt(|x|). Negative inputs are clamped to abs to match
      // D3D9: DXVK src/dxso/dxso_compiler.cpp:2962 (inversesqrt(abs))
      // sqrt() on a negative would return NaN.
      fold_unary(ins, [&](Value *a) {
        return air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, a));
      });
      break;
    case DxsoOpcode::Exp:
    case DxsoOpcode::ExpP:
      // D3D9 Exp / Log are base-2 (one of the FFP unfortunate names).
      // ExpP is SM<2 partial-precision exp with a special four-lane
      // form (DXVK src/dxso/dxso_compiler.cpp:2009-2032); SM2+ falls
      // through to plain Exp. The SM<2 four-lane form lands when the
      // SM1.x PS path needs it.
      fold_unary(ins, [&](Value *a) { return air.CreateFPUnOp(llvm::air::AIRBuilder::exp2, a); });
      break;
    case DxsoOpcode::Log:
    case DxsoOpcode::LogP:
      // D3D9 log is log2(|x|). LogP is the partial-precision alias;
      // DXVK src/dxso/dxso_compiler.cpp:2034 collapses both to the
      // same opcode.
      fold_unary(ins, [&](Value *a) {
        return air.CreateFPUnOp(llvm::air::AIRBuilder::log2, air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, a));
      });
      break;
    case DxsoOpcode::Pow:
      // D3D9 pow(a, b) = 2^(b * log2(|a|)). Base is implicitly abs'd;
      // air.fast_pow on a negative base with non-integer exponent
      // returns NaN, which diverges. DXVK src/dxso/dxso_compiler.cpp:2046.
      // air.fast_pow rather than llvm.pow because AGX's pipeline
      // compiler rejects llvm.* intrinsics in metallib bodies as
      // unrecognized opcodes (XPC_ERROR_CONNECTION_INTERRUPTED at
      // link, no diagnostic at metallib-write).
      fold_binary(ins, [&](Value *a, Value *b) {
        return air.CreateFPBinOp(llvm::air::AIRBuilder::pow, air.CreateFPUnOp(llvm::air::AIRBuilder::fabs, a), b);
      });
      break;
    case DxsoOpcode::DsX:
    case DxsoOpcode::DsY: {
      // SM3+ explicit gradient: dst = ddx(src) or ddy(src). Quad
      // derivatives are PS-only. Mirrors DXVK
      // src/dxso/dxso_compiler.cpp:2289 (opDpdx / opDpdy).
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
      // DXVK src/dxso/dxso_compiler.cpp:2089 emits OpFSign — same
      // tri-state result. Metal AIR has no direct sign intrinsic,
      // so unfold to a fcmp/select pair on the <4 x float>. The
      // SM<3 src[1]/src[2] scratch operands DXVK consumes for
      // sw-emulation are ignored, matching the SM3+ HW path.
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
      // Mirrors DXVK src/dxso/dxso_compiler.cpp:2184. Used by FFP
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
      // FFP-style lighting coefficient.
      // dst.x = 1
      // dst.y = max(src.x, 0)
      // dst.z = (src.x >= 0 && src.y >= 0) ? pow(max(src.y, 0), p) : 0
      //         where p = clamp(src.w, -127.9961, 127.9961)
      // dst.w = 1
      // Mirrors DXVK src/dxso/dxso_compiler.cpp:2141. The exponent
      // clamp is the D3D9-spec range; pow(max(...,0)) keeps the base
      // non-negative so AIR's air.fast_pow doesn't see a malformed
      // input. The zTest selects 0 for z when either src.x or src.y
      // is negative, matching the FFP behavior.
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
      Value *xge = builder.CreateFCmpOGE(sx, zero_f);
      Value *yge = builder.CreateFCmpOGE(sy, zero_f);
      Value *cond = builder.CreateAnd(xge, yge);
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
      // away). Mirrors DXVK src/dxso/dxso_compiler.cpp:2063 — same
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
      // src/dxso/dxso_compiler.cpp:2110 — same shape; the SM2-only
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
      // the dst's masked lanes. DXVK src/dxso/dxso_compiler.cpp:2274
      // — same shape: emitDot on .xy + FAdd of src[2].x.
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
      // src/dxso/dxso_compiler.cpp:2093 — same shape: rsqrt(dot3),
      // multiply src * splat. The d3d9FloatEmulation FLT_MAX clamp
      // is a DXVK option, not the default; skip until a shader needs
      // it.
      if (!ins.has_dst || ins.src_count < 1)
        break;
      Value *a = load_src(ins.src[0]);
      if (!a)
        break;
      Value *dot3 = compute_dot(a, a, 3);
      Value *rcp_len = air.CreateFPUnOp(llvm::air::AIRBuilder::rsqrt, dot3);
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
      // (a < b) ? 1.0 : 0.0, lane-wise. Ordered compare — NaN inputs
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
      // shader encodes it. DXVK src/dxso/dxso_compiler.cpp:2236.
      fold_ternary(ins, [&](Value *a, Value *b, Value *c) {
        return builder.CreateSelect(builder.CreateFCmpOGE(a, v4splat(0.0)), b, c);
      });
      break;
    case DxsoOpcode::Cnd:
      // SM1.x conditional: (src0 > 0.5) ? src1 : src2, lane-wise.
      // DXVK src/dxso/dxso_compiler.cpp:2259.
      fold_ternary(ins, [&](Value *a, Value *b, Value *c) {
        return builder.CreateSelect(builder.CreateFCmpOGT(a, v4splat(0.5)), b, c);
      });
      break;
    case DxsoOpcode::Lrp:
      // src0 * (src1 - src2) + src2 — the standard mix/lerp. DXVK
      // src/dxso/dxso_compiler.cpp:1438 emitMix derives the same
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
      // dp{3,4} broadcasts the dot to all four dst lanes — store_dst's
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
      // M<N>x<M> dst, vec, mat — N-element dot of `vec` against M
      // consecutive matrix rows starting at src1.base.num. dst.lane[i]
      // = dot(vec, mat[i]). Mirrors DXVK src/dxso/dxso_compiler.cpp
      // emitMatrixAlu (line 2325) — that walks src1.id.num the same
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
      // Trim the dst mask to the first compCount set lanes — DXVK
      // src/dxso/dxso_compiler.cpp:2363. m4x3 r0.xyzw still writes
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
      // Two encodings share opcode 66 (Tex):
      //   SM 1.0-1.3 `tex t<n>` — no source operand. Coord is the t<n>
      //     slot (interpolated TEXCOORD<n>, possibly already overwritten
      //     by an earlier texcoord / texreg2* into the same slot).
      //   SM 1.4    `texld dst, src` — one source. Coord is src[0] with
      //     swizzle/modifier already lowered by load_src. Sampler is
      //     still implicit at dst.num.
      //   SM 2+     `texld / texldl / texldd dst, coord, sampler [, ddx,
      //     ddy]` — sampler is src[1]; TexLdl reads coord.w as explicit
      //     LOD; TexLdd reads src[2]/src[3] as ddx/ddy gradients.
      // wined3d glsl_shader.c shader_glsl_tex; DXVK
      // src/dxso/dxso_compiler.cpp:2872 / 2879 / 2940-2970.
      if (is_vertex || !ins.has_dst)
        break;
      bool is_grad = ins.opcode == DxsoOpcode::TexLdd;
      uint32_t slot;
      Value *coord4;
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
      }
      if (!coord4)
        break;
      // Coord shape follows the dcl'd texture type: 2D reads xy, Cube
      // reads xyz (Metal's cube sampler takes a direction vector, not
      // a face/uv pair), 3D reads xyz. TexLdl reads lane 3 as the LOD
      // regardless of the texture type — DXVK
      // src/dxso/dxso_compiler.cpp:2872 does the same composite-extract
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
        // Texture3D — the only remaining kind the bind loop emits
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
      // src/dxso/dxso_compiler.cpp:2885-2895. Project divides the
      // coord by w before sampling; Bias passes w as a LOD bias.
      // TexLdl/TexLdd ignore the mode — they always carry an explicit
      // LOD or explicit gradients, so Project/Bias don't apply.
      auto mode = static_cast<DxsoTexLdMode>(ins.specific_data);
      bool is_proj = ins.opcode == DxsoOpcode::Tex && mode == DxsoTexLdMode::Project;
      bool is_bias = ins.opcode == DxsoOpcode::Tex && mode == DxsoTexLdMode::Bias;
      if (is_proj) {
        Value *w = builder.CreateExtractElement(coord4, builder.getInt32(3));
        Value *w_splat = builder.CreateVectorSplat(cast<FixedVectorType>(coord->getType())->getNumElements(), w);
        coord = builder.CreateFDiv(coord, w_splat);
      }
      Value *texel = nullptr;
      if (is_grad) {
        // ddx in src[2], ddy in src[3]; shuffle each to the coord
        // shape (DXVK src/dxso/dxso_compiler.cpp:2880 derives the
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
        auto [t, residency] = air.CreateSampleGrad(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, ddx, ddy, no_offset
        );
        (void)residency;
        texel = t;
      } else if (ins.opcode == DxsoOpcode::TexLdl) {
        Value *lod = builder.CreateExtractElement(coord4, builder.getInt32(3));
        auto [t, residency] = air.CreateSample(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_level{lod}
        );
        (void)residency;
        texel = t;
      } else if (is_bias) {
        Value *bias = builder.CreateExtractElement(coord4, builder.getInt32(3));
        auto [t, residency] = air.CreateSample(
            tex_desc, tex_handle, samp_handle, coord,
            /*ArrayIndex=*/nullptr, no_offset, llvm::air::sample_bias{bias}
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
      // depth_2d sample (and sample_compare) returns a scalar float;
      // the rest of the DXSO pipeline operates on float4. Splat to
      // all four lanes per the NVIDIA/ATI INTZ / hardware-PCF
      // contract — `texld r#, t#, s#` against a depth texture
      // returns the shadow factor / depth value replicated in
      // r#.xyzw, which is what shadow-modulating shaders rely on
      // when they read r#.x as the shadow factor. Without this,
      // sampling Depth32Float_Stencil8 as MSL texture2d<float>
      // leaves r#.yzw undefined and the visible result is per-pixel
      // brightness variation on every shaded surface — most visible
      // on alpha-blended geometry like smoke / particles.
      if (samp_kind[slot] == DxsoTextureType::Texture2DDepth && texel) {
        texel = builder.CreateVectorSplat(4, texel);
      }
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::TexKill: {
      // texkill <reg> — discard the fragment if any tested lane of
      // the source is < 0. DXVK src/dxso/dxso_compiler.cpp:3066:
      //  - SM 2.0+ and SM 1.4 read the source through the dst slot
      //    and apply the dst writemask to select which lanes test.
      //  - SM 1.0-1.3 read the interpolated TEXCOORD<dst.num> via the
      //    PixelTexcoord register file and test xyz unconditionally.
      // Early-Z opt-out: AIR's metallib linker detects
      // discard_fragment and disables early fragment tests on its
      // own, and the DXSO path never opts in (only DXBC does, via
      // dxbc_converter_cfg.cpp:540), so kill-bearing PS already runs
      // late. No explicit ExecutionModeEarlyFragment-style negation
      // needed here.
      if (is_vertex)
        break;
      bool sm14_or_later = shader->header.major >= 2 || (shader->header.major == 1 && shader->header.minor == 4);
      Value *coord4 = nullptr;
      bool test_lane[4] = {false, false, false, false};
      if (sm14_or_later) {
        // Mirror the dst slot as a src — identity swizzle, no
        // modifier. has_relative / relative are propagated verbatim
        // per DXVK src/dxso/dxso_compiler.cpp:3069; SM3 dst-relative
        // texkill works end-to-end once load_src grows relative-
        // addressing for non-Const operand types.
        DxsoSrcRegister src{};
        src.base = ins.dst.base;
        src.has_relative = ins.dst.has_relative;
        src.relative = ins.dst.relative;
        coord4 = load_src(src);
        if (!coord4)
          break;
        for (uint32_t i = 0; i < 4; ++i)
          test_lane[i] = ins.dst.mask[i];
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
      // TEXCOORD<n> to [0,1] and write back to t<n>. SM 1.4 `texcrd
      // dst, src`: copy src to dst (no clamp); src swizzle and Dz/Dw
      // modifier are lowered by load_src. wined3d glsl_shader.c
      // shader_glsl_texcoord:6432-6471.
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
      // DXVK dxso_compiler.cpp:2823-2841.
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
        // 1D coord under a 2D sampler — pack as (dot, 0) per DXVK
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
      auto [texel, residency] = air.CreateSample(
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
      // dxso_compiler.cpp:2818-2821 swizzles (0,1,2,2); for 2D only
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
      auto [texel, residency] = air.CreateSample(
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
      // the destination stage. Bem does the math only — no sampling.
      // DXVK dxso_compiler.cpp:1868-1905 (emitBem) + 2790-2803 (sample
      // wiring) + 2968-2987 (TexBemL luminance).
      //
      //   src0 = tc[dst.num] (interpolated TEXCOORD<dst.num>)
      //   src1 = n (typically t<dst.num-1>'s post-sample register)
      //   dst.u = src0.x + bm[0][0]*n.x + bm[1][0]*n.y
      //   dst.v = src0.y + bm[0][1]*n.x + bm[1][1]*n.y
      //   TexBem  : sample dst.num at (dst.u, dst.v), store texel.
      //   TexBemL : same as TexBem, then result *= clamp(n.z * lscale +
      //             loffset, 0, 1). (n.z = the perturbed-sample's .b
      //             channel per DXVK shape — m_module.opCompositeExtract
      //             at index 2 of the post-sample `result`.)
      //   Bem     : store (dst.u, dst.v, 0, 0) to dst, no sample.
      //
      // ps_bump_env may be nullptr (host didn't thread the arg) — in
      // that case the bump-env matrix is implicitly identity (silent
      // pass-through). Variant cache keys the constants in, so when
      // the host gates correctly the matrix is always populated.
      if (is_vertex || !ins.has_dst || ins.src_count < 1 || !tex_inputs)
        break;
      uint32_t slot = ins.dst.base.num;
      if (slot >= 8)
        break;
      // src1 = n (bump-map sample from the previous stage). Loaded via
      // load_src to honor any swizzle/modifier the bytecode applied —
      // SM 1.x source modifiers (signed bias _bx2, etc.) lower into
      // load_src so we don't need to re-handle them here.
      Value *n4 = load_src(ins.src[0]);
      if (!n4)
        break;
      // src0 = tc[dst.num]. Interpolated TEXCOORD<dst.num>, pre-loaded
      // into tex_inputs at function entry — matches the SM 1.0..1.3
      // implicit-source convention used by Tex, TexBem*, and the
      // TexM3x* family.
      auto *gep_tc = builder.CreateGEP(texInputArrTy, tex_inputs, {builder.getInt32(0), builder.getInt32(slot)});
      Value *tc4 = builder.CreateLoad(float4Ty, gep_tc);
      // Use raw .xy from the texcoord. DXVK dxso_compiler.cpp:2800 calls
      // DoProjection(tc, /*switchProjRes=*/true) which is a SpecSampler-
      // Projected-keyed opSelect (dynamic gate on the D3DTSS_TEXTURE-
      // TRANSFORMFLAGS PROJECTED bit), NOT an unconditional divide.
      // A prior judge-fix here applied an unconditional /w that produced
      // NaN/Inf when tc.w==0 and over-divided non-PROJECTED apps with
      // non-unit tc.w — both regressions vs the per-spec gate. Plain
      // .xy matches the common non-PROJECTED case (the vast majority of
      // SM 1.x TexBem content); the PROJECTED-stage case is left as a
      // known gap and would need either a per-stage projection mask on
      // DXSO_SHADER_PS_BUMP_ENV_DATA + a variant-cache axis, or DXVK's
      // spec-constant runtime gate.
      Value *tc_x = builder.CreateExtractElement(tc4, builder.getInt32(0));
      Value *tc_y = builder.CreateExtractElement(tc4, builder.getInt32(1));
      Value *n_x = builder.CreateExtractElement(n4, builder.getInt32(0));
      Value *n_y = builder.CreateExtractElement(n4, builder.getInt32(1));
      // Per-stage bump-env matrix. Identity (bm00=bm11=1, bm01=bm10=0)
      // when the host didn't thread the arg — preserves the same
      // pass-through behaviour the prior silent-default-drop produced,
      // but with TexBem/TexBemL actually sampling.
      float bm00 = 1.0f, bm01 = 0.0f, bm10 = 0.0f, bm11 = 1.0f;
      if (ps_bump_env) {
        bm00 = ps_bump_env->mat[slot][0];
        bm01 = ps_bump_env->mat[slot][1];
        bm10 = ps_bump_env->mat[slot][2];
        bm11 = ps_bump_env->mat[slot][3];
      }
      auto *fT = Type::getFloatTy(context);
      // perturbed.x = tc.x + bm00 * n.x + bm10 * n.y
      Value *u = builder.CreateFAdd(
          tc_x,
          builder.CreateFAdd(
              builder.CreateFMul(ConstantFP::get(fT, bm00), n_x), builder.CreateFMul(ConstantFP::get(fT, bm10), n_y)
          )
      );
      // perturbed.y = tc.y + bm01 * n.x + bm11 * n.y
      Value *v = builder.CreateFAdd(
          tc_y,
          builder.CreateFAdd(
              builder.CreateFMul(ConstantFP::get(fT, bm01), n_x), builder.CreateFMul(ConstantFP::get(fT, bm11), n_y)
          )
      );
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
      auto [texel, residency] = air.CreateSample(
          tex_desc, tex_handle, samp_handle, coord2,
          /*ArrayIndex=*/nullptr, no_offset
      );
      (void)residency;
      // TexBemL: scale by clamp(n.z * lscale + loffset, 0, 1). Per the
      // DXVK shape n.z is the *post-sample* result's .b channel (not
      // the n bump-map's .z) — extract from texel.
      if (ins.opcode == DxsoOpcode::TexBemL) {
        float lscale = ps_bump_env ? ps_bump_env->lscale[slot] : 1.0f;
        float loffset = ps_bump_env ? ps_bump_env->loffset[slot] : 0.0f;
        Value *texel_z = builder.CreateExtractElement(texel, builder.getInt32(2));
        Value *lum =
            builder.CreateFAdd(builder.CreateFMul(texel_z, ConstantFP::get(fT, lscale)), ConstantFP::get(fT, loffset));
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
      // DXVK dxso_compiler.cpp:2740-2790. Each opcode consumes a matrix
      // assembled from the previous (count-1) texcoord input registers
      // — the matching TexM3x{2,3}Pad ops are bytecode-side markers
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
      //                         directly without sampling — used to feed
      //                         a follow-up TexM3x3Spec / TexM3x3VSpec.
      // NFS:MW (PS 1.x era car shaders) uses TexM3x3Tex to look up
      // tangent-space cube reflections; silent-default-drop on these
      // produces black-where-shiny on car bodywork.
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
      // TexM3x3 stores the matrix dots without sampling — finishes here.
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
      auto [texel, residency] = air.CreateSample(
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
      // environment-mapped specular highlights — car-paint envmap
      // reflections in racing games of that era depend on this.
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
      // normalize(v) = v * rsqrt(dot(v, v)) — same shape as the
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
      // (dxso_compiler.cpp:2782). Matches the cubemap-sample convention
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
        // 2D fallback — extract xy. Apps binding a 2D texture to a
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
      auto [texel, residency] = air.CreateSample(
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
      auto [texel, residency] = air.CreateSample(
          tex_desc, tex_handle, samp_handle, coord,
          /*ArrayIndex=*/nullptr, no_offset
      );
      (void)residency;
      store_dst(ins.dst, texel);
      break;
    }
    case DxsoOpcode::If: {
      // SM2+ `if b#` reads a bool register and `if p#` reads a
      // predicate. DXVK src/dxso/dxso_compiler.cpp:2607.
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
      if (!cond)
        break;
      auto *then_bb = BasicBlock::Create(context, "if.then", fn);
      auto *else_bb = BasicBlock::Create(context, "if.else", fn);
      auto *merge_bb = BasicBlock::Create(context, "if.end", fn);
      builder.CreateCondBr(cond, then_bb, else_bb);
      builder.SetInsertPoint(then_bb);
      cf_stack.push_back({else_bb, merge_bb, false});
      break;
    }
    case DxsoOpcode::Ifc: {
      // Structured control flow. DXVK src/dxso/dxso_compiler.cpp:2592.
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
      // is unordered per DXVK src/dxso/dxso_compiler.cpp:1288; the
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
      if (!cond)
        break;
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
        // No else arm — close the empty else_bb with a fall-through
        // before switching to the merge block.
        builder.SetInsertPoint(b.else_bb);
        builder.CreateBr(b.merge_bb);
      }
      builder.SetInsertPoint(b.merge_bb);
      break;
    }
    case DxsoOpcode::Rep:
    case DxsoOpcode::Loop: {
      // Rep src0 (i#.x = count) — DXVK src/dxso/dxso_compiler.cpp:2510.
      // Loop aL, src1 (i#.x = count, .y = init aL, .z = stride) —
      // DXVK src/dxso/dxso_compiler.cpp:2521. Counts can come from a
      // def-baked literal (defi i#, ...) or from the runtime i#
      // binding ([[buffer(1)]]); the same emitter handles both since
      // LoopFrame carries Values, not int32_t constants.
      bool is_loop = ins.opcode == DxsoOpcode::Loop;
      if (ins.src_count < (is_loop ? 2u : 1u))
        break;
      const auto &s = ins.src[is_loop ? 1u : 0u];
      if (s.base.type != DxsoRegisterType::ConstInt)
        break;
      auto *i32Ty = Type::getInt32Ty(context);
      auto *int4Ty = FixedVectorType::get(i32Ty, 4);
      Value *count = nullptr;
      Value *init_aL = builder.getInt32(0);
      Value *stride = builder.getInt32(0);
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
      } else if (s.base.num < 16) {
        // Runtime i# read. The binding is `int4 *i` at slot 1 — GEP
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
      } else {
        break;
      }
      auto *header_bb = BasicBlock::Create(context, "loop.header", fn);
      auto *body_bb = BasicBlock::Create(context, "loop.body", fn);
      auto *latch_bb = BasicBlock::Create(context, "loop.latch", fn);
      auto *merge_bb = BasicBlock::Create(context, "loop.end", fn);
      Value *counter = builder.CreateAlloca(i32Ty, nullptr, "loop.i");
      builder.CreateStore(builder.getInt32(0), counter);
      Value *aL_backup = nullptr;
      if (is_loop) {
        // Save the outer-scope aL before overwriting with init so a
        // nested Loop can restore on EndLoop. DXVK
        // src/dxso/dxso_compiler.cpp:2421-2426 / 2500-2507.
        aL_backup = builder.CreateAlloca(i32Ty, nullptr, "aL.backup");
        Value *outer = builder.CreateLoad(i32Ty, aL_slot);
        builder.CreateStore(outer, aL_backup);
        builder.CreateStore(init_aL, aL_slot);
      }
      builder.CreateBr(header_bb);
      builder.SetInsertPoint(header_bb);
      Value *cur = builder.CreateLoad(i32Ty, counter);
      Value *cond = builder.CreateICmpSLT(cur, count);
      builder.CreateCondBr(cond, body_bb, merge_bb);
      builder.SetInsertPoint(body_bb);
      loop_stack.push_back({counter, aL_backup, count, stride, header_bb, latch_bb, merge_bb});
      break;
    }
    case DxsoOpcode::SetP: {
      // setp_<cmp> p0.<mask>, src0, src1 — lane-wise compare, masked
      // store into p0. DXVK src/dxso/dxso_compiler.cpp:2306. NotEqual
      // is unordered (line 1288); the rest are ordered.
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
      // Early loop exit. DXVK src/dxso/dxso_compiler.cpp:2545 / 2559.
      // Break unconditionally jumps to the enclosing loop's merge BB;
      // BreakC wraps the jump in a comparison whose true edge takes
      // the break and whose false edge continues the body. Both must
      // leave builder in an open block so subsequent body
      // instructions still have a place to emit (LLVM tolerates the
      // unreachable BB if no successor consumes it).
      if (loop_stack.empty())
        break;
      BasicBlock *merge_bb = loop_stack.back().merge_bb;
      auto *next_bb = BasicBlock::Create(context, "break.cont", fn);
      if (ins.opcode == DxsoOpcode::Break) {
        builder.CreateBr(merge_bb);
      } else {
        if (ins.src_count < 2)
          break;
        Value *a = load_src(ins.src[0]);
        Value *b = load_src(ins.src[1]);
        if (!a || !b)
          break;
        Value *ax = builder.CreateExtractElement(a, builder.getInt32(0));
        Value *bx = builder.CreateExtractElement(b, builder.getInt32(0));
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
        if (!cond)
          break;
        builder.CreateCondBr(cond, merge_bb, next_bb);
      }
      builder.SetInsertPoint(next_bb);
      break;
    }
    // SM 1.x texture-coordinate matrix pad opcodes — DXVK
    // dxso_compiler.cpp:196-199 treats both as literal no-ops with the
    // comment "We don't need to do anything here, these are just
    // padding instructions." The dependent TexM3x{2,3}Tex (and Spec /
    // VSpec / Depth) reads the pad ops' destination texcoord registers
    // by register-file lookup, not by reading state we'd have to
    // preserve from these instructions. Without an explicit no-op case
    // the default branch fires its one-shot "unhandled opcode" warn,
    // which is misleading — the pad ops genuinely don't need codegen.
    case DxsoOpcode::TexDepth: {
      // SM 1.4 only. Writes [[depth]] derived from the current contents
      // of the destination register (r5 implicitly per SM 1.4 docs;
      // dxmt reads temps[dst.base.num] to keep the formal "operand is
      // the destination register" shape). DXVK dxso_compiler.cpp:3117-
      // 3138; wined3d glsl_shader.c:6542-6555.
      //
      //   depth = clamp(r.x / min(r.y, 1.0), 0, 1)
      //
      // Clamp + min match wined3d's "tests show r.y is clamped to 1.0,
      // x stays" gloss. Negative x produces 0 via the saturate, matching
      // native driver behavior.
      if (is_vertex || !ins.has_dst || oDepth_arg_idx < 0 || !oDepth_slot)
        break;
      auto *gep_r = builder.CreateGEP(tempArrTy, temps, {builder.getInt32(0), builder.getInt32(ins.dst.base.num)});
      Value *r4 = builder.CreateLoad(float4Ty, gep_r);
      auto *fT = Type::getFloatTy(context);
      Value *rx = builder.CreateExtractElement(r4, builder.getInt32(0));
      Value *ry = builder.CreateExtractElement(r4, builder.getInt32(1));
      Value *one_f = ConstantFP::get(fT, 1.0f);
      Value *ry_clamped = air.CreateFPBinOp(llvm::air::AIRBuilder::fmin, ry, one_f);
      Value *depth = builder.CreateFDiv(rx, ry_clamped);
      depth = air.CreateFPUnOp(llvm::air::AIRBuilder::saturate, depth);
      // oDepth_slot is a float4; lane 0 is what the epilogue extracts
      // for [[depth]]. Splat for stable contents on the unused lanes.
      builder.CreateStore(builder.CreateVectorSplat(4, depth), oDepth_slot);
      break;
    }
    case DxsoOpcode::TexM3x2Depth: {
      // SM 1.3 only. Last row of a 3x2 matrix multiply combined with a
      // depth write. The preceding TexM3x2Pad provides row 0; this
      // opcode provides row 1 and emits depth. Since dxmt's TexM3x2Pad
      // is a no-op (recomputing here is cheaper than recording mid-pass
      // scratch state), this case re-derives both rows from tex_inputs
      // at slot-1 and slot, dots each against src[0], then:
      //
      //   depth = (row1·src == 0) ? 1 : clamp((row0·src) / (row1·src), 0, 1)
      //
      // wined3d glsl_shader.c:6563-6573 is the literal model; the
      // y==0 → 1.0 special-case mirrors wined3d's gloss.
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
        // Loop (not Rep) — step aL by stride. Stride may be zero
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
          std::fprintf(stderr, "warn: dxso unhandled opcode %u in %s shader\n", op_val, is_vertex ? "vs" : "ps");
        }
      }
      break;
    }
    }
    if (ins.opcode == DxsoOpcode::End)
      break;
  }

  // SM 1.x PS has no oC# register file — `r0` is the implicit pixel
  // output (D3D9 SDK ps_1_x reference; wined3d glsl_shader.c:7778-7780
  // emits the same `oC0 = R0` copy at the GLSL backend). The body
  // writes `mul r0, ...` through store_dst's Temp arm (temps[0]); the
  // retval assembly below loads from oC_slot[0] which is zero-init
  // unless we copy here. Without this every ps_1_x PS returns
  // (0,0,0,0) regardless of body — drove the NFS:MW black-screen even
  // after the input_arg_idx wiring fix made temps[0] carry useful data.
  if (!is_vertex && shader->header.major < 2 && oC_arg_idx[0] >= 0) {
    auto *r0_gep = builder.CreateGEP(tempArrTy, temps, {builder.getInt32(0), builder.getInt32(0)});
    Value *r0 = builder.CreateLoad(float4Ty, r0_gep);
    builder.CreateStore(r0, oC_slot[0]);
  }

  // D3D9 alpha test, lowered to discard_fragment per wined3d's GLSL
  // backend (dlls/wined3d/glsl_shader.c shader_glsl_generate_alpha_
  // test): "alpha_func is the PASS condition" — we compare oC0.a
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
      Value *ref =
          ConstantFP::get(Type::getFloatTy(context), static_cast<double>(ps_args->alpha_test_ref & 0xFF) / 255.0);
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
        pass = ConstantInt::getTrue(context);
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
      auto *pos = builder.CreateLoad(float4Ty, out_slot);
      retval = builder.CreateInsertValue(retval, pos, {0});
      // User clip planes — emit clip_distance[i] = (i < clip_count)
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
      // in pre-scan order. oC_arg_idx[N] gives the struct index.
      for (int i = 0; i < 4; ++i) {
        if (oC_arg_idx[i] < 0)
          continue;
        auto *v = builder.CreateLoad(float4Ty, oC_slot[i]);
        retval = builder.CreateInsertValue(retval, v, {(unsigned)oC_arg_idx[i]});
      }
      if (oDepth_arg_idx >= 0) {
        auto *v4 = builder.CreateLoad(float4Ty, oDepth_slot);
        Value *d = builder.CreateExtractElement(v4, builder.getInt32(0));
        retval = builder.CreateInsertValue(retval, d, {(unsigned)oDepth_arg_idx});
      }
    }
    if (sm12_vs_varyings) {
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
    }
    if (oPts_arg_idx >= 0 && oPts_slot) {
      // [[point_size]] is a scalar float — extract lane 0 from the
      // float4 storage slot (store_dst's plumbing writes the same
      // value to all four lanes for a scalar write like `mov oPts, c0.x`).
      auto *v4 = builder.CreateLoad(float4Ty, oPts_slot);
      Value *ps = builder.CreateExtractElement(v4, builder.getInt32(0));
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

  auto metadata = walk_dxso_shader(words, word_count, *header);
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
  // Mirrors SM50Destroy (dxbc_converter.cpp:1380-1382): C-style cast
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
  if (!pShader || !FunctionName)
    return -1;

  // Walk the argument chain. Recognised arg types: IA layout (VS),
  // PSO alpha-test (PS), PS sampler-kind layout (PS), PS point-sprite
  // (PS). Unknown types are silently skipped — same forgiveness
  // contract SM50 uses.
  DXSO_SHADER_IA_INPUT_LAYOUT_DATA *ia_layout = nullptr;
  DXSO_SHADER_PSO_PIXEL_SHADER_DATA *ps_args = nullptr;
  DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *ps_samp_layout = nullptr;
  DXSO_SHADER_PS_BUMP_ENV_DATA *ps_bump_env = nullptr;
  bool ps_point_sprite = false;
  float vs_point_size_override = 0.0f;
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
      vs_point_size_override = ((DXSO_SHADER_VS_POINT_SIZE_DATA *)arg)->value;
      break;
    case DXSO_SHADER_PS_BUMP_ENV:
      ps_bump_env = (DXSO_SHADER_PS_BUMP_ENV_DATA *)arg;
      break;
    case DXSO_SHADER_ARGUMENT_TYPE_MAX:
      break;
    }
  }

  LLVMContext context;
  context.setOpaquePointers(false);
  auto module = std::make_unique<Module>("dxso.air", context);
  dxmt::initializeModule(*module);
  dxmt::compile_dxso(
      (dxmt::DxsoShader *)pShader, ia_layout, ps_args, ps_samp_layout, ps_point_sprite, vs_point_size_override,
      ps_bump_env, FunctionName, context, *module
  );

  auto *compiled = new (std::nothrow) dxmt::DxsoBitcode();
  if (!compiled)
    return -1;
  llvm::raw_svector_ostream os(compiled->bytes);
  dxmt::metallib::MetallibWriter writer;
  writer.Write(*module, os);

  // Env-gated metallib dump — point DXMT_AIRCONV_DUMP at a directory
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
