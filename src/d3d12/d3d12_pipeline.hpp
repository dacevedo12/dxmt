#pragma once

#include "d3d12_root_signature.hpp"
#include "Metal.hpp"
#include "DXILParser/DXILParser.hpp"
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <vector>

namespace dxmt::d3d12 {

enum class PipelineStateType {
  Graphics,
  Compute,
};

enum class PipelineShaderStage {
  Vertex,
  Pixel,
  Geometry,
  Hull,
  Domain,
  Compute,
};

struct PipelineDxilShader {
  PipelineShaderStage stage = PipelineShaderStage::Vertex;
  std::vector<uint8_t> bytecode;
  dxil::Parser parser;

  const dxil::DxilTranslationInfo *translation() const {
    const auto &info = parser.dxilTranslation();
    return info ? &*info : nullptr;
  }
};

struct PipelineSignatureLink {
  uint32_t producer_shader_index = 0;
  uint32_t producer_signature_index = 0;
  uint32_t consumer_shader_index = 0;
  uint32_t consumer_signature_index = 0;
  std::string semantic_key;
  uint8_t producer_component_mask = 0;
  uint8_t consumer_component_mask = 0;
};

struct PipelineGraphicsState {
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
  std::vector<D3D12_INPUT_ELEMENT_DESC> input_elements;
  std::vector<std::string> input_element_semantic_names;
  std::vector<D3D12_SO_DECLARATION_ENTRY> stream_output_entries;
  std::vector<std::string> stream_output_semantic_names;
  std::vector<UINT> stream_output_strides;
  std::vector<uint8_t> cached_pso;
};

struct PipelineComputeState {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
  std::vector<uint8_t> cached_pso;
};

class PipelineState {
public:
  virtual ~PipelineState() = default;

  virtual PipelineStateType GetType() const = 0;
  virtual ID3D12RootSignature *GetRootSignature() const = 0;
  virtual const std::vector<PipelineDxilShader> &GetDxilShaders() const = 0;
  virtual const std::vector<PipelineSignatureLink> &GetSignatureLinks() const = 0;
  virtual const PipelineGraphicsState *GetGraphicsState() const = 0;
  virtual const PipelineComputeState *GetComputeState() const = 0;
};

Com<ID3D12PipelineState>
CreateGraphicsPipelineState(IMTLD3D12Device *device,
                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                            HRESULT *status = nullptr);

Com<ID3D12PipelineState>
CreateComputePipelineState(IMTLD3D12Device *device,
                           const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                           HRESULT *status = nullptr);

} // namespace dxmt::d3d12
