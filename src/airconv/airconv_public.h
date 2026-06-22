#include "stddef.h"
#include "stdint.h"
#include "stdbool.h"

#ifndef __AIRCONV_H
#define __AIRCONV_H

#ifdef __cplusplus
#include <string>
enum class ShaderType {
  Vertex,
  /* Metal: fragment function */
  Pixel,
  /* Metal: kernel function */
  Compute,
  /* Not present in Metal */
  Hull,
  /* Metal: post-vertex function */
  Domain,
  /* Not present in Metal */
  Geometry,
  Mesh,
  /* Metal: object function */
  Amplification,
};

enum class SM50BindingType : uint32_t {
  ConstantBuffer,
  Sampler,
  SRV,
  UAV,
};
#else
typedef uint32_t ShaderType;
typedef uint32_t SM50BindingType;
#endif

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
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED = 1 << 12,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_3D = 1 << 13,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_CUBE = 1 << 14,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_UINT = 1 << 15,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_SINT = 1 << 16,
  MTL_SM50_SHADER_ARGUMENT_TEXTURE_DEPTH = 1 << 17,
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
  uint32_t RegisterSpace;
  uint32_t RegisterLowerBound;
  uint32_t RegisterCount;
  uint32_t CBufferSizeInVec4;
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
  uint64_t demote_msaa_srv_mask_lo;
  uint64_t demote_msaa_srv_mask_hi;
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
  DXSO_SHADER_PS_BUMP_ENV = 6,
  DXSO_SHADER_PS_FOG = 7,
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
};

/* PS-only specialisation: bake the D3D9 alpha test
   (D3DRS_ALPHATESTENABLE / D3DRS_ALPHAFUNC / D3DRS_ALPHAREF) into the
   metallib at compile time. Metal has no fixed-function alpha test;
   the comparison has to land in the fragment shader as a
   discard_fragment, same as wined3d's GLSL backend. alpha_test_func
   encodes the D3DCMP_* PASS condition (D3DCMP_ALWAYS skips emit;
   D3DCMP_NEVER unconditionally discards). alpha_test_ref is the
   D3DRS_ALPHAREF DWORD (0..255), normalised to alpha space at emit
   time. The host caches PS variants per (func, ref) tuple: apps
   typically settle on one or two combinations. */
struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t alpha_test_func;
  uint32_t alpha_test_ref;
  /* When the blend state reads SRC1 factors (D3DBLEND_SRCCOLOR2 /
     INVSRCCOLOR2), oC0/oC1 must be declared as the two color indices of
     attachment 0 ([[color(0), index(0)]] / [[color(0), index(1)]])
     rather than two separate attachments, the same shape DXBC takes
     under SM50_SHADER_PSO_PIXEL_SHADER_DATA::dual_source_blending. The
     host sets this only when the active blend factors require it, so
     the normal MRT case (oC1 as attachment 1) is untouched. */
  uint32_t dual_source_blending;
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
};

/* VS-only specialisation: D3DRS_POINTSIZE auto-injection. When this
   arg is present, the compiled VS unconditionally writes `value` to
   the AIR [[point_size]] output, regardless of whether the bytecode
   touches oPts. The host gates the variant on (primitive_type ==
   D3DPT_POINTLIST AND vs.metadata.writes_point_size == false AND
   rs[D3DRS_POINTSIZE] != 1.0) so the common case (VS computes its own
   point size, or non-point primitive, or default 1.0) keeps the
   default-variant cache slot. DXVK src/dxso/dxso_compiler.cpp
   does the equivalent via SPIR-V opStore at the same epilogue point. */
struct DXSO_SHADER_VS_POINT_SIZE_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  float value;
};

/* PS-only specialisation: per-stage D3DTSS_BUMPENV* constants for
   TexBem / TexBemL / Bem opcodes. Indexed by texture stage (0..7,
   matching dxmt's m_textureStageStates[8] storage). Per-stage layout:
   mat[0][0], mat[0][1], mat[1][0], mat[1][1] (2x2 bump-env matrix used
   by all three opcodes), then lscale + loffset (TexBemL only: ignored
   for TexBem and Bem). The host gates the variant on
   ps.metadata.bem_stage_mask != 0; non-bem shaders never see this arg
   and keep their default variant key. SM 1.x-only opcodes per the
   D3D9 spec, so the slot count caps at the SM 1.x sampler limit (4
   active stages typical) and the upper stages stay zero-filled.
   DXVK src/dxso/dxso_compiler.cpp + 2790-2803 + 2968-2987
   do the equivalent via a shared PS uniform buffer; dxmt bakes the
   constants into the variant since SM 1.x apps set bump-env once at
   init. */
struct DXSO_SHADER_PS_BUMP_ENV_DATA {
  void *next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  /* mat[stage] = {bm00, bm01, bm10, bm11} ; padded to vec4 for
     std140-friendliness even though dxmt's AIR side doesn't need it. */
  float mat[8][4];
  float lscale[8];
  float loffset[8];
};

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
