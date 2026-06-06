#define WIN_EXPORT
#include "wineunixlib.h"
#include "airconv_thunks.h"

AIRCONV_API int
SM50Initialize(
    const void *pBytecode, size_t BytecodeSize, sm50_shader_t *ppShader, struct MTL_SHADER_REFLECTION *pRefl,
    sm50_error_t *ppError
) {
  struct sm50_initialize_params params;
  params.bytecode = pBytecode;
  params.bytecode_size = BytecodeSize;
  params.shader = ppShader;
  params.reflection = pRefl;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_initialize, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void
SM50Destroy(sm50_shader_t pShader) {
  struct sm50_destroy_params params;
  params.shader = pShader;
  UNIX_CALL(sm50_destroy, &params);
}

AIRCONV_API int
SM50Compile(
    sm50_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_params params;
  params.shader = (sm50_shader_t)pShader;
  params.args = pArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile, &params);
  if (status)
    return (int)status;
  return params.ret;
}

AIRCONV_API void
SM50GetCompiledBitcode(sm50_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData) {
  struct sm50_get_compiled_bitcode_params params;
  params.bitcode = pBitcode;
  params.data_out = pData;
  UNIX_CALL(sm50_get_compiled_bitcode, &params);
}

AIRCONV_API void
SM50DestroyBitcode(sm50_bitcode_t pBitcode) {
  struct sm50_destroy_bitcode_params params;
  params.bitcode = pBitcode;
  UNIX_CALL(sm50_destroy_bitcode, &params);
}

AIRCONV_API size_t
SM50GetErrorMessage(sm50_error_t pError, char *pBuffer, size_t BufferSize) {
  struct sm50_get_error_message_params params;
  params.error = pError;
  params.buffer = pBuffer;
  params.buffer_size = BufferSize;
  params.ret_size = 0;
  UNIX_CALL(sm50_get_error_message, &params);
  return params.ret_size;
}

AIRCONV_API void
SM50FreeError(sm50_error_t pError) {
  struct sm50_free_error_params params;
  params.error = pError;
  UNIX_CALL(sm50_free_error, &params);
}

AIRCONV_API int
SM50CompileTessellationPipelineHull(
    sm50_shader_t pVertexShader, sm50_shader_t pHullShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs, const char *FunctionName, sm50_bitcode_t *ppBitcode,
    sm50_error_t *ppError
) {
  struct sm50_compile_tessellation_pipeline_hull_params params;
  params.vertex = pVertexShader;
  params.hull = pHullShader;
  params.hull_args = pHullShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_tessellation_hull, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileTessellationPipelineDomain(
    sm50_shader_t pHullShader, sm50_shader_t pDomainShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_tessellation_pipeline_domain_params params;
  params.domain = pDomainShader;
  params.hull = pHullShader;
  params.domain_args = pDomainShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_tessellation_domain, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileGeometryPipelineVertex(
    sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_geometry_pipeline_vertex_params params;
  params.vertex = pVertexShader;
  params.geometry = pGeometryShader;
  params.vertex_args = pVertexShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_geometry_pipeline_vertex, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API int
SM50CompileGeometryPipelineGeometry(
    sm50_shader_t pVertexShader, sm50_shader_t pGeometryShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs, const char *FunctionName,
    sm50_bitcode_t *ppBitcode, sm50_error_t *ppError
) {
  struct sm50_compile_geometry_pipeline_geometry_params params;
  params.vertex = pVertexShader;
  params.geometry = pGeometryShader;
  params.geometry_args = pGeometryShaderArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(sm50_compile_geometry_pipeline_geometry, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void SM50GetArgumentsInfo(
  sm50_shader_t pShader, struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
  struct MTL_SM50_SHADER_ARGUMENT *pArguments
) {
  struct sm50_get_arguments_info_params params;
  params.shader = pShader;
  params.constant_buffers = pConstantBuffers;
  params.arguments = pArguments;
  UNIX_CALL(sm50_get_arguments_info, &params);
};

AIRCONV_API int
DXMT12SM50Initialize(
    const void *pBytecode, size_t BytecodeSize, dxmt12_airconv_shader_t *ppShader,
    DXMT12_MTL4_SHADER_REFLECTION *pRefl, dxmt12_airconv_error_t *ppError
) {
  return SM50Initialize(pBytecode, BytecodeSize, ppShader, pRefl, ppError);
}

AIRCONV_API void
DXMT12SM50Destroy(dxmt12_airconv_shader_t pShader) {
  SM50Destroy(pShader);
}

AIRCONV_API int
DXMT12SM50Compile(
    dxmt12_airconv_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName,
    dxmt12_airconv_bitcode_t *ppBitcode, dxmt12_airconv_error_t *ppError
) {
  return SM50Compile(pShader, pArgs, FunctionName, ppBitcode, ppError);
}

AIRCONV_API void
DXMT12SM50GetCompiledBitcode(dxmt12_airconv_bitcode_t pBitcode, struct SM50_COMPILED_BITCODE *pData) {
  SM50GetCompiledBitcode(pBitcode, pData);
}

AIRCONV_API void
DXMT12SM50DestroyBitcode(dxmt12_airconv_bitcode_t pBitcode) {
  SM50DestroyBitcode(pBitcode);
}

AIRCONV_API size_t
DXMT12SM50GetErrorMessage(dxmt12_airconv_error_t pError, char *pBuffer, size_t BufferSize) {
  return SM50GetErrorMessage(pError, pBuffer, BufferSize);
}

AIRCONV_API void
DXMT12SM50FreeError(dxmt12_airconv_error_t pError) {
  SM50FreeError(pError);
}

AIRCONV_API int
DXMT12SM50CompileTessellationPipelineHull(
    dxmt12_airconv_shader_t pVertexShader, dxmt12_airconv_shader_t pHullShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pHullShaderArgs, const char *FunctionName,
    dxmt12_airconv_bitcode_t *ppBitcode, dxmt12_airconv_error_t *ppError
) {
  return SM50CompileTessellationPipelineHull(
      pVertexShader, pHullShader, pHullShaderArgs, FunctionName, ppBitcode, ppError);
}

AIRCONV_API int
DXMT12SM50CompileTessellationPipelineDomain(
    dxmt12_airconv_shader_t pHullShader, dxmt12_airconv_shader_t pDomainShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pDomainShaderArgs, const char *FunctionName,
    dxmt12_airconv_bitcode_t *ppBitcode, dxmt12_airconv_error_t *ppError
) {
  return SM50CompileTessellationPipelineDomain(
      pHullShader, pDomainShader, pDomainShaderArgs, FunctionName, ppBitcode, ppError);
}

AIRCONV_API int
DXMT12SM50CompileGeometryPipelineVertex(
    dxmt12_airconv_shader_t pVertexShader, dxmt12_airconv_shader_t pGeometryShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pVertexShaderArgs, const char *FunctionName,
    dxmt12_airconv_bitcode_t *ppBitcode, dxmt12_airconv_error_t *ppError
) {
  return SM50CompileGeometryPipelineVertex(
      pVertexShader, pGeometryShader, pVertexShaderArgs, FunctionName, ppBitcode, ppError);
}

AIRCONV_API int
DXMT12SM50CompileGeometryPipelineGeometry(
    dxmt12_airconv_shader_t pVertexShader, dxmt12_airconv_shader_t pGeometryShader,
    struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pGeometryShaderArgs, const char *FunctionName,
    dxmt12_airconv_bitcode_t *ppBitcode, dxmt12_airconv_error_t *ppError
) {
  return SM50CompileGeometryPipelineGeometry(
      pVertexShader, pGeometryShader, pGeometryShaderArgs, FunctionName, ppBitcode, ppError);
}

AIRCONV_API void
DXMT12SM50GetArgumentsInfo(
  dxmt12_airconv_shader_t pShader, DXMT12_MTL4_SHADER_ARGUMENT *pConstantBuffers,
  DXMT12_MTL4_SHADER_ARGUMENT *pArguments
) {
  SM50GetArgumentsInfo(pShader, pConstantBuffers, pArguments);
}

AIRCONV_API int
DXMT12DXILInitialize(
    const void *pBytecode, size_t BytecodeSize, dxmt12_airconv_shader_t *ppShader,
    DXMT12_MTL4_SHADER_REFLECTION *pRefl, dxmt12_airconv_error_t *ppError
) {
  struct sm50_initialize_params params;
  params.bytecode = pBytecode;
  params.bytecode_size = BytecodeSize;
  params.shader = ppShader;
  params.reflection = pRefl;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(dxil_initialize, &params);
  if (status)
    return -1;
  return params.ret;
}

AIRCONV_API void
DXMT12DXILDestroy(dxmt12_airconv_shader_t pShader) {
  struct sm50_destroy_params params;
  params.shader = pShader;
  UNIX_CALL(dxil_destroy, &params);
}

AIRCONV_API int
DXMT12DXILCompile(
    dxmt12_airconv_shader_t pShader, struct SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs, const char *FunctionName,
    dxmt12_airconv_bitcode_t *ppBitcode, dxmt12_airconv_error_t *ppError
) {
  struct sm50_compile_params params;
  params.shader = (sm50_shader_t)pShader;
  params.args = pArgs;
  params.func_name = FunctionName;
  params.bitcode = ppBitcode;
  params.error = ppError;
  NTSTATUS status;
  status = UNIX_CALL(dxil_compile, &params);
  if (status)
    return (int)status;
  return params.ret;
}

AIRCONV_API void DXMT12DXILGetArgumentsInfo(
  dxmt12_airconv_shader_t pShader, DXMT12_MTL4_SHADER_ARGUMENT *pConstantBuffers,
  DXMT12_MTL4_SHADER_ARGUMENT *pArguments
) {
  struct sm50_get_arguments_info_params params;
  params.shader = (sm50_shader_t)pShader;
  params.constant_buffers = pConstantBuffers;
  params.arguments = pArguments;
  UNIX_CALL(dxil_get_arguments_info, &params);
};
