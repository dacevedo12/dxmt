#include "d3d12_pipeline.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <span>
#include <unordered_map>
#include <utility>

namespace dxmt::d3d12 {
namespace {

static bool
D3D12PipelineDiagEnabledEnv(const char *name) {
  auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" || value == "trace";
}

static bool
D3D12PipelineDiagEnabled() {
  static const bool enabled =
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_PIPELINE") ||
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_D3D12_PIPELINE") ||
      D3D12PipelineDiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE");
  return enabled;
}

static uint32_t
D3D12PipelineDiagLimit() {
  static const uint32_t limit = []() {
    auto value = env::getEnvVar("DXMT_DIAG_PIPELINE_LIMIT");
    if (value.empty())
      value = env::getEnvVar("DXMT_DIAG_D3D12_LIMIT");
    if (value.empty())
      return 512u;
    char *end = nullptr;
    auto parsed = std::strtoul(value.c_str(), &end, 10);
    if (end == value.c_str())
      return 512u;
    return static_cast<uint32_t>(std::max<unsigned long>(1, parsed));
  }();
  return limit;
}

static bool
D3D12PipelineDiagShouldLog() {
  static std::atomic<uint32_t> count = 0;
  if (!D3D12PipelineDiagEnabled())
    return false;
  return count.fetch_add(1, std::memory_order_relaxed) <
         D3D12PipelineDiagLimit();
}

static std::string
D3D12PipelineDumpDirectory() {
  std::string path = env::getEnvVar("DXMT_DUMP_PATH");
  if (path.empty())
    path = env::getEnvVar("DXMT_LOG_PATH");
  if (path.empty() || path == "none")
    path = ".";
  env::createDirectory(path);
  if (!path.empty() && path.back() != '/' && path.back() != '\\')
    path += '/';
  return path;
}

constexpr uint32_t kShaderKindPixel = 0;
constexpr uint32_t kShaderKindVertex = 1;
constexpr uint32_t kShaderKindGeometry = 2;
constexpr uint32_t kShaderKindHull = 3;
constexpr uint32_t kShaderKindDomain = 4;
constexpr uint32_t kShaderKindCompute = 5;

constexpr WMTCompareFunction kCompareFunctionMap[] = {
    WMTCompareFunctionNever, WMTCompareFunctionNever, WMTCompareFunctionLess,
    WMTCompareFunctionEqual, WMTCompareFunctionLessEqual,
    WMTCompareFunctionGreater, WMTCompareFunctionNotEqual,
    WMTCompareFunctionGreaterEqual, WMTCompareFunctionAlways};

constexpr WMTStencilOperation kStencilOperationMap[] = {
    WMTStencilOperationZero,
    WMTStencilOperationKeep,
    WMTStencilOperationZero,
    WMTStencilOperationReplace,
    WMTStencilOperationIncrementClamp,
    WMTStencilOperationDecrementClamp,
    WMTStencilOperationInvert,
    WMTStencilOperationIncrementWrap,
    WMTStencilOperationDecrementWrap,
};

constexpr WMTBlendOperation kBlendOpMap[] = {
    WMTBlendOperationAdd,
    WMTBlendOperationAdd,
    WMTBlendOperationSubtract,
    WMTBlendOperationReverseSubtract,
    WMTBlendOperationMin,
    WMTBlendOperationMax,
};

constexpr WMTBlendFactor kBlendFactorMap[] = {
    WMTBlendFactorZero,
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,
    WMTBlendFactorZero,
    WMTBlendFactorBlendColor,
    WMTBlendFactorOneMinusBlendColor,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
};

constexpr WMTBlendFactor kBlendAlphaFactorMap[] = {
    WMTBlendFactorZero,
    WMTBlendFactorZero,
    WMTBlendFactorOne,
    WMTBlendFactorSourceColor,
    WMTBlendFactorOneMinusSourceColor,
    WMTBlendFactorSourceAlpha,
    WMTBlendFactorOneMinusSourceAlpha,
    WMTBlendFactorDestinationAlpha,
    WMTBlendFactorOneMinusDestinationAlpha,
    WMTBlendFactorDestinationColor,
    WMTBlendFactorOneMinusDestinationColor,
    WMTBlendFactorSourceAlphaSaturated,
    WMTBlendFactorZero,
    WMTBlendFactorZero,
    WMTBlendFactorBlendAlpha,
    WMTBlendFactorOneMinusBlendAlpha,
    WMTBlendFactorSource1Color,
    WMTBlendFactorOneMinusSource1Color,
    WMTBlendFactorSource1Alpha,
    WMTBlendFactorOneMinusSource1Alpha,
};

constexpr WMTColorWriteMask kColorWriteMaskMap[] = {
    WMTColorWriteMaskNone,
    WMTColorWriteMaskRed,
    WMTColorWriteMaskGreen,
    WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    WMTColorWriteMaskBlue,
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    WMTColorWriteMaskBlue | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    WMTColorWriteMaskAlpha,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskGreen,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskRed | WMTColorWriteMaskGreen,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskRed,
    WMTColorWriteMaskAlpha | WMTColorWriteMaskBlue | WMTColorWriteMaskGreen,
    WMTColorWriteMaskAll,
};

constexpr WMTLogicOperation kLogicOpMap[] = {
    WMTLogicOperationClear,      WMTLogicOperationSet,
    WMTLogicOperationCopy,       WMTLogicOperationCopyInverted,
    WMTLogicOperationNoOp,       WMTLogicOperationInvert,
    WMTLogicOperationAnd,        WMTLogicOperationNand,
    WMTLogicOperationOr,         WMTLogicOperationNor,
    WMTLogicOperationXor,        WMTLogicOperationEquiv,
    WMTLogicOperationAndReverse, WMTLogicOperationAndInverted,
    WMTLogicOperationOrReverse,  WMTLogicOperationOrInverted,
};

WMTPixelFormat
GetRenderTargetPixelFormat(WMT::Device device, DXGI_FORMAT format) {
  if (format == DXGI_FORMAT_R8G8B8A8_UNORM)
    return WMTPixelFormatBGRA8Unorm;
  if (format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    return WMTPixelFormatBGRA8Unorm_sRGB;

  MTL_DXGI_FORMAT_DESC desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, format, desc)))
    return WMTPixelFormatInvalid;
  return desc.PixelFormat;
}

class BlobImpl final : public ComObjectWithInitialRef<ID3DBlob> {
public:
  explicit BlobImpl(std::vector<std::byte> &&data) : data_(std::move(data)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;
    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D10Blob)) {
      *object = ref(this);
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  void *STDMETHODCALLTYPE GetBufferPointer() override {
    return data_.empty() ? nullptr : data_.data();
  }

  SIZE_T STDMETHODCALLTYPE GetBufferSize() override {
    return data_.size();
  }

private:
  std::vector<std::byte> data_;
};

Com<ID3DBlob>
CreateBlob(std::span<const uint8_t> data) {
  std::vector<std::byte> bytes;
  bytes.resize(data.size());
  if (!data.empty())
    std::memcpy(bytes.data(), data.data(), data.size());
  return Com<ID3DBlob>::transfer(new BlobImpl(std::move(bytes)));
}

const char *
ShaderStageName(PipelineShaderStage stage) {
  switch (stage) {
  case PipelineShaderStage::Vertex:
    return "VS";
  case PipelineShaderStage::Pixel:
    return "PS";
  case PipelineShaderStage::Geometry:
    return "GS";
  case PipelineShaderStage::Hull:
    return "HS";
  case PipelineShaderStage::Domain:
    return "DS";
  case PipelineShaderStage::Compute:
    return "CS";
  }
  return "unknown";
}

uint32_t
ExpectedShaderKind(PipelineShaderStage stage) {
  switch (stage) {
  case PipelineShaderStage::Vertex:
    return kShaderKindVertex;
  case PipelineShaderStage::Pixel:
    return kShaderKindPixel;
  case PipelineShaderStage::Geometry:
    return kShaderKindGeometry;
  case PipelineShaderStage::Hull:
    return kShaderKindHull;
  case PipelineShaderStage::Domain:
    return kShaderKindDomain;
  case PipelineShaderStage::Compute:
    return kShaderKindCompute;
  }
  return UINT32_MAX;
}

bool
HasBytecode(const D3D12_SHADER_BYTECODE &bytecode) {
  return bytecode.pShaderBytecode && bytecode.BytecodeLength;
}

bool
IsDxilContainer(const D3D12_SHADER_BYTECODE &bytecode) {
  dxil::ContainerInfo container = {};
  const auto status = dxil::ParseContainer(
      bytecode.pShaderBytecode, bytecode.BytecodeLength, container);
  return status == dxil::ParseStatus::Ok &&
         container.hasPart(dxil::fourcc::Dxil);
}

constexpr uint32_t
FourCC(char a, char b, char c, char d) {
  return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) |
         (uint32_t(uint8_t(c)) << 16) | (uint32_t(uint8_t(d)) << 24);
}

uint32_t
ReadLe32(const uint8_t *data) {
  return uint32_t(data[0]) | (uint32_t(data[1]) << 8) |
         (uint32_t(data[2]) << 16) | (uint32_t(data[3]) << 24);
}

bool
FindDxbcShaderBlob(const void *data, size_t size,
                   const uint8_t **blob, uint32_t *blob_size) {
  if (!data || size < 32)
    return false;

  const auto *bytes = static_cast<const uint8_t *>(data);
  if (ReadLe32(bytes) != FourCC('D', 'X', 'B', 'C'))
    return false;

  const auto container_size = ReadLe32(bytes + 24);
  const auto blob_count = ReadLe32(bytes + 28);
  if (container_size > size || blob_count > (container_size - 32) / 4)
    return false;

  for (uint32_t i = 0; i < blob_count; i++) {
    const auto offset = ReadLe32(bytes + 32 + i * 4);
    if (offset > container_size || container_size - offset < 8)
      return false;

    const auto fourcc = ReadLe32(bytes + offset);
    const auto length = ReadLe32(bytes + offset + 4);
    if (length > container_size - offset - 8)
      return false;

    if (fourcc == FourCC('S', 'H', 'D', 'R') ||
        fourcc == FourCC('S', 'H', 'E', 'X')) {
      *blob = bytes + offset + 8;
      *blob_size = length;
      return true;
    }
  }

  return false;
}

uint32_t
ExpectedDxbcShaderType(PipelineShaderStage stage) {
  switch (stage) {
  case PipelineShaderStage::Pixel:
    return 0;
  case PipelineShaderStage::Vertex:
    return 1;
  case PipelineShaderStage::Geometry:
    return 2;
  case PipelineShaderStage::Hull:
    return 3;
  case PipelineShaderStage::Domain:
    return 4;
  case PipelineShaderStage::Compute:
    return 5;
  }
  return UINT32_MAX;
}

bool
ValidateDxbcShaderStage(PipelineShaderStage stage,
                        const D3D12_SHADER_BYTECODE &bytecode) {
  const uint8_t *shader_blob = nullptr;
  uint32_t shader_blob_size = 0;
  if (!FindDxbcShaderBlob(bytecode.pShaderBytecode, bytecode.BytecodeLength,
                          &shader_blob, &shader_blob_size) ||
      shader_blob_size < 4)
    return false;

  const auto token = ReadLe32(shader_blob);
  const auto actual_type = token >> 16;
  const auto expected_type = ExpectedDxbcShaderType(stage);
  return expected_type == UINT32_MAX || actual_type == expected_type;
}

D3D12_ROOT_SIGNATURE_FLAGS
GetRootSignatureFlags(ID3D12RootSignature *root_signature) {
  auto *rs = dynamic_cast<RootSignature *>(root_signature);
  if (!rs)
    return D3D12_ROOT_SIGNATURE_FLAG_NONE;

  const auto &desc = rs->GetVersionedDesc();
  return desc.Version == D3D_ROOT_SIGNATURE_VERSION_1_0
             ? desc.Desc_1_0.Flags
             : desc.Desc_1_1.Flags;
}

HRESULT
ParsePipelineShader(PipelineShaderStage stage,
                    IMTLD3D12Device *device,
                    const D3D12_SHADER_BYTECODE &bytecode,
                    PipelineDxilShader &shader) {
  if (!bytecode.pShaderBytecode && bytecode.BytecodeLength)
    return WARN_E_INVALIDARG(__func__);
  if (!HasBytecode(bytecode))
    return S_FALSE;

  shader = {};
  shader.stage = stage;
  shader.bytecode.resize(bytecode.BytecodeLength);
  std::memcpy(shader.bytecode.data(), bytecode.pShaderBytecode,
              bytecode.BytecodeLength);

  const bool is_dxil = IsDxilContainer(bytecode);
  shader.kind = is_dxil ? PipelineShaderBytecodeKind::Dxil
                        : PipelineShaderBytecodeKind::Dxbc;

  const auto status = shader.parser.parse(shader.bytecode.data(),
                                          shader.bytecode.size());
  if (is_dxil && status != dxil::ParseStatus::Ok) {
    WARN("D3D12PipelineState: failed to parse ", ShaderStageName(stage),
         " DXIL bytecode: ", dxil::StatusName(status));
    return WARN_E_INVALIDARG(__func__);
  }

  const auto &program = shader.parser.dxilProgram();
  if (is_dxil) {
    const auto expected_kind = ExpectedShaderKind(stage);
    if (program && expected_kind != UINT32_MAX &&
        program->shader_kind() != expected_kind) {
      WARN("D3D12PipelineState: ", ShaderStageName(stage),
           " bytecode contains ", dxil::PsvShaderKindName(program->shader_kind()),
           " shader");
      return WARN_E_INVALIDARG(__func__);
    }
  } else if (!ValidateDxbcShaderStage(stage, bytecode)) {
    WARN("D3D12PipelineState: invalid or mismatched ",
         ShaderStageName(stage), " DXBC bytecode");
    return WARN_E_INVALIDARG(__func__);
  }

  sm50_error_t error = nullptr;
  const auto initialize_failed =
      is_dxil ? DXILInitialize(shader.bytecode.data(), shader.bytecode.size(),
                               &shader.shader, &shader.reflection, &error)
              : SM50Initialize(shader.bytecode.data(), shader.bytecode.size(),
                               &shader.shader, &shader.reflection, &error);
  if (initialize_failed) {
    WARN("D3D12PipelineState: failed to initialize ", ShaderStageName(stage),
         is_dxil ? " DXIL shader: " : " DXBC shader: ",
         SM50GetErrorMessageString(error));
    SM50FreeError(error);
    return WARN_E_INVALIDARG(__func__);
  }

  const auto argument_count =
      shader.reflection.NumConstantBuffers + shader.reflection.NumArguments;
  shader.argument_info.resize(argument_count);
  if (argument_count) {
    auto *constant_buffers =
        shader.argument_info.empty() ? nullptr : shader.argument_info.data();
    auto *arguments =
        shader.argument_info.size() <= shader.reflection.NumConstantBuffers
            ? nullptr
            : shader.argument_info.data() + shader.reflection.NumConstantBuffers;
    if (is_dxil)
      DXILGetArgumentsInfo(shader.shader, constant_buffers, arguments);
    else
      SM50GetArgumentsInfo(shader.shader, constant_buffers, arguments);
  }

  return S_OK;
}

HRESULT
AppendPipelineShader(PipelineShaderStage stage,
                     IMTLD3D12Device *device,
                     const D3D12_SHADER_BYTECODE &bytecode,
                     std::vector<PipelineDxilShader> &shaders) {
  PipelineDxilShader shader = {};
  const auto hr = ParsePipelineShader(stage, device, bytecode, shader);
  if (hr == S_FALSE)
    return S_OK;
  if (FAILED(hr))
    return hr;

  shaders.push_back(std::move(shader));
  return S_OK;
}

const PipelineDxilShader *
FindShader(const std::vector<PipelineDxilShader> &shaders,
           PipelineShaderStage stage, uint32_t *index = nullptr) {
  for (uint32_t i = 0; i < shaders.size(); i++) {
    if (shaders[i].stage == stage) {
      if (index)
        *index = i;
      return &shaders[i];
    }
  }
  return nullptr;
}

D3D12_SHADER_BYTECODE
ShaderBytecodeView(const std::vector<PipelineDxilShader> &shaders,
                   PipelineShaderStage stage) {
  const auto *shader = FindShader(shaders, stage);
  if (!shader)
    return {};

  return {
      .pShaderBytecode = shader->bytecode.empty() ? nullptr
                                                  : shader->bytecode.data(),
      .BytecodeLength = shader->bytecode.size(),
  };
}

bool
IsSystemGeneratedValue(const dxil::DxilTranslationSignatureElementInfo &sig) {
  return sig.semantic_kind != 0 || sig.semantic_key == "SV_POSITION0" ||
         sig.semantic_key == "SV_TARGET0" ||
         sig.semantic_key == "SV_DEPTH0" ||
         sig.semantic_key == "SV_COVERAGE0";
}

void
AppendSignatureLinks(const std::vector<PipelineDxilShader> &shaders,
                     PipelineShaderStage producer_stage,
                     PipelineShaderStage consumer_stage,
                     std::vector<PipelineSignatureLink> &links) {
  uint32_t producer_shader_index = 0;
  uint32_t consumer_shader_index = 0;
  const auto *producer =
      FindShader(shaders, producer_stage, &producer_shader_index);
  const auto *consumer =
      FindShader(shaders, consumer_stage, &consumer_shader_index);
  if (!producer || !consumer)
    return;

  const auto *producer_info = producer->translation();
  const auto *consumer_info = consumer->translation();
  if (!producer_info || !consumer_info)
    return;

  for (uint32_t consumer_sig_index = 0;
       consumer_sig_index < consumer_info->signatures.size();
       consumer_sig_index++) {
    const auto &consumer_sig = consumer_info->signatures[consumer_sig_index];
    if (consumer_sig.kind != dxil::DxilTranslationSignatureKind::Input ||
        IsSystemGeneratedValue(consumer_sig))
      continue;

    auto match = std::find_if(
        producer_info->signatures.begin(), producer_info->signatures.end(),
        [&](const dxil::DxilTranslationSignatureElementInfo &producer_sig) {
          return producer_sig.kind ==
                     dxil::DxilTranslationSignatureKind::Output &&
                 producer_sig.output_stream == consumer_sig.output_stream &&
                 producer_sig.semantic_key == consumer_sig.semantic_key &&
                 (producer_sig.component_mask & consumer_sig.component_mask) ==
                     consumer_sig.component_mask;
        });
    if (match == producer_info->signatures.end()) {
      WARN("D3D12PipelineState: unmatched ", ShaderStageName(consumer_stage),
           " input signature ", consumer_sig.semantic_key);
      continue;
    }

    links.push_back({
        .producer_shader_index = producer_shader_index,
        .producer_signature_index =
            uint32_t(match - producer_info->signatures.begin()),
        .consumer_shader_index = consumer_shader_index,
        .consumer_signature_index = consumer_sig_index,
        .semantic_key = consumer_sig.semantic_key,
        .producer_component_mask = match->component_mask,
        .consumer_component_mask = consumer_sig.component_mask,
    });
  }
}

std::vector<PipelineSignatureLink>
BuildSignatureLinks(const std::vector<PipelineDxilShader> &shaders) {
  std::vector<PipelineSignatureLink> links;

  if (FindShader(shaders, PipelineShaderStage::Hull))
    AppendSignatureLinks(shaders, PipelineShaderStage::Vertex,
                         PipelineShaderStage::Hull, links);
  if (FindShader(shaders, PipelineShaderStage::Domain))
    AppendSignatureLinks(shaders, PipelineShaderStage::Hull,
                         PipelineShaderStage::Domain, links);

  const auto raster_producer = FindShader(shaders, PipelineShaderStage::Domain)
                                   ? PipelineShaderStage::Domain
                                   : PipelineShaderStage::Vertex;
  if (FindShader(shaders, PipelineShaderStage::Geometry)) {
    AppendSignatureLinks(shaders, raster_producer,
                         PipelineShaderStage::Geometry, links);
    AppendSignatureLinks(shaders, PipelineShaderStage::Geometry,
                         PipelineShaderStage::Pixel, links);
  } else {
    AppendSignatureLinks(shaders, raster_producer,
                         PipelineShaderStage::Pixel, links);
  }

  return links;
}

void
DebugLogDxilShaderInfo(std::string_view shader_cache_key,
                       const PipelineDxilShader &shader) {
  if (!D3D12PipelineDiagShouldLog())
    return;

  const auto *info = shader.translation();
  INFO("D3D12 diagnostic: pipeline shader",
       " pso=", shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())),
       " stage=", ShaderStageName(shader.stage),
       " bytecode=", uint64_t(shader.bytecode.size()),
       " cbv=", uint32_t(shader.reflection.NumConstantBuffers),
       " resources=", uint32_t(shader.reflection.NumArguments),
       " argumentQwords=", uint32_t(shader.reflection.ArgumentTableQwords),
       " signatures=", info ? uint32_t(info->signatures.size()) : 0u,
       " hasRootSignature=", info && info->has_root_signature);

  if (!info)
    return;

  for (uint32_t i = 0; i < info->signatures.size(); i++) {
    const auto &sig = info->signatures[i];
    INFO("D3D12 diagnostic: pipeline signature",
         " pso=", shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())),
         " stage=", ShaderStageName(shader.stage),
         " index=", i,
         " kind=", uint32_t(sig.kind),
         " semantic=", sig.semantic_key,
         " semanticKind=", uint32_t(sig.semantic_kind),
         " semanticIndex=", uint32_t(sig.semantic_index),
         " elementId=", uint32_t(sig.element_id),
         " stream=", uint32_t(sig.output_stream),
         " rows=", uint32_t(sig.rows),
         " cols=", uint32_t(sig.cols),
         " startRow=", uint32_t(sig.start_row),
         " startCol=", uint32_t(sig.start_col),
         " mask=0x", std::hex, uint32_t(sig.component_mask), std::dec);
  }
}

void
DebugLogSignatureLinks(std::string_view shader_cache_key,
                       const std::vector<PipelineSignatureLink> &links) {
  if (!D3D12PipelineDiagShouldLog())
    return;

  INFO("D3D12 diagnostic: pipeline signature links",
       " pso=", shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())),
       " count=", uint32_t(links.size()));
  for (uint32_t i = 0; i < links.size(); i++) {
    const auto &link = links[i];
    INFO("D3D12 diagnostic: pipeline signature link",
         " pso=", shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())),
         " index=", i,
         " semantic=", link.semantic_key,
         " producerShader=", link.producer_shader_index,
         " producerSig=", link.producer_signature_index,
         " consumerShader=", link.consumer_shader_index,
         " consumerSig=", link.consumer_signature_index,
         " producerMask=0x", std::hex, uint32_t(link.producer_component_mask),
         " consumerMask=0x", uint32_t(link.consumer_component_mask),
         std::dec);
  }
}

Com<ID3D12RootSignature>
ResolveRootSignature(IMTLD3D12Device *device,
                     ID3D12RootSignature *explicit_root_signature,
                     const std::vector<PipelineDxilShader> &shaders) {
  if (explicit_root_signature)
    return explicit_root_signature;

  for (const auto &shader : shaders) {
    const auto *info = shader.translation();
    if (!info || !info->has_root_signature || info->root_signature.empty())
      continue;

    auto blob = std::span<const std::byte>(
        reinterpret_cast<const std::byte *>(info->root_signature.data()),
        info->root_signature.size());
    auto root_signature = CreateRootSignatureFromBlob(device, blob);
    if (root_signature)
      return root_signature;
  }

  return nullptr;
}

bool
CopyCachedBlob(D3D12_CACHED_PIPELINE_STATE cached,
               std::vector<uint8_t> &out) {
  out.clear();
  if (!cached.CachedBlobSizeInBytes)
    return true;
  if (!cached.pCachedBlob)
    return false;

  const auto *bytes = static_cast<const uint8_t *>(cached.pCachedBlob);
  out.assign(bytes, bytes + cached.CachedBlobSizeInBytes);
  return true;
}

void
HashBytes(Sha1HashState &hash, const void *data, size_t size) {
  if (size)
    hash.update(data, size);
}

template <typename T>
void
HashValue(Sha1HashState &hash, const T &value) {
  hash.update(value);
}

void
HashString(Sha1HashState &hash, std::string_view value) {
  const auto size = uint32_t(value.size());
  HashValue(hash, size);
  HashBytes(hash, value.data(), value.size());
}

template <typename T>
void
HashVector(Sha1HashState &hash, const std::vector<T> &values) {
  const auto size = uint32_t(values.size());
  HashValue(hash, size);
  HashBytes(hash, values.data(), values.size() * sizeof(T));
}

void
HashPipelineShaders(Sha1HashState &hash,
                const std::vector<PipelineDxilShader> &shaders) {
  const auto count = uint32_t(shaders.size());
  HashValue(hash, count);
  for (const auto &shader : shaders) {
    HashValue(hash, shader.stage);
    HashVector(hash, shader.bytecode);
  }
}

std::string
PipelineShaderDigest(const PipelineDxilShader &shader) {
  return Sha1HashState::compute(shader.bytecode.data(), shader.bytecode.size())
      .string();
}

bool
D3D12PipelineFilterMatches(std::string filter, std::string_view value) {
  if (filter.empty())
    return false;
  if (filter.starts_with("0x") || filter.starts_with("0X"))
    filter = filter.substr(2);

  size_t start = 0;
  for (;;) {
    const size_t end = filter.find_first_of(",; ", start);
    const auto item =
        filter.substr(start, end == std::string::npos ? end : end - start);
    if (!item.empty() &&
        (value == item || value.substr(0, item.size()) == item))
      return true;
    if (end == std::string::npos)
      return false;
    start = end + 1;
  }
}

std::string
TrimFilterToken(std::string_view token) {
  const auto begin = token.find_first_not_of(" \t\r\n");
  if (begin == std::string_view::npos)
    return {};
  const auto end = token.find_last_not_of(" \t\r\n");
  return std::string(token.substr(begin, end - begin + 1));
}

size_t
ShaderBytecodeSize(const std::vector<PipelineDxilShader> &shaders,
                   PipelineShaderStage stage) {
  if (auto *shader = FindShader(shaders, stage))
    return shader->bytecode.size();
  return 0;
}

std::string
D3D12DebugShaderName(const std::vector<PipelineDxilShader> &shaders,
                     PipelineShaderStage stage) {
  auto *shader = FindShader(shaders, stage);
  if (!shader)
    return "null";

  std::stringstream stream;
  stream << PipelineShaderDigest(*shader) << "/" << shader->bytecode.size();
  return stream.str();
}

std::string
D3D12DebugPipelineDesc(const char *kind, std::string_view shader_cache_key,
                       const std::vector<PipelineDxilShader> &shaders,
                       const PipelineGraphicsState *graphics_state,
                       const PipelineComputeState *compute_state) {
  std::stringstream stream;
  stream << kind << " key=0x" << shader_cache_key
         << " VS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Vertex)
         << " HS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Hull)
         << " DS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Domain)
         << " GS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Geometry)
         << " PS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Pixel)
         << " CS=" << D3D12DebugShaderName(shaders, PipelineShaderStage::Compute)
         << " VS.bytes=" << ShaderBytecodeSize(shaders, PipelineShaderStage::Vertex)
         << " PS.bytes=" << ShaderBytecodeSize(shaders, PipelineShaderStage::Pixel)
         << " CS.bytes=" << ShaderBytecodeSize(shaders, PipelineShaderStage::Compute);

  if (graphics_state) {
    const auto &desc = graphics_state->desc;
    const auto &blend =
        desc.BlendState.RenderTarget[desc.BlendState.IndependentBlendEnable ? 0 : 0];
    stream << " inputs=" << graphics_state->input_elements.size()
           << " so_entries=" << graphics_state->stream_output_entries.size()
           << " rt_count=" << desc.NumRenderTargets
           << " dsv=" << uint32_t(desc.DSVFormat)
           << " topology=" << uint32_t(desc.PrimitiveTopologyType)
           << " sample_count=" << desc.SampleDesc.Count
           << " sample_quality=" << desc.SampleDesc.Quality
           << " sample_mask=0x" << std::hex << desc.SampleMask << std::dec
           << " alpha_to_coverage=" << uint32_t(desc.BlendState.AlphaToCoverageEnable)
           << " independent_blend=" << uint32_t(desc.BlendState.IndependentBlendEnable)
           << " depth_enable=" << uint32_t(desc.DepthStencilState.DepthEnable)
           << " depth_write=" << uint32_t(desc.DepthStencilState.DepthWriteMask)
           << " depth_func=" << uint32_t(desc.DepthStencilState.DepthFunc)
           << " stencil_enable=" << uint32_t(desc.DepthStencilState.StencilEnable)
           << " fill=" << uint32_t(desc.RasterizerState.FillMode)
           << " cull=" << uint32_t(desc.RasterizerState.CullMode)
           << " front_ccw=" << uint32_t(desc.RasterizerState.FrontCounterClockwise)
           << " depth_clip=" << uint32_t(desc.RasterizerState.DepthClipEnable)
           << " blend0=" << uint32_t(blend.BlendEnable)
           << " src0=" << uint32_t(blend.SrcBlend)
           << " dst0=" << uint32_t(blend.DestBlend)
           << " op0=" << uint32_t(blend.BlendOp)
           << " src_alpha0=" << uint32_t(blend.SrcBlendAlpha)
           << " dst_alpha0=" << uint32_t(blend.DestBlendAlpha)
           << " op_alpha0=" << uint32_t(blend.BlendOpAlpha)
           << " write0=" << uint32_t(blend.RenderTargetWriteMask)
           << " rtv=[";
    for (UINT i = 0; i < desc.NumRenderTargets && i < 8; i++) {
      if (i)
        stream << ",";
      stream << uint32_t(desc.RTVFormats[i]);
    }
    stream << "]";
    for (UINT i = 0; i < desc.NumRenderTargets && i < 8; i++)
      stream << " rtv" << i << "=" << uint32_t(desc.RTVFormats[i]);
    for (size_t i = 0; i < graphics_state->input_elements.size(); i++) {
      const auto &element = graphics_state->input_elements[i];
      stream << " input" << i << "="
             << (i < graphics_state->input_element_semantic_names.size()
                     ? graphics_state->input_element_semantic_names[i]
                     : "")
             << element.SemanticIndex << ":" << uint32_t(element.Format)
             << ":" << element.InputSlot << ":" << element.AlignedByteOffset
             << ":" << uint32_t(element.InputSlotClass) << ":"
             << element.InstanceDataStepRate;
    }
  }

  if (compute_state)
    stream << " flags=" << uint32_t(compute_state->desc.Flags);

  return stream.str();
}

bool
D3D12PipelineDescFilterMatches(std::string_view group,
                               std::string_view desc) {
  size_t start = 0;
  bool has_token = false;
  for (;;) {
    const size_t end = group.find_first_of("; ", start);
    const auto token = TrimFilterToken(group.substr(
        start, end == std::string_view::npos ? group.size() - start
                                             : end - start));
    if (!token.empty()) {
      has_token = true;
      if (desc.find(token) == std::string_view::npos)
        return false;
    }
    if (end == std::string_view::npos)
      return has_token;
    start = end + 1;
  }
}

bool
D3D12PipelineKeyFilterMatches(std::string filter, std::string_view key,
                              std::string_view desc) {
  if (filter.find('=') == std::string::npos)
    return D3D12PipelineFilterMatches(std::move(filter), key);

  size_t start = 0;
  for (;;) {
    const size_t end = filter.find(',', start);
    const auto group = std::string_view(filter).substr(
        start, end == std::string::npos ? filter.size() - start
                                        : end - start);
    const auto trimmed = TrimFilterToken(group);
    if (!trimmed.empty() &&
        (D3D12PipelineFilterMatches(trimmed, key) ||
         D3D12PipelineDescFilterMatches(trimmed, desc)))
      return true;
    if (end == std::string::npos)
      return false;
    start = end + 1;
  }
}

std::string
D3D12PipelineStageFilter(PipelineShaderStage stage) {
  switch (stage) {
  case PipelineShaderStage::Vertex:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_VS");
  case PipelineShaderStage::Pixel:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_PS");
  case PipelineShaderStage::Geometry:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_GS");
  case PipelineShaderStage::Hull:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_HS");
  case PipelineShaderStage::Domain:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_DS");
  case PipelineShaderStage::Compute:
    return env::getEnvVar("DXMT_DUMP_PIPELINE_CS");
  }
  return {};
}

bool
D3D12PipelineShaderFilterMatches(
    const std::vector<PipelineDxilShader> &shaders, PipelineShaderStage stage,
    std::string filter) {
  if (filter.empty())
    return true;

  auto *shader = FindShader(shaders, stage);
  if (!shader)
    return filter == "null";

  return D3D12PipelineFilterMatches(std::move(filter), PipelineShaderDigest(*shader));
}

bool
D3D12PipelineGraphicsStageFiltersMatch(
    const std::vector<PipelineDxilShader> &shaders) {
  constexpr PipelineShaderStage stages[] = {
      PipelineShaderStage::Vertex, PipelineShaderStage::Pixel,
      PipelineShaderStage::Geometry, PipelineShaderStage::Hull,
      PipelineShaderStage::Domain};

  bool has_stage_filter = false;
  for (auto stage : stages) {
    if (!D3D12PipelineStageFilter(stage).empty()) {
      has_stage_filter = true;
      break;
    }
  }
  if (!has_stage_filter)
    return false;

  for (auto stage : stages) {
    if (!D3D12PipelineShaderFilterMatches(
            shaders, stage, D3D12PipelineStageFilter(stage)))
      return false;
  }
  return true;
}

bool
D3D12ShouldDumpPipeline(std::string_view shader_cache_key,
                        const std::vector<PipelineDxilShader> &shaders,
                        const PipelineGraphicsState *graphics_state,
                        const PipelineComputeState *compute_state,
                        bool compute) {
  std::string mode = compute ? env::getEnvVar("DXMT_DUMP_COMPUTE_SHADERS")
                             : env::getEnvVar("DXMT_DUMP_PIPELINES");
  if (mode == "0" || mode == "none" || mode == "false")
    return false;

  auto key = env::getEnvVar("DXMT_DUMP_PIPELINE_KEY");
  const auto desc = D3D12DebugPipelineDesc(compute ? "compute" : "graphics",
                                           shader_cache_key, shaders,
                                           graphics_state, compute_state);
  if (!key.empty())
    return D3D12PipelineKeyFilterMatches(key, shader_cache_key, desc);

  if (compute) {
    if (mode == "1" || mode == "all")
      return true;

    auto cs_filter = mode.empty()
                         ? D3D12PipelineStageFilter(PipelineShaderStage::Compute)
                         : mode;
    if (cs_filter.empty())
      return false;

    return D3D12PipelineShaderFilterMatches(
        shaders, PipelineShaderStage::Compute, std::move(cs_filter));
  }

  if (D3D12PipelineGraphicsStageFiltersMatch(shaders))
    return true;

  return mode == "1" || mode == "all";
}

void
D3D12DumpPipelineShaders(const char *kind, std::string_view shader_cache_key,
                         const std::vector<PipelineDxilShader> &shaders,
                         const PipelineGraphicsState *graphics_state,
                         const PipelineComputeState *compute_state) {
  const bool compute = std::string_view(kind) == "compute";
  if (!D3D12ShouldDumpPipeline(shader_cache_key, shaders, graphics_state,
                               compute_state, compute))
    return;

  const auto dir = D3D12PipelineDumpDirectory();
  const auto key_prefix =
      std::string(shader_cache_key.substr(0, std::min<size_t>(16, shader_cache_key.size())));
  const auto manifest_path =
      dir + env::getExeBaseName() + "_d3d12_pipeline_" + kind + "_" +
      key_prefix + ".txt";

  std::ofstream manifest(manifest_path, std::ios::out | std::ios::trunc);
  if (manifest)
    manifest << "kind=" << kind << "\n"
             << "pso=" << shader_cache_key << "\n"
             << D3D12DebugPipelineDesc(kind, shader_cache_key, shaders,
                                       graphics_state, compute_state)
             << "\n";

  for (const auto &shader : shaders) {
    const auto digest = PipelineShaderDigest(shader);
    const auto stage = ShaderStageName(shader.stage);
    const auto filename = env::getExeBaseName() + "_d3d12_" + key_prefix +
                          "_" + stage + "_" + digest.substr(0, 16) + ".dxil";
    const auto path = dir + filename;
    std::ofstream dump(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (dump && !shader.bytecode.empty())
      dump.write(reinterpret_cast<const char *>(shader.bytecode.data()),
                 shader.bytecode.size());
    if (manifest)
      manifest << stage << "=" << digest << " " << filename
               << " bytes=" << shader.bytecode.size() << "\n";
  }

  WARN("D3D12PipelineState: dumped ", kind, " pipeline shaders to ",
       manifest_path);
}

void
HashGraphicsInputElements(Sha1HashState &hash,
                          const PipelineGraphicsState &graphics_state) {
  HashValue(hash, uint32_t(graphics_state.input_elements.size()));
  for (size_t i = 0; i < graphics_state.input_elements.size(); i++) {
    const auto &element = graphics_state.input_elements[i];
    HashString(hash, i < graphics_state.input_element_semantic_names.size()
                         ? graphics_state.input_element_semantic_names[i]
                         : std::string_view());
    HashValue(hash, element.SemanticIndex);
    HashValue(hash, element.Format);
    HashValue(hash, element.InputSlot);
    HashValue(hash, element.AlignedByteOffset);
    HashValue(hash, element.InputSlotClass);
    HashValue(hash, element.InstanceDataStepRate);
  }
}

void
HashGraphicsStreamOutput(Sha1HashState &hash,
                         const PipelineGraphicsState &graphics_state) {
  HashValue(hash, uint32_t(graphics_state.stream_output_entries.size()));
  for (size_t i = 0; i < graphics_state.stream_output_entries.size(); i++) {
    const auto &entry = graphics_state.stream_output_entries[i];
    HashValue(hash, entry.Stream);
    HashString(hash, i < graphics_state.stream_output_semantic_names.size()
                         ? graphics_state.stream_output_semantic_names[i]
                         : std::string_view());
    HashValue(hash, entry.SemanticIndex);
    HashValue(hash, entry.StartComponent);
    HashValue(hash, entry.ComponentCount);
    HashValue(hash, entry.OutputSlot);
  }
  HashVector(hash, graphics_state.stream_output_strides);
}

std::string
BuildShaderCacheKey(PipelineStateType type,
                    const std::vector<PipelineDxilShader> &shaders,
                    const PipelineGraphicsState &graphics_state,
                    const PipelineComputeState &compute_state,
                    ID3D12RootSignature *root_signature) {
  Sha1HashState hash;
  HashString(hash, type == PipelineStateType::Graphics
                       ? "dxmt-d3d12-graphics-pipeline-cache-v2"
                       : "dxmt-d3d12-compute-pipeline-cache-v2");
  HashPipelineShaders(hash, shaders);
  if (type == PipelineStateType::Graphics) {
    HashGraphicsInputElements(hash, graphics_state);
    HashGraphicsStreamOutput(hash, graphics_state);
    HashValue(hash, graphics_state.desc.BlendState);
    HashValue(hash, graphics_state.desc.SampleMask);
    HashValue(hash, graphics_state.desc.RasterizerState);
    HashValue(hash, graphics_state.desc.DepthStencilState);
    HashValue(hash, graphics_state.desc.PrimitiveTopologyType);
    HashValue(hash, graphics_state.desc.NumRenderTargets);
    HashBytes(hash, graphics_state.desc.RTVFormats,
              sizeof(graphics_state.desc.RTVFormats));
    HashValue(hash, graphics_state.desc.DSVFormat);
    HashValue(hash, graphics_state.desc.SampleDesc);
    HashValue(hash, graphics_state.desc.Flags);
  } else {
    HashValue(hash, compute_state.desc.Flags);
  }
  if (auto *root = dynamic_cast<RootSignature *>(root_signature)) {
    const auto blob = root->GetSerializedBlob();
    HashValue(hash, uint32_t(blob.size()));
    HashBytes(hash, blob.data(), blob.size());
  }
  return hash.final().string();
}

std::vector<uint8_t>
BuildCachedShaderBlob(PipelineStateType type,
                      const PipelineGraphicsState &graphics_state,
                      const PipelineComputeState &compute_state,
                      std::string_view shader_cache_key) {
  if (type == PipelineStateType::Graphics && !graphics_state.cached_pso.empty())
    return graphics_state.cached_pso;
  if (type == PipelineStateType::Compute && !compute_state.cached_pso.empty())
    return compute_state.cached_pso;

  constexpr std::string_view magic = "DXMT:D3D12:ShaderCache:";
  std::vector<uint8_t> blob;
  blob.reserve(magic.size() + shader_cache_key.size());
  blob.insert(blob.end(), magic.begin(), magic.end());
  blob.insert(blob.end(), shader_cache_key.begin(), shader_cache_key.end());
  return blob;
}

void
DestroyPipelineShaders(std::vector<PipelineDxilShader> &shaders) {
  for (auto &shader : shaders) {
    if (shader.shader) {
      if (shader.kind == PipelineShaderBytecodeKind::Dxil)
        DXILDestroy(shader.shader);
      else
        SM50Destroy(shader.shader);
      shader.shader = nullptr;
    }
  }
}

std::string
BuildFunctionName(const char *prefix, std::string_view key) {
  std::string name(prefix);
  name += "_";
  name += key.substr(0, std::min<size_t>(key.size(), 16));
  return name;
}

SM50_SHADER_METAL_VERSION
GetMetalVersion(IMTLD3D12Device *device) {
  return static_cast<SM50_SHADER_METAL_VERSION>(
      device->GetDXMTDevice().metalVersion());
}

SM50_SHADER_FLAG
GetShaderFlags() {
  return SM50_SHADER_FLAG(0);
}

bool
CompileMetalFunction(IMTLD3D12Device *device, PipelineDxilShader &shader,
                     const char *function_name,
                     SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
                     PipelineMetalShader &out) {
  sm50_bitcode_t bitcode_handle = nullptr;
  sm50_error_t error = nullptr;
  const auto compile_failed =
      shader.kind == PipelineShaderBytecodeKind::Dxil
          ? DXILCompile(shader.shader, args, function_name, &bitcode_handle,
                        &error)
          : SM50Compile(shader.shader, args, function_name, &bitcode_handle,
                        &error);
  if (compile_failed) {
    WARN("D3D12PipelineState: failed to compile ", ShaderStageName(shader.stage),
         shader.kind == PipelineShaderBytecodeKind::Dxil ? " DXIL shader: "
                                                         : " DXBC shader: ",
         SM50GetErrorMessageString(error));
    SM50FreeError(error);
    return false;
  }

  SM50_COMPILED_BITCODE bitcode = {};
  SM50GetCompiledBitcode(bitcode_handle, &bitcode);

  WMT::Reference<WMT::Error> metal_error;
  auto lib_data = WMT::MakeDispatchData(bitcode.Data, bitcode.Size);
  out.library = device->GetMTLDevice().newLibrary(lib_data, metal_error);
  SM50DestroyBitcode(bitcode_handle);
  if (metal_error || !out.library) {
    WARN("D3D12PipelineState: failed to create Metal library for ",
         ShaderStageName(shader.stage), ": ",
         metal_error ? metal_error.description().getUTF8String()
                     : "unknown error");
    return false;
  }

  out.function = out.library.newFunction(function_name);
  if (!out.function) {
    WARN("D3D12PipelineState: Metal function not found: ", function_name);
    return false;
  }

  return true;
}

bool
CreateMetalFunctionFromBitcode(IMTLD3D12Device *device,
                               PipelineShaderStage stage,
                               const char *function_name,
                               sm50_bitcode_t bitcode_handle,
                               PipelineMetalShader &out) {
  SM50_COMPILED_BITCODE bitcode = {};
  SM50GetCompiledBitcode(bitcode_handle, &bitcode);

  WMT::Reference<WMT::Error> metal_error;
  auto lib_data = WMT::MakeDispatchData(bitcode.Data, bitcode.Size);
  out.library = device->GetMTLDevice().newLibrary(lib_data, metal_error);
  SM50DestroyBitcode(bitcode_handle);
  if (metal_error || !out.library) {
    WARN("D3D12PipelineState: failed to create Metal library for ",
         ShaderStageName(stage), ": ",
         metal_error ? metal_error.description().getUTF8String()
                     : "unknown error");
    return false;
  }

  out.function = out.library.newFunction(function_name);
  if (!out.function) {
    WARN("D3D12PipelineState: Metal function not found: ", function_name);
    return false;
  }

  return true;
}

bool
CompileGeometryPipelineVertexFunction(
    IMTLD3D12Device *device, PipelineDxilShader &vs, PipelineDxilShader &gs,
    const char *function_name, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    PipelineMetalShader &out) {
  if (vs.kind != PipelineShaderBytecodeKind::Dxbc ||
      gs.kind != PipelineShaderBytecodeKind::Dxbc) {
    WARN("D3D12PipelineState: DXIL geometry shader mesh lowering is unsupported");
    return false;
  }

  sm50_bitcode_t bitcode_handle = nullptr;
  sm50_error_t error = nullptr;
  if (SM50CompileGeometryPipelineVertex(vs.shader, gs.shader, args,
                                        function_name, &bitcode_handle,
                                        &error)) {
    WARN("D3D12PipelineState: failed to compile DXBC geometry vertex shader: ",
         SM50GetErrorMessageString(error));
    SM50FreeError(error);
    return false;
  }

  return CreateMetalFunctionFromBitcode(
      device, PipelineShaderStage::Vertex, function_name, bitcode_handle, out);
}

bool
CompileGeometryPipelineGeometryFunction(
    IMTLD3D12Device *device, PipelineDxilShader &vs, PipelineDxilShader &gs,
    const char *function_name, SM50_SHADER_COMPILATION_ARGUMENT_DATA *args,
    PipelineMetalShader &out) {
  if (vs.kind != PipelineShaderBytecodeKind::Dxbc ||
      gs.kind != PipelineShaderBytecodeKind::Dxbc) {
    WARN("D3D12PipelineState: DXIL geometry shader mesh lowering is unsupported");
    return false;
  }

  sm50_bitcode_t bitcode_handle = nullptr;
  sm50_error_t error = nullptr;
  if (SM50CompileGeometryPipelineGeometry(vs.shader, gs.shader, args,
                                          function_name, &bitcode_handle,
                                          &error)) {
    WARN("D3D12PipelineState: failed to compile DXBC geometry shader: ",
         SM50GetErrorMessageString(error));
    SM50FreeError(error);
    return false;
  }

  return CreateMetalFunctionFromBitcode(
      device, PipelineShaderStage::Geometry, function_name, bitcode_handle,
      out);
}

WMTPrimitiveTopologyClass
GetTopologyClass(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) {
  switch (type) {
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT:
    return WMTPrimitiveTopologyClassPoint;
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE:
    return WMTPrimitiveTopologyClassLine;
  case D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE:
    return WMTPrimitiveTopologyClassTriangle;
  default:
    return WMTPrimitiveTopologyClassUnspecified;
  }
}

bool
BuildInputElements(IMTLD3D12Device *device,
                   const PipelineGraphicsState &state,
                   const PipelineDxilShader *vs,
                   std::vector<SM50_IA_INPUT_ELEMENT> &elements,
                   uint32_t &slot_mask) {
  elements.clear();
  slot_mask = 0;
  std::array<uint32_t, 32> append_offsets = {};
  std::vector<SM50_IA_INPUT_ELEMENT> layout_elements;
  layout_elements.reserve(state.input_elements.size());

  for (uint32_t i = 0; i < state.input_elements.size(); i++) {
    const auto &input = state.input_elements[i];
    if (input.InputSlot >= append_offsets.size()) {
      WARN("D3D12PipelineState: unsupported input layout slot ",
           uint32_t(input.InputSlot));
      return false;
    }

    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), input.Format,
                                  format)) ||
        format.AttributeFormat == WMTAttributeFormatInvalid) {
      WARN("D3D12PipelineState: unsupported input layout format ",
           uint32_t(input.Format));
      return false;
    }
    if (!format.BytesPerTexel) {
      WARN("D3D12PipelineState: unsupported non-ordinary input layout format ",
           uint32_t(input.Format));
      return false;
    }

    const auto aligned_byte_offset =
        input.AlignedByteOffset == D3D12_APPEND_ALIGNED_ELEMENT
            ? align(append_offsets[input.InputSlot],
                    std::min(4u, format.BytesPerTexel))
            : input.AlignedByteOffset;
    append_offsets[input.InputSlot] =
        aligned_byte_offset + format.BytesPerTexel;

    layout_elements.push_back({
        .reg = i,
        .slot = input.InputSlot,
        .aligned_byte_offset = aligned_byte_offset,
        .format = uint32_t(format.AttributeFormat),
        .step_function = input.InputSlotClass ==
                         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
        .step_rate = input.InstanceDataStepRate,
    });
    if (D3D12PipelineDiagShouldLog()) {
      INFO("D3D12 diagnostic: pipeline input element",
           " semantic=", input.SemanticName ? input.SemanticName : "",
           " semanticIndex=", uint32_t(input.SemanticIndex),
           " format=", uint32_t(input.Format),
           " slot=", uint32_t(input.InputSlot),
           " sourceOffset=", uint32_t(input.AlignedByteOffset),
           " loweredOffset=", uint32_t(aligned_byte_offset),
           " attrFormat=", uint32_t(format.AttributeFormat),
           " bytes=", uint32_t(format.BytesPerTexel),
           " slotClass=", uint32_t(input.InputSlotClass),
           " stepRate=", uint32_t(input.InstanceDataStepRate),
           " reg=", i);
    }
    if (input.InputSlot < 32)
      slot_mask |= 1u << input.InputSlot;
  }

  if (!vs) {
    elements = std::move(layout_elements);
    return true;
  }

  const auto *translation = vs->translation();
  if (!translation) {
    elements = std::move(layout_elements);
    return true;
  }

  auto equal_semantic = [](std::string_view lhs, std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin(),
                      [](char a, char b) {
                        return std::tolower(static_cast<unsigned char>(a)) ==
                               std::tolower(static_cast<unsigned char>(b));
                      });
  };

  for (const auto &sig : translation->signatures) {
    if (sig.kind != dxil::DxilTranslationSignatureKind::Input ||
        IsSystemGeneratedValue(sig))
      continue;

    auto match = std::find_if(
        state.input_elements.begin(), state.input_elements.end(),
        [&](const D3D12_INPUT_ELEMENT_DESC &input) {
          const auto index =
              uint32_t(&input - state.input_elements.data());
          const auto semantic =
              index < state.input_element_semantic_names.size()
                  ? std::string_view(state.input_element_semantic_names[index])
                  : std::string_view();
          if (!sig.semantic_name.empty())
            return input.SemanticIndex == sig.semantic_index &&
                   equal_semantic(semantic, sig.semantic_name);
          return equal_semantic(
              str::format(semantic, input.SemanticIndex), sig.semantic_key);
        });
    if (match == state.input_elements.end())
      continue;

    const auto layout_index = size_t(match - state.input_elements.begin());
    auto element = layout_elements[layout_index];
    element.reg = sig.start_row;
    elements.push_back(element);
  }

  if (elements.empty())
    elements = std::move(layout_elements);

  return true;
}

template <typename T, size_t N>
const T &
Lookup(const T (&table)[N], uint32_t index, const T &fallback) {
  return index < N ? table[index] : fallback;
}

bool
IsDualSourceBlend(const D3D12_RENDER_TARGET_BLEND_DESC &desc) {
  return desc.BlendEnable &&
         (desc.SrcBlend >= D3D12_BLEND_SRC1_COLOR ||
          desc.DestBlend >= D3D12_BLEND_SRC1_COLOR ||
          desc.SrcBlendAlpha >= D3D12_BLEND_SRC1_COLOR ||
          desc.DestBlendAlpha >= D3D12_BLEND_SRC1_COLOR);
}

bool
UsesDualSourceBlending(const D3D12_BLEND_DESC &desc) {
  const auto count = desc.IndependentBlendEnable ? 8u : 1u;
  for (UINT i = 0; i < count; i++) {
    if (IsDualSourceBlend(desc.RenderTarget[i]))
      return true;
  }
  return false;
}

bool
ValidateGraphicsRenderFormats(IMTLD3D12Device *device,
                              const PipelineGraphicsState &state,
                              std::array<WMTPixelFormat, 8> &rtv_formats,
                              WMTPixelFormat &depth_format,
                              WMTPixelFormat &stencil_format) {
  rtv_formats.fill(WMTPixelFormatInvalid);
  depth_format = WMTPixelFormatInvalid;
  stencil_format = WMTPixelFormatInvalid;

  for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
    const auto format = state.desc.RTVFormats[i];
    if (format == DXGI_FORMAT_UNKNOWN)
      continue;
    const auto pixel_format =
        GetRenderTargetPixelFormat(device->GetMTLDevice(), format);
    if (pixel_format == WMTPixelFormatInvalid ||
        DepthStencilPlanarFlags(pixel_format)) {
      WARN("D3D12PipelineState: unsupported RTV format slot=", i,
           " format=", uint32_t(format));
      return false;
    }
    rtv_formats[i] = pixel_format;
  }

  if (state.desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(),
                                  state.desc.DSVFormat, format)) ||
        format.PixelFormat == WMTPixelFormatInvalid ||
        !DepthStencilPlanarFlags(format.PixelFormat)) {
      WARN("D3D12PipelineState: unsupported DSV format format=",
           uint32_t(state.desc.DSVFormat));
      return false;
    }
    if (DepthStencilPlanarFlags(format.PixelFormat) & 1)
      depth_format = format.PixelFormat;
    if (DepthStencilPlanarFlags(format.PixelFormat) & 2)
      stencil_format = format.PixelFormat;
  }

  return true;
}

template <typename PipelineInfo>
void
ApplyBlendState(PipelineInfo &info,
                const D3D12_BLEND_DESC &blend_desc,
                uint32_t render_target_count) {
  for (UINT rt = 0; rt < render_target_count && rt < 8; rt++) {
    const auto &src =
        blend_desc.RenderTarget[blend_desc.IndependentBlendEnable ? rt : 0];
    auto &dst = info.colors[rt];
    dst.write_mask =
        Lookup(kColorWriteMaskMap, uint32_t(src.RenderTargetWriteMask),
               kColorWriteMaskMap[15]);
    if (!src.BlendEnable || dst.pixel_format == WMTPixelFormatInvalid)
      continue;

    dst.blending_enabled = true;
    dst.rgb_blend_operation =
        Lookup(kBlendOpMap, uint32_t(src.BlendOp), WMTBlendOperationAdd);
    dst.alpha_blend_operation =
        Lookup(kBlendOpMap, uint32_t(src.BlendOpAlpha), WMTBlendOperationAdd);
    dst.src_rgb_blend_factor =
        Lookup(kBlendFactorMap, uint32_t(src.SrcBlend), WMTBlendFactorOne);
    dst.dst_rgb_blend_factor =
        Lookup(kBlendFactorMap, uint32_t(src.DestBlend), WMTBlendFactorZero);
    dst.src_alpha_blend_factor =
        Lookup(kBlendAlphaFactorMap, uint32_t(src.SrcBlendAlpha),
               WMTBlendFactorOne);
    dst.dst_alpha_blend_factor =
        Lookup(kBlendAlphaFactorMap, uint32_t(src.DestBlendAlpha),
               WMTBlendFactorZero);
  }

  info.alpha_to_coverage_enabled = blend_desc.AlphaToCoverageEnable;
  if (!blend_desc.IndependentBlendEnable &&
      blend_desc.RenderTarget[0].LogicOpEnable) {
    info.logic_operation_enabled = true;
    info.logic_operation =
        Lookup(kLogicOpMap, uint32_t(blend_desc.RenderTarget[0].LogicOp),
               WMTLogicOperationCopy);
  }
}

void
BuildRasterizerCommand(const D3D12_RASTERIZER_DESC &desc,
                       wmtcmd_render_setrasterizerstate &cmd) {
  cmd = {};
  cmd.type = WMTRenderCommandSetRasterizerState;
  cmd.fill_mode = desc.FillMode == D3D12_FILL_MODE_WIREFRAME
                      ? WMTTriangleFillModeLines
                      : WMTTriangleFillModeFill;
  switch (desc.CullMode) {
  case D3D12_CULL_MODE_FRONT:
    cmd.cull_mode = WMTCullModeFront;
    break;
  case D3D12_CULL_MODE_BACK:
    cmd.cull_mode = WMTCullModeBack;
    break;
  default:
    cmd.cull_mode = WMTCullModeNone;
    break;
  }
  cmd.depth_clip_mode =
      desc.DepthClipEnable ? WMTDepthClipModeClip : WMTDepthClipModeClamp;
  cmd.winding = desc.FrontCounterClockwise ? WMTWindingCounterClockwise
                                           : WMTWindingClockwise;
  cmd.depth_bias = float(desc.DepthBias);
  cmd.scole_scale = desc.SlopeScaledDepthBias;
  cmd.depth_bias_clamp = desc.DepthBiasClamp;
}

void
FillStencilInfo(WMTStencilInfo &dst,
                const D3D12_DEPTH_STENCILOP_DESC &src,
                UINT8 read_mask, UINT8 write_mask) {
  dst.enabled = true;
  dst.depth_stencil_pass_op =
      Lookup(kStencilOperationMap, uint32_t(src.StencilPassOp),
             WMTStencilOperationKeep);
  dst.stencil_fail_op =
      Lookup(kStencilOperationMap, uint32_t(src.StencilFailOp),
             WMTStencilOperationKeep);
  dst.depth_fail_op =
      Lookup(kStencilOperationMap, uint32_t(src.StencilDepthFailOp),
             WMTStencilOperationKeep);
  dst.stencil_compare_function =
      Lookup(kCompareFunctionMap, uint32_t(src.StencilFunc),
             WMTCompareFunctionAlways);
  dst.read_mask = read_mask;
  dst.write_mask = write_mask;
}

WMT::Reference<WMT::DepthStencilState>
CreateDepthStencilState(IMTLD3D12Device *device,
                        const D3D12_DEPTH_STENCIL_DESC &desc) {
  WMTDepthStencilInfo info = {};
  info.depth_compare_function = WMTCompareFunctionAlways;
  info.depth_write_enabled = false;
  info.front_stencil.enabled = false;
  info.back_stencil.enabled = false;

  if (desc.DepthEnable) {
    info.depth_compare_function =
        Lookup(kCompareFunctionMap, uint32_t(desc.DepthFunc),
               WMTCompareFunctionAlways);
    info.depth_write_enabled =
        desc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
  }

  if (desc.StencilEnable) {
    FillStencilInfo(info.front_stencil, desc.FrontFace,
                    desc.StencilReadMask, desc.StencilWriteMask);
    FillStencilInfo(info.back_stencil, desc.BackFace,
                    desc.StencilReadMask, desc.StencilWriteMask);
  }

  return device->GetMTLDevice().newDepthStencilState(info);
}

bool
CreateMetalGraphicsPipeline(IMTLD3D12Device *device,
                            std::vector<PipelineDxilShader> &shaders,
                            const PipelineGraphicsState &state,
                            std::string_view shader_cache_key,
                            PipelineMetalGraphicsState &out) {
  auto *vs = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Vertex));
  auto *ps = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Pixel));
  auto *gs = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Geometry));
  if (!vs)
    return false;

  SM50_SHADER_COMMON_DATA common = {};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = GetMetalVersion(device);
  common.flags = GetShaderFlags();
  common.next = nullptr;

  std::vector<SM50_IA_INPUT_ELEMENT> input_elements;
  uint32_t slot_mask = 0;
  if (!BuildInputElements(device, state, vs, input_elements, slot_mask))
    return false;

  std::array<WMTPixelFormat, 8> rtv_formats = {};
  WMTPixelFormat depth_format = WMTPixelFormatInvalid;
  WMTPixelFormat stencil_format = WMTPixelFormatInvalid;
  if (!ValidateGraphicsRenderFormats(device, state, rtv_formats,
                                     depth_format, stencil_format))
    return false;

  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout = {};
  ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
  ia_layout.next = &common;
  ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
  ia_layout.slot_mask = slot_mask;
  ia_layout.num_elements = uint32_t(input_elements.size());
  ia_layout.elements = input_elements.empty() ? nullptr : input_elements.data();

  const auto vs_name = BuildFunctionName("vs", shader_cache_key);
  for (const auto &shader : shaders)
    DebugLogDxilShaderInfo(shader_cache_key, shader);
  D3D12DumpPipelineShaders("graphics", shader_cache_key, shaders, &state,
                           nullptr);

  uint32_t unorm_output_reg_mask = 0;
  for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
    if (IsUnorm8RenderTargetFormat(rtv_formats[i]))
      unorm_output_reg_mask |= 1u << i;
  }
  if (ps) {
    SM50_SHADER_PSO_PIXEL_SHADER_DATA ps_args = {};
    ps_args.type = SM50_SHADER_PSO_PIXEL_SHADER;
    ps_args.next = &common;
    ps_args.sample_mask = state.desc.SampleMask;
    ps_args.dual_source_blending =
        UsesDualSourceBlending(state.desc.BlendState);
    ps_args.disable_depth_output = state.desc.DSVFormat == DXGI_FORMAT_UNKNOWN;
    ps_args.unorm_output_reg_mask = unorm_output_reg_mask;

    const auto ps_name = BuildFunctionName("ps", shader_cache_key);
    if (!CompileMetalFunction(device, *ps, ps_name.c_str(),
                              reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&ps_args),
                              out.pixel))
      return false;
  }

  if (gs) {
    auto create_geometry_variant =
        [&](bool strip_topology, PipelineMetalShader &object_shader,
            PipelineMetalShader &mesh_shader,
            WMT::Reference<WMT::RenderPipelineState> &pso) {
      SM50_SHADER_PSO_GEOMETRY_SHADER_DATA geometry_args = {};
      geometry_args.type = SM50_SHADER_PSO_GEOMETRY_SHADER;
      geometry_args.next = &ia_layout;
      geometry_args.strip_topology = strip_topology;

      const auto vsgs_name = BuildFunctionName(
          strip_topology ? "vsgs_strip" : "vsgs", shader_cache_key);
      if (!CompileGeometryPipelineVertexFunction(
              device, *vs, *gs, vsgs_name.c_str(),
              reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
                  &geometry_args),
              object_shader))
        return false;

      geometry_args.next = &common;
      const auto gs_name = BuildFunctionName(
          strip_topology ? "gs_strip" : "gs", shader_cache_key);
      if (!CompileGeometryPipelineGeometryFunction(
              device, *vs, *gs, gs_name.c_str(),
              reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(
                  &geometry_args),
              mesh_shader))
        return false;

      WMTMeshRenderPipelineInfo info = {};
      WMT::InitializeMeshRenderPipelineInfo(info);
      info.object_function = object_shader.function;
      info.mesh_function = mesh_shader.function;
      info.fragment_function = out.pixel.function;
      info.payload_memory_length = 16256;
      info.rasterization_enabled = true;
      info.raster_sample_count = state.desc.SampleDesc.Count;
      info.immutable_object_buffers =
          (1 << 16) | (1 << 21) | (1 << 29) | (1 << 30);
      info.immutable_mesh_buffers = (1 << 29) | (1 << 30);
      info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

      for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
        if (state.desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN)
          info.colors[i].pixel_format = rtv_formats[i];
      }
      ApplyBlendState(info, state.desc.BlendState, state.desc.NumRenderTargets);

      if (state.desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
        info.depth_pixel_format = depth_format;
        info.stencil_pixel_format = stencil_format;
      }

      WMT::Reference<WMT::Error> error;
      pso = device->GetMTLDevice().newRenderPipelineState(info, error);
      if (error || !pso) {
        WARN("D3D12PipelineState: failed to create Metal geometry PSO: ",
             error ? error.description().getUTF8String() : "unknown error");
        return false;
      }
      return true;
    };

    if (!create_geometry_variant(false, out.geometry_vertex, out.geometry,
                                 out.pso) ||
        !create_geometry_variant(true, out.geometry_strip_vertex,
                                 out.geometry_strip, out.strip_pso))
      return false;

    out.use_geometry = true;
    BuildRasterizerCommand(state.desc.RasterizerState, out.rasterizer);
    out.depth_stencil =
        CreateDepthStencilState(device, state.desc.DepthStencilState);
    return true;
  }

  if (!CompileMetalFunction(device, *vs, vs_name.c_str(),
                            reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&ia_layout),
                            out.vertex))
    return false;

  WMTRenderPipelineInfo info = {};
  WMT::InitializeRenderPipelineInfo(info);
  info.vertex_function = out.vertex.function;
  info.fragment_function = out.pixel.function;
  info.rasterization_enabled = true;
  info.raster_sample_count = state.desc.SampleDesc.Count;
  info.input_primitive_topology =
      GetTopologyClass(state.desc.PrimitiveTopologyType);
  info.immutable_vertex_buffers = (1 << 16) | (1 << 29) | (1 << 30);
  info.immutable_fragment_buffers = (1 << 29) | (1 << 30);

  for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
    if (state.desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN)
      info.colors[i].pixel_format = rtv_formats[i];
  }
  ApplyBlendState(info, state.desc.BlendState, state.desc.NumRenderTargets);

  if (state.desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
    info.depth_pixel_format = depth_format;
    info.stencil_pixel_format = stencil_format;
  }

  WMT::Reference<WMT::Error> error;
  out.pso = device->GetMTLDevice().newRenderPipelineState(info, error);
  if (error || !out.pso) {
    WARN("D3D12PipelineState: failed to create Metal graphics PSO: ",
         error ? error.description().getUTF8String() : "unknown error");
    return false;
  }
  BuildRasterizerCommand(state.desc.RasterizerState, out.rasterizer);
  out.depth_stencil =
      CreateDepthStencilState(device, state.desc.DepthStencilState);

  return true;
}

bool
CreateMetalComputePipeline(IMTLD3D12Device *device,
                           std::vector<PipelineDxilShader> &shaders,
                           std::string_view shader_cache_key,
                           PipelineMetalComputeState &out) {
  auto *cs = const_cast<PipelineDxilShader *>(
      FindShader(shaders, PipelineShaderStage::Compute));
  if (!cs)
    return false;

  SM50_SHADER_COMMON_DATA common = {};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = GetMetalVersion(device);
  common.flags = GetShaderFlags();
  common.next = nullptr;

  const auto cs_name = BuildFunctionName("cs", shader_cache_key);
  D3D12DumpPipelineShaders("compute", shader_cache_key, shaders, nullptr,
                           nullptr);
  if (!CompileMetalFunction(device, *cs, cs_name.c_str(),
                            reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&common),
                            out.compute))
    return false;

  WMTComputePipelineInfo info = {};
  WMT::InitializeComputePipelineInfo(info);
  info.compute_function = out.compute.function;
  info.immutable_buffers = (1 << 29) | (1 << 30);
  const auto tgx = std::max<uint32_t>(1, cs->reflection.ThreadgroupSize[0]);
  const auto tgy = std::max<uint32_t>(1, cs->reflection.ThreadgroupSize[1]);
  const auto tgz = std::max<uint32_t>(1, cs->reflection.ThreadgroupSize[2]);
  info.tgsize_is_multiple_of_sgwidth = ((tgx * tgy * tgz) % 32) == 0;

  WMT::Reference<WMT::Error> error;
  out.pso = device->GetMTLDevice().newComputePipelineState(info, error);
  if (error || !out.pso) {
    WARN("D3D12PipelineState: failed to create Metal compute PSO: ",
         error ? error.description().getUTF8String() : "unknown error");
    return false;
  }

  out.threadgroup_size = {tgx, tgy, tgz};
  return true;
}

void
FixGraphicsStatePointers(PipelineGraphicsState &state,
                         const std::vector<PipelineDxilShader> &shaders) {
  state.desc.VS = ShaderBytecodeView(shaders, PipelineShaderStage::Vertex);
  state.desc.PS = ShaderBytecodeView(shaders, PipelineShaderStage::Pixel);
  state.desc.DS = ShaderBytecodeView(shaders, PipelineShaderStage::Domain);
  state.desc.HS = ShaderBytecodeView(shaders, PipelineShaderStage::Hull);
  state.desc.GS = ShaderBytecodeView(shaders, PipelineShaderStage::Geometry);

  for (size_t i = 0; i < state.input_elements.size(); i++) {
    state.input_elements[i].SemanticName =
        state.input_element_semantic_names[i].empty()
            ? nullptr
            : state.input_element_semantic_names[i].c_str();
  }
  state.desc.InputLayout.NumElements =
      UINT(state.input_elements.size());
  state.desc.InputLayout.pInputElementDescs =
      state.input_elements.empty() ? nullptr : state.input_elements.data();

  for (size_t i = 0; i < state.stream_output_entries.size(); i++) {
    state.stream_output_entries[i].SemanticName =
        state.stream_output_semantic_names[i].empty()
            ? nullptr
            : state.stream_output_semantic_names[i].c_str();
  }
  state.desc.StreamOutput.NumEntries =
      UINT(state.stream_output_entries.size());
  state.desc.StreamOutput.pSODeclaration =
      state.stream_output_entries.empty()
          ? nullptr
          : state.stream_output_entries.data();
  state.desc.StreamOutput.NumStrides =
      UINT(state.stream_output_strides.size());
  state.desc.StreamOutput.pBufferStrides =
      state.stream_output_strides.empty()
          ? nullptr
          : state.stream_output_strides.data();

  state.desc.CachedPSO.pCachedBlob =
      state.cached_pso.empty() ? nullptr : state.cached_pso.data();
  state.desc.CachedPSO.CachedBlobSizeInBytes = state.cached_pso.size();
}

void
FixComputeStatePointers(PipelineComputeState &state,
                        const std::vector<PipelineDxilShader> &shaders) {
  state.desc.CS = ShaderBytecodeView(shaders, PipelineShaderStage::Compute);
  state.desc.CachedPSO.pCachedBlob =
      state.cached_pso.empty() ? nullptr : state.cached_pso.data();
  state.desc.CachedPSO.CachedBlobSizeInBytes = state.cached_pso.size();
}

HRESULT
CloneGraphicsState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc,
                   PipelineGraphicsState &state) {
  if (desc.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
      desc.SampleDesc.Count == 0)
    return WARN_E_INVALIDARG(__func__);
  if (desc.NodeMask > 1) {
    WARN("D3D12PipelineState: multi-node graphics PSOs are unsupported");
    return WARN_E_INVALIDARG(__func__);
  }
  if (desc.StreamOutput.NumEntries || desc.StreamOutput.NumStrides) {
    if (!(GetRootSignatureFlags(desc.pRootSignature) &
          D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT)) {
      WARN("D3D12PipelineState: stream output requires root signature support");
      return WARN_E_INVALIDARG(__func__);
    }
  }
  for (UINT i = desc.NumRenderTargets; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
    if (desc.RTVFormats[i] != DXGI_FORMAT_UNKNOWN)
      return WARN_E_INVALIDARG(__func__);
  }
  for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) {
    const auto &rt = desc.BlendState.RenderTarget[i];
    if (rt.BlendEnable && rt.LogicOpEnable)
      return WARN_E_INVALIDARG(__func__);
    if (rt.LogicOpEnable && desc.BlendState.IndependentBlendEnable)
      return WARN_E_INVALIDARG(__func__);
  }
  if (desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED ||
      desc.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH) {
    WARN("D3D12PipelineState: unsupported primitive topology type ",
         uint32_t(desc.PrimitiveTopologyType));
    return WARN_E_INVALIDARG(__func__);
  }
  if (desc.SampleDesc.Quality != 0) {
    WARN("D3D12PipelineState: MSAA quality levels are unsupported");
    return WARN_E_INVALIDARG(__func__);
  }
  if ((desc.HS.BytecodeLength || desc.HS.pShaderBytecode ||
       desc.DS.BytecodeLength || desc.DS.pShaderBytecode) &&
      (!HasBytecode(desc.HS) || !HasBytecode(desc.DS)))
    return WARN_E_INVALIDARG(__func__);
  if (HasBytecode(desc.HS) || HasBytecode(desc.DS)) {
    WARN("D3D12PipelineState: tessellation shaders are unsupported");
    return WARN_E_INVALIDARG(__func__);
  }
  if (!HasBytecode(desc.VS))
    return WARN_E_INVALIDARG(__func__);
  if (desc.InputLayout.NumElements && !desc.InputLayout.pInputElementDescs)
    return WARN_E_INVALIDARG(__func__);

  state = {};
  state.desc = desc;

  state.input_elements.reserve(desc.InputLayout.NumElements);
  state.input_element_semantic_names.reserve(desc.InputLayout.NumElements);
  for (UINT i = 0; i < desc.InputLayout.NumElements; i++) {
    const auto &element = desc.InputLayout.pInputElementDescs[i];
    if (!element.SemanticName)
      return WARN_E_INVALIDARG(__func__);
    state.input_element_semantic_names.emplace_back(element.SemanticName);
    state.input_elements.push_back(element);
  }

  state.stream_output_entries.reserve(desc.StreamOutput.NumEntries);
  state.stream_output_semantic_names.reserve(desc.StreamOutput.NumEntries);
  for (UINT i = 0; i < desc.StreamOutput.NumEntries; i++) {
    const auto &entry = desc.StreamOutput.pSODeclaration[i];
    state.stream_output_semantic_names.emplace_back(
        entry.SemanticName ? entry.SemanticName : "");
    state.stream_output_entries.push_back(entry);
  }

  if (desc.StreamOutput.NumStrides) {
    state.stream_output_strides.assign(
        desc.StreamOutput.pBufferStrides,
        desc.StreamOutput.pBufferStrides + desc.StreamOutput.NumStrides);
  }

  if (!CopyCachedBlob(desc.CachedPSO, state.cached_pso))
    return WARN_E_INVALIDARG(__func__);

  return S_OK;
}

HRESULT
CloneComputeState(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc,
                  PipelineComputeState &state) {
  if (!HasBytecode(desc.CS))
    return WARN_E_INVALIDARG(__func__);

  state = {};
  state.desc = desc;
  if (!CopyCachedBlob(desc.CachedPSO, state.cached_pso))
    return WARN_E_INVALIDARG(__func__);
  return S_OK;
}

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
size_t
AlignStreamOffset(size_t value) {
  const size_t alignment = sizeof(void *);
  return (value + alignment - 1) & ~(alignment - 1);
}

size_t
AlignStreamOffset(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

size_t
PipelineStreamPayloadSize(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
  switch (type) {
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
    return sizeof(ID3D12RootSignature *);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    return sizeof(D3D12_SHADER_BYTECODE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
    return sizeof(D3D12_STREAM_OUTPUT_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
    return sizeof(D3D12_BLEND_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    return sizeof(UINT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
    return sizeof(D3D12_RASTERIZER_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
    return sizeof(D3D12_DEPTH_STENCIL_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
    return sizeof(D3D12_DEPTH_STENCIL_DESC1);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
    return sizeof(D3D12_INPUT_LAYOUT_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
    return sizeof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
    return sizeof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
    return sizeof(D3D12_RT_FORMAT_ARRAY);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
    return sizeof(DXGI_FORMAT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
    return sizeof(DXGI_SAMPLE_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
    return sizeof(UINT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
    return sizeof(D3D12_CACHED_PIPELINE_STATE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
    return sizeof(D3D12_PIPELINE_STATE_FLAGS);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
    return sizeof(D3D12_VIEW_INSTANCING_DESC);
  default:
    return 0;
  }
}

size_t
PipelineStreamPayloadAlignment(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) {
  switch (type) {
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
    return alignof(ID3D12RootSignature *);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
    return alignof(D3D12_SHADER_BYTECODE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
    return alignof(D3D12_STREAM_OUTPUT_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
    return alignof(D3D12_BLEND_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
    return alignof(UINT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
    return alignof(D3D12_RASTERIZER_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
    return alignof(D3D12_DEPTH_STENCIL_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1:
    return alignof(D3D12_DEPTH_STENCIL_DESC1);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
    return alignof(D3D12_INPUT_LAYOUT_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
    return alignof(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
    return alignof(D3D12_PRIMITIVE_TOPOLOGY_TYPE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS:
    return alignof(D3D12_RT_FORMAT_ARRAY);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
    return alignof(DXGI_FORMAT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
    return alignof(DXGI_SAMPLE_DESC);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
    return alignof(UINT);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
    return alignof(D3D12_CACHED_PIPELINE_STATE);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
    return alignof(D3D12_PIPELINE_STATE_FLAGS);
  case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING:
    return alignof(D3D12_VIEW_INSTANCING_DESC);
  default:
    return 0;
  }
}

HRESULT
FailPipelineStreamParse(const char *reason,
                        D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type,
                        size_t offset, HRESULT hr = E_INVALIDARG) {
  if (D3D12PipelineDiagShouldLog())
    INFO("D3D12 diagnostic: pipeline stream parse failed",
         " reason=", reason,
         " type=", uint32_t(type),
         " offset=", offset,
         " hr=0x", std::hex, uint32_t(hr), std::dec);
  return hr;
}

HRESULT
ParsePipelineStateStream(const D3D12_PIPELINE_STATE_STREAM_DESC &stream,
                         D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics,
                         D3D12_COMPUTE_PIPELINE_STATE_DESC &compute,
                         bool &has_compute_shader) {
  if (!stream.pPipelineStateSubobjectStream || !stream.SizeInBytes)
    return FailPipelineStreamParse("empty stream",
                                   D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID,
                                   0);

  graphics = {};
  graphics.SampleMask = UINT_MAX;
  graphics.SampleDesc.Count = 1;
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  compute = {};
  has_compute_shader = false;
  bool seen_subobjects[D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING + 1] = {};

  const auto *bytes =
      static_cast<const uint8_t *>(stream.pPipelineStateSubobjectStream);
  size_t offset = 0;
  while (offset < stream.SizeInBytes) {
    if (stream.SizeInBytes - offset < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
      return FailPipelineStreamParse("truncated type",
                                     D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MAX_VALID,
                                     offset);

    auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(
        bytes + offset);
    const size_t type_offset = offset;
    offset += sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE);

    const size_t payload_size = PipelineStreamPayloadSize(type);
    const size_t payload_alignment = PipelineStreamPayloadAlignment(type);
    if (!payload_size || !payload_alignment)
      return FailPipelineStreamParse("unknown type", type, type_offset);
    offset = AlignStreamOffset(offset, payload_alignment);
    if (stream.SizeInBytes - offset < payload_size)
      return FailPipelineStreamParse("truncated payload", type, type_offset);
    const auto type_index = static_cast<uint32_t>(type);
    if (type_index >= std::size(seen_subobjects) || seen_subobjects[type_index])
      return FailPipelineStreamParse("duplicate subobject", type, type_offset);
    seen_subobjects[type_index] = true;

    const void *payload = bytes + offset;
    switch (type) {
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE:
      graphics.pRootSignature =
          *static_cast<ID3D12RootSignature *const *>(payload);
      compute.pRootSignature = graphics.pRootSignature;
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS:
      graphics.VS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS:
      graphics.PS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS:
      graphics.DS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS:
      graphics.HS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS:
      graphics.GS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS:
      compute.CS = *static_cast<const D3D12_SHADER_BYTECODE *>(payload);
      has_compute_shader |= HasBytecode(compute.CS);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT:
      graphics.StreamOutput =
          *static_cast<const D3D12_STREAM_OUTPUT_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND:
      graphics.BlendState = *static_cast<const D3D12_BLEND_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK:
      graphics.SampleMask = *static_cast<const UINT *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER:
      graphics.RasterizerState =
          *static_cast<const D3D12_RASTERIZER_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL:
      graphics.DepthStencilState =
          *static_cast<const D3D12_DEPTH_STENCIL_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1: {
      const auto &depth_stencil =
          *static_cast<const D3D12_DEPTH_STENCIL_DESC1 *>(payload);
      graphics.DepthStencilState.DepthEnable = depth_stencil.DepthEnable;
      graphics.DepthStencilState.DepthWriteMask =
          depth_stencil.DepthWriteMask;
      graphics.DepthStencilState.DepthFunc = depth_stencil.DepthFunc;
      graphics.DepthStencilState.StencilEnable = depth_stencil.StencilEnable;
      graphics.DepthStencilState.StencilReadMask =
          depth_stencil.StencilReadMask;
      graphics.DepthStencilState.StencilWriteMask =
          depth_stencil.StencilWriteMask;
      graphics.DepthStencilState.FrontFace = depth_stencil.FrontFace;
      graphics.DepthStencilState.BackFace = depth_stencil.BackFace;
      if (depth_stencil.DepthBoundsTestEnable) {
        // TODO(d3d12): lower depth bounds once dynamic min/max depth-bounds
        // state is represented in the Metal command stream.
        WARN("D3D12PipelineState: depth bounds in PSO stream is unsupported");
        return E_NOTIMPL;
      }
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT:
      graphics.InputLayout =
          *static_cast<const D3D12_INPUT_LAYOUT_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_IB_STRIP_CUT_VALUE:
      graphics.IBStripCutValue =
          *static_cast<const D3D12_INDEX_BUFFER_STRIP_CUT_VALUE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY:
      graphics.PrimitiveTopologyType =
          *static_cast<const D3D12_PRIMITIVE_TOPOLOGY_TYPE *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS: {
      const auto &formats = *static_cast<const D3D12_RT_FORMAT_ARRAY *>(payload);
      if (formats.NumRenderTargets > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT)
        return WARN_E_INVALIDARG(__func__);
      graphics.NumRenderTargets = formats.NumRenderTargets;
      for (UINT i = 0; i < formats.NumRenderTargets; i++)
      graphics.RTVFormats[i] = formats.RTFormats[i];
      break;
    }
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT:
      graphics.DSVFormat = *static_cast<const DXGI_FORMAT *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC:
      graphics.SampleDesc = *static_cast<const DXGI_SAMPLE_DESC *>(payload);
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_NODE_MASK:
      graphics.NodeMask = *static_cast<const UINT *>(payload);
      compute.NodeMask = graphics.NodeMask;
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO:
      graphics.CachedPSO =
          *static_cast<const D3D12_CACHED_PIPELINE_STATE *>(payload);
      compute.CachedPSO = graphics.CachedPSO;
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS:
      graphics.Flags = *static_cast<const D3D12_PIPELINE_STATE_FLAGS *>(payload);
      compute.Flags = graphics.Flags;
      break;
    case D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VIEW_INSTANCING: {
      const auto &view_instancing =
          *static_cast<const D3D12_VIEW_INSTANCING_DESC *>(payload);
      if (view_instancing.ViewInstanceCount)
        return FailPipelineStreamParse("view instancing unsupported", type,
                                       type_offset, E_NOTIMPL);
      break;
    }
    default:
      return FailPipelineStreamParse("unknown type", type, type_offset);
    }

    offset = AlignStreamOffset(offset + payload_size);
  }

  if (has_compute_shader &&
      (HasBytecode(graphics.VS) || HasBytecode(graphics.DS) ||
       HasBytecode(graphics.HS) || HasBytecode(graphics.GS)))
    return FailPipelineStreamParse("compute stream has incompatible shader",
                                   D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS,
                                   stream.SizeInBytes);

  return S_OK;
}
#endif

#ifdef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
#ifdef __ID3D12PipelineLibrary1_INTERFACE_DEFINED__
using PipelineLibraryBase = ID3D12PipelineLibrary1;
#else
using PipelineLibraryBase = ID3D12PipelineLibrary;
#endif

using PipelineLibraryKey = std::string;

PipelineLibraryKey
PipelineLibraryNameKey(const WCHAR *name) {
  return name ? str::fromws(name) : PipelineLibraryKey();
}

class PipelineLibraryImpl final
    : public ComObjectWithInitialRef<PipelineLibraryBase> {
public:
  PipelineLibraryImpl(IMTLD3D12Device *device) : device_(device) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;

    *object = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12PipelineLibrary)) {
      *object = ref(static_cast<ID3D12PipelineLibrary *>(
          static_cast<PipelineLibraryBase *>(this)));
      return S_OK;
    }
#ifdef __ID3D12PipelineLibrary1_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12PipelineLibrary1)) {
      *object = ref(static_cast<ID3D12PipelineLibrary1 *>(this));
      return S_OK;
    }
#endif

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineLibrary), riid))
      WARN("D3D12PipelineLibrary: unknown interface query ",
           str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE StorePipeline(const WCHAR *name,
                                          ID3D12PipelineState *pipeline) override {
    if (!name || !pipeline)
      return WARN_E_INVALIDARG(__func__);

    auto *state = dynamic_cast<PipelineState *>(pipeline);
    if (!state)
      return WARN_E_INVALIDARG(__func__);

    std::lock_guard lock(mutex_);
    pipelines_[PipelineLibraryNameKey(name)] = {
        .type = state->GetType(),
        .pipeline = pipeline,
    };
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  LoadGraphicsPipeline(const WCHAR *name,
                       const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                       REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;
    if (!name)
      return WARN_E_INVALIDARG(__func__);

    auto key = PipelineLibraryNameKey(name);
    {
      std::lock_guard lock(mutex_);
      auto entry = pipelines_.find(key);
      if (entry != pipelines_.end()) {
        if (entry->second.type != PipelineStateType::Graphics)
          return WARN_E_INVALIDARG(__func__);
        return entry->second.pipeline->QueryInterface(riid, pipeline_state);
      }
    }

    HRESULT status = S_OK;
    auto pipeline = CreateGraphicsPipelineState(device_.ptr(), desc, &status);
    if (!pipeline)
      return status;

    std::lock_guard lock(mutex_);
    pipelines_[std::move(key)] = {
        .type = PipelineStateType::Graphics,
        .pipeline = pipeline,
    };
    return pipeline->QueryInterface(riid, pipeline_state);
  }

  HRESULT STDMETHODCALLTYPE
  LoadComputePipeline(const WCHAR *name,
                      const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                      REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;
    if (!name)
      return WARN_E_INVALIDARG(__func__);

    auto key = PipelineLibraryNameKey(name);
    {
      std::lock_guard lock(mutex_);
      auto entry = pipelines_.find(key);
      if (entry != pipelines_.end()) {
        if (entry->second.type != PipelineStateType::Compute)
          return WARN_E_INVALIDARG(__func__);
        return entry->second.pipeline->QueryInterface(riid, pipeline_state);
      }
    }

    HRESULT status = S_OK;
    auto pipeline = CreateComputePipelineState(device_.ptr(), desc, &status);
    if (!pipeline)
      return status;

    std::lock_guard lock(mutex_);
    pipelines_[std::move(key)] = {
        .type = PipelineStateType::Compute,
        .pipeline = pipeline,
    };
    return pipeline->QueryInterface(riid, pipeline_state);
  }

  SIZE_T STDMETHODCALLTYPE GetSerializedSize() override {
    return 0;
  }

  HRESULT STDMETHODCALLTYPE Serialize(void *data,
                                      SIZE_T data_size_in_bytes) override {
    return data || data_size_in_bytes == 0 ? S_OK : E_INVALIDARG;
  }

#ifdef __ID3D12PipelineLibrary1_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE LoadPipeline(
      const WCHAR *name, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
      REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;
    if (!name)
      return WARN_E_INVALIDARG(__func__);

    auto key = PipelineLibraryNameKey(name);
    {
      std::lock_guard lock(mutex_);
      auto entry = pipelines_.find(key);
      if (entry != pipelines_.end())
        return entry->second.pipeline->QueryInterface(riid, pipeline_state);
    }

    HRESULT status = S_OK;
    auto pipeline = CreatePipelineStateFromStream(device_.ptr(), desc, &status);
    if (!pipeline)
      return status;

    auto *state = dynamic_cast<PipelineState *>(pipeline.ptr());
    std::lock_guard lock(mutex_);
    pipelines_[std::move(key)] = {
        .type = state ? state->GetType() : PipelineStateType::Graphics,
        .pipeline = pipeline,
    };
    return pipeline->QueryInterface(riid, pipeline_state);
  }
#endif

private:
  struct Entry {
    PipelineStateType type = PipelineStateType::Graphics;
    Com<ID3D12PipelineState> pipeline;
  };

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  std::string name_;
  std::mutex mutex_;
  std::unordered_map<PipelineLibraryKey, Entry> pipelines_;
};
#endif

class PipelineStateImpl final : public ComObjectWithInitialRef<ID3D12PipelineState>,
                                public PipelineState {
public:
  PipelineStateImpl(IMTLD3D12Device *device, PipelineStateType type,
                    ID3D12RootSignature *root_signature,
                    std::vector<PipelineDxilShader> &&shaders,
                    std::vector<PipelineSignatureLink> &&signature_links,
                    PipelineGraphicsState &&graphics_state,
                    PipelineComputeState &&compute_state)
      : device_(device), type_(type),
        root_signature_(dynamic_cast<RootSignature *>(root_signature)),
        shaders_(std::move(shaders)),
        signature_links_(std::move(signature_links)),
        graphics_state_(std::move(graphics_state)),
        compute_state_(std::move(compute_state)) {
    FixGraphicsStatePointers(graphics_state_, shaders_);
    FixComputeStatePointers(compute_state_, shaders_);
    shader_cache_key_ = BuildShaderCacheKey(type_, shaders_, graphics_state_,
                                            compute_state_,
                                            GetRootSignature());
    cached_shader_blob_ =
        BuildCachedShaderBlob(type_, graphics_state_, compute_state_,
                              shader_cache_key_);
  }

  ~PipelineStateImpl() {
    DestroyPipelineShaders(shaders_);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override {
    if (!object)
      return E_POINTER;

    *object = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12PipelineState)) {
      *object = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12PipelineState), riid))
      WARN("D3D12PipelineState: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                   const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE GetCachedBlob(ID3DBlob **blob) override {
    InitReturnPtr(blob);
    if (!blob)
      return E_POINTER;

    *blob = CreateBlob(cached_shader_blob_).takeOwnership();
    return S_OK;
  }

  PipelineStateType GetType() const override {
    return type_;
  }

  ID3D12RootSignature *GetRootSignature() const override {
    return root_signature_.ptr()
               ? dynamic_cast<ID3D12RootSignature *>(root_signature_.ptr())
               : nullptr;
  }

  const std::vector<PipelineDxilShader> &GetDxilShaders() const override {
    return shaders_;
  }

  const std::vector<PipelineSignatureLink> &GetSignatureLinks() const override {
    return signature_links_;
  }

  const PipelineGraphicsState *GetGraphicsState() const override {
    return type_ == PipelineStateType::Graphics ? &graphics_state_ : nullptr;
  }

  const PipelineComputeState *GetComputeState() const override {
    return type_ == PipelineStateType::Compute ? &compute_state_ : nullptr;
  }

  const std::string &GetShaderCacheKey() const override {
    return shader_cache_key_;
  }

  const PipelineMetalGraphicsState *GetMetalGraphicsState() override {
    if (type_ != PipelineStateType::Graphics)
      return nullptr;

    std::lock_guard lock(metal_mutex_);
    if (!metal_graphics_ready_) {
      metal_graphics_ready_ = true;
      if (!CreateMetalGraphicsPipeline(device_.ptr(), shaders_, graphics_state_,
                                       shader_cache_key_, metal_graphics_))
        metal_graphics_ = {};
    }

    return metal_graphics_.pso ? &metal_graphics_ : nullptr;
  }

  const PipelineMetalComputeState *GetMetalComputeState() override {
    if (type_ != PipelineStateType::Compute)
      return nullptr;

    std::lock_guard lock(metal_mutex_);
    if (!metal_compute_ready_) {
      metal_compute_ready_ = true;
      if (!CreateMetalComputePipeline(device_.ptr(), shaders_,
                                      shader_cache_key_, metal_compute_))
        metal_compute_ = {};
    }

    return metal_compute_.pso ? &metal_compute_ : nullptr;
  }

private:
  Com<IMTLD3D12Device> device_;
  PipelineStateType type_;
  Com<RootSignature, false> root_signature_;
  std::vector<PipelineDxilShader> shaders_;
  std::vector<PipelineSignatureLink> signature_links_;
  PipelineGraphicsState graphics_state_;
  PipelineComputeState compute_state_;
  std::string shader_cache_key_;
  std::vector<uint8_t> cached_shader_blob_;
  std::mutex metal_mutex_;
  bool metal_graphics_ready_ = false;
  bool metal_compute_ready_ = false;
  PipelineMetalGraphicsState metal_graphics_;
  PipelineMetalComputeState metal_compute_;
  ComPrivateData private_data_;
  std::string name_;
};

Com<ID3D12PipelineState>
CreatePipelineStateObject(IMTLD3D12Device *device, PipelineStateType type,
                          ID3D12RootSignature *root_signature,
                          std::vector<PipelineDxilShader> &&shaders,
                          PipelineGraphicsState &&graphics_state,
                          PipelineComputeState &&compute_state) {
  auto signature_links = BuildSignatureLinks(shaders);
  auto resolved_root_signature =
      ResolveRootSignature(device, root_signature, shaders);
  const auto shader_cache_key =
      BuildShaderCacheKey(type, shaders, graphics_state, compute_state,
                          resolved_root_signature.ptr());
  DebugLogSignatureLinks(shader_cache_key, signature_links);
  return Com<ID3D12PipelineState>::transfer(
      new PipelineStateImpl(device, type, resolved_root_signature.ptr(),
                            std::move(shaders), std::move(signature_links),
                            std::move(graphics_state),
                            std::move(compute_state)));
}

void
StoreStatus(HRESULT *status, HRESULT value) {
  if (status)
    *status = value;
}

void
LogGraphicsPipelineDesc(const char *where,
                        const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
  INFO("D3D12 diagnostic: graphics pipeline create",
       " where=", where,
       " hasVS=", HasBytecode(desc.VS),
       " hasPS=", HasBytecode(desc.PS),
       " hasGS=", HasBytecode(desc.GS),
       " hasHS=", HasBytecode(desc.HS),
       " hasDS=", HasBytecode(desc.DS),
       " topologyType=", uint32_t(desc.PrimitiveTopologyType),
       " numRT=", uint32_t(desc.NumRenderTargets),
       " sampleCount=", uint32_t(desc.SampleDesc.Count),
       " sampleQuality=", uint32_t(desc.SampleDesc.Quality),
       " streamOutputEntries=", uint32_t(desc.StreamOutput.NumEntries),
       " streamOutputStrides=", uint32_t(desc.StreamOutput.NumStrides),
       " cachedPsoBytes=", uint64_t(desc.CachedPSO.CachedBlobSizeInBytes));
}

} // namespace

Com<ID3D12PipelineState>
CreateGraphicsPipelineState(IMTLD3D12Device *device,
                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                            HRESULT *status) {
  if (!device || !desc) {
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }
  LogGraphicsPipelineDesc("CreateGraphicsPipelineState", *desc);

  PipelineGraphicsState graphics_state = {};
  HRESULT hr = CloneGraphicsState(*desc, graphics_state);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }

  std::vector<PipelineDxilShader> shaders;
  shaders.reserve(5);

  hr = AppendPipelineShader(PipelineShaderStage::Vertex, device, desc->VS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendPipelineShader(PipelineShaderStage::Pixel, device, desc->PS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendPipelineShader(PipelineShaderStage::Geometry, device, desc->GS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendPipelineShader(PipelineShaderStage::Hull, device, desc->HS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendPipelineShader(PipelineShaderStage::Domain, device, desc->DS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  if (shaders.empty()) {
    DestroyPipelineShaders(shaders);
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  PipelineComputeState compute_state = {};
  auto pso = CreatePipelineStateObject(
      device, PipelineStateType::Graphics, desc->pRootSignature,
      std::move(shaders), std::move(graphics_state),
      std::move(compute_state));
  StoreStatus(status, pso ? S_OK : E_OUTOFMEMORY);
  return pso;
}

Com<ID3D12PipelineState>
CreateComputePipelineState(IMTLD3D12Device *device,
                           const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                           HRESULT *status) {
  if (!device || !desc) {
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  PipelineComputeState compute_state = {};
  HRESULT hr = CloneComputeState(*desc, compute_state);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }

  std::vector<PipelineDxilShader> shaders;
  shaders.reserve(1);
  hr = AppendPipelineShader(PipelineShaderStage::Compute, device, desc->CS, shaders);
  if (FAILED(hr)) {
    DestroyPipelineShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  if (shaders.empty()) {
    DestroyPipelineShaders(shaders);
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  PipelineGraphicsState graphics_state = {};
  auto pso = CreatePipelineStateObject(
      device, PipelineStateType::Compute, desc->pRootSignature,
      std::move(shaders), std::move(graphics_state),
      std::move(compute_state));
  StoreStatus(status, pso ? S_OK : E_OUTOFMEMORY);
  return pso;
}

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
Com<ID3D12PipelineState>
CreatePipelineStateFromStream(IMTLD3D12Device *device,
                              const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                              HRESULT *status) {
  if (!device || !desc) {
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics = {};
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
  bool has_compute_shader = false;
  HRESULT hr = ParsePipelineStateStream(*desc, graphics, compute,
                                        has_compute_shader);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }

  if (!has_compute_shader)
    LogGraphicsPipelineDesc("CreatePipelineStateFromStream", graphics);

  if (has_compute_shader)
    return CreateComputePipelineState(device, &compute, status);
  return CreateGraphicsPipelineState(device, &graphics, status);
}
#endif

#ifdef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
Com<ID3D12PipelineLibrary>
CreatePipelineLibrary(IMTLD3D12Device *device) {
  return Com<ID3D12PipelineLibrary>::transfer(
      new PipelineLibraryImpl(device));
}
#endif

} // namespace dxmt::d3d12
