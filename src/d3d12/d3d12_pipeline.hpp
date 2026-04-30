#pragma once

#include "d3d12_root_signature.hpp"
#include "Metal.hpp"
#include "DXILParser/DXILParser.hpp"
#include <d3d12.h>
#include <cstdint>
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

class PipelineState {
public:
  virtual ~PipelineState() = default;

  virtual PipelineStateType GetType() const = 0;
  virtual ID3D12RootSignature *GetRootSignature() const = 0;
  virtual const std::vector<PipelineDxilShader> &GetDxilShaders() const = 0;
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
