#pragma once

#include "d3d12_root_signature.hpp"
#include "Metal.hpp"
#include "DXILParser/DXILParser.hpp"
#include "airconv_public.h"
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <optional>
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

enum class PipelineShaderBytecodeKind {
  Unknown,
  Dxbc,
  Dxil,
};

struct PipelineDxilShader {
  PipelineShaderStage stage = PipelineShaderStage::Vertex;
  PipelineShaderBytecodeKind kind = PipelineShaderBytecodeKind::Unknown;
  std::vector<uint8_t> bytecode;
  dxil::Parser parser;
  dxil_shader_t shader = nullptr;
  MTL_SHADER_REFLECTION reflection = {};
  std::vector<MTL_SM50_SHADER_ARGUMENT> argument_info;

  const dxil::DxilTranslationInfo *translation() const {
    const auto &info = parser.dxilTranslation();
    return info ? &*info : nullptr;
  }

  const MTL_SM50_SHADER_ARGUMENT *constantBufferInfo() const {
    return argument_info.empty() ? nullptr : argument_info.data();
  }

  const MTL_SM50_SHADER_ARGUMENT *resourceArgumentInfo() const {
    return argument_info.size() <= reflection.NumConstantBuffers
               ? nullptr
               : argument_info.data() + reflection.NumConstantBuffers;
  }
};

struct PipelineMetalShader {
  WMT::Reference<WMT::Library> library;
  WMT::Reference<WMT::Function> function;
};

struct PipelineMetalGraphicsState {
  PipelineMetalShader vertex;
  PipelineMetalShader pixel;
  WMT::Reference<WMT::RenderPipelineState> pso;
  WMT::Reference<WMT::DepthStencilState> depth_stencil;
  wmtcmd_render_setrasterizerstate rasterizer = {};
};

struct PipelineMetalComputeState {
  PipelineMetalShader compute;
  WMT::Reference<WMT::ComputePipelineState> pso;
  WMTSize threadgroup_size = {1, 1, 1};
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
  virtual const std::string &GetShaderCacheKey() const = 0;
  virtual const PipelineMetalGraphicsState *GetMetalGraphicsState() = 0;
  virtual const PipelineMetalComputeState *GetMetalComputeState() = 0;
};

Com<ID3D12PipelineState>
CreateGraphicsPipelineState(IMTLD3D12Device *device,
                            const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                            HRESULT *status = nullptr);

Com<ID3D12PipelineState>
CreateComputePipelineState(IMTLD3D12Device *device,
                           const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                           HRESULT *status = nullptr);

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
Com<ID3D12PipelineState>
CreatePipelineStateFromStream(IMTLD3D12Device *device,
                              const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                              HRESULT *status = nullptr);
#endif

#ifdef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
Com<ID3D12PipelineLibrary>
CreatePipelineLibrary(IMTLD3D12Device *device);
#endif

} // namespace dxmt::d3d12
