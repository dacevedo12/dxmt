#include "d3d12_resource.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_heap.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace dxmt::d3d12 {
namespace {

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

void RegisterBufferGpuVirtualAddress(Resource *resource,
                                     D3D12_GPU_VIRTUAL_ADDRESS base,
                                     UINT64 size) {
  if (!base || !size)
    return;

  std::lock_guard lock(g_buffer_va_mutex);
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
ResolveTextureBackingFormat(const D3D12_RESOURCE_DESC &desc) {
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
  return mip_levels *
         (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
              ? 1
              : desc.DepthOrArraySize);
}

static UINT
GetTextureSubresourceMipLevel(const D3D12_RESOURCE_DESC &desc,
                              UINT sub_resource) {
  return sub_resource % GetMipLevels(desc);
}

static UINT
GetTextureSubresourceArraySlice(const D3D12_RESOURCE_DESC &desc,
                                UINT sub_resource) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 0;
  return sub_resource / GetMipLevels(desc);
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
    return E_INVALIDARG;

  MTL_DXGI_FORMAT_DESC format = {};
  if (FAILED(MTLQueryDXGIFormat(device, desc.Format, format)))
    return E_INVALIDARG;

  const UINT mip = GetTextureSubresourceMipLevel(desc, sub_resource);
  layout.width = std::max<UINT64>(1, desc.Width >> mip);
  layout.height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                      ? 1
                      : std::max<UINT>(1, desc.Height >> mip);
  layout.depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                     ? std::max<UINT>(1, desc.DepthOrArraySize >> mip)
                     : 1;
  layout.block_width = (format.Flag & MTL_DXGI_FORMAT_BC) ? 4 : 1;
  layout.block_height = (format.Flag & MTL_DXGI_FORMAT_BC) ? 4 : 1;
  layout.row_count = std::max<UINT>(
      1, (layout.height + layout.block_height - 1) / layout.block_height);
  layout.element_size = (format.Flag & MTL_DXGI_FORMAT_BC)
                            ? format.BlockSize
                            : format.BytesPerTexel;
  if (!layout.element_size)
    return E_INVALIDARG;

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
    return E_INVALIDARG;
  if (slice_pitch && row_count > 1 &&
      slice_pitch < row_pitch * (row_count - 1) + row_size)
    return E_INVALIDARG;
  if (row_count == 1 && slice_pitch && slice_pitch < row_size)
    return E_INVALIDARG;
  return S_OK;
}

static HRESULT
ValidateTextureCopyPitches(UINT64 row_size, UINT row_count, UINT depth_count,
                           UINT row_pitch, UINT slice_pitch) {
  if (FAILED(ValidateTextureCopyPitches(row_size, row_count, row_pitch,
                                        slice_pitch)))
    return E_INVALIDARG;
  if (depth_count > 1 && slice_pitch < row_pitch * (row_count - 1) + row_size)
    return E_INVALIDARG;
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
    return E_INVALIDARG;

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

class ResourceImpl final : public ComObjectWithInitialRef<ID3D12Resource>,
                           public Resource {
public:
  ResourceImpl(IMTLD3D12Device *device,
               const D3D12_HEAP_PROPERTIES &heap_properties,
               D3D12_HEAP_FLAGS heap_flags,
               const D3D12_RESOURCE_DESC &desc,
               D3D12_RESOURCE_STATES initial_state,
               UINT64 heap_offset,
               const D3D12_CLEAR_VALUE *optimized_clear_value)
      : device_(device), heap_properties_(heap_properties),
        heap_flags_(heap_flags), desc_(desc), initial_state_(initial_state),
        heap_offset_(heap_offset),
        has_clear_value_(optimized_clear_value != nullptr) {
    if (optimized_clear_value)
      clear_value_ = *optimized_clear_value;
    if (!desc_.Alignment)
      desc_.Alignment = GetDefaultResourceAlignment(desc_);
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER && !desc_.MipLevels)
      desc_.MipLevels = GetMaxMipLevels(desc_);

    if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      CreateBuffer();
    else
      CreateTexture();
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
        riid == __uuidof(ID3D12Pageable) || riid == __uuidof(ID3D12Resource)) {
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

  HRESULT STDMETHODCALLTYPE Map(UINT sub_resource, const D3D12_RANGE *read_range,
                                void **data) override {
    if (sub_resource >= GetTextureSubresourceCount(desc_))
      return E_INVALIDARG;

    if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
      if (sub_resource != 0 || !buffer_allocation_ ||
          !buffer_allocation_->mappedMemory(0))
        return E_INVALIDARG;

      if (data)
        *data = static_cast<char *>(buffer_allocation_->mappedMemory(0)) +
                heap_offset_;
      return S_OK;
    }

    if (!texture_allocation_ ||
        !IsTextureSubresourceMappable(desc_, sub_resource, heap_properties_))
      return E_INVALIDARG;

    if (!data)
      return S_OK;

    if (!texture_allocation_->mappedMemory ||
        !IsCpuLinearTextureSubresource(desc_, sub_resource))
      return E_INVALIDARG;

    *data = texture_allocation_->mappedMemory;
    return S_OK;
  }

  void STDMETHODCALLTYPE Unmap(UINT sub_resource,
                               const D3D12_RANGE *written_range) override {
    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        sub_resource != 0 || !buffer_allocation_)
      return;

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
      return E_INVALIDARG;

    return WriteTextureSubresource(dst_sub_resource, dst_box, src_data,
                                   src_row_pitch, src_slice_pitch);
  }

  HRESULT STDMETHODCALLTYPE ReadFromSubresource(
      void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
      UINT src_sub_resource, const D3D12_BOX *src_box) override {
    if (!dst_data)
      return E_POINTER;

    if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      return E_INVALIDARG;

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

  dxmt::TextureAllocation *GetTextureAllocation() const override {
    return texture_allocation_.ptr();
  }

  ID3D12Resource *GetD3D12Resource() override {
    return static_cast<ID3D12Resource *>(this);
  }

private:
  HRESULT WriteTextureSubresource(UINT dst_sub_resource,
                                  const D3D12_BOX *dst_box,
                                  const void *src_data, UINT src_row_pitch,
                                  UINT src_slice_pitch) {
    if (!texture_ || !texture_allocation_)
      return E_INVALIDARG;

    TextureSubresourceLayout layout = {};
    HRESULT hr = GetTextureSubresourceLayout(device_->GetDXMTDevice().device(),
                                             desc_, dst_sub_resource, layout);
    if (FAILED(hr))
      return hr;

    D3D12_BOX box = {};
    if (!NormalizeTextureBox(desc_, layout, dst_box, box))
      return E_INVALIDARG;

    const UINT row_count = TextureBoxRowCount(box, layout);
    const UINT depth_count = TextureBoxDepthCount(box);
    const UINT64 row_size = TextureBoxRowSize(box, layout);
    if (FAILED(ValidateTextureCopyPitches(row_size, row_count, depth_count,
                                          src_row_pitch, src_slice_pitch)))
      return E_INVALIDARG;

    if (texture_allocation_->mappedMemory &&
        IsCpuLinearTextureSubresource(desc_, dst_sub_resource))
      return WriteMappedTextureRows(box, layout, src_data, src_row_pitch,
                                    src_slice_pitch, row_size, row_count,
                                    depth_count);

    return WriteTextureRowsViaBlit(box, layout, src_data, src_row_pitch,
                                   src_slice_pitch, row_size, row_count,
                                   depth_count, dst_sub_resource);
  }

  HRESULT ReadTextureSubresource(void *dst_data, UINT dst_row_pitch,
                                 UINT dst_slice_pitch, UINT src_sub_resource,
                                 const D3D12_BOX *src_box) {
    if (!texture_ || !texture_allocation_)
      return E_INVALIDARG;

    TextureSubresourceLayout layout = {};
    HRESULT hr = GetTextureSubresourceLayout(device_->GetDXMTDevice().device(),
                                             desc_, src_sub_resource, layout);
    if (FAILED(hr))
      return hr;

    D3D12_BOX box = {};
    if (!NormalizeTextureBox(desc_, layout, src_box, box))
      return E_INVALIDARG;

    const UINT row_count = TextureBoxRowCount(box, layout);
    const UINT depth_count = TextureBoxDepthCount(box);
    const UINT64 row_size = TextureBoxRowSize(box, layout);
    if (FAILED(ValidateTextureCopyPitches(row_size, row_count, depth_count,
                                          dst_row_pitch, dst_slice_pitch)))
      return E_INVALIDARG;

    if (texture_allocation_->mappedMemory &&
        IsCpuLinearTextureSubresource(desc_, src_sub_resource))
      return ReadMappedTextureRows(dst_data, dst_row_pitch, box, layout,
                                   dst_slice_pitch, row_size, row_count,
                                   depth_count);

    return ReadTextureRowsViaBlit(dst_data, dst_row_pitch, box, layout,
                                  dst_slice_pitch, row_size, row_count,
                                  depth_count, src_sub_resource);
  }

  HRESULT WriteMappedTextureRows(const D3D12_BOX &box,
                                 const TextureSubresourceLayout &layout,
                                 const void *src_data, UINT src_row_pitch,
                                 UINT src_slice_pitch, UINT64 row_size,
                                 UINT row_count, UINT depth_count) {
    auto *dst = static_cast<char *>(texture_allocation_->mappedMemory);
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
                                UINT row_count, UINT depth_count) {
    auto *dst = static_cast<char *>(dst_data);
    const auto *src = static_cast<const char *>(texture_allocation_->mappedMemory);
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
                                  UINT dst_sub_resource) {
    const UINT staging_slice_pitch = layout.row_pitch * row_count;
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

    Rc<dxmt::Texture> texture = texture_;
    const UINT dst_slice = GetTextureSubresourceArraySlice(desc_, dst_sub_resource);
    const UINT dst_level = GetTextureSubresourceMipLevel(desc_, dst_sub_resource);
    const UINT origin_z =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? box.front : 0;
    const UINT bytes_per_image =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? staging_slice_pitch
            : 0;
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
                                 UINT src_sub_resource) {
    const UINT staging_slice_pitch = layout.row_pitch * row_count;
    WMTBufferInfo buffer_info = {};
    buffer_info.length = staging_slice_pitch * depth_count;
    buffer_info.options = WMTResourceHazardTrackingModeUntracked;
    auto buffer = device_->GetDXMTDevice().device().newBuffer(buffer_info);
    if (!buffer || !buffer_info.memory.get())
      return E_FAIL;

    Rc<dxmt::Texture> texture = texture_;
    const UINT src_slice = GetTextureSubresourceArraySlice(desc_, src_sub_resource);
    const UINT src_level = GetTextureSubresourceMipLevel(desc_, src_sub_resource);
    const UINT origin_z =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? box.front : 0;
    const UINT bytes_per_image =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? staging_slice_pitch
            : 0;
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
    buffer_ = new dxmt::Buffer(desc_.Width + heap_offset_,
                               device_->GetDXMTDevice().device());
    buffer_allocation_ =
        buffer_->allocate(GetHeapBufferAllocationFlags(heap_properties_));
    buffer_->rename(Rc<dxmt::BufferAllocation>(buffer_allocation_));
    RegisterBufferGpuVirtualAddress(this, GetGpuVirtualAddress(), desc_.Width);
  }

  void CreateTexture() {
    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device_->GetDXMTDevice().device(),
                                  ResolveTextureBackingFormat(desc_), format)))
      return;

    WMTTextureInfo info = {};
    info.pixel_format = format.PixelFormat;
    info.width = static_cast<uint32_t>(desc_.Width);
    info.height = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                      ? 1
                      : desc_.Height;
    info.depth = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                     ? desc_.DepthOrArraySize
                     : 1;
    info.array_length = desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                            ? 1
                            : desc_.DepthOrArraySize;
    info.type = GetTextureType(desc_);
    info.mipmap_level_count = std::min(GetMipLevels(desc_), GetMaxMipLevels(desc_));
    info.sample_count = desc_.SampleDesc.Count ? desc_.SampleDesc.Count : 1;
    info.usage = GetTextureUsage(desc_.Flags);

    Flags<dxmt::TextureAllocationFlag> flags;
    TextureSubresourceLayout layout = {};
    const bool linear_cpu_texture =
        GetHeapType(heap_properties_) != D3D12_HEAP_TYPE_DEFAULT &&
        IsCpuLinearTextureSubresource(desc_, 0) &&
        SUCCEEDED(GetTextureSubresourceLayout(device_->GetDXMTDevice().device(),
                                             desc_, 0, layout));

    if (linear_cpu_texture) {
      // TODO(d3d12): extend CPU-linear custom heap textures beyond the
      // single-subresource 2D case so ReadFromSubresource/WriteToSubresource
      // can cover the full D3D12 custom-heap texture surface.
      texture_ = new dxmt::Texture(layout.slice_pitch, layout.row_pitch, info,
                                   device_->GetDXMTDevice().device());
    } else {
      texture_ = new dxmt::Texture(info, device_->GetDXMTDevice().device());
    }

    if (GetHeapType(heap_properties_) == D3D12_HEAP_TYPE_DEFAULT) {
      flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
      flags.set(dxmt::TextureAllocationFlag::GpuPrivate);
    } else if (GetHeapType(heap_properties_) == D3D12_HEAP_TYPE_UPLOAD) {
      flags.set(dxmt::TextureAllocationFlag::CpuWriteCombined);
    }
    texture_allocation_ = texture_->allocate(flags);
    texture_->rename(Rc<dxmt::TextureAllocation>(texture_allocation_));

    if (GetHeapType(heap_properties_) == D3D12_HEAP_TYPE_DEFAULT) {
      InitializeTextureContents(format.PixelFormat);
    }
  }

  void InitializeTextureContents(WMTPixelFormat pixel_format) {
    auto &initializer = device_->GetDXMTDevice().queue().initializer;
    const UINT mip_levels = desc_.MipLevels ? desc_.MipLevels : 1;
    const UINT array_size =
        desc_.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
            ? 1
            : desc_.DepthOrArraySize;
    const auto dsv_planar = DepthStencilPlanarFlags(pixel_format);

    if (dsv_planar &&
        (desc_.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
      const float depth = has_clear_value_ ? clear_value_.DepthStencil.Depth : 0.0f;
      const uint8_t stencil = has_clear_value_ ? clear_value_.DepthStencil.Stencil : 0;
      for (UINT slice = 0; slice < array_size; ++slice) {
        for (UINT level = 0; level < mip_levels; ++level) {
          initializer.initDepthStencilWithZero(
              texture_.ptr(), texture_->current(), slice, level, dsv_planar,
              depth, stencil);
        }
      }
      return;
    }

    if ((texture_->usage() & WMTTextureUsageRenderTarget) &&
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
              texture_.ptr(), texture_->current(), slice, level, color);
        }
      }
      return;
    }

    for (UINT slice = 0; slice < array_size; ++slice) {
      for (UINT level = 0; level < mip_levels; ++level) {
        initializer.initWithZero(texture_.ptr(), texture_->current(),
                                 slice, level);
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
  D3D12_CLEAR_VALUE clear_value_ = {};
  bool has_clear_value_ = false;
  Rc<dxmt::Buffer> buffer_;
  Rc<dxmt::BufferAllocation> buffer_allocation_;
  Rc<dxmt::Texture> texture_;
  Rc<dxmt::TextureAllocation> texture_allocation_;
  std::string name_;
};

} // namespace

Resource *
LookupBufferResourceByGpuVirtualAddress(D3D12_GPU_VIRTUAL_ADDRESS address,
                                        UINT64 *offset) {
  std::lock_guard lock(g_buffer_va_mutex);
  for (const auto &range : g_buffer_va_ranges) {
    if (address >= range.base && address < range.base + range.size) {
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
               const D3D12_CLEAR_VALUE *optimized_clear_value) {
  return Com<ID3D12Resource>::transfer(
      new ResourceImpl(device, *heap_properties, heap_flags, *desc,
                       initial_state, heap_offset, optimized_clear_value));
}

} // namespace dxmt::d3d12
