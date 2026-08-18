#include "stddef.h"
#include "stdint.h"
#include "stdbool.h"

#ifndef __AIRCONV_H
#define __AIRCONV_H

#define AIRCONV_VERSION 24

#ifdef __cplusplus
#include <string>

enum class SM50BindingType : uint32_t {
  ConstantBuffer,
  Sampler,
  SRV,
  UAV,
};
#else
typedef uint32_t SM50BindingType;
#endif

enum SM50_BINDING_INDEX: uint32_t {
  SM50_BINDING_INDEX_CONSTANT_BUFFER = 0,
  SM50_BINDING_INDEX_ARGUMENT_TABLE = 1,
  SM50_BINDING_INDEX_VERTEX_BUFFER = 2,
  SM50_BINDING_INDEX_INDEX_BUFFER = 3,
  SM50_BINDING_INDEX_DRAW_ARGUMENTS = 4,
  SM50_BINDING_INDEX_INDIRECT_ARGUMENTS = 4,
  SM50_BINDING_INDEX_STREAM_OUTPUT0 = 5,
  /* For multiple stages in the same shader function */
  SM50_BINDING_INDEX_CONSTANT_BUFFER2 = 6,
  SM50_BINDING_INDEX_ARGUMENT_TABLE2 = 7,

  SM50_BINDING_INDEX_ROOT_ARGUMENTS = 0,
  SM50_BINDING_INDEX_STATIC_SAMPLERS = 1,
};

enum MTL_SM50_SHADER_ARGUMENT_FLAG : uint32_t {
  MTL_SM50_SHADER_ARGUMENT_BUFFER = 1 << 0,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE = 1 << 1,
  MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH = 1 << 2,
  MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER = 1 << 3,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP = 1 << 4,
  MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET = 1 << 5,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY = 1 << 6,
  MTL_SM50_SHADER_ARGUMENT_READ_ACCESS = 1 << 10,
  MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS = 1 << 11,
};

struct MTL_SM50_SHADER_ARGUMENT {
  SM50BindingType Type;
  /**
  bind point of it's corresponding resource space
  constant buffer:    cb1 -> 1
  srv:                t10 -> 10
  uav:                u0  -> 0
  sampler:            s2  -> 2
  */
  uint32_t SM50BindingSlot;
  enum MTL_SM50_SHADER_ARGUMENT_FLAG Flags;
  uint32_t StructurePtrOffset;
};

enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE {
  MTL_TESSELLATOR_OUTPUT_POINT = 1,
  MTL_TESSELLATOR_OUTPUT_LINE = 2,
  MTL_TESSELLATOR_OUTPUT_TRIANGLE_CW = 3,
  MTL_TESSELLATOR_TRIANGLE_CCW = 4
};

struct MTL_TESSELLATOR_REFLECTION {
  uint32_t Partition;
  float MaxFactor;
  enum MTL_TESSELLATOR_OUTPUT_PRIMITIVE OutputPrimitive;
};

struct MTL_GEOMETRY_SHADER_PASS_THROUGH {
  uint8_t RenderTargetArrayIndexReg;
  uint8_t RenderTargetArrayIndexComponent;
  uint8_t ViewportArrayIndexReg;
  uint8_t ViewportArrayIndexComponent;
};

struct MTL_GEOMETRY_SHADER_REFLECTION {
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t GSPassThrough;
  };
  uint32_t Primitive;
};

struct MTL_POST_TESSELLATOR_REFLECTION {
  uint32_t MaxPotentialTessFactor;
};

struct MTL_SHADER_REFLECTION {
  uint32_t ConstanttBufferTableBindIndex;
  uint32_t ArgumentBufferBindIndex;
  uint32_t NumConstantBuffers;
  uint32_t NumArguments;
  union {
    uint32_t ThreadgroupSize[3];
    struct MTL_TESSELLATOR_REFLECTION Tessellator;
    struct MTL_GEOMETRY_SHADER_REFLECTION GeometryShader;
    struct MTL_POST_TESSELLATOR_REFLECTION PostTessellator;
    uint32_t PSValidRenderTargets;
  };
  uint16_t ConstantBufferSlotMask;
  uint16_t SamplerSlotMask;
  uint64_t UAVSlotMask;
  uint64_t SRVSlotMaskHi;
  uint64_t SRVSlotMaskLo;
  uint32_t NumOutputElement;
  uint32_t ThreadsPerPatch;
  uint32_t ArgumentTableQwords;
};

#if defined(__LP64__) || defined(_WIN64)
typedef void *sm50_ptr64_t;
#else
typedef struct sm50_ptr64_t {
  uint64_t impl;

#ifdef __cplusplus
  sm50_ptr64_t() {
    impl = 0;
  }

  sm50_ptr64_t(void * ptr) {
    impl = (uint64_t)ptr;
  }

  sm50_ptr64_t(uint64_t v) {
    impl = v;
  }

  operator uint64_t () const {
    return impl;
  }
#endif

} sm50_ptr64_t;
#endif

typedef sm50_ptr64_t sm50_shader_t;
typedef sm50_ptr64_t sm50_bitcode_t;
typedef sm50_ptr64_t sm50_error_t;

/* DXSO (D3D9 SM1.x/2.x/3.x) compilation companion to SM50*. The
   lifecycle mirrors SM50Initialize / SM50Destroy / SM50Compile;
   the bytecode + reflection types are different (no DXBC chunks,
   no SRV/UAV/CB ranges). */
typedef sm50_ptr64_t dxso_shader_t;
typedef sm50_ptr64_t dxso_bitcode_t;

#ifdef _WIN32
#ifdef WIN_EXPORT
#define AIRCONV_API __declspec(dllexport)
#else
#define AIRCONV_API __declspec(dllimport)
#endif
#else
#define AIRCONV_API __attribute__((sysv_abi))
#endif

struct SM50_COMPILED_BITCODE {
  sm50_ptr64_t Data;
  uint64_t Size;
};

#ifdef __cplusplus

inline uint32_t
GetArgumentIndex(SM50BindingType Type, uint32_t SM50BindingSlot) {
  switch (Type) {
  case SM50BindingType::ConstantBuffer:
    return SM50BindingSlot;
  case SM50BindingType::Sampler:
    return SM50BindingSlot + 32;
  case SM50BindingType::SRV:
    return SM50BindingSlot * 3 + 128;
  case SM50BindingType::UAV:
    return SM50BindingSlot * 3 + 512;
  }
};

inline uint32_t GetArgumentIndex(struct MTL_SM50_SHADER_ARGUMENT &Argument) {
  return GetArgumentIndex(Argument.Type, Argument.SM50BindingSlot);
};

extern "C" {
#endif

enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE {
  SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT = 1,
  SM50_SHADER_COMMON = 2,
  SM50_SHADER_PSO_PIXEL_SHADER = 3,
  SM50_SHADER_IA_INPUT_LAYOUT = 4,
  SM50_SHADER_GS_PASS_THROUGH = 5,
  SM50_SHADER_PSO_GEOMETRY_SHADER = 6,
  SM50_SHADER_PSO_TESSELLATOR = 7,
  SM50_SHADER_ROOT_SIGNATURE = 8,
  SM50_SHADER_ARGUMENT_TYPE_MAX = 0xffffffff,
};

struct SM50_SHADER_COMPILATION_ARGUMENT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct SM50_STREAM_OUTPUT_ELEMENT {
  uint32_t reg_id;
  uint32_t component;
  uint32_t output_slot;
  uint32_t offset;
};

struct SM50_SHADER_EMULATE_VERTEX_STREAM_OUTPUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t num_output_slots;
  uint32_t num_elements;
  uint32_t strides[4];
  struct SM50_STREAM_OUTPUT_ELEMENT *elements;
};

enum SM50_SHADER_METAL_VERSION {
  SM50_SHADER_METAL_310 = 310,
  SM50_SHADER_METAL_320 = 320,
  SM50_SHADER_METAL_MAX = 0xffffffff,
};

enum SM50_SHADER_FLAG {
  SM50_SHADER_FLAG_SAMPLE_NAN_TO_ZERO = 1 << 0,
  SM50_SHADER_FLAG_DEFUSE_FMA = 1 << 1,
};

struct SM50_SHADER_COMMON_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_SHADER_METAL_VERSION metal_version;
  enum SM50_SHADER_FLAG flags;
};

struct SM50_SHADER_PSO_PIXEL_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t sample_mask;
  bool dual_source_blending;
  bool disable_depth_output;
  uint32_t unorm_output_reg_mask;
};

struct SM50_IA_INPUT_ELEMENT {
  uint32_t reg;
  uint32_t slot;
  uint32_t aligned_byte_offset;
  /** MTLAttributeFormat */
  uint32_t format;
  uint32_t step_function: 1;
  uint32_t step_rate: 31;
};

enum SM50_INDEX_BUFFER_FORMAT {
  SM50_INDEX_BUFFER_FORMAT_NONE = 0,
  SM50_INDEX_BUFFER_FORMAT_UINT16 = 1,
  SM50_INDEX_BUFFER_FORMAT_UINT32 = 2,
};

struct SM50_SHADER_IA_INPUT_LAYOUT_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum SM50_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  struct SM50_IA_INPUT_ELEMENT *elements;
};

struct SM50_SHADER_GS_PASS_THROUGH_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  union {
    struct MTL_GEOMETRY_SHADER_PASS_THROUGH Data;
    uint32_t DataEncoded;
  };
  bool RasterizationDisabled;
};

struct SM50_SHADER_PSO_GEOMETRY_SHADER_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  bool strip_topology;
};

struct SM50_SHADER_PSO_TESSELLATOR_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t max_potential_tess_factor;
};

struct SM50_SHADER_ROOT_SIGNATURE_DATA {
  void *next;
  enum SM50_SHADER_COMPILATION_ARGUMENT_TYPE type;
  const void* bytecode;
  size_t bytecode_length;
};

AIRCONV_API int SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader,
  struct MTL_SHADER_REFLECTION *pRefl, sm50_error_t *ppError
);
AIRCONV_API void SM50Destroy(sm50_shader_t pShader);

/* Parse + walk a DXSO bytecode blob, return an opaque handle that
   later passes (compile, signature query) consume. Returns 0 on
   success, non-zero if the header or instruction stream is
   malformed; *ppShader is set to NULL on failure. */
AIRCONV_API int DXSOInitialize(
  const void *pBytecode, size_t BytecodeSize, dxso_shader_t *ppShader
);
AIRCONV_API void DXSODestroy(dxso_shader_t pShader);

/* DXSO compilation argument chain: same shape as SM50's
   SM50_SHADER_COMPILATION_ARGUMENT_DATA: a typed linked list the
   caller threads from a head pointer, terminated when `next` is
   NULL. The DXSO compiler walks the chain at compile time and picks
   up specialization data (IA layout etc.) by argument type. NULL
   chain ⇒ no specialization (stage_in pass-through model). */
enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE {
  DXSO_SHADER_IA_INPUT_LAYOUT = 1,
  DXSO_SHADER_PSO_PIXEL_SHADER = 2,
  DXSO_SHADER_PS_SAMPLER_LAYOUT = 3,
  DXSO_SHADER_PS_POINT_SPRITE = 4,
  DXSO_SHADER_VS_POINT_SIZE = 5,
  /* Reserved (was PS bump-env): the TexBem D3DTSS_BUMPENV* constants now
     ride the shared PS uniform tail (buffer(2)) and the shader reads them
     at runtime, so no arg carries them. The value stays reserved to keep
     the numbering (and the shader-cache arg-type hash) stable. */
  DXSO_SHADER_PS_BUMP_ENV = 6,
  DXSO_SHADER_PS_FOG = 7,
  DXSO_SHADER_FFP_KEY = 8,
  DXSO_SHADER_ARGUMENT_TYPE_MAX = 0xffffffff,
};

/* Per-stage texture kind for the PS sampler layout. Values mirror
   airconv's internal DxsoTextureType: bypass the typedef to avoid
   leaking that header into the public API. 0 = "no binding /
   unknown" (leave as the shader's compile-time default); 2 = Texture2D;
   3 = TextureCube; 4 = Texture3D; 5 = Texture2D bound to a Metal
   depth-format texture (INTZ / DF24 / DF16 / auto-DS reuse), which
   needs depth2d<float> in MSL instead of texture2d<float>: Metal's
   texture2d sampling of a Depth32Float_Stencil8 returns the depth in
   .r with .gba documented as "undefined", which produces a visible
   flake-and-flicker pattern on depth-sampling draws. depth2d returns a
   single float that we wire back to all four
   channels per the NVIDIA/ATI INTZ contract (depth replicated). */
enum DXSO_PS_SAMPLER_KIND {
  DXSO_PS_SAMPLER_KIND_UNKNOWN = 0,
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D = 2,
  DXSO_PS_SAMPLER_KIND_TEXTURE_CUBE = 3,
  DXSO_PS_SAMPLER_KIND_TEXTURE_3D = 4,
  // 2D depth texture sampled for its RAW depth (INTZ / RAWZ): texld
  // returns the stored depth replicated to .xyzw for in-shader compare.
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH = 5,
  // 2D depth texture sampled with HARDWARE PCF (D24S8/D16 bound as a
  // texture, DF24, DF16): texld/texldp returns the filtered comparison
  // of ref (coord.z, or coord.z/coord.w when projective) against the
  // stored depth: Metal sample_compare on a depth2d + a LessEqual
  // compareFunction sampler. Same Metal depth2d kind as _TEXTURE_2D_DEPTH;
  // only the sample op + sampler compare func differ.
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_COMPARE = 6,
  /* AMD FETCH4 (D3DSAMP_MIPMAPLODBIAS magic 'GET4'): point-sampling a
     single-channel color format gathers the four neighbouring texels
     into (B, R, G, A) order instead of filtering. The host resolves the
     latch + format + point-filter gate and picks this kind. */
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4 = 7,
  /* Two-channel signed formats (V8U8, V16U16): Metal snorm converts as
     c over 2^(n-1) minus 1 while D3D9 divides by 2^(n-1), and the
     missing z and w channels read one. The Tex arm rescales the sampled
     x and y and forces z = w = 1. The scale leaves the most negative
     code at -0.992 where D3D9 pins -1; wine's test data never probes
     it. */
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_SNORM2_8 = 8,
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_SNORM2_16 = 9,
  /* Raw-depth texture with the FETCH4 latch armed: the sample gathers
     the four neighbouring depth values (depth2d gather) through the
     same coordinate nudge and B, R, G, A order the colour FETCH4 kind
     uses. Only the raw-depth trio (INTZ, DF16, DF24) takes this kind;
     the hardware-PCF formats keep their compare sampling. */
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_FETCH4 = 10,
  /* DF16 / DF24 sampled without the FETCH4 latch: raw depth lands in
     the red channel only, (d, 0, 0, 1); INTZ keeps the full replicate
     (DXVK expresses the same split as view swizzles, R001 vs RRRR). */
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_DEPTH_DF = 11,
  /* The FETCH4 latch armed on a format outside the single-channel
     set. The vendor hardware returns zero for the plain sample forms
     and degrades only the projected form to a normal sample; wine's
     fetch4 format rows pin both sides. */
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4_BROKEN = 12,
  /* FETCH4 on a block-compressed single-channel format: the vendor
     hardware does not gather across a compressed block, it replicates
     the point-sampled red to all four lanes (wine's ATI1 fetch4 rows
     pin the uniform result for both plain and projected forms). */
  DXSO_PS_SAMPLER_KIND_TEXTURE_2D_FETCH4_REPLICATE = 13,
};

struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

/* Per-attribute element in the input layout: mirrors
   SM50_IA_INPUT_ELEMENT verbatim (same field order, same widths).
   `reg` is the v# register number the shader's dcl_<usage> bound the
   semantic to; the host resolves that mapping by walking the
   declaration + VS metadata before compile. `format` is a
   WMTAttributeFormat. */
/* D3DDECLTYPE_UDEC3 has no Metal attribute format: its three 10-bit channels
   are unsigned and NOT normalized, and Metal only offers the normalized pair.
   So `format` carries this dxmt-private value for it, chosen far above every
   real MTLVertexFormat. That is safe only because the Direct3D 9 path fetches
   vertices in the shader, so the value reaches the DXSO codegen and never an
   attribute descriptor; the Direct3D 11 path hands its format straight to
   Metal and must keep using the real one. */
#define DXSO_ATTR_FORMAT_UDEC3 0x1000u

struct DXSO_IA_INPUT_ELEMENT {
  uint32_t reg;
  uint32_t slot;
  uint32_t aligned_byte_offset;
  uint32_t format;
  uint32_t step_function: 1;
  uint32_t step_rate: 31;
};

/* Index buffer format the compiled VS variant should assume: the
   DXSO codegen needs this when an indexed draw routes through
   manual fetch and the shader has to do its own gather. NONE is
   the only correct value for non-indexed draws. Mirrors
   SM50_INDEX_BUFFER_FORMAT verbatim. */
enum DXSO_INDEX_BUFFER_FORMAT {
  DXSO_INDEX_BUFFER_FORMAT_NONE = 0,
  DXSO_INDEX_BUFFER_FORMAT_UINT16 = 1,
  DXSO_INDEX_BUFFER_FORMAT_UINT32 = 2,
};

struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum DXSO_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  struct DXSO_IA_INPUT_ELEMENT *elements;
  /* Pre-transformed (D3DDECLUSAGE_POSITIONT / D3DFVF_XYZRHW) draw: the
     position stream is already in window space, so the VS must remap it
     to clip space (screen->NDC + rhw divide) instead of passing it
     through. The host packs invExtent/invOffset into a VS uniform at
     location 5 (see compile_dxso). Matches wined3d position_transformed /
     DXVK HasPositionT. */
  uint32_t position_transformed;
  /* Size of the vertex float constant register file the host binds for this
     compile: 256 on a hardware-VP device, up to 8192 (D3D9_MAX_VS_CONST_F_SWVP)
     on a software / mixed-VP device. The DXSO codegen uses it as the direct
     c# ceiling and the relative-index (c[a0.x]) clamp, so a reladdr shader on
     an SWVP device can reach the extended constants the host uploads. 0 means
     "hardware ceiling" for an older host that does not set it. */
  uint32_t vs_float_const_count;
};

/* PS-only specialisation: bake the D3D9 alpha test compare into the
   metallib at compile time. Metal has no fixed-function alpha test; the
   comparison has to land in the fragment shader as a discard_fragment,
   same as wined3d's GLSL backend. alpha_test_func encodes the D3DCMP_*
   PASS condition (D3DCMP_ALWAYS skips emit; D3DCMP_NEVER unconditionally
   discards) and is the only alpha-test axis that keys the metallib. The
   ref (D3DRS_ALPHAREF, 0..255 normalised to alpha space) is NOT here: it
   rides the shared PS uniform tail (buffer(2), uint32 index 28) and the
   shader reads it at runtime, so shaders that differ only in the animated
   ref share one variant. DXVK splits it the same way (the compare
   function is a spec constant, the ref a uniform). */
struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t alpha_test_func;
  /* When the blend state reads SRC1 factors (D3DBLEND_SRCCOLOR2 /
     INVSRCCOLOR2), oC0/oC1 must be declared as the two color indices of
     attachment 0 ([[color(0), index(0)]] / [[color(0), index(1)]])
     rather than two separate attachments, the same shape DXBC takes
     under SM50_SHADER_PSO_PIXEL_SHADER_DATA::dual_source_blending. The
     host sets this only when the active blend factors require it, so
     the normal MRT case (oC1 as attachment 1) is untouched. */
  uint32_t dual_source_blending;
  /* D3DSHADE_FLAT: color-usage inputs interpolate flat for shader
     models below 3 (native leaves SM3 smooth; wine's shademode rows
     mark the SM3 flat case as broken). */
  uint32_t flat_shading;
  /* D3DRS_MULTISAMPLEMASK on a maskable multisample target: emit the
     [[sample_mask]] coverage output so the fragment covers only the
     enabled samples. Only the 1-bit enable keys the metallib; the 32-bit
     mask word rides the shared PS uniform tail (buffer(2), uint32 index
     29) and the shader reads it at runtime, so an animated mask shares one
     variant. The host sets this only when the mask differs from all-ones
     AND the RT is genuinely multisampled (a cleared bit on a 1-sample
     target must not kill fragments). Mirrors the ref / bump-env split. */
  uint32_t emit_sample_mask;
  /* Per-color-attachment 8-bit-UNORM snap mask: bit i set means the shader's
     oC<i> output targets a LINEAR 8-bit UNORM render target, so the PS epilogue
     snaps that output to the nearest k/255 with round-half-to-even (rint)
     before the store. Metal's unorm-write conversion rounds an exact half away
     from zero while D3D/WARP rounds to even, so a channel landing on k.5 (e.g.
     76.5) would store 77 where WARP stores 76; rint sends it to the even bucket
     and off the ambiguous half, and is a no-op for every off-half value. Cleared
     for float / HDR / sRGB targets (an sRGB attachment applies its own transfer
     curve), so their precision is untouched. The host resolves the mask from the
     bound RT formats via IsUnorm8RenderTargetFormat; it is a compile-time
     constant carrying no runtime blob. Mirrors the SM50 (DXBC) sibling's
     SM50_SHADER_PSO_PIXEL_SHADER_DATA::unorm_output_reg_mask. */
  uint32_t unorm_output_reg_mask;
};

/* PS-only specialisation: per-stage texture kind. SM 1.0..1.3 PS has
   no dcl_2d / dcl_cube / dcl_volume tokens: the sampler dimensionality
   is implicit and would otherwise default to Texture2D in
   dxso_compile.cpp. When an app binds a non-2D texture (e.g. an env-map
   cube for reflections) to a shader compiled as 2D, Metal's shader
   validation flags the type mismatch and the GPU samples undefined
   data: visible flicker on the affected draws.

   The host snapshots cap.textures[stage].commonTextureType() at draw
   resolve time, converts to DXSO_PS_SAMPLER_KIND_*, and threads this
   arg into the compile chain. Slots set to UNKNOWN fall back to the
   shader's own inference (which is correct for SM 2.0+ dcl-bearing
   shaders). PS variants cache per (alpha-test, sampler-kind-layout)
   tuple in MTLD3D9PixelShader. */
struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint8_t kinds[16];
};

/* PS-only specialisation: enable D3DRS_POINTSPRITEENABLE rewriting.
   When this arg is present, the compiled PS substitutes every
   TEXCOORD<N> stage-in read with float4(point_coord.x, point_coord.y,
   0, 1) sourced from Metal's [[point_coord]] fragment input. D3D9 spec
   replaces ALL texcoord inputs at the PS under POINTSPRITEENABLE
   (regardless of D3DTSS_TEXCOORDINDEX) so the substitution applies
   uniformly. Only meaningful for point-list primitive draws; the host
   gates the variant key on both D3DRS_POINTSPRITEENABLE != 0 AND
   primitive_type == D3DPT_POINTLIST so non-point draws don't pay the
   variant-cache cost. */
struct DXSO_SHADER_PS_POINT_SPRITE_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

/* Fog factor source for DXSO_SHADER_PS_FOG_DATA. VERTEX takes the
   factor straight from the VS oFog varying (the legacy blend). The
   table modes compute the factor per fragment from the window-space
   depth, mirroring D3D9's table (pixel) fog. The mode lands in the
   PS variant key (four slots max); FOGSTART / FOGEND / FOGDENSITY stay
   runtime data in the bool-constant blob so they never fork a PSO. */
enum DXSO_PS_FOG_MODE {
  DXSO_PS_FOG_MODE_VERTEX = 0,
  DXSO_PS_FOG_MODE_LINEAR = 1,
  DXSO_PS_FOG_MODE_EXP = 2,
  DXSO_PS_FOG_MODE_EXP2 = 3,
  /* Table mode NONE with a non-fog-writing vertex stage (bytecode VS
     without an oFog write, or a pre-transformed fixed-function draw):
     the blend factor is the interpolated specular alpha, with the fog
     params ignored. test_fog pins this from hardware behavior. */
  DXSO_PS_FOG_MODE_SPECULAR_ALPHA = 4,
};

/* PS-only specialisation: D3D9 fog blend. When present, the compiled
   PS (ps < 3_0 only; ps_3_0 computes fog itself per spec) emits
   rgb = mix(fog_color, rgb, saturate(fog)).

   mode == VERTEX takes the factor from the FOG0 varying (VS oFog), the
   legacy fixed vertex-fog blend wined3d and DXVK bake into their
   generated pixel shaders. The table modes (LINEAR / EXP / EXP2) ignore
   the varying and derive the factor from the fragment's window-space
   depth, which D3D9 spells z * (1/w): the [[position]] built-in already
   carries window depth (z/w) in .z and 1/w in .w, so depth = .z / .w,
   matching DXVK d3d9_fixed_function.cpp DoFixedFunctionFog (it computes
   z * (1.0 / w) from gl_FragCoord, same semantics). FOGTABLEMODE takes
   priority over vertex fog per the D3D9 contract.

   The fog colour is read at runtime from the bool-constant buffer tail
   (buffer(2), float4 at byte offset 16); the table-fog params follow it
   (float fog_start / fog_end / fog_density at byte offset 96), so the
   mode is the only thing in the variant key. The host gates the variant
   on FOGENABLE + ps.major < 3; a vertex-fog VS that writes no oFog means
   factor 1.0 and an identity lerp. */
struct DXSO_SHADER_PS_FOG_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t mode;
  /* Table-fog coordinate source: nonzero samples eye-space w (the
     reciprocal of the interpolated position's w), zero samples the
     device z. The host derives the choice from the projection matrix
     the way wined3d does (a typical perspective matrix selects w) and
     forces z for pre-transformed draws. */
  uint32_t coord_is_w;
};

/* VS-only marker: emit the AIR [[point_size]] output for a point-list
   draw. When this arg is present the VS binds a point-size uniform at
   VS buffer 6 (float4 = size, min, max) and its epilogue writes
   clamp(size-or-shader-oPts, min, max) into [[point_size]]. The size
   and bounds ride the uniform rather than a baked constant, so ONE VS
   variant serves every point size instead of minting one per value.
   The host gates the marker on primitive_type == D3DPT_POINTLIST (plus,
   for a VS that does not write oPts, a clamped size that leaves the 1.0
   default). DXVK clamps at the same epilogue point against the point-size
   values it supplies as push data. */
struct DXSO_SHADER_VS_POINT_SIZE_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

/* Fixed-function shader request: a DXSOCompile call whose pShader is
   null and whose chain carries this key compiles a generated
   fixed-function shader instead of translating bytecode. The vertex
   side pairs the key with the regular IA-layout argument; register
   conventions between the host layout and the generated fetch are
   reg 0 = position (POSITIONT when the layout says so), reg 1 =
   diffuse. The struct only grows (append-only), so the host versions
   it by milestone rather than reshaping. */
struct DXSO_SHADER_FFP_KEY_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  /* 0 = vertex stage, 1 = pixel stage */
  uint32_t kind;
  /* VS: the layout carries a diffuse element on reg 1; when clear the
     generated shader feeds COLOR0 with opaque white. */
  uint32_t has_diffuse;
  /* VS: the layout carries a texcoord element on reg 2, forwarded to
     the TEXCOORD0 varying (the stub seed otherwise). */
  uint32_t has_texcoord0;
  /* VS: the layout carries a specular element on reg 3, forwarded to
     the COLOR1 varying; the specular-alpha fog source reads it. */
  uint32_t has_specular;
  /* PS: 0 passes the interpolated diffuse through; 1 modulates it with
     the stage-0 texture sampled at TEXCOORD0; 2 runs the per-stage
     combiner table below. */
  uint32_t tex0_mode;
  /* PS combiner table (tex0_mode 2), stage-packed. Word 0 carries
     D3DTSS_COLOROP in bits 0..7, ALPHAOP in 8..15, flags in 16..23
     (bit 16 = a texture is bound, bit 17 = D3DTSS_RESULTARG is TEMP,
     bit 18 = projected); each stage samples its own varying. Words 1
     and 2 pack the three D3DTSS_COLORARG / ALPHAARG values as bytes
     (arg1, arg2, arg0). */
  uint32_t stages[8][3];
  /* VS: nonzero emits the [[point_size]] output for point-list draws;
     0 means no point-size output (non-point draws). The size and its
     clamp bounds ride the uniforms block (float4 9 = size/min/max), so
     one generated variant serves every point size. */
  uint32_t point_size;
  /* PS: nonzero substitutes [[point_coord]] for the TEXCOORD0 sample
     coordinate (D3DRS_POINTSPRITEENABLE on a point-list draw). */
  uint32_t point_sprite;
  /* VS: D3DRS_POINTSCALEENABLE distance attenuation. The size, clamp
     bounds, scale factors and viewport height ride the uniforms block
     (float4 6 = world*view x column, 7 = y column, 8 = A/B/C/viewport
     height, 9 = size/min/max); the plain size arm reads the same block. */
  uint32_t point_scale;
  /* VS: bit N forwards TEXCOORDn. Texcoord 0 rides layout reg 2 (the
     original contract) and texcoords 1..7 ride regs 5 + n. */
  uint32_t texcoord_mask;
  /* VS texcoord transforms, one meaningful bit per stage (four-bit stride):
     bit 0 enables the texture matrix (float4 72 + 4*stage in the uniforms).
     The D3DTTFF_COUNT / D3DTTFF_PROJECTED behaviour is folded into the matrix
     at upload (the wined3d get_texture_matrix transcription), not keyed, so
     distinct count/projected values share one generated shader. */
  uint32_t texcoord_transform_key;
  /* VS lighting key. Bit 0 = lighting enabled, bit 1 = the layout
     carries a normal (reg 4), bit 2 = D3DRS_SPECULARENABLE, bit 3 =
     D3DRS_NORMALIZENORMALS, bit 4 = D3DRS_LOCALVIEWER, bit 5 =
     D3DRS_COLORVERTEX; bits 8..15 pack the four material source
     selectors (diffuse, specular, ambient, emissive; two bits each,
     0 material, 1 color1, 2 color2). The material, global ambient and
     the enabled lights ride the uniforms block from float4 10; the
     eye-space normal uses the inverse-transpose of the matrix-0
     world*view (float4 125..127) so lighting holds under non-uniform
     scale. */
  uint32_t lighting_key;
  /* VS: D3DRS_FOGVERTEXMODE when vertex fog is active (D3DFOG_EXP,
     _EXP2 or _LINEAR); 0 (D3DFOG_NONE) emits the zero fog factor. The
     factor computes from the view-space depth in the uniforms block
     and lands in the FOG0 varying; the pixel stage's fog blend
     consumes it through the same argument the bytecode path uses. */
  uint32_t fog_vertex_mode;
  /* VS: D3DRS_VERTEXBLEND declared weight count (1..3); 0 disables.
     The clip position blends across world matrices 0..count with the
     last weight implied as one minus the declared sum (the wined3d
     vertex pipe shape); weights ride layout reg 12. The extra
     world*view*projection products sit after the texture matrices in
     the uniforms (float4 104 + 4*(i-1)), and the matching world*view
     columns for the blended eye-space position and normal at float4
     116 + 3*(i-1). Every eye-space consumer (lighting, fog, texgen,
     point scale) reads the same blended eye position and normal. */
  uint32_t vertex_blend;
  /* VS: D3DTSS_TCI_* texture generation, three bits per stage holding
     the TEXCOORDINDEX high word (1 camera-space normal, 2 position,
     3 reflection vector, 4 sphere map; 0 passes the fetched set
     through). Generated stages write the stage's own varying and
     ignore the low coordinate index (wined3d utils.c). Bit 24 carries
     D3DRS_NORMALIZENORMALS for the generated normal. */
  uint32_t texgen_key;
  /* VS: the D3DTSS_TEXCOORDINDEX low bits, three per stage. Varyings
     are per stage (wined3d's model): stage n's varying carries the
     fetched set this field names, pushed through stage n's texture
     matrix; the pixel side always samples its own stage's varying. */
  uint32_t texcoord_index_key;
  /* PS: DXSO_PS_SAMPLER_KIND_* per stage, four bits each. Drives the
     combiner's per-stage texture kind (2D, cube, volume), the FETCH4
     gather and the signed-format rescale, the same host resolution
     the bytecode variants receive through their PSO argument. Depth
     kinds stay unhandled in the combiner, a marked gap. */
  uint32_t sampler_kind_key;
  /* PS: D3DSHADE_FLAT. The COLOR0 / COLOR1 fragment inputs interpolate
     flat (Metal's provoking vertex is the first, matching D3D9); the
     mixed pair of a generated vertex stage with a bytecode pixel
     shader keeps smooth interpolation, a marked gap. */
  uint32_t flat_shading;
  /* VS: the declaration carries D3DDECLUSAGE_PSIZE on layout reg 13
     and the point size comes from the vertex, clamped by the same
     bounds the uniforms carry; the scale-attenuation path uses it as
     the base size the same way. */
  uint32_t point_size_per_vertex;
  /* VS: the declaration carries a diffuse (COLOR0) element even when no
     stream feeds it (has_diffuse clear). wined3d keys the unlit diffuse
     default on the DECL (utils.c): a declared-but-unbound diffuse renders
     the zero attribute default (0,0,0,0), while a decl with no diffuse at
     all takes the D3D9 opaque-white no-color default. */
  uint32_t decl_has_diffuse;
  /* VS: D3DRS_RANGEFOGENABLE with active vertex fog. When set, the fog
     coordinate is the radial eye-space distance length(eye_pos.xyz) rather
     than the view-space z, so screen-edge objects fog by true distance
     (wined3d WINED3D_FFP_VS_FOG_RANGE, DXVK RangeFog). Range fog affects
     vertex fog only, never table fog; 0 keeps the planar depth. */
  uint32_t range_fog;
  /* PS: D3DRS_MULTISAMPLEMASK on a maskable multisample target. Emits the
     [[sample_mask]] coverage output that ANDs the app mask (from the shared
     PS uniform tail, uint32 index 29) into hardware coverage. Only the 1-bit
     enable keys the generated combiner; the mask word itself rides the tail,
     so an animated mask shares one variant. Inert on the vertex kind. */
  uint32_t emit_sample_mask;
};

/* The TexBem / TexBemL / Bem per-stage D3DTSS_BUMPENV* constants (2x2
   matrix, luminance scale + offset) are no longer a compile argument:
   they ride the shared PS uniform tail (buffer(2)), host-written per draw
   and read at runtime, exactly like DXVK's D3D9SharedPS uniform. Baking
   the animated floats into the variant used to mint a cold PSO link per
   frame; the enum value DXSO_SHADER_PS_BUMP_ENV stays reserved. */

/* Compile the previously-initialized DXSO shader to an AIR metallib.
   pArgs is the head of a DXSO_SHADER_COMPILATION_ARGUMENT_DATA chain
   (NULL for unspecialized). FunctionName is the entry-point name
   baked into the resulting metallib. Returns 0 on success, non-zero
   on failure (the caller should treat the bitcode handle as
   invalid). */
AIRCONV_API int DXSOCompile(
  dxso_shader_t pShader,
  struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName,
  dxso_bitcode_t *ppBitcode
);
AIRCONV_API void DXSOGetCompiledBitcode(
  dxso_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData
);
AIRCONV_API void DXSODestroyBitcode(dxso_bitcode_t pBitcode);

AIRCONV_API int SM50Compile(
  sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API void SM50GetCompiledBitcode(
  sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData
);
AIRCONV_API void SM50DestroyBitcode(sm50_bitcode_t pBitcode);
AIRCONV_API size_t SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize);
AIRCONV_API void SM50FreeError(sm50_error_t pError);

AIRCONV_API int SM50CompileTessellationPipelineHull(
  sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileTessellationPipelineDomain(
  sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API int SM50CompileGeometryPipelineVertex(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);
AIRCONV_API int SM50CompileGeometryPipelineGeometry(
  sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
);

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
);

#ifdef __cplusplus
};

inline std::string SM50GetErrorMessageString(sm50_error_t pError) {
  std::string str;
  str.resize(256);
  auto size = SM50GetErrorMessage(pError, str.data(), str.size());
  str.resize(size);
  return str;
};

#endif

#endif
