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
#include <vector>

namespace dxmt::d3d12 {
namespace {

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
  WMTTextureUsage usage = WMTTextureUsageShaderRead;
  if (flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
               D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
    usage |= WMTTextureUsageRenderTarget;
  if (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    usage |= WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
  return usage;
}

WMTTextureType
GetTextureType(const D3D12_RESOURCE_DESC &desc) {
  if (desc.SampleDesc.Count > 1)
    return desc.DepthOrArraySize > 1 ? WMTTextureType2DMultisampleArray
                                     : WMTTextureType2DMultisample;
  return desc.DepthOrArraySize > 1 ? WMTTextureType2DArray : WMTTextureType2D;
}

class ResourceImpl final : public ComObjectWithInitialRef<ID3D12Resource>,
                           public Resource {
public:
  ResourceImpl(IMTLD3D12Device *device,
               const D3D12_HEAP_PROPERTIES &heap_properties,
               D3D12_HEAP_FLAGS heap_flags,
               const D3D12_RESOURCE_DESC &desc,
               D3D12_RESOURCE_STATES initial_state,
               UINT64 heap_offset)
      : device_(device), heap_properties_(heap_properties),
        heap_flags_(heap_flags), desc_(desc), initial_state_(initial_state),
        heap_offset_(heap_offset) {
    if (desc_.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
      CreateBuffer();
    else
      CreateTexture2D();
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
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  HRESULT STDMETHODCALLTYPE Map(UINT sub_resource, const D3D12_RANGE *read_range,
                                void **data) override {
    if (!data)
      return E_POINTER;
    *data = nullptr;

    if (desc_.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || sub_resource != 0)
      return E_INVALIDARG;
    if (!buffer_allocation_ || !buffer_allocation_->mappedMemory(0))
      return E_INVALIDARG;

    *data = static_cast<char *>(buffer_allocation_->mappedMemory(0)) +
            heap_offset_;
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
    return E_NOTIMPL;
  }

  HRESULT STDMETHODCALLTYPE ReadFromSubresource(
      void *dst_data, UINT dst_row_pitch, UINT dst_slice_pitch,
      UINT src_sub_resource, const D3D12_BOX *src_box) override {
    return E_NOTIMPL;
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

private:
  void CreateBuffer() {
    buffer_ = new dxmt::Buffer(desc_.Width + heap_offset_,
                               device_->GetDXMTDevice().device());
    buffer_allocation_ =
        buffer_->allocate(GetHeapBufferAllocationFlags(heap_properties_));
    buffer_->rename(Rc<dxmt::BufferAllocation>(buffer_allocation_));
    RegisterBufferGpuVirtualAddress(this, GetGpuVirtualAddress(), desc_.Width);
  }

  void CreateTexture2D() {
    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device_->GetDXMTDevice().device(),
                                  desc_.Format, format)))
      return;

    WMTTextureInfo info = {};
    info.pixel_format = format.PixelFormat;
    info.width = static_cast<uint32_t>(desc_.Width);
    info.height = desc_.Height;
    info.depth = 1;
    info.array_length = desc_.DepthOrArraySize;
    info.type = GetTextureType(desc_);
    info.mipmap_level_count = desc_.MipLevels ? desc_.MipLevels : 1;
    info.sample_count = desc_.SampleDesc.Count ? desc_.SampleDesc.Count : 1;
    info.usage = GetTextureUsage(desc_.Flags);

    texture_ = new dxmt::Texture(info, device_->GetDXMTDevice().device());
    Flags<dxmt::TextureAllocationFlag> flags;
    if (GetHeapType(heap_properties_) == D3D12_HEAP_TYPE_DEFAULT) {
      flags.set(dxmt::TextureAllocationFlag::CpuInvisible);
      flags.set(dxmt::TextureAllocationFlag::GpuPrivate);
    } else if (GetHeapType(heap_properties_) == D3D12_HEAP_TYPE_UPLOAD) {
      flags.set(dxmt::TextureAllocationFlag::CpuWriteCombined);
    }
    texture_allocation_ = texture_->allocate(flags);
    texture_->rename(Rc<dxmt::TextureAllocation>(texture_allocation_));
  }

  Com<IMTLD3D12Device> device_;
  ComPrivateData private_data_;
  D3D12_HEAP_PROPERTIES heap_properties_ = {};
  D3D12_HEAP_FLAGS heap_flags_ = D3D12_HEAP_FLAG_NONE;
  D3D12_RESOURCE_DESC desc_ = {};
  D3D12_RESOURCE_STATES initial_state_ = D3D12_RESOURCE_STATE_COMMON;
  UINT64 heap_offset_ = 0;
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
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    return desc.Height == 1 && desc.DepthOrArraySize == 1 &&
           desc.MipLevels == 1 && desc.Format == DXGI_FORMAT_UNKNOWN &&
           desc.SampleDesc.Count == 1 && desc.SampleDesc.Quality == 0 &&
           desc.Layout == D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  }
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
    if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN &&
        desc.Layout != D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE &&
        desc.Layout != D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE)
      return false;
    return desc.Format != DXGI_FORMAT_UNKNOWN;
  }
  return false;
}

Com<ID3D12Resource>
CreateResource(IMTLD3D12Device *device,
               const D3D12_HEAP_PROPERTIES *heap_properties,
               D3D12_HEAP_FLAGS heap_flags, const D3D12_RESOURCE_DESC *desc,
               D3D12_RESOURCE_STATES initial_state, UINT64 heap_offset) {
  return Com<ID3D12Resource>::transfer(
      new ResourceImpl(device, *heap_properties, heap_flags, *desc,
                       initial_state, heap_offset));
}

} // namespace dxmt::d3d12
