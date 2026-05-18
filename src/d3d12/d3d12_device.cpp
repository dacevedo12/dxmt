#include "d3d12_device.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_command_allocator.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_command_queue.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_agility.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_query.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace dxmt::d3d12 {

namespace {

constexpr D3D_FEATURE_LEVEL kSupportedFeatureLevel = D3D_FEATURE_LEVEL_12_0;

static UINT64
Align(UINT64 value, UINT64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static UINT
SubresourceMipSlice(UINT sub_resource, UINT mip_levels) {
  return mip_levels ? sub_resource % mip_levels : 0;
}

static UINT
GetD3D12FormatPlaneCount(DXGI_FORMAT format) {
  const auto &traits = GetDXGIFormatTraits(format);
  return traits.planeCount ? traits.planeCount : 1;
}

static DXGI_FORMAT
GetD3D12FootprintFormat(DXGI_FORMAT format, UINT plane) {
  const auto &traits = GetDXGIFormatTraits(format);
  if (plane < traits.planeCount && traits.planes[plane].footprintFormat)
    return static_cast<DXGI_FORMAT>(traits.planes[plane].footprintFormat);
  return format;
}

static UINT
GetD3D12FormatPlaneElementSize(DXGI_FORMAT format, UINT plane,
                               const MTL_DXGI_FORMAT_DESC &format_desc) {
  const auto &traits = GetDXGIFormatTraits(format);
  if (plane < traits.planeCount && traits.planes[plane].elementSize)
    return traits.planes[plane].elementSize;
  return (format_desc.Flag & MTL_DXGI_FORMAT_BC)
             ? format_desc.BlockSize
             : format_desc.BytesPerTexel;
}

static bool
HasD3D12TraitFootprintLayout(const DXGIFormatTraits &traits) {
  if (!traits.planeCount || traits.classification == DXGIFormatClass::Unsupported ||
      traits.classification == DXGIFormatClass::Mask)
    return false;

  for (UINT plane = 0; plane < traits.planeCount; ++plane) {
    if (!traits.planes[plane].elementSize)
      return false;
  }
  return true;
}

static UINT
GetD3D12FormatBlockWidth(const MTL_DXGI_FORMAT_DESC &format_desc) {
  return (format_desc.Flag & MTL_DXGI_FORMAT_BC) ? 4 : 1;
}

static UINT
GetD3D12FormatBlockHeight(const MTL_DXGI_FORMAT_DESC &format_desc) {
  return (format_desc.Flag & MTL_DXGI_FORMAT_BC) ? 4 : 1;
}

static void
GetD3D12FormatSubsampleLog2(DXGI_FORMAT format, UINT plane, UINT &x, UINT &y) {
  const auto &traits = GetDXGIFormatTraits(format);
  if (plane < traits.planeCount) {
    x = traits.planes[plane].subsampleXLog2;
    y = traits.planes[plane].subsampleYLog2;
  } else {
    x = 0;
    y = 0;
  }
}

static UINT64
MipSize(UINT64 value, UINT mip_slice) {
  return std::max<UINT64>(1, value >> mip_slice);
}

static bool
IsCpuVisibleHeap(D3D12_HEAP_TYPE heap_type) {
  return heap_type == D3D12_HEAP_TYPE_UPLOAD ||
         heap_type == D3D12_HEAP_TYPE_READBACK;
}

static bool
HasInvalidCpuVisibleBufferFlags(const D3D12_RESOURCE_DESC &desc,
                                D3D12_HEAP_TYPE heap_type) {
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
    return false;
  if (heap_type != D3D12_HEAP_TYPE_UPLOAD &&
      heap_type != D3D12_HEAP_TYPE_READBACK)
    return false;
  return desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                       D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
}

static bool
IsValidCrossAdapterTextureDesc(const D3D12_HEAP_PROPERTIES &heap_properties,
                               D3D12_HEAP_FLAGS heap_flags,
                               const D3D12_RESOURCE_DESC &desc) {
  const bool cross_adapter =
      desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
  const bool cross_adapter_heap =
      heap_flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;

  if (!cross_adapter && !cross_adapter_heap)
    return true;

  if (!cross_adapter || !cross_adapter_heap ||
      !(heap_flags & D3D12_HEAP_FLAG_SHARED))
    return false;
  if (d3d12::GetHeapType(heap_properties) != D3D12_HEAP_TYPE_DEFAULT)
    return false;
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR &&
         desc.MipLevels <= 1 && desc.DepthOrArraySize == 1 &&
         desc.SampleDesc.Count == 1;
}

static bool
IsValidCopyableFootprintDesc(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Width == 0 || desc.Height == 0 || desc.DepthOrArraySize == 0)
    return false;
  if (desc.SampleDesc.Count == 0)
    return false;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return desc.Format == DXGI_FORMAT_UNKNOWN;
  return desc.Format != DXGI_FORMAT_UNKNOWN;
}

static bool
IsBcFormat(WMT::Device device, DXGI_FORMAT format) {
  MTL_DXGI_FORMAT_DESC format_desc = {};
  return SUCCEEDED(MTLQueryDXGIFormat(device, format, format_desc)) &&
         (format_desc.Flag & MTL_DXGI_FORMAT_BC);
}

static UINT
IndirectArgumentByteSize(const D3D12_INDIRECT_ARGUMENT_DESC &argument) {
  switch (argument.Type) {
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
    return sizeof(D3D12_DRAW_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
    return sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
    return sizeof(D3D12_DISPATCH_ARGUMENTS);
  case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW:
    return sizeof(D3D12_VERTEX_BUFFER_VIEW);
  case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW:
    return sizeof(D3D12_INDEX_BUFFER_VIEW);
  case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
    return sizeof(UINT) * argument.Constant.Num32BitValuesToSet;
  case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
  case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
  case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
    return sizeof(D3D12_GPU_VIRTUAL_ADDRESS);
  default:
    return 0;
  }
}

static bool
IsIndirectOperationArgument(D3D12_INDIRECT_ARGUMENT_TYPE type) {
  return type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW ||
         type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED ||
         type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
}

static bool
HasFormatCapability(FormatCapability caps, FormatCapability cap) {
  return (static_cast<int>(caps) & static_cast<int>(cap)) != 0;
}

static DXGI_FORMAT
ResolveDepthTypelessFormatForD3D12Caps(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R16_TYPELESS:
    return DXGI_FORMAT_D16_UNORM;
  case DXGI_FORMAT_R32_TYPELESS:
    return DXGI_FORMAT_D32_FLOAT;
  default:
    return format;
  }
}

static FormatCapability
GetD3D12FormatCapability(WMT::Device device,
                         const MTL_DXGI_FORMAT_DESC &format) {
  FormatCapabilityInspector inspector;
  inspector.Inspect(device);
  auto entry = inspector.textureCapabilities.find(format.PixelFormat);
  return entry == inspector.textureCapabilities.end()
             ? FormatCapability::None
             : entry->second;
}

static D3D12_FORMAT_SUPPORT1
GetD3D12FormatSupport1(FormatCapability caps,
                       const MTL_DXGI_FORMAT_DESC &format) {
  D3D12_FORMAT_SUPPORT1 support = D3D12_FORMAT_SUPPORT1_NONE;
  if (HasFormatCapability(caps, FormatCapability::TextureBufferRead) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferWrite) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT1_BUFFER;

  support |= D3D12_FORMAT_SUPPORT1_TEXTURE1D |
             D3D12_FORMAT_SUPPORT1_TEXTURE2D |
             D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
             D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;

  if (!(format.Flag & (MTL_DXGI_FORMAT_DEPTH_PLANER |
                       MTL_DXGI_FORMAT_STENCIL_PLANER))) {
    support |= D3D12_FORMAT_SUPPORT1_TEXTURE3D |
               D3D12_FORMAT_SUPPORT1_TEXTURECUBE;
  }
  if (HasFormatCapability(caps, FormatCapability::Filter))
    support |= D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
               D3D12_FORMAT_SUPPORT1_SHADER_GATHER;
  if (HasFormatCapability(caps, FormatCapability::Color))
    support |= D3D12_FORMAT_SUPPORT1_RENDER_TARGET;
  if (HasFormatCapability(caps, FormatCapability::Blend))
    support |= D3D12_FORMAT_SUPPORT1_BLENDABLE;
  if (HasFormatCapability(caps, FormatCapability::DepthStencil))
    support |= D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL;
  if (HasFormatCapability(caps, FormatCapability::MSAA))
    support |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET |
               D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD;
  if (HasFormatCapability(caps, FormatCapability::Resolve))
    support |= D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE;
  if (HasFormatCapability(caps, FormatCapability::Write))
    support |= D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
  if (format.Flag & MTL_DXGI_FORMAT_BACKBUFFER)
    support |= D3D12_FORMAT_SUPPORT1_DISPLAY |
               D3D12_FORMAT_SUPPORT1_BACK_BUFFER_CAST;

  return support;
}

static D3D12_FORMAT_SUPPORT2
GetD3D12FormatSupport2(FormatCapability caps) {
  D3D12_FORMAT_SUPPORT2 support = D3D12_FORMAT_SUPPORT2_NONE;
  if (HasFormatCapability(caps, FormatCapability::TextureBufferRead) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
  if (HasFormatCapability(caps, FormatCapability::Write) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferWrite) ||
      HasFormatCapability(caps, FormatCapability::TextureBufferReadWrite))
    support |= D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
  if (HasFormatCapability(caps, FormatCapability::Atomic))
    support |= D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_ADD |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_BITWISE_OPS |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_COMPARE_STORE_OR_COMPARE_EXCHANGE |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_SIGNED_MIN_OR_MAX |
               D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_UNSIGNED_MIN_OR_MAX;
  if (HasFormatCapability(caps, FormatCapability::Sparse))
    support |= D3D12_FORMAT_SUPPORT2_TILED;
  return support;
}

static UINT8
GetD3D12FormatPlaneCountForFeatureData(DXGI_FORMAT dxgi_format,
                         const MTL_DXGI_FORMAT_DESC &format) {
  const auto &traits = GetDXGIFormatTraits(dxgi_format);
  if (traits.planeCount)
    return traits.planeCount;
  const uint32_t planes = DepthStencilPlanarFlags(format.PixelFormat);
  return planes == 3 ? 2 : 1;
}

static D3D12_FORMAT_SUPPORT1
GetD3D12TraitFormatSupport1(const DXGIFormatTraits &traits) {
  if (traits.classification == DXGIFormatClass::Mask ||
      traits.classification == DXGIFormatClass::Unsupported)
    return D3D12_FORMAT_SUPPORT1_NONE;

  if (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) {
    return D3D12_FORMAT_SUPPORT1_TEXTURE2D |
           D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
           D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
           D3D12_FORMAT_SUPPORT1_SHADER_GATHER |
           D3D12_FORMAT_SUPPORT1_CAST_WITHIN_BIT_LAYOUT;
  }

  return D3D12_FORMAT_SUPPORT1_NONE;
}

static D3D12_FORMAT_SUPPORT2
GetD3D12TraitFormatSupport2(const DXGIFormatTraits &traits) {
  if (traits.classification == DXGIFormatClass::Mask ||
      traits.classification == DXGIFormatClass::Unsupported)
    return D3D12_FORMAT_SUPPORT2_NONE;
  return D3D12_FORMAT_SUPPORT2_NONE;
}

static bool
IsSupportedD3D12SampleCount(UINT sample_count) {
  return sample_count == 1 || sample_count == 2 || sample_count == 4 ||
         sample_count == 8;
}

static bool
IsSupportedCommandListType(D3D12_COMMAND_LIST_TYPE type) {
  return type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         type == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
         type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
         type == D3D12_COMMAND_LIST_TYPE_COPY;
}

static bool
IsSupportedCommandQueuePriority(UINT priority) {
  return priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL ||
         priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
}

static DescriptorRecord *
GetDescriptorRecordForWrite(D3D12_CPU_DESCRIPTOR_HANDLE handle,
                            D3D12_DESCRIPTOR_HEAP_TYPE expected_type,
                            const char *context) {
  auto *record = d3d12::GetDescriptorRecordRangeFromCpuHandle(
      handle, expected_type, 1, context);
  if (!record)
    WARN("D3D12Device: invalid descriptor handle for ", context);
  return record;
}

static void
ResetDescriptorRecord(DescriptorRecord &record) {
  const auto magic = record.magic;
  const auto heap_type = record.heap_type;
  const auto shader_visible = record.shader_visible;
  const auto cpu_handle = record.cpu_handle;
  const auto heap_index = record.heap_index;
  const auto heap_count = record.heap_count;
  record = {};
  record.magic = magic;
  record.heap_type = heap_type;
  record.shader_visible = shader_visible;
  record.cpu_handle = cpu_handle;
  record.heap_index = heap_index;
  record.heap_count = heap_count;
}

static bool
IsBufferResource(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  return d3d12_resource &&
         d3d12_resource->GetResourceDesc().Dimension ==
             D3D12_RESOURCE_DIMENSION_BUFFER;
}

static bool
IsTextureResource(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  if (!d3d12_resource)
    return false;
  const auto dimension = d3d12_resource->GetResourceDesc().Dimension;
  return dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
         dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
         dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
}

static const D3D12_RESOURCE_DESC *
GetResourceDesc(ID3D12Resource *resource) {
  auto *d3d12_resource = dynamic_cast<Resource *>(resource);
  return d3d12_resource ? &d3d12_resource->GetResourceDesc() : nullptr;
}

static UINT
GetTextureMipDepth(const D3D12_RESOURCE_DESC &resource_desc, UINT mip_slice) {
  if (resource_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 1;
  return static_cast<UINT>(
      std::max<UINT64>(1, resource_desc.DepthOrArraySize >> mip_slice));
}

static bool
IsSupportedSrvDimension(D3D12_SRV_DIMENSION dimension) {
  switch (dimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
  case D3D12_SRV_DIMENSION_TEXTURE1D:
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE2D:
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE2DMS:
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
  case D3D12_SRV_DIMENSION_TEXTURE3D:
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    return true;
  default:
    return false;
  }
}

static bool
IsSupportedUavDimension(D3D12_UAV_DIMENSION dimension) {
  switch (dimension) {
  case D3D12_UAV_DIMENSION_BUFFER:
  case D3D12_UAV_DIMENSION_TEXTURE1D:
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
  case D3D12_UAV_DIMENSION_TEXTURE2D:
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    return true;
  default:
    return false;
  }
}

static bool
ValidateShaderResourceView(ID3D12Resource *resource,
                           const D3D12_SHADER_RESOURCE_VIEW_DESC &desc) {
  if (!IsSupportedSrvDimension(desc.ViewDimension)) {
    WARN("D3D12Device: unsupported SRV dimension ",
         uint32_t(desc.ViewDimension));
    return false;
  }
  if (desc.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
    if (resource && !IsBufferResource(resource)) {
      WARN("D3D12Device: buffer SRV created for non-buffer resource");
      return false;
    }
    const auto raw = (desc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) != 0;
    const auto structured = desc.Buffer.StructureByteStride != 0;
    const auto typed =
        desc.Format != DXGI_FORMAT_UNKNOWN &&
        !(raw && desc.Format == DXGI_FORMAT_R32_TYPELESS);
    if ((raw && structured) || (raw && typed) || (structured && typed)) {
      WARN("D3D12Device: ambiguous buffer SRV typed/raw/structured descriptor");
      return false;
    }
    if (raw && desc.Format != DXGI_FORMAT_R32_TYPELESS &&
        desc.Format != DXGI_FORMAT_UNKNOWN) {
      WARN("D3D12Device: raw buffer SRV should use R32_TYPELESS/UNKNOWN format");
      return false;
    }
    return true;
  }
  if (resource && !IsTextureResource(resource)) {
    WARN("D3D12Device: texture SRV created for non-texture resource");
    return false;
  }
  const auto *resource_desc = GetResourceDesc(resource);
  if (resource_desc) {
    const bool resource_msaa = resource_desc->SampleDesc.Count > 1;
    const bool view_msaa =
        desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMS ||
        desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
    if (resource_msaa != view_msaa) {
      WARN("D3D12Device: SRV multisample dimension does not match resource sample count");
      return false;
    }
  }
  return true;
}

static bool
ValidateUnorderedAccessView(ID3D12Resource *resource,
                            ID3D12Resource *counter_resource,
                            const D3D12_UNORDERED_ACCESS_VIEW_DESC &desc) {
  if (!IsSupportedUavDimension(desc.ViewDimension)) {
    WARN("D3D12Device: unsupported UAV dimension ",
         uint32_t(desc.ViewDimension));
    return false;
  }
  if (desc.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
    if (resource && !IsBufferResource(resource)) {
      WARN("D3D12Device: buffer UAV created for non-buffer resource");
      return false;
    }
    const auto raw = (desc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) != 0;
    const auto structured = desc.Buffer.StructureByteStride != 0;
    const auto typed =
        desc.Format != DXGI_FORMAT_UNKNOWN &&
        !(raw && desc.Format == DXGI_FORMAT_R32_TYPELESS);
    if ((raw && structured) || (raw && typed) || (structured && typed)) {
      WARN("D3D12Device: ambiguous buffer UAV typed/raw/structured descriptor");
      return false;
    }
    if (counter_resource && !structured) {
      WARN("D3D12Device: UAV counter resource is only valid for structured buffer UAVs");
      return false;
    }
    return true;
  }
  if (resource && !IsTextureResource(resource)) {
    WARN("D3D12Device: texture UAV created for non-texture resource");
    return false;
  }
  const auto *resource_desc = GetResourceDesc(resource);
  if (resource_desc && resource_desc->SampleDesc.Count > 1) {
    WARN("D3D12Device: UAVs cannot be created for multisampled resources");
    return false;
  }
  if (desc.ViewDimension == D3D12_UAV_DIMENSION_TEXTURE3D) {
    if (resource_desc && resource_desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
      WARN("D3D12Device: 3D texture UAV created for non-3D texture resource");
      return false;
    }
    if (resource_desc && desc.Texture3D.MipSlice >= resource_desc->MipLevels) {
      WARN("D3D12Device: 3D texture UAV mip slice out of range ",
           desc.Texture3D.MipSlice);
      return false;
    }
    if (resource_desc) {
      const UINT mip_depth = GetTextureMipDepth(*resource_desc, desc.Texture3D.MipSlice);
      const UINT first_w = desc.Texture3D.FirstWSlice;
      const UINT w_size = desc.Texture3D.WSize == UINT_MAX
                              ? (first_w < mip_depth ? mip_depth - first_w : 0)
                              : desc.Texture3D.WSize;
      if (first_w >= mip_depth || w_size == 0 || w_size > mip_depth - first_w) {
        WARN("D3D12Device: invalid 3D texture UAV W slice range first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return false;
      }
      if (first_w != 0 || w_size != mip_depth) {
        // TODO(d3d12): support 3D texture UAV W-slice subranges when the
        // Metal texture view layer can preserve D3D12 depth-slice semantics.
        WARN("D3D12Device: unsupported 3D texture UAV W slice subrange first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return false;
      }
    }
  }
  if (counter_resource) {
    WARN("D3D12Device: UAV counter resource is ignored for texture UAVs");
    return false;
  }
  return true;
}

static bool
ValidateRenderTargetView(ID3D12Resource *resource,
                         const D3D12_RENDER_TARGET_VIEW_DESC &desc) {
  const auto *resource_desc = GetResourceDesc(resource);
  if (!resource_desc || !IsTextureResource(resource))
    return true;
  const bool resource_msaa = resource_desc->SampleDesc.Count > 1;
  const bool view_msaa =
      desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMS ||
      desc.ViewDimension == D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
  if (resource_msaa != view_msaa) {
    WARN("D3D12Device: RTV multisample dimension does not match resource sample count");
    return false;
  }
  return true;
}

static bool
ValidateDepthStencilView(ID3D12Resource *resource,
                         const D3D12_DEPTH_STENCIL_VIEW_DESC &desc) {
  const auto *resource_desc = GetResourceDesc(resource);
  if (!resource_desc || !IsTextureResource(resource))
    return true;
  const bool resource_msaa = resource_desc->SampleDesc.Count > 1;
  const bool view_msaa =
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMS ||
      desc.ViewDimension == D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
  if (resource_msaa != view_msaa) {
    WARN("D3D12Device: DSV multisample dimension does not match resource sample count");
    return false;
  }
  return true;
}

static void
CopyDescriptorRecord(DescriptorRecord &dst, const DescriptorRecord &src) {
  const auto magic = dst.magic;
  const auto heap_type = dst.heap_type;
  const auto shader_visible = dst.shader_visible;
  const auto cpu_handle = dst.cpu_handle;
  const auto heap_index = dst.heap_index;
  const auto heap_count = dst.heap_count;
  dst = src;
  dst.magic = magic;
  dst.heap_type = heap_type;
  dst.shader_visible = shader_visible;
  dst.cpu_handle = cpu_handle;
  dst.heap_index = heap_index;
  dst.heap_count = heap_count;
}

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
using DeviceComBase = ID3D12Device2;
#else
using DeviceComBase = ID3D12Device1;
#endif

class DeviceImpl final : public ComObjectWithInitialRef<IMTLD3D12Device,
                                                        DeviceComBase,
                                                        ID3D12DeviceConfiguration1> {
public:
  DeviceImpl(std::unique_ptr<dxmt::Device> &&device, IMTLDXGIAdapter *adapter)
      : adapter_(adapter), device_(std::move(device)) {
    enqueue_set_event_signal_ = device_->device().newSharedEvent();
    multiple_fence_wait_signal_ = device_->device().newSharedEvent();
    DXGI_ADAPTER_DESC adapter_desc = {};
    if (SUCCEEDED(adapter_->GetDesc(&adapter_desc)))
      adapter_luid_ = adapter_desc.AdapterLuid;

    if (adapter_->GetLocalD3DKMT()) {
      D3DKMT_CREATEDEVICE create = {};
      create.hAdapter = adapter_->GetLocalD3DKMT();
      if (D3DKMTCreateDevice(&create))
        WARN("D3D12Device: failed to create D3DKMT device");
      else
        local_kmt_ = create.hDevice;
    }
  }

  ~DeviceImpl() {
    if (local_kmt_) {
      D3DKMT_DESTROYDEVICE destroy = {};
      destroy.hDevice = local_kmt_;
      D3DKMTDestroyDevice(&destroy);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12Device)) {
      *ppvObject = ref(AsD3D12Device());
      return S_OK;
    }

    if (riid == __uuidof(ID3D12Device1)) {
      *ppvObject = ref(static_cast<ID3D12Device1 *>(
          static_cast<DeviceComBase *>(this)));
      return S_OK;
    }

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12Device2)) {
      *ppvObject = ref(static_cast<ID3D12Device2 *>(this));
      return S_OK;
    }
#endif

    if (riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) ||
        riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IMTLDXGIDevice) ||
        riid == __uuidof(IMTLD3D12Device)) {
      *ppvObject = ref(static_cast<IMTLD3D12Device *>(this));
      return S_OK;
    }

    if (riid == __uuidof(ID3D12DeviceConfiguration) ||
        riid == __uuidof(ID3D12DeviceConfiguration1)) {
      *ppvObject = ref(static_cast<ID3D12DeviceConfiguration1 *>(this));
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Device), riid))
      WARN("D3D12Device: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size, void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size, const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return adapter_->QueryInterface(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    if (!pAdapter)
      return DXGI_ERROR_INVALID_CALL;

    return adapter_->QueryInterface(IID_PPV_ARGS(pAdapter));
  }

  HRESULT STDMETHODCALLTYPE
  CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count, DXGI_USAGE usage,
                const DXGI_SHARED_RESOURCE *shared_resource, IDXGISurface **surface) override {
    InitReturnPtr(surface);
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE
  QueryResourceResidency(IUnknown *const *resources, DXGI_RESIDENCY *residency, UINT resource_count) override {
    if (!resources || !residency)
      return WARN_E_INVALIDARG(__func__);

    for (UINT i = 0; i < resource_count; i++)
      residency[i] = DXGI_RESIDENCY_FULLY_RESIDENT;

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override {
    if (priority < -7 || priority > 7)
      return WARN_E_INVALIDARG(__func__);

    gpu_thread_priority_ = priority;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *priority) override {
    if (!priority)
      return WARN_E_INVALIDARG(__func__);

    *priority = gpu_thread_priority_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    maximum_frame_latency_ = max_latency;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    if (!max_latency)
      return WARN_E_INVALIDARG(__func__);

    *max_latency = maximum_frame_latency_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  OfferResources(UINT resource_count, IDXGIResource *const *resources,
                 DXGI_OFFER_RESOURCE_PRIORITY priority) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT resource_count, IDXGIResource *const *resources,
                                             WINBOOL *discarded) override {
    if (discarded) {
      for (UINT i = 0; i < resource_count; i++)
        discarded[i] = FALSE;
    }

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
    if (!event)
      return WARN_E_INVALIDARG(__func__);
    auto &queue = device_->queue();
    auto signal = enqueue_set_event_signal_;
    auto value = ++enqueue_set_event_value_;
    MTLSharedEvent_setWin32EventAtValue(
        signal.handle, queue.GetSharedEventListener(), event, value);
    queue.CurrentChunk()->emitcc([signal = std::move(signal), value](
                                    ArgumentEncodingContext &enc) mutable {
      enc.signalEvent(std::move(signal), value);
    });
    queue.CommitCurrentChunk();
    return S_OK;
  }

  void STDMETHODCALLTYPE Trim() override {}

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return adapter_->GetMTLDevice();
  }

  dxmt::Device &GetDXMTDevice() override {
    return *device_;
  }

  uint64_t NextTimestampQueryValue() override {
    return ++timestamp_query_value_;
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return local_kmt_;
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(
      IDXGIFactory1 *factory, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1 *desc,
      const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
      IDXGISwapChain1 **swap_chain) override {
    InitReturnPtr(swap_chain);
    return DXGI_ERROR_UNSUPPORTED;
  }

  UINT STDMETHODCALLTYPE GetNodeCount() override {
    return 1;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandQueue(const D3D12_COMMAND_QUEUE_DESC *desc,
                                               REFIID riid, void **command_queue) override {
    return d3d12::CreateCommandQueue(static_cast<IMTLD3D12Device *>(this), desc, riid, command_queue);
  }

  HRESULT STDMETHODCALLTYPE CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE type,
                                                   REFIID riid, void **command_allocator) override {
    InitReturnPtr(command_allocator);
    if (!command_allocator)
      return E_POINTER;
    if (!IsSupportedCommandListType(type)) {
      WARN("D3D12Device: unsupported command allocator type ", type);
      return WARN_E_INVALIDARG(__func__);
    }

    auto allocator = d3d12::CreateCommandAllocator(static_cast<IMTLD3D12Device *>(this), type);
    return allocator->QueryInterface(riid, command_allocator);
  }

  HRESULT STDMETHODCALLTYPE
  CreateGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                              REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    HRESULT status = S_OK;
    auto state = d3d12::CreateGraphicsPipelineState(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    return state->QueryInterface(riid, pipeline_state);
  }

  HRESULT STDMETHODCALLTYPE
  CreateComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                             REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    HRESULT status = S_OK;
    auto state = d3d12::CreateComputePipelineState(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    return state->QueryInterface(riid, pipeline_state);
  }

#ifdef __ID3D12Device2_INTERFACE_DEFINED__
  HRESULT STDMETHODCALLTYPE
  CreatePipelineState(const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                      REFIID riid, void **pipeline_state) override {
    InitReturnPtr(pipeline_state);
    if (!pipeline_state)
      return E_POINTER;

    HRESULT status = S_OK;
    auto state = d3d12::CreatePipelineStateFromStream(
        static_cast<IMTLD3D12Device *>(this), desc, &status);
    if (!state)
      return status;
    return state->QueryInterface(riid, pipeline_state);
  }
#endif

  HRESULT STDMETHODCALLTYPE CreateCommandList(UINT node_mask, D3D12_COMMAND_LIST_TYPE type,
                                              ID3D12CommandAllocator *command_allocator,
                                              ID3D12PipelineState *initial_pipeline_state,
                                              REFIID riid, void **command_list) override {
    InitReturnPtr(command_list);

    if (node_mask > 1 || !command_allocator)
      return WARN_E_INVALIDARG(__func__);
    if (!IsSupportedCommandListType(type)) {
      WARN("D3D12Device: unsupported command list type ", type);
      return WARN_E_INVALIDARG(__func__);
    }

    auto allocator_state = dynamic_cast<d3d12::CommandAllocator *>(command_allocator);
    if (!allocator_state || allocator_state->GetCommandListType() != type)
      return WARN_E_INVALIDARG(__func__);

    HRESULT status = S_OK;
    auto list = d3d12::CreateGraphicsCommandList(
        static_cast<IMTLD3D12Device *>(this), node_mask, type,
        command_allocator, initial_pipeline_state, &status);
    if (!list)
      return status;
    return list->QueryInterface(riid, command_list);
  }

  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(D3D12_FEATURE feature, void *feature_data,
                                                UINT feature_data_size) override {
    if (!feature_data)
      return WARN_E_INVALIDARG(__func__);

    switch (feature) {
    case D3D12_FEATURE_FEATURE_LEVELS: {
      if (feature_data_size != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS *>(feature_data);
      data->MaxSupportedFeatureLevel = D3D_FEATURE_LEVEL(0);

      if (!data->pFeatureLevelsRequested || !data->NumFeatureLevels)
        return WARN_E_INVALIDARG(__func__);

      for (UINT i = 0; i < data->NumFeatureLevels; i++) {
        const auto requested = data->pFeatureLevelsRequested[i];
        if (requested <= kSupportedFeatureLevel)
          data->MaxSupportedFeatureLevel = std::max(data->MaxSupportedFeatureLevel, requested);
      }

      return S_OK;
    }
    case D3D12_FEATURE_ARCHITECTURE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_ARCHITECTURE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE *>(feature_data);
      if (data->NodeIndex != 0)
        return WARN_E_INVALIDARG(__func__);

      const bool unified_memory = GetMTLDevice().hasUnifiedMemory();
      data->TileBasedRenderer = TRUE;
      data->UMA = unified_memory;
      data->CacheCoherentUMA = unified_memory;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_1;
      data->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_1;
      data->TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED;
      data->CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      data->MaxGPUVirtualAddressBitsPerResource = 40;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_SUPPORT: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_FORMAT_SUPPORT))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_FORMAT_SUPPORT *>(feature_data);
      data->Support1 = D3D12_FORMAT_SUPPORT1_NONE;
      data->Support2 = D3D12_FORMAT_SUPPORT2_NONE;

      if (data->Format == DXGI_FORMAT_UNKNOWN) {
        data->Support1 = D3D12_FORMAT_SUPPORT1_BUFFER;
        return S_OK;
      }

      const auto &traits = GetDXGIFormatTraits(data->Format);
      if (traits.classification == DXGIFormatClass::Mask) {
        return S_OK;
      }
      if (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) {
        data->Support1 = GetD3D12TraitFormatSupport1(traits);
        data->Support2 = GetD3D12TraitFormatSupport2(traits);
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device_->device(), data->Format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(FORMAT_SUPPORT) unsupported format ",
             data->Format);
        return S_OK;
      }

      const auto caps = GetD3D12FormatCapability(device_->device(), format);
      data->Support1 = GetD3D12FormatSupport1(caps, format);
      data->Support2 = GetD3D12FormatSupport2(caps);
      return S_OK;
    }
    case D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS *>(feature_data);
      data->NumQualityLevels = 0;
      if (!data->SampleCount)
        return E_FAIL;
      if (data->Flags & ~D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE)
        return WARN_E_INVALIDARG(__func__);

      if (data->SampleCount == 1) {
        data->NumQualityLevels = 1;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format = {};
      const auto query_format =
          ResolveDepthTypelessFormatForD3D12Caps(data->Format);
      if (FAILED(MTLQueryDXGIFormat(device_->device(), query_format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(MSAA) unsupported format ",
             data->Format);
        return S_OK;
      }

      const auto caps = GetD3D12FormatCapability(device_->device(), format);
      if (IsSupportedD3D12SampleCount(data->SampleCount) &&
          (data->SampleCount == 1 || HasFormatCapability(caps, FormatCapability::MSAA)))
        data->NumQualityLevels = 1;
      return S_OK;
    }
    case D3D12_FEATURE_FORMAT_INFO: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_FORMAT_INFO))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_FORMAT_INFO *>(feature_data);
      data->PlaneCount = 0;

      if (data->Format == DXGI_FORMAT_UNKNOWN) {
        data->PlaneCount = 1;
        return S_OK;
      }
      const auto &traits = GetDXGIFormatTraits(data->Format);
      if (traits.classification == DXGIFormatClass::Mask)
        return WARN_E_INVALIDARG(__func__);
      if (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) {
        data->PlaneCount = traits.planeCount;
        return S_OK;
      }

      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device_->device(), data->Format, format))) {
        WARN("D3D12Device: CheckFeatureSupport(FORMAT_INFO) unsupported format ",
             data->Format);
        return S_OK;
      }

      data->PlaneCount = GetD3D12FormatPlaneCountForFeatureData(data->Format, format);
      return S_OK;
    }
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT *>(feature_data);
      data->MaxGPUVirtualAddressBitsPerResource = 40;
      data->MaxGPUVirtualAddressBitsPerProcess = 40;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_MODEL: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL *>(feature_data);
      if (data->HighestShaderModel >= D3D_SHADER_MODEL_6_0)
        data->HighestShaderModel = D3D_SHADER_MODEL_6_0;
      else
        data->HighestShaderModel = D3D_SHADER_MODEL_5_1;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS1: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS1))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS1 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ExpandedComputeResourceStates = TRUE;
      return S_OK;
    }
    case D3D12_FEATURE_ROOT_SIGNATURE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_ROOT_SIGNATURE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_ROOT_SIGNATURE *>(feature_data);
      if (data->HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_0)
        return S_OK;
      if (data->HighestVersion == D3D_ROOT_SIGNATURE_VERSION_1_1) {
        data->HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
        return S_OK;
      }
      return WARN_E_INVALIDARG(__func__);
    }
    case D3D12_FEATURE_ARCHITECTURE1: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_ARCHITECTURE1))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_ARCHITECTURE1 *>(feature_data);
      if (data->NodeIndex != 0)
        return WARN_E_INVALIDARG(__func__);

      const bool unified_memory = GetMTLDevice().hasUnifiedMemory();
      data->TileBasedRenderer = TRUE;
      data->UMA = unified_memory;
      data->CacheCoherentUMA = unified_memory;
      data->IsolatedMMU = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS2: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS2))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS2 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->ProgrammableSamplePositionsTier =
          D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_SHADER_CACHE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_SHADER_CACHE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_SHADER_CACHE *>(feature_data);
      data->SupportFlags = D3D12_SHADER_CACHE_SUPPORT_NONE;
      return S_OK;
    }
    case D3D12_FEATURE_COMMAND_QUEUE_PRIORITY: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_COMMAND_QUEUE_PRIORITY *>(feature_data);
      data->PriorityForTypeIsSupported =
          IsSupportedCommandListType(data->CommandListType) &&
          IsSupportedCommandQueuePriority(data->Priority);
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS3: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS3))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS3 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->CopyQueueTimestampQueriesSupported = TRUE;
      data->ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_EXISTING_HEAPS: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_EXISTING_HEAPS))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_EXISTING_HEAPS *>(feature_data);
      data->Supported = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS4: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS4))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS4 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->SharedResourceCompatibilityTier =
          D3D12_SHARED_RESOURCE_COMPATIBILITY_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_SERIALIZATION: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_SERIALIZATION))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_SERIALIZATION *>(feature_data);
      if (data->NodeIndex != 0)
        return WARN_E_INVALIDARG(__func__);
      data->HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
      return S_OK;
    }
    case D3D12_FEATURE_CROSS_NODE: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_CROSS_NODE))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_CROSS_NODE *>(feature_data);
      data->SharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
      data->AtomicShaderInstructions = FALSE;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS5: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS5 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
      data->RaytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
      return S_OK;
    }
#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
    case D3D12_FEATURE_D3D12_OPTIONS6: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS6))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS6 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->VariableShadingRateTier =
          D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS7: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS7 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->MeshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
      data->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS8: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS8))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS8 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS9: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS9))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS9 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->WaveMMATier = D3D12_WAVE_MMA_TIER_NOT_SUPPORTED;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS10: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS10))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS10 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS11: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS11))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS11 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS12: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS12))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS12 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      data->MSPrimitivesPipelineStatisticIncludesCulledPrimitives =
          D3D12_TRI_STATE_UNKNOWN;
      return S_OK;
    }
    case D3D12_FEATURE_D3D12_OPTIONS13: {
      if (feature_data_size < sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS13))
        return WARN_E_INVALIDARG(__func__);

      auto *data = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS13 *>(feature_data);
      std::memset(data, 0, sizeof(*data));
      return S_OK;
    }
#endif
    default:
      WARN("D3D12Device: CheckFeatureSupport unsupported feature ", feature);
      return DXGI_ERROR_UNSUPPORTED;
    }
  }

  HRESULT STDMETHODCALLTYPE CreateDescriptorHeap(const D3D12_DESCRIPTOR_HEAP_DESC *desc,
                                                 REFIID riid, void **descriptor_heap) override {
    InitReturnPtr(descriptor_heap);
    if (!descriptor_heap)
      return E_POINTER;
    if (!desc || desc->NumDescriptors == 0)
      return WARN_E_INVALIDARG(__func__);
    if (desc->NodeMask > 1)
      return WARN_E_INVALIDARG(__func__);

    switch (desc->Type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      if ((desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
           desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
          (desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
        return WARN_E_INVALIDARG(__func__);
      break;
    default:
      return WARN_E_INVALIDARG(__func__);
    }

    auto heap = d3d12::CreateDescriptorHeap(
        static_cast<IMTLD3D12Device *>(this), desc);
    return heap->QueryInterface(riid, descriptor_heap);
  }

  UINT STDMETHODCALLTYPE GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    switch (descriptor_heap_type) {
    case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
    case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
    case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
      return sizeof(DescriptorRecord);
    default:
      return 0;
    }
  }

  HRESULT STDMETHODCALLTYPE CreateRootSignature(UINT node_mask, const void *bytecode,
                                                SIZE_T bytecode_length, REFIID riid,
                                                void **root_signature) override {
    InitReturnPtr(root_signature);
    if (!root_signature)
      return E_POINTER;
    if (node_mask > 1 || (!bytecode && bytecode_length))
      return WARN_E_INVALIDARG(__func__);

    auto object = d3d12::CreateRootSignatureFromBlob(
        static_cast<IMTLD3D12Device *>(this),
        std::span<const std::byte>(static_cast<const std::byte *>(bytecode),
                                   bytecode_length));
    if (!object)
      return WARN_E_INVALIDARG(__func__);

    return object->QueryInterface(riid, root_signature);
  }

  void STDMETHODCALLTYPE CreateConstantBufferView(const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateConstantBufferView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::ConstantBufferView;
    if (desc) {
      if (desc->BufferLocation & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
        WARN("D3D12Device: CBV BufferLocation is not 256-byte aligned");
      if (desc->SizeInBytes & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))
        WARN("D3D12Device: CBV SizeInBytes is not 256-byte aligned");
      UINT64 offset = 0;
      auto *resource =
          LookupBufferResourceByGpuVirtualAddress(desc->BufferLocation, &offset);
      if (!resource || !resource->GetBuffer()) {
        WARN("D3D12Device: CBV BufferLocation does not resolve to a buffer resource");
      } else if (desc->SizeInBytes >
                 resource->GetResourceDesc().Width - offset) {
        WARN("D3D12Device: CBV range exceeds buffer resource size");
      }
      record->desc.cbv = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateShaderResourceView(ID3D12Resource *resource,
                                                  const D3D12_SHADER_RESOURCE_VIEW_DESC *desc,
                                                  D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateShaderResourceView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::ShaderResourceView;
    record->resource = resource;
    if (desc) {
      if (!ValidateShaderResourceView(resource, *desc))
        return;
      record->desc.srv = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateUnorderedAccessView(ID3D12Resource *resource,
                                                   ID3D12Resource *counter_resource,
                                                   const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc,
                                                   D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
        "CreateUnorderedAccessView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::UnorderedAccessView;
    record->resource = resource;
    record->counter_resource = counter_resource;
    if (desc) {
      if (!ValidateUnorderedAccessView(resource, counter_resource, *desc))
        return;
      record->desc.uav = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateRenderTargetView(ID3D12Resource *resource,
                                                const D3D12_RENDER_TARGET_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        "CreateRenderTargetView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::RenderTargetView;
    record->resource = resource;
    if (desc) {
      if (!ValidateRenderTargetView(resource, *desc))
        return;
      record->desc.rtv = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateDepthStencilView(ID3D12Resource *resource,
                                                const D3D12_DEPTH_STENCIL_VIEW_DESC *desc,
                                                D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
        "CreateDepthStencilView");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::DepthStencilView;
    record->resource = resource;
    if (desc) {
      if (!ValidateDepthStencilView(resource, *desc))
        return;
      record->desc.dsv = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CreateSampler(const D3D12_SAMPLER_DESC *desc,
                                       D3D12_CPU_DESCRIPTOR_HANDLE descriptor) override {
    auto *record = GetDescriptorRecordForWrite(
        descriptor, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, "CreateSampler");
    if (!record)
      return;
    ResetDescriptorRecord(*record);
    record->type = DescriptorRecordType::Sampler;
    if (desc) {
      record->desc.sampler = *desc;
      record->has_desc = true;
    }
  }

  void STDMETHODCALLTYPE CopyDescriptors(UINT dst_descriptor_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *dst_descriptor_range_offsets,
                                         const UINT *dst_descriptor_range_sizes,
                                         UINT src_descriptor_range_count,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE *src_descriptor_range_offsets,
                                         const UINT *src_descriptor_range_sizes,
                                         D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    if (!dst_descriptor_range_count)
      return;
    if ((dst_descriptor_range_count && !dst_descriptor_range_offsets) ||
        (src_descriptor_range_count && !src_descriptor_range_offsets)) {
      WARN("D3D12Device: CopyDescriptors called with null range offsets");
      return;
    }

    std::vector<DescriptorRecord *> destinations;
    UINT64 dst_total = 0;
    for (UINT dst_range = 0; dst_range < dst_descriptor_range_count; dst_range++) {
      const UINT dst_count = dst_descriptor_range_sizes
                                 ? dst_descriptor_range_sizes[dst_range]
                                 : 1;
      if (!dst_count)
        continue;
      auto *dst = d3d12::GetDescriptorRecordRangeFromCpuHandle(
          dst_descriptor_range_offsets[dst_range], descriptor_heap_type,
          dst_count, "CopyDescriptors destination");
      if (!dst)
        return;
      if (dst_total > UINT_MAX - dst_count) {
        WARN("D3D12Device: CopyDescriptors destination descriptor count overflow");
        return;
      }
      dst_total += dst_count;
      destinations.reserve(static_cast<size_t>(dst_total));
      for (UINT i = 0; i < dst_count; i++)
        destinations.push_back(dst + i);
    }
    if (destinations.empty())
      return;

    std::vector<DescriptorRecord> copied;
    copied.reserve(destinations.size());
    for (UINT src_range = 0; src_range < src_descriptor_range_count; src_range++) {
      const UINT src_count = src_descriptor_range_sizes
                                 ? src_descriptor_range_sizes[src_range]
                                 : 1;
      if (!src_count)
        continue;
      auto *src = d3d12::GetDescriptorRecordRangeFromCpuHandle(
          src_descriptor_range_offsets[src_range], descriptor_heap_type,
          src_count, "CopyDescriptors source");
      if (!src)
        return;
      for (UINT i = 0; i < src_count && copied.size() < destinations.size(); i++)
        copied.push_back(src[i]);
      if (copied.size() == destinations.size())
        break;
    }
    if (copied.size() != destinations.size()) {
      WARN("D3D12Device: CopyDescriptors source descriptor count is smaller "
           "than destination descriptor count");
      return;
    }

    for (size_t i = 0; i < copied.size(); i++)
      CopyDescriptorRecord(*destinations[i], copied[i]);
  }

  void STDMETHODCALLTYPE CopyDescriptorsSimple(UINT descriptor_count,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE dst_descriptor_range_offset,
                                               const D3D12_CPU_DESCRIPTOR_HANDLE src_descriptor_range_offset,
                                               D3D12_DESCRIPTOR_HEAP_TYPE descriptor_heap_type) override {
    if (!descriptor_count)
      return;
    auto *dst = d3d12::GetDescriptorRecordRangeFromCpuHandle(
        dst_descriptor_range_offset, descriptor_heap_type, descriptor_count,
        "CopyDescriptorsSimple destination");
    auto *src = d3d12::GetDescriptorRecordRangeFromCpuHandle(
        src_descriptor_range_offset, descriptor_heap_type, descriptor_count,
        "CopyDescriptorsSimple source");
    if (!dst || !src)
      return;
    std::vector<DescriptorRecord> copied(src, src + descriptor_count);
    for (UINT i = 0; i < copied.size(); i++)
      CopyDescriptorRecord(dst[i], copied[i]);
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
  GetResourceAllocationInfo(D3D12_RESOURCE_ALLOCATION_INFO *__ret, UINT visible_mask,
                            UINT resource_desc_count, const D3D12_RESOURCE_DESC *resource_descs) override {
    *__ret = GetResourceAllocationInfoImpl(visible_mask, resource_desc_count, resource_descs);
    return __ret;
  }
#else
  D3D12_RESOURCE_ALLOCATION_INFO STDMETHODCALLTYPE
  GetResourceAllocationInfo(UINT visible_mask, UINT resource_desc_count,
                            const D3D12_RESOURCE_DESC *resource_descs) override {
    return GetResourceAllocationInfoImpl(visible_mask, resource_desc_count, resource_descs);
  }
#endif

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE
  GetCustomHeapProperties(D3D12_HEAP_PROPERTIES *__ret, UINT node_mask,
                          D3D12_HEAP_TYPE heap_type) override {
    *__ret = GetCustomHeapPropertiesImpl(node_mask, heap_type);
    return __ret;
  }
#else
  D3D12_HEAP_PROPERTIES STDMETHODCALLTYPE
  GetCustomHeapProperties(UINT node_mask, D3D12_HEAP_TYPE heap_type) override {
    return GetCustomHeapPropertiesImpl(node_mask, heap_type);
  }
#endif

  HRESULT STDMETHODCALLTYPE
  CreateCommittedResource(const D3D12_HEAP_PROPERTIES *heap_properties,
                          D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
                          D3D12_RESOURCE_STATES initial_state,
                          const D3D12_CLEAR_VALUE *optimized_clear_value,
                          REFIID riid, void **resource) override {
    InitReturnPtr(resource);
    if (!resource)
      return E_POINTER;
    if (desc && desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        optimized_clear_value)
      return WARN_E_INVALIDARG(__func__);
    if (!IsValidCommittedResourceDesc(heap_properties, heap_flags, desc,
                                      initial_state))
      return WARN_E_INVALIDARG(__func__);

    auto resource_object = d3d12::CreateResource(
        static_cast<IMTLD3D12Device *>(this), heap_properties, heap_flags,
        desc, initial_state, 0, optimized_clear_value);
    return resource_object->QueryInterface(riid, resource);
  }

  HRESULT STDMETHODCALLTYPE CreateHeap(const D3D12_HEAP_DESC *desc, REFIID riid,
                                       void **heap) override {
    InitReturnPtr(heap);
    if (!heap)
      return E_POINTER;
    if (!IsValidHeapDesc(desc)) {
      LogInvalidHeapDesc("CreateHeap", desc, GetInvalidHeapDescReason(desc));
      return WARN_E_INVALIDARG(__func__);
    }

    auto heap_object = d3d12::CreateHeap(static_cast<IMTLD3D12Device *>(this),
                                         desc);
    return heap_object->QueryInterface(riid, heap);
  }

  HRESULT STDMETHODCALLTYPE CreatePlacedResource(ID3D12Heap *heap, UINT64 heap_offset,
                                                 const D3D12_RESOURCE_DESC *desc,
                                                 D3D12_RESOURCE_STATES initial_state,
                                                 const D3D12_CLEAR_VALUE *optimized_clear_value,
                                                 REFIID riid, void **resource) override {
    InitReturnPtr(resource);
    if (!resource)
      return E_POINTER;
    if (!heap || !desc)
      return WARN_E_INVALIDARG(__func__);
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        optimized_clear_value)
      return WARN_E_INVALIDARG(__func__);

    auto *heap_object = dynamic_cast<d3d12::Heap *>(heap);
    if (!heap_object)
      return WARN_E_INVALIDARG(__func__);

    if (const char *reason = GetInvalidPlacedResourceDescReason(
            *heap_object, heap_offset, desc, initial_state)) {
      LogInvalidPlacedResourceDesc(*heap_object, heap_offset, desc,
                                   initial_state, reason);
      return WARN_E_INVALIDARG(__func__);
    }

    const auto &heap_desc = heap_object->GetHeapDesc();
    auto resource_object = d3d12::CreateResource(
        static_cast<IMTLD3D12Device *>(this), &heap_desc.Properties,
        heap_desc.Flags, desc, initial_state, heap_offset,
        optimized_clear_value);
    return resource_object->QueryInterface(riid, resource);
  }

  HRESULT STDMETHODCALLTYPE CreateReservedResource(const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES initial_state,
                                                   const D3D12_CLEAR_VALUE *optimized_clear_value,
                                                   REFIID riid, void **resource) override {
    InitReturnPtr(resource);
    // TODO(d3d12): implement reserved resources together with tile mappings.
    WARN("D3D12Device: reserved resources are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE CreateSharedHandle(ID3D12DeviceChild *object,
                                               const SECURITY_ATTRIBUTES *attributes,
                                               DWORD access, const WCHAR *name,
                                               HANDLE *handle) override {
    if (handle)
      *handle = nullptr;
    // TODO(d3d12): export shareable NT handles once resource/process ownership
    // is represented outside the local COM object lifetime.
    WARN("D3D12Device: shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandle(HANDLE handle, REFIID riid, void **object) override {
    InitReturnPtr(object);
    WARN("D3D12Device: shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE OpenSharedHandleByName(const WCHAR *name, DWORD access,
                                                   HANDLE *handle) override {
    if (handle)
      *handle = nullptr;
    WARN("D3D12Device: named shared handles are unsupported");
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE MakeResident(UINT object_count, ID3D12Pageable *const *objects) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Evict(UINT object_count, ID3D12Pageable *const *objects) override {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreatePipelineLibrary(const void *blob,
                                                  SIZE_T blob_size,
                                                  REFIID iid,
                                                  void **lib) override {
    InitReturnPtr(lib);
    if (!lib)
      return E_POINTER;
    if (!blob && blob_size)
      return WARN_E_INVALIDARG(__func__);

    if (blob_size)
      WARN("D3D12Device: ignoring serialized pipeline library blob");

#ifdef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
    auto library = d3d12::CreatePipelineLibrary(
        static_cast<IMTLD3D12Device *>(this));
    return library->QueryInterface(iid, lib);
#else
    return E_NOINTERFACE;
#endif
  }

  HRESULT STDMETHODCALLTYPE SetEventOnMultipleFenceCompletion(
      ID3D12Fence *const *fences, const UINT64 *values, UINT fence_count,
      D3D12_MULTIPLE_FENCE_WAIT_FLAGS flags, HANDLE event) override {
    if (!fence_count || !fences || !values)
      return WARN_E_INVALIDARG(__func__);
    if (static_cast<UINT>(flags) &
        ~static_cast<UINT>(D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY))
      return WARN_E_INVALIDARG(__func__);

    std::vector<Fence *> wait_fences;
    wait_fences.reserve(fence_count);
    std::vector<UINT64> wait_values(values, values + fence_count);
    for (UINT i = 0; i < fence_count; i++) {
      if (!fences[i])
        return WARN_E_INVALIDARG(__func__);
      auto *fence = dynamic_cast<Fence *>(fences[i]);
      if (!fence)
        return WARN_E_INVALIDARG(__func__);
      fence->AddRefPrivate();
      wait_fences.push_back(fence);
    }

    auto completed = [wait_fences = wait_fences,
                      wait_values = wait_values, flags]() {
      if (flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY) {
        for (size_t i = 0; i < wait_fences.size(); i++) {
          if (wait_fences[i]->GetCompletedValue() >= wait_values[i])
            return true;
        }
        return false;
      }

      for (size_t i = 0; i < wait_fences.size(); i++) {
        if (wait_fences[i]->GetCompletedValue() < wait_values[i])
          return false;
      }
      return true;
    };
    auto release_wait_fences = [&]() {
      for (auto *fence : wait_fences)
        fence->ReleasePrivate();
      wait_fences.clear();
    };

    if (completed()) {
      if (event) {
        auto signal = device_->device().newSharedEvent();
        MTLSharedEvent_setWin32EventAtValue(
            signal.handle, device_->queue().GetSharedEventListener(), event, 1);
        signal.signalValue(1);
      }
      release_wait_fences();
      return S_OK;
    }

    if (!event) {
      while (!completed())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      release_wait_fences();
      return S_OK;
    }

    auto signal = multiple_fence_wait_signal_;
    auto signal_value = ++multiple_fence_wait_value_;
    MTLSharedEvent_setWin32EventAtValue(
        signal.handle, device_->queue().GetSharedEventListener(), event,
        signal_value);

    dxmt::thread([wait_fences = std::move(wait_fences),
                  wait_values = std::move(wait_values), flags,
                  signal = std::move(signal), signal_value]() mutable {
      auto completed = [&]() {
        if (flags & D3D12_MULTIPLE_FENCE_WAIT_FLAG_ANY) {
          for (size_t i = 0; i < wait_fences.size(); i++) {
            if (wait_fences[i]->GetCompletedValue() >= wait_values[i])
              return true;
          }
          return false;
        }

        for (size_t i = 0; i < wait_fences.size(); i++) {
          if (wait_fences[i]->GetCompletedValue() < wait_values[i])
            return false;
        }
        return true;
      };

      while (!completed())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      signal.signalValue(signal_value);
      for (auto *fence : wait_fences)
        fence->ReleasePrivate();
    }).detach();

    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  SetResidencyPriority(UINT object_count, ID3D12Pageable *const *objects,
                       const D3D12_RESIDENCY_PRIORITY *priorities) override {
    if (object_count && (!objects || !priorities))
      return WARN_E_INVALIDARG(__func__);
    for (UINT i = 0; i < object_count; i++) {
      if (!objects[i])
        return WARN_E_INVALIDARG(__func__);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateFence(UINT64 initial_value, D3D12_FENCE_FLAGS flags,
                                        REFIID riid, void **fence) override {
    InitReturnPtr(fence);
    if (!fence)
      return S_FALSE;
    if (flags & ~D3D12_FENCE_FLAG_SHARED) {
      WARN("D3D12Device::CreateFence: unsupported fence flags ", flags);
      return E_NOTIMPL;
    }

    auto fence_object = d3d12::CreateFence(static_cast<IMTLD3D12Device *>(this), initial_value, flags);
    return fence_object->QueryInterface(riid, fence);
  }

  HRESULT STDMETHODCALLTYPE GetDeviceRemovedReason() override {
    return S_OK;
  }

  void STDMETHODCALLTYPE GetCopyableFootprints(const D3D12_RESOURCE_DESC *desc,
                                               UINT first_sub_resource,
                                               UINT sub_resource_count, UINT64 base_offset,
                                               D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts,
                                               UINT *row_count, UINT64 *row_size,
                                               UINT64 *total_bytes) override {
    GetCopyableFootprintsImpl(desc, first_sub_resource, sub_resource_count,
                              base_offset, layouts, row_count, row_size,
                              total_bytes);
  }

    HRESULT STDMETHODCALLTYPE CreateQueryHeap(const D3D12_QUERY_HEAP_DESC *desc,
                                              REFIID riid, void **heap) override {
      InitReturnPtr(heap);
      if (!heap)
        return E_POINTER;
      if (!desc || desc->Count == 0 || desc->NodeMask > 1)
        return WARN_E_INVALIDARG(__func__);

      switch (desc->Type) {
      case D3D12_QUERY_HEAP_TYPE_OCCLUSION:
      case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
      case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
      case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS:
        break;
      default:
        return WARN_E_INVALIDARG(__func__);
      }

      auto query_heap = d3d12::CreateQueryHeap(
          static_cast<IMTLD3D12Device *>(this), desc);
      return query_heap->QueryInterface(riid, heap);
    }

  HRESULT STDMETHODCALLTYPE SetStablePowerState(WINBOOL enable) override {
    if (enable)
      WARN("D3D12Device: stable power state is unsupported");
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE CreateCommandSignature(const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                                   ID3D12RootSignature *root_signature,
                                                   REFIID riid, void **command_signature) override {
    InitReturnPtr(command_signature);
    if (!command_signature)
      return E_POINTER;
    if (!desc || !desc->NumArgumentDescs || !desc->pArgumentDescs ||
        !desc->ByteStride || desc->NodeMask > 1)
      return WARN_E_INVALIDARG(__func__);

    UINT min_stride = 0;
    UINT operation_count = 0;
    for (UINT i = 0; i < desc->NumArgumentDescs; i++) {
      const auto &argument = desc->pArgumentDescs[i];
      const auto argument_size = IndirectArgumentByteSize(argument);
      if (!argument_size) {
        WARN("D3D12Device::CreateCommandSignature: unsupported indirect "
             "argument type ",
             argument.Type);
        return E_NOTIMPL;
      }
      min_stride += argument_size;
      if (IsIndirectOperationArgument(argument.Type)) {
        if (++operation_count > 1 || i + 1 != desc->NumArgumentDescs) {
          WARN("D3D12Device::CreateCommandSignature: unsupported indirect "
               "operation layout");
          return WARN_E_INVALIDARG(__func__);
        }
      }
      switch (argument.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        if (!root_signature)
          return WARN_E_INVALIDARG(__func__);
        break;
      default:
        break;
      }
    }
    if (!operation_count) {
      WARN("D3D12Device::CreateCommandSignature: missing indirect operation");
      return WARN_E_INVALIDARG(__func__);
    }
    if (desc->ByteStride < min_stride)
      return WARN_E_INVALIDARG(__func__);

    auto signature = d3d12::CreateCommandSignature(
        static_cast<IMTLD3D12Device *>(this), desc, root_signature);
    return signature->QueryInterface(riid, command_signature);
  }

  void STDMETHODCALLTYPE GetResourceTiling(ID3D12Resource *resource, UINT *total_tile_count,
                                           D3D12_PACKED_MIP_INFO *packed_mip_info,
                                           D3D12_TILE_SHAPE *standard_tile_shape,
                                           UINT *sub_resource_tiling_count,
                                           UINT first_sub_resource_tiling,
                                           D3D12_SUBRESOURCE_TILING *sub_resource_tilings) override {
    // TODO(d3d12): return real tiling once reserved/tiled resources are
    // implemented. The device advertises TiledResourcesTier NOT_SUPPORTED.
    if (resource)
      WARN("D3D12Device: resource tiling is unsupported");
    if (total_tile_count)
      *total_tile_count = 0;
    if (sub_resource_tiling_count)
      *sub_resource_tiling_count = 0;
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  LUID *STDMETHODCALLTYPE GetAdapterLuid(LUID *__ret) override {
    *__ret = adapter_luid_;
    return __ret;
  }
#else
  LUID STDMETHODCALLTYPE GetAdapterLuid() override {
    return adapter_luid_;
  }
#endif

private:
  ID3D12Device *AsD3D12Device() {
    return static_cast<ID3D12Device *>(static_cast<DeviceComBase *>(this));
  }

  D3D12_RESOURCE_ALLOCATION_INFO
  GetResourceAllocationInfoImpl(UINT visible_mask, UINT resource_desc_count,
                                const D3D12_RESOURCE_DESC *resource_descs) const {
    D3D12_RESOURCE_ALLOCATION_INFO info = {};
    info.Alignment = 1;

    if (!resource_desc_count || !resource_descs)
      return info;

    for (UINT i = 0; i < resource_desc_count; i++) {
      const auto &desc = resource_descs[i];
      UINT64 alignment = desc.Alignment;
      if (!alignment)
        alignment = desc.SampleDesc.Count > 1
                        ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                        : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

      UINT64 size = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
      if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        size = desc.Width;
      } else {
        const UINT mip_levels = desc.MipLevels ? desc.MipLevels : 1;
        const UINT plane_count = GetD3D12FormatPlaneCount(desc.Format);
        const UINT subresources = mip_levels *
                                  (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                       ? 1
                                       : desc.DepthOrArraySize) *
                                  plane_count;
        UINT64 total_bytes = 0;
        GetCopyableFootprintsImpl(&desc, 0, subresources, 0, nullptr, nullptr,
                                  nullptr, &total_bytes, false);
        size = total_bytes;
      }
      if (!info.Alignment)
        info.Alignment = alignment;
      else
        info.Alignment = std::max(info.Alignment, alignment);
      info.SizeInBytes += Align(size, alignment);
    }

    return info;
  }

  D3D12_HEAP_PROPERTIES
  GetCustomHeapPropertiesImpl(UINT node_mask, D3D12_HEAP_TYPE heap_type) const {
    D3D12_HEAP_PROPERTIES properties = {};
    properties.Type = heap_type;
    properties.CreationNodeMask = node_mask ? node_mask : 1;
    properties.VisibleNodeMask = node_mask ? node_mask : 1;
    properties.CPUPageProperty = IsCpuVisibleHeap(heap_type) ? D3D12_CPU_PAGE_PROPERTY_WRITE_BACK
                                                             : D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    properties.MemoryPoolPreference = IsCpuVisibleHeap(heap_type) ? D3D12_MEMORY_POOL_L0
                                                                  : D3D12_MEMORY_POOL_L1;
    return properties;
  }

  const char *GetInvalidHeapDescReason(const D3D12_HEAP_DESC *desc) const {
    if (!desc)
      return "null-desc";
    if (desc->SizeInBytes == 0)
      return "zero-size";
    if (desc->Alignment > D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
      return "alignment-too-large";
    if (desc->Alignment &&
        desc->Alignment != D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT &&
        desc->Alignment != D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT)
      return "unsupported-alignment";
    if (desc->Properties.CreationNodeMask > 1)
      return "creation-node-mask";
    if (desc->Properties.VisibleNodeMask > 1)
      return "visible-node-mask";
    if (desc->Properties.CreationNodeMask && desc->Properties.VisibleNodeMask &&
        (desc->Properties.CreationNodeMask & desc->Properties.VisibleNodeMask) == 0)
      return "disjoint-node-masks";
    if (desc->Properties.Type == D3D12_HEAP_TYPE_CUSTOM) {
      switch (desc->Properties.CPUPageProperty) {
      case D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE:
      case D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE:
      case D3D12_CPU_PAGE_PROPERTY_WRITE_BACK:
        break;
      default:
        return "custom-cpu-page-property";
      }
      switch (desc->Properties.MemoryPoolPreference) {
      case D3D12_MEMORY_POOL_UNKNOWN:
      case D3D12_MEMORY_POOL_L0:
      case D3D12_MEMORY_POOL_L1:
        break;
      default:
        return "custom-memory-pool";
      }
      if (desc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
        return "custom-allow-display";
      if ((desc->Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER) &&
          desc->Properties.CPUPageProperty !=
              D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE)
        return "custom-cross-adapter-cpu-visible";
      return nullptr;
    }
    if (desc->Properties.CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN)
      return "typed-cpu-page-property";
    if (desc->Properties.MemoryPoolPreference != D3D12_MEMORY_POOL_UNKNOWN)
      return "typed-memory-pool";
    if (desc->Flags & D3D12_HEAP_FLAG_ALLOW_DISPLAY)
      return "allow-display";
    if (desc->Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)
      return "shared-cross-adapter";
    switch (desc->Properties.Type) {
    case D3D12_HEAP_TYPE_DEFAULT:
    case D3D12_HEAP_TYPE_UPLOAD:
    case D3D12_HEAP_TYPE_READBACK:
      return nullptr;
    default:
      return "heap-type";
    }
  }

  void LogInvalidHeapDesc(const char *where, const D3D12_HEAP_DESC *desc,
                          const char *reason) const {
    if (!desc) {
      WARN("D3D12 diagnostic: invalid heap desc",
           " where=", where, " reason=", reason ? reason : "unknown");
      return;
    }

    WARN("D3D12 diagnostic: invalid heap desc",
         " where=", where,
         " reason=", reason ? reason : "unknown",
         " size=", uint64_t(desc->SizeInBytes),
         " alignment=", uint64_t(desc->Alignment),
         " type=", uint32_t(desc->Properties.Type),
         " cpuPage=", uint32_t(desc->Properties.CPUPageProperty),
         " memoryPool=", uint32_t(desc->Properties.MemoryPoolPreference),
         " creationNode=", uint32_t(desc->Properties.CreationNodeMask),
         " visibleNode=", uint32_t(desc->Properties.VisibleNodeMask),
         " flags=", uint32_t(desc->Flags));
  }

  bool IsValidHeapDesc(const D3D12_HEAP_DESC *desc) const {
    if (GetInvalidHeapDescReason(desc))
      return false;
    return true;
  }

  bool IsValidHeapProperties(const D3D12_HEAP_PROPERTIES *properties) const {
    if (!properties)
      return false;

    D3D12_HEAP_DESC heap_desc = {};
    heap_desc.SizeInBytes = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap_desc.Properties = *properties;
    return IsValidHeapDesc(&heap_desc);
  }

  bool IsValidInitialState(const D3D12_HEAP_PROPERTIES &properties,
                           D3D12_RESOURCE_STATES initial_state) const {
    if (properties.Type == D3D12_HEAP_TYPE_UPLOAD) {
      return initial_state == D3D12_RESOURCE_STATE_GENERIC_READ ||
             initial_state == D3D12_RESOURCE_STATE_COMMON ||
             initial_state == D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    if (properties.Type == D3D12_HEAP_TYPE_READBACK) {
      return initial_state == D3D12_RESOURCE_STATE_COPY_DEST ||
             initial_state == D3D12_RESOURCE_STATE_COMMON;
    }
    if ((initial_state & D3D12_RESOURCE_STATE_RENDER_TARGET) &&
        initial_state != D3D12_RESOURCE_STATE_RENDER_TARGET)
      return false;
    if ((initial_state & D3D12_RESOURCE_STATE_DEPTH_WRITE) &&
        initial_state != D3D12_RESOURCE_STATE_DEPTH_WRITE)
      return false;
    return true;
  }

  bool IsValidResourceStateForDesc(const D3D12_RESOURCE_DESC &desc,
                                   D3D12_RESOURCE_STATES initial_state) const {
    if ((initial_state & D3D12_RESOURCE_STATE_RENDER_TARGET) &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
      return false;
    if ((initial_state & D3D12_RESOURCE_STATE_DEPTH_WRITE) &&
        !(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
      return false;
    return true;
  }

  bool IsValidResourceAlignment(const D3D12_RESOURCE_DESC &desc) const {
    switch (desc.Alignment) {
    case 0:
    case D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT:
      return true;
    case D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT:
      return desc.SampleDesc.Count > 1;
    case D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT: {
      if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
          desc.SampleDesc.Count > 1 ||
          (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
        return false;
      const auto allocation_info = GetResourceAllocationInfoImpl(1, 1, &desc);
      return allocation_info.SizeInBytes <= D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    default:
      return false;
    }
  }

  bool IsValidCommittedResourceDesc(
      const D3D12_HEAP_PROPERTIES *heap_properties,
      D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
      D3D12_RESOURCE_STATES initial_state) const {
    if (!IsValidHeapProperties(heap_properties) || !desc)
      return false;
    if (!d3d12::IsSupportedResourceDesc(*desc))
      return false;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        IsBcFormat(device_->device(), desc->Format))
      return false;
    if (!IsValidInitialState(*heap_properties, initial_state))
      return false;
    if (!IsValidResourceStateForDesc(*desc, initial_state))
      return false;
    if (!IsValidResourceAlignment(*desc))
      return false;
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
      return false;

    const auto heap_type = d3d12::GetHeapType(*heap_properties);
    if (!IsValidCrossAdapterTextureDesc(*heap_properties, heap_flags, *desc))
      return false;
    if (HasInvalidCpuVisibleBufferFlags(*desc, heap_type))
      return false;
    if (heap_type == D3D12_HEAP_TYPE_UPLOAD ||
        heap_type == D3D12_HEAP_TYPE_READBACK) {
      if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
          desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
        return false;
      if (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                         D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
        return false;
    }

    return true;
  }

  bool IsValidPlacedResourceDesc(const d3d12::Heap &heap, UINT64 heap_offset,
                                 const D3D12_RESOURCE_DESC *desc,
                                 D3D12_RESOURCE_STATES initial_state) const {
    return !GetInvalidPlacedResourceDescReason(heap, heap_offset, desc,
                                               initial_state);
  }

  const char *GetInvalidPlacedResourceDescReason(
      const d3d12::Heap &heap, UINT64 heap_offset,
      const D3D12_RESOURCE_DESC *desc,
      D3D12_RESOURCE_STATES initial_state) const {
    if (!desc)
      return "null-desc";
    if (!d3d12::IsSupportedResourceDesc(*desc))
      return "unsupported-desc";
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
        IsBcFormat(device_->device(), desc->Format))
      return "bc-texture1d";
    if (!IsValidInitialState(heap.GetHeapDesc().Properties, initial_state))
      return "initial-state";
    if (!IsValidResourceStateForDesc(*desc, initial_state))
      return "state-for-desc";
    if (!IsValidResourceAlignment(*desc))
      return "resource-alignment";
    if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER &&
        (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS))
      return "buffer-simultaneous-access";

    const auto allocation_info = GetResourceAllocationInfoImpl(1, 1, desc);
    if (allocation_info.SizeInBytes == 0)
      return "zero-allocation-size";
    if (heap_offset % allocation_info.Alignment)
      return "heap-offset-alignment";
    if (heap_offset > heap.GetHeapDesc().SizeInBytes ||
        allocation_info.SizeInBytes > heap.GetHeapDesc().SizeInBytes - heap_offset)
      return "heap-range";

    const auto heap_type = heap.GetHeapType();
    if (!IsValidCrossAdapterTextureDesc(heap.GetHeapDesc().Properties,
                                        heap.GetHeapDesc().Flags, *desc))
      return "cross-adapter-texture";
    if (HasInvalidCpuVisibleBufferFlags(*desc, heap_type))
      return "cpu-visible-buffer-flags";
    if ((heap_type == D3D12_HEAP_TYPE_UPLOAD ||
         heap_type == D3D12_HEAP_TYPE_READBACK) &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        desc->Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
      return "cpu-visible-texture-layout";

    const auto heap_flags = heap.GetHeapDesc().Flags;
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_BUFFERS) &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return "heap-deny-buffer";
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES) &&
        (desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
      return "heap-deny-rt-ds";
    if ((heap_flags & D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES) &&
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        !(desc->Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                         D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
      return "heap-deny-non-rt-ds";

    return nullptr;
  }

  void LogInvalidPlacedResourceDesc(const d3d12::Heap &heap,
                                    UINT64 heap_offset,
                                    const D3D12_RESOURCE_DESC *desc,
                                    D3D12_RESOURCE_STATES initial_state,
                                    const char *reason) const {
    const auto &heap_desc = heap.GetHeapDesc();
    D3D12_RESOURCE_ALLOCATION_INFO allocation_info = {};
    if (desc)
      allocation_info = GetResourceAllocationInfoImpl(1, 1, desc);

    ERR("D3D12 diagnostic: invalid placed resource desc",
        " reason=", reason ? reason : "unknown",
        " heapOffset=", uint64_t(heap_offset),
        " allocationSize=", uint64_t(allocation_info.SizeInBytes),
        " allocationAlignment=", uint64_t(allocation_info.Alignment),
        " heapSize=", uint64_t(heap_desc.SizeInBytes),
        " heapAlignment=", uint64_t(heap_desc.Alignment),
        " heapType=", uint32_t(heap_desc.Properties.Type),
        " heapCpuPage=", uint32_t(heap_desc.Properties.CPUPageProperty),
        " heapMemoryPool=", uint32_t(heap_desc.Properties.MemoryPoolPreference),
        " heapFlags=", uint32_t(heap_desc.Flags),
        " descDimension=", desc ? uint32_t(desc->Dimension) : 0,
        " descAlignment=", desc ? uint64_t(desc->Alignment) : 0,
        " descWidth=", desc ? uint64_t(desc->Width) : 0,
        " descHeight=", desc ? uint32_t(desc->Height) : 0,
        " descDepthOrArraySize=", desc ? uint32_t(desc->DepthOrArraySize) : 0,
        " descMipLevels=", desc ? uint32_t(desc->MipLevels) : 0,
        " descFormat=", desc ? uint32_t(desc->Format) : 0,
        " descSampleCount=", desc ? uint32_t(desc->SampleDesc.Count) : 0,
        " descSampleQuality=", desc ? uint32_t(desc->SampleDesc.Quality) : 0,
        " descLayout=", desc ? uint32_t(desc->Layout) : 0,
        " descFlags=", desc ? uint32_t(desc->Flags) : 0,
        " initialState=", uint32_t(initial_state));
  }

  void GetCopyableFootprintsImpl(
      const D3D12_RESOURCE_DESC *desc, UINT first_sub_resource,
      UINT sub_resource_count, UINT64 base_offset,
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *row_count,
      UINT64 *row_size, UINT64 *total_bytes,
      bool strict_resource_desc = true) const {
    UINT64 offset = 0;
    UINT64 total = 0;

    auto set_invalid = [&]() {
      for (UINT i = 0; i < sub_resource_count; i++) {
        if (layouts) {
          layouts[i].Offset = UINT64_MAX;
          layouts[i].Footprint.Format = static_cast<DXGI_FORMAT>(~0u);
          layouts[i].Footprint.Width = ~0u;
          layouts[i].Footprint.Height = ~0u;
          layouts[i].Footprint.Depth = ~0u;
          layouts[i].Footprint.RowPitch = ~0u;
        }
        if (row_count)
          row_count[i] = ~0u;
        if (row_size)
          row_size[i] = UINT64_MAX;
      }
      if (total_bytes)
        *total_bytes = UINT64_MAX;
    };

    if (!desc || sub_resource_count == 0) {
      if (total_bytes)
        *total_bytes = 0;
      return;
    }

    if (!(strict_resource_desc ? d3d12::IsSupportedResourceDesc(*desc)
                               : IsValidCopyableFootprintDesc(*desc))) {
      set_invalid();
      return;
    }

    const auto &traits = GetDXGIFormatTraits(desc->Format);
    const bool trait_layout_format =
        desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        HasD3D12TraitFootprintLayout(traits);

    MTL_DXGI_FORMAT_DESC format = {};
    if (desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
        !trait_layout_format &&
        FAILED(MTLQueryDXGIFormat(device_->device(), desc->Format, format))) {
      set_invalid();
      return;
    }

    const UINT mip_levels = desc->MipLevels ? desc->MipLevels : 1;
    const UINT array_size =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? 1
            : desc->DepthOrArraySize;
    const UINT plane_count =
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
            ? 1
            : GetD3D12FormatPlaneCount(desc->Format);
    const UINT subresources_per_plane = mip_levels * array_size;
    const UINT max_subresources = subresources_per_plane * plane_count;
    if (first_sub_resource >= max_subresources ||
        sub_resource_count > max_subresources - first_sub_resource) {
      set_invalid();
      return;
    }
    for (UINT i = 0; i < sub_resource_count; i++) {
      const UINT subresource = first_sub_resource + i;
      const UINT plane =
          subresources_per_plane ? subresource / subresources_per_plane : 0;
      const UINT plane_subresource =
          subresources_per_plane ? subresource % subresources_per_plane
                                 : subresource;
      D3D12_SUBRESOURCE_FOOTPRINT footprint = {};
      UINT rows = 1;
      UINT64 unpadded_row_size = desc->Width;
      UINT64 subresource_size = desc->Width;

      footprint.Format = GetD3D12FootprintFormat(desc->Format, plane);

      if (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        footprint.Width = static_cast<UINT>(desc->Width);
        footprint.Height = 1;
        footprint.Depth = 1;
        footprint.RowPitch =
            static_cast<UINT>(Align(desc->Width, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        subresource_size = desc->Width;
      } else {
        if (plane >= plane_count) {
          WARN("D3D12Device: GetCopyableFootprints subresource plane out of range subresource=",
               subresource, " plane=", plane, " plane_count=", plane_count);
          break;
        }

        const UINT mip_slice = SubresourceMipSlice(plane_subresource, mip_levels);
        UINT subsample_x_log2 = 0;
        UINT subsample_y_log2 = 0;
        GetD3D12FormatSubsampleLog2(desc->Format, plane, subsample_x_log2,
                                    subsample_y_log2);
        const UINT64 width =
            Align(MipSize(desc->Width, mip_slice + subsample_x_log2),
                  GetD3D12FormatBlockWidth(format));
        const UINT height = static_cast<UINT>(
            Align(MipSize(desc->Height, mip_slice + subsample_y_log2),
                  GetD3D12FormatBlockHeight(format)));
        const UINT depth = desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                               ? static_cast<UINT>(MipSize(desc->DepthOrArraySize, mip_slice))
                               : 1;
        const UINT block_width = GetD3D12FormatBlockWidth(format);
        const UINT block_height = GetD3D12FormatBlockHeight(format);
        const UINT element_size =
            GetD3D12FormatPlaneElementSize(desc->Format, plane, format);
        const UINT num_planes = std::max(1u, plane_count);

        footprint.Width = static_cast<UINT>(width);
        footprint.Height = height;
        footprint.Depth = depth;

        rows = std::max(1u, height / block_height);
        unpadded_row_size = (width / block_width) * element_size;

        UINT64 row_alignment = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        if (num_planes == 2 || desc->Format == DXGI_FORMAT_420_OPAQUE)
          row_alignment *= 2;
        footprint.RowPitch = static_cast<UINT>(
            Align(unpadded_row_size, row_alignment));
        const UINT64 row_span =
            rows > 1 ? (rows - 1) * UINT64(footprint.RowPitch) + unpadded_row_size
                     : unpadded_row_size;
        const UINT64 slice_pitch =
            Align(row_span, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT * num_planes);
        subresource_size =
            depth > 1 ? (depth - 1) * slice_pitch + row_span : row_span;
      }

      if (layouts) {
        layouts[i].Offset = base_offset + offset;
        layouts[i].Footprint = footprint;
      }
      if (row_count)
        row_count[i] = rows;
      if (row_size)
        row_size[i] = unpadded_row_size;

      total = offset + subresource_size;
      offset = Align(total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    }

    if (total_bytes)
      *total_bytes = total;
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_DEVICE_CONFIGURATION_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DEVICE_CONFIGURATION_DESC *__ret) override {
    *__ret = GetDeviceConfigurationDesc();
    return __ret;
  }
#else
  D3D12_DEVICE_CONFIGURATION_DESC STDMETHODCALLTYPE GetDesc() override {
    return GetDeviceConfigurationDesc();
  }
#endif

  HRESULT STDMETHODCALLTYPE GetEnabledExperimentalFeatures(GUID *guids,
                                                           UINT guid_count) override {
    if (guid_count && !guids)
      return WARN_E_INVALIDARG(__func__);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SerializeVersionedRootSignature(
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob,
      ID3DBlob **error_blob) override {
    return d3d12::SerializeVersionedRootSignature(desc, blob, error_blob);
  }

  HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializer(
      const void *blob, SIZE_T size, REFIID riid, void **deserializer) override {
    if (!blob && size)
      return WARN_E_INVALIDARG(__func__);
    return d3d12::CreateRootSignatureDeserializer(
        std::span<const std::byte>(static_cast<const std::byte *>(blob), size),
        riid, deserializer);
  }

  HRESULT STDMETHODCALLTYPE
  CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *library_blob, SIZE_T size, LPCWSTR subobject_name,
      REFIID riid, void **deserializer) override {
    if (!library_blob && size)
      return WARN_E_INVALIDARG(__func__);
    return d3d12::CreateRootSignatureDeserializerFromSubobjectInLibrary(
        std::span<const std::byte>(
            static_cast<const std::byte *>(library_blob), size),
        subobject_name, riid, deserializer);
  }

  static D3D12_DEVICE_CONFIGURATION_DESC GetDeviceConfigurationDesc() {
    D3D12_DEVICE_CONFIGURATION_DESC desc = {};
    desc.Flags = D3D12_DEVICE_FLAG_NONE;
    desc.SDKVersion = kAgilitySdkVersion;
    return desc;
  }

  Com<IMTLDXGIAdapter> adapter_;
  std::unique_ptr<dxmt::Device> device_;
  ComPrivateData private_data_;
  WMT::Reference<WMT::SharedEvent> enqueue_set_event_signal_;
  WMT::Reference<WMT::SharedEvent> multiple_fence_wait_signal_;
  D3DKMT_HANDLE local_kmt_ = 0;
  LUID adapter_luid_ = {};
  UINT maximum_frame_latency_ = 3;
  INT gpu_thread_priority_ = 0;
  uint64_t timestamp_query_value_ = 0;
  uint64_t enqueue_set_event_value_ = 0;
  uint64_t multiple_fence_wait_value_ = 0;
  std::string name_;
};

} // namespace

Com<IMTLD3D12Device>
CreateD3D12Device(std::unique_ptr<dxmt::Device> &&device, IMTLDXGIAdapter *adapter) {
  return Com<IMTLD3D12Device>::transfer(new DeviceImpl(std::move(device), adapter));
}

} // namespace dxmt::d3d12
