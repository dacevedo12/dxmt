#include "d3d12_pipeline.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string_view>
#include <span>
#include <unordered_map>
#include <utility>

namespace dxmt::d3d12 {
namespace {

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

HRESULT
ParseDxilShader(PipelineShaderStage stage,
                IMTLD3D12Device *device,
                const D3D12_SHADER_BYTECODE &bytecode,
                PipelineDxilShader &shader) {
  if (!bytecode.pShaderBytecode && bytecode.BytecodeLength)
    return E_INVALIDARG;
  if (!HasBytecode(bytecode))
    return S_FALSE;
  if (!IsDxilContainer(bytecode))
    return E_NOTIMPL;

  shader = {};
  shader.stage = stage;
  shader.bytecode.resize(bytecode.BytecodeLength);
  std::memcpy(shader.bytecode.data(), bytecode.pShaderBytecode,
              bytecode.BytecodeLength);

  const auto status = shader.parser.parse(shader.bytecode.data(),
                                          shader.bytecode.size());
  if (status != dxil::ParseStatus::Ok) {
    WARN("D3D12PipelineState: failed to parse ", ShaderStageName(stage),
         " DXIL bytecode: ", dxil::StatusName(status));
    return E_INVALIDARG;
  }

  const auto &program = shader.parser.dxilProgram();
  const auto expected_kind = ExpectedShaderKind(stage);
  if (program && expected_kind != UINT32_MAX &&
      program->shader_kind() != expected_kind) {
    WARN("D3D12PipelineState: ", ShaderStageName(stage),
         " bytecode contains ", dxil::PsvShaderKindName(program->shader_kind()),
         " shader");
    return E_INVALIDARG;
  }

  sm50_error_t error = nullptr;
  if (DXILInitialize(shader.bytecode.data(), shader.bytecode.size(),
                     &shader.shader, &shader.reflection, &error)) {
    WARN("D3D12PipelineState: failed to initialize ", ShaderStageName(stage),
         " DXIL shader: ", SM50GetErrorMessageString(error));
    SM50FreeError(error);
    return E_INVALIDARG;
  }

  const auto argument_count =
      shader.reflection.NumConstantBuffers + shader.reflection.NumArguments;
  shader.argument_info.resize(argument_count);
  if (argument_count) {
    DXILGetArgumentsInfo(
        shader.shader,
        shader.argument_info.empty() ? nullptr : shader.argument_info.data(),
        shader.argument_info.size() <= shader.reflection.NumConstantBuffers
            ? nullptr
            : shader.argument_info.data() + shader.reflection.NumConstantBuffers);
  }

  return S_OK;
}

HRESULT
AppendDxilShader(PipelineShaderStage stage,
                 IMTLD3D12Device *device,
                 const D3D12_SHADER_BYTECODE &bytecode,
                 std::vector<PipelineDxilShader> &shaders) {
  PipelineDxilShader shader = {};
  const auto hr = ParseDxilShader(stage, device, bytecode, shader);
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
HashDxilShaders(Sha1HashState &hash,
                const std::vector<PipelineDxilShader> &shaders) {
  const auto count = uint32_t(shaders.size());
  HashValue(hash, count);
  for (const auto &shader : shaders) {
    HashValue(hash, shader.stage);
    HashVector(hash, shader.bytecode);
  }
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
  HashDxilShaders(hash, shaders);
  if (type == PipelineStateType::Graphics) {
    HashVector(hash, graphics_state.input_elements);
    HashVector(hash, graphics_state.stream_output_entries);
    HashVector(hash, graphics_state.stream_output_strides);
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
DestroyDxilShaders(std::vector<PipelineDxilShader> &shaders) {
  for (auto &shader : shaders) {
    if (shader.shader) {
      DXILDestroy(shader.shader);
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
  if (DXILCompile(shader.shader, args, function_name, &bitcode_handle, &error)) {
    WARN("D3D12PipelineState: failed to compile ", ShaderStageName(shader.stage),
         " DXIL shader: ", SM50GetErrorMessageString(error));
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
                   std::vector<SM50_IA_INPUT_ELEMENT> &elements,
                   uint32_t &slot_mask) {
  elements.clear();
  slot_mask = 0;
  elements.reserve(state.input_elements.size());

  for (uint32_t i = 0; i < state.input_elements.size(); i++) {
    const auto &input = state.input_elements[i];
    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), input.Format,
                                  format)) ||
        format.AttributeFormat == WMTAttributeFormatInvalid) {
      WARN("D3D12PipelineState: unsupported input layout format ",
           uint32_t(input.Format));
      return false;
    }

    elements.push_back({
        .reg = i,
        .slot = input.InputSlot,
        .aligned_byte_offset = input.AlignedByteOffset,
        .format = uint32_t(format.AttributeFormat),
        .step_function = input.InputSlotClass ==
                         D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,
        .step_rate = input.InstanceDataStepRate,
    });
    if (input.InputSlot < 32)
      slot_mask |= 1u << input.InputSlot;
  }

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

void
ApplyBlendState(WMTRenderPipelineInfo &info,
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
  if (!vs)
    return false;

  SM50_SHADER_COMMON_DATA common = {};
  common.type = SM50_SHADER_COMMON;
  common.metal_version = GetMetalVersion(device);
  common.flags = GetShaderFlags();
  common.next = nullptr;

  std::vector<SM50_IA_INPUT_ELEMENT> input_elements;
  uint32_t slot_mask = 0;
  if (!BuildInputElements(device, state, input_elements, slot_mask))
    return false;

  SM50_SHADER_IA_INPUT_LAYOUT_DATA ia_layout = {};
  ia_layout.type = SM50_SHADER_IA_INPUT_LAYOUT;
  ia_layout.next = &common;
  ia_layout.index_buffer_format = SM50_INDEX_BUFFER_FORMAT_NONE;
  ia_layout.slot_mask = slot_mask;
  ia_layout.num_elements = uint32_t(input_elements.size());
  ia_layout.elements = input_elements.empty() ? nullptr : input_elements.data();

  const auto vs_name = BuildFunctionName("vs", shader_cache_key);
  if (!CompileMetalFunction(device, *vs, vs_name.c_str(),
                            reinterpret_cast<SM50_SHADER_COMPILATION_ARGUMENT_DATA *>(&ia_layout),
                            out.vertex))
    return false;

  if (ps) {
    uint32_t unorm_output_reg_mask = 0;
    for (UINT i = 0; i < state.desc.NumRenderTargets; i++) {
      const auto format = GetRenderTargetPixelFormat(device->GetMTLDevice(),
                                                     state.desc.RTVFormats[i]);
      if (IsUnorm8RenderTargetFormat(format))
        unorm_output_reg_mask |= 1u << i;
    }

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
      info.colors[i].pixel_format = GetRenderTargetPixelFormat(
          device->GetMTLDevice(), state.desc.RTVFormats[i]);
  }
  ApplyBlendState(info, state.desc.BlendState, state.desc.NumRenderTargets);

  if (state.desc.DSVFormat != DXGI_FORMAT_UNKNOWN) {
    MTL_DXGI_FORMAT_DESC format = {};
    if (SUCCEEDED(MTLQueryDXGIFormat(device->GetMTLDevice(),
                                     state.desc.DSVFormat, format))) {
      info.depth_pixel_format = format.PixelFormat;
      if (DepthStencilPlanarFlags(format.PixelFormat) & 2)
        info.stencil_pixel_format = format.PixelFormat;
    }
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
    return E_INVALIDARG;
  if ((desc.HS.BytecodeLength || desc.HS.pShaderBytecode ||
       desc.DS.BytecodeLength || desc.DS.pShaderBytecode) &&
      (!HasBytecode(desc.HS) || !HasBytecode(desc.DS)))
    return E_INVALIDARG;
  if (!HasBytecode(desc.VS))
    return E_INVALIDARG;
  if (desc.InputLayout.NumElements && !desc.InputLayout.pInputElementDescs)
    return E_INVALIDARG;
  if (desc.StreamOutput.NumEntries && !desc.StreamOutput.pSODeclaration)
    return E_INVALIDARG;
  if (desc.StreamOutput.NumStrides && !desc.StreamOutput.pBufferStrides)
    return E_INVALIDARG;

  state = {};
  state.desc = desc;

  state.input_elements.reserve(desc.InputLayout.NumElements);
  state.input_element_semantic_names.reserve(desc.InputLayout.NumElements);
  for (UINT i = 0; i < desc.InputLayout.NumElements; i++) {
    const auto &element = desc.InputLayout.pInputElementDescs[i];
    if (!element.SemanticName)
      return E_INVALIDARG;
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
    return E_INVALIDARG;

  return S_OK;
}

HRESULT
CloneComputeState(const D3D12_COMPUTE_PIPELINE_STATE_DESC &desc,
                  PipelineComputeState &state) {
  if (!HasBytecode(desc.CS))
    return E_INVALIDARG;

  state = {};
  state.desc = desc;
  if (!CopyCachedBlob(desc.CachedPSO, state.cached_pso))
    return E_INVALIDARG;
  return S_OK;
}

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
size_t
AlignStreamOffset(size_t value) {
  const size_t alignment = sizeof(void *);
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

HRESULT
ParsePipelineStateStream(const D3D12_PIPELINE_STATE_STREAM_DESC &stream,
                         D3D12_GRAPHICS_PIPELINE_STATE_DESC &graphics,
                         D3D12_COMPUTE_PIPELINE_STATE_DESC &compute,
                         bool &has_compute_shader) {
  if (!stream.pPipelineStateSubobjectStream || !stream.SizeInBytes)
    return E_INVALIDARG;

  graphics = {};
  graphics.SampleMask = UINT_MAX;
  graphics.SampleDesc.Count = 1;
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  compute = {};
  has_compute_shader = false;

  const auto *bytes =
      static_cast<const uint8_t *>(stream.pPipelineStateSubobjectStream);
  size_t offset = 0;
  while (offset < stream.SizeInBytes) {
    if (stream.SizeInBytes - offset < sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE))
      return E_INVALIDARG;

    auto type = *reinterpret_cast<const D3D12_PIPELINE_STATE_SUBOBJECT_TYPE *>(
        bytes + offset);
    offset += sizeof(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE);
    offset = AlignStreamOffset(offset);

    const size_t payload_size = PipelineStreamPayloadSize(type);
    if (!payload_size || stream.SizeInBytes - offset < payload_size)
      return E_INVALIDARG;

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
      has_compute_shader = HasBytecode(compute.CS);
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
      if (depth_stencil.DepthBoundsTestEnable)
        WARN("D3D12PipelineState: depth bounds in PSO stream is ignored");
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
        return E_INVALIDARG;
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
        return E_NOTIMPL;
      break;
    }
    default:
      return E_INVALIDARG;
    }

    offset = AlignStreamOffset(offset + payload_size);
  }

  if (has_compute_shader &&
      (HasBytecode(graphics.VS) || HasBytecode(graphics.PS) ||
       HasBytecode(graphics.DS) || HasBytecode(graphics.HS) ||
       HasBytecode(graphics.GS)))
    return E_INVALIDARG;

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
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE StorePipeline(const WCHAR *name,
                                          ID3D12PipelineState *pipeline) override {
    if (!name || !pipeline)
      return E_INVALIDARG;

    auto *state = dynamic_cast<PipelineState *>(pipeline);
    if (!state)
      return E_INVALIDARG;

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
      return E_INVALIDARG;

    auto key = PipelineLibraryNameKey(name);
    {
      std::lock_guard lock(mutex_);
      auto entry = pipelines_.find(key);
      if (entry != pipelines_.end()) {
        if (entry->second.type != PipelineStateType::Graphics)
          return E_INVALIDARG;
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
      return E_INVALIDARG;

    auto key = PipelineLibraryNameKey(name);
    {
      std::lock_guard lock(mutex_);
      auto entry = pipelines_.find(key);
      if (entry != pipelines_.end()) {
        if (entry->second.type != PipelineStateType::Compute)
          return E_INVALIDARG;
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
      return E_INVALIDARG;

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
      : device_(device), type_(type), root_signature_(root_signature),
        shaders_(std::move(shaders)),
        signature_links_(std::move(signature_links)),
        graphics_state_(std::move(graphics_state)),
        compute_state_(std::move(compute_state)) {
    FixGraphicsStatePointers(graphics_state_, shaders_);
    FixComputeStatePointers(compute_state_, shaders_);
    shader_cache_key_ = BuildShaderCacheKey(type_, shaders_, graphics_state_,
                                            compute_state_,
                                            root_signature_.ptr());
    cached_shader_blob_ =
        BuildCachedShaderBlob(type_, graphics_state_, compute_state_,
                              shader_cache_key_);
  }

  ~PipelineStateImpl() {
    DestroyDxilShaders(shaders_);
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
    return S_OK;
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
    return root_signature_.ptr();
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
  Com<ID3D12RootSignature> root_signature_;
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

} // namespace

Com<ID3D12PipelineState>
CreateGraphicsPipelineState(IMTLD3D12Device *device,
                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                            HRESULT *status) {
  if (!device || !desc) {
    StoreStatus(status, E_INVALIDARG);
    return nullptr;
  }

  PipelineGraphicsState graphics_state = {};
  HRESULT hr = CloneGraphicsState(*desc, graphics_state);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }

  std::vector<PipelineDxilShader> shaders;
  shaders.reserve(5);

  hr = AppendDxilShader(PipelineShaderStage::Vertex, device, desc->VS, shaders);
  if (FAILED(hr)) {
    DestroyDxilShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendDxilShader(PipelineShaderStage::Pixel, device, desc->PS, shaders);
  if (FAILED(hr)) {
    DestroyDxilShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendDxilShader(PipelineShaderStage::Geometry, device, desc->GS, shaders);
  if (FAILED(hr)) {
    DestroyDxilShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendDxilShader(PipelineShaderStage::Hull, device, desc->HS, shaders);
  if (FAILED(hr)) {
    DestroyDxilShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendDxilShader(PipelineShaderStage::Domain, device, desc->DS, shaders);
  if (FAILED(hr)) {
    DestroyDxilShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  if (shaders.empty()) {
    DestroyDxilShaders(shaders);
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
  hr = AppendDxilShader(PipelineShaderStage::Compute, device, desc->CS, shaders);
  if (FAILED(hr)) {
    DestroyDxilShaders(shaders);
    StoreStatus(status, hr);
    return nullptr;
  }
  if (shaders.empty()) {
    DestroyDxilShaders(shaders);
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
