#include "d3d12_pipeline_write_policy.hpp"

#include "dxmt_deptrack.hpp"
#include "log/log.hpp"

namespace dxmt::d3d12 {

namespace {

constexpr uint32_t kMaxThreadgroupWidth = 1024;
constexpr uint32_t kMaxThreadgroupHeight = 1024;
constexpr uint32_t kMaxThreadgroupDepth = 64;
constexpr uint32_t kMaxThreadsPerThreadgroup = 1024;
constexpr UINT kMaxThreadgroupCountPerDimension = 65535;

} // namespace

bool StencilOpWrites(D3D12_STENCIL_OP op) {
  return op != D3D12_STENCIL_OP_KEEP;
}

bool StencilFaceWrites(const D3D12_DEPTH_STENCILOP_DESC &face) {
  return StencilOpWrites(face.StencilFailOp) ||
         StencilOpWrites(face.StencilDepthFailOp) ||
         StencilOpWrites(face.StencilPassOp);
}

bool PipelineWritesDepth(const PipelineGraphicsState *graphics) {
  if (!graphics)
    return true;
  const auto &desc = graphics->desc.DepthStencilState;
  return desc.DepthEnable &&
         desc.DepthWriteMask == D3D12_DEPTH_WRITE_MASK_ALL;
}

bool PipelineWritesStencil(const PipelineGraphicsState *graphics) {
  if (!graphics)
    return true;
  const auto &desc = graphics->desc.DepthStencilState;
  return desc.StencilEnable && desc.StencilWriteMask &&
         (StencilFaceWrites(desc.FrontFace) ||
          StencilFaceWrites(desc.BackFace));
}

int AccessForDepthStencilPlane(const DescriptorRecord &descriptor,
                               D3D12_DSV_FLAGS read_only_flag,
                               bool pipeline_writes) {
  if (descriptor.has_desc && (descriptor.desc.dsv.Flags & read_only_flag))
    return ResourceAccess::Read;
  return pipeline_writes ? ResourceAccess::ReadWrite : ResourceAccess::Read;
}

D3D12_RESOURCE_STATES
DepthStencilResourceStateForAccess(int depth_access, int stencil_access) {
  return ((depth_access | stencil_access) & ResourceAccess::Write)
             ? D3D12_RESOURCE_STATE_DEPTH_WRITE
             : D3D12_RESOURCE_STATE_DEPTH_READ;
}

bool ValidateComputeDispatch(const WMTSize &threadgroup_size, UINT x, UINT y,
                             UINT z) {
  const auto threads_per_group = threadgroup_size.width *
                                 threadgroup_size.height *
                                 threadgroup_size.depth;
  if (!threadgroup_size.width || !threadgroup_size.height ||
      !threadgroup_size.depth || threads_per_group == 0) {
    WARN("D3D12CommandQueue: dispatch skipped because compute shader has invalid threadgroup size");
    return false;
  }
  if (threadgroup_size.width > kMaxThreadgroupWidth ||
      threadgroup_size.height > kMaxThreadgroupHeight ||
      threadgroup_size.depth > kMaxThreadgroupDepth ||
      threads_per_group > kMaxThreadsPerThreadgroup) {
    WARN("D3D12CommandQueue: dispatch skipped because compute shader threadgroup size exceeds D3D limits size=",
         threadgroup_size.width, "x", threadgroup_size.height, "x",
         threadgroup_size.depth);
    return false;
  }
  if (x > kMaxThreadgroupCountPerDimension ||
      y > kMaxThreadgroupCountPerDimension ||
      z > kMaxThreadgroupCountPerDimension) {
    WARN("D3D12CommandQueue: dispatch grid exceeds D3D threadgroup-count limits grid=",
         x, "x", y, "x", z);
    return false;
  }
  return true;
}

} // namespace dxmt::d3d12
