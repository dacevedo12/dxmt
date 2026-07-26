#include "d3d12_descriptor_diagnostics.hpp"

#include "d3d12_subresource_geometry.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include <atomic>

namespace dxmt::d3d12 {
namespace {

bool DescriptorViewDiagnosticsEnabled() {
  static const bool enabled = [] {
    for (const char *name :
         {"DXMT_DIAG_D3D12_VIEWS", "DXMT_DIAG_COMMAND_QUEUE",
          "DXMT_DIAG_ROOT_CAUSE_DENSE"}) {
      const auto value = env::getEnvVar(name);
      if (value == "1" || value == "true" || value == "yes" ||
          value == "trace")
        return true;
    }
    return false;
  }();
  return enabled;
}

bool ShouldLogDescriptorView(std::atomic<uint32_t> &counter) noexcept {
  if (!DescriptorViewDiagnosticsEnabled())
    return false;
  const auto occurrence =
      counter.fetch_add(1, std::memory_order_relaxed) + 1;
  return occurrence <= 16 || (occurrence & (occurrence - 1)) == 0;
}

} // namespace

DXGI_FORMAT
D3D12DiagDescriptorFormat(const DescriptorRecord &descriptor) noexcept {
  if (!descriptor.has_desc)
    return DXGI_FORMAT_UNKNOWN;

  switch (descriptor.type) {
  case DescriptorRecordType::ShaderResourceView:
    return descriptor.desc.srv.Format;
  case DescriptorRecordType::UnorderedAccessView:
    return descriptor.desc.uav.Format;
  case DescriptorRecordType::RenderTargetView:
    return descriptor.desc.rtv.Format;
  case DescriptorRecordType::DepthStencilView:
    return descriptor.desc.dsv.Format;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

const char *DescriptorRecordTypeName(DescriptorRecordType type) noexcept {
  switch (type) {
  case DescriptorRecordType::Empty:
    return "empty";
  case DescriptorRecordType::ConstantBufferView:
    return "cbv";
  case DescriptorRecordType::ShaderResourceView:
    return "srv";
  case DescriptorRecordType::UnorderedAccessView:
    return "uav";
  case DescriptorRecordType::RenderTargetView:
    return "rtv";
  case DescriptorRecordType::DepthStencilView:
    return "dsv";
  case DescriptorRecordType::Sampler:
    return "sampler";
  default:
    return "unknown";
  }
}

void D3D12DiagLogTextureView(const char *kind, Resource &resource,
                             const DescriptorRecord &descriptor,
                             const TextureViewDescriptor &view,
                             TextureViewKey key) {
  static std::atomic<uint32_t> log_count = 0;
  if (!ShouldLogDescriptorView(log_count))
    return;

  auto *texture = resource.GetTexture();
  auto *allocation = resource.GetTextureAllocation();
  const auto &desc = resource.GetResourceDesc();
  INFO("D3D12 diagnostic: texture view",
       " kind=", kind,
       " key=", uint64_t(key),
       " resource=", uint64_t(resource.GetD3D12Resource()),
       " texture_descriptor=", uint64_t(texture),
       " allocation=", uint64_t(allocation),
       " has_desc=", descriptor.has_desc,
       " desc_format=", uint32_t(D3D12DiagDescriptorFormat(descriptor)),
       " resource_format=", uint32_t(desc.Format),
       " resource_dimension=", uint32_t(desc.Dimension),
       " resource_size=", uint64_t(desc.Width), "x", uint32_t(desc.Height),
       "x", uint32_t(desc.DepthOrArraySize),
       " resource_mips=", uint32_t(desc.MipLevels),
       " texture_format=",
       texture ? uint32_t(texture->pixelFormat()) : 0,
       " texture=",
       texture && texture->current()
           ? uint64_t(texture->current()->texture())
           : 0,
       " texture_type=", texture ? uint32_t(texture->textureType()) : 0,
       " texture_size=", texture ? texture->width() : 0, "x",
       texture ? texture->height() : 0, "x",
       texture ? texture->depth() : 0,
       " texture_array=", texture ? texture->arrayLength() : 0,
       " texture_mips=", texture ? texture->miplevelCount() : 0,
       " texture_samples=", texture ? texture->sampleCount() : 0,
       " view_format=", uint32_t(view.format),
       " view_type=", uint32_t(view.type),
       " view_mip=", uint32_t(view.firstMiplevel),
       " view_mips=", uint32_t(view.miplevelCount),
       " view_array=", uint32_t(view.firstArraySlice),
       " view_array_size=", uint32_t(view.arraySize),
       " view_swizzle=", uint32_t(view.swizzle.r), ",",
       uint32_t(view.swizzle.g), ",", uint32_t(view.swizzle.b), ",",
       uint32_t(view.swizzle.a),
       " view_usage=", uint32_t(view.intendedUsage));
}

void D3D12DiagLogDSVReplayDescriptor(
    const char *context, Resource &resource,
    const DescriptorRecord &descriptor, const TextureViewDescriptor &view,
    TextureViewKey key) {
  static std::atomic<uint32_t> log_count = 0;
  if (!ShouldLogDescriptorView(log_count))
    return;

  auto *texture = resource.GetTexture();
  const auto &desc = resource.GetResourceDesc();
  const auto &dsv = descriptor.desc.dsv;
  WARN_FILE_ONLY(
      "D3D12 diagnostic: DSV replay descriptor"
      " context=",
      context, " key=", uint64_t(key),
      " descriptorType=", DescriptorRecordTypeName(descriptor.type),
      " cpuHandle=", uint64_t(descriptor.cpu_handle.ptr),
      " heapIndex=", descriptor.heap_index,
      " heapCount=", descriptor.heap_count,
      " has_desc=", descriptor.has_desc,
      " resource=", uint64_t(resource.GetD3D12Resource()),
      " resource_dimension=", uint32_t(desc.Dimension),
      " resource_size=", uint64_t(desc.Width), "x", uint32_t(desc.Height),
      "x", uint32_t(desc.DepthOrArraySize),
      " resource_mips=", uint32_t(desc.MipLevels),
      " resource_format=", uint32_t(desc.Format),
      " resource_samples=", uint32_t(desc.SampleDesc.Count),
      " texture_descriptor=", uint64_t(texture),
      " texture_type=", texture ? uint32_t(texture->textureType()) : 0,
      " texture_size=", texture ? texture->width() : 0, "x",
      texture ? texture->height() : 0, "x",
      texture ? texture->depth() : 0,
      " texture_array=", texture ? texture->arrayLength() : 0,
      " texture_mips=", texture ? texture->miplevelCount() : 0,
      " texture_samples=", texture ? texture->sampleCount() : 0,
      " dsv_format=", descriptor.has_desc ? uint32_t(dsv.Format) : 0,
      " dsv_dimension=",
      descriptor.has_desc ? uint32_t(dsv.ViewDimension) : 0,
      " dsv_flags=", descriptor.has_desc ? uint32_t(dsv.Flags) : 0,
      " dsv_tex2d_mip=",
      descriptor.has_desc ? uint32_t(dsv.Texture2D.MipSlice) : 0,
      " dsv_tex2d_array_mip=",
      descriptor.has_desc ? uint32_t(dsv.Texture2DArray.MipSlice) : 0,
      " dsv_tex2d_array_first=",
      descriptor.has_desc ? uint32_t(dsv.Texture2DArray.FirstArraySlice) : 0,
      " dsv_tex2d_array_size=",
      descriptor.has_desc ? uint32_t(dsv.Texture2DArray.ArraySize) : 0,
      " dsv_tex2dms_array_first=",
      descriptor.has_desc ? uint32_t(dsv.Texture2DMSArray.FirstArraySlice) : 0,
      " dsv_tex2dms_array_size=",
      descriptor.has_desc ? uint32_t(dsv.Texture2DMSArray.ArraySize) : 0,
      " view_format=", uint32_t(view.format),
      " view_type=", uint32_t(view.type),
      " view_mip=", uint32_t(view.firstMiplevel),
      " view_mips=", uint32_t(view.miplevelCount),
      " view_array=", uint32_t(view.firstArraySlice),
      " view_array_size=", uint32_t(view.arraySize));
}

void WarnDescriptorSubresourceRangeUnsupported(
    Resource &resource, const DescriptorRecord &descriptor,
    const char *context) {
  static std::atomic<uint32_t> log_count = 0;
  if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
    WARN("D3D12CommandQueue: TODO descriptor subresource range unsupported;"
         " skipping resource-state tracking context=",
         context, " resource=", resource.GetD3D12Resource(),
         " descriptorType=", DescriptorRecordTypeName(descriptor.type),
         " descriptorFormat=", uint32_t(D3D12DiagDescriptorFormat(descriptor)),
         " dimension=", uint32_t(resource.GetResourceDesc().Dimension),
         " size=", uint64_t(resource.GetResourceDesc().Width), "x",
         uint32_t(resource.GetResourceDesc().Height), "x",
         uint32_t(resource.GetResourceDesc().DepthOrArraySize),
         " format=", uint32_t(resource.GetResourceDesc().Format),
         " mipLevels=", uint32_t(GetResourceMipLevelCount(resource)),
         " flags=", uint32_t(resource.GetResourceDesc().Flags));
  }
}

void WarnDescriptorSubresourceRangeEmpty(Resource &resource,
                                         const DescriptorRecord &descriptor,
                                         const char *context) {
  static std::atomic<uint32_t> log_count = 0;
  if (log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
    WARN("D3D12CommandQueue: descriptor subresource range is empty;"
         " skipping resource-state tracking context=",
         context, " resource=", resource.GetD3D12Resource(),
         " descriptorType=", DescriptorRecordTypeName(descriptor.type));
  }
}

} // namespace dxmt::d3d12
