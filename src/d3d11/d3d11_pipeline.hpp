#pragma once

#include "Metal.hpp"
#include "d3d11_device.hpp"
#include "d3d11_input_layout.hpp"
#include "d3d11_shader.hpp"
#include "d3d11_state_object.hpp"
#include "util_hash.hpp"
#include "util_env.hpp"
#include <cstdint>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

struct MTL_GRAPHICS_PIPELINE_DESC {
  ManagedShader VertexShader;
  ManagedShader HullShader;
  ManagedShader DomainShader;
  ManagedShader GeometryShader;
  ManagedShader PixelShader;
  IMTLD3D11BlendState *BlendState;
  ManagedInputLayout InputLayout;
  IMTLD3D11StreamOutputLayout *SOLayout;
  UINT NumColorAttachments;
  WMTPixelFormat ColorAttachmentFormats[8];
  WMTPixelFormat DepthStencilFormat;
  WMTPrimitiveTopologyClass TopologyClass;
  bool RasterizationEnabled;
  uint8_t SampleCount;
  bool GSStripTopology;
  SM50_INDEX_BUFFER_FORMAT IndexBufferFormat;
  uint32_t SampleMask;
  uint32_t GSPassthrough;
};

struct MTL_COMPUTE_PIPELINE_DESC {
  ManagedShader ComputeShader;
};

struct MTL_COMPILED_GRAPHICS_PIPELINE {
  WMT::RenderPipelineState PipelineState;
};

struct MTL_COMPILED_COMPUTE_PIPELINE {
  WMT::ComputePipelineState PipelineState;
};

struct MTL_COMPILED_TESSELLATION_MESH_PIPELINE {
  WMT::RenderPipelineState PipelineState;
  uint32_t NumControlPointOutputElement;
  uint32_t ThreadsPerPatch;
};

namespace std {
template <> struct hash<MTL_GRAPHICS_PIPELINE_DESC> {
  size_t operator()(const MTL_GRAPHICS_PIPELINE_DESC &v) const noexcept {
    dxmt::HashState state;
    state.add((size_t)v.VertexShader);
    state.add((size_t)v.PixelShader);
    state.add((size_t)v.HullShader);
    state.add((size_t)v.DomainShader);
    state.add((size_t)v.GeometryShader);
    state.add((size_t)v.InputLayout);
    /* don't add blend */
    // state.add((size_t)v.BlendState);
    state.add((size_t)v.DepthStencilFormat);
    state.add((size_t)v.TopologyClass);
    state.add((size_t)v.IndexBufferFormat);
    state.add((size_t)v.GSStripTopology);
    state.add((size_t)v.SampleMask);
    state.add((size_t)v.GSPassthrough);
    state.add((size_t)v.SampleCount);
    state.add((size_t)v.NumColorAttachments);
    for (unsigned i = 0; i < v.NumColorAttachments; i++) {
      state.add(v.ColorAttachmentFormats[i]);
    }
    return state;
  };
};
template <> struct equal_to<MTL_GRAPHICS_PIPELINE_DESC> {
  bool operator()(const MTL_GRAPHICS_PIPELINE_DESC &x,
                  const MTL_GRAPHICS_PIPELINE_DESC &y) const {
    if (x.NumColorAttachments != y.NumColorAttachments)
      return false;
    for (unsigned i = 0; i < x.NumColorAttachments; i++) {
      if (x.ColorAttachmentFormats[i] != y.ColorAttachmentFormats[i])
        return false;
    }
    if (x.BlendState != y.BlendState) {
      D3D11_BLEND_DESC1 x_, y_;
      x.BlendState->GetDesc1(&x_);
      y.BlendState->GetDesc1(&y_);
      if (x_.IndependentBlendEnable != y_.IndependentBlendEnable)
        return false;
      if (x_.AlphaToCoverageEnable != y_.AlphaToCoverageEnable)
        return false;
      uint32_t num_blend_target =
          x_.IndependentBlendEnable ? x.NumColorAttachments : 1;
      for (unsigned i = 0; i < num_blend_target; i++) {
        auto &blend_target_x = x_.RenderTarget[i];
        auto &blend_target_y = y_.RenderTarget[i];
        if (blend_target_x.RenderTargetWriteMask !=
            blend_target_y.RenderTargetWriteMask)
          return false;
        if (blend_target_x.BlendEnable != blend_target_y.BlendEnable)
          return false;
        if (blend_target_x.LogicOpEnable != blend_target_y.LogicOpEnable)
          return false;
        if (blend_target_x.BlendEnable) {
          if (blend_target_x.BlendOp != blend_target_y.BlendOp)
            return false;
          if (blend_target_x.BlendOpAlpha != blend_target_y.BlendOpAlpha)
            return false;
          if (blend_target_x.SrcBlend != blend_target_y.SrcBlend)
            return false;
          if (blend_target_x.SrcBlendAlpha != blend_target_y.SrcBlendAlpha)
            return false;
          if (blend_target_x.DestBlend != blend_target_y.DestBlend)
            return false;
          if (blend_target_x.DestBlendAlpha != blend_target_y.DestBlendAlpha)
            return false;
        }
        if (blend_target_x.LogicOpEnable) {
          if (blend_target_x.LogicOp != blend_target_y.LogicOp)
            return false;
        }
      }
    }
    return (x.VertexShader == y.VertexShader) &&
           (x.PixelShader == y.PixelShader) && (x.HullShader == y.HullShader) &&
           (x.DomainShader == y.DomainShader) && 
           (x.GeometryShader == y.GeometryShader) &&
           (x.InputLayout == y.InputLayout) &&
           (x.DepthStencilFormat == y.DepthStencilFormat) &&
           (x.TopologyClass == y.TopologyClass) &&
           (x.RasterizationEnabled == y.RasterizationEnabled) &&
           (x.GSStripTopology == y.GSStripTopology) &&
           (x.SampleCount == y.SampleCount) &&
           (x.IndexBufferFormat == y.IndexBufferFormat) &&
           (x.SampleMask == y.SampleMask) &&
           (x.GSPassthrough == y.GSPassthrough);
  }
};
} // namespace std

namespace dxmt {

inline std::string
DebugShaderName(ManagedShader shader) {
  if (!shader)
    return "null";

  std::stringstream stream;
  stream << "0x" << std::hex << reinterpret_cast<std::uintptr_t>(shader)
         << "/" << shader->sha1().string();
  return stream.str();
}

inline std::string
DebugPipelineKey(const MTL_GRAPHICS_PIPELINE_DESC &desc) {
  std::stringstream stream;
  stream << std::hex << std::hash<MTL_GRAPHICS_PIPELINE_DESC>{}(desc);
  return stream.str();
}

inline std::string
DebugPipelineDesc(const char *kind, const MTL_GRAPHICS_PIPELINE_DESC &desc) {
  std::stringstream stream;
  stream << kind << " key=0x" << DebugPipelineKey(desc) << std::dec
         << " VS=" << DebugShaderName(desc.VertexShader)
         << " HS=" << DebugShaderName(desc.HullShader)
         << " DS=" << DebugShaderName(desc.DomainShader)
         << " GS=" << DebugShaderName(desc.GeometryShader)
         << " PS=" << DebugShaderName(desc.PixelShader)
         << " IL=0x" << std::hex << reinterpret_cast<std::uintptr_t>(desc.InputLayout)
         << " SO=0x" << reinterpret_cast<std::uintptr_t>(desc.SOLayout)
         << std::dec
         << " raster=" << desc.RasterizationEnabled
         << " rt_count=" << desc.NumColorAttachments
         << " depth=" << desc.DepthStencilFormat
         << " topology=" << desc.TopologyClass
         << " sample_count=" << unsigned(desc.SampleCount)
         << " sample_mask=0x" << std::hex << desc.SampleMask << std::dec
         << " gs_passthrough=0x" << std::hex << desc.GSPassthrough << std::dec
         << " gs_strip=" << desc.GSStripTopology
         << " index_format=" << desc.IndexBufferFormat
         << " rt_formats=[";

  for (unsigned i = 0; i < desc.NumColorAttachments && i < 8; i++) {
    if (i)
      stream << ",";
    stream << desc.ColorAttachmentFormats[i];
  }

  stream << "]";
  return stream.str();
}

inline std::string
DebugPipelineDumpDirectory() {
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

inline bool
DebugShaderHashMatchesFilter(ManagedShader shader, std::string filter) {
  if (filter.empty())
    return false;
  if (!shader)
    return filter == "null";

  std::string hash = shader->sha1().string();
  size_t start = 0;
  for (;;) {
    size_t end = filter.find_first_of(",; ", start);
    std::string item = filter.substr(start, end == std::string::npos ? end : end - start);
    if (!item.empty() && (hash == item || hash.starts_with(item)))
      return true;
    if (end == std::string::npos)
      return false;
    start = end + 1;
  }
}

inline bool
DebugShouldDumpPipeline(const MTL_GRAPHICS_PIPELINE_DESC &desc, bool problem) {
  std::string mode = env::getEnvVar("DXMT_DUMP_PIPELINES");
  if (mode == "0" || mode == "none" || mode == "false")
    return false;

  std::string key = env::getEnvVar("DXMT_DUMP_PIPELINE_KEY");
  std::string vs = env::getEnvVar("DXMT_DUMP_PIPELINE_VS");
  std::string ps = env::getEnvVar("DXMT_DUMP_PIPELINE_PS");
  std::string gs = env::getEnvVar("DXMT_DUMP_PIPELINE_GS");
  std::string hs = env::getEnvVar("DXMT_DUMP_PIPELINE_HS");
  std::string ds = env::getEnvVar("DXMT_DUMP_PIPELINE_DS");

  if (!key.empty()) {
    if (key.starts_with("0x") || key.starts_with("0X"))
      key = key.substr(2);
    if (key != DebugPipelineKey(desc))
      return false;
  }

  auto shader_matches = [](const std::string &expected, ManagedShader shader) {
    if (expected.empty())
      return true;
    if (!shader)
      return expected == "null";
    return expected == shader->sha1().string();
  };

  bool has_shader_filter = !vs.empty() || !ps.empty() || !gs.empty() ||
                           !hs.empty() || !ds.empty();
  if (has_shader_filter) {
    return shader_matches(vs, desc.VertexShader) &&
           shader_matches(ps, desc.PixelShader) &&
           shader_matches(gs, desc.GeometryShader) &&
           shader_matches(hs, desc.HullShader) &&
           shader_matches(ds, desc.DomainShader);
  }

  if (!key.empty())
    return true;

  if (mode == "all")
    return true;

  return problem || mode == "1" || mode == "problem";
}

inline bool
DebugShouldDumpComputeShader(ManagedShader shader) {
  std::string filter = env::getEnvVar("DXMT_DUMP_COMPUTE_SHADERS");
  if (filter == "0" || filter == "none" || filter == "false")
    return false;
  if (filter == "1" || filter == "all")
    return true;

  if (filter.empty())
    filter = env::getEnvVar("DXMT_DUMP_PIPELINE_CS");
  return DebugShaderHashMatchesFilter(shader, filter);
}

inline void
DebugDumpShader(const char *stage, ManagedShader shader) {
  if (!shader)
    return;
  WARN("DXMT diagnostic: dumping ", stage, " shader ", shader->sha1().string());
  shader->dump();
}

inline void
DebugDumpComputeShader(ManagedShader shader, const char *reason) {
  if (!DebugShouldDumpComputeShader(shader))
    return;

  std::string path = DebugPipelineDumpDirectory() + env::getExeBaseName() +
                     "_pipeline_compute_" + shader->sha1().string().substr(0, 16) + ".txt";

  std::ofstream dump(path, std::ios::out | std::ios::trunc);
  if (dump) {
    auto &reflection = shader->reflection();
    dump << "reason=" << reason << "\n";
    dump << "CS=" << DebugShaderName(shader) << "\n";
    dump << "threadgroup_size=" << reflection.ThreadgroupSize[0] << ","
         << reflection.ThreadgroupSize[1] << ","
         << reflection.ThreadgroupSize[2] << "\n";
  }

  DebugDumpShader("CS", shader);
  WARN("DXMT diagnostic: compute pipeline dumped to ", path);
}

inline void
DebugDumpPipeline(const char *kind, const MTL_GRAPHICS_PIPELINE_DESC &desc,
                  const char *reason, bool problem) {
  if (!DebugShouldDumpPipeline(desc, problem))
    return;

  std::string key = DebugPipelineKey(desc);
  std::string path = DebugPipelineDumpDirectory() + env::getExeBaseName() +
                     "_pipeline_" + kind + "_0x" + key + ".txt";

  std::ofstream dump(path, std::ios::out | std::ios::trunc);
  if (dump) {
    dump << "reason=" << reason << "\n";
    dump << DebugPipelineDesc(kind, desc) << "\n";
  }

  DebugDumpShader("VS", desc.VertexShader);
  DebugDumpShader("HS", desc.HullShader);
  DebugDumpShader("DS", desc.DomainShader);
  DebugDumpShader("GS", desc.GeometryShader);
  DebugDumpShader("PS", desc.PixelShader);

  WARN("DXMT diagnostic: pipeline dumped to ", path);
}

inline void
WarnMissingMeshFragmentFunction(const char *kind, const MTL_GRAPHICS_PIPELINE_DESC &desc) {
  if (desc.RasterizationEnabled && !desc.PixelShader) {
    WARN("DXMT diagnostic: ", kind,
         " mesh pipeline has rasterization enabled but no pixel shader: ",
         DebugPipelineDesc(kind, desc));
    DebugDumpPipeline(kind, desc, "mesh pipeline has rasterization enabled but no pixel shader", true);
  }
}

class MTLCompiledGraphicsPipeline : public ThreadpoolWork {
public:
  /**
  NOTE: the current thread is blocked if it's not ready
   */
  virtual void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pGraphicsPipeline) = 0;
};

class MTLCompiledComputePipeline : public ThreadpoolWork {
public:
  virtual void GetPipeline(MTL_COMPILED_COMPUTE_PIPELINE *pComputePipeline) = 0;
};

class MTLCompiledGeometryPipeline : public ThreadpoolWork {
public:
  virtual void GetPipeline(MTL_COMPILED_GRAPHICS_PIPELINE *pGeometryPipeline) = 0;
};

class MTLCompiledTessellationMeshPipeline : public ThreadpoolWork {
public:
  virtual void GetPipeline(MTL_COMPILED_TESSELLATION_MESH_PIPELINE *pTessellationPipeline) = 0;
};

std::unique_ptr<MTLCompiledGraphicsPipeline>
CreateGraphicsPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

std::unique_ptr<MTLCompiledComputePipeline> CreateComputePipeline(MTLD3D11Device *pDevice, ManagedShader ComputeShader);

std::unique_ptr<MTLCompiledGeometryPipeline>
CreateGeometryPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

std::unique_ptr<MTLCompiledTessellationMeshPipeline>
CreateTessellationMeshPipeline(MTLD3D11Device *pDevice, MTL_GRAPHICS_PIPELINE_DESC *pDesc);

}; // namespace dxmt
