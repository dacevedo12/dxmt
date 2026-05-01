#include "dxil_converter.hpp"

#include "airconv_context.hpp"
#include "airconv_error.hpp"
#include "airconv_internal.hpp"
#include "dxil_air_converter.hpp"
#include "metallib_writer.hpp"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kConstantBufferBindIndex = 29;
constexpr uint32_t kArgumentBufferBindIndex = 30;

class DXILShaderInternal {
public:
  std::vector<uint8_t> bytecode;
  dxmt::dxil::Parser parser;
  std::vector<MTL_SM50_SHADER_ARGUMENT> cbuffer_arguments;
  std::vector<MTL_SM50_SHADER_ARGUMENT> resource_arguments;
};

MTL_SM50_SHADER_ARGUMENT_FLAG
TextureArgumentFlags(const dxmt::dxil::DxilTranslationResourceInfo &resource,
                     bool uav) {
  auto flags = MTL_SM50_SHADER_ARGUMENT_FLAG(
      MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP |
      MTL_SM50_SHADER_ARGUMENT_TEXTURE);

  switch (resource.dimension) {
  case 3:
  case 8:
    flags = MTL_SM50_SHADER_ARGUMENT_FLAG(
        flags | MTL_SM50_SHADER_ARGUMENT_TEXTURE_MULTISAMPLED);
    break;
  case 4:
    flags =
        MTL_SM50_SHADER_ARGUMENT_FLAG(flags | MTL_SM50_SHADER_ARGUMENT_TEXTURE_3D);
    break;
  case 5:
  case 9:
    flags = MTL_SM50_SHADER_ARGUMENT_FLAG(
        flags | MTL_SM50_SHADER_ARGUMENT_TEXTURE_CUBE);
    break;
  default:
    break;
  }

  switch (resource.return_type) {
  case 2:
    flags =
        MTL_SM50_SHADER_ARGUMENT_FLAG(flags | MTL_SM50_SHADER_ARGUMENT_TEXTURE_UINT);
    break;
  case 3:
    flags =
        MTL_SM50_SHADER_ARGUMENT_FLAG(flags | MTL_SM50_SHADER_ARGUMENT_TEXTURE_SINT);
    break;
  default:
    break;
  }

  if (resource.dimension == 6 || resource.dimension == 7 ||
      resource.dimension == 8 || resource.dimension == 9)
    flags =
        MTL_SM50_SHADER_ARGUMENT_FLAG(flags | MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);

  if (resource.read || resource.sampled || (!uav && !resource.written))
    flags =
        MTL_SM50_SHADER_ARGUMENT_FLAG(flags | MTL_SM50_SHADER_ARGUMENT_READ_ACCESS);
  if (resource.written)
    flags =
        MTL_SM50_SHADER_ARGUMENT_FLAG(flags | MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS);
  return flags;
}

MTL_SM50_SHADER_ARGUMENT_FLAG
BufferArgumentFlags(const dxmt::dxil::DxilTranslationResourceInfo &resource,
                    bool uav) {
  auto flags = MTL_SM50_SHADER_ARGUMENT_FLAG(
      MTL_SM50_SHADER_ARGUMENT_ELEMENT_WIDTH |
      MTL_SM50_SHADER_ARGUMENT_BUFFER);
  if (resource.read || !uav)
    flags =
        MTL_SM50_SHADER_ARGUMENT_FLAG(flags | MTL_SM50_SHADER_ARGUMENT_READ_ACCESS);
  if (resource.written)
    flags =
        MTL_SM50_SHADER_ARGUMENT_FLAG(flags | MTL_SM50_SHADER_ARGUMENT_WRITE_ACCESS);
  return flags;
}

bool IsBufferResource(const dxmt::dxil::DxilTranslationResourceInfo &resource) {
  return resource.resource_kind == 11 || resource.resource_kind == 12 ||
         resource.dimension == 0;
}

void
BuildDxilArgumentInfo(DXILShaderInternal &shader) {
  shader.cbuffer_arguments.clear();
  shader.resource_arguments.clear();
  const auto &translation = shader.parser.dxilTranslation();
  if (!translation)
    return;

  for (const auto &resource : translation->resources) {
    switch (resource.resource_class) {
    case dxmt::dxil::DxilTranslationResourceClass::Cbv:
      shader.cbuffer_arguments.push_back({
          .Type = SM50BindingType::ConstantBuffer,
          .SM50BindingSlot = resource.id,
          .Flags = MTL_SM50_SHADER_ARGUMENT_FLAG(
              MTL_SM50_SHADER_ARGUMENT_BUFFER |
              MTL_SM50_SHADER_ARGUMENT_READ_ACCESS),
          .StructurePtrOffset = resource.id,
      });
      break;
    case dxmt::dxil::DxilTranslationResourceClass::Sampler:
      shader.resource_arguments.push_back({
          .Type = SM50BindingType::Sampler,
          .SM50BindingSlot = resource.id,
          .Flags = MTL_SM50_SHADER_ARGUMENT_FLAG(0),
          .StructurePtrOffset =
              GetArgumentIndex(SM50BindingType::Sampler, resource.id),
      });
      break;
    case dxmt::dxil::DxilTranslationResourceClass::Srv:
      shader.resource_arguments.push_back({
          .Type = SM50BindingType::SRV,
          .SM50BindingSlot = resource.id,
          .Flags = IsBufferResource(resource)
                       ? BufferArgumentFlags(resource, false)
                       : TextureArgumentFlags(resource, false),
          .StructurePtrOffset =
              GetArgumentIndex(SM50BindingType::SRV, resource.id),
      });
      break;
    case dxmt::dxil::DxilTranslationResourceClass::Uav:
      shader.resource_arguments.push_back({
          .Type = SM50BindingType::UAV,
          .SM50BindingSlot = resource.id,
          .Flags = IsBufferResource(resource)
                       ? BufferArgumentFlags(resource, true)
                       : TextureArgumentFlags(resource, true),
          .StructurePtrOffset =
              GetArgumentIndex(SM50BindingType::UAV, resource.id),
      });
      break;
    default:
      break;
    }
  }
}

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

  BuildDxilArgumentInfo(*shader);
  dxmt::airconv::FillDxilReflection(shader->parser, pRefl);
  if (pRefl) {
    pRefl->ConstanttBufferTableBindIndex =
        shader->cbuffer_arguments.empty() ? ~0u : kConstantBufferBindIndex;
    pRefl->ArgumentBufferBindIndex =
        shader->resource_arguments.empty() ? ~0u : kArgumentBufferBindIndex;
    pRefl->NumConstantBuffers = shader->cbuffer_arguments.size();
    pRefl->NumArguments = shader->resource_arguments.size();
    uint32_t qwords = 0;
    for (const auto &argument : shader->resource_arguments) {
      uint32_t count = 1;
      if (argument.Type == SM50BindingType::Sampler)
        count = 3;
      else if (argument.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE)
        count = 2;
      else if (argument.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER)
        count = 2;
      if (argument.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER)
        count = std::max(count, 3u);
      qwords = std::max(qwords, argument.StructurePtrOffset + count);
    }
    pRefl->ArgumentTableQwords = qwords;
  }
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
  if (ppError)
    *ppError = nullptr;
  if (ppBitcode)
    *ppBitcode = nullptr;

  if (!pShader)
    return DXILFail(ppError, "pShader can not be null");
  if (!ppBitcode)
    return DXILFail(ppError, "ppBitcode can not be null");

  llvm::LLVMContext context;
  context.setOpaquePointers(false);

  auto module = std::make_unique<llvm::Module>("dxil.air", context);
  dxmt::initializeModule(*module);

  auto *shader = (DXILShaderInternal *)pShader;
  if (auto err = dxmt::airconv::ConvertDxilToAir(
          shader->parser, FunctionName, context, *module, pArgs)) {
    std::string message;
    llvm::raw_string_ostream stream(message);
    llvm::handleAllErrors(std::move(err), [&](const dxmt::UnsupportedFeature &unsupported) {
      stream << unsupported.msg;
    });
    return DXILFail(ppError, stream.str());
  }

  dxmt::runOptimizationPasses(*module);

  auto compiled = std::make_unique<SM50CompiledBitcodeInternal>();
  llvm::raw_svector_ostream out(compiled->vec);
  dxmt::metallib::MetallibWriter writer;
  writer.Write(*module, out);
  *ppBitcode = sm50_bitcode_t(compiled.release());
  return 0;
}

AIRCONV_API void
SM50GetArgumentsInfo(sm50_shader_t pShader,
                     struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
                     struct MTL_SM50_SHADER_ARGUMENT *pArguments);

AIRCONV_API void
DXILGetArgumentsInfo(dxil_shader_t pShader,
                     struct MTL_SM50_SHADER_ARGUMENT *pConstantBuffers,
                     struct MTL_SM50_SHADER_ARGUMENT *pArguments) {
  auto *shader = (DXILShaderInternal *)pShader;
  if (!shader)
    return;
  if (pConstantBuffers && !shader->cbuffer_arguments.empty())
    std::memcpy(pConstantBuffers, shader->cbuffer_arguments.data(),
                shader->cbuffer_arguments.size() *
                    sizeof(MTL_SM50_SHADER_ARGUMENT));
  if (pArguments && !shader->resource_arguments.empty())
    std::memcpy(pArguments, shader->resource_arguments.data(),
                shader->resource_arguments.size() *
                    sizeof(MTL_SM50_SHADER_ARGUMENT));
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
