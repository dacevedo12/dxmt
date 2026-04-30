#include "d3d12_pipeline.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <span>
#include <utility>

namespace dxmt::d3d12 {
namespace {

constexpr uint32_t kShaderKindPixel = 0;
constexpr uint32_t kShaderKindVertex = 1;
constexpr uint32_t kShaderKindGeometry = 2;
constexpr uint32_t kShaderKindHull = 3;
constexpr uint32_t kShaderKindDomain = 4;
constexpr uint32_t kShaderKindCompute = 5;

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

  return S_OK;
}

HRESULT
AppendDxilShader(PipelineShaderStage stage,
                 const D3D12_SHADER_BYTECODE &bytecode,
                 std::vector<PipelineDxilShader> &shaders) {
  PipelineDxilShader shader = {};
  const auto hr = ParseDxilShader(stage, bytecode, shader);
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
                    const std::vector<PipelineDxilShader> &shaders) {
  Sha1HashState hash;
  HashString(hash, type == PipelineStateType::Graphics
                       ? "dxmt-d3d12-graphics-shader-cache-v1"
                       : "dxmt-d3d12-compute-shader-cache-v1");
  HashDxilShaders(hash, shaders);
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
    shader_cache_key_ = BuildShaderCacheKey(type_, shaders_);
    cached_shader_blob_ =
        BuildCachedShaderBlob(type_, graphics_state_, compute_state_,
                              shader_cache_key_);
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

  hr = AppendDxilShader(PipelineShaderStage::Vertex, desc->VS, shaders);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendDxilShader(PipelineShaderStage::Pixel, desc->PS, shaders);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendDxilShader(PipelineShaderStage::Geometry, desc->GS, shaders);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendDxilShader(PipelineShaderStage::Hull, desc->HS, shaders);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }
  hr = AppendDxilShader(PipelineShaderStage::Domain, desc->DS, shaders);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }
  if (shaders.empty()) {
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
  hr = AppendDxilShader(PipelineShaderStage::Compute, desc->CS, shaders);
  if (FAILED(hr)) {
    StoreStatus(status, hr);
    return nullptr;
  }
  if (shaders.empty()) {
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

} // namespace dxmt::d3d12
