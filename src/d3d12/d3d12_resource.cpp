#include "d3d12_resource.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_heap.hpp"
#include "dxmt_format.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <array>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {
namespace {

constexpr UINT kMaxTexturePlanes = 3;

static UINT64
Align(UINT64 value, UINT64 alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

struct BufferGpuVirtualAddressRange {
  D3D12_GPU_VIRTUAL_ADDRESS base = 0;
  UINT64 size = 0;
  Resource *resource = nullptr;
};

std::mutex g_buffer_va_mutex;
std::vector<BufferGpuVirtualAddressRange> g_buffer_va_ranges;

bool ShouldLogRepeatedGpuVaWarning(std::atomic<uint32_t> &counter) {
  const uint32_t count = counter.fetch_add(1, std::memory_order_relaxed);
  return count < 8 || (count & (count - 1)) == 0;
}

bool BufferGpuVirtualAddressRangeContains(
    const BufferGpuVirtualAddressRange &range,
    D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (!range.base || !range.size)
    return false;
  const UINT64 offset = address - range.base;
  return address >= range.base && offset < range.size;
}

bool BufferGpuVirtualAddressRangesOverlap(
    D3D12_GPU_VIRTUAL_ADDRESS a_base, UINT64 a_size,
    D3D12_GPU_VIRTUAL_ADDRESS b_base, UINT64 b_size) {
  if (!a_base || !a_size || !b_base || !b_size)
    return false;
  const UINT64 a_offset = b_base - a_base;
  const UINT64 b_offset = a_base - b_base;
  return (b_base >= a_base && a_offset < a_size) ||
         (a_base >= b_base && b_offset < b_size);
}

void RegisterBufferGpuVirtualAddress(Resource *resource,
                                     D3D12_GPU_VIRTUAL_ADDRESS base,
                                     UINT64 size) {
  if (!base || !size)
    return;

  std::lock_guard lock(g_buffer_va_mutex);
  std::erase_if(g_buffer_va_ranges,
                [resource](const BufferGpuVirtualAddressRange &range) {
                  return range.resource == resource;
                });
  size_t removed_overlaps = 0;
  std::erase_if(g_buffer_va_ranges,
                [&](const BufferGpuVirtualAddressRange &range) {
                  const bool overlaps = BufferGpuVirtualAddressRangesOverlap(
                      base, size, range.base, range.size);
                  if (overlaps)
                    removed_overlaps++;
                  return overlaps;
                });
  if (removed_overlaps) {
    static std::atomic<uint32_t> overlap_log_count = 0;
    if (ShouldLogRepeatedGpuVaWarning(overlap_log_count)) {
      WARN("D3D12Resource: GPU virtual address range replaced overlapping"
           " registrations base=", uint64_t(base),
           " size=", size,
           " overlapCount=", removed_overlaps);
    }
  }
  g_buffer_va_ranges.push_back({base, size, resource});
}

void UnregisterBufferGpuVirtualAddress(Resource *resource) {
  std::lock_guard lock(g_buffer_va_mutex);
  std::erase_if(g_buffer_va_ranges,
                [resource](const BufferGpuVirtualAddressRange &range) {
                  return range.resource == resource;
                });
}

WMTTextureUsage
GetTextureUsage(D3D12_RESOURCE_FLAGS flags) {
  WMTTextureUsage usage =
      WMTTextureUsageShaderRead | WMTTextureUsagePixelFormatView;
  if (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
               D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    usage |= WMTTextureUsageRenderTarget;
  if (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    usage |= WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
  return usage;
}

DXGI_FORMAT
ResolveTextureBackingFormat(const D3D12_RESOURCE_DESC &desc, UINT plane = 0) {
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  if ((traits.flags & DXGI_FORMAT_TRAIT_VIDEO) && traits.planeCount) {
    if (plane >= traits.planeCount)
      return DXGI_FORMAT_UNKNOWN;
    return static_cast<DXGI_FORMAT>(traits.planes[plane].backingFormat);
  }

  if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    return desc.Format;

  switch (desc.Format) {
  case DXGI_FORMAT_R16_TYPELESS:
    return DXGI_FORMAT_D16_UNORM;
  case DXGI_FORMAT_R32_TYPELESS:
    return DXGI_FORMAT_D32_FLOAT;
  default:
    return desc.Format;
  }
}

static UINT
GetTextureLogicalPlaneCount(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  return std::max(1u, traits.planeCount);
}

static bool
UsesSplitPlaneBacking(const D3D12_RESOURCE_DESC &desc) {
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  return (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) && traits.planeCount > 1;
}

static UINT
GetTextureBackingPlaneCount(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;
  return UsesSplitPlaneBacking(desc) ? GetTextureLogicalPlaneCount(desc) : 1;
}

static UINT
GetTextureBackingPlaneIndex(const D3D12_RESOURCE_DESC &desc, UINT plane) {
  return UsesSplitPlaneBacking(desc) ? plane : 0;
}

static bool
IsDepthStencilResourceFormat(DXGI_FORMAT format) {
  return GetDXGIFormatTraits(format).flags & DXGI_FORMAT_TRAIT_DEPTH_STENCIL;
}

static TextureViewKey
CreateDepthStencilPlaneReadView(dxmt::Texture *texture, UINT plane, UINT level,
                                UINT slice) {
  if (!texture)
    return {};

  TextureViewDescriptor view = {};
  switch (texture->textureType()) {
  case WMTTextureType2D:
  case WMTTextureType2DArray:
  case WMTTextureTypeCube:
  case WMTTextureTypeCubeArray:
    view.type = WMTTextureType2D;
    break;
  default:
    return {};
  }
  view.format = plane ? WMTPixelFormatX32G8X32 : texture->pixelFormat();
  view.firstMiplevel = level;
  view.miplevelCount = 1;
  view.firstArraySlice = slice;
  view.arraySize = 1;
  view.intendedUsage = WMTTextureUsageShaderRead;
  return texture->createView(view);
}

static UINT64
GetTexturePlaneWidth(const D3D12_RESOURCE_DESC &desc, UINT plane) {
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  const UINT subsample =
      plane < traits.planeCount ? traits.planes[plane].subsampleXLog2 : 0;
  return std::max<UINT64>(1, desc.Width >> subsample);
}

static UINT
GetTexturePlaneHeight(const D3D12_RESOURCE_DESC &desc, UINT plane) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
    return 1;
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  const UINT subsample =
      plane < traits.planeCount ? traits.planes[plane].subsampleYLog2 : 0;
  return std::max<UINT>(1, desc.Height >> subsample);
}

WMTTextureType
GetTextureType(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
    return desc.DepthOrArraySize > 1 ? WMTTextureType2DArray
                                     : WMTTextureType2D;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return WMTTextureType3D;
  if (desc.SampleDesc.Count > 1)
    return desc.DepthOrArraySize > 1 ? WMTTextureType2DMultisampleArray
                                     : WMTTextureType2DMultisample;
  return desc.DepthOrArraySize > 1 ? WMTTextureType2DArray : WMTTextureType2D;
}

struct TextureSubresourceLayout {
  UINT64 width = 0;
  UINT height = 0;
  UINT depth = 0;
  UINT block_width = 1;
  UINT block_height = 1;
  UINT row_count = 0;
  UINT64 row_size = 0;
  UINT row_pitch = 0;
  UINT slice_pitch = 0;
  UINT element_size = 0;
};

static UINT
GetMaxMipLevels(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;
  UINT64 width = std::max<UINT64>(1, desc.Width);
  UINT height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                    ? 1
                    : std::max<UINT>(1, desc.Height);
  UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                   ? std::max<UINT>(1, desc.DepthOrArraySize)
                   : 1;
  UINT levels = 1;
  while (width > 1 || height > 1 || depth > 1) {
    width = std::max<UINT64>(1, width >> 1);
    height = std::max<UINT>(1, height >> 1);
    depth = std::max<UINT>(1, depth >> 1);
    levels++;
  }
  return levels;
}

static UINT
GetMipLevels(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return desc.MipLevels ? desc.MipLevels : 1;
  if (desc.MipLevels)
    return desc.MipLevels;
  return GetMaxMipLevels(desc);
}

static UINT64
GetDefaultResourceAlignment(const D3D12_RESOURCE_DESC &desc) {
  return desc.SampleDesc.Count > 1
             ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
             : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
}

static bool
IsSupportedSampleCount(UINT sample_count) {
  return sample_count == 1 || sample_count == 2 || sample_count == 4 ||
         sample_count == 8;
}

static UINT
GetTextureSubresourceCount(const D3D12_RESOURCE_DESC &desc) {
  const UINT mip_levels = GetMipLevels(desc);
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  const UINT plane_count = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
                               ? 1u
                               : std::max(1u, traits.planeCount);
  return mip_levels *
         (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
              ? 1
              : desc.DepthOrArraySize) *
         plane_count;
}

static UINT
GetTextureSubresourcesPerPlane(const D3D12_RESOURCE_DESC &desc) {
  return GetMipLevels(desc) *
         (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
              ? 1
              : desc.DepthOrArraySize);
}

static UINT
GetTextureSubresourcePlane(const D3D12_RESOURCE_DESC &desc,
                           UINT sub_resource) {
  const UINT subresources_per_plane = GetTextureSubresourcesPerPlane(desc);
  if (!subresources_per_plane)
    return 0;
  return sub_resource / subresources_per_plane;
}

static UINT
GetTextureSubresourcePlaneLocalIndex(const D3D12_RESOURCE_DESC &desc,
                                     UINT sub_resource) {
  const UINT subresources_per_plane = GetTextureSubresourcesPerPlane(desc);
  if (!subresources_per_plane)
    return sub_resource;
  return sub_resource % subresources_per_plane;
}

static UINT
GetTextureSubresourceMipLevel(const D3D12_RESOURCE_DESC &desc,
                              UINT sub_resource) {
  return GetTextureSubresourcePlaneLocalIndex(desc, sub_resource) %
         GetMipLevels(desc);
}

static UINT
GetTextureSubresourceArraySlice(const D3D12_RESOURCE_DESC &desc,
                                UINT sub_resource) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 0;
  return GetTextureSubresourcePlaneLocalIndex(desc, sub_resource) /
         GetMipLevels(desc);
}

static bool
IsCpuLinearTextureSubresource(const D3D12_RESOURCE_DESC &desc,
                              UINT sub_resource) {
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         sub_resource == 0 && GetMipLevels(desc) == 1 &&
         desc.DepthOrArraySize == 1 && desc.SampleDesc.Count == 1;
}

static bool
IsTextureSubresourceMappable(const D3D12_RESOURCE_DESC &desc,
                             UINT sub_resource,
                             const D3D12_HEAP_PROPERTIES &heap_properties) {
  const auto heap_type = GetHeapType(heap_properties);

  if (heap_type == D3D12_HEAP_TYPE_DEFAULT)
    return false;
  if (desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR)
    return false;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D &&
      GetMipLevels(desc) > 1)
    return false;
  return sub_resource < GetTextureSubresourceCount(desc);
}

static HRESULT
GetTextureSubresourceLayout(WMT::Device device,
                            const D3D12_RESOURCE_DESC &desc,
                            UINT sub_resource,
                            TextureSubresourceLayout &layout) {
  if (sub_resource >= GetTextureSubresourceCount(desc))
    return WARN_E_INVALIDARG(__func__);

  MTL_DXGI_FORMAT_DESC format = {};
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  const UINT plane = GetTextureSubresourcePlane(desc, sub_resource);
  if (plane >= std::max(1u, traits.planeCount))
    return WARN_E_INVALIDARG(__func__);
  if (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) {
    const auto plane_format =
        static_cast<DXGI_FORMAT>(traits.planes[plane].footprintFormat);
    if (FAILED(MTLQueryDXGIFormat(device, plane_format, format)))
      return WARN_E_INVALIDARG(__func__);
  } else if (FAILED(MTLQueryDXGIFormat(device, desc.Format, format))) {
    return WARN_E_INVALIDARG(__func__);
  }

  const UINT mip = GetTextureSubresourceMipLevel(desc, sub_resource);
  const UINT x_subsample =
      plane < traits.planeCount ? traits.planes[plane].subsampleXLog2 : 0;
  const UINT y_subsample =
      plane < traits.planeCount ? traits.planes[plane].subsampleYLog2 : 0;
  layout.width = std::max<UINT64>(1, desc.Width >> (mip + x_subsample));
  layout.height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                      ? 1
                      : std::max<UINT>(1, desc.Height >> (mip + y_subsample));
  layout.depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                     ? std::max<UINT>(1, desc.DepthOrArraySize >> mip)
                     : 1;
  layout.block_width = (format.Flag & MTL_DXGI_FORMAT_BC) ? 4 : 1;
  layout.block_height = (format.Flag & MTL_DXGI_FORMAT_BC) ? 4 : 1;
  layout.row_count = std::max<UINT>(
      1, (layout.height + layout.block_height - 1) / layout.block_height);
  layout.element_size =
      plane < traits.planeCount && traits.planes[plane].elementSize
          ? traits.planes[plane].elementSize
          : ((format.Flag & MTL_DXGI_FORMAT_BC) ? format.BlockSize
                                                 : format.BytesPerTexel);
  if (!layout.element_size)
    return WARN_E_INVALIDARG(__func__);

  const UINT64 block_columns =
      (layout.width + layout.block_width - 1) / layout.block_width;
  layout.row_size = block_columns * layout.element_size;
  layout.row_pitch = static_cast<UINT>(
      Align(layout.row_size, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
  layout.slice_pitch = layout.row_pitch * layout.row_count;
  return S_OK;
}

static bool
NormalizeTextureBox(const D3D12_RESOURCE_DESC &desc,
                    const TextureSubresourceLayout &layout,
                    const D3D12_BOX *box, D3D12_BOX &normalized) {
  if (box) {
    normalized = *box;
  } else {
    normalized.left = 0;
    normalized.top = 0;
    normalized.front = 0;
    normalized.right = static_cast<UINT>(layout.width);
    normalized.bottom = layout.height;
    normalized.back = layout.depth;
  }

  if (normalized.left >= normalized.right || normalized.top >= normalized.bottom ||
      normalized.front >= normalized.back)
    return false;
  if (normalized.right > layout.width || normalized.bottom > layout.height ||
      normalized.back > layout.depth)
    return false;
  if ((normalized.left % layout.block_width) ||
      (normalized.top % layout.block_height))
    return false;
  if (normalized.right != layout.width &&
      (normalized.right % layout.block_width))
    return false;
  if (normalized.bottom != layout.height &&
      (normalized.bottom % layout.block_height))
    return false;
  return true;
}

static UINT64
TextureRowOffset(const D3D12_BOX &box,
                 const TextureSubresourceLayout &layout, UINT row,
                 UINT depth_slice) {
  const UINT block_x = box.left / layout.block_width;
  const UINT block_y = (box.top / layout.block_height) + row;
  return static_cast<UINT64>(box.front + depth_slice) * layout.slice_pitch +
         static_cast<UINT64>(block_y) * layout.row_pitch +
         static_cast<UINT64>(block_x) * layout.element_size;
}

static UINT
TextureBoxRowCount(const D3D12_BOX &box,
                   const TextureSubresourceLayout &layout) {
  return std::max<UINT>(
      1, (box.bottom - box.top + layout.block_height - 1) /
             layout.block_height);
}

static UINT64
TextureBoxRowSize(const D3D12_BOX &box,
                  const TextureSubresourceLayout &layout) {
  const UINT64 block_columns =
      (box.right - box.left + layout.block_width - 1) / layout.block_width;
  return block_columns * layout.element_size;
}

static UINT
TextureBoxDepthCount(const D3D12_BOX &box) {
  return box.back - box.front;
}

static UINT
EffectiveSlicePitch(UINT slice_pitch, UINT row_pitch, UINT row_count) {
  return slice_pitch ? slice_pitch : row_pitch * row_count;
}

static HRESULT
ValidateTextureCopyPitches(UINT64 row_size, UINT row_count, UINT row_pitch,
                           UINT slice_pitch) {
  if (row_pitch < row_size)
    return WARN_E_INVALIDARG(__func__);
  if (slice_pitch && row_count > 1 &&
      slice_pitch < row_pitch * (row_count - 1) + row_size)
    return WARN_E_INVALIDARG(__func__);
  if (row_count == 1 && slice_pitch && slice_pitch < row_size)
    return WARN_E_INVALIDARG(__func__);
  return S_OK;
}

static HRESULT
ValidateTextureCopyPitches(UINT64 row_size, UINT row_count, UINT depth_count,
                           UINT row_pitch, UINT slice_pitch) {
  if (FAILED(ValidateTextureCopyPitches(row_size, row_count, row_pitch,
                                        slice_pitch)))
    return WARN_E_INVALIDARG(__func__);
  if (depth_count > 1 && slice_pitch < row_pitch * (row_count - 1) + row_size)
    return WARN_E_INVALIDARG(__func__);
  return S_OK;
}

static HRESULT
CopyTextureRowsToMemory(void *dst_data, UINT dst_row_pitch,
                        UINT dst_slice_pitch, const void *src_data,
                        UINT src_row_pitch, UINT src_slice_pitch,
                        UINT64 row_size, UINT row_count,
                        UINT depth_count = 1) {
  if (!dst_data || !src_data)
    return E_POINTER;
  if (FAILED(ValidateTextureCopyPitches(row_size, row_count, depth_count,
                                        src_row_pitch, src_slice_pitch)) ||
      FAILED(ValidateTextureCopyPitches(row_size, row_count, depth_count,
                                        dst_row_pitch, dst_slice_pitch)))
    return WARN_E_INVALIDARG(__func__);

  auto *dst = static_cast<char *>(dst_data);
  const auto *src = static_cast<const char *>(src_data);
  const UINT dst_effective_slice =
      EffectiveSlicePitch(dst_slice_pitch, dst_row_pitch, row_count);
  const UINT src_effective_slice =
      EffectiveSlicePitch(src_slice_pitch, src_row_pitch, row_count);
  for (UINT z = 0; z < depth_count; z++) {
    for (UINT row = 0; row < row_count; row++)
      std::memcpy(dst + z * dst_effective_slice + row * dst_row_pitch,
                  src + z * src_effective_slice + row * src_row_pitch,
                  static_cast<size_t>(row_size));
  }
  return S_OK;
}

class ResourceImpl final : public ComObjectWithInitialRef<ID3D12Resource2>,
                           public Resource {
public:
  ResourceImpl(IMTLD3D12Device *device,
               const D3D12_HEAP_PROPERTIES &heap_properties,
               D3D12_HEAP_FLAGS heap_flags,
               const D3D12_RESOURCE_DESC &desc,
               D3D12_RESOURCE_STATES initial_state,
               UINT64 heap_offset,
               const D3D12_CLEAR_VALUE *optimized_clear_value,
               ResourceKind kind,
               dxmt::Buffer *placed_buffer = nullptr,
               dxmt::BufferAllocation *placed_buffer_allocation = nullptr)
      : device_(device), heap_properties_(heap_properties),
        heap_flags_(heap_flags), desc_(desc), initial_state_(initial_state),
        heap_offset_(heap_offset), kind_(kind),
        placed_buffer_(placed_buffer),
        placed_buffer_allocation_(placed_buffer_allocation),
        has_clear_value_(optimized_clear_value != nullptr) {
    if (optimized_clear_value)
      clear_value_ = *optimized_clear_value;
    if (!desc_.Alignment)
      desc_.Alignment = GetDefaultResourceAlignment(desc_);
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && !desc_.MipLevels)
      desc_.MipLevels = GetMaxMipLevels(desc_);

    if (kind_ == ResourceKind::ReservedTexture &&
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      WARN("D3D12Resource: TODO CreateReservedResource(buffer) unsupported"
           " width=", desc_.Width,
           " flags=", desc_.Flags,
           " layout=", desc_.Layout);
    } else if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      CreateBuffer();
    } else {
      CreateTexture();
    }
  }

  ~ResourceImpl() {
    UnregisterBufferGpuVirtualAddress(this);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource) ||
        riid == __uuidof(ID3D12Resource1) ||
        riid == __uuidof(ID3D12Resource2)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12Resource), riid))
      WARN("D3D12Resource: unknown interface query ", str::format(riid));
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
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE GetProtectedResourceSession(
      REFIID riid, void **protected_session) override {
    InitReturnPtr(protected_session);
    if (!protected_session)
      return E_POINTER;

    return DXGI_ERROR_NOT_FOUND;
  }

  D3D12_RESOURCE_DESC1 MakeDesc1() const {
    D3D12_RESOURCE_DESC1 desc = {};
    desc.Dimension = desc_.Dimension;
    desc.Alignment = desc_.Alignment;
    desc.Width = desc_.Width;
    desc.Height = desc_.Height;
    desc.DepthOrArraySize = desc_.DepthOrArraySize;
    desc.MipLevels = desc_.MipLevels;
    desc.Format = desc_.Format;
    desc.SampleDesc = desc_.SampleDesc;
    desc.Layout = desc_.Layout;
    desc.Flags = desc_.Flags;
    return desc;
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_DESC1 *STDMETHODCALLTYPE
  GetDesc1(D3D12_RESOURCE_DESC1 *__ret) override {
    *__ret = MakeDesc1();
    return __ret;
  }
#else
  D3D12_RESOURCE_DESC1 STDMETHODCALLTYPE GetDesc1() override {
    return MakeDesc1();
  }
#endif

  HRESULT STDMETHODCALLTYPE Map(UINT sub_resource, const D3D12_RANGE *read_range,
                                void **data) override {
    HRESULT hr = S_OK;
    void *mapped = nullptr;
    if (sub_resource >= GetTextureSubresourceCount(desc_)) {
      hr = E_INVALIDARG;
      WARN(__func__, ": invalid subresource");
      goto done;
    }

    if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      if (sub_resource != 0 || !buffer_allocation_ ||
          !buffer_allocation_->mappedMemory(0)) {
        hr = E_INVALIDARG;
        WARN(__func__, ": buffer is not mappable");
        goto done;
      }

      mapped = static_cast<char *>(buffer_allocation_->mappedMemory(0)) +
               heap_offset_;
      goto done;
    }

    if (!texture_allocation_ ||
        !IsTextureSubresourceMappable(desc_, sub_resource, heap_properties_)) {
      hr = E_INVALIDARG;
      WARN(__func__, ": texture subresource is not mappable");
      goto done;
    }

    if (!data)
      goto done;

    if (!texture_allocation_->mappedMemory ||
        !IsCpuLinearTextureSubresource(desc_, sub_resource)) {
      hr = E_INVALIDARG;
      WARN(__func__, ": texture allocation has no CPU-linear mapped memory");
      goto done;
    }

    mapped = texture_allocation_->mappedMemory;

  done:
    if (data)
      *data = SUCCEEDED(hr) ? mapped : nullptr;
    dxmt::apitrace::record_resource_map(
        this, sub_resource, read_range, SUCCEEDED(hr) && mapped != nullptr, hr);
    return hr;
  }

  void STDMETHODCALLTYPE Unmap(UINT sub_resource,
                               const D3D12_RANGE *written_range) override {
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        sub_resource != 0 || !buffer_allocation_)
      return;

    auto *mapped_memory = static_cast<char *>(buffer_allocation_->mappedMemory(0));
    if (mapped_memory && dxmt::apitrace::d3d_enabled()) {
      UINT64 written_begin = 0;
      UINT64 written_end = desc_.Width;
      if (written_range) {
        written_begin = std::min<UINT64>(written_range->Begin, desc_.Width);
        written_end = std::min<UINT64>(written_range->End, desc_.Width);
      }
      if (written_end > written_begin) {
        dxmt::apitrace::record_resource_unmap(
            this, sub_resource, written_begin, written_end,
            mapped_memory + heap_offset_ + written_begin,
            static_cast<size_t>(written_end - written_begin));
      }
    }

    if (written_range && written_range->End > written_range->Begin)
      buffer_allocation_->flushCpuShadow(written_range->Begin,
                                         written_range->End - written_range->Begin);
  }

#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  D3D12_RESOURCE_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_RESOURCE_DESC *__ret) override {
    *__ret = desc_;
    return __ret;
  }
#else
  D3D12_RESOURCE_DESC STDMETHODCALLTYPE GetDesc() override {
    return desc_;
  }
#endif

  D3D12_GPU_VIRTUAL_ADDRESS STDMETHODCALLTYPE
  GetGPUVirtualAddress() override {
    return GetGpuVirtualAddress();
  }

  D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const override {
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        !buffer_allocation_)
      return 0;
    return buffer_allocation_->gpuAddress() + heap_offset_;
  }

  HRESULT STDMETHODCALLTYPE WriteToSubresource(
      UINT dst_sub_resource, const D3D12_BOX *dst_box, const void *src_data,
      UINT src_row_pitch, UINT src_slice_pitch) override {
    if (!src_data)
      return E_POINTER;

    if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return WARN_E_INVALIDARG(__func__);

    return WriteTextureSubresource(dst_sub_resource, dst_box, src_data,
                                   src_row_pitch, src_slice_pitch);
  }

  HRESULT STDMETHODCALLTYPE ReadFromSubresource(
      void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
      UINT src_sub_resource, const D3D12_BOX *src_box) override {
    if (!dst_data)
      return E_POINTER;

    if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return WARN_E_INVALIDARG(__func__);

    return ReadTextureSubresource(dst_data, dst_row_pitch, dst_slice_pitch,
                                  src_sub_resource, src_box);
  }

  HRESULT STDMETHODCALLTYPE
  GetHeapProperties(D3D12_HEAP_PROPERTIES *heap_properties,
                    D3D12_HEAP_FLAGS *flags) override {
    if (!heap_properties || !flags)
      return E_POINTER;
    *heap_properties = heap_properties_;
    *flags = heap_flags_;
    return S_OK;
  }

  ResourceKind GetKind() const override {
    return kind_;
  }

  bool IsReservedTexture() const override {
    return kind_ == ResourceKind::ReservedTexture;
  }

  const ResourceTiling *GetTiling() const override {
    return has_tiling_ ? &tiling_ : nullptr;
  }

  bool UpdateTileMapping(UINT subresource, UINT x, UINT y, UINT z,
                         ID3D12Heap *heap, bool mapped,
                         UINT64 heap_tile) override {
    if (!has_tiling_ || subresource >= tiling_.subresources.size())
      return false;
    const auto &tile = tiling_.subresources[subresource];
    if (x >= tile.width_in_tiles || y >= tile.height_in_tiles ||
        z >= tile.depth_in_tiles)
      return false;
    const UINT index = tile.start_tile_index +
                       (z * tile.height_in_tiles + y) *
                           tile.width_in_tiles +
                       x;
    if (index >= tile_map_.size())
      return false;
    if (mapped) {
      tile_map_[index].heap = heap;
      tile_map_[index].heap_tile = static_cast<int64_t>(heap_tile);
    } else {
      tile_map_[index] = {};
      tile_map_[index].heap_tile = -1;
    }
    return true;
  }

  bool GetTileMapping(UINT subresource, UINT x, UINT y, UINT z,
                      ResourceTileMapping &mapping) const override {
    mapping = {};
    mapping.heap_tile = -1;
    if (!has_tiling_ || subresource >= tiling_.subresources.size())
      return false;
    const auto &tile = tiling_.subresources[subresource];
    if (x >= tile.width_in_tiles || y >= tile.height_in_tiles ||
        z >= tile.depth_in_tiles)
      return false;
    const UINT index = tile.start_tile_index +
                       (z * tile.height_in_tiles + y) *
                           tile.width_in_tiles +
                       x;
    if (index >= tile_map_.size())
      return false;
    mapping = tile_map_[index];
    return true;
  }

  const D3D12_RESOURCE_DESC &GetResourceDesc() const override {
    return desc_;
  }

  const D3D12_HEAP_PROPERTIES &GetResourceHeapProperties() const override {
    return heap_properties_;
  }

  D3D12_HEAP_FLAGS GetResourceHeapFlags() const override {
    return heap_flags_;
  }

  UINT64 GetHeapOffset() const override {
    return heap_offset_;
  }

  D3D12_RESOURCE_STATES GetInitialState() const override {
    return initial_state_;
  }

  dxmt::Buffer *GetBuffer() const override {
    return buffer_.ptr();
  }

  dxmt::BufferAllocation *GetBufferAllocation() const override {
    return buffer_allocation_.ptr();
  }

  dxmt::Texture *GetTexture() const override {
    return texture_.ptr();
  }

  dxmt::Texture *GetTexture(UINT plane) const override {
    if (plane >= GetTextureLogicalPlaneCount(desc_))
      return nullptr;
    const auto backing_plane = GetTextureBackingPlaneIndex(desc_, plane);
    return backing_plane < plane_textures_.size()
               ? plane_textures_[backing_plane].ptr()
               : nullptr;
  }

  dxmt::TextureAllocation *GetTextureAllocation() const override {
    return texture_allocation_.ptr();
  }

  dxmt::TextureAllocation *GetTextureAllocation(UINT plane) const override {
    if (plane >= GetTextureLogicalPlaneCount(desc_))
      return nullptr;
    const auto backing_plane = GetTextureBackingPlaneIndex(desc_, plane);
    return backing_plane < plane_allocations_.size()
               ? plane_allocations_[backing_plane].ptr()
               : nullptr;
  }

  void SetPresentSourceView(dxmt::TextureViewKey view) override {
    present_source_view_ = view;
  }

  dxmt::TextureViewKey GetPresentSourceView() const override {
    return present_source_view_;
  }

  ID3D12Resource *GetD3D12Resource() override {
    return static_cast<ID3D12Resource *>(this);
  }

private:
  Rc<dxmt::Texture> GetTextureRef(UINT plane) const {
    if (plane >= GetTextureLogicalPlaneCount(desc_))
      return nullptr;
    const auto backing_plane = GetTextureBackingPlaneIndex(desc_, plane);
    return backing_plane < plane_textures_.size() ? plane_textures_[backing_plane]
                                                  : nullptr;
  }

  Rc<dxmt::TextureAllocation> GetTextureAllocationRef(UINT plane) const {
    if (plane >= GetTextureLogicalPlaneCount(desc_))
      return nullptr;
    const auto backing_plane = GetTextureBackingPlaneIndex(desc_, plane);
    return backing_plane < plane_allocations_.size()
               ? plane_allocations_[backing_plane]
               : nullptr;
  }

  HRESULT WriteTextureSubresource(UINT dst_sub_resource,
                                  const D3D12_BOX *dst_box,
                                  const void *src_data, UINT src_row_pitch,
                                  UINT src_slice_pitch) {
    const UINT plane = GetTextureSubresourcePlane(desc_, dst_sub_resource);
    auto texture = GetTextureRef(plane);
    auto texture_allocation = GetTextureAllocationRef(plane);
    if (!texture || !texture_allocation)
      return WARN_E_INVALIDARG(__func__);

    TextureSubresourceLayout layout = {};
    HRESULT hr = GetTextureSubresourceLayout(device_->GetDXMTDevice().device(),
                                             desc_, dst_sub_resource, layout);
    if (FAILED(hr))
      return hr;

    D3D12_BOX box = {};
    if (!NormalizeTextureBox(desc_, layout, dst_box, box))
      return WARN_E_INVALIDARG(__func__);

    const UINT row_count = TextureBoxRowCount(box, layout);
    const UINT depth_count = TextureBoxDepthCount(box);
    const UINT64 row_size = TextureBoxRowSize(box, layout);
    if (FAILED(ValidateTextureCopyPitches(row_size, row_count, depth_count,
                                          src_row_pitch, src_slice_pitch)))
      return WARN_E_INVALIDARG(__func__);

    if (texture_allocation->mappedMemory &&
        IsCpuLinearTextureSubresource(desc_, dst_sub_resource))
      return WriteMappedTextureRows(box, layout, src_data, src_row_pitch,
                                    src_slice_pitch, row_size, row_count,
                                    depth_count, texture_allocation.ptr());

    return WriteTextureRowsViaBlit(box, layout, src_data, src_row_pitch,
                                   src_slice_pitch, row_size, row_count,
                                   depth_count, dst_sub_resource,
                                   std::move(texture));
  }

  HRESULT ReadTextureSubresource(void *dst_data, UINT dst_row_pitch,
                                 UINT dst_slice_pitch, UINT src_sub_resource,
                                 const D3D12_BOX *src_box) {
    const UINT plane = GetTextureSubresourcePlane(desc_, src_sub_resource);
    auto texture = GetTextureRef(plane);
    auto texture_allocation = GetTextureAllocationRef(plane);
    if (!texture || !texture_allocation)
      return WARN_E_INVALIDARG(__func__);

    TextureSubresourceLayout layout = {};
    HRESULT hr = GetTextureSubresourceLayout(device_->GetDXMTDevice().device(),
                                             desc_, src_sub_resource, layout);
    if (FAILED(hr))
      return hr;

    D3D12_BOX box = {};
    if (!NormalizeTextureBox(desc_, layout, src_box, box))
      return WARN_E_INVALIDARG(__func__);

    const UINT row_count = TextureBoxRowCount(box, layout);
    const UINT depth_count = TextureBoxDepthCount(box);
    const UINT64 row_size = TextureBoxRowSize(box, layout);
    if (FAILED(ValidateTextureCopyPitches(row_size, row_count, depth_count,
                                          dst_row_pitch, dst_slice_pitch)))
      return WARN_E_INVALIDARG(__func__);

    if (texture_allocation->mappedMemory &&
        IsCpuLinearTextureSubresource(desc_, src_sub_resource))
      return ReadMappedTextureRows(dst_data, dst_row_pitch, box, layout,
                                   dst_slice_pitch, row_size, row_count,
                                   depth_count, texture_allocation.ptr());

    return ReadTextureRowsViaBlit(dst_data, dst_row_pitch, box, layout,
                                  dst_slice_pitch, row_size, row_count,
                                  depth_count, src_sub_resource,
                                  std::move(texture));
  }

  HRESULT WriteMappedTextureRows(const D3D12_BOX &box,
                                 const TextureSubresourceLayout &layout,
                                 const void *src_data, UINT src_row_pitch,
                                 UINT src_slice_pitch, UINT64 row_size,
                                 UINT row_count, UINT depth_count,
                                 dxmt::TextureAllocation *texture_allocation) {
    auto *dst = static_cast<char *>(texture_allocation->mappedMemory);
    const auto *src = static_cast<const char *>(src_data);
    const UINT src_effective_slice =
        EffectiveSlicePitch(src_slice_pitch, src_row_pitch, row_count);
    for (UINT z = 0; z < depth_count; z++) {
      for (UINT row = 0; row < row_count; row++)
        std::memcpy(dst + TextureRowOffset(box, layout, row, z),
                    src + z * src_effective_slice + row * src_row_pitch,
                    static_cast<size_t>(row_size));
    }
    return S_OK;
  }

  HRESULT ReadMappedTextureRows(void *dst_data, UINT dst_row_pitch,
                                const D3D12_BOX &box,
                                const TextureSubresourceLayout &layout,
                                UINT dst_slice_pitch, UINT64 row_size,
                                UINT row_count, UINT depth_count,
                                dxmt::TextureAllocation *texture_allocation) {
    auto *dst = static_cast<char *>(dst_data);
    const auto *src = static_cast<const char *>(texture_allocation->mappedMemory);
    const UINT dst_effective_slice =
        EffectiveSlicePitch(dst_slice_pitch, dst_row_pitch, row_count);
    for (UINT z = 0; z < depth_count; z++) {
      for (UINT row = 0; row < row_count; row++)
        std::memcpy(dst + z * dst_effective_slice + row * dst_row_pitch,
                    src + TextureRowOffset(box, layout, row, z),
                    static_cast<size_t>(row_size));
    }
    return S_OK;
  }

  HRESULT WriteTextureRowsViaBlit(const D3D12_BOX &box,
                                  const TextureSubresourceLayout &layout,
                                  const void *src_data, UINT src_row_pitch,
                                  UINT src_slice_pitch, UINT64 row_size,
                                  UINT row_count, UINT depth_count,
                                  UINT dst_sub_resource,
                                  Rc<dxmt::Texture> texture) {
    const UINT staging_slice_pitch = layout.row_pitch * row_count;
    const UINT dst_slice = GetTextureSubresourceArraySlice(desc_, dst_sub_resource);
    const UINT dst_level = GetTextureSubresourceMipLevel(desc_, dst_sub_resource);
    const UINT dst_plane = GetTextureSubresourcePlane(desc_, dst_sub_resource);
    const UINT origin_z =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? box.front : 0;
    const UINT bytes_per_image =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? staging_slice_pitch
            : 0;

    if (IsDepthStencilResourceFormat(desc_.Format) &&
        DepthStencilPlanarFlags(texture->pixelFormat()) > 1) {
      if (depth_count != 1 || dst_plane > 1)
        return WARN_E_INVALIDARG(__func__);

      Flags<dxmt::BufferAllocationFlag> flags;
      flags.set(dxmt::BufferAllocationFlag::CpuWriteCombined);
      Rc<dxmt::Buffer> buffer =
          new dxmt::Buffer(staging_slice_pitch,
                           device_->GetDXMTDevice().device());
      auto allocation = buffer->allocate(flags);
      buffer->rename(Rc<dxmt::BufferAllocation>(allocation));
      auto *mapped = static_cast<char *>(allocation->mappedMemory(0));
      if (!mapped)
        return E_FAIL;

      std::memset(mapped, 0, static_cast<size_t>(staging_slice_pitch));
      const auto *src = static_cast<const char *>(src_data);
      for (UINT row = 0; row < row_count; row++)
        std::memcpy(mapped + row * layout.row_pitch,
                    src + row * src_row_pitch,
                    static_cast<size_t>(row_size));

      const WMTOrigin origin = {box.left, box.top, origin_z};
      const WMTSize size = {box.right - box.left, box.bottom - box.top, 1};
      return SubmitSynchronousDxmtBlit(
          [buffer = std::move(buffer), texture = std::move(texture),
           row_pitch = layout.row_pitch, bytes_per_image, dst_slice, dst_level,
           dst_plane, origin, size](ArgumentEncodingContext &enc) mutable {
            enc.blit_depth_stencil_cmd.copyPlaneFromBuffer(
                buffer, 0, buffer->length(), row_pitch, bytes_per_image,
                texture, dst_level, dst_slice, dst_plane == 1, origin, size);
          });
    }

    WMTBufferInfo buffer_info = {};
    buffer_info.length = staging_slice_pitch * depth_count;
    buffer_info.options = WMTResourceHazardTrackingModeUntracked |
                          WMTResourceOptionCPUCacheModeWriteCombined;
    auto buffer = device_->GetDXMTDevice().device().newBuffer(buffer_info);
    if (!buffer || !buffer_info.memory.get())
      return E_FAIL;

    auto *mapped = static_cast<char *>(buffer_info.memory.get());
    std::memset(mapped, 0, static_cast<size_t>(buffer_info.length));
    const auto *src = static_cast<const char *>(src_data);
    const UINT src_effective_slice =
        EffectiveSlicePitch(src_slice_pitch, src_row_pitch, row_count);
    for (UINT z = 0; z < depth_count; z++) {
      for (UINT row = 0; row < row_count; row++)
        std::memcpy(mapped + z * staging_slice_pitch + row * layout.row_pitch,
                    src + z * src_effective_slice + row * src_row_pitch,
                    static_cast<size_t>(row_size));
    }

    return SubmitSynchronousDxmtBlit(
        [buffer, texture, box, row_pitch = layout.row_pitch,
         bytes_per_image, dst_slice, dst_level, origin_z, depth_count](
            ArgumentEncodingContext &enc) {
          enc.startBlitPass();
          auto dst = enc.access(texture, dst_level, dst_slice,
                                ResourceAccess::Write);
          auto &copy =
              enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
          copy.type = WMTBlitCommandCopyFromBufferToTexture;
          copy.src = buffer;
          copy.src_offset = 0;
          copy.bytes_per_row = row_pitch;
          copy.bytes_per_image = bytes_per_image;
          copy.size = {box.right - box.left, box.bottom - box.top,
                       depth_count};
          copy.dst = dst;
          copy.slice = dst_slice;
          copy.level = dst_level;
          copy.origin = {box.left, box.top, origin_z};
          enc.endPass();
        });
  }

  HRESULT ReadTextureRowsViaBlit(void *dst_data, UINT dst_row_pitch,
                                 const D3D12_BOX &box,
                                 const TextureSubresourceLayout &layout,
                                 UINT dst_slice_pitch, UINT64 row_size,
                                 UINT row_count, UINT depth_count,
                                 UINT src_sub_resource,
                                 Rc<dxmt::Texture> texture) {
    const UINT staging_slice_pitch = layout.row_pitch * row_count;
    const UINT src_slice = GetTextureSubresourceArraySlice(desc_, src_sub_resource);
    const UINT src_level = GetTextureSubresourceMipLevel(desc_, src_sub_resource);
    const UINT src_plane = GetTextureSubresourcePlane(desc_, src_sub_resource);
    const UINT origin_z =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? box.front : 0;
    const UINT bytes_per_image =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? staging_slice_pitch
            : 0;

    if (IsDepthStencilResourceFormat(desc_.Format) &&
        DepthStencilPlanarFlags(texture->pixelFormat()) > 1) {
      if (depth_count != 1 || src_plane > 1)
        return WARN_E_INVALIDARG(__func__);

      Rc<dxmt::Buffer> buffer =
          new dxmt::Buffer(staging_slice_pitch,
                           device_->GetDXMTDevice().device());
      Flags<dxmt::BufferAllocationFlag> flags;
      auto allocation = buffer->allocate(flags);
      buffer->rename(Rc<dxmt::BufferAllocation>(allocation));
      if (!allocation->mappedMemory(0))
        return E_FAIL;

      const auto view =
          CreateDepthStencilPlaneReadView(texture.ptr(), src_plane, src_level,
                                          src_slice);
      if (!view)
        return WARN_E_INVALIDARG(__func__);

      const WMTOrigin origin = {box.left, box.top, origin_z};
      const WMTSize size = {box.right - box.left, box.bottom - box.top, 1};
      HRESULT hr = SubmitSynchronousDxmtBlit(
          [buffer, texture, view, row_pitch = layout.row_pitch,
           bytes_per_image, src_plane, origin, size](
              ArgumentEncodingContext &enc) mutable {
            enc.blit_depth_stencil_cmd.copyPlaneToBuffer(
                texture, view, buffer, 0, buffer->length(), row_pitch,
                bytes_per_image, src_plane == 1, origin, size);
          });
      if (FAILED(hr))
        return hr;

      return CopyTextureRowsToMemory(dst_data, dst_row_pitch, dst_slice_pitch,
                                     allocation->mappedMemory(0),
                                     layout.row_pitch, staging_slice_pitch,
                                     row_size, row_count, depth_count);
    }

    WMTBufferInfo buffer_info = {};
    buffer_info.length = staging_slice_pitch * depth_count;
    buffer_info.options = WMTResourceHazardTrackingModeUntracked;
    auto buffer = device_->GetDXMTDevice().device().newBuffer(buffer_info);
    if (!buffer || !buffer_info.memory.get())
      return E_FAIL;

    HRESULT hr = SubmitSynchronousDxmtBlit(
        [buffer, texture, box, row_pitch = layout.row_pitch, bytes_per_image,
         src_slice, src_level, origin_z, depth_count](
            ArgumentEncodingContext &enc) {
          enc.startBlitPass();
          auto src = enc.access(texture, src_level, src_slice,
                                ResourceAccess::Read);
          auto &copy =
              enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
          copy.type = WMTBlitCommandCopyFromTextureToBuffer;
          copy.src = src;
          copy.slice = src_slice;
          copy.level = src_level;
          copy.origin = {box.left, box.top, origin_z};
          copy.size = {box.right - box.left, box.bottom - box.top,
                       depth_count};
          copy.dst = buffer;
          copy.offset = 0;
          copy.bytes_per_row = row_pitch;
          copy.bytes_per_image = bytes_per_image;
          enc.endPass();
        });
    if (FAILED(hr))
      return hr;

    return CopyTextureRowsToMemory(dst_data, dst_row_pitch, dst_slice_pitch,
                                   buffer_info.memory.get(), layout.row_pitch,
                                   staging_slice_pitch, row_size, row_count,
                                   depth_count);
  }

  template <typename Encode>
  HRESULT SubmitSynchronousDxmtBlit(Encode &&encode) {
    auto &queue = device_->GetDXMTDevice().queue();
    const auto seq = queue.CurrentSeqId();
    auto *chunk = queue.CurrentChunk();
    chunk->emitcc(std::forward<Encode>(encode));
    queue.CommitCurrentChunk();
    queue.WaitCPUFence(seq);
    return S_OK;
  }

  void CreateBuffer() {
    if (placed_buffer_allocation_) {
      buffer_ = placed_buffer_;
      buffer_allocation_ = placed_buffer_allocation_;
      if (!buffer_) {
        WARN("D3D12Resource: placed buffer resource is missing heap backing"
             " width=", desc_.Width,
             " heapOffset=", heap_offset_,
             " heapFlags=", heap_flags_);
      }
    } else {
      buffer_ = new dxmt::Buffer(desc_.Width + heap_offset_,
                                 device_->GetDXMTDevice().device());
      buffer_allocation_ =
          buffer_->allocate(GetHeapBufferAllocationFlags(heap_properties_));
      buffer_->rename(Rc<dxmt::BufferAllocation>(buffer_allocation_));
    }
    RegisterBufferGpuVirtualAddress(this, GetGpuVirtualAddress(), desc_.Width);
  }

  void CreateTexture() {
    Flags<dxmt::TextureAllocationFlag> flags;
    if (GetHeapType(heap_properties_) == D3D12_HEAP_TYPE_DEFAULT) {
      flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
      flags.set(dxmt::TextureAllocationFlag::GpuPrivate);
    } else if (GetHeapType(heap_properties_) == D3D12_HEAP_TYPE_UPLOAD) {
      flags.set(dxmt::TextureAllocationFlag::CpuWriteCombined);
    }

    const UINT plane_count = GetTextureBackingPlaneCount(desc_);
    if (plane_count > plane_textures_.size()) {
      WARN("D3D12Resource: unsupported texture plane count ", plane_count);
      return;
    }
    if (kind_ == ResourceKind::ReservedTexture && plane_count != 1) {
      WARN("D3D12Resource: TODO reserved texture split-plane backing unsupported"
           " format=", desc_.Format,
           " planes=", plane_count,
           " width=", desc_.Width,
           " height=", desc_.Height);
      return;
    }
    if (kind_ == ResourceKind::ReservedTexture) {
      if (!device_->GetDXMTDevice().device().supportsPlacementSparse()) {
        WARN("D3D12Resource: TODO reserved texture requires Metal4 placement sparse"
             " format=", desc_.Format,
             " width=", desc_.Width,
             " height=", desc_.Height,
             " depthOrArray=", desc_.DepthOrArraySize,
             " flags=", desc_.Flags,
             " layout=", desc_.Layout);
        return;
      }
      if (desc_.SampleDesc.Count > 1) {
        WARN("D3D12Resource: TODO reserved MSAA texture unsupported"
             " format=", desc_.Format,
             " samples=", desc_.SampleDesc.Count,
             " width=", desc_.Width,
             " height=", desc_.Height,
             " flags=", desc_.Flags);
        return;
      }
      if (desc_.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN &&
          desc_.Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE &&
          desc_.Layout != D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE) {
        WARN("D3D12Resource: TODO reserved texture layout unsupported"
             " format=", desc_.Format,
             " width=", desc_.Width,
             " height=", desc_.Height,
             " layout=", desc_.Layout,
             " flags=", desc_.Flags);
        return;
      }
      flags.set(dxmt::TextureAllocationFlag::PlacementSparse);
    }

    const bool cpu_linear_candidate =
        GetTextureLogicalPlaneCount(desc_) == 1 &&
        GetHeapType(heap_properties_) != D3D12_HEAP_TYPE_DEFAULT &&
        IsCpuLinearTextureSubresource(desc_, 0);

    WMTPixelFormat first_pixel_format = WMTPixelFormatInvalid;
    for (UINT plane = 0; plane < plane_count; ++plane) {
      MTL_DXGI_FORMAT_DESC format = {};
      if (FAILED(MTLQueryDXGIFormat(device_->GetDXMTDevice().device(),
                                    ResolveTextureBackingFormat(desc_, plane),
                                    format)))
        return;

      WMTTextureInfo info = {};
      info.pixel_format = format.PixelFormat;
      info.width = static_cast<uint32_t>(GetTexturePlaneWidth(desc_, plane));
      info.height = GetTexturePlaneHeight(desc_, plane);
      info.depth = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                       ? desc_.DepthOrArraySize
                       : 1;
      info.array_length = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                              ? 1
                              : desc_.DepthOrArraySize;
      info.type = GetTextureType(desc_);
      info.mipmap_level_count =
          std::min(GetMipLevels(desc_), GetMaxMipLevels(desc_));
      info.sample_count = desc_.SampleDesc.Count ? desc_.SampleDesc.Count : 1;
      info.usage = GetTextureUsage(desc_.Flags);
      if (kind_ == ResourceKind::ReservedTexture) {
        WMTSparseTileSize tile_size = {};
        if (!device_->GetDXMTDevice().device().sparseTileSize(info,
                                                             tile_size)) {
          WARN("D3D12Resource: TODO unsupported sparse texture format"
               " format=", desc_.Format,
               " metalFormat=", info.pixel_format,
               " type=", info.type,
               " width=", desc_.Width,
               " height=", desc_.Height,
               " depthOrArray=", desc_.DepthOrArraySize,
               " flags=", desc_.Flags);
          return;
        }
        BuildTilingMetadata(tile_size);
      }

      TextureSubresourceLayout layout = {};
      const bool linear_cpu_texture =
          cpu_linear_candidate &&
          SUCCEEDED(GetTextureSubresourceLayout(device_->GetDXMTDevice().device(),
                                               desc_, 0, layout));

      if (linear_cpu_texture) {
        // TODO(d3d12): extend CPU-linear custom heap textures beyond the
        // single-subresource 2D case so ReadFromSubresource/WriteToSubresource
        // can cover the full D3D12 custom-heap texture surface.
        plane_textures_[plane] = new dxmt::Texture(
            layout.slice_pitch, layout.row_pitch, info,
            device_->GetDXMTDevice().device());
      } else {
        plane_textures_[plane] =
            new dxmt::Texture(info, device_->GetDXMTDevice().device());
      }

      plane_allocations_[plane] = plane_textures_[plane]->allocate(flags);
      plane_textures_[plane]->rename(
          Rc<dxmt::TextureAllocation>(plane_allocations_[plane]));
      if (!plane) {
        first_pixel_format = format.PixelFormat;
        texture_ = plane_textures_[plane];
        texture_allocation_ = plane_allocations_[plane];
      }
    }

    if (kind_ != ResourceKind::ReservedTexture &&
        GetHeapType(heap_properties_) == D3D12_HEAP_TYPE_DEFAULT) {
      InitializeTextureContents(first_pixel_format);
    }
  }

  void BuildTilingMetadata(const WMTSparseTileSize &tile_size) {
    tiling_ = {};
    const UINT tile_width = std::max<UINT>(1, static_cast<UINT>(tile_size.width));
    const UINT tile_height = std::max<UINT>(1, static_cast<UINT>(tile_size.height));
    const UINT tile_depth = std::max<UINT>(1, static_cast<UINT>(tile_size.depth));
    tiling_.tile_shape.WidthInTexels = tile_width;
    tiling_.tile_shape.HeightInTexels = tile_height;
    tiling_.tile_shape.DepthInTexels = tile_depth;

    const UINT mip_levels = GetMipLevels(desc_);
    const UINT plane_count = GetTextureLogicalPlaneCount(desc_);
    const UINT array_size =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? 1
            : desc_.DepthOrArraySize;
    UINT standard_mips = 0;
    for (; standard_mips < mip_levels; ++standard_mips) {
      const UINT64 width =
          std::max<UINT64>(1, GetTexturePlaneWidth(desc_, 0) >> standard_mips);
      const UINT height = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                              ? 1
                              : std::max<UINT>(
                                    1, GetTexturePlaneHeight(desc_, 0) >> standard_mips);
      const UINT depth = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                             ? std::max<UINT>(1, desc_.DepthOrArraySize >> standard_mips)
                             : 1;
      bool standard = width >= tile_width;
      if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D)
        standard &= height >= tile_height;
      if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
        standard &= depth >= tile_depth;
      if (!standard)
        break;
    }
    tiling_.packed_mip_info.NumStandardMips = standard_mips;
    tiling_.packed_mip_info.NumPackedMips = mip_levels - standard_mips;
    tiling_.packed_mip_info.NumTilesForPackedMips =
        tiling_.packed_mip_info.NumPackedMips ? 1 : 0;
    tiling_.packed_mip_info.StartTileIndexInOverallResource = 0;

    tiling_.subresources.reserve(mip_levels * array_size * plane_count);
    UINT next_tile = 0;
    bool packed_start_set = false;
    for (UINT plane = 0; plane < plane_count; ++plane) {
      for (UINT slice = 0; slice < array_size; ++slice) {
        for (UINT mip = 0; mip < mip_levels; ++mip) {
          const UINT64 width = std::max<UINT64>(1, GetTexturePlaneWidth(desc_, plane) >> mip);
          const UINT height = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                                  ? 1
                                  : std::max<UINT>(1, GetTexturePlaneHeight(desc_, plane) >> mip);
          const UINT depth = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                 ? std::max<UINT>(1, desc_.DepthOrArraySize >> mip)
                                 : 1;
          SubresourceTiling subresource = {};
          subresource.width_in_tiles =
              static_cast<UINT>((width + tile_size.width - 1) / tile_size.width);
          subresource.height_in_tiles =
              static_cast<UINT16>((height + tile_size.height - 1) / tile_size.height);
          subresource.depth_in_tiles =
              static_cast<UINT16>((depth + tile_size.depth - 1) / tile_size.depth);
          subresource.start_tile_index = next_tile;
          subresource.mip_level = mip;
          subresource.array_slice = slice;
          subresource.plane = plane;
          if (mip < standard_mips) {
            next_tile += subresource.width_in_tiles *
                         subresource.height_in_tiles *
                         subresource.depth_in_tiles;
          } else {
            if (mip == standard_mips) {
              if (!packed_start_set) {
                tiling_.packed_mip_info.StartTileIndexInOverallResource = next_tile;
                packed_start_set = true;
              }
              ++next_tile;
            }
            subresource.width_in_tiles = 0;
            subresource.height_in_tiles = 0;
            subresource.depth_in_tiles = 0;
            subresource.start_tile_index = D3D12_PACKED_TILE;
          }
          tiling_.subresources.push_back(subresource);
        }
      }
    }
    tiling_.total_tile_count = next_tile;
    tile_map_.assign(next_tile, {});
    for (auto &mapping : tile_map_)
      mapping.heap_tile = -1;
    has_tiling_ = true;
  }

  void InitializeTextureContents(WMTPixelFormat pixel_format) {
    auto &initializer = device_->GetDXMTDevice().queue().initializer;
    const UINT mip_levels = desc_.MipLevels ? desc_.MipLevels : 1;
    const UINT array_size =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? 1
            : desc_.DepthOrArraySize;
    const auto dsv_planar = DepthStencilPlanarFlags(pixel_format);
    const UINT plane_count = GetTextureBackingPlaneCount(desc_);

    for (UINT plane = 0; plane < plane_count; ++plane) {
      auto texture = plane_textures_[plane];
      auto allocation = plane_allocations_[plane];
      if (!texture || !allocation)
        continue;

      if (!plane && dsv_planar &&
          (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
        const float depth = has_clear_value_ ? clear_value_.DepthStencil.Depth : 0.0f;
        const uint8_t stencil = has_clear_value_ ? clear_value_.DepthStencil.Stencil : 0;
        for (UINT slice = 0; slice < array_size; ++slice) {
          for (UINT level = 0; level < mip_levels; ++level) {
            initializer.initDepthStencilWithZero(
                texture.ptr(), texture->current(), slice, level, dsv_planar,
                depth, stencil);
          }
        }
        continue;
      }

      if (!plane && (texture->usage() & WMTTextureUsageRenderTarget) &&
          (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) {
        WMTClearColor color = {0, 0, 0, 0};
        if (has_clear_value_) {
          color.r = clear_value_.Color[0];
          color.g = clear_value_.Color[1];
          color.b = clear_value_.Color[2];
          color.a = clear_value_.Color[3];
        }
        for (UINT slice = 0; slice < array_size; ++slice) {
          for (UINT level = 0; level < mip_levels; ++level) {
            initializer.initRenderTargetWithZero(
                texture.ptr(), texture->current(), slice, level, color);
          }
        }
        continue;
      }

      for (UINT slice = 0; slice < array_size; ++slice) {
        for (UINT level = 0; level < mip_levels; ++level) {
          initializer.initWithZero(texture.ptr(), texture->current(),
                                   slice, level);
        }
      }
    }
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_HEAP_PROPERTIES heap_properties_ = {};
  D3D12_HEAP_FLAGS heap_flags_ = D3D12_HEAP_FLAG_NONE;
  D3D12_RESOURCE_DESC desc_ = {};
  D3D12_RESOURCE_STATES initial_state_ = D3D12_RESOURCE_STATE_COMMON;
  UINT64 heap_offset_ = 0;
  ResourceKind kind_ = ResourceKind::Committed;
  Rc<dxmt::Buffer> placed_buffer_;
  Rc<dxmt::BufferAllocation> placed_buffer_allocation_;
  D3D12_CLEAR_VALUE clear_value_ = {};
  bool has_clear_value_ = false;
  bool has_tiling_ = false;
  ResourceTiling tiling_ = {};
  std::vector<ResourceTileMapping> tile_map_;
  Rc<dxmt::Buffer> buffer_;
  Rc<dxmt::BufferAllocation> buffer_allocation_;
  Rc<dxmt::Texture> texture_;
  Rc<dxmt::TextureAllocation> texture_allocation_;
  std::array<Rc<dxmt::Texture>, kMaxTexturePlanes> plane_textures_{};
  std::array<Rc<dxmt::TextureAllocation>, kMaxTexturePlanes> plane_allocations_{};
  dxmt::TextureViewKey present_source_view_ = {};
  std::string name_;
};

} // namespace

Resource *
LookupBufferResourceByGpuVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS address,
                                        UINT64 *offset) {
  std::lock_guard lock(g_buffer_va_mutex);
  for (auto it = g_buffer_va_ranges.rbegin(); it != g_buffer_va_ranges.rend(); ++it) {
    const auto &range = *it;
    if (BufferGpuVirtualAddressRangeContains(range, address)) {
      if (offset)
        *offset = address - range.base;
      return range.resource;
    }
  }
  return nullptr;
}

bool
IsSupportedResourceDesc(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Width == 0 || desc.Height == 0 || desc.DepthOrArraySize == 0)
    return false;
  if (desc.SampleDesc.Count == 0)
    return false;
  if (!IsSupportedSampleCount(desc.SampleDesc.Count) ||
      desc.SampleDesc.Quality != 0)
    return false;
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    return desc.Height == 1 && desc.DepthOrArraySize == 1 &&
           desc.MipLevels == 1 && desc.Format == DXGI_FORMAT_UNKNOWN &&
           desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
           desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  }
  if (desc.Format == DXGI_FORMAT_UNKNOWN)
    return false;
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  if (traits.classification == DXGIFormatClass::Mask)
    return false;
  if (traits.flags & DXGI_FORMAT_TRAIT_VIDEO) {
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        desc.SampleDesc.Count != 1 || desc.DepthOrArraySize == 0)
      return false;
    if ((desc.Width & 1) || (desc.Height & 1))
      return false;
  }

  const bool layout_row_major =
      desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const bool layout_swizzled =
      desc.Layout == D3D12_TEXTURE_LAYOUT_UNKNOWN ||
      desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE ||
      desc.Layout == D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE;
  if (!layout_row_major && !layout_swizzled)
    return false;

  switch (desc.Dimension) {
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
    if (desc.Height != 1 || desc.SampleDesc.Count != 1)
      return false;
    if (layout_row_major)
      return desc.DepthOrArraySize == 1 && desc.MipLevels <= 1;
    return true;
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
    if (layout_row_major) {
      if (desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 ||
          desc.MipLevels > 1)
        return false;
    } else if (desc.SampleDesc.Count > 1) {
      if (desc.MipLevels != 1)
        return false;
    }
    return true;
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
    if (desc.SampleDesc.Count != 1 || layout_row_major)
      return false;
    return true;
  default:
    return false;
  }
}

Com<ID3D12Resource>
CreateResource(IMTLD3D12Device *device,
               const D3D12_HEAP_PROPERTIES *heap_properties,
               D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
               D3D12_RESOURCE_STATES initial_state, UINT64 heap_offset,
               const D3D12_CLEAR_VALUE *optimized_clear_value,
               ResourceKind kind,
               dxmt::Buffer *placed_buffer,
               dxmt::BufferAllocation *placed_buffer_allocation) {
  return Com<ID3D12Resource>::transfer(
      new ResourceImpl(device, *heap_properties, heap_flags, *desc,
                       initial_state, heap_offset, optimized_clear_value,
                       kind, placed_buffer, placed_buffer_allocation));
}

} // namespace dxmt::d3d12
