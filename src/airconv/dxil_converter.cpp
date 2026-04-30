#include "dxil_converter.hpp"

#include "airconv_internal.hpp"

#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

class DXILShaderInternal {
public:
  std::vector<uint8_t> bytecode;
  dxmt::dxil::Parser parser;
};

int
DXILFail(sm50_error_t *ppError, const std::string &message) {
  if (ppError) {
    auto error = std::make_unique<SM50ErrorInternal>();
    llvm::raw_svector_ostream stream(error->buf);
    stream << message;
    *ppError = sm50_error_t(error.release());
  }
  return 1;
}

void
InitializeEmptyReflection(MTL_SHADER_REFLECTION *pRefl) {
  if (!pRefl)
    return;

  std::memset(pRefl, 0, sizeof(*pRefl));
  pRefl->ConstanttBufferTableBindIndex = ~0u;
  pRefl->ArgumentBufferBindIndex = ~0u;
}

} // namespace

AIRCONV_API int
DXILInitialize(const void *pBytecode, size_t BytecodeSize,
               dxil_shader_t *ppShader, MTL_SHADER_REFLECTION *pRefl,
               sm50_error_t *ppError) {
  if (ppError)
    *ppError = nullptr;
  if (ppShader)
    *ppShader = nullptr;

  if (!ppShader)
    return DXILFail(ppError, "ppShader can not be null");
  if (!pBytecode && BytecodeSize)
    return DXILFail(ppError, "pBytecode can not be null");

  auto shader = std::make_unique<DXILShaderInternal>();
  shader->bytecode.resize(BytecodeSize);
  if (BytecodeSize)
    std::memcpy(shader->bytecode.data(), pBytecode, BytecodeSize);

  const auto status = shader->parser.parse(shader->bytecode.data(),
                                           shader->bytecode.size());
  if (status != dxmt::dxil::ParseStatus::Ok) {
    return DXILFail(ppError, std::string("Invalid DXIL bytecode: ") +
                                dxmt::dxil::StatusName(status));
  }

  InitializeEmptyReflection(pRefl);
  *ppShader = dxil_shader_t(shader.release());
  return 0;
}

AIRCONV_API void
DXILDestroy(dxil_shader_t pShader) {
  delete (DXILShaderInternal *)pShader;
}

AIRCONV_API int
DXILCompile(dxil_shader_t pShader, SM50_SHADER_COMPILATION_ARGUMENT_DATA *pArgs,
            const char *FunctionName, sm50_bitcode_t *ppBitcode,
            sm50_error_t *ppError) {
  (void)pShader;
  (void)pArgs;
  (void)FunctionName;
  if (ppError)
    *ppError = nullptr;
  if (ppBitcode)
    *ppBitcode = nullptr;
  return DXILFail(ppError, "DXIL to Metal translation is not implemented yet");
}

namespace dxmt::airconv {

const dxil::DxilTranslationInfo *
GetDxilTranslationInfo(dxil_shader_t pShader) {
  if (!pShader)
    return nullptr;

  const auto *shader = (const DXILShaderInternal *)pShader;
  const auto &translation = shader->parser.dxilTranslation();
  return translation ? &*translation : nullptr;
}

} // namespace dxmt::airconv
