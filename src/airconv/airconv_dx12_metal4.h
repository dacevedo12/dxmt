#pragma once

#include "airconv_public.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DX12-only Airconv ABI.
 *
 * The legacy airconv_public.h entry points are kept for the DX11/SM50 path.
 * D3D12 uses these names through winemetal4 so the Metal4 shader ABI can evolve
 * independently from the legacy argument-buffer ABI.
 */
typedef sm50_shader_t dxmt12_airconv_shader_t;
typedef sm50_bitcode_t dxmt12_airconv_bitcode_t;
typedef sm50_error_t dxmt12_airconv_error_t;
typedef dxmt12_airconv_shader_t dxil_shader_t;

typedef struct MTL_SHADER_REFLECTION DXMT12_MTL4_SHADER_REFLECTION;
typedef struct MTL_SM50_SHADER_ARGUMENT DXMT12_MTL4_SHADER_ARGUMENT;

AIRCONV_API int DXMT12SM50Initialize(
  const void *pBytecode, size_t BytecodeSize, dxmt12_airconv_shader_t *ppShader,
  DXMT12_MTL4_SHADER_REFLECTION *pRefl, dxmt12_airconv_error_t *ppError
);
AIRCONV_API void DXMT12SM50Destroy(dxmt12_airconv_shader_t pShader);
AIRCONV_API int DXMT12SM50Compile(
  dxmt12_airconv_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, dxmt12_airconv_bitcode_t *ppBitcode,
  dxmt12_airconv_error_t *ppError
);
AIRCONV_API void DXMT12SM50GetCompiledBitcode(
  dxmt12_airconv_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData
);
AIRCONV_API void DXMT12SM50DestroyBitcode(dxmt12_airconv_bitcode_t pBitcode);
AIRCONV_API size_t DXMT12SM50GetErrorMessage(
  dxmt12_airconv_error_t pError, char *pBuffer, size_t BufferSize
);
AIRCONV_API void DXMT12SM50FreeError(dxmt12_airconv_error_t pError);

AIRCONV_API int DXMT12SM50CompileTessellationPipelineHull(
  dxmt12_airconv_shader_t pVertexShader, dxmt12_airconv_shader_t pHullShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs,
  const char *FunctionName, dxmt12_airconv_bitcode_t *ppBitcode,
  dxmt12_airconv_error_t *ppError
);
AIRCONV_API int DXMT12SM50CompileTessellationPipelineDomain(
  dxmt12_airconv_shader_t pHullShader, dxmt12_airconv_shader_t pDomainShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs,
  const char *FunctionName, dxmt12_airconv_bitcode_t *ppBitcode,
  dxmt12_airconv_error_t *ppError
);
AIRCONV_API int DXMT12SM50CompileGeometryPipelineVertex(
  dxmt12_airconv_shader_t pVertexShader, dxmt12_airconv_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs,
  const char *FunctionName, dxmt12_airconv_bitcode_t *ppBitcode,
  dxmt12_airconv_error_t *ppError
);
AIRCONV_API int DXMT12SM50CompileGeometryPipelineGeometry(
  dxmt12_airconv_shader_t pVertexShader, dxmt12_airconv_shader_t pGeometryShader,
  struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs,
  const char *FunctionName, dxmt12_airconv_bitcode_t *ppBitcode,
  dxmt12_airconv_error_t *ppError
);
AIRCONV_API void DXMT12SM50GetArgumentsInfo(
  dxmt12_airconv_shader_t pShader, DXMT12_MTL4_SHADER_ARGUMENT *pConstantBuffers,
  DXMT12_MTL4_SHADER_ARGUMENT *pArguments
);

AIRCONV_API int DXMT12DXILInitialize(
  const void *pBytecode, size_t BytecodeSize, dxmt12_airconv_shader_t *ppShader,
  DXMT12_MTL4_SHADER_REFLECTION *pRefl, dxmt12_airconv_error_t *ppError
);
AIRCONV_API void DXMT12DXILDestroy(dxmt12_airconv_shader_t pShader);
AIRCONV_API int DXMT12DXILCompile(
  dxmt12_airconv_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
  const char *FunctionName, dxmt12_airconv_bitcode_t *ppBitcode,
  dxmt12_airconv_error_t *ppError
);
AIRCONV_API void DXMT12DXILGetArgumentsInfo(
  dxmt12_airconv_shader_t pShader, DXMT12_MTL4_SHADER_ARGUMENT *pConstantBuffers,
  DXMT12_MTL4_SHADER_ARGUMENT *pArguments
);

#ifdef __cplusplus
};

namespace dxmt::dxil {
struct DxilTranslationInfo;
}

namespace dxmt::airconv {
const dxil::DxilTranslationInfo *
GetDxmt12DxilTranslationInfo(dxmt12_airconv_shader_t pShader);
}

inline std::string DXMT12SM50GetErrorMessageString(dxmt12_airconv_error_t pError) {
  std::string str;
  str.resize(256);
  auto size = DXMT12SM50GetErrorMessage(pError, str.data(), str.size());
  str.resize(size);
  return str;
};

#endif
