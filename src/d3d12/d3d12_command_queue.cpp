#include "d3d12_command_queue.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_fence.hpp"
#include "d3d12_query.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_root_signature.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "dxmt_hud_state.hpp"
#include "dxmt_info.hpp"
#include "dxmt_presenter.hpp"
#include "dxmt_sampler.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include "util_win32_compat.h"
#include "wsi_window.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cfloat>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace dxmt::d3d12 {
namespace {

static bool
IsSupportedQueueType(D3D12_COMMAND_LIST_TYPE type) {
  return type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
         type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
         type == D3D12_COMMAND_LIST_TYPE_COPY;
}

static bool
IsSupportedQueuePriority(INT priority) {
  return priority == D3D12_COMMAND_QUEUE_PRIORITY_NORMAL ||
         priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
}

static bool
IsSupportedQueueFlags(D3D12_COMMAND_QUEUE_FLAGS flags) {
  return (flags & ~D3D12_COMMAND_QUEUE_FLAG_DISABLE_GPU_TIMEOUT) == 0;
}

static Resource *
GetResource(ID3D12Resource *resource) {
  return dynamic_cast<Resource *>(resource);
}

static PipelineState *
GetPipelineState(ID3D12PipelineState *pipeline_state) {
  return dynamic_cast<PipelineState *>(pipeline_state);
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
ReadBufferBytes(ID3D12Resource *resource, UINT64 offset, void *dst,
                UINT64 size, const char *context) {
  auto *d3d12_resource = GetResource(resource);
  if (!d3d12_resource || !d3d12_resource->GetBufferAllocation() || !dst)
    return false;
  if (offset > d3d12_resource->GetResourceDesc().Width ||
      size > d3d12_resource->GetResourceDesc().Width - offset) {
    WARN("D3D12CommandQueue: ", context, " read exceeds buffer bounds");
    return false;
  }
  auto *mapped = d3d12_resource->GetBufferAllocation()->mappedMemory(0);
  if (!mapped) {
    WARN("D3D12CommandQueue: ", context,
         " requires a CPU-visible buffer for initial support");
    return false;
  }
  std::memcpy(dst,
              static_cast<const char *>(mapped) +
                  d3d12_resource->GetHeapOffset() + offset,
              size);
  return true;
}

static bool
ValidateBufferRange(Resource *resource, UINT64 offset, UINT64 size,
                    const char *context) {
  if (!resource || !resource->GetBufferAllocation())
    return false;
  const UINT64 width = resource->GetResourceDesc().Width;
  if (offset > width || size > width - offset) {
    WARN("D3D12CommandQueue: ", context, " exceeds buffer bounds");
    return false;
  }
  return true;
}

enum class DirectIndirectOperation {
  None,
  Draw,
  DrawIndexed,
  Dispatch,
};

static DirectIndirectOperation
GetDirectIndirectOperation(
    const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &arguments) {
  if (arguments.size() != 1)
    return DirectIndirectOperation::None;

  switch (arguments[0].Type) {
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
    return DirectIndirectOperation::Draw;
  case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
    return DirectIndirectOperation::DrawIndexed;
  case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
    return DirectIndirectOperation::Dispatch;
  default:
    return DirectIndirectOperation::None;
  }
}

static RootSignature *
GetRootSignature(ID3D12RootSignature *root_signature) {
  return dynamic_cast<RootSignature *>(root_signature);
}

static std::optional<WMTPrimitiveType>
GetPrimitiveType(D3D12_PRIMITIVE_TOPOLOGY topology) {
  switch (topology) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    return WMTPrimitiveTypePoint;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    return WMTPrimitiveTypeLine;
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    return WMTPrimitiveTypeLineStrip;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    return WMTPrimitiveTypeTriangle;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    return WMTPrimitiveTypeTriangleStrip;
  default:
    return std::nullopt;
  }
}

static WMTPixelFormat
GetSwapChainPixelFormat(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return WMTPixelFormatBGRA8Unorm_sRGB;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM:
    return WMTPixelFormatBGRA8Unorm;
  case DXGI_FORMAT_R10G10B10A2_UNORM:
    return WMTPixelFormatRGB10A2Unorm;
  case DXGI_FORMAT_R16G16B16A16_FLOAT:
    return WMTPixelFormatRGBA16Float;
  default:
    return WMTPixelFormatInvalid;
  }
}

static WMTColorSpace
GetSwapChainColorSpace(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? WMTColorSpaceSRGBLinear
                                                  : WMTColorSpaceSRGB;
}

static WMTColorSpace
GetD3D12SwapChainColorSpace(DXGI_COLOR_SPACE_TYPE color_space) {
  switch (color_space) {
  case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    return WMTColorSpaceSRGB;
  case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
    return WMTColorSpaceSRGBLinear;
  default:
    return WMTColorSpaceInvalid;
  }
}

static bool
IsSupportedD3D12SwapChainColorSpace(DXGI_COLOR_SPACE_TYPE color_space) {
  const WMTColorSpace wmt_color_space =
      GetD3D12SwapChainColorSpace(color_space);
  return wmt_color_space != WMTColorSpaceInvalid &&
         CGColorSpace_checkColorSpaceSupported(wmt_color_space);
}

static WMTColorSpace
GetD3D12SwapChainLayerColorSpace(DXGI_FORMAT format,
                                 DXGI_COLOR_SPACE_TYPE color_space) {
  return color_space == DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709
             ? GetSwapChainColorSpace(format)
             : GetD3D12SwapChainColorSpace(color_space);
}

static constexpr UINT D3D12SupportedPresentFlags =
    DXGI_PRESENT_TEST | DXGI_PRESENT_ALLOW_TEARING;

static constexpr UINT D3D12SupportedSwapChainFlags =
    DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH |
    DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
    DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

static WMTIndexType
GetIndexType(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32
                                        : WMTIndexTypeUInt16;
}

static UINT
GetIndexSize(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R32_UINT ? 4 : 2;
}

static bool
IsSupportedIndexBufferFormat(DXGI_FORMAT format) {
  return format == DXGI_FORMAT_R16_UINT || format == DXGI_FORMAT_R32_UINT;
}

static const char *
PipelineStageName(PipelineStage stage) {
  switch (stage) {
  case PipelineStage::Pixel:
    return "pixel";
  case PipelineStage::Compute:
    return "compute";
  case PipelineStage::Geometry:
    return "geometry";
  case PipelineStage::Hull:
    return "hull";
  case PipelineStage::Domain:
    return "domain";
  case PipelineStage::Vertex:
  default:
    return "vertex";
  }
}

static UINT
GetMipLevel(const Resource &resource, UINT subresource) {
  const auto &desc = resource.GetResourceDesc();
  const UINT mip_levels = desc.MipLevels ? desc.MipLevels : 1;
  return mip_levels ? subresource % mip_levels : 0;
}

static UINT
GetArraySlice(const Resource &resource, UINT subresource) {
  const auto &desc = resource.GetResourceDesc();
  const UINT mip_levels = desc.MipLevels ? desc.MipLevels : 1;
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
             ? 0
             : subresource / mip_levels;
}

static UINT
GetSubresourceCount(const Resource &resource) {
  const auto &desc = resource.GetResourceDesc();
  const UINT mip_levels = desc.MipLevels ? desc.MipLevels : 1;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;
  return mip_levels *
         (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
              ? 1
              : desc.DepthOrArraySize);
}

static WMTSize
GetSubresourceSize(const Resource &resource, UINT subresource,
                   const D3D12_BOX *box) {
  if (box) {
    return {box->right - box->left, box->bottom - box->top,
            box->back - box->front};
  }

  const auto &desc = resource.GetResourceDesc();
  const auto mip = GetMipLevel(resource, subresource);
  return {std::max<UINT64>(1, desc.Width >> mip),
          std::max<UINT64>(1, desc.Height >> mip),
          desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
              ? std::max<UINT64>(1, desc.DepthOrArraySize >> mip)
              : 1};
}

static bool
StateHasWriteAccess(D3D12_RESOURCE_STATES state) {
  constexpr uint32_t kWriteStates =
      uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
      uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
      uint32_t(D3D12_RESOURCE_STATE_STREAM_OUT) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_DEST) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_DEST);
  return (uint32_t(state) & kWriteStates) != 0;
}

static bool
StateHasReadAccess(D3D12_RESOURCE_STATES state) {
  constexpr uint32_t kReadStates =
      uint32_t(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
      uint32_t(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
      uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_SOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_READ) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_SOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_PREDICATION);
  return (uint32_t(state) & kReadStates) != 0;
}

static int
ResourceAccessForState(D3D12_RESOURCE_STATES state) {
  int access = 0;
  if (StateHasReadAccess(state))
    access |= ResourceAccess::Read;
  if (StateHasWriteAccess(state))
    access |= ResourceAccess::Write;
  if (uint32_t(state) & uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    access |= ResourceAccess::UAV;
  return access;
}

static bool
IsKnownResourceState(D3D12_RESOURCE_STATES state) {
  constexpr uint32_t kKnownStates =
      uint32_t(D3D12_RESOURCE_STATE_COMMON) |
      uint32_t(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
      uint32_t(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
      uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET) |
      uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_READ) |
      uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_STREAM_OUT) |
      uint32_t(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_DEST) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_SOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_SOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_PREDICATION);
  return (uint32_t(state) & ~kKnownStates) == 0;
}

static void
WarnUnsupportedResourceState(D3D12_RESOURCE_STATES state, const char *context) {
  constexpr uint32_t kWriteStates =
      uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
      uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
      uint32_t(D3D12_RESOURCE_STATE_STREAM_OUT) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_DEST) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_DEST);
  if (!IsKnownResourceState(state)) {
    WARN("D3D12CommandQueue: unsupported resource state bits in ", context,
         " state=", uint32_t(state));
  }
  const auto writes = uint32_t(state) & kWriteStates;
  if (writes && (StateHasReadAccess(state) || (writes & (writes - 1)))) {
    WARN("D3D12CommandQueue: conservative handling for combined write state in ",
         context, " state=", uint32_t(state));
  }
}

static WMTPixelFormat
ResolveDepthStencilViewFormat(WMT::Device device, Resource &resource,
                              DXGI_FORMAT format);

static TextureViewKey
CreateRenderTargetView(Resource &resource, const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = 1;
  view.firstArraySlice = 0;
  view.arraySize = 1;
  view.intendedUsage = WMTTextureUsageRenderTarget;

  if (descriptor.has_desc) {
    const auto &rtv = descriptor.desc.rtv;
    switch (rtv.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE2D:
      view.firstMiplevel = rtv.Texture2D.MipSlice;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = rtv.Texture2DArray.MipSlice;
      view.firstArraySlice = rtv.Texture2DArray.FirstArraySlice;
      view.arraySize = rtv.Texture2DArray.ArraySize;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
      view.type = WMTTextureType2DMultisampleArray;
      view.firstArraySlice = rtv.Texture2DMSArray.FirstArraySlice;
      view.arraySize = rtv.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
    }
  }

  return texture->createView(view);
}

static TextureViewKey
CreateDepthStencilView(WMT::Device device, Resource &resource,
                       const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = 1;
  view.firstArraySlice = 0;
  view.arraySize = 1;
  view.intendedUsage = WMTTextureUsageRenderTarget;

  if (descriptor.has_desc) {
    const auto &dsv = descriptor.desc.dsv;
    view.format = ResolveDepthStencilViewFormat(device, resource, dsv.Format);
    if (view.format == WMTPixelFormatInvalid)
      return {};

    switch (dsv.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE2D:
      view.firstMiplevel = dsv.Texture2D.MipSlice;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
      view.type = WMTTextureType2DArray;
      view.firstMiplevel = dsv.Texture2DArray.MipSlice;
      view.firstArraySlice = dsv.Texture2DArray.FirstArraySlice;
      view.arraySize = dsv.Texture2DArray.ArraySize;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
      view.type = WMTTextureType2DMultisampleArray;
      view.firstArraySlice = dsv.Texture2DMSArray.FirstArraySlice;
      view.arraySize = dsv.Texture2DMSArray.ArraySize;
      break;
    default:
      break;
    }
  }

  return texture->createView(view);
}

static UINT
GetRenderTargetArrayLength(const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc)
    return 1;

  switch (descriptor.desc.rtv.ViewDimension) {
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    return descriptor.desc.rtv.Texture2DArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    return descriptor.desc.rtv.Texture2DMSArray.ArraySize;
  default:
    return 1;
  }
}

static UINT
GetDepthStencilArrayLength(const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc)
    return 1;

  switch (descriptor.desc.dsv.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return descriptor.desc.dsv.Texture2DArray.ArraySize;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    return descriptor.desc.dsv.Texture2DMSArray.ArraySize;
  default:
    return 1;
  }
}

static BufferSlice
DefaultBufferSlice(Resource &resource, UINT64 offset = 0,
                   UINT64 requested_size = 0) {
  const auto width = resource.GetResourceDesc().Width;
  const auto remaining = width > offset ? width - offset : 0;
  const auto size = requested_size ? std::min<UINT64>(requested_size, remaining)
                                  : remaining;
  return {
      .byteOffset = UINT32(offset),
      .byteLength = UINT32(std::min<UINT64>(size, UINT32_MAX)),
      .firstElement = UINT32(offset),
      .elementCount = UINT32(std::min<UINT64>(size, UINT32_MAX)),
  };
}

static BufferSlice
StructuredBufferSlice(Resource &resource, UINT64 offset,
                      UINT64 byte_size, UINT stride) {
  auto slice = DefaultBufferSlice(resource, offset, byte_size);
  if (stride) {
    slice.firstElement = UINT32(offset / stride);
    slice.elementCount = UINT32(slice.byteLength / stride);
  }
  return slice;
}

static UINT64
ResolveBufferGpuAddress(D3D12_GPU_VIRTUAL_ADDRESS address,
                        Resource *&resource) {
  UINT64 offset = 0;
  resource = LookupBufferResourceByGpuVirtualAddress(address, &offset);
  return offset;
}

static uint64_t
CreateBufferView(WMT::Device device, Resource &resource, DXGI_FORMAT format,
                 UINT64 offset, UINT64 byte_size, WMTTextureUsage usage) {
  if (!resource.GetBuffer())
    return 0;

  MTL_DXGI_FORMAT_DESC format_desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, format, format_desc)) ||
      format_desc.PixelFormat == WMTPixelFormatInvalid)
    return 0;

  BufferViewDescriptor view = {};
  view.format = format_desc.PixelFormat;
  view.usage = usage;
  view.type = WMTTextureTypeTextureBuffer;
  view.byteOffset = UINT32(offset);
  view.byteLength = UINT32(std::min<UINT64>(byte_size, UINT32_MAX));
  return resource.GetBuffer()->createView(view);
}

static DXGI_FORMAT
UintBufferViewFormatForStride(UINT stride) {
  switch (stride) {
  case 4:
    return DXGI_FORMAT_R32_UINT;
  case 8:
    return DXGI_FORMAT_R32G32_UINT;
  case 16:
    return DXGI_FORMAT_R32G32B32A32_UINT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

static D3D12_DESCRIPTOR_HEAP_TYPE
DescriptorHeapTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
  return range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
             ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
             : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
}

static UINT
NormalizeViewCount(UINT requested, UINT first, UINT total) {
  if (first >= total)
    return 1;
  const UINT remaining = total - first;
  if (requested == UINT_MAX || requested == 0)
    return remaining;
  return std::min(requested, remaining);
}

static UINT
GetMipDepth(const Resource &resource, UINT mip_slice) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 1;
  return static_cast<UINT>(std::max<UINT64>(1, desc.DepthOrArraySize >> mip_slice));
}

static WMTPixelFormat
ResolveTextureViewFormat(WMT::Device device, Resource &resource,
                         DXGI_FORMAT format) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return WMTPixelFormatInvalid;
  if (format == DXGI_FORMAT_UNKNOWN)
    return texture->pixelFormat();
  if (DepthStencilPlanarFlags(texture->pixelFormat())) {
    switch (format) {
    case DXGI_FORMAT_R16_UNORM:
      if (texture->pixelFormat() == WMTPixelFormatDepth16Unorm)
        return texture->pixelFormat();
      break;
    case DXGI_FORMAT_R32_FLOAT:
      if (texture->pixelFormat() == WMTPixelFormatDepth32Float)
        return texture->pixelFormat();
      break;
    default:
      break;
    }
  }

  MTL_DXGI_FORMAT_DESC format_desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, format, format_desc)) ||
      format_desc.PixelFormat == WMTPixelFormatInvalid) {
    WARN("D3D12CommandQueue: unsupported texture view format ",
         uint32_t(format));
    return WMTPixelFormatInvalid;
  }
  return format_desc.PixelFormat;
}

static WMTPixelFormat
ResolveDepthStencilViewFormat(WMT::Device device, Resource &resource,
                              DXGI_FORMAT format) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return WMTPixelFormatInvalid;
  if (format == DXGI_FORMAT_UNKNOWN)
    return texture->pixelFormat();

  MTL_DXGI_FORMAT_DESC format_desc = {};
  if (FAILED(MTLQueryDXGIFormat(device, format, format_desc)) ||
      !DepthStencilPlanarFlags(format_desc.PixelFormat)) {
    WARN("D3D12CommandQueue: unsupported DSV texture view format ",
         uint32_t(format));
    return WMTPixelFormatInvalid;
  }
  return format_desc.PixelFormat;
}

static bool
ValidateTextureViewRange(const char *context, TextureViewDescriptor &view,
                         const Resource &resource) {
  const auto *texture = resource.GetTexture();
  if (!texture)
    return false;

  if (view.firstMiplevel >= texture->miplevelCount() ||
      view.miplevelCount == 0 ||
      view.miplevelCount > texture->miplevelCount() - view.firstMiplevel) {
    WARN("D3D12CommandQueue: ", context,
         " mip range exceeds texture levels first=", view.firstMiplevel,
         " count=", view.miplevelCount,
         " levels=", texture->miplevelCount());
    return false;
  }

  if (resource.GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
    view.firstArraySlice = 0;
    view.arraySize = 1;
    return true;
  }

  if (view.firstArraySlice >= texture->arrayLength() ||
      view.arraySize == 0 ||
      view.arraySize > texture->arrayLength() - view.firstArraySlice) {
    WARN("D3D12CommandQueue: ", context,
         " array range exceeds texture array first=", view.firstArraySlice,
         " count=", view.arraySize, " array_length=", texture->arrayLength());
    return false;
  }
  return true;
}

static TextureViewKey
CreateShaderResourceTextureView(WMT::Device device, Resource &resource,
                                const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = texture->miplevelCount();
  view.firstArraySlice = 0;
  view.arraySize = texture->arrayLength();
  view.intendedUsage = WMTTextureUsageShaderRead;

  if (!descriptor.has_desc)
    return texture->createView(view);

  const auto &srv = descriptor.desc.srv;
  view.format = ResolveTextureViewFormat(device, resource, srv.Format);
  if (view.format == WMTPixelFormatInvalid)
    return {};

  switch (srv.ViewDimension) {
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    view.type = WMTTextureType1D;
    view.firstMiplevel = srv.Texture1D.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture1D.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.arraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    view.type = WMTTextureType1DArray;
    view.firstMiplevel = srv.Texture1DArray.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture1DArray.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.firstArraySlice = srv.Texture1DArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(srv.Texture1DArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    view.type = WMTTextureType2D;
    view.firstMiplevel = srv.Texture2D.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture2D.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.arraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    view.type = WMTTextureType2DArray;
    view.firstMiplevel = srv.Texture2DArray.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture2DArray.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.firstArraySlice = srv.Texture2DArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(srv.Texture2DArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DMS:
    view.type = WMTTextureType2DMultisample;
    view.miplevelCount = 1;
    view.arraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
    view.type = WMTTextureType2DMultisampleArray;
    view.miplevelCount = 1;
    view.firstArraySlice = srv.Texture2DMSArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(srv.Texture2DMSArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    view.type = WMTTextureType3D;
    view.firstMiplevel = srv.Texture3D.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.Texture3D.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.arraySize = 1;
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    view.type = WMTTextureTypeCube;
    view.firstMiplevel = srv.TextureCube.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.TextureCube.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.arraySize = std::min<UINT>(6, texture->arrayLength());
    break;
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    view.type = WMTTextureTypeCubeArray;
    view.firstMiplevel = srv.TextureCubeArray.MostDetailedMip;
    view.miplevelCount =
        NormalizeViewCount(srv.TextureCubeArray.MipLevels, view.firstMiplevel,
                           texture->miplevelCount());
    view.firstArraySlice = srv.TextureCubeArray.First2DArrayFace;
    view.arraySize = NormalizeViewCount(srv.TextureCubeArray.NumCubes * 6,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  default:
    WARN("D3D12CommandQueue: unsupported SRV texture dimension ",
         uint32_t(srv.ViewDimension));
    return {};
  }

  if (!ValidateTextureViewRange("SRV texture view", view, resource))
    return {};
  return texture->createView(view);
}

static TextureViewKey
CreateUnorderedAccessTextureView(WMT::Device device, Resource &resource,
                                 const DescriptorRecord &descriptor) {
  auto *texture = resource.GetTexture();
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  view.format = texture->pixelFormat();
  view.type = texture->textureType();
  view.firstMiplevel = 0;
  view.miplevelCount = 1;
  view.firstArraySlice = 0;
  view.arraySize = texture->arrayLength();
  view.intendedUsage =
      WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;

  if (!descriptor.has_desc)
    return texture->createView(view);

  const auto &uav = descriptor.desc.uav;
  view.format = ResolveTextureViewFormat(device, resource, uav.Format);
  if (view.format == WMTPixelFormatInvalid)
    return {};

  switch (uav.ViewDimension) {
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    view.type = WMTTextureType1D;
    view.firstMiplevel = uav.Texture1D.MipSlice;
    view.arraySize = 1;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    view.type = WMTTextureType1DArray;
    view.firstMiplevel = uav.Texture1DArray.MipSlice;
    view.firstArraySlice = uav.Texture1DArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(uav.Texture1DArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    view.type = WMTTextureType2D;
    view.firstMiplevel = uav.Texture2D.MipSlice;
    view.arraySize = 1;
    break;
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    view.type = WMTTextureType2DArray;
    view.firstMiplevel = uav.Texture2DArray.MipSlice;
    view.firstArraySlice = uav.Texture2DArray.FirstArraySlice;
    view.arraySize = NormalizeViewCount(uav.Texture2DArray.ArraySize,
                                        view.firstArraySlice,
                                        texture->arrayLength());
    break;
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    view.type = WMTTextureType3D;
    view.firstMiplevel = uav.Texture3D.MipSlice;
    view.arraySize = 1;
    if (view.firstMiplevel >= texture->miplevelCount()) {
      WARN("D3D12CommandQueue: invalid 3D texture UAV mip slice ",
           view.firstMiplevel);
      return {};
    }
    {
      const UINT mip_depth = GetMipDepth(resource, view.firstMiplevel);
      const UINT first_w = uav.Texture3D.FirstWSlice;
      const UINT w_size = uav.Texture3D.WSize == UINT_MAX
                              ? (first_w < mip_depth ? mip_depth - first_w : 0)
                              : uav.Texture3D.WSize;
      if (first_w >= mip_depth || w_size == 0 || w_size > mip_depth - first_w) {
        WARN("D3D12CommandQueue: invalid 3D texture UAV W slice range first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return {};
      }
      if (first_w != 0 || w_size != mip_depth) {
        // TODO(d3d12): lower 3D texture UAV depth-slice subviews once the
        // DXMT texture view layer can represent a W-slice range for 3D images.
        WARN("D3D12CommandQueue: unsupported 3D texture UAV W slice subrange first=",
             first_w, " size=", w_size, " mip_depth=", mip_depth);
        return {};
      }
    }
    break;
  default:
    WARN("D3D12CommandQueue: unsupported UAV texture dimension ",
         uint32_t(uav.ViewDimension));
    return {};
  }

  if (!ValidateTextureViewRange("UAV texture view", view, resource))
    return {};
  return texture->createView(view);
}

static HRESULT
NormalizeQueueDesc(const D3D12_COMMAND_QUEUE_DESC *desc,
                   D3D12_COMMAND_QUEUE_DESC &normalized) {
  normalized = {};
  normalized.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  normalized.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
  normalized.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  normalized.NodeMask = 0;

  if (desc)
    normalized = *desc;

  if (!IsSupportedQueueType(normalized.Type)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported queue type ", normalized.Type));
    return E_INVALIDARG;
  }

  if (!IsSupportedQueuePriority(normalized.Priority)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported priority ", normalized.Priority));
    return E_INVALIDARG;
  }

  if (!IsSupportedQueueFlags(normalized.Flags)) {
    Logger::err(str::format("D3D12CommandQueue: unsupported flags ", normalized.Flags));
    return E_INVALIDARG;
  }

  if (normalized.NodeMask > 1) {
    Logger::err(str::format("D3D12CommandQueue: unsupported node mask ", normalized.NodeMask));
    return E_INVALIDARG;
  }

  return S_OK;
}

class CommandQueueImpl final : public ComObjectWithInitialRef<ID3D12CommandQueue, IMTLDXGIDevice> {
public:
  CommandQueueImpl(IMTLD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC &desc)
      : device_(device), desc_(desc) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandQueue)) {
      *ppvObject = ref(static_cast<ID3D12CommandQueue *>(this));
      return S_OK;
    }

    if (riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDevice) ||
        riid == __uuidof(IDXGIDevice1) || riid == __uuidof(IDXGIDevice2) ||
        riid == __uuidof(IDXGIDevice3) || riid == __uuidof(IMTLDXGIDevice)) {
      *ppvObject = ref(static_cast<IMTLDXGIDevice *>(this));
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12CommandQueue), riid))
      WARN("D3D12CommandQueue: unknown interface query ", str::format(riid));

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
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

#ifdef __MINGW32__
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *resource, UINT region_count,
                                            const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
                                            const D3D12_TILE_REGION_SIZE *region_sizes,
                                            ID3D12Heap *heap,
                                            UINT range_count,
                                            const D3D12_TILE_RANGE_FLAGS *range_flags,
                                            const UINT *heap_range_offsets,
                                            const UINT *range_tile_counts,
                                            D3D12_TILE_MAPPING_FLAGS flags) override {
    // TODO(d3d12): implement reserved-resource tile mappings once tiled
    // resources are represented by the D3D12 resource layer.
    WARN("D3D12CommandQueue: UpdateTileMappings is unsupported");
  }
#else
  void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource *resource, UINT region_count,
                                            const D3D12_TILED_RESOURCE_COORDINATE *region_start_coordinates,
                                            const D3D12_TILE_REGION_SIZE *region_sizes,
                                            UINT range_count,
                                            const D3D12_TILE_RANGE_FLAGS *range_flags,
                                            UINT *heap_range_offsets,
                                            UINT *range_tile_counts,
                                            D3D12_TILE_MAPPING_FLAGS flags) override {
    // TODO(d3d12): implement reserved-resource tile mappings once tiled
    // resources are represented by the D3D12 resource layer.
    WARN("D3D12CommandQueue: UpdateTileMappings is unsupported");
  }
#endif

  void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource *dst_resource,
                                          const D3D12_TILED_RESOURCE_COORDINATE *dst_region_start_coordinate,
                                          ID3D12Resource *src_resource,
                                          const D3D12_TILED_RESOURCE_COORDINATE *src_region_start_coordinate,
                                          const D3D12_TILE_REGION_SIZE *region_size,
                                          D3D12_TILE_MAPPING_FLAGS flags) override {
    // TODO(d3d12): implement reserved-resource tile mapping copies once tiled
    // resources are represented by the D3D12 resource layer.
    WARN("D3D12CommandQueue: CopyTileMappings is unsupported");
  }

  void STDMETHODCALLTYPE ExecuteCommandLists(UINT command_list_count,
                                             ID3D12CommandList *const *command_lists) override {
    if (!command_list_count)
      return;

    if (!command_lists) {
      Logger::err("D3D12CommandQueue: ExecuteCommandLists called with null command list array");
      return;
    }

    bool submitted_any = false;
    for (UINT i = 0; i < command_list_count; i++) {
      auto *command_list = command_lists[i];
      if (!command_list) {
        Logger::err(str::format("D3D12CommandQueue: null command list at index ", i));
        continue;
      }

      auto *state = dynamic_cast<GraphicsCommandList *>(command_list);
      if (!state) {
        Logger::err(str::format("D3D12CommandQueue: foreign command list at index ", i));
        continue;
      }

      if (state->GetCommandListType() != desc_.Type) {
        Logger::err(str::format("D3D12CommandQueue: command list type ", state->GetCommandListType(),
                                " does not match queue type ", desc_.Type));
        continue;
      }

      if (!state->IsClosed()) {
        Logger::err(str::format("D3D12CommandQueue: command list at index ", i, " is not closed"));
        continue;
      }

      if (SUCCEEDED(state->MarkSubmittedToQueue(desc_.Type))) {
        ReplayCommandRecords(state->GetCommandRecords());
        submitted_any = true;
      }
    }

    if (submitted_any) {
      device_->GetDXMTDevice().queue().CommitCurrentChunk();
      submitted_batches_++;
    }
  }

  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) override {}

  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) override {}

  void STDMETHODCALLTYPE EndEvent() override {}

  HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence *fence, UINT64 value) override {
    if (!fence)
      return E_INVALIDARG;

    auto *state = dynamic_cast<Fence *>(fence);
    if (!state)
      return E_INVALIDARG;

    auto event = state->GetSharedEvent();
    {
      std::lock_guard lock(mutex_);
      auto &queue = device_->GetDXMTDevice().queue();
      queue.CurrentChunk()->emitcc([event = std::move(event), value](ArgumentEncodingContext &enc) mutable {
        enc.signalEvent(std::move(event), value);
      });
      queue.CommitCurrentChunk();
      signal_count_++;
      last_signal_value_ = value;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence *fence, UINT64 value) override {
    if (!fence)
      return E_INVALIDARG;

    auto *state = dynamic_cast<Fence *>(fence);
    if (!state)
      return E_INVALIDARG;

    auto event = state->GetSharedEvent();
    {
      std::lock_guard lock(mutex_);
      auto &queue = device_->GetDXMTDevice().queue();
      queue.CurrentChunk()->emitcc([event = std::move(event), value](ArgumentEncodingContext &enc) mutable {
        enc.waitEvent(std::move(event), value);
      });
      queue.CommitCurrentChunk();
      wait_values_.push_back(value);
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64 *frequency) override {
    if (!frequency)
      return E_INVALIDARG;

    *frequency = 1'000'000'000ull;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64 *gpu_timestamp, UINT64 *cpu_timestamp) override {
    if (!gpu_timestamp || !cpu_timestamp)
      return E_INVALIDARG;

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto timestamp =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    const auto monotonic = timestamp < 0 ? 0 : static_cast<UINT64>(timestamp);
    *gpu_timestamp = monotonic;
    *cpu_timestamp = monotonic;
    return S_OK;
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE GetDesc(D3D12_COMMAND_QUEUE_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }
#else
  D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc() override {
    return desc_;
  }
#endif

  HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **ppParent) override {
    return device_->GetParent(riid, ppParent);
  }

  HRESULT STDMETHODCALLTYPE GetAdapter(IDXGIAdapter **pAdapter) override {
    return device_->GetAdapter(pAdapter);
  }

  HRESULT STDMETHODCALLTYPE CreateSurface(const DXGI_SURFACE_DESC *desc, UINT surface_count,
                                          DXGI_USAGE usage,
                                          const DXGI_SHARED_RESOURCE *shared_resource,
                                          IDXGISurface **surface) override {
    InitReturnPtr(surface);
    return DXGI_ERROR_UNSUPPORTED;
  }

  HRESULT STDMETHODCALLTYPE QueryResourceResidency(IUnknown *const *resources,
                                                   DXGI_RESIDENCY *residency,
                                                   UINT resource_count) override {
    return device_->QueryResourceResidency(resources, residency, resource_count);
  }

  HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT priority) override {
    return device_->SetGPUThreadPriority(priority);
  }

  HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT *priority) override {
    return device_->GetGPUThreadPriority(priority);
  }

  HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
    return device_->SetMaximumFrameLatency(max_latency);
  }

  HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
    return device_->GetMaximumFrameLatency(max_latency);
  }

  HRESULT STDMETHODCALLTYPE OfferResources(UINT resource_count, IDXGIResource *const *resources,
                                           DXGI_OFFER_RESOURCE_PRIORITY priority) override {
    return device_->OfferResources(resource_count, resources, priority);
  }

  HRESULT STDMETHODCALLTYPE ReclaimResources(UINT resource_count, IDXGIResource *const *resources,
                                             WINBOOL *discarded) override {
    return device_->ReclaimResources(resource_count, resources, discarded);
  }

  HRESULT STDMETHODCALLTYPE EnqueueSetEvent(HANDLE event) override {
    return device_->EnqueueSetEvent(event);
  }

  void STDMETHODCALLTYPE Trim() override {
    device_->Trim();
  }

  WMT::Device STDMETHODCALLTYPE GetMTLDevice() override {
    return device_->GetMTLDevice();
  }

  D3DKMT_HANDLE STDMETHODCALLTYPE GetLocalD3DKMT() override {
    return device_->GetLocalD3DKMT();
  }

  HRESULT STDMETHODCALLTYPE CreateSwapChain(IDXGIFactory1 *factory, HWND hWnd,
                                            const DXGI_SWAP_CHAIN_DESC1 *desc,
                                            const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc,
                                            IDXGISwapChain1 **swap_chain) override {
    InitReturnPtr(swap_chain);
    if (!swap_chain || !factory || !hWnd || !desc)
      return DXGI_ERROR_INVALID_CALL;
    if (desc_.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
      return DXGI_ERROR_UNSUPPORTED;

    auto object = Com<IDXGISwapChain1>::transfer(
        new SwapChainImpl(this, factory, hWnd, desc, fullscreen_desc));
    return object->QueryInterface(IID_PPV_ARGS(swap_chain));
  }

private:
  class SwapChainImpl final : public ComObjectWithInitialRef<IDXGISwapChain4> {
  public:
    SwapChainImpl(CommandQueueImpl *queue, IDXGIFactory1 *factory, HWND hWnd,
                  const DXGI_SWAP_CHAIN_DESC1 *desc,
                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreen_desc)
        : queue_(queue), factory_(factory), hWnd_(hWnd), desc_(*desc),
          fullscreen_desc_(fullscreen_desc ? *fullscreen_desc
                                           : DXGI_SWAP_CHAIN_FULLSCREEN_DESC{}),
          hud_(WMT::DeveloperHUDProperties::instance()) {
      if (!fullscreen_desc)
        fullscreen_desc_.Windowed = TRUE;

      native_view_ = WMT::CreateMetalViewFromHWND(
          reinterpret_cast<intptr_t>(hWnd_), queue->device_->GetMTLDevice(),
          layer_);
      if (!native_view_) {
        Logger::err("D3D12SwapChain: failed to create Metal view");
        return;
      }

      presenter_ = Rc(new Presenter(
          queue->device_->GetMTLDevice(), layer_,
          queue->device_->GetDXMTDevice().queue().cmd_library, 1.0f,
          desc_.SampleDesc.Count ? desc_.SampleDesc.Count : 1));
      hud_.initialize(GetVersionDescriptionText(12, D3D_FEATURE_LEVEL_12_0));
      if (desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        present_semaphore_ =
            CreateSemaphore(nullptr, frame_latency_, DXGI_MAX_SWAP_CHAIN_BUFFERS,
                            nullptr);
      }
      ResizeBuffers(desc_.BufferCount, desc_.Width, desc_.Height, desc_.Format,
                    desc_.Flags);
    }

    ~SwapChainImpl() {
      backbuffers_.clear();
      if (present_semaphore_)
        CloseHandle(present_semaphore_);
      if (native_view_)
        WMT::ReleaseMetalView(native_view_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                             void **object) override {
      if (!object)
        return E_POINTER;
      *object = nullptr;
      if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) ||
          riid == __uuidof(IDXGIDeviceSubObject) ||
          riid == __uuidof(IDXGISwapChain) ||
          riid == __uuidof(IDXGISwapChain1) ||
          riid == __uuidof(IDXGISwapChain2) ||
          riid == __uuidof(IDXGISwapChain3) ||
          riid == __uuidof(IDXGISwapChain4)) {
        *object = ref(this);
        return S_OK;
      }
      if (logQueryInterfaceError(__uuidof(IDXGISwapChain1), riid))
        WARN("D3D12SwapChain: unknown interface query ", str::format(riid));
      return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void **parent) override {
      return factory_->QueryInterface(riid, parent);
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
    SetPrivateDataInterface(REFGUID guid, const IUnknown *object) override {
      return private_data_.setInterface(guid, object);
    }

    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
      return queue_->device_->QueryInterface(riid, device);
    }

    HRESULT STDMETHODCALLTYPE Present(UINT sync_interval,
                                      UINT flags) override {
      return Present1(sync_interval, flags, nullptr);
    }

    HRESULT STDMETHODCALLTYPE GetBuffer(UINT buffer_idx, REFIID riid,
                                        void **surface) override {
      if (!surface)
        return E_POINTER;
      *surface = nullptr;
      if (buffer_idx >= backbuffers_.size())
        return DXGI_ERROR_INVALID_CALL;
      return backbuffers_[buffer_idx]->QueryInterface(riid, surface);
    }

    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL fullscreen,
                                                 IDXGIOutput *target) override {
      fullscreen_desc_.Windowed = !fullscreen;
      target_ = target;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL *fullscreen,
                                                 IDXGIOutput **target) override {
      if (fullscreen)
        *fullscreen = !fullscreen_desc_.Windowed;
      if (target)
        *target = target_.ref();
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC *desc) override {
      if (!desc)
        return E_INVALIDARG;
      desc->BufferDesc.Width = desc_.Width;
      desc->BufferDesc.Height = desc_.Height;
      desc->BufferDesc.RefreshRate = fullscreen_desc_.RefreshRate;
      desc->BufferDesc.Format = desc_.Format;
      desc->BufferDesc.ScanlineOrdering = fullscreen_desc_.ScanlineOrdering;
      desc->BufferDesc.Scaling = fullscreen_desc_.Scaling;
      desc->SampleDesc = desc_.SampleDesc;
      desc->BufferUsage = desc_.BufferUsage;
      desc->BufferCount = desc_.BufferCount;
      desc->OutputWindow = hWnd_;
      desc->Windowed = fullscreen_desc_.Windowed;
      desc->SwapEffect = desc_.SwapEffect;
      desc->Flags = desc_.Flags;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT buffer_count, UINT width,
                                            UINT height, DXGI_FORMAT format,
                                            UINT flags) override {
      UINT old_width = desc_.Width;
      UINT old_height = desc_.Height;
      UINT old_index = current_backbuffer_;
      if (buffer_count == 0)
        buffer_count = desc_.BufferCount ? desc_.BufferCount : 2;
      if (!buffer_count || buffer_count > DXGI_MAX_SWAP_CHAIN_BUFFERS) {
        WARN("D3D12SwapChain::ResizeBuffers: invalid buffer count ",
             buffer_count);
        return DXGI_ERROR_INVALID_CALL;
      }
      UINT new_width = width;
      UINT new_height = height;
      if (new_width == 0 || new_height == 0)
        wsi::getWindowSize(hWnd_, new_width ? nullptr : &new_width,
                           new_height ? nullptr : &new_height);
      new_width = new_width ? new_width : 1;
      new_height = new_height ? new_height : 1;
      DXGI_FORMAT new_format =
          format == DXGI_FORMAT_UNKNOWN ? desc_.Format : format;
      if (GetSwapChainPixelFormat(new_format) == WMTPixelFormatInvalid) {
        WARN("D3D12SwapChain::ResizeBuffers: unsupported format ",
             new_format);
        return DXGI_ERROR_UNSUPPORTED;
      }
      if (HasExternalBackBufferReferences()) {
        WARN("D3D12SwapChain::ResizeBuffers: backbuffer references are still "
             "held by the application");
        return DXGI_ERROR_INVALID_CALL;
      }
      if (flags & ~D3D12SupportedSwapChainFlags) {
        WARN("D3D12SwapChain::ResizeBuffers: unsupported flags ",
             flags & ~D3D12SupportedSwapChainFlags);
        return DXGI_ERROR_UNSUPPORTED;
      }

      desc_.BufferCount = buffer_count;
      desc_.Width = new_width;
      desc_.Height = new_height;
      desc_.Format = new_format;
      desc_.Flags = flags;

      if (!width || !height) {
        WARN("D3D12SwapChain::ResizeBuffers: resolved zero size request to ",
             desc_.Width, "x", desc_.Height);
      }

      presenter_->changeLayerProperties(GetSwapChainPixelFormat(desc_.Format),
                                        GetD3D12SwapChainLayerColorSpace(
                                            desc_.Format, color_space_),
                                        desc_.Width, desc_.Height,
                                        desc_.SampleDesc.Count
                                            ? desc_.SampleDesc.Count
                                            : 1);

      backbuffers_.clear();
      backbuffers_.reserve(desc_.BufferCount);
      for (UINT i = 0; i < desc_.BufferCount; i++) {
        D3D12_HEAP_PROPERTIES heap_props = {};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heap_props.CreationNodeMask = 1;
        heap_props.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC resource_desc = {};
        resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resource_desc.Alignment = 0;
        resource_desc.Width = desc_.Width;
        resource_desc.Height = desc_.Height;
        resource_desc.DepthOrArraySize = 1;
        resource_desc.MipLevels = 1;
        resource_desc.Format = desc_.Format;
        resource_desc.SampleDesc = desc_.SampleDesc;
        if (!resource_desc.SampleDesc.Count)
          resource_desc.SampleDesc.Count = 1;
        resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        backbuffers_.push_back(CreateResource(
            queue_->device_.ptr(), &heap_props, D3D12_HEAP_FLAG_NONE,
            &resource_desc, D3D12_RESOURCE_STATE_PRESENT, 0));
        if (!backbuffers_.back())
          return E_FAIL;
      }

      current_backbuffer_ = desc_.BufferCount ? old_index % desc_.BufferCount : 0;
      if (!source_width_ || source_width_ == old_width)
        source_width_ = desc_.Width;
      if (!source_height_ || source_height_ == old_height)
        source_height_ = desc_.Height;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC *desc) override {
      return desc ? S_OK : DXGI_ERROR_INVALID_CALL;
    }

    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput **output) override {
      InitReturnPtr(output);
      if (!output)
        return E_POINTER;
      if (!wsi::isWindow(hWnd_))
        return DXGI_ERROR_INVALID_CALL;
      if (target_) {
        *output = target_.ref();
        return S_OK;
      }
      return GetOutputFromMonitor(wsi::getWindowMonitor(hWnd_), output);
    }

    HRESULT STDMETHODCALLTYPE
    GetFrameStatistics(DXGI_FRAME_STATISTICS *stats) override {
      if (!stats)
        return E_INVALIDARG;
      stats->PresentCount = presentation_count_;
      stats->SyncRefreshCount = presentation_count_;
      stats->PresentRefreshCount = presentation_count_;
      stats->SyncGPUTime = {};
      stats->SyncQPCTime = {};
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT *last_present_count) override {
      if (!last_present_count)
        return E_POINTER;
      *last_present_count = presentation_count_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1 *desc) override {
      if (!desc)
        return E_POINTER;
      *desc = desc_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *desc) override {
      if (!desc)
        return E_POINTER;
      *desc = fullscreen_desc_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetHwnd(HWND *hWnd) override {
      if (!hWnd)
        return E_POINTER;
      *hWnd = hWnd_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID riid, void **window) override {
      InitReturnPtr(window);
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Present1(
        UINT sync_interval, UINT flags,
        const DXGI_PRESENT_PARAMETERS *present_parameters) override {
      if (sync_interval > 4)
        return DXGI_ERROR_INVALID_CALL;
      if (flags & ~D3D12SupportedPresentFlags) {
        WARN("D3D12SwapChain::Present1: unsupported flags ",
             flags & ~D3D12SupportedPresentFlags);
        return DXGI_ERROR_UNSUPPORTED;
      }
      if ((flags & DXGI_PRESENT_ALLOW_TEARING) &&
          !(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING)) {
        WARN("D3D12SwapChain::Present1: ALLOW_TEARING used without swapchain "
             "tearing support");
        return DXGI_ERROR_INVALID_CALL;
      }
      if ((flags & DXGI_PRESENT_ALLOW_TEARING) && sync_interval) {
        WARN("D3D12SwapChain::Present1: ALLOW_TEARING requires sync interval 0");
        return DXGI_ERROR_INVALID_CALL;
      }
      if (present_parameters &&
          (present_parameters->DirtyRectsCount || present_parameters->pDirtyRects ||
           present_parameters->pScrollRect || present_parameters->pScrollOffset)) {
        WARN("D3D12SwapChain::Present1: dirty rect and scroll parameters are "
             "not supported");
        return DXGI_ERROR_UNSUPPORTED;
      }

      bool occluded = wsi::isMinimized(hWnd_);
      HRESULT hr = occluded ? DXGI_STATUS_OCCLUDED : S_OK;
      if (flags & DXGI_PRESENT_TEST)
        return hr;
      if (hr == DXGI_STATUS_OCCLUDED)
        return hr;

      auto *resource = dynamic_cast<Resource *>(
          backbuffers_[current_backbuffer_].ptr());
      if (!resource || !resource->GetTexture())
        return E_FAIL;

      double vsync_duration = sync_interval ? sync_interval / 60.0 : 0.0;
      auto &dxmt_queue = queue_->device_->GetDXMTDevice().queue();
      auto *chunk = dxmt_queue.CurrentChunk();
      chunk->signal_frame_latency_fence_ = dxmt_queue.CurrentFrameSeq();
      auto state = presenter_->synchronizeLayerProperties();
      HANDLE present_signal = nullptr;
      if (present_semaphore_) {
        HANDLE process = GetCurrentProcess();
        DuplicateHandle(process, present_semaphore_, process, &present_signal,
                        0, FALSE, DUPLICATE_SAME_ACCESS);
      }
      chunk->emitcc([
        backbuffer = Rc<Texture>(resource->GetTexture()),
        presenter = presenter_,
        present_signal,
        vsync_duration,
        state = std::move(state)
      ](ArgumentEncodingContext &ctx) mutable {
        ctx.present(backbuffer, presenter, vsync_duration, state.metadata);
        if (present_signal) {
          ReleaseSemaphore(present_signal, 1, nullptr);
          CloseHandle(present_signal);
        }
      });
      dxmt_queue.CommitCurrentChunk();
      dxmt_queue.PresentBoundary();

      presentation_count_++;
      current_backbuffer_ =
          desc_.BufferCount ? (current_backbuffer_ + 1) % desc_.BufferCount : 0;
      return S_OK;
    }

    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return FALSE; }

    HRESULT STDMETHODCALLTYPE
    GetRestrictToOutput(IDXGIOutput **restrict_to_output) override {
      InitReturnPtr(restrict_to_output);
      return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA *color) override {
      background_color_ = color ? *color : DXGI_RGBA{};
      return color ? S_OK : E_INVALIDARG;
    }

    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA *color) override {
      if (!color)
        return E_INVALIDARG;
      *color = background_color_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION rotation) override {
      rotation_ = rotation;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION *rotation) override {
      if (!rotation)
        return E_INVALIDARG;
      *rotation = rotation_;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT width, UINT height) override {
      if (!width || !height)
        return E_INVALIDARG;
      source_width_ = width;
      source_height_ = height;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT *width, UINT *height) override {
      if (width)
        *width = source_width_ ? source_width_ : desc_.Width;
      if (height)
        *height = source_height_ ? source_height_ : desc_.Height;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT max_latency) override {
      if (!max_latency || max_latency > DXGI_MAX_SWAP_CHAIN_BUFFERS)
        return E_INVALIDARG;
      if (present_semaphore_ && max_latency > frame_latency_)
        ReleaseSemaphore(present_semaphore_, max_latency - frame_latency_,
                         nullptr);
      frame_latency_ = max_latency;
      queue_->device_->GetDXMTDevice().queue().SetMaxLatency(max_latency);
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT *max_latency) override {
      if (max_latency)
        *max_latency = frame_latency_;
      return S_OK;
    }

    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
      if (!(desc_.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) ||
          !present_semaphore_)
        return nullptr;

      HANDLE result = nullptr;
      HANDLE process = GetCurrentProcess();
      if (!DuplicateHandle(process, present_semaphore_, process, &result, 0,
                           FALSE, DUPLICATE_SAME_ACCESS))
        return nullptr;
      return result;
    }

    HRESULT STDMETHODCALLTYPE
    SetMatrixTransform(const DXGI_MATRIX_3X2_F *matrix) override {
      if (!matrix)
        return E_INVALIDARG;
      matrix_ = *matrix;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE
    GetMatrixTransform(DXGI_MATRIX_3X2_F *matrix) override {
      if (!matrix)
        return E_INVALIDARG;
      *matrix = matrix_;
      return S_OK;
    }

    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
      return current_backbuffer_;
    }

    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(
        DXGI_COLOR_SPACE_TYPE color_space, UINT *color_space_support) override {
      if (!color_space_support)
        return E_INVALIDARG;
      *color_space_support =
          IsSupportedD3D12SwapChainColorSpace(color_space)
              ? DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT
              : 0;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE SetColorSpace1(
        DXGI_COLOR_SPACE_TYPE color_space) override {
      if (!IsSupportedD3D12SwapChainColorSpace(color_space)) {
        WARN("D3D12SwapChain::SetColorSpace1: unsupported color space ",
             color_space);
        return DXGI_ERROR_UNSUPPORTED;
      }
      color_space_ = color_space;
      presenter_->changeLayerColorSpace(
          GetD3D12SwapChainLayerColorSpace(desc_.Format, color_space_));
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ResizeBuffers1(
        UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format,
        UINT flags, const UINT *creation_node_mask,
        IUnknown *const *present_queue) override {
      if (creation_node_mask) {
        for (UINT i = 0; i < buffer_count; i++) {
          if (creation_node_mask[i] > 1) {
            WARN("D3D12SwapChain::ResizeBuffers1: unsupported creation node mask ",
                 creation_node_mask[i]);
            return DXGI_ERROR_UNSUPPORTED;
          }
        }
      }
      if (present_queue) {
        WARN("D3D12SwapChain::ResizeBuffers1: present queues are not supported");
        return DXGI_ERROR_UNSUPPORTED;
      }
      return ResizeBuffers(buffer_count, width, height, format, flags);
    }

    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE type,
                                             UINT size, void *metadata) override {
      if (type == DXGI_HDR_METADATA_TYPE_NONE)
        return S_OK;
      WARN("D3D12SwapChain::SetHDRMetaData: HDR metadata is not supported");
      return DXGI_ERROR_UNSUPPORTED;
    }

  private:
    bool HasExternalBackBufferReferences() {
      for (auto &backbuffer : backbuffers_) {
        if (!backbuffer)
          continue;
        ULONG ref_count = backbuffer->AddRef();
        backbuffer->Release();
        if (ref_count > 3)
          return true;
      }
      return false;
    }

    HRESULT GetOutputFromMonitor(HMONITOR monitor, IDXGIOutput **output) {
      Com<IDXGIAdapter> adapter;
      Com<IDXGIOutput> candidate;
      if (FAILED(queue_->device_->GetAdapter(&adapter)))
        return E_FAIL;

      for (UINT i = 0; SUCCEEDED(adapter->EnumOutputs(i, &candidate)); i++) {
        DXGI_OUTPUT_DESC desc = {};
        if (SUCCEEDED(candidate->GetDesc(&desc)) && desc.Monitor == monitor)
          return candidate->QueryInterface(IID_PPV_ARGS(output));
        candidate = nullptr;
      }
      return DXGI_ERROR_NOT_FOUND;
    }

    Com<CommandQueueImpl> queue_;
    Com<IDXGIFactory1> factory_;
    ComPrivateData private_data_;
    HWND hWnd_ = nullptr;
    DXGI_SWAP_CHAIN_DESC1 desc_ = {};
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC fullscreen_desc_ = {};
    std::vector<Com<ID3D12Resource>> backbuffers_;
    UINT current_backbuffer_ = 0;
    UINT presentation_count_ = 0;
    WMT::Object native_view_;
    WMT::MetalLayer layer_;
    Rc<Presenter> presenter_;
    HUDState hud_;
    Com<IDXGIOutput> target_;
    UINT frame_latency_ = 1;
    UINT source_width_ = 0;
    UINT source_height_ = 0;
    DXGI_RGBA background_color_ = {};
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    DXGI_MATRIX_3X2_F matrix_ = {1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    DXGI_COLOR_SPACE_TYPE color_space_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    HANDLE present_semaphore_ = nullptr;
  };

  struct ReplayState {
    std::unordered_map<ID3D12Resource *,
                       std::vector<D3D12_RESOURCE_STATES>> *resource_states =
        nullptr;
    Com<ID3D12PipelineState> pipeline_state;
    Com<ID3D12RootSignature> graphics_root_signature;
    Com<ID3D12RootSignature> compute_root_signature;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    std::vector<D3D12_VIEWPORT> viewports;
    std::vector<D3D12_RECT> scissors;
    std::array<FLOAT, 4> blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
    UINT stencil_ref = 0;
    std::vector<DescriptorRecord> render_targets;
    std::optional<DescriptorRecord> depth_stencil;
    std::array<std::optional<D3D12_VERTEX_BUFFER_VIEW>, 32> vertex_buffers = {};
    std::optional<D3D12_INDEX_BUFFER_VIEW> index_buffer;
    Com<ID3D12DescriptorHeap> cbv_srv_uav_heap;
    Com<ID3D12DescriptorHeap> sampler_heap;
    std::unordered_map<UINT, D3D12_GPU_DESCRIPTOR_HANDLE> graphics_tables;
    std::unordered_map<UINT, D3D12_GPU_DESCRIPTOR_HANDLE> compute_tables;
    std::unordered_map<UINT, std::vector<UINT>> graphics_root_constants;
    std::unordered_map<UINT, std::vector<UINT>> compute_root_constants;
    std::unordered_map<UINT, D3D12_GPU_VIRTUAL_ADDRESS> graphics_cbv_roots;
    std::unordered_map<UINT, D3D12_GPU_VIRTUAL_ADDRESS> compute_cbv_roots;
    std::unordered_map<UINT, D3D12_GPU_VIRTUAL_ADDRESS> graphics_srv_roots;
    std::unordered_map<UINT, D3D12_GPU_VIRTUAL_ADDRESS> compute_srv_roots;
    std::unordered_map<UINT, D3D12_GPU_VIRTUAL_ADDRESS> graphics_uav_roots;
    std::unordered_map<UINT, D3D12_GPU_VIRTUAL_ADDRESS> compute_uav_roots;
    Com<ID3D12Resource> predication_buffer;
    UINT64 predication_buffer_offset = 0;
    D3D12_PREDICATION_OP predication_operation =
        D3D12_PREDICATION_OP_EQUAL_ZERO;
  };

  struct ReplayRenderTargetAttachment {
    Rc<Texture> texture;
    TextureViewKey view = {};
    UINT slot = 0;
    UINT array_length = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    WMTPixelFormat format = WMTPixelFormatInvalid;
  };

  struct ReplayDepthStencilAttachment {
    Rc<Texture> texture;
    TextureViewKey view = {};
    UINT array_length = 1;
    uint32_t width = 0;
    uint32_t height = 0;
    WMTPixelFormat format = WMTPixelFormatInvalid;
  };

  struct ReplayRenderPassAttachments {
    std::vector<ReplayRenderTargetAttachment> colors;
    std::optional<ReplayDepthStencilAttachment> depth_stencil;
  };

  void ReplayCommandRecords(const std::vector<CommandRecord> &records) {
    auto *chunk = device_->GetDXMTDevice().queue().CurrentChunk();
    ReplayState state = {};
    state.resource_states = &resource_states_;
    for (const auto &record : records) {
      std::visit([&](const auto &payload) { ReplayRecord(chunk, state, payload); },
                 record.payload);
    }
  }

  template <typename T>
  void ReplayRecord(CommandChunk *chunk, ReplayState &state, const T &record) {
    if constexpr (std::is_same_v<T, CopyBufferRegionRecord>) {
      ReplayCopyBufferRegion(chunk, record);
    } else if constexpr (std::is_same_v<T, CopyTextureRegionRecord>) {
      ReplayCopyTextureRegion(chunk, record);
    } else if constexpr (std::is_same_v<T, CopyResourceRecord>) {
      ReplayCopyResource(chunk, record);
    } else if constexpr (std::is_same_v<T, ResolveSubresourceRecord>) {
      ReplayResolveSubresource(chunk, record);
    } else if constexpr (std::is_same_v<T, ClearRenderTargetRecord>) {
      ReplayClearRenderTarget(chunk, record);
    } else if constexpr (std::is_same_v<T, ClearDepthStencilRecord>) {
      ReplayClearDepthStencil(chunk, record);
    } else if constexpr (std::is_same_v<T, ClearUnorderedAccessRecord>) {
      ReplayClearUnorderedAccess(chunk, record);
    } else if constexpr (std::is_same_v<T, DiscardResourceRecord>) {
      ReplayDiscardResource(chunk, record);
    } else if constexpr (std::is_same_v<T, PipelineStateRecord>) {
      state.pipeline_state = record.pipeline_state;
    } else if constexpr (std::is_same_v<T, PrimitiveTopologyRecord>) {
      state.topology = record.topology;
    } else if constexpr (std::is_same_v<T, ViewportRecord>) {
      state.viewports = record.viewports;
    } else if constexpr (std::is_same_v<T, ScissorRecord>) {
      state.scissors = record.rects;
    } else if constexpr (std::is_same_v<T, BlendFactorRecord>) {
      state.blend_factor = record.blend_factor;
    } else if constexpr (std::is_same_v<T, StencilRefRecord>) {
      state.stencil_ref = record.stencil_ref;
    } else if constexpr (std::is_same_v<T, RenderTargetsRecord>) {
      state.render_targets = record.render_targets;
      state.depth_stencil = record.depth_stencil;
    } else if constexpr (std::is_same_v<T, DescriptorHeapsRecord>) {
      state.cbv_srv_uav_heap = nullptr;
      state.sampler_heap = nullptr;
      for (const auto &heap : record.heaps) {
        auto *descriptor_heap = dynamic_cast<DescriptorHeap *>(heap.ptr());
        if (!descriptor_heap)
          continue;
        const auto &desc = descriptor_heap->GetDescriptorHeapDesc();
        if (!(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
          continue;
        if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
          state.cbv_srv_uav_heap = heap;
        else if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
          state.sampler_heap = heap;
      }
    } else if constexpr (std::is_same_v<T, VertexBuffersRecord>) {
      for (UINT i = 0; i < record.views.size() &&
                       record.start_slot + i < state.vertex_buffers.size();
           i++)
        state.vertex_buffers[record.start_slot + i] = record.views[i];
    } else if constexpr (std::is_same_v<T, IndexBufferRecord>) {
      state.index_buffer = record.view;
    } else if constexpr (std::is_same_v<T, ResourceBarrierRecord>) {
      ReplayResourceBarrier(chunk, state, record);
    } else if constexpr (std::is_same_v<T, RootSignatureRecord>) {
      if (record.compute)
        state.compute_root_signature = record.root_signature;
      else
        state.graphics_root_signature = record.root_signature;
    } else if constexpr (std::is_same_v<T, RootDescriptorTableRecord>) {
      auto &tables = record.compute ? state.compute_tables
                                    : state.graphics_tables;
      tables[record.root_parameter_index] = record.base_descriptor;
    } else if constexpr (std::is_same_v<T, RootDescriptorRecord>) {
      StoreRootDescriptor(state, record);
    } else if constexpr (std::is_same_v<T, RootConstantsRecord>) {
      StoreRootConstants(state, record);
    } else if constexpr (std::is_same_v<T, BeginQueryRecord>) {
      ReplayBeginQuery(chunk, record);
    } else if constexpr (std::is_same_v<T, EndQueryRecord>) {
      ReplayEndQuery(chunk, record);
    } else if constexpr (std::is_same_v<T, ResolveQueryDataRecord>) {
      ReplayResolveQueryData(chunk, record);
    } else if constexpr (std::is_same_v<T, PredicationRecord>) {
      ReplaySetPredication(state, record);
    } else if constexpr (std::is_same_v<T, WriteBufferImmediateRecord>) {
      ReplayWriteBufferImmediate(record);
    } else if constexpr (std::is_same_v<T, ExecuteIndirectRecord>) {
      ReplayExecuteIndirect(chunk, state, record);
    } else if constexpr (std::is_same_v<T, DrawInstancedRecord>) {
      ReplayDrawInstanced(chunk, state, record);
    } else if constexpr (std::is_same_v<T, DrawIndexedInstancedRecord>) {
      ReplayDrawIndexedInstanced(chunk, state, record);
    } else if constexpr (std::is_same_v<T, DispatchRecord>) {
      ReplayDispatch(chunk, state, record);
    }
  }

  void ReplayCopyBufferRegion(CommandChunk *chunk,
                              const CopyBufferRegionRecord &record) {
    auto *dst = GetResource(record.dst.ptr());
    auto *src = GetResource(record.src.ptr());
    if (!dst || !src || !dst->GetBufferAllocation() ||
        !src->GetBufferAllocation())
      return;

    Rc<BufferAllocation> dst_allocation = dst->GetBufferAllocation();
    Rc<BufferAllocation> src_allocation = src->GetBufferAllocation();
    const UINT64 src_offset = src->GetHeapOffset() + record.src_offset;
    const UINT64 dst_offset = dst->GetHeapOffset() + record.dst_offset;
    chunk->emitcc([dst_allocation, src_allocation, src_offset, dst_offset,
                   byte_count = record.byte_count](ArgumentEncodingContext &enc) {
      enc.retainAllocation(dst_allocation.ptr());
      enc.retainAllocation(src_allocation.ptr());
      enc.startBlitPass();
      auto &copy =
          enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      copy.type = WMTBlitCommandCopyFromBufferToBuffer;
      copy.src = src_allocation->buffer();
      copy.src_offset = src_offset;
      copy.dst = dst_allocation->buffer();
      copy.dst_offset = dst_offset;
      copy.copy_length = byte_count;
      enc.endPass();
    });
  }

  void ReplayResourceBarrier(CommandChunk *chunk, ReplayState &state,
                             const ResourceBarrierRecord &record) {
    if (record.barriers.empty())
      return;

    for (const auto &barrier : record.barriers) {
      switch (barrier.barrier.Type) {
      case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
        ReplayTransitionBarrier(chunk, state, barrier);
        break;
      case D3D12_RESOURCE_BARRIER_TYPE_UAV:
        ReplayUavBarrier(chunk, barrier);
        break;
      case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
        ReplayAliasingBarrier(chunk, barrier);
        break;
      default:
        WARN("D3D12CommandQueue: unsupported resource barrier type ",
             barrier.barrier.Type);
        EmitPassSeparator(chunk);
        break;
      }
    }
  }

  void ReplayWriteBufferImmediate(const WriteBufferImmediateRecord &record) {
    for (size_t i = 0; i < record.parameters.size(); i++) {
      Resource *resource = nullptr;
      const UINT64 offset =
          ResolveBufferGpuAddress(record.parameters[i].Dest, resource);
      if (!resource || !resource->GetBufferAllocation()) {
        WARN("D3D12CommandQueue: WriteBufferImmediate skipped unknown "
             "destination");
        continue;
      }

      resource->GetBufferAllocation()->updateContents(
          resource->GetHeapOffset() + offset, &record.parameters[i].Value,
          sizeof(record.parameters[i].Value));
    }
  }

  std::vector<D3D12_RESOURCE_STATES> &
  GetReplayResourceStates(ReplayState &state, Resource &resource) {
    auto &states = (*state.resource_states)[resource.GetD3D12Resource()];
    const UINT subresource_count = GetSubresourceCount(resource);
    if (states.size() != subresource_count)
      states.assign(subresource_count, resource.GetInitialState());
    return states;
  }

  void ReplayTransitionBarrier(CommandChunk *chunk, ReplayState &state,
                               const StoredResourceBarrier &barrier) {
    auto *resource = GetResource(barrier.resource.ptr());
    if (!resource) {
      WARN("D3D12CommandQueue: transition barrier skipped for foreign resource");
      EmitPassSeparator(chunk);
      return;
    }

    const auto &transition = barrier.barrier.Transition;
    WarnUnsupportedResourceState(transition.StateBefore, "transition before");
    WarnUnsupportedResourceState(transition.StateAfter, "transition after");

    auto &states = GetReplayResourceStates(state, *resource);
    const UINT subresource_count = states.size();
    const bool all_subresources =
        transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (!all_subresources && transition.Subresource >= subresource_count) {
      WARN("D3D12CommandQueue: transition barrier subresource out of range ",
           transition.Subresource, " count=", subresource_count);
      EmitPassSeparator(chunk);
      return;
    }

    const UINT first = all_subresources ? 0 : transition.Subresource;
    const UINT count = all_subresources ? subresource_count : 1;
    for (UINT i = 0; i < count; i++) {
      const UINT subresource = first + i;
      if (states[subresource] != transition.StateBefore) {
        WARN("D3D12CommandQueue: transition barrier state mismatch subresource=",
             subresource, " expected=", uint32_t(states[subresource]),
             " before=", uint32_t(transition.StateBefore));
      }
      states[subresource] = transition.StateAfter;
    }

    const int before_access = ResourceAccessForState(transition.StateBefore);
    const int after_access = ResourceAccessForState(transition.StateAfter);
    int access = before_access | after_access;
    if (!access)
      access = ResourceAccess::All;
    EmitResourceAccessBarrier(chunk, *resource, first, count, access);
  }

  void ReplayUavBarrier(CommandChunk *chunk,
                        const StoredResourceBarrier &barrier) {
    if (barrier.resource) {
      auto *resource = GetResource(barrier.resource.ptr());
      if (!resource) {
        WARN("D3D12CommandQueue: UAV barrier skipped for foreign resource");
        EmitPassSeparator(chunk);
        return;
      }
      EmitResourceAccessBarrier(chunk, *resource, 0,
                                GetSubresourceCount(*resource),
                                ResourceAccess::All);
      return;
    }

    EmitPassSeparator(chunk);
  }

  void ReplayAliasingBarrier(CommandChunk *chunk,
                             const StoredResourceBarrier &barrier) {
    bool touched = false;
    if (auto *before = GetResource(barrier.resource_before.ptr())) {
      EmitResourceAccessBarrier(chunk, *before, 0,
                                GetSubresourceCount(*before),
                                ResourceAccess::All);
      touched = true;
    } else if (barrier.resource_before) {
      WARN("D3D12CommandQueue: aliasing barrier has foreign before resource");
    }

    if (auto *after = GetResource(barrier.resource_after.ptr())) {
      EmitResourceAccessBarrier(chunk, *after, 0, GetSubresourceCount(*after),
                                ResourceAccess::All);
      touched = true;
    } else if (barrier.resource_after) {
      WARN("D3D12CommandQueue: aliasing barrier has foreign after resource");
    }

    if (!touched)
      EmitPassSeparator(chunk);
  }

  void EmitResourceAccessBarrier(CommandChunk *chunk, Resource &resource,
                                 UINT first_subresource,
                                 UINT subresource_count, int access) {
    if (resource.GetBuffer()) {
      Rc<Buffer> buffer = resource.GetBuffer();
      const UINT length =
          UINT(std::min<UINT64>(resource.GetResourceDesc().Width, UINT_MAX));
      chunk->emitcc([buffer = std::move(buffer), length, access](ArgumentEncodingContext &enc) {
        enc.startBlitPass();
        enc.access(buffer, 0, length, access);
        enc.endPass();
      });
      return;
    }

    if (resource.GetTexture()) {
      Rc<Texture> texture = resource.GetTexture();
      std::vector<std::pair<UINT, UINT>> subresources;
      subresources.reserve(subresource_count);
      for (UINT i = 0; i < subresource_count; i++) {
        const UINT subresource = first_subresource + i;
        subresources.push_back({GetMipLevel(resource, subresource),
                                GetArraySlice(resource, subresource)});
      }
      chunk->emitcc([texture = std::move(texture),
                     subresources = std::move(subresources),
                     access](ArgumentEncodingContext &enc) mutable {
        enc.startBlitPass();
        for (const auto &[level, slice] : subresources)
          enc.access(texture, level, slice, access);
        enc.endPass();
      });
      return;
    }

    EmitPassSeparator(chunk);
  }

  void EmitPassSeparator(CommandChunk *chunk) {
    chunk->emitcc([](ArgumentEncodingContext &enc) {
      enc.startBlitPass();
      enc.endPass();
    });
  }

  void StoreRootDescriptor(ReplayState &state,
                           const RootDescriptorRecord &record) {
    auto &map = [&]() -> std::unordered_map<UINT, D3D12_GPU_VIRTUAL_ADDRESS> & {
      switch (record.parameter_type) {
      case D3D12_ROOT_PARAMETER_TYPE_CBV:
        return record.compute ? state.compute_cbv_roots
                              : state.graphics_cbv_roots;
      case D3D12_ROOT_PARAMETER_TYPE_UAV:
        return record.compute ? state.compute_uav_roots
                              : state.graphics_uav_roots;
      case D3D12_ROOT_PARAMETER_TYPE_SRV:
      default:
        return record.compute ? state.compute_srv_roots
                              : state.graphics_srv_roots;
      }
    }();
    map[record.root_parameter_index] = record.address;
  }

  void StoreRootConstants(ReplayState &state, const RootConstantsRecord &record) {
    auto &map = record.compute ? state.compute_root_constants
                               : state.graphics_root_constants;
    auto &values = map[record.root_parameter_index];
    const auto required_size =
        record.dst_offset + static_cast<UINT>(record.values.size());
    if (values.size() < required_size)
      values.resize(required_size, 0);
    std::copy(record.values.begin(), record.values.end(),
              values.begin() + record.dst_offset);
  }

  bool CurrentPipelineIsCompute(const ReplayState &state) const {
    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    return pipeline && pipeline->GetType() == PipelineStateType::Compute;
  }

  bool PredicationAllows(const ReplayState &state) const {
    if (!state.predication_buffer)
      return true;

    uint64_t value = 0;
    if (!ReadBufferBytes(state.predication_buffer.ptr(),
                         state.predication_buffer_offset, &value,
                         sizeof(value), "predication")) {
      WARN("D3D12CommandQueue: predication buffer is unavailable; command will be skipped");
      return false;
    }

    switch (state.predication_operation) {
    case D3D12_PREDICATION_OP_EQUAL_ZERO:
      return value == 0;
    case D3D12_PREDICATION_OP_NOT_EQUAL_ZERO:
      return value != 0;
    default:
      WARN("D3D12CommandQueue: unsupported predication operation ",
           state.predication_operation);
      return false;
    }
  }

  void ReplayBeginQuery(CommandChunk *chunk, const BeginQueryRecord &record) {
    auto *heap = dynamic_cast<QueryHeap *>(record.heap.ptr());
    if (!heap) {
      WARN("D3D12CommandQueue: BeginQuery skipped for foreign query heap");
      return;
    }
    if (record.type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||
        record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 ||
        record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1 ||
        record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2 ||
        record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
      heap->BeginStatistics(record.type, record.index);
      return;
    }
    auto query = heap->BeginVisibility(record.type, record.index);
    if (!query)
      return;
  }

  void ReplayEndQuery(CommandChunk *chunk, const EndQueryRecord &record) {
    auto *heap = dynamic_cast<QueryHeap *>(record.heap.ptr());
    if (!heap) {
      WARN("D3D12CommandQueue: EndQuery skipped for foreign query heap");
      return;
    }
    if (record.type == D3D12_QUERY_TYPE_TIMESTAMP) {
      auto query = heap->EndTimestamp(record.type, record.index);
      if (!query)
        return;
      return;
    }
    if (record.type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||
        record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 ||
        record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1 ||
        record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2 ||
        record.type == D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3) {
      heap->EndStatistics(record.type, record.index);
      return;
    }
    auto query = heap->EndVisibility(record.type, record.index);
    if (!query)
      return;
  }

  void ReplayResolveQueryData(CommandChunk *chunk,
                              const ResolveQueryDataRecord &record) {
    auto *heap = dynamic_cast<QueryHeap *>(record.heap.ptr());
    if (!heap) {
      WARN("D3D12CommandQueue: ResolveQueryData skipped for foreign query heap");
      return;
    }

    std::vector<uint8_t> sizing_data;
    if (!heap->Resolve(record.type, record.start_index, record.query_count,
                       sizing_data))
      return;
    auto *dst = GetResource(record.dst_buffer.ptr());
    if (!ValidateBufferRange(dst, record.dst_buffer_offset, sizing_data.size(),
                             "query resolve"))
      return;

    Com<ID3D12QueryHeap> heap_ref = record.heap;
    Com<ID3D12Resource> dst_ref = record.dst_buffer;
    const auto type = record.type;
    const UINT start_index = record.start_index;
    const UINT query_count = record.query_count;
    const UINT64 dst_buffer_offset = record.dst_buffer_offset;
    chunk->deferred_readbacks.push_back(
        [heap_ref = std::move(heap_ref), dst_ref = std::move(dst_ref), type,
         start_index, query_count, dst_buffer_offset]() mutable {
          auto *heap = dynamic_cast<QueryHeap *>(heap_ref.ptr());
          auto *dst = GetResource(dst_ref.ptr());
          if (!heap || !dst)
            return;

          std::vector<uint8_t> data;
          if (!heap->Resolve(type, start_index, query_count, data))
            return;
          if (!ValidateBufferRange(dst, dst_buffer_offset, data.size(),
                                   "query resolve"))
            return;
          if (!data.empty())
            dst->GetBufferAllocation()->updateContents(
                dst->GetHeapOffset() + dst_buffer_offset, data.data(),
                data.size());
        });
  }

  void ReplaySetPredication(ReplayState &state,
                            const PredicationRecord &record) {
    state.predication_buffer = record.buffer;
    state.predication_buffer_offset = record.buffer_offset;
    state.predication_operation = record.operation;
  }

  void ReplayExecuteIndirect(CommandChunk *chunk, ReplayState &state,
                             const ExecuteIndirectRecord &record) {
    if (!PredicationAllows(state))
      return;

    auto *signature =
        dynamic_cast<CommandSignature *>(record.command_signature.ptr());
    if (!signature) {
      WARN("D3D12CommandQueue: ExecuteIndirect skipped for foreign command signature");
      return;
    }

    const auto &desc = signature->GetDesc();
    const auto &arguments = signature->GetArguments();
    if (!desc.ByteStride || arguments.empty())
      return;
    if (RequiresRootSignature(arguments) && !signature->GetRootSignature()) {
      WARN("D3D12CommandQueue: ExecuteIndirect skipped because command signature has root arguments but no root signature");
      return;
    }

    const auto direct_operation = GetDirectIndirectOperation(arguments);
    if (direct_operation != DirectIndirectOperation::None &&
        ReplayExecuteIndirectDirect(chunk, state, record, desc, arguments[0],
                                    direct_operation))
      return;

    ReplayExecuteIndirectCpuFallback(chunk, state, record, *signature);
  }

  static bool RequiresRootSignature(
      const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &arguments) {
    for (const auto &argument : arguments) {
      switch (argument.Type) {
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT:
      case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW:
      case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW:
        return true;
      default:
        break;
      }
    }
    return false;
  }

  bool ReplayExecuteIndirectDirect(
      CommandChunk *chunk, ReplayState &state,
      const ExecuteIndirectRecord &record,
      const D3D12_COMMAND_SIGNATURE_DESC &desc,
      const D3D12_INDIRECT_ARGUMENT_DESC &argument,
      DirectIndirectOperation operation) {
    const UINT command_count = record.max_command_count;
    if (!command_count)
      return true;

    const UINT argument_size = IndirectArgumentByteSize(argument);
    if (!argument_size || desc.ByteStride < argument_size) {
      WARN("D3D12CommandQueue: ExecuteIndirect argument layout exceeds stride");
      return true;
    }

    auto *arg_resource = GetResource(record.arg_buffer.ptr());
    if (!ValidateBufferRange(
            arg_resource, record.arg_buffer_offset,
            UINT64(command_count - 1) * desc.ByteStride + argument_size,
            "indirect argument buffer"))
      return true;

    Rc<Buffer> arg_buffer = arg_resource->GetBuffer();
    const UINT64 arg_base_offset =
        arg_resource->GetHeapOffset() + record.arg_buffer_offset;

    Rc<Buffer> count_buffer;
    UINT64 count_offset = 0;
    WMT::Reference<WMT::Buffer> counted_args;
    if (record.count_buffer) {
      auto *count_resource = GetResource(record.count_buffer.ptr());
      if (!ValidateBufferRange(count_resource, record.count_buffer_offset,
                               sizeof(UINT), "indirect count buffer"))
        return true;

      count_buffer = count_resource->GetBuffer();
      count_offset = count_resource->GetHeapOffset() + record.count_buffer_offset;

      WMTBufferInfo counted_info = {};
      counted_info.length = UINT64(command_count) * argument_size;
      counted_info.options = WMTResourceStorageModePrivate |
                             WMTResourceHazardTrackingModeTracked;
      counted_args =
          device_->GetDXMTDevice().device().newBuffer(counted_info);
      if (!counted_args) {
        WARN("D3D12CommandQueue: ExecuteIndirect failed to allocate counted argument buffer");
        return true;
      }
    }

    for (UINT command_index = 0; command_index < command_count;
         command_index++) {
      const UINT64 arg_offset =
          arg_base_offset + UINT64(command_index) * desc.ByteStride;
      const UINT64 counted_offset = UINT64(command_index) * argument_size;

      switch (operation) {
      case DirectIndirectOperation::Draw:
        ReplayDrawInstancedIndirect(chunk, state, arg_buffer, arg_offset,
                                    count_buffer, count_offset, counted_args,
                                    counted_offset, argument_size,
                                    command_index);
        break;
      case DirectIndirectOperation::DrawIndexed:
        ReplayDrawIndexedInstancedIndirect(
            chunk, state, arg_buffer, arg_offset, count_buffer, count_offset,
            counted_args, counted_offset, argument_size, command_index);
        break;
      case DirectIndirectOperation::Dispatch:
        ReplayDispatchIndirect(chunk, state, arg_buffer, arg_offset,
                               count_buffer, count_offset, counted_args,
                               counted_offset, argument_size, command_index);
        break;
      default:
        return false;
      }
    }
    return true;
  }

  void ReplayExecuteIndirectCpuFallback(CommandChunk *chunk, ReplayState &state,
                                        const ExecuteIndirectRecord &record,
                                        const CommandSignature &signature) {
    UINT command_count = record.max_command_count;
    if (record.count_buffer) {
      UINT count = 0;
      if (!ReadBufferBytes(record.count_buffer.ptr(), record.count_buffer_offset,
                           &count, sizeof(count), "indirect count buffer")) {
        WARN("D3D12CommandQueue: ExecuteIndirect complex command signature requires CPU-visible buffers until GPU state-command lowering is implemented");
        return;
      }
      command_count = std::min(command_count, count);
    }
    if (!command_count)
      return;

    const auto &desc = signature.GetDesc();
    const auto &arguments = signature.GetArguments();

    std::vector<uint8_t> command(desc.ByteStride);
    for (UINT command_index = 0; command_index < command_count;
         command_index++) {
      const UINT64 command_offset =
          record.arg_buffer_offset + UINT64(command_index) * desc.ByteStride;
      if (!ReadBufferBytes(record.arg_buffer.ptr(), command_offset,
                           command.data(), command.size(),
                           "indirect argument buffer")) {
        WARN("D3D12CommandQueue: ExecuteIndirect complex command signature requires CPU-visible buffers until GPU state-command lowering is implemented");
        return;
      }

      size_t argument_offset = 0;
      for (const auto &argument : arguments) {
        const auto argument_size = IndirectArgumentByteSize(argument);
        if (!argument_size || argument_offset + argument_size > command.size()) {
          WARN("D3D12CommandQueue: ExecuteIndirect argument layout exceeds stride");
          return;
        }

        const auto *bytes = command.data() + argument_offset;
        const bool compute = CurrentPipelineIsCompute(state);
        switch (argument.Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW: {
          D3D12_DRAW_ARGUMENTS args = {};
          std::memcpy(&args, bytes, sizeof(args));
          ReplayDrawInstanced(chunk, state,
                              DrawInstancedRecord{
                                  args.VertexCountPerInstance,
                                  args.InstanceCount,
                                  args.StartVertexLocation,
                                  args.StartInstanceLocation});
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED: {
          D3D12_DRAW_INDEXED_ARGUMENTS args = {};
          std::memcpy(&args, bytes, sizeof(args));
          ReplayDrawIndexedInstanced(
              chunk, state,
              DrawIndexedInstancedRecord{
                  args.IndexCountPerInstance, args.InstanceCount,
                  args.StartIndexLocation, args.BaseVertexLocation,
                  args.StartInstanceLocation});
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH: {
          D3D12_DISPATCH_ARGUMENTS args = {};
          std::memcpy(&args, bytes, sizeof(args));
          ReplayDispatch(chunk, state,
                         DispatchRecord{args.ThreadGroupCountX,
                                        args.ThreadGroupCountY,
                                        args.ThreadGroupCountZ});
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW: {
          D3D12_VERTEX_BUFFER_VIEW view = {};
          std::memcpy(&view, bytes, sizeof(view));
          if (argument.VertexBuffer.Slot < state.vertex_buffers.size())
            state.vertex_buffers[argument.VertexBuffer.Slot] = view;
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW: {
          D3D12_INDEX_BUFFER_VIEW view = {};
          std::memcpy(&view, bytes, sizeof(view));
          state.index_buffer = view;
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT: {
          RootConstantsRecord constants = {};
          constants.compute = compute;
          constants.root_parameter_index =
              argument.Constant.RootParameterIndex;
          constants.dst_offset =
              argument.Constant.DestOffsetIn32BitValues;
          constants.values.resize(argument.Constant.Num32BitValuesToSet);
          if (!constants.values.empty())
            std::memcpy(constants.values.data(), bytes,
                        constants.values.size() * sizeof(UINT));
          StoreRootConstants(state, constants);
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW: {
          D3D12_GPU_VIRTUAL_ADDRESS address = 0;
          std::memcpy(&address, bytes, sizeof(address));
          StoreRootDescriptor(
              state, RootDescriptorRecord{
                         compute, D3D12_ROOT_PARAMETER_TYPE_CBV,
                         argument.ConstantBufferView.RootParameterIndex,
                         address});
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW: {
          D3D12_GPU_VIRTUAL_ADDRESS address = 0;
          std::memcpy(&address, bytes, sizeof(address));
          StoreRootDescriptor(
              state, RootDescriptorRecord{
                         compute, D3D12_ROOT_PARAMETER_TYPE_SRV,
                         argument.ShaderResourceView.RootParameterIndex,
                         address});
          break;
        }
        case D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW: {
          D3D12_GPU_VIRTUAL_ADDRESS address = 0;
          std::memcpy(&address, bytes, sizeof(address));
          StoreRootDescriptor(
              state, RootDescriptorRecord{
                         compute, D3D12_ROOT_PARAMETER_TYPE_UAV,
                         argument.UnorderedAccessView.RootParameterIndex,
                         address});
          break;
        }
        default:
          WARN("D3D12CommandQueue: unsupported ExecuteIndirect argument type ",
               argument.Type);
          return;
        }
        argument_offset += argument_size;
      }
    }
  }

  void PrepareCountedIndirectArguments(
      ArgumentEncodingContext &enc, const Rc<Buffer> &arg_buffer,
      UINT64 arg_offset, const Rc<Buffer> &count_buffer, UINT64 count_offset,
      WMT::Buffer counted_args, UINT64 counted_offset, UINT argument_size,
      UINT command_index) {
    enc.startComputePass(0);
    auto [arg_allocation, arg_sub_offset] =
        enc.access<PipelineStage::Compute>(arg_buffer, arg_offset,
                                           argument_size, ResourceAccess::Read);
    auto [count_allocation, count_sub_offset] =
        enc.access<PipelineStage::Compute>(count_buffer, count_offset,
                                           sizeof(UINT), ResourceAccess::Read);
    enc.emulated_cmd.PrepareCountedIndirectArguments(
        count_allocation->buffer(), count_sub_offset + count_offset,
        arg_allocation->buffer(), arg_sub_offset + arg_offset, counted_args,
        counted_offset, argument_size, command_index);
    enc.endPass();
  }

  void ReplayDrawInstancedIndirect(
      CommandChunk *chunk, ReplayState &state, Rc<Buffer> arg_buffer,
      UINT64 arg_offset, Rc<Buffer> count_buffer, UINT64 count_offset,
      WMT::Reference<WMT::Buffer> counted_args, UINT64 counted_offset,
      UINT argument_size, UINT command_index) {
    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: indirect draw skipped without graphics pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalGraphicsState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: indirect draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    const auto primitive = GetPrimitiveType(state.topology);
    if (!primitive) {
      WARN("D3D12CommandQueue: indirect draw skipped because primitive topology is unsupported topology=",
           uint32_t(state.topology));
      return;
    }
    auto viewports = state.viewports;
    auto scissors = state.scissors;
    auto attachments = BuildRenderPassAttachments(state);
    if (!ResolveDynamicRasterRects(viewports, scissors, "indirect draw"))
      return;
    const auto argument_buffer_size = EstimateGraphicsArgumentBufferSize(*pipeline);
    chunk->emitcc([this, metal_pso = metal->pso,
                   depth_stencil = metal->depth_stencil,
                   rasterizer = metal->rasterizer, pipeline,
                   replay_state = state, primitive, argument_buffer_size,
                   blend_factor = state.blend_factor,
                   stencil_ref = state.stencil_ref, arg_buffer, arg_offset,
                   count_buffer, count_offset, counted_args, counted_offset,
                   argument_size, command_index,
                   viewports = std::move(viewports),
                   scissors = std::move(scissors),
                   attachments = std::move(attachments)](ArgumentEncodingContext &enc) mutable {
      const bool has_count = count_buffer.ptr();
      if (has_count) {
        PrepareCountedIndirectArguments(
            enc, arg_buffer, arg_offset, count_buffer, count_offset,
            counted_args, counted_offset, argument_size, command_index);
      }

      if (!BeginRenderPass(enc, attachments, argument_buffer_size))
        return;

      auto &set_pso = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      set_pso.type = WMTRenderCommandSetPSO;
      set_pso.pso = metal_pso;
      if (depth_stencil) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
        cmd.type = WMTRenderCommandSetDSSO;
        cmd.dsso = depth_stencil;
        cmd.stencil_ref = static_cast<uint8_t>(stencil_ref);
      }
      auto &rs = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      rs = rasterizer;

      uint64_t argbuf_offset = 0;
      EncodeGraphicsBindings(enc, replay_state, *pipeline, argbuf_offset);
      EncodeDynamicRenderState(enc, viewports, scissors, blend_factor,
                               stencil_ref);

      WMT::Buffer indirect_buffer = counted_args;
      UINT64 indirect_offset = counted_offset;
      if (!has_count) {
        auto [arg_allocation, arg_sub_offset] =
            enc.access<PipelineStage::Vertex>(arg_buffer, arg_offset,
                                              argument_size,
                                              ResourceAccess::Read);
        indirect_buffer = arg_allocation->buffer();
        indirect_offset = arg_sub_offset + arg_offset;
      }

      auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw_indirect>();
      draw.type = WMTRenderCommandDrawIndirect;
      draw.primitive_type = *primitive;
      draw.indirect_args_buffer = indirect_buffer;
      draw.indirect_args_offset = indirect_offset;
      enc.endPass();
    });
  }

  void ReplayDrawIndexedInstancedIndirect(
      CommandChunk *chunk, ReplayState &state, Rc<Buffer> arg_buffer,
      UINT64 arg_offset, Rc<Buffer> count_buffer, UINT64 count_offset,
      WMT::Reference<WMT::Buffer> counted_args, UINT64 counted_offset,
      UINT argument_size, UINT command_index) {
    if (!state.index_buffer)
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped without graphics pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalGraphicsState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    UINT64 index_resource_offset = 0;
    auto *index_resource = LookupBufferResourceByGpuVirtualAddress(
        state.index_buffer->BufferLocation, &index_resource_offset);
    if (!index_resource || !index_resource->GetBufferAllocation()) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped because index buffer binding is unavailable");
      return;
    }
    if (!IsSupportedIndexBufferFormat(state.index_buffer->Format)) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped because index buffer format is unsupported format=",
           uint32_t(state.index_buffer->Format));
      return;
    }

    Rc<BufferAllocation> index_allocation = index_resource->GetBufferAllocation();
    const auto primitive = GetPrimitiveType(state.topology);
    if (!primitive) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped because primitive topology is unsupported topology=",
           uint32_t(state.topology));
      return;
    }
    const auto index_type = GetIndexType(state.index_buffer->Format);
    const UINT64 index_offset =
        index_resource->GetHeapOffset() + index_resource_offset;
    auto attachments = BuildRenderPassAttachments(state);
    auto viewports = state.viewports;
    auto scissors = state.scissors;
    if (!ResolveDynamicRasterRects(viewports, scissors, "indirect indexed draw"))
      return;
    const auto argument_buffer_size = EstimateGraphicsArgumentBufferSize(*pipeline);
    chunk->emitcc([this, metal_pso = metal->pso,
                   depth_stencil = metal->depth_stencil,
                   rasterizer = metal->rasterizer, pipeline,
                   replay_state = state, index_allocation, primitive,
                   index_type, index_offset, argument_buffer_size,
                   blend_factor = state.blend_factor,
                   stencil_ref = state.stencil_ref, arg_buffer, arg_offset,
                   count_buffer, count_offset, counted_args, counted_offset,
                   argument_size, command_index,
                   viewports = std::move(viewports),
                   scissors = std::move(scissors),
                   attachments = std::move(attachments)](ArgumentEncodingContext &enc) mutable {
      const bool has_count = count_buffer.ptr();
      if (has_count) {
        PrepareCountedIndirectArguments(
            enc, arg_buffer, arg_offset, count_buffer, count_offset,
            counted_args, counted_offset, argument_size, command_index);
      }

      enc.retainAllocation(index_allocation.ptr());
      if (!BeginRenderPass(enc, attachments, argument_buffer_size))
        return;

      auto &set_pso = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      set_pso.type = WMTRenderCommandSetPSO;
      set_pso.pso = metal_pso;
      if (depth_stencil) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
        cmd.type = WMTRenderCommandSetDSSO;
        cmd.dsso = depth_stencil;
        cmd.stencil_ref = static_cast<uint8_t>(stencil_ref);
      }
      auto &rs = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      rs = rasterizer;

      uint64_t argbuf_offset = 0;
      EncodeGraphicsBindings(enc, replay_state, *pipeline, argbuf_offset);
      EncodeDynamicRenderState(enc, viewports, scissors, blend_factor,
                               stencil_ref);

      WMT::Buffer indirect_buffer = counted_args;
      UINT64 indirect_offset = counted_offset;
      if (!has_count) {
        auto [arg_allocation, arg_sub_offset] =
            enc.access<PipelineStage::Vertex>(arg_buffer, arg_offset,
                                              argument_size,
                                              ResourceAccess::Read);
        indirect_buffer = arg_allocation->buffer();
        indirect_offset = arg_sub_offset + arg_offset;
      }

      auto &draw =
          enc.encodeRenderCommand<wmtcmd_render_draw_indexed_indirect>();
      draw.type = WMTRenderCommandDrawIndexedIndirect;
      draw.primitive_type = *primitive;
      draw.index_type = index_type;
      draw.index_buffer = index_allocation->buffer();
      draw.index_buffer_offset = index_offset;
      draw.indirect_args_buffer = indirect_buffer;
      draw.indirect_args_offset = indirect_offset;
      enc.endPass();
    });
  }

  void ReplayDispatchIndirect(
      CommandChunk *chunk, ReplayState &state, Rc<Buffer> arg_buffer,
      UINT64 arg_offset, Rc<Buffer> count_buffer, UINT64 count_offset,
      WMT::Reference<WMT::Buffer> counted_args, UINT64 counted_offset,
      UINT argument_size, UINT command_index) {
    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: indirect dispatch skipped without compute pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalComputeState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: indirect dispatch skipped because Metal compute PSO is unavailable");
      return;
    }

    const auto argument_buffer_size = EstimateComputeArgumentBufferSize(*pipeline);
    chunk->emitcc([this, metal_pso = metal->pso,
                   threadgroup_size = metal->threadgroup_size, pipeline,
                   replay_state = state, argument_buffer_size, arg_buffer,
                   arg_offset, count_buffer, count_offset, counted_args,
                   counted_offset, argument_size,
                   command_index](ArgumentEncodingContext &enc) {
      const bool has_count = count_buffer.ptr();
      if (has_count) {
        PrepareCountedIndirectArguments(
            enc, arg_buffer, arg_offset, count_buffer, count_offset,
            counted_args, counted_offset, argument_size, command_index);
      }

      enc.startComputePass(argument_buffer_size);
      auto &set_pso = enc.encodeComputeCommand<wmtcmd_compute_setpso>();
      set_pso.type = WMTComputeCommandSetPSO;
      set_pso.pso = metal_pso;
      set_pso.threadgroup_size = threadgroup_size;

      uint64_t argbuf_offset = 0;
      EncodeComputeBindings(enc, replay_state, *pipeline, argbuf_offset);
      if (argbuf_offset > argument_buffer_size) {
        WARN("D3D12CommandQueue: compute argument buffer estimate was too small estimated=",
             argument_buffer_size, " actual=", argbuf_offset);
      }

      WMT::Buffer indirect_buffer = counted_args;
      UINT64 indirect_offset = counted_offset;
      if (!has_count) {
        auto [arg_allocation, arg_sub_offset] =
            enc.access<PipelineStage::Compute>(arg_buffer, arg_offset,
                                               argument_size,
                                               ResourceAccess::Read);
        indirect_buffer = arg_allocation->buffer();
        indirect_offset = arg_sub_offset + arg_offset;
      }

      auto &dispatch =
          enc.encodeComputeCommand<wmtcmd_compute_dispatch_indirect>();
      dispatch.type = WMTComputeCommandDispatchIndirect;
      dispatch.indirect_args_buffer = indirect_buffer;
      dispatch.indirect_args_offset = indirect_offset;
      enc.endPass();
    });
  }

  static void ForEachVisibleStage(D3D12_SHADER_VISIBILITY visibility,
                                  bool compute, const auto &fn) {
    if (compute) {
      fn(PipelineStage::Compute);
      return;
    }

    switch (visibility) {
    case D3D12_SHADER_VISIBILITY_VERTEX:
      fn(PipelineStage::Vertex);
      break;
    case D3D12_SHADER_VISIBILITY_PIXEL:
      fn(PipelineStage::Pixel);
      break;
    case D3D12_SHADER_VISIBILITY_GEOMETRY:
      fn(PipelineStage::Geometry);
      break;
    case D3D12_SHADER_VISIBILITY_HULL:
      fn(PipelineStage::Hull);
      break;
    case D3D12_SHADER_VISIBILITY_DOMAIN:
      fn(PipelineStage::Domain);
      break;
    case D3D12_SHADER_VISIBILITY_ALL:
    default:
      fn(PipelineStage::Vertex);
      fn(PipelineStage::Pixel);
      fn(PipelineStage::Geometry);
      fn(PipelineStage::Hull);
      fn(PipelineStage::Domain);
      break;
    }
  }

  static const PipelineDxilShader *
  FindShaderForStage(const PipelineState &pipeline, PipelineStage stage) {
    PipelineShaderStage shader_stage = PipelineShaderStage::Vertex;
    switch (stage) {
    case PipelineStage::Compute:
      shader_stage = PipelineShaderStage::Compute;
      break;
    case PipelineStage::Pixel:
      shader_stage = PipelineShaderStage::Pixel;
      break;
    case PipelineStage::Geometry:
      shader_stage = PipelineShaderStage::Geometry;
      break;
    case PipelineStage::Hull:
      shader_stage = PipelineShaderStage::Hull;
      break;
    case PipelineStage::Domain:
      shader_stage = PipelineShaderStage::Domain;
      break;
    case PipelineStage::Vertex:
    default:
      shader_stage = PipelineShaderStage::Vertex;
      break;
    }

    for (const auto &shader : pipeline.GetDxilShaders()) {
      if (shader.stage == shader_stage)
        return &shader;
    }
    return nullptr;
  }

  static SM50BindingType
  BindingTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
    switch (range_type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
      return SM50BindingType::ConstantBuffer;
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
      return SM50BindingType::Sampler;
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
      return SM50BindingType::UAV;
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
    default:
      return SM50BindingType::SRV;
    }
  }

  static std::optional<UINT>
  ResolveShaderBindingSlot(const PipelineState &pipeline, PipelineStage stage,
                           SM50BindingType binding_type, UINT shader_register,
                           UINT register_space) {
    const auto *shader = FindShaderForStage(pipeline, stage);
    if (!shader)
      return std::nullopt;

    const auto *arguments =
        binding_type == SM50BindingType::ConstantBuffer
            ? shader->constantBufferInfo()
            : shader->resourceArgumentInfo();
    const auto argument_count =
        binding_type == SM50BindingType::ConstantBuffer
            ? shader->reflection.NumConstantBuffers
            : shader->reflection.NumArguments;
    if (!arguments)
      return std::nullopt;

    for (UINT i = 0; i < argument_count; i++) {
      const auto &argument = arguments[i];
      if (argument.Type != binding_type)
        continue;
      const auto lower = argument.RegisterCount ? argument.RegisterLowerBound
                                                : argument.SM50BindingSlot;
      const auto space = argument.RegisterCount ? argument.RegisterSpace : 0;
      const auto count = argument.RegisterCount ? argument.RegisterCount : 1;
      if (space != register_space || shader_register < lower)
        continue;
      const auto index = shader_register - lower;
      if (count != UINT_MAX && index >= count)
        continue;
      return argument.SM50BindingSlot + index;
    }
    return std::nullopt;
  }

  static const MTL_SM50_SHADER_ARGUMENT *
  ResolveShaderBindingArgument(const PipelineState &pipeline,
                               PipelineStage stage,
                               SM50BindingType binding_type,
                               UINT shader_register,
                               UINT register_space) {
    const auto *shader = FindShaderForStage(pipeline, stage);
    if (!shader)
      return nullptr;

    const auto *arguments =
        binding_type == SM50BindingType::ConstantBuffer
            ? shader->constantBufferInfo()
            : shader->resourceArgumentInfo();
    const auto argument_count =
        binding_type == SM50BindingType::ConstantBuffer
            ? shader->reflection.NumConstantBuffers
            : shader->reflection.NumArguments;
    if (!arguments)
      return nullptr;

    for (UINT i = 0; i < argument_count; i++) {
      const auto &argument = arguments[i];
      if (argument.Type != binding_type)
        continue;
      const auto lower = argument.RegisterCount ? argument.RegisterLowerBound
                                                : argument.SM50BindingSlot;
      const auto space = argument.RegisterCount ? argument.RegisterSpace : 0;
      const auto count = argument.RegisterCount ? argument.RegisterCount : 1;
      if (space != register_space || shader_register < lower)
        continue;
      const auto index = shader_register - lower;
      if (count != UINT_MAX && index >= count)
        continue;
      return &argument;
    }
    return nullptr;
  }

  static const MTL_SM50_SHADER_ARGUMENT *
  ResolveShaderBindingArgumentBySlot(const PipelineState &pipeline,
                                     PipelineStage stage,
                                     SM50BindingType binding_type,
                                     UINT binding_slot) {
    const auto *shader = FindShaderForStage(pipeline, stage);
    if (!shader)
      return nullptr;

    const auto *arguments =
        binding_type == SM50BindingType::ConstantBuffer
            ? shader->constantBufferInfo()
            : shader->resourceArgumentInfo();
    const auto argument_count =
        binding_type == SM50BindingType::ConstantBuffer
            ? shader->reflection.NumConstantBuffers
            : shader->reflection.NumArguments;
    if (!arguments)
      return nullptr;

    for (UINT i = 0; i < argument_count; i++) {
      const auto &argument = arguments[i];
      if (argument.Type == binding_type &&
          argument.SM50BindingSlot == binding_slot)
        return &argument;
    }
    return nullptr;
  }

  void BindRootConstants(ArgumentEncodingContext &enc, const ReplayState &state,
                         const PipelineState &pipeline, bool compute,
                         UINT root_index,
                         const RootSignatureParameter &parameter) {
    const auto &map = compute ? state.compute_root_constants
                              : state.graphics_root_constants;
    auto it = map.find(root_index);
    if (it == map.end() || it->second.empty())
      return;

    const auto declared_count = parameter.constants.Num32BitValues;
    const auto actual_count =
        std::max<uint32_t>(declared_count, uint32_t(it->second.size()));
    if (!actual_count)
      return;

    auto buffer = Rc<Buffer>(new Buffer(uint64_t(actual_count) * sizeof(UINT),
                                        device_->GetMTLDevice()));
    Flags<BufferAllocationFlag> flags;
    flags.set(BufferAllocationFlag::GpuReadonly);
    auto allocation = buffer->allocate(flags);
    std::vector<UINT> packed(actual_count, 0);
    std::copy(it->second.begin(),
              it->second.begin() +
                  std::min<size_t>(it->second.size(), packed.size()),
              packed.begin());
    allocation->updateContents(0, packed.data(),
                               uint64_t(packed.size()) * sizeof(UINT));
    buffer->rename(std::move(allocation));

    ForEachVisibleStage(parameter.visibility, compute, [&](PipelineStage stage) {
      auto slot = ResolveShaderBindingSlot(
          pipeline, stage, SM50BindingType::ConstantBuffer,
          parameter.constants.ShaderRegister,
          parameter.constants.RegisterSpace);
      if (!slot)
        return;
      if (*slot >= 14) {
        WARN("D3D12CommandQueue: root constants target unsupported CBV slot b",
             *slot);
        return;
      }
      switch (stage) {
      case PipelineStage::Compute:
        enc.bindConstantBuffer<PipelineStage::Compute>(*slot, 0,
                                                       Rc<Buffer>(buffer));
        break;
      case PipelineStage::Pixel:
        enc.bindConstantBuffer<PipelineStage::Pixel>(*slot, 0,
                                                     Rc<Buffer>(buffer));
        break;
      case PipelineStage::Geometry:
        enc.bindConstantBuffer<PipelineStage::Geometry>(*slot, 0,
                                                        Rc<Buffer>(buffer));
        break;
      case PipelineStage::Hull:
        enc.bindConstantBuffer<PipelineStage::Hull>(*slot, 0,
                                                    Rc<Buffer>(buffer));
        break;
      case PipelineStage::Domain:
        enc.bindConstantBuffer<PipelineStage::Domain>(*slot, 0,
                                                      Rc<Buffer>(buffer));
        break;
      case PipelineStage::Vertex:
      default:
        enc.bindConstantBuffer<PipelineStage::Vertex>(*slot, 0,
                                                      Rc<Buffer>(buffer));
        break;
      }
    });
  }

  static D3D12_GPU_DESCRIPTOR_HANDLE
  GetTableHandle(const ReplayState &state, bool compute,
                 UINT root_parameter_index) {
    const auto &tables = compute ? state.compute_tables : state.graphics_tables;
    auto it = tables.find(root_parameter_index);
    return it == tables.end() ? D3D12_GPU_DESCRIPTOR_HANDLE{} : it->second;
  }

  static UINT
  DescriptorRangeOffset(const RootSignatureRange &range,
                        UINT running_offset) {
    return range.offset_in_descriptors_from_table_start == UINT_MAX
               ? running_offset
               : range.offset_in_descriptors_from_table_start;
  }

  const DescriptorRecord *
  GetBoundDescriptorRecord(const ReplayState &state,
                           D3D12_GPU_DESCRIPTOR_HANDLE handle,
                           D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
    const auto &heap = heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                           ? state.sampler_heap
                           : state.cbv_srv_uav_heap;
    auto *descriptor_heap = dynamic_cast<DescriptorHeap *>(heap.ptr());
    if (!descriptor_heap) {
      WARN("D3D12CommandQueue: GPU descriptor handle used without bound heap type=",
           uint32_t(heap_type));
      return nullptr;
    }

    const auto *descriptor = descriptor_heap->GetDescriptorRecord(handle);
    if (!descriptor) {
      WARN("D3D12CommandQueue: GPU descriptor handle does not belong to the currently bound heap type=",
           uint32_t(heap_type));
      return nullptr;
    }
    if (!descriptor->shader_visible || descriptor->heap_type != heap_type) {
      WARN("D3D12CommandQueue: invalid GPU descriptor heap visibility/type");
      return nullptr;
    }
    return descriptor;
  }

  const DescriptorRecord *
  GetBoundDescriptorRecordInRange(const ReplayState &state,
                                  D3D12_GPU_DESCRIPTOR_HANDLE base,
                                  UINT range_offset, UINT descriptor_index,
                                  UINT descriptor_count,
                                  D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
    const auto total_offset = range_offset + descriptor_index;
    if (total_offset < range_offset) {
      WARN("D3D12CommandQueue: descriptor table offset overflow");
      return nullptr;
    }

    const auto handle =
        D3D12_GPU_DESCRIPTOR_HANDLE{base.ptr +
                                    sizeof(DescriptorRecord) * total_offset};
    const auto *descriptor = GetBoundDescriptorRecord(state, handle, heap_type);
    if (!descriptor)
      return nullptr;
    if (descriptor_count &&
        (descriptor->heap_index >= descriptor->heap_count ||
         descriptor_count - descriptor_index >
             descriptor->heap_count - descriptor->heap_index)) {
      WARN("D3D12CommandQueue: descriptor table range exceeds heap start=",
           descriptor->heap_index, " index=", descriptor_index,
           " count=", descriptor_count, " heap_count=", descriptor->heap_count);
      return nullptr;
    }
    return descriptor;
  }

  void BindDescriptor(ArgumentEncodingContext &enc, PipelineStage stage,
                      D3D12_DESCRIPTOR_RANGE_TYPE range_type, UINT slot,
                      const DescriptorRecord &descriptor,
                      const MTL_SM50_SHADER_ARGUMENT *argument) {
    switch (range_type) {
    case D3D12_DESCRIPTOR_RANGE_TYPE_CBV:
      BindConstantBufferDescriptor(enc, stage, slot, descriptor);
      break;
    case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:
      BindShaderResourceDescriptor(enc, stage, slot, descriptor, argument);
      break;
    case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:
      BindUnorderedAccessDescriptor(enc, stage, slot, descriptor, argument);
      break;
    case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
      BindSamplerDescriptor(enc, stage, slot, descriptor);
      break;
    }
  }

  void BindConstantBufferDescriptor(ArgumentEncodingContext &enc,
                                    PipelineStage stage, UINT slot,
                                    const DescriptorRecord &descriptor) {
    if (descriptor.type != DescriptorRecordType::ConstantBufferView ||
        !descriptor.has_desc)
      return;
    if (slot >= 14) {
      WARN("D3D12CommandQueue: CBV slot b", slot, " is unsupported");
      return;
    }
    if (descriptor.desc.cbv.BufferLocation &
        (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)) {
      WARN("D3D12CommandQueue: root/table CBV BufferLocation is not 256-byte aligned");
      return;
    }
    if (descriptor.desc.cbv.SizeInBytes &
        (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)) {
      WARN("D3D12CommandQueue: root/table CBV SizeInBytes is not 256-byte aligned");
      return;
    }

    Resource *resource = nullptr;
    const auto offset =
        ResolveBufferGpuAddress(descriptor.desc.cbv.BufferLocation, resource);
    if (!resource || !resource->GetBuffer())
      return;

    auto buffer = Rc<Buffer>(resource->GetBuffer());
    switch (stage) {
    case PipelineStage::Compute:
      enc.bindConstantBuffer<PipelineStage::Compute>(slot, offset,
                                                     std::move(buffer));
      break;
    case PipelineStage::Pixel:
      enc.bindConstantBuffer<PipelineStage::Pixel>(slot, offset,
                                                   std::move(buffer));
      break;
    default:
      enc.bindConstantBuffer<PipelineStage::Vertex>(slot, offset,
                                                    std::move(buffer));
      break;
    }
  }

  void BindShaderResourceDescriptor(ArgumentEncodingContext &enc,
                                    PipelineStage stage, UINT slot,
                                    const DescriptorRecord &descriptor,
                                    const MTL_SM50_SHADER_ARGUMENT *argument) {
    if (descriptor.type != DescriptorRecordType::ShaderResourceView)
      return;
    if (slot >= kSRVBindings) {
      WARN("D3D12CommandQueue: SRV slot t", slot, " is unsupported for ",
           PipelineStageName(stage), " stage");
      return;
    }

    auto *resource = GetResource(descriptor.resource.ptr());
    if (!resource)
      return;

    if (resource->GetBuffer()) {
      UINT64 offset = 0;
      if (!ResolveDescriptorBufferOffset(descriptor.resource.ptr(), offset))
        offset = 0;
      UINT64 byte_size = resource->GetResourceDesc().Width;
      uint64_t view_id = 0;
      BufferSlice slice = DefaultBufferSlice(*resource);
      if (descriptor.has_desc) {
        const auto &srv = descriptor.desc.srv;
        if (srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
          const bool needs_texture_buffer_view =
              argument && (argument->Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE);
          const UINT64 first_element = srv.Buffer.FirstElement;
          if (srv.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW) {
            offset += first_element * sizeof(uint32_t);
            byte_size = UINT64(srv.Buffer.NumElements) * sizeof(uint32_t);
            if (needs_texture_buffer_view) {
              view_id = CreateBufferView(device_->GetMTLDevice(), *resource,
                                         DXGI_FORMAT_R32_UINT, offset,
                                         byte_size, WMTTextureUsageShaderRead);
            }
            slice = StructuredBufferSlice(*resource, offset, byte_size,
                                          sizeof(uint32_t));
          } else if (srv.Format != DXGI_FORMAT_UNKNOWN) {
            MTL_DXGI_FORMAT_DESC format = {};
            if (SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(),
                                             srv.Format, format))) {
              offset += first_element * format.BytesPerTexel;
              byte_size = UINT64(srv.Buffer.NumElements) *
                          format.BytesPerTexel;
              if (needs_texture_buffer_view) {
                view_id = CreateBufferView(device_->GetMTLDevice(), *resource,
                                           srv.Format, offset, byte_size,
                                           WMTTextureUsageShaderRead);
                if (!view_id) {
                  WARN("D3D12CommandQueue: typed buffer SRV failed to create Metal texture buffer view format=",
                       uint32_t(srv.Format),
                       "; falling back to raw buffer binding");
                }
              }
              slice = StructuredBufferSlice(*resource, offset, byte_size,
                                            format.BytesPerTexel);
            } else {
              WARN("D3D12CommandQueue: typed buffer SRV uses unsupported format ",
                   uint32_t(srv.Format));
              return;
            }
          } else if (srv.Buffer.StructureByteStride) {
            offset += first_element * srv.Buffer.StructureByteStride;
            byte_size = UINT64(srv.Buffer.NumElements) *
                        srv.Buffer.StructureByteStride;
            if (needs_texture_buffer_view) {
              if (const auto view_format = UintBufferViewFormatForStride(
                      srv.Buffer.StructureByteStride);
                  view_format != DXGI_FORMAT_UNKNOWN) {
                view_id = CreateBufferView(device_->GetMTLDevice(), *resource,
                                           view_format, offset, byte_size,
                                           WMTTextureUsageShaderRead);
              }
            }
            slice = StructuredBufferSlice(*resource, offset, byte_size,
                                          srv.Buffer.StructureByteStride);
          } else {
            offset += first_element;
            byte_size = srv.Buffer.NumElements;
            slice = DefaultBufferSlice(*resource, offset, byte_size);
          }
        } else {
          WARN("D3D12CommandQueue: buffer SRV has unsupported view dimension ",
               uint32_t(srv.ViewDimension));
          return;
        }
      }
      auto buffer = Rc<Buffer>(resource->GetBuffer());
      if (stage == PipelineStage::Compute)
        enc.bindBuffer<PipelineStage::Compute>(slot, std::move(buffer),
                                               view_id, slice);
      else if (stage == PipelineStage::Pixel)
        enc.bindBuffer<PipelineStage::Pixel>(slot, std::move(buffer),
                                             view_id, slice);
      else
        enc.bindBuffer<PipelineStage::Vertex>(slot, std::move(buffer),
                                              view_id, slice);
      return;
    }

    if (resource->GetTexture()) {
      if (descriptor.has_desc &&
          descriptor.desc.srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
        WARN("D3D12CommandQueue: texture SRV cannot use BUFFER view dimension");
        return;
      }
      const auto view =
          CreateShaderResourceTextureView(device_->GetMTLDevice(), *resource,
                                          descriptor);
      if (!uint64_t(view))
        return;
      auto texture = Rc<Texture>(resource->GetTexture());
      if (stage == PipelineStage::Compute)
        enc.bindTexture<PipelineStage::Compute>(slot, std::move(texture), view);
      else if (stage == PipelineStage::Pixel)
        enc.bindTexture<PipelineStage::Pixel>(slot, std::move(texture), view);
      else
        enc.bindTexture<PipelineStage::Vertex>(slot, std::move(texture), view);
    }
  }

  void BindUnorderedAccessDescriptor(ArgumentEncodingContext &enc,
                                     PipelineStage stage, UINT slot,
                                     const DescriptorRecord &descriptor,
                                     const MTL_SM50_SHADER_ARGUMENT *argument) {
    if (descriptor.type != DescriptorRecordType::UnorderedAccessView)
      return;
    if (slot >= kUAVBindings) {
      WARN("D3D12CommandQueue: UAV slot u", slot, " is unsupported for ",
           PipelineStageName(stage), " stage");
      return;
    }

    auto *resource = GetResource(descriptor.resource.ptr());
    if (!resource)
      return;

    Rc<Buffer> counter;
    if (auto *counter_resource = GetResource(descriptor.counter_resource.ptr())) {
      if (counter_resource->GetBuffer())
        counter = Rc<Buffer>(counter_resource->GetBuffer());
      else
        WARN("D3D12CommandQueue: UAV counter resource must be a buffer");
    }

    if (resource->GetBuffer()) {
      UINT64 offset = 0;
      if (!ResolveDescriptorBufferOffset(descriptor.resource.ptr(), offset))
        offset = 0;
      UINT64 byte_size = resource->GetResourceDesc().Width;
      uint64_t view_id = 0;
      BufferSlice slice = DefaultBufferSlice(*resource);
      if (descriptor.has_desc) {
        const auto &uav = descriptor.desc.uav;
        if (uav.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
          const bool needs_texture_buffer_view =
              argument && (argument->Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE);
          const UINT64 first_element = uav.Buffer.FirstElement;
          if (uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
            offset += first_element * sizeof(uint32_t);
            byte_size = UINT64(uav.Buffer.NumElements) * sizeof(uint32_t);
            if (needs_texture_buffer_view) {
              view_id = CreateBufferView(device_->GetMTLDevice(), *resource,
                                         DXGI_FORMAT_R32_UINT, offset,
                                         byte_size,
                                         WMTTextureUsageShaderRead |
                                             WMTTextureUsageShaderWrite);
            }
            slice = StructuredBufferSlice(*resource, offset, byte_size,
                                          sizeof(uint32_t));
          } else if (uav.Format != DXGI_FORMAT_UNKNOWN) {
            MTL_DXGI_FORMAT_DESC format = {};
            if (SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(),
                                             uav.Format, format))) {
              offset += first_element * format.BytesPerTexel;
              byte_size = UINT64(uav.Buffer.NumElements) *
                          format.BytesPerTexel;
              if (needs_texture_buffer_view) {
                view_id = CreateBufferView(device_->GetMTLDevice(), *resource,
                                           uav.Format, offset, byte_size,
                                           WMTTextureUsageShaderRead |
                                               WMTTextureUsageShaderWrite);
                if (!view_id) {
                  WARN("D3D12CommandQueue: typed buffer UAV failed to create Metal texture buffer view format=",
                       uint32_t(uav.Format),
                       "; falling back to raw buffer binding");
                }
              }
              slice = StructuredBufferSlice(*resource, offset, byte_size,
                                            format.BytesPerTexel);
            } else {
              WARN("D3D12CommandQueue: typed buffer UAV uses unsupported format ",
                   uint32_t(uav.Format));
              return;
            }
          } else if (uav.Buffer.StructureByteStride) {
            offset += first_element * uav.Buffer.StructureByteStride;
            byte_size = UINT64(uav.Buffer.NumElements) *
                        uav.Buffer.StructureByteStride;
            if (needs_texture_buffer_view) {
              if (const auto view_format = UintBufferViewFormatForStride(
                      uav.Buffer.StructureByteStride);
                  view_format != DXGI_FORMAT_UNKNOWN) {
                view_id = CreateBufferView(device_->GetMTLDevice(), *resource,
                                           view_format, offset, byte_size,
                                           WMTTextureUsageShaderRead |
                                               WMTTextureUsageShaderWrite);
              }
            }
            slice = StructuredBufferSlice(*resource, offset, byte_size,
                                          uav.Buffer.StructureByteStride);
          } else {
            offset += first_element;
            byte_size = uav.Buffer.NumElements;
            slice = DefaultBufferSlice(*resource, offset, byte_size);
          }
        } else {
          WARN("D3D12CommandQueue: buffer UAV has unsupported view dimension ",
               uint32_t(uav.ViewDimension));
          return;
        }
      }
      auto buffer = Rc<Buffer>(resource->GetBuffer());
      if (stage == PipelineStage::Compute)
        enc.bindOutputBuffer<PipelineStage::Compute>(
            slot, std::move(buffer), view_id, std::move(counter), slice);
      else
        enc.bindOutputBuffer<PipelineStage::Pixel>(
            slot, std::move(buffer), view_id, std::move(counter), slice);
      return;
    }

    if (resource->GetTexture()) {
      if (descriptor.has_desc &&
          descriptor.desc.uav.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
        WARN("D3D12CommandQueue: texture UAV cannot use BUFFER view dimension");
        return;
      }
      const auto view =
          CreateUnorderedAccessTextureView(device_->GetMTLDevice(), *resource,
                                           descriptor);
      if (!uint64_t(view))
        return;
      auto texture = Rc<Texture>(resource->GetTexture());
      if (stage == PipelineStage::Compute)
        enc.bindOutputTexture<PipelineStage::Compute>(slot,
                                                      std::move(texture), view);
      else
        enc.bindOutputTexture<PipelineStage::Pixel>(slot, std::move(texture),
                                                   view);
    }
  }

  static bool ResolveDescriptorBufferOffset(ID3D12Resource *d3d_resource,
                                            UINT64 &offset) {
    auto *resource = GetResource(d3d_resource);
    if (!resource || !resource->GetBuffer())
      return false;
    const auto address = resource->GetGpuVirtualAddress();
    if (!address)
      return false;
    Resource *resolved = nullptr;
    offset = ResolveBufferGpuAddress(address, resolved);
    return resolved == resource;
  }

  void BindSamplerDescriptor(ArgumentEncodingContext &enc, PipelineStage stage,
                             UINT slot, const DescriptorRecord &descriptor) {
    if (descriptor.type != DescriptorRecordType::Sampler ||
        !descriptor.has_desc)
      return;
    if (slot >= 16) {
      WARN("D3D12CommandQueue: sampler slot s", slot, " is unsupported for ",
           PipelineStageName(stage), " stage");
      return;
    }

    auto sampler = CreateSampler(descriptor.desc.sampler);
    if (!sampler)
      return;

    if (stage == PipelineStage::Compute)
      enc.bindSampler<PipelineStage::Compute>(slot, std::move(sampler));
    else if (stage == PipelineStage::Pixel)
      enc.bindSampler<PipelineStage::Pixel>(slot, std::move(sampler));
    else
      enc.bindSampler<PipelineStage::Vertex>(slot, std::move(sampler));
  }

  Rc<Sampler> CreateSampler(const D3D12_SAMPLER_DESC &desc) {
    WMTSamplerInfo info = {};
    info.lod_average = false;
    info.min_filter = D3D12_DECODE_MIN_FILTER(desc.Filter)
                          ? WMTSamplerMinMagFilterLinear
                          : WMTSamplerMinMagFilterNearest;
    info.mag_filter = D3D12_DECODE_MAG_FILTER(desc.Filter)
                          ? WMTSamplerMinMagFilterLinear
                          : WMTSamplerMinMagFilterNearest;
    info.mip_filter = D3D12_DECODE_MIP_FILTER(desc.Filter)
                          ? WMTSamplerMipFilterLinear
                          : WMTSamplerMipFilterNearest;
    info.lod_min_clamp = desc.MinLOD;
    info.lod_max_clamp = std::max(desc.MinLOD, desc.MaxLOD);
    info.max_anisotroy =
        D3D12_DECODE_IS_ANISOTROPIC_FILTER(desc.Filter)
            ? std::clamp<UINT>(desc.MaxAnisotropy, 1, 16)
            : 1;
    info.s_address_mode = AddressMode(desc.AddressU);
    info.t_address_mode = AddressMode(desc.AddressV);
    info.r_address_mode = AddressMode(desc.AddressW);
    info.compare_function = WMTCompareFunctionNever;
    if (D3D12_DECODE_IS_COMPARISON_FILTER(desc.Filter))
      info.compare_function = CompareFunction(desc.ComparisonFunc);
    info.border_color = BorderColor(desc.BorderColor);
    info.support_argument_buffers = true;
    info.normalized_coords = true;
    return Sampler::createSampler(device_->GetMTLDevice(), info,
                                  desc.MipLODBias);
  }

  Rc<Sampler> CreateStaticSampler(const D3D12_STATIC_SAMPLER_DESC &desc) {
    D3D12_SAMPLER_DESC sampler = {};
    sampler.Filter = desc.Filter;
    sampler.AddressU = desc.AddressU;
    sampler.AddressV = desc.AddressV;
    sampler.AddressW = desc.AddressW;
    sampler.MipLODBias = desc.MipLODBias;
    sampler.MaxAnisotropy = desc.MaxAnisotropy;
    sampler.ComparisonFunc = desc.ComparisonFunc;
    sampler.MinLOD = desc.MinLOD;
    sampler.MaxLOD = desc.MaxLOD;
    switch (desc.BorderColor) {
    case D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK:
      sampler.BorderColor[3] = 1.0f;
      break;
    case D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE:
      sampler.BorderColor[0] = 1.0f;
      sampler.BorderColor[1] = 1.0f;
      sampler.BorderColor[2] = 1.0f;
      sampler.BorderColor[3] = 1.0f;
      break;
    case D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK:
    default:
      break;
    }
    return CreateSampler(sampler);
  }

  static WMTSamplerAddressMode AddressMode(D3D12_TEXTURE_ADDRESS_MODE mode) {
    switch (mode) {
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR:
      return WMTSamplerAddressModeMirrorRepeat;
    case D3D12_TEXTURE_ADDRESS_MODE_CLAMP:
      return WMTSamplerAddressModeClampToEdge;
    case D3D12_TEXTURE_ADDRESS_MODE_BORDER:
      return WMTSamplerAddressModeClampToBorderColor;
    case D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE:
      return WMTSamplerAddressModeMirrorClampToEdge;
    default:
      return WMTSamplerAddressModeRepeat;
    }
  }

  static WMTCompareFunction CompareFunction(D3D12_COMPARISON_FUNC func) {
    switch (func) {
    case D3D12_COMPARISON_FUNC_LESS:
      return WMTCompareFunctionLess;
    case D3D12_COMPARISON_FUNC_EQUAL:
      return WMTCompareFunctionEqual;
    case D3D12_COMPARISON_FUNC_LESS_EQUAL:
      return WMTCompareFunctionLessEqual;
    case D3D12_COMPARISON_FUNC_GREATER:
      return WMTCompareFunctionGreater;
    case D3D12_COMPARISON_FUNC_NOT_EQUAL:
      return WMTCompareFunctionNotEqual;
    case D3D12_COMPARISON_FUNC_GREATER_EQUAL:
      return WMTCompareFunctionGreaterEqual;
    case D3D12_COMPARISON_FUNC_ALWAYS:
      return WMTCompareFunctionAlways;
    default:
      return WMTCompareFunctionNever;
    }
  }

  static WMTSamplerBorderColor BorderColor(const FLOAT color[4]) {
    if (color[0] == 0.0f && color[1] == 0.0f && color[2] == 0.0f &&
        color[3] == 0.0f)
      return WMTSamplerBorderColorTransparentBlack;
    if (color[0] == 0.0f && color[1] == 0.0f && color[2] == 0.0f &&
        color[3] == 1.0f)
      return WMTSamplerBorderColorOpaqueBlack;
    return WMTSamplerBorderColorOpaqueWhite;
  }

  UINT ReflectedDescriptorRangeCount(const PipelineState &pipeline,
                                     const RootSignatureRange &range,
                                     D3D12_SHADER_VISIBILITY visibility,
                                     bool compute) {
    UINT count = 0;
    const auto binding_type = BindingTypeForRange(range.range_type);
    ForEachVisibleStage(
        visibility, compute, [&](PipelineStage stage) {
          const auto *shader = FindShaderForStage(pipeline, stage);
          if (!shader)
            return;
          const auto *arguments =
              binding_type == SM50BindingType::ConstantBuffer
                  ? shader->constantBufferInfo()
                  : shader->resourceArgumentInfo();
          const auto argument_count =
              binding_type == SM50BindingType::ConstantBuffer
                  ? shader->reflection.NumConstantBuffers
                  : shader->reflection.NumArguments;
          if (!arguments)
            return;
          for (UINT i = 0; i < argument_count; i++) {
            const auto &argument = arguments[i];
            if (argument.Type != binding_type)
              continue;
            const auto space =
                argument.RegisterCount ? argument.RegisterSpace : 0;
            const auto lower = argument.RegisterCount
                                   ? argument.RegisterLowerBound
                                   : argument.SM50BindingSlot;
            const auto arg_count =
                argument.RegisterCount ? argument.RegisterCount : 1;
            if (space != range.register_space ||
                lower < range.base_shader_register)
              continue;
            const auto first = lower - range.base_shader_register;
            const auto size =
                arg_count == UINT_MAX ? 1u : std::max<UINT>(arg_count, 1u);
            count = std::max(count, first + size);
          }
        });
    return count ? std::min<UINT>(count, 4096u) : 1u;
  }

  void ApplyStaticSamplers(ArgumentEncodingContext &enc,
                           const PipelineState &pipeline,
                           const RootSignature &root, bool compute) {
    for (const auto &sampler_desc : root.GetStaticSamplers()) {
      ForEachVisibleStage(
          sampler_desc.ShaderVisibility, compute, [&](PipelineStage stage) {
            auto slot = ResolveShaderBindingSlot(
                pipeline, stage, SM50BindingType::Sampler,
                sampler_desc.ShaderRegister, sampler_desc.RegisterSpace);
            if (!slot)
              return;

            auto sampler = CreateStaticSampler(sampler_desc);
            if (!sampler)
              return;
            if (stage == PipelineStage::Compute)
              enc.bindSampler<PipelineStage::Compute>(*slot,
                                                       std::move(sampler));
            else if (stage == PipelineStage::Pixel)
              enc.bindSampler<PipelineStage::Pixel>(*slot,
                                                     std::move(sampler));
            else
              enc.bindSampler<PipelineStage::Vertex>(*slot,
                                                      std::move(sampler));
          });
    }
  }

  void ApplyRootDescriptorTables(ArgumentEncodingContext &enc,
                                 const ReplayState &state,
                                 const PipelineState &pipeline, bool compute) {
    auto *root = GetRootSignature(compute ? state.compute_root_signature.ptr()
                                          : state.graphics_root_signature.ptr());
    if (!root)
      return;

    ApplyStaticSamplers(enc, pipeline, *root, compute);

    const auto parameters = root->GetParameters();
    for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
      const auto &parameter = parameters[root_index];
      if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        const auto base = GetTableHandle(state, compute, root_index);
        if (!base.ptr)
          continue;
        UINT running_offset = 0;
        for (const auto &range : parameter.ranges) {
          const auto range_offset = DescriptorRangeOffset(range, running_offset);
          const auto count =
              range.descriptor_count == UINT_MAX
                  ? ReflectedDescriptorRangeCount(
                        pipeline, range, parameter.visibility, compute)
                  : range.descriptor_count;
          for (UINT i = 0; i < count; i++) {
            auto *descriptor = GetBoundDescriptorRecordInRange(
                state, base, range_offset, i, count,
                DescriptorHeapTypeForRange(range.range_type));
            if (!descriptor)
              continue;
            ForEachVisibleStage(
                parameter.visibility, compute, [&](PipelineStage stage) {
                  auto slot = ResolveShaderBindingSlot(
                      pipeline, stage, BindingTypeForRange(range.range_type),
                      range.base_shader_register + i, range.register_space);
                  if (!slot)
                    return;
                  const auto *argument = ResolveShaderBindingArgumentBySlot(
                      pipeline, stage, BindingTypeForRange(range.range_type),
                      *slot);
                  BindDescriptor(enc, stage, range.range_type, *slot,
                                 *descriptor, argument);
                });
          }
          if (range.descriptor_count != UINT_MAX)
            running_offset = range_offset + range.descriptor_count;
        }
      } else if (parameter.parameter_type ==
                 D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS) {
        BindRootConstants(enc, state, pipeline, compute, root_index, parameter);
      } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
        ApplyRootBufferDescriptor(enc, state, pipeline, compute, root_index,
                                  parameter,
                                  DescriptorRecordType::ConstantBufferView);
      } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_SRV) {
        ApplyRootBufferDescriptor(enc, state, pipeline, compute, root_index,
                                  parameter,
                                  DescriptorRecordType::ShaderResourceView);
      } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_UAV) {
        ApplyRootBufferDescriptor(enc, state, pipeline, compute, root_index,
                                  parameter,
                                  DescriptorRecordType::UnorderedAccessView);
      }
    }
  }

  void ApplyRootBufferDescriptor(ArgumentEncodingContext &enc,
                                 const ReplayState &state,
                                 const PipelineState &pipeline, bool compute,
                                 UINT root_index,
                                 const RootSignatureParameter &parameter,
                                 DescriptorRecordType type) {
    const auto &map =
        type == DescriptorRecordType::ConstantBufferView
            ? (compute ? state.compute_cbv_roots : state.graphics_cbv_roots)
            : type == DescriptorRecordType::ShaderResourceView
                  ? (compute ? state.compute_srv_roots
                             : state.graphics_srv_roots)
                  : (compute ? state.compute_uav_roots
                             : state.graphics_uav_roots);
    auto it = map.find(root_index);
    if (it == map.end())
      return;

    Resource *resource = nullptr;
    const auto offset = ResolveBufferGpuAddress(it->second, resource);
    if (!resource || !resource->GetBuffer())
      return;
    if (type == DescriptorRecordType::ConstantBufferView &&
        (it->second & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1))) {
      WARN("D3D12CommandQueue: root CBV address is not 256-byte aligned");
      return;
    }

    DescriptorRecord descriptor = {};
    descriptor.type = type;
    descriptor.resource = resource->GetD3D12Resource();
    descriptor.has_desc = true;
    if (type == DescriptorRecordType::ConstantBufferView) {
      const auto remaining = resource->GetResourceDesc().Width - offset;
      const auto size = std::min<UINT64>(remaining, UINT_MAX);
      if (size & (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)) {
        WARN("D3D12CommandQueue: root CBV resolved size is not 256-byte aligned");
        return;
      }
      descriptor.desc.cbv.BufferLocation = it->second;
      descriptor.desc.cbv.SizeInBytes = UINT(size);
    } else if (type == DescriptorRecordType::ShaderResourceView) {
      descriptor.desc.srv.Format = DXGI_FORMAT_UNKNOWN;
      descriptor.desc.srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      descriptor.desc.srv.Shader4ComponentMapping =
          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      descriptor.desc.srv.Buffer.FirstElement = offset;
      descriptor.desc.srv.Buffer.NumElements =
          UINT(std::min<UINT64>(resource->GetResourceDesc().Width - offset,
                                UINT_MAX));
      descriptor.desc.srv.Buffer.StructureByteStride = 1;
    } else {
      descriptor.desc.uav.Format = DXGI_FORMAT_UNKNOWN;
      descriptor.desc.uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
      descriptor.desc.uav.Buffer.FirstElement = offset;
      descriptor.desc.uav.Buffer.NumElements =
          UINT(std::min<UINT64>(resource->GetResourceDesc().Width - offset,
                                UINT_MAX));
      descriptor.desc.uav.Buffer.StructureByteStride = 1;
    }

    const auto range_type =
        type == DescriptorRecordType::ConstantBufferView
            ? D3D12_DESCRIPTOR_RANGE_TYPE_CBV
            : type == DescriptorRecordType::ShaderResourceView
                  ? D3D12_DESCRIPTOR_RANGE_TYPE_SRV
                  : D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ForEachVisibleStage(parameter.visibility, compute, [&](PipelineStage stage) {
      const auto *argument = ResolveShaderBindingArgument(
          pipeline, stage, BindingTypeForRange(range_type),
          parameter.descriptor.ShaderRegister,
          parameter.descriptor.RegisterSpace);
      if (!argument)
        return;
      BindDescriptor(enc, stage, range_type, argument->SM50BindingSlot,
                     descriptor, argument);
    });
  }

  template <PipelineStage Stage>
  void EncodeShaderBindingsForStage(ArgumentEncodingContext &enc,
                                    const PipelineDxilShader &shader,
                                    const std::string &shader_key,
                                    uint64_t &argbuf_offset) {
    const auto &reflection = shader.reflection;
    if (reflection.NumConstantBuffers && shader.constantBufferInfo()) {
      const auto offset =
          AllocateArgumentBuffer(argbuf_offset,
                                 reflection.NumConstantBuffers << 3);
      enc.encodeConstantBuffers<Stage, PipelineKind::Ordinary>(
          &reflection, shader.constantBufferInfo(), offset);
    }
    if (reflection.NumArguments && shader.resourceArgumentInfo()) {
      const auto offset =
          AllocateArgumentBuffer(argbuf_offset,
                                 reflection.ArgumentTableQwords << 3);
      enc.encodeShaderResources<Stage, PipelineKind::Ordinary>(
          &reflection, shader.resourceArgumentInfo(), offset, shader_key,
          nullptr);
    }
  }

  static uint64_t AllocateArgumentBuffer(uint64_t &cursor, uint64_t size) {
    const auto alignment = 32ull;
    const auto aligned = (cursor + alignment - 1) & ~(alignment - 1);
    cursor = aligned + std::max<uint64_t>(size, 8);
    return aligned;
  }

  void EncodeVertexBuffers(ArgumentEncodingContext &enc,
                           const ReplayState &state,
                           const PipelineGraphicsState *graphics_state,
                           uint64_t &argbuf_offset) {
    if (!graphics_state)
      return;

    uint32_t slot_mask = 0;
    for (const auto &element : graphics_state->input_elements) {
      if (element.InputSlot < 32)
        slot_mask |= 1u << element.InputSlot;
    }
    if (!slot_mask)
      return;

    const auto max_slot = 32u - __builtin_clz(slot_mask);
    for (UINT slot = 0; slot < max_slot; slot++) {
      if (!(slot_mask & (1u << slot)) || !state.vertex_buffers[slot])
        continue;
      const auto &view = *state.vertex_buffers[slot];
      UINT64 resource_offset = 0;
      auto *resource =
          LookupBufferResourceByGpuVirtualAddress(view.BufferLocation,
                                                  &resource_offset);
      if (!resource || !resource->GetBuffer())
        continue;
      enc.bindVertexBuffer(slot, resource_offset, view.StrideInBytes,
                           Rc<Buffer>(resource->GetBuffer()));
    }

    const auto table_size = uint64_t(__builtin_popcount(slot_mask)) * 16u;
    const auto offset = AllocateArgumentBuffer(argbuf_offset, table_size);
    enc.encodeVertexBuffers<PipelineKind::Ordinary>(slot_mask, offset);
  }

  void EncodeGraphicsBindings(ArgumentEncodingContext &enc,
                              const ReplayState &state,
                              PipelineState &pipeline,
                              uint64_t &argbuf_offset) {
    ApplyRootDescriptorTables(enc, state, pipeline, false);
    EncodeVertexBuffers(enc, state, pipeline.GetGraphicsState(),
                        argbuf_offset);
    const auto &shaders = pipeline.GetDxilShaders();
    const auto &key = pipeline.GetShaderCacheKey();
    for (const auto &shader : shaders) {
      if (shader.stage == PipelineShaderStage::Vertex)
        EncodeShaderBindingsForStage<PipelineStage::Vertex>(
            enc, shader, key, argbuf_offset);
      else if (shader.stage == PipelineShaderStage::Pixel)
        EncodeShaderBindingsForStage<PipelineStage::Pixel>(
            enc, shader, key, argbuf_offset);
    }
  }

  void EncodeComputeBindings(ArgumentEncodingContext &enc,
                             const ReplayState &state,
                             PipelineState &pipeline,
                             uint64_t &argbuf_offset) {
    ApplyRootDescriptorTables(enc, state, pipeline, true);
    const auto &shaders = pipeline.GetDxilShaders();
    const auto &key = pipeline.GetShaderCacheKey();
    for (const auto &shader : shaders) {
      if (shader.stage == PipelineShaderStage::Compute)
        EncodeShaderBindingsForStage<PipelineStage::Compute>(
            enc, shader, key, argbuf_offset);
    }
  }

  static uint64_t AlignArgumentBufferSize(uint64_t size) {
    return (size + 31ull) & ~31ull;
  }

  static uint64_t EstimateShaderArgumentBufferSize(
      const PipelineDxilShader &shader) {
    uint64_t size = 0;
    if (shader.reflection.NumConstantBuffers)
      size = AlignArgumentBufferSize(size) +
             (uint64_t(shader.reflection.NumConstantBuffers) << 3);
    if (shader.reflection.NumArguments)
      size = AlignArgumentBufferSize(size) +
             (uint64_t(shader.reflection.ArgumentTableQwords) << 3);
    return AlignArgumentBufferSize(size);
  }

  static uint64_t EstimateGraphicsArgumentBufferSize(PipelineState &pipeline) {
    uint64_t size = 0;
    if (const auto *graphics = pipeline.GetGraphicsState()) {
      uint32_t slot_mask = 0;
      for (const auto &element : graphics->input_elements) {
        if (element.InputSlot < 32)
          slot_mask |= 1u << element.InputSlot;
      }
      if (slot_mask)
        size = AlignArgumentBufferSize(size) +
               uint64_t(__builtin_popcount(slot_mask)) * 16u;
    }
    for (const auto &shader : pipeline.GetDxilShaders()) {
      if (shader.stage == PipelineShaderStage::Vertex ||
          shader.stage == PipelineShaderStage::Pixel)
        size = AlignArgumentBufferSize(size) +
               EstimateShaderArgumentBufferSize(shader);
    }
    return AlignArgumentBufferSize(size);
  }

  static uint64_t EstimateComputeArgumentBufferSize(PipelineState &pipeline) {
    uint64_t size = 0;
    for (const auto &shader : pipeline.GetDxilShaders()) {
      if (shader.stage == PipelineShaderStage::Compute)
        size = AlignArgumentBufferSize(size) +
               EstimateShaderArgumentBufferSize(shader);
    }
    return AlignArgumentBufferSize(size);
  }

  static bool ValidateComputeDispatch(const WMTSize &threadgroup_size,
                                      UINT x, UINT y, UINT z) {
    const auto threads_per_group = threadgroup_size.width *
                                   threadgroup_size.height *
                                   threadgroup_size.depth;
    if (!threadgroup_size.width || !threadgroup_size.height ||
        !threadgroup_size.depth || threads_per_group == 0) {
      WARN("D3D12CommandQueue: dispatch skipped because compute shader has invalid threadgroup size");
      return false;
    }
    if (threadgroup_size.width > 1024 || threadgroup_size.height > 1024 ||
        threadgroup_size.depth > 64 || threads_per_group > 1024) {
      WARN("D3D12CommandQueue: dispatch skipped because compute shader threadgroup size exceeds D3D limits size=",
           threadgroup_size.width, "x", threadgroup_size.height, "x",
           threadgroup_size.depth);
      return false;
    }
    if (x > 65535 || y > 65535 || z > 65535) {
      WARN("D3D12CommandQueue: dispatch grid exceeds D3D threadgroup-count limits grid=",
           x, "x", y, "x", z);
      return false;
    }
    return true;
  }

  ReplayRenderPassAttachments BuildRenderPassAttachments(
      const ReplayState &state) {
    ReplayRenderPassAttachments attachments = {};
    attachments.colors.reserve(state.render_targets.size());

    for (UINT i = 0; i < state.render_targets.size(); i++) {
      const auto &descriptor = state.render_targets[i];
      auto *resource = GetResource(descriptor.resource.ptr());
      if (!resource || !resource->GetTexture())
        continue;

      auto view = CreateRenderTargetView(*resource, descriptor);
      auto *texture = resource->GetTexture();
      attachments.colors.push_back({
          .texture = texture,
          .view = view,
          .slot = i,
          .array_length = GetRenderTargetArrayLength(descriptor),
          .width = texture->width(),
          .height = texture->height(),
          .format = texture->pixelFormat(),
      });
    }

    if (state.depth_stencil) {
      auto *resource = GetResource(state.depth_stencil->resource.ptr());
      if (resource && resource->GetTexture()) {
        auto view = CreateDepthStencilView(device_->GetMTLDevice(), *resource,
                                           *state.depth_stencil);
        auto *texture = resource->GetTexture();
        attachments.depth_stencil = ReplayDepthStencilAttachment{
            .texture = texture,
            .view = view,
            .array_length = GetDepthStencilArrayLength(*state.depth_stencil),
            .width = texture->width(),
            .height = texture->height(),
            .format = texture->pixelFormat(),
        };
      }
    }

    return attachments;
  }

  static bool BeginRenderPass(ArgumentEncodingContext &enc,
                              ReplayRenderPassAttachments &attachments,
                              uint64_t argument_buffer_size) {
    if (attachments.colors.empty() && !attachments.depth_stencil)
      return false;

    UINT render_target_count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t array_length = 1;
    for (const auto &color : attachments.colors) {
      render_target_count = std::max(render_target_count, color.slot + 1);
      width = width ? width : color.width;
      height = height ? height : color.height;
      array_length = std::max<uint32_t>(array_length, color.array_length);
    }
    if (attachments.depth_stencil) {
      width = width ? width : attachments.depth_stencil->width;
      height = height ? height : attachments.depth_stencil->height;
      array_length =
          std::max<uint32_t>(array_length, attachments.depth_stencil->array_length);
    }

    const auto dsv_format = attachments.depth_stencil
                                ? attachments.depth_stencil->format
                                : WMTPixelFormatInvalid;
    auto &info = *enc.startRenderPass(DepthStencilPlanarFlags(dsv_format), 0,
                                      render_target_count,
                                      argument_buffer_size);
    for (auto &rtv : attachments.colors) {
      auto &color = info.colors[rtv.slot];
      color.attachment = enc.access<PipelineStage::Pixel>(
          rtv.texture, rtv.view, ResourceAccess::ReadWrite);
      color.load_action = WMTLoadActionLoad;
      color.store_action = WMTStoreActionStore;
      color.depth_plane = 0;
      info.tile_barrier_pso_key.color_formats[rtv.slot] = rtv.format;
    }

    if (attachments.depth_stencil) {
      const auto planar_flags =
          DepthStencilPlanarFlags(attachments.depth_stencil->format);
      if (planar_flags & 1) {
        auto &depth = info.depth;
        depth.attachment = enc.access<PipelineStage::Pixel>(
            attachments.depth_stencil->texture, attachments.depth_stencil->view,
            ResourceAccess::ReadWrite);
        depth.load_action = WMTLoadActionLoad;
        depth.store_action = WMTStoreActionStore;
      }
      if (planar_flags & 2) {
        auto &stencil = info.stencil;
        stencil.attachment = enc.access<PipelineStage::Pixel>(
            attachments.depth_stencil->texture, attachments.depth_stencil->view,
            ResourceAccess::ReadWrite);
        stencil.load_action = WMTLoadActionLoad;
        stencil.store_action = WMTStoreActionStore;
      }
    }

    info.render_target_width = width;
    info.render_target_height = height;
    info.render_target_array_length = array_length;
    info.tile_barrier_pso_key.raster_sample_count = 1;
    return true;
  }

  static bool ResolveDynamicRasterRects(
      std::vector<D3D12_VIEWPORT> &viewports,
      std::vector<D3D12_RECT> &scissors, const char *context) {
    if (viewports.empty()) {
      WARN("D3D12CommandQueue: ", context,
           " skipped because no viewport was set");
      return false;
    }

    if (!scissors.empty())
      return true;

    scissors.reserve(viewports.size());
    for (const auto &viewport : viewports) {
      D3D12_RECT rect = {};
      rect.left = static_cast<LONG>(std::max(0.0f, viewport.TopLeftX));
      rect.top = static_cast<LONG>(std::max(0.0f, viewport.TopLeftY));
      rect.right =
          static_cast<LONG>(std::max(0.0f, viewport.TopLeftX + viewport.Width));
      rect.bottom =
          static_cast<LONG>(std::max(0.0f, viewport.TopLeftY + viewport.Height));
      scissors.push_back(rect);
    }
    WARN("D3D12CommandQueue: ", context,
         " used viewport-sized default scissor rects");
    return true;
  }

  static void EncodeDynamicRenderState(
      ArgumentEncodingContext &enc, const std::vector<D3D12_VIEWPORT> &viewports,
      const std::vector<D3D12_RECT> &scissors,
      const std::array<FLOAT, 4> &blend_factor, UINT stencil_ref) {
    auto &blend = enc.encodeRenderCommand<wmtcmd_render_setblendcolor>();
    blend.type = WMTRenderCommandSetBlendFactorAndStencilRef;
    blend.red = blend_factor[0];
    blend.green = blend_factor[1];
    blend.blue = blend_factor[2];
    blend.alpha = blend_factor[3];
    blend.stencil_ref = static_cast<uint8_t>(stencil_ref);

    auto &viewport_cmd = enc.encodeRenderCommand<wmtcmd_render_setviewports>();
    viewport_cmd.type = WMTRenderCommandSetViewports;
    auto *viewport_data = static_cast<WMTViewport *>(
        enc.allocate_cpu_heap(sizeof(WMTViewport) * viewports.size(),
                              alignof(WMTViewport)));
    for (size_t i = 0; i < viewports.size(); i++) {
      const auto &viewport = viewports[i];
      viewport_data[i] = {viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                          viewport.Height, viewport.MinDepth,
                          viewport.MaxDepth};
    }
    viewport_cmd.viewports.set(viewport_data);
    viewport_cmd.viewport_count = viewports.size();

    auto &scissor_cmd =
        enc.encodeRenderCommand<wmtcmd_render_setscissorrects>();
    scissor_cmd.type = WMTRenderCommandSetScissorRects;
    auto *scissor_data = static_cast<WMTScissorRect *>(
        enc.allocate_cpu_heap(sizeof(WMTScissorRect) * scissors.size(),
                              alignof(WMTScissorRect)));
    for (size_t i = 0; i < scissors.size(); i++) {
      const auto &rect = scissors[i];
      scissor_data[i] = {uint32_t(std::max<LONG>(0, rect.left)),
                         uint32_t(std::max<LONG>(0, rect.top)),
                         uint32_t(std::max<LONG>(0, rect.right - rect.left)),
                         uint32_t(std::max<LONG>(0, rect.bottom - rect.top))};
    }
    scissor_cmd.scissor_rects.set(scissor_data);
    scissor_cmd.rect_count = scissors.size();
  }

  void ReplayDrawInstanced(CommandChunk *chunk, ReplayState &state,
                           const DrawInstancedRecord &record) {
    if (!record.vertex_count_per_instance || !record.instance_count)
      return;
    if (!PredicationAllows(state))
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: draw skipped without graphics pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalGraphicsState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    const auto primitive = GetPrimitiveType(state.topology);
    if (!primitive) {
      WARN("D3D12CommandQueue: draw skipped because primitive topology is unsupported topology=",
           uint32_t(state.topology));
      return;
    }
    auto viewports = state.viewports;
    auto scissors = state.scissors;
    auto attachments = BuildRenderPassAttachments(state);
    if (!ResolveDynamicRasterRects(viewports, scissors, "draw"))
      return;
    const auto argument_buffer_size = EstimateGraphicsArgumentBufferSize(*pipeline);
    chunk->emitcc([this, metal_pso = metal->pso,
                   depth_stencil = metal->depth_stencil,
                   rasterizer = metal->rasterizer,
                   pipeline, replay_state = state, primitive,
                   argument_buffer_size,
                   blend_factor = state.blend_factor,
                   stencil_ref = state.stencil_ref,
                   vertex_start = record.start_vertex_location,
                   vertex_count = record.vertex_count_per_instance,
                   instance_count = record.instance_count,
                   base_instance = record.start_instance_location,
                   viewports = std::move(viewports),
                   scissors = std::move(scissors),
                   attachments = std::move(attachments)](ArgumentEncodingContext &enc) mutable {
      if (!BeginRenderPass(enc, attachments, argument_buffer_size))
        return;

      auto &set_pso = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      set_pso.type = WMTRenderCommandSetPSO;
      set_pso.pso = metal_pso;
      if (depth_stencil) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
        cmd.type = WMTRenderCommandSetDSSO;
        cmd.dsso = depth_stencil;
        cmd.stencil_ref = static_cast<uint8_t>(stencil_ref);
      }
      auto &rs = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      rs = rasterizer;

      uint64_t argbuf_offset = 0;
      EncodeGraphicsBindings(enc, replay_state, *pipeline, argbuf_offset);
      EncodeDynamicRenderState(enc, viewports, scissors, blend_factor,
                               stencil_ref);

      auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw>();
      draw.type = WMTRenderCommandDraw;
      draw.primitive_type = *primitive;
      draw.vertex_start = vertex_start;
      draw.vertex_count = vertex_count;
      draw.instance_count = instance_count;
      draw.base_instance = base_instance;
      enc.endPass();
    });
  }

  void ReplayDrawIndexedInstanced(CommandChunk *chunk, ReplayState &state,
                                  const DrawIndexedInstancedRecord &record) {
    if (!record.index_count_per_instance || !record.instance_count ||
        !state.index_buffer)
      return;
    if (!PredicationAllows(state))
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: indexed draw skipped without graphics pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalGraphicsState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: indexed draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    UINT64 index_resource_offset = 0;
    auto *index_resource = LookupBufferResourceByGpuVirtualAddress(
        state.index_buffer->BufferLocation, &index_resource_offset);
    if (!index_resource || !index_resource->GetBufferAllocation()) {
      WARN("D3D12CommandQueue: indexed draw skipped because index buffer binding is unavailable");
      return;
    }
    if (!IsSupportedIndexBufferFormat(state.index_buffer->Format)) {
      WARN("D3D12CommandQueue: indexed draw skipped because index buffer format is unsupported format=",
           uint32_t(state.index_buffer->Format));
      return;
    }

    Rc<BufferAllocation> index_allocation = index_resource->GetBufferAllocation();
    const auto primitive = GetPrimitiveType(state.topology);
    if (!primitive) {
      WARN("D3D12CommandQueue: indexed draw skipped because primitive topology is unsupported topology=",
           uint32_t(state.topology));
      return;
    }
    const auto index_type = GetIndexType(state.index_buffer->Format);
    const UINT64 index_offset = index_resource->GetHeapOffset() +
                                index_resource_offset +
                                record.start_index_location *
                                    GetIndexSize(state.index_buffer->Format);
    auto attachments = BuildRenderPassAttachments(state);
    auto viewports = state.viewports;
    auto scissors = state.scissors;
    if (!ResolveDynamicRasterRects(viewports, scissors, "indexed draw"))
      return;
    const auto argument_buffer_size = EstimateGraphicsArgumentBufferSize(*pipeline);
    chunk->emitcc([this, metal_pso = metal->pso,
                   depth_stencil = metal->depth_stencil,
                   rasterizer = metal->rasterizer,
                   pipeline, replay_state = state,
                   index_allocation, primitive,
                   index_type, index_offset,
                   argument_buffer_size,
                   blend_factor = state.blend_factor,
                   stencil_ref = state.stencil_ref,
                   viewports = std::move(viewports),
                   scissors = std::move(scissors),
                   index_count = record.index_count_per_instance,
                   instance_count = record.instance_count,
                   base_vertex = record.base_vertex_location,
                   base_instance = record.start_instance_location,
                   attachments = std::move(attachments)](ArgumentEncodingContext &enc) mutable {
      enc.retainAllocation(index_allocation.ptr());
      if (!BeginRenderPass(enc, attachments, argument_buffer_size))
        return;
      auto &set_pso = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      set_pso.type = WMTRenderCommandSetPSO;
      set_pso.pso = metal_pso;
      if (depth_stencil) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
        cmd.type = WMTRenderCommandSetDSSO;
        cmd.dsso = depth_stencil;
        cmd.stencil_ref = static_cast<uint8_t>(stencil_ref);
      }
      auto &rs = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      rs = rasterizer;

      uint64_t argbuf_offset = 0;
      EncodeGraphicsBindings(enc, replay_state, *pipeline, argbuf_offset);
      EncodeDynamicRenderState(enc, viewports, scissors, blend_factor,
                               stencil_ref);

      auto &draw = enc.encodeRenderCommand<wmtcmd_render_draw_indexed>();
      draw.type = WMTRenderCommandDrawIndexed;
      draw.primitive_type = *primitive;
      draw.index_type = index_type;
      draw.index_count = index_count;
      draw.index_buffer = index_allocation->buffer();
      draw.index_buffer_offset = index_offset;
      draw.instance_count = instance_count;
      draw.base_vertex = base_vertex;
      draw.base_instance = base_instance;
      enc.endPass();
    });
  }

  void ReplayDispatch(CommandChunk *chunk, ReplayState &state,
                      const DispatchRecord &record) {
    if (!record.x || !record.y || !record.z)
      return;
    if (!PredicationAllows(state))
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: dispatch skipped without compute pipeline state");
      return;
    }

    auto *metal = pipeline->GetMetalComputeState();
    if (!metal || !metal->pso) {
      WARN("D3D12CommandQueue: dispatch skipped because Metal compute PSO is unavailable");
      return;
    }

    if (!ValidateComputeDispatch(metal->threadgroup_size, record.x, record.y,
                                 record.z))
      return;

    const auto argument_buffer_size = EstimateComputeArgumentBufferSize(*pipeline);
    chunk->emitcc([this, metal_pso = metal->pso,
                   threadgroup_size = metal->threadgroup_size,
                   pipeline, replay_state = state,
                   argument_buffer_size,
                   x = record.x, y = record.y, z = record.z](ArgumentEncodingContext &enc) {
      enc.startComputePass(argument_buffer_size);
      auto &set_pso = enc.encodeComputeCommand<wmtcmd_compute_setpso>();
      set_pso.type = WMTComputeCommandSetPSO;
      set_pso.pso = metal_pso;
      set_pso.threadgroup_size = threadgroup_size;

      uint64_t argbuf_offset = 0;
      EncodeComputeBindings(enc, replay_state, *pipeline, argbuf_offset);
      if (argbuf_offset > argument_buffer_size) {
        WARN("D3D12CommandQueue: compute argument buffer estimate was too small estimated=",
             argument_buffer_size, " actual=", argbuf_offset);
      }

      auto &dispatch = enc.encodeComputeCommand<wmtcmd_compute_dispatch>();
      dispatch.type = WMTComputeCommandDispatch;
      dispatch.size = {x, y, z};
      enc.endPass();
    });
  }

  void ReplayCopyResource(CommandChunk *chunk, const CopyResourceRecord &record) {
    auto *dst = GetResource(record.dst.ptr());
    auto *src = GetResource(record.src.ptr());
    if (!dst || !src)
      return;

    if (dst->GetBufferAllocation() && src->GetBufferAllocation()) {
      const UINT64 size = std::min(dst->GetResourceDesc().Width,
                                   src->GetResourceDesc().Width);
      CopyBufferRegionRecord copy = {};
      copy.dst = record.dst;
      copy.src = record.src;
      copy.byte_count = size;
      ReplayCopyBufferRegion(chunk, copy);
      return;
    }

    if (dst->GetTextureAllocation() && src->GetTextureAllocation()) {
      CopyTextureRegionRecord copy = {};
      copy.dst.resource = record.dst;
      copy.dst.type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      copy.src.resource = record.src;
      copy.src.type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      const UINT dst_subresources =
          (dst->GetResourceDesc().MipLevels ? dst->GetResourceDesc().MipLevels : 1) *
          (dst->GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
               ? 1
               : dst->GetResourceDesc().DepthOrArraySize);
      const UINT src_subresources =
          (src->GetResourceDesc().MipLevels ? src->GetResourceDesc().MipLevels : 1) *
          (src->GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
               ? 1
               : src->GetResourceDesc().DepthOrArraySize);
      const UINT count = std::min(dst_subresources, src_subresources);
      for (UINT i = 0; i < count; i++) {
        copy.dst.subresource_index = i;
        copy.src.subresource_index = i;
        ReplayCopyTextureRegion(chunk, copy);
      }
    }
  }

  TextureViewKey CreateResolveView(Resource &resource, UINT subresource,
                                   WMTPixelFormat format) {
    auto *texture = resource.GetTexture();
    if (!texture)
      return {};

    TextureViewDescriptor view = {};
    view.format = format;
    view.type = texture->textureType();
    view.firstMiplevel = GetMipLevel(resource, subresource);
    view.miplevelCount = 1;
    view.firstArraySlice = GetArraySlice(resource, subresource);
    view.arraySize = 1;
    view.intendedUsage = WMTTextureUsageRenderTarget;
    return texture->createView(view);
  }

  void ReplayResolveSubresource(CommandChunk *chunk,
                                const ResolveSubresourceRecord &record) {
    auto *dst = GetResource(record.dst.ptr());
    auto *src = GetResource(record.src.ptr());
    if (!dst || !src || !dst->GetTexture() || !src->GetTexture())
      return;

    const auto &dst_desc = dst->GetResourceDesc();
    const auto &src_desc = src->GetResourceDesc();
    if (src_desc.SampleDesc.Count <= 1 || dst_desc.SampleDesc.Count != 1) {
      WARN("D3D12CommandQueue: ResolveSubresource supports MSAA color source to single-sample destination only");
      return;
    }
    if (record.dst_subresource >= GetSubresourceCount(*dst) ||
        record.src_subresource >= GetSubresourceCount(*src)) {
      WARN("D3D12CommandQueue: ResolveSubresource subresource out of range");
      return;
    }

    WMTPixelFormat format = src->GetTexture()->pixelFormat();
    if (record.format != DXGI_FORMAT_UNKNOWN) {
      MTL_DXGI_FORMAT_DESC format_desc = {};
      if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(), record.format,
                                    format_desc)) ||
          format_desc.PixelFormat == WMTPixelFormatInvalid) {
        WARN("D3D12CommandQueue: ResolveSubresource unsupported format ",
             uint32_t(record.format));
        return;
      }
      format = format_desc.PixelFormat;
    }

    if (src->GetTexture()->pixelFormat() != format ||
        dst->GetTexture()->pixelFormat() != format) {
      WARN("D3D12CommandQueue: ResolveSubresource currently supports same-format color resolves only");
      return;
    }

    auto src_view = CreateResolveView(*src, record.src_subresource, format);
    auto dst_view = CreateResolveView(*dst, record.dst_subresource, format);
    Rc<Texture> src_texture = src->GetTexture();
    Rc<Texture> dst_texture = dst->GetTexture();
    chunk->emitcc([src_texture = std::move(src_texture),
                   dst_texture = std::move(dst_texture), src_view,
                   dst_view](ArgumentEncodingContext &enc) mutable {
      enc.resolveTexture(std::move(src_texture), src_view,
                         std::move(dst_texture), dst_view);
    });
  }

  void ReplayCopyTextureRegion(CommandChunk *chunk,
                               const CopyTextureRegionRecord &record) {
    auto *dst = GetResource(record.dst.resource.ptr());
    auto *src = GetResource(record.src.resource.ptr());
    if (!dst || !src)
      return;

    if (dst->GetTextureAllocation() && src->GetTextureAllocation() &&
        dst->GetTexture() && src->GetTexture()) {
      const UINT dst_subresource =
          record.dst.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
              ? record.dst.subresource_index
              : 0;
      const UINT src_subresource =
          record.src.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
              ? record.src.subresource_index
              : 0;
      const auto size =
          GetSubresourceSize(*src, src_subresource,
                             record.src_box ? &*record.src_box : nullptr);
      const auto src_origin = record.src_box
                                  ? WMTOrigin{record.src_box->left,
                                              record.src_box->top,
                                              record.src_box->front}
                                  : WMTOrigin{0, 0, 0};
      const auto dst_origin = WMTOrigin{record.dst_x, record.dst_y, record.dst_z};
      Rc<TextureAllocation> dst_allocation = dst->GetTextureAllocation();
      Rc<TextureAllocation> src_allocation = src->GetTextureAllocation();
      const UINT dst_slice = GetArraySlice(*dst, dst_subresource);
      const UINT dst_level = GetMipLevel(*dst, dst_subresource);
      const UINT src_slice = GetArraySlice(*src, src_subresource);
      const UINT src_level = GetMipLevel(*src, src_subresource);
      Rc<Texture> dst_texture = dst->GetTexture();
      Rc<Texture> src_texture = src->GetTexture();
      chunk->emitcc([dst_texture = std::move(dst_texture),
                     src_texture = std::move(src_texture), dst_slice, dst_level,
                     src_slice, src_level, src_origin, dst_origin,
                     size](ArgumentEncodingContext &enc) {
        enc.startBlitPass();
        auto src = enc.access(src_texture, src_level, src_slice,
                              ResourceAccess::Read);
        auto dst = enc.access(dst_texture, dst_level, dst_slice,
                              ResourceAccess::Write);
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
        copy.type = WMTBlitCommandCopyFromTextureToTexture;
        copy.src = src;
        copy.src_slice = src_slice;
        copy.src_level = src_level;
        copy.src_origin = src_origin;
        copy.src_size = size;
        copy.dst = dst;
        copy.dst_slice = dst_slice;
        copy.dst_level = dst_level;
        copy.dst_origin = dst_origin;
        enc.endPass();
      });
      return;
    }

    ReplayBufferTextureCopy(chunk, record, *dst, *src);
  }

  void ReplayBufferTextureCopy(CommandChunk *chunk,
                               const CopyTextureRegionRecord &record,
                               Resource &dst, Resource &src) {
    const bool dst_is_buffer = dst.GetBufferAllocation() != nullptr;
    const bool src_is_buffer = src.GetBufferAllocation() != nullptr;
    if (dst_is_buffer == src_is_buffer)
      return;

    auto &buffer_resource = dst_is_buffer ? dst : src;
    auto &texture_resource = dst_is_buffer ? src : dst;
    Rc<Buffer> buffer = buffer_resource.GetBuffer();
    Rc<Texture> texture = texture_resource.GetTexture();
    if (!buffer || !texture)
      return;

    const auto &buffer_location = dst_is_buffer ? record.dst : record.src;
    const auto &texture_location = dst_is_buffer ? record.src : record.dst;
    if (buffer_location.type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
      return;
    const UINT subresource =
        texture_location.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
            ? texture_location.subresource_index
            : 0;
    const UINT slice = GetArraySlice(texture_resource, subresource);
    const UINT level = GetMipLevel(texture_resource, subresource);
    const auto size =
        GetSubresourceSize(texture_resource, subresource,
                           record.src_box ? &*record.src_box : nullptr);
    const auto origin = record.src_box
                            ? WMTOrigin{record.src_box->left,
                                        record.src_box->top,
                                        record.src_box->front}
                            : WMTOrigin{dst_is_buffer ? 0u : record.dst_x,
                                        dst_is_buffer ? 0u : record.dst_y,
                                        dst_is_buffer ? 0u : record.dst_z};
    const auto footprint = buffer_location.placed_footprint.Footprint;
    const UINT64 buffer_offset =
        buffer_resource.GetHeapOffset() + buffer_location.placed_footprint.Offset;
    const UINT row_pitch = footprint.RowPitch;
    const UINT image_pitch = footprint.RowPitch * footprint.Height;

    chunk->emitcc([dst_is_buffer, buffer = std::move(buffer),
                   texture = std::move(texture),
                   buffer_offset, row_pitch, image_pitch, size, origin, slice,
                   level](ArgumentEncodingContext &enc) {
      enc.startBlitPass();
      if (dst_is_buffer) {
        auto src = enc.access(texture, level, slice, ResourceAccess::Read);
        auto [dst, dst_offset] =
            enc.access(buffer, buffer_offset, image_pitch, ResourceAccess::Write);
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
        copy.type = WMTBlitCommandCopyFromTextureToBuffer;
        copy.src = src;
        copy.slice = slice;
        copy.level = level;
        copy.origin = origin;
        copy.size = size;
        copy.dst = dst->buffer();
        copy.offset = dst_offset + buffer_offset;
        copy.bytes_per_row = row_pitch;
        copy.bytes_per_image = image_pitch;
      } else {
        auto [src, src_offset] =
            enc.access(buffer, buffer_offset, image_pitch, ResourceAccess::Read);
        auto dst = enc.access(texture, level, slice, ResourceAccess::Write);
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        copy.type = WMTBlitCommandCopyFromBufferToTexture;
        copy.src = src->buffer();
        copy.src_offset = src_offset + buffer_offset;
        copy.bytes_per_row = row_pitch;
        copy.bytes_per_image = image_pitch;
        copy.size = size;
        copy.dst = dst;
        copy.slice = slice;
        copy.level = level;
        copy.origin = origin;
      }
      enc.endPass();
    });
  }

  void ReplayClearRenderTarget(CommandChunk *chunk,
                               const ClearRenderTargetRecord &record) {
    auto *resource = GetResource(record.descriptor.resource.ptr());
    if (!resource || !resource->GetTexture() || !resource->GetTextureAllocation())
      return;

    Rc<Texture> texture = resource->GetTexture();
    auto view = CreateRenderTargetView(*resource, record.descriptor);
    const UINT array_length = GetRenderTargetArrayLength(record.descriptor);
    WMTClearColor color = {record.color[0], record.color[1], record.color[2],
                           record.color[3]};
    chunk->emitcc([texture = std::move(texture), view, array_length,
                   color](ArgumentEncodingContext &enc) mutable {
      enc.clearColor(std::move(texture), view, array_length, color);
    });
  }

  void ReplayClearDepthStencil(CommandChunk *chunk,
                               const ClearDepthStencilRecord &record) {
    auto *resource = GetResource(record.descriptor.resource.ptr());
    if (!resource || !resource->GetTexture() || !resource->GetTextureAllocation())
      return;

    Rc<Texture> texture = resource->GetTexture();
    auto view = CreateDepthStencilView(device_->GetMTLDevice(), *resource,
                                       record.descriptor);
    const UINT array_length = GetDepthStencilArrayLength(record.descriptor);
    unsigned flags = 0;
    if (record.flags & D3D12_CLEAR_FLAG_DEPTH)
      flags |= 1;
    if (record.flags & D3D12_CLEAR_FLAG_STENCIL)
      flags |= 2;
    chunk->emitcc([texture = std::move(texture), view, array_length, flags,
                   depth = record.depth,
                   stencil = record.stencil](ArgumentEncodingContext &enc) mutable {
      enc.clearDepthStencil(std::move(texture), view, array_length, flags,
                            depth, stencil);
    });
  }

  void ReplayClearUnorderedAccess(CommandChunk *chunk,
                                  const ClearUnorderedAccessRecord &record) {
    auto *resource = GetResource(record.resource.ptr());
    if (!resource) {
      WARN("D3D12CommandQueue: ClearUnorderedAccessView skipped for foreign resource");
      return;
    }

    if (resource->GetBuffer()) {
      UINT64 offset = 0;
      UINT64 byte_size = resource->GetResourceDesc().Width;
      uint64_t view_id = 0;
      bool raw_buffer = false;

      if (record.descriptor.has_desc &&
          record.descriptor.desc.uav.ViewDimension ==
              D3D12_UAV_DIMENSION_BUFFER) {
        const auto &uav = record.descriptor.desc.uav;
        const UINT64 first_element = uav.Buffer.FirstElement;
        if (uav.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) {
          raw_buffer = true;
          offset += first_element * sizeof(uint32_t);
          byte_size = UINT64(uav.Buffer.NumElements) * sizeof(uint32_t);
        } else if (uav.Format != DXGI_FORMAT_UNKNOWN) {
          MTL_DXGI_FORMAT_DESC format = {};
          if (SUCCEEDED(MTLQueryDXGIFormat(device_->GetMTLDevice(),
                                           uav.Format, format))) {
            offset += first_element * format.BytesPerTexel;
            byte_size = UINT64(uav.Buffer.NumElements) *
                        format.BytesPerTexel;
            view_id = CreateBufferView(device_->GetMTLDevice(), *resource,
                                       uav.Format, offset, byte_size,
                                       WMTTextureUsageShaderRead |
                                           WMTTextureUsageShaderWrite);
          }
        } else if (uav.Buffer.StructureByteStride) {
          offset += first_element * uav.Buffer.StructureByteStride;
          byte_size = UINT64(uav.Buffer.NumElements) *
                      uav.Buffer.StructureByteStride;
          if (const auto view_format =
                  UintBufferViewFormatForStride(uav.Buffer.StructureByteStride);
              view_format != DXGI_FORMAT_UNKNOWN) {
            view_id = CreateBufferView(device_->GetMTLDevice(), *resource,
                                       view_format, offset, byte_size,
                                       WMTTextureUsageShaderRead |
                                           WMTTextureUsageShaderWrite);
          }
        }
      } else {
        raw_buffer = true;
      }

      if (!view_id && !raw_buffer) {
        WARN("D3D12CommandQueue: ClearUnorderedAccessView buffer view is unsupported");
        return;
      }

      Rc<Buffer> buffer = resource->GetBuffer();
      const UINT element_count =
          UINT(std::min<UINT64>(byte_size / sizeof(uint32_t), UINT_MAX));
      if (!element_count)
        return;
      chunk->emitcc([buffer = std::move(buffer), view_id, raw_buffer,
                     integer = record.integer,
                     uint_values = record.uint_values,
                     float_values = record.float_values,
                     byte_offset = offset,
                     byte_size, element_count](ArgumentEncodingContext &enc) mutable {
        if (raw_buffer) {
          enc.startComputePass(0);
          auto [allocation, suballocation_offset] =
              enc.access(buffer, byte_offset, byte_size, ResourceAccess::Write);
          if (integer)
            enc.emulated_cmd.ClearBufferUint(allocation->buffer(),
                                             suballocation_offset + byte_offset,
                                             element_count, uint_values);
          else
            enc.emulated_cmd.ClearBufferFloat(allocation->buffer(),
                                               suballocation_offset + byte_offset,
                                               element_count, float_values);
          enc.endPass();
        } else {
          if (integer)
            enc.clear_res_cmd.begin(uint_values, Rc<Buffer>(buffer), view_id);
          else
            enc.clear_res_cmd.begin(float_values, Rc<Buffer>(buffer), view_id);
          enc.clear_res_cmd.clear(0, 0, element_count, 1);
        }
        enc.clear_res_cmd.end();
      });
      return;
    }

    if (resource->GetTexture()) {
      auto view = CreateUnorderedAccessTextureView(device_->GetMTLDevice(),
                                                   *resource, record.descriptor);
      if (!uint64_t(view))
        return;
      auto *texture = resource->GetTexture();
      const auto type = texture->textureType(view);
      if (type != WMTTextureType2D && type != WMTTextureType2DArray) {
        WARN("D3D12CommandQueue: ClearUnorderedAccessView texture type is unsupported");
        return;
      }
      std::vector<D3D12_RECT> rects = record.rects;
      if (rects.empty()) {
        rects.push_back({0, 0, static_cast<LONG>(texture->width(view)),
                         static_cast<LONG>(texture->height(view))});
      }
      Rc<Texture> rc_texture = texture;
      chunk->emitcc([texture = std::move(rc_texture), view,
                     integer = record.integer,
                     uint_values = record.uint_values,
                     float_values = record.float_values,
                     rects = std::move(rects)](ArgumentEncodingContext &enc) mutable {
        if (integer)
          enc.clear_res_cmd.begin(uint_values, Rc<Texture>(texture), view);
        else
          enc.clear_res_cmd.begin(float_values, Rc<Texture>(texture), view);
        for (const auto &rect : rects) {
          const auto left = uint32_t(std::max<LONG>(0, rect.left));
          const auto top = uint32_t(std::max<LONG>(0, rect.top));
          const auto width =
              uint32_t(std::max<LONG>(0, rect.right - rect.left));
          const auto height =
              uint32_t(std::max<LONG>(0, rect.bottom - rect.top));
          if (width && height)
            enc.clear_res_cmd.clear(left, top, width, height);
        }
        enc.clear_res_cmd.end();
      });
      return;
    }

    WARN("D3D12CommandQueue: ClearUnorderedAccessView resource has no backing allocation");
  }

  void ReplayDiscardResource(CommandChunk *chunk,
                             const DiscardResourceRecord &record) {
    auto *resource = GetResource(record.resource.ptr());
    if (!resource) {
      WARN("D3D12CommandQueue: DiscardResource skipped for foreign resource");
      return;
    }

    WARN("D3D12CommandQueue: DiscardResource treated as conservative no-op");
    EmitResourceAccessBarrier(chunk, *resource, 0, GetSubresourceCount(*resource),
                              ResourceAccess::All);
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_COMMAND_QUEUE_DESC desc_ = {};
  UINT64 submitted_batches_ = 0;
  UINT64 signal_count_ = 0;
  UINT64 last_signal_value_ = 0;
  std::vector<UINT64> wait_values_;
  std::unordered_map<ID3D12Resource *,
                     std::vector<D3D12_RESOURCE_STATES>> resource_states_;
  std::mutex mutex_;
  std::string name_;
};

} // namespace

HRESULT
CreateCommandQueue(IMTLD3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc,
                   REFIID riid, void **command_queue) {
  InitReturnPtr(command_queue);
  if (!command_queue)
    return E_INVALIDARG;

  D3D12_COMMAND_QUEUE_DESC normalized = {};
  HRESULT hr = NormalizeQueueDesc(desc, normalized);
  if (FAILED(hr))
    return hr;

  auto queue = Com<ID3D12CommandQueue>::transfer(new CommandQueueImpl(device, normalized));
  return queue->QueryInterface(riid, command_queue);
}

} // namespace dxmt::d3d12
