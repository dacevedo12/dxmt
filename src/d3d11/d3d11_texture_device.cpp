#include "com/com_pointer.hpp"
#include "d3d11_device.hpp"
#include "d3d11_enumerable.hpp"
#include "d3d11_view.hpp"
#include "dxmt_dynamic.hpp"
#include "dxmt_staging.hpp"
#include "dxmt_texture.hpp"
#include "d3d11_resource.hpp"
#include "util_win32_compat.h"
#include <cstddef>

namespace dxmt {

#pragma region DeviceTexture

template <typename desc_t>
static UINT
TextureDescHeight(const desc_t &desc) {
  if constexpr (std::is_same_v<desc_t, D3D11_TEXTURE1D_DESC>) {
    return 1;
  } else {
    return desc.Height;
  }
}

template <typename desc_t>
static UINT
TextureDescDepth(const desc_t &desc) {
  if constexpr (std::is_same_v<desc_t, D3D11_TEXTURE3D_DESC1>) {
    return desc.Depth;
  } else {
    return 1;
  }
}

template <typename desc_t>
static UINT
TextureDescArraySize(const desc_t &desc) {
  if constexpr (std::is_same_v<desc_t, D3D11_TEXTURE3D_DESC1>) {
    return 1;
  } else {
    return desc.ArraySize;
  }
}

template <typename desc_t>
static UINT
TextureDescSampleCount(const desc_t &desc) {
  if constexpr (std::is_same_v<desc_t, D3D11_TEXTURE2D_DESC1>) {
    return desc.SampleDesc.Count;
  } else {
    return 1;
  }
}

static bool
IsTextureViewRangeValid(const TextureViewDescriptor &view, UINT mip_levels,
                        UINT array_size) {
  return view.miplevelCount && view.firstMiplevel < mip_levels &&
         view.miplevelCount <= mip_levels - view.firstMiplevel &&
         view.arraySize && view.firstArraySlice < array_size &&
         view.arraySize <= array_size - view.firstArraySlice;
}

static constexpr SIZE_T kD3DKMTExistingHeapPageSize = 0x1000;

static SIZE_T
AlignD3DKMTExistingHeapSize(SIZE_T size) {
  if (!size)
    size = kD3DKMTExistingHeapPageSize;
  return (size + kD3DKMTExistingHeapPageSize - 1) & ~(kD3DKMTExistingHeapPageSize - 1);
}

template <typename desc_t>
static SIZE_T
CalculateD3DKMTExistingHeapSize(MTLD3D11Device *pDevice, const desc_t &desc) {
  SIZE_T size = 0;
  auto mipLevels = desc.MipLevels ? desc.MipLevels : 1;
  auto arraySize = TextureDescArraySize(desc);
  auto sampleCount = TextureDescSampleCount(desc);

  for (UINT level = 0; level < mipLevels; level++) {
    uint32_t bytesPerRow = 0;
    uint32_t bytesPerImage = 0;
    uint32_t bytesPerSlice = 0;
    if (FAILED(GetLinearTextureLayout(pDevice, desc, level, bytesPerRow, bytesPerImage, bytesPerSlice, true)))
      return kD3DKMTExistingHeapPageSize;
    size += SIZE_T(bytesPerSlice) * arraySize * sampleCount;
  }

  return AlignD3DKMTExistingHeapSize(size);
}

static void
DestroyD3DKMTKeyedMutex(D3DKMT_HANDLE &handle) {
  if (!handle)
    return;
  D3DKMT_DESTROYKEYEDMUTEX destroy = {};
  destroy.hKeyedMutex = handle;
  D3DKMTDestroyKeyedMutex(&destroy);
  handle = 0;
}

static void
DestroyD3DKMTSyncObject(D3DKMT_HANDLE &handle) {
  if (!handle)
    return;
  D3DKMT_DESTROYSYNCHRONIZATIONOBJECT destroy = {};
  destroy.hSyncObject = handle;
  D3DKMTDestroySynchronizationObject(&destroy);
  handle = 0;
}

template <typename tag_texture>
class DeviceTexture : public TResourceBase<tag_texture, IMTLMinLODClampable> {
private:
  Rc<RenamableTexturePool> renamable_;
  float min_lod = 0.0;
  D3DKMT_HANDLE local_kmt_ = 0;
  D3DKMT_HANDLE global_kmt_ = 0;
  D3DKMT_HANDLE keyed_mutex_ = 0;
  D3DKMT_HANDLE sync_object_ = 0;
  D3DKMT_HANDLE keyed_mutex_global_ = 0;
  D3DKMT_HANDLE sync_object_global_ = 0;
  WMT::Reference<WMT::SharedEvent> keyed_mutex_event_;

  static UINT
  DebugDescHeight(const typename tag_texture::DESC1 &desc) {
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE1D_DESC>) {
      return 1;
    } else {
      return desc.Height;
    }
  }

  static UINT
  DebugDescDepth(const typename tag_texture::DESC1 &desc) {
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      return desc.Depth;
    } else {
      return 1;
    }
  }

  static UINT
  DebugDescArraySize(const typename tag_texture::DESC1 &desc) {
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      return 1;
    } else {
      return desc.ArraySize;
    }
  }

  static UINT
  DebugDescSampleCount(const typename tag_texture::DESC1 &desc) {
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE2D_DESC1>) {
      return desc.SampleDesc.Count;
    } else {
      return 1;
    }
  }

  static UINT
  DebugDescSampleQuality(const typename tag_texture::DESC1 &desc) {
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE2D_DESC1>) {
      return desc.SampleDesc.Quality;
    } else {
      return 0;
    }
  }

  void
  LogRenderTargetViewFailure(const char *reason, const D3D11_RENDER_TARGET_VIEW_DESC1 &viewDesc) const {
    WARN(
        "CreateRenderTargetView: ", reason,
        " resource=", reinterpret_cast<const void *>(this),
        " dimension=", uint32_t(tag_texture::dimension),
        " resource_format=", uint32_t(this->desc.Format),
        " view_format=", uint32_t(viewDesc.Format),
        " view_dimension=", uint32_t(viewDesc.ViewDimension),
        " bind_flags=", this->desc.BindFlags,
        " metal_usage=", uint32_t(this->texture_->usage()),
        " usage=", uint32_t(this->desc.Usage),
        " cpu_access=", this->desc.CPUAccessFlags,
        " misc_flags=", this->desc.MiscFlags,
        " size=", this->desc.Width, "x", DebugDescHeight(this->desc), "x", DebugDescDepth(this->desc),
        " array_size=", DebugDescArraySize(this->desc),
        " mip_levels=", this->desc.MipLevels,
        " sample=", DebugDescSampleCount(this->desc), ":", DebugDescSampleQuality(this->desc)
    );
  }

  using SRVBase =
      TResourceViewBase<tag_shader_resource_view<DeviceTexture<tag_texture>>>;
  class TextureSRV : public SRVBase {
  public:
    TextureSRV(const TextureViewDescriptor &descriptor,
               const tag_shader_resource_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, MTLD3D11Device *pDevice)
        : SRVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->texture_.ptr();
      this->view_id_ = this->texture_->createView(descriptor);
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }

  };

  using UAVBase =
      TResourceViewBase<tag_unordered_access_view<DeviceTexture<tag_texture>>>;
  class TextureUAV : public UAVBase {
  public:
    TextureUAV(const TextureViewDescriptor &descriptor,
               const tag_unordered_access_view<>::DESC1 *pDesc,
               DeviceTexture *pResource, MTLD3D11Device *pDevice)
        : UAVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->texture_.ptr();
      this->view_id_ = this->texture_->createView(descriptor);
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }
  };

  using RTVBase =
      TResourceViewBase<tag_render_target_view<DeviceTexture<tag_texture>>>;
  class TextureRTV : public RTVBase {
  public:
    TextureRTV(
        const TextureViewDescriptor &descriptor, WMTPixelFormat view_format, const tag_render_target_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &mtl_rtv_desc
    ) :
        RTVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->texture_.ptr();
      auto rtv_descriptor = descriptor;
      rtv_descriptor.intendedUsage = WMTTextureUsageRenderTarget;
      this->view_id_ = this->texture_->createView(rtv_descriptor);
      this->format_ = view_format;
      this->pass_desc_ = mtl_rtv_desc;
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength()
      );
    }
  };

  using DSVBase =
      TResourceViewBase<tag_depth_stencil_view<DeviceTexture<tag_texture>>>;
  class TextureDSV : public DSVBase {
  public:
    TextureDSV(
        const TextureViewDescriptor &descriptor, WMTPixelFormat view_format, const tag_depth_stencil_view<>::DESC1 *pDesc,
        DeviceTexture *pResource, MTLD3D11Device *pDevice, const MTL_RENDER_PASS_ATTACHMENT_DESC &attachment_desc
    ) :
        DSVBase(pDesc, pResource, pDevice) {
      this->texture_ = pResource->texture_.ptr();
      auto dsv_descriptor = descriptor;
      dsv_descriptor.intendedUsage = WMTTextureUsageRenderTarget;
      this->view_id_ = this->texture_->createView(dsv_descriptor);
      this->format_ = view_format;
      this->pass_desc_ = attachment_desc;
      this->renamable_ = pResource->renamable_.ptr();
      this->readonly_flags_ = this->desc.Flags & 0b11;
      this->subset_ = ResourceSubsetState(
        &descriptor,
        this->texture_->miplevelCount(),
        this->texture_->arrayLength(),
        this->readonly_flags_
      );
    }
  };

public:
  DeviceTexture(const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, MTLD3D11Device *pDevice) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice) {
        this->texture_ = std::move(u_texture);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, Rc<RenamableTexturePool> &&renamable,
      MTLD3D11Device *pDevice
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      renamable_(std::move(renamable)) {
        this->texture_ = std::move(u_texture);
      }

  DeviceTexture(
      const tag_texture::DESC1 *pDesc, Rc<Texture> &&u_texture, D3DKMT_HANDLE localHandle, D3DKMT_HANDLE globalHandle,
      MTLD3D11Device *pDevice, D3DKMT_HANDLE keyedMutex = 0, D3DKMT_HANDLE syncObject = 0,
      D3DKMT_HANDLE keyedMutexGlobal = 0, D3DKMT_HANDLE syncObjectGlobal = 0,
      WMT::Reference<WMT::SharedEvent> keyedMutexEvent = {}
  ) :
      TResourceBase<tag_texture, IMTLMinLODClampable>(*pDesc, pDevice),
      local_kmt_(localHandle), global_kmt_(globalHandle), keyed_mutex_(keyedMutex), sync_object_(syncObject),
      keyed_mutex_global_(keyedMutexGlobal), sync_object_global_(syncObjectGlobal),
      keyed_mutex_event_(std::move(keyedMutexEvent)) {
        this->texture_ = std::move(u_texture);
      }

  ~DeviceTexture() {
    if (local_kmt_) {
      D3DKMT_DESTROYALLOCATION destroy = {};
      destroy.hDevice = this->m_parent->GetLocalD3DKMT();
      destroy.hResource = local_kmt_;
      D3DKMTDestroyAllocation(&destroy);
    }
    DestroyD3DKMTKeyedMutex(keyed_mutex_);
    DestroyD3DKMTSyncObject(sync_object_);
  }

  void DetachKeyedMutexHandles() {
    keyed_mutex_ = 0;
    sync_object_ = 0;
  }

  Rc<StagingResource> staging(UINT) final { return nullptr; }
  Rc<DynamicBuffer> dynamicBuffer(UINT*, UINT*) final { return {}; }
  Rc<DynamicLinearTexture> dynamicLinearTexture(UINT*, UINT*) final { return {}; };
  Rc<DynamicBuffer> dynamicTexture(UINT , UINT *, UINT *) final { return {}; };

  HRESULT STDMETHODCALLTYPE CreateRenderTargetView(const D3D11_RENDER_TARGET_VIEW_DESC1 *pDesc,
                                 ID3D11RenderTargetView1 **ppView) override {
    D3D11_RENDER_TARGET_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    if (!(this->desc.BindFlags & D3D11_BIND_RENDER_TARGET)) {
      LogRenderTargetViewFailure("resource missing D3D11_BIND_RENDER_TARGET", finalDesc);
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->texture_.ptr(), finalDesc,
            attachment_desc, descriptor
        ))) {
      return E_FAIL;
    }
    if (!IsTextureViewRangeValid(descriptor, this->desc.MipLevels, arraySize))
      return E_INVALIDARG;
    if (!(this->texture_->usage() & WMTTextureUsageRenderTarget)) {
      LogRenderTargetViewFailure("texture usage missing WMTTextureUsageRenderTarget", finalDesc);
      return E_FAIL;
    }
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureRTV(descriptor, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT STDMETHODCALLTYPE CreateDepthStencilView(const D3D11_DEPTH_STENCIL_VIEW_DESC *pDesc,
                                 ID3D11DepthStencilView **ppView) override {
    D3D11_DEPTH_STENCIL_VIEW_DESC finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    MTL_RENDER_PASS_ATTACHMENT_DESC attachment_desc;
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->texture_.ptr(), finalDesc,
            attachment_desc, descriptor
        ))) {
      return E_FAIL;
    }
    if (!IsTextureViewRangeValid(descriptor, this->desc.MipLevels, arraySize))
      return E_INVALIDARG;
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureDSV(descriptor, descriptor.format, &finalDesc, this, this->m_parent, attachment_desc));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateShaderResourceView(const D3D11_SHADER_RESOURCE_VIEW_DESC1 *pDesc,
                           ID3D11ShaderResourceView1 **ppView) override {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      ERR("DeviceTexture: Failed to create SRV descriptor");
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->texture_.ptr(), finalDesc, descriptor
        ))) {
      ERR("DeviceTexture: Failed to create texture SRV");
      return E_FAIL;
    }
    if (!IsTextureViewRangeValid(descriptor, this->desc.MipLevels, arraySize))
      return E_INVALIDARG;
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureSRV(descriptor, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  HRESULT
  STDMETHODCALLTYPE
  CreateUnorderedAccessView(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 *pDesc,
                            ID3D11UnorderedAccessView1 **ppView) override {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 finalDesc;
    if (FAILED(ExtractEntireResourceViewDescription(&this->desc, pDesc,
                                                    &finalDesc))) {
      return E_INVALIDARG;
    }
    TextureViewDescriptor descriptor;
    uint32_t arraySize;
    if constexpr (std::is_same_v<typename tag_texture::DESC1, D3D11_TEXTURE3D_DESC1>) {
      arraySize = this->desc.Depth;
    } else {
      arraySize = this->desc.ArraySize;
    }
    if (FAILED(InitializeAndNormalizeViewDescriptor(
            this->m_parent, this->desc.MipLevels, arraySize, this->texture_.ptr(), finalDesc, descriptor
        ))) {
      ERR("DeviceTexture: Failed to create texture UAV");
      return E_FAIL;
    }
    if (!IsTextureViewRangeValid(descriptor, this->desc.MipLevels, arraySize))
      return E_INVALIDARG;
    if (!ppView) {
      return S_FALSE;
    }
    *ppView = ref(new TextureUAV(descriptor, &finalDesc, this, this->m_parent));
    return S_OK;
  };

  virtual HRESULT
  GetSharedHandle(HANDLE *pSharedHandle) override {
    if (pSharedHandle == nullptr || (this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
      return E_INVALIDARG;
    }

    if (!(this->desc.MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX))) {
      *pSharedHandle = NULL;
      return S_OK;
    }

    if (!global_kmt_) {
      return E_INVALIDARG;
    }

    *pSharedHandle = reinterpret_cast<HANDLE>(global_kmt_);
    return S_OK;
  }

  virtual HRESULT
  CreateSharedHandle(const SECURITY_ATTRIBUTES *Attributes, DWORD Access, const WCHAR *pName, HANDLE *pNTHandle)
      override {
    InitReturnPtr(pNTHandle);
    if (!local_kmt_)
      return E_INVALIDARG;
    if ((this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) && (!keyed_mutex_ || !sync_object_))
      return E_INVALIDARG;

    OBJECT_ATTRIBUTES attr = {};
    attr.Length = sizeof(attr);
    attr.SecurityDescriptor = const_cast<SECURITY_ATTRIBUTES*>(Attributes);

    WCHAR buffer[MAX_PATH];
    UNICODE_STRING name_str;
    if (pName) {
      DWORD session, len, name_len = wcslen(pName);

      ProcessIdToSessionId(GetCurrentProcessId(), &session);
      len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
      memcpy(buffer + len, pName, (name_len + 1) * sizeof(WCHAR));
      name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
      name_str.MaximumLength += sizeof(WCHAR);
      name_str.Buffer = buffer;

      attr.ObjectName = &name_str;
      attr.Attributes = OBJ_CASE_INSENSITIVE;
    }

    D3DKMT_HANDLE handles[3] = {local_kmt_, keyed_mutex_, sync_object_};
    UINT count = (this->desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) ? 3 : 1;

    if (D3DKMTShareObjects(count, handles, &attr, Access, pNTHandle)) {
      ERR("DeviceTexture: Failed to create shared handle");
      return E_FAIL;
    }

    return S_OK;
  }

  D3DKMT_HANDLE GetKeyedMutexHandle() const override { return keyed_mutex_; }

  WMT::Reference<WMT::SharedEvent> GetKeyedMutexEvent() const override { return keyed_mutex_event_; }

  void SetMinLOD(float MinLod) override { min_lod = MinLod; }

  float GetMinLOD() override { return min_lod; }
};

struct WineD3DKMTDXGIDesc {
  UINT size;
  UINT version;
  UINT width;
  UINT height;
  DXGI_FORMAT format;
  UINT unknown_0;
  UINT unknown_1;
  UINT keyed_mutex;
  D3DKMT_HANDLE mutex_handle;
  D3DKMT_HANDLE sync_handle;
  UINT nt_shared;
  UINT unknown_2;
  UINT unknown_3;
  UINT unknown_4;
};

struct WineD3DKMTD3D11Desc {
  WineD3DKMTDXGIDesc dxgi;
  D3D11_RESOURCE_DIMENSION dimension;
  union {
    D3D10_BUFFER_DESC d3d10_buf;
    D3D10_TEXTURE1D_DESC d3d10_1d;
    D3D10_TEXTURE2D_DESC d3d10_2d;
    D3D10_TEXTURE3D_DESC d3d10_3d;
    D3D11_BUFFER_DESC d3d11_buf;
    D3D11_TEXTURE1D_DESC d3d11_1d;
    D3D11_TEXTURE2D_DESC d3d11_2d;
    D3D11_TEXTURE3D_DESC d3d11_3d;
  };
};
static_assert(sizeof(WineD3DKMTD3D11Desc) == 0x68);

struct SharedResourceDataV1 {
  char mach_port_name[54];
  D3D11_RESOURCE_DIMENSION dimension;
  union {
    D3D11_TEXTURE1D_DESC desc1d;
    D3D11_TEXTURE2D_DESC1 desc2d;
    D3D11_TEXTURE3D_DESC1 desc3d;
  } desc;
  D3DKMT_HANDLE keyed_mutex_global;
  D3DKMT_HANDLE sync_object_global;
  char keyed_mutex_event_name[54];
};

struct SharedResourceData {
  WineD3DKMTD3D11Desc wine_desc;
  char mach_port_name[54];
  char keyed_mutex_event_name[54];
};

static constexpr UINT kSharedResourceDataLegacySize = offsetof(SharedResourceDataV1, keyed_mutex_global);
static constexpr UINT kSharedResourceDataNoKeyedEventSize = offsetof(SharedResourceDataV1, keyed_mutex_event_name);

static bool
IsValidSharedResourceDataSize(UINT size) {
  return (size >= kSharedResourceDataLegacySize && size <= sizeof(SharedResourceDataV1)) ||
         size == sizeof(SharedResourceData);
}

struct SharedResourceInfo {
  char mach_port_name[54] = {};
  char keyed_mutex_event_name[54] = {};
  D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  union {
    D3D11_TEXTURE1D_DESC desc1d;
    D3D11_TEXTURE2D_DESC1 desc2d;
    D3D11_TEXTURE3D_DESC1 desc3d;
  } desc = {};
  D3DKMT_HANDLE keyed_mutex_global = 0;
  D3DKMT_HANDLE sync_object_global = 0;
  bool has_keyed_mutex_event = false;
};

static bool
NormalizeSharedResourceData(const SharedResourceData &runtimeData, UINT size, SharedResourceInfo &info) {
  if (size == sizeof(SharedResourceData)) {
    memcpy(info.mach_port_name, runtimeData.mach_port_name, sizeof(info.mach_port_name));
    memcpy(info.keyed_mutex_event_name, runtimeData.keyed_mutex_event_name, sizeof(info.keyed_mutex_event_name));
    info.dimension = runtimeData.wine_desc.dimension;
    info.keyed_mutex_global = runtimeData.wine_desc.dxgi.mutex_handle;
    info.sync_object_global = runtimeData.wine_desc.dxgi.sync_handle;
    info.has_keyed_mutex_event = info.keyed_mutex_event_name[0] != '\0';

    switch (info.dimension)
    {
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
      info.desc.desc1d = runtimeData.wine_desc.d3d11_1d;
      return true;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
      UpgradeResourceDescription(&runtimeData.wine_desc.d3d11_2d, info.desc.desc2d);
      return true;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
      UpgradeResourceDescription(&runtimeData.wine_desc.d3d11_3d, info.desc.desc3d);
      return true;
    default:
      return false;
    }
  }

  SharedResourceDataV1 legacy = {};
  memcpy(&legacy, &runtimeData, std::min<size_t>(size, sizeof(legacy)));
  memcpy(info.mach_port_name, legacy.mach_port_name, sizeof(info.mach_port_name));
  info.dimension = legacy.dimension;
  switch (info.dimension)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    info.desc.desc1d = legacy.desc.desc1d;
    break;
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    info.desc.desc2d = legacy.desc.desc2d;
    break;
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    info.desc.desc3d = legacy.desc.desc3d;
    break;
  default:
    return false;
  }
  if (size >= kSharedResourceDataLegacySize + sizeof(D3DKMT_HANDLE) * 2) {
    info.keyed_mutex_global = legacy.keyed_mutex_global;
    info.sync_object_global = legacy.sync_object_global;
  }
  if (size >= sizeof(SharedResourceDataV1)) {
    memcpy(info.keyed_mutex_event_name, legacy.keyed_mutex_event_name, sizeof(info.keyed_mutex_event_name));
    info.has_keyed_mutex_event = info.keyed_mutex_event_name[0] != '\0';
  }
  return true;
}

template <typename desc_t>
static void
FillWineSharedResourceDesc(WineD3DKMTD3D11Desc &wineDesc, const desc_t &desc, D3DKMT_HANDLE keyedMutexGlobal,
                           D3DKMT_HANDLE syncObjectGlobal, bool ntSecuritySharing) {
  wineDesc.dxgi.size = sizeof(WineD3DKMTD3D11Desc);
  wineDesc.dxgi.version = 1;
  wineDesc.dxgi.width = desc.Width;
  wineDesc.dxgi.height = TextureDescHeight(desc);
  wineDesc.dxgi.format = desc.Format;
  wineDesc.dxgi.keyed_mutex = keyedMutexGlobal ? 1 : 0;
  wineDesc.dxgi.mutex_handle = keyedMutexGlobal;
  wineDesc.dxgi.sync_handle = syncObjectGlobal;
  wineDesc.dxgi.nt_shared = ntSecuritySharing ? 1 : 0;
  wineDesc.dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;

  if constexpr (std::is_same_v<desc_t, D3D11_TEXTURE1D_DESC>) {
    wineDesc.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    wineDesc.d3d11_1d = desc;
  } else if constexpr (std::is_same_v<desc_t, D3D11_TEXTURE2D_DESC1>) {
    D3D11_TEXTURE2D_DESC desc2d = {};
    DowngradeResourceDescription(desc, &desc2d);
    wineDesc.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    wineDesc.d3d11_2d = desc2d;
  } else if constexpr (std::is_same_v<desc_t, D3D11_TEXTURE3D_DESC1>) {
    D3D11_TEXTURE3D_DESC desc3d = {};
    DowngradeResourceDescription(desc, &desc3d);
    wineDesc.dimension = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    wineDesc.d3d11_3d = desc3d;
  }
}

static WMT::Reference<WMT::SharedEvent>
OpenSharedKeyedMutexEvent(MTLD3D11Device *pDevice, const SharedResourceInfo &runtimeData) {
  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeData.keyed_mutex_event_name, &mach_port)) {
    WARN("ImportSharedTexture: Failed to look up keyed mutex shared event");
    return {};
  }

  auto event = pDevice->GetMTLDevice().newSharedEventWithMachPort(mach_port);
  if (!event) {
    WARN("ImportSharedTexture: Failed to import keyed mutex shared event");
    return {};
  }
  return event;
}

static HRESULT
OpenSharedKeyedMutexHandles(const SharedResourceInfo &runtimeData, D3DKMT_HANDLE &keyedMutex,
                            D3DKMT_HANDLE &syncObject) {
  keyedMutex = 0;
  syncObject = 0;

  if (!runtimeData.keyed_mutex_global || !runtimeData.sync_object_global) {
    WARN("ImportSharedTexture: keyed resource is missing shared keyed mutex handles");
    return E_INVALIDARG;
  }

  D3DKMT_OPENKEYEDMUTEX2 openMutex = {};
  openMutex.hSharedHandle = runtimeData.keyed_mutex_global;
  auto status = D3DKMTOpenKeyedMutex2(&openMutex);
  if (status) {
    WARN("ImportSharedTexture: Failed to open keyed mutex, status=", status);
    return E_INVALIDARG;
  }

  D3DKMT_OPENSYNCHRONIZATIONOBJECT openSync = {};
  openSync.hSharedHandle = runtimeData.sync_object_global;
  status = D3DKMTOpenSynchronizationObject(&openSync);
  if (status) {
    WARN("ImportSharedTexture: Failed to open sync object, status=", status);
    keyedMutex = openMutex.hKeyedMutex;
    DestroyD3DKMTKeyedMutex(keyedMutex);
    return E_INVALIDARG;
  }

  keyedMutex = openMutex.hKeyedMutex;
  syncObject = openSync.hSyncObject;
  return S_OK;
}

template <typename tag>
HRESULT CreateDeviceTextureInternal(MTLD3D11Device *pDevice,
                                    const typename tag::DESC1 *pDesc,
                                    const D3D11_SUBRESOURCE_DATA *pInitialData,
                                    typename tag::COM_IMPL **ppTexture) {
  WMTTextureInfo info = {};
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDesc, &finalDesc, &info))) {
    return E_INVALIDARG;
  }
  bool single_subresource = info.mipmap_level_count == 1 && info.array_length == 1 &&
                            !(finalDesc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE);
  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));

  auto &initializer = pDevice->GetDXMTDevice().queue().initializer;

  auto initialize = [&](Rc<TextureAllocation> &&allocation) {
    texture->rename(std::move(allocation));
    if (!pInitialData) {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        initializer.initWithZero(texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel);
      }
    } else {
      for (auto sub : EnumerateSubresources(finalDesc)) {
        auto &data = pInitialData[sub.SubresourceId];
        initializer.initWithData(
            texture.ptr(), texture->current(), sub.ArraySlice, sub.MipLevel, data.pSysMem, data.SysMemPitch,
            data.SysMemSlicePitch
        );
      }
    }
  };

  auto shared_flag =
      D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
  if (finalDesc.MiscFlags & shared_flag) {
    auto local_kmt = pDevice->GetLocalD3DKMT();
    if (!local_kmt) {
      ERR(
          "DeviceTexture: Invalid device handle",
          " local_kmt=", local_kmt,
          " misc_flags=", finalDesc.MiscFlags,
          " dimension=", uint32_t(tag::dimension),
          " size=", finalDesc.Width, "x", TextureDescHeight(finalDesc), "x", TextureDescDepth(finalDesc)
      );
      return E_FAIL;
    }
    // use a dedicated path for now, because there are other works for private storage mode

    Flags<TextureAllocationFlag> flags;
    flags.set(TextureAllocationFlag::GpuPrivate);
    if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
      flags.set(TextureAllocationFlag::GpuReadonly);
    if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
      flags.set(TextureAllocationFlag::ShaderReadonly);
    flags.set(TextureAllocationFlag::Shared);
    auto allocation = texture->allocate(flags);

    mach_port_t mach_port = allocation->machPort;
    if (!mach_port) {
      ERR("DeviceTexture: Failed to get mach port for shared texture");
      return E_FAIL;
    }
    D3DKMT_HANDLE keyedMutex = 0;
    D3DKMT_HANDLE syncObject = 0;
    D3DKMT_HANDLE keyedMutexGlobal = 0;
    D3DKMT_HANDLE syncObjectGlobal = 0;
    WMT::Reference<WMT::SharedEvent> keyedMutexEvent;
    bool hasKeyedMutex = finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    bool ntSecuritySharing = finalDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    NTSTATUS status = 0;

    if (hasKeyedMutex) {
      D3DKMT_CREATEKEYEDMUTEX2 createMutex = {};
      createMutex.Flags.NtSecuritySharing = ntSecuritySharing;
      status = D3DKMTCreateKeyedMutex2(&createMutex);
      if (status) {
        ERR("DeviceTexture: Failed to create keyed mutex", " status=", status);
        return E_FAIL;
      }
      keyedMutex = createMutex.hKeyedMutex;
      keyedMutexGlobal = createMutex.hSharedHandle;

      D3DKMT_CREATESYNCHRONIZATIONOBJECT2 createSync = {};
      createSync.hDevice = local_kmt;
      createSync.Info.Type = D3DDDI_SYNCHRONIZATION_MUTEX;
      createSync.Info.Flags.Shared = 1;
      createSync.Info.Flags.NtSecuritySharing = ntSecuritySharing;
      status = D3DKMTCreateSynchronizationObject2(&createSync);
      if (status) {
        ERR("DeviceTexture: Failed to create keyed mutex sync object", " status=", status);
        DestroyD3DKMTKeyedMutex(keyedMutex);
        return E_FAIL;
      }
      syncObject = createSync.hSyncObject;
      syncObjectGlobal = createSync.Info.SharedHandle;

      keyedMutexEvent = pDevice->GetMTLDevice().newSharedEvent();
      if (!keyedMutexEvent) {
        ERR("DeviceTexture: Failed to create keyed mutex shared event");
        DestroyD3DKMTKeyedMutex(keyedMutex);
        DestroyD3DKMTSyncObject(syncObject);
        return E_FAIL;
      }
      keyedMutexEvent.signalValue(0);
    }

    SharedResourceData runtimeData = {};
    FillWineSharedResourceDesc(runtimeData.wine_desc, finalDesc, keyedMutexGlobal, syncObjectGlobal, ntSecuritySharing);
    MakeUniqueSharedName(runtimeData.mach_port_name);
    if (!WMTBootstrapRegister(runtimeData.mach_port_name, mach_port)) {
      ERR("DeviceTexture: Failed to register mach port for shared texture");
      DestroyD3DKMTKeyedMutex(keyedMutex);
      DestroyD3DKMTSyncObject(syncObject);
      return E_FAIL;
    }
    if (keyedMutexEvent) {
      auto eventMachPort = keyedMutexEvent.createMachPort();
      if (!eventMachPort) {
        ERR("DeviceTexture: Failed to get mach port for keyed mutex shared event");
        DestroyD3DKMTKeyedMutex(keyedMutex);
        DestroyD3DKMTSyncObject(syncObject);
        return E_FAIL;
      }
      MakeUniqueSharedName(runtimeData.keyed_mutex_event_name);
      if (!WMTBootstrapRegister(runtimeData.keyed_mutex_event_name, eventMachPort)) {
        ERR("DeviceTexture: Failed to register keyed mutex shared event");
        DestroyD3DKMTKeyedMutex(keyedMutex);
        DestroyD3DKMTSyncObject(syncObject);
        return E_FAIL;
      }
    }

    D3DKMT_CREATEALLOCATION create = {};
    create.hDevice = local_kmt;
    create.pPrivateRuntimeData = &runtimeData;
    create.PrivateRuntimeDataSize = sizeof(runtimeData);
    create.Flags.StandardAllocation = 1;
    create.NumAllocations = 1;
    D3DDDI_ALLOCATIONINFO allocationInfo = {};
    create.pAllocationInfo = &allocationInfo;
    D3DKMT_CREATESTANDARDALLOCATION standardAllocation = {};
    create.pStandardAllocation = &standardAllocation;
    standardAllocation.Type = D3DKMT_STANDARDALLOCATIONTYPE_EXISTINGHEAP;
    standardAllocation.ExistingHeapData.Size = CalculateD3DKMTExistingHeapSize(pDevice, finalDesc);
    create.Flags.ExistingSysMem = 1;
    alignas(kD3DKMTExistingHeapPageSize) static const char systemMem[kD3DKMTExistingHeapPageSize] = {};
    allocationInfo.pSystemMem = systemMem;
    create.Flags.CreateResource = 1;
    create.Flags.CreateShared = 1;
    create.Flags.NtSecuritySharing = ntSecuritySharing;
    status = D3DKMTCreateAllocation2(&create);
    if (status) {
      ERR(
          "DeviceTexture: Failed to create D3DKMT for shared texture",
          " status=", status,
          " hDevice=", create.hDevice,
          " hResource=", create.hResource,
          " hGlobalShare=", create.hGlobalShare,
          " nt_security=", create.Flags.NtSecuritySharing,
          " existing_heap_size=", standardAllocation.ExistingHeapData.Size,
          " allocation_private_size=", allocationInfo.PrivateDriverDataSize
      );
      DestroyD3DKMTKeyedMutex(keyedMutex);
      DestroyD3DKMTSyncObject(syncObject);
      return E_FAIL;
    }

    initialize(std::move(allocation));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), create.hResource,
                                   create.hGlobalShare, pDevice, keyedMutex, syncObject,
                                   keyedMutexGlobal, syncObjectGlobal, std::move(keyedMutexEvent)))
    );
    return S_OK;
  }

  Flags<TextureAllocationFlag> flags;
  flags.set(finalDesc.CPUAccessFlags ? TextureAllocationFlag::GpuManaged : TextureAllocationFlag::GpuPrivate);
  if (!(finalDesc.BindFlags & (D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)))
    flags.set(TextureAllocationFlag::ShaderReadonly);
  if (finalDesc.Usage == D3D11_USAGE_IMMUTABLE)
    flags.set(TextureAllocationFlag::GpuReadonly);
  if (single_subresource && (finalDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
    Rc<RenamableTexturePool> renamable = new RenamableTexturePool(texture.ptr(), 32, flags);
    initialize(renamable->getNext(0));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), std::move(renamable), pDevice)));
  } else {
    initialize(texture->allocate(flags));
    *ppTexture = reinterpret_cast<typename tag::COM_IMPL *>(
        ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), pDevice)));
  }
  return S_OK;
}

HRESULT
CreateDeviceTexture1D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE1D_DESC *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture1D **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_1d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture2D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE2D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture2D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_2d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

HRESULT
CreateDeviceTexture3D(MTLD3D11Device *pDevice,
                      const D3D11_TEXTURE3D_DESC1 *pDesc,
                      const D3D11_SUBRESOURCE_DATA *pInitialData,
                      ID3D11Texture3D1 **ppTexture) {
  return CreateDeviceTextureInternal<tag_texture_3d>(pDevice, pDesc,
                                                     pInitialData, ppTexture);
}

template <typename tag>
HRESULT
ImportSharedTextureInternal(
    MTLD3D11Device *pDevice, const typename tag::DESC1 *pDescUnchecked, mach_port_t MachPort, REFIID riid,
    void **ppTexture, D3DKMT_HANDLE keyedMutex = 0, D3DKMT_HANDLE syncObject = 0,
    WMT::Reference<WMT::SharedEvent> keyedMutexEvent = {}
) {
  WMTTextureInfo info = {};
  typename tag::DESC1 finalDesc;
  if (FAILED(CreateMTLTextureDescriptor(pDevice, pDescUnchecked, &finalDesc, &info))) {
    DestroyD3DKMTKeyedMutex(keyedMutex);
    DestroyD3DKMTSyncObject(syncObject);
    return E_INVALIDARG;
  }

  auto texture = Rc<Texture>(new Texture(info, pDevice->GetMTLDevice()));
  auto allocation = texture->import(MachPort);
  if (!allocation) {
    DestroyD3DKMTKeyedMutex(keyedMutex);
    DestroyD3DKMTSyncObject(syncObject);
    return E_FAIL;
  }
  texture->rename(std::move(allocation));

  Com<DeviceTexture<tag>> device_texture = Com<DeviceTexture<tag>>::transfer(
      ref(new DeviceTexture<tag>(&finalDesc, std::move(texture), 0, 0, pDevice, keyedMutex, syncObject, 0, 0,
                                 std::move(keyedMutexEvent)))
  );
  HRESULT hr = device_texture->QueryInterface(riid, ppTexture);
  if (FAILED(hr)) {
    device_texture->DetachKeyedMutexHandles();
    DestroyD3DKMTKeyedMutex(keyedMutex);
    DestroyD3DKMTSyncObject(syncObject);
  }
  return hr;
}

HRESULT
ImportSharedTexture(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (!(reinterpret_cast<uintptr_t>(hResource) & 0xc0000000)) {
    WARN("ImportSharedTexture: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  struct SharedResourceData runtimeData = {};

  D3DKMT_QUERYRESOURCEINFO query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  if (D3DKMTQueryResourceInfo(&query)) {
    WARN("ImportSharedTexture: Failed to query resource: ", hResource);
    return E_INVALIDARG;
  }

  if (!IsValidSharedResourceDataSize(query.PrivateRuntimeDataSize)) {
    WARN("ImportSharedTexture: Unexpected size: ", query.PrivateRuntimeDataSize);
    return E_INVALIDARG;
  } 

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCE open = {};
  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hGlobalShare = reinterpret_cast<uintptr_t>(hResource);
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;

  if (D3DKMTOpenResource2(&open)) {
    WARN("ImportSharedTexture: Failed to open resource: ", hResource);
    return E_INVALIDARG;
  }

  D3DKMT_DESTROYALLOCATION destroy = {};
  destroy.hDevice = pDevice->GetLocalD3DKMT();
  destroy.hResource = open.hResource;
  D3DKMTDestroyAllocation(&destroy);

  SharedResourceInfo runtimeInfo;
  if (!NormalizeSharedResourceData(runtimeData, open.PrivateRuntimeDataSize, runtimeInfo)) {
    WARN("ImportSharedTexture: Unsupported runtime data");
    return E_INVALIDARG;
  }

  D3DKMT_HANDLE keyedMutex = 0;
  D3DKMT_HANDLE syncObject = 0;
  WMT::Reference<WMT::SharedEvent> keyedMutexEvent;
  switch (runtimeInfo.dimension)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    if (runtimeInfo.desc.desc1d.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX &&
        FAILED(OpenSharedKeyedMutexHandles(runtimeInfo, keyedMutex, syncObject)))
      return E_INVALIDARG;
    break;
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    if (runtimeInfo.desc.desc2d.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX &&
        FAILED(OpenSharedKeyedMutexHandles(runtimeInfo, keyedMutex, syncObject)))
      return E_INVALIDARG;
    break;
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    if (runtimeInfo.desc.desc3d.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX &&
        FAILED(OpenSharedKeyedMutexHandles(runtimeInfo, keyedMutex, syncObject)))
      return E_INVALIDARG;
    break;
  default:
    break;
  }

  if (keyedMutex && runtimeInfo.has_keyed_mutex_event)
    keyedMutexEvent = OpenSharedKeyedMutexEvent(pDevice, runtimeInfo);
  else if (keyedMutex)
    WARN("ImportSharedTexture: keyed resource has no shared GPU event in runtime data");

  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeInfo.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    DestroyD3DKMTKeyedMutex(keyedMutex);
    DestroyD3DKMTSyncObject(syncObject);
    return E_INVALIDARG;
  }

  switch (runtimeInfo.dimension)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    return ImportSharedTextureInternal<tag_texture_1d>(
        pDevice, &runtimeInfo.desc.desc1d, mach_port, riid, ppTexture, keyedMutex, syncObject,
        std::move(keyedMutexEvent)
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    return ImportSharedTextureInternal<tag_texture_2d>(
        pDevice, &runtimeInfo.desc.desc2d, mach_port, riid, ppTexture, keyedMutex, syncObject,
        std::move(keyedMutexEvent)
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    return ImportSharedTextureInternal<tag_texture_3d>(
        pDevice, &runtimeInfo.desc.desc3d, mach_port, riid, ppTexture, keyedMutex, syncObject,
        std::move(keyedMutexEvent)
    );
  default:
    ERR("ImportSharedTexture: Unsupported resource dimension");
    DestroyD3DKMTKeyedMutex(keyedMutex);
    DestroyD3DKMTSyncObject(syncObject);
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTextureFromNtHandle(MTLD3D11Device *pDevice, HANDLE hResource, REFIID riid, void **ppTexture) {
  InitReturnPtr(ppTexture);

  if (reinterpret_cast<uintptr_t>(hResource) & 0xc0000000) {
    WARN("ImportSharedTextureFromNtHandle: Invalid shared handle type");
    return E_INVALIDARG;
  }

  if (ppTexture == nullptr)
    return S_FALSE;

  struct SharedResourceData runtimeData = {};

  D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE query = {};
  query.hDevice = pDevice->GetLocalD3DKMT();
  query.hNtHandle = hResource;
  query.pPrivateRuntimeData = &runtimeData;
  query.PrivateRuntimeDataSize = sizeof(runtimeData);

  if (D3DKMTQueryResourceInfoFromNtHandle(&query)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to query resource: ", hResource));
    return E_INVALIDARG;
  }
  
  if (!IsValidSharedResourceDataSize(query.PrivateRuntimeDataSize)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Unexpected size: ", query.PrivateRuntimeDataSize));
    return E_INVALIDARG;
  }

  D3DDDI_OPENALLOCATIONINFO2 alloc = {};
  D3DKMT_OPENRESOURCEFROMNTHANDLE open = {};
  char dummy;

  open.hDevice = pDevice->GetLocalD3DKMT();
  open.hNtHandle = hResource;
  open.NumAllocations = 1;
  open.pOpenAllocationInfo2 = &alloc;
  open.pPrivateRuntimeData = &runtimeData;
  open.PrivateRuntimeDataSize = query.PrivateRuntimeDataSize;
  open.pTotalPrivateDriverDataBuffer = &dummy;
  open.TotalPrivateDriverDataBufferSize = 0;

  if (D3DKMTOpenResourceFromNtHandle(&open)) {
    WARN(str::format("ImportSharedTextureFromNtHandle: Failed to open resource: ", hResource));
    return E_INVALIDARG;
  }

  D3DKMT_DESTROYALLOCATION destroy = {};
  destroy.hDevice = pDevice->GetLocalD3DKMT();
  destroy.hResource = open.hResource;
  D3DKMTDestroyAllocation(&destroy);

  SharedResourceInfo runtimeInfo;
  if (!NormalizeSharedResourceData(runtimeData, open.PrivateRuntimeDataSize, runtimeInfo)) {
    WARN("ImportSharedTextureFromNtHandle: Unsupported runtime data");
    DestroyD3DKMTKeyedMutex(open.hKeyedMutex);
    DestroyD3DKMTSyncObject(open.hSyncObject);
    return E_INVALIDARG;
  }

  mach_port_t mach_port;
  if (!WMTBootstrapLookUp(runtimeInfo.mach_port_name, &mach_port)) {
    ERR("ImportSharedTexture: Failed to look up mach port");
    DestroyD3DKMTKeyedMutex(open.hKeyedMutex);
    DestroyD3DKMTSyncObject(open.hSyncObject);
    return E_INVALIDARG;
  }

  WMT::Reference<WMT::SharedEvent> keyedMutexEvent;
  if (open.hKeyedMutex && runtimeInfo.has_keyed_mutex_event)
    keyedMutexEvent = OpenSharedKeyedMutexEvent(pDevice, runtimeInfo);
  else if (open.hKeyedMutex)
    WARN("ImportSharedTextureFromNtHandle: keyed resource has no shared GPU event in runtime data");

  switch (runtimeInfo.dimension)
  {
  case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
    if ((runtimeInfo.desc.desc1d.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) && !open.hKeyedMutex) {
      WARN("ImportSharedTextureFromNtHandle: Missing bundled keyed mutex");
      DestroyD3DKMTSyncObject(open.hSyncObject);
      return E_INVALIDARG;
    }
    return ImportSharedTextureInternal<tag_texture_1d>(
        pDevice, &runtimeInfo.desc.desc1d, mach_port, riid, ppTexture, open.hKeyedMutex, open.hSyncObject,
        std::move(keyedMutexEvent)
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
    if ((runtimeInfo.desc.desc2d.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) && !open.hKeyedMutex) {
      WARN("ImportSharedTextureFromNtHandle: Missing bundled keyed mutex");
      DestroyD3DKMTSyncObject(open.hSyncObject);
      return E_INVALIDARG;
    }
    return ImportSharedTextureInternal<tag_texture_2d>(
        pDevice, &runtimeInfo.desc.desc2d, mach_port, riid, ppTexture, open.hKeyedMutex, open.hSyncObject,
        std::move(keyedMutexEvent)
    );
  case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
    if ((runtimeInfo.desc.desc3d.MiscFlags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) && !open.hKeyedMutex) {
      WARN("ImportSharedTextureFromNtHandle: Missing bundled keyed mutex");
      DestroyD3DKMTSyncObject(open.hSyncObject);
      return E_INVALIDARG;
    }
    return ImportSharedTextureInternal<tag_texture_3d>(
        pDevice, &runtimeInfo.desc.desc3d, mach_port, riid, ppTexture, open.hKeyedMutex, open.hSyncObject,
        std::move(keyedMutexEvent)
    );
  default:
    ERR("ImportSharedTexture: Unsupported resource dimension");
    DestroyD3DKMTKeyedMutex(open.hKeyedMutex);
    DestroyD3DKMTSyncObject(open.hSyncObject);
    return E_INVALIDARG;
  }
}

HRESULT
ImportSharedTextureByName(
    MTLD3D11Device *pDevice, LPCWSTR lpName, DWORD dwDesiredAccess, REFIID riid, void **ppTexture
) {
  D3DKMT_OPENNTHANDLEFROMNAME openFromName = {};
  openFromName.dwDesiredAccess = dwDesiredAccess;

  OBJECT_ATTRIBUTES attr = {};
  attr.Length = sizeof(attr);

  WCHAR buffer[MAX_PATH];
  UNICODE_STRING name_str;
  DWORD session, len, name_len = wcslen(lpName);

  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  len = swprintf(buffer, ARRAYSIZE(buffer), L"\\Sessions\\%u\\BaseNamedObjects\\", session);
  memcpy(buffer + len, lpName, (name_len + 1) * sizeof(WCHAR));
  name_str.MaximumLength = name_str.Length = (len + name_len) * sizeof(WCHAR);
  name_str.MaximumLength += sizeof(WCHAR);
  name_str.Buffer = buffer;

  attr.ObjectName = &name_str;
  attr.Attributes = OBJ_CASE_INSENSITIVE;
  openFromName.pObjAttrib = &attr;

  if (D3DKMTOpenNtHandleFromName(&openFromName)) {
    WARN(str::format("ImportSharedTextureByName: Failed to open NT handle from name: ", lpName));
    return E_INVALIDARG;
  }

  HRESULT res = ImportSharedTextureFromNtHandle(pDevice, openFromName.hNtHandle, riid, ppTexture);
  CloseHandle(openFromName.hNtHandle);
  return res;
}

#pragma endregion

} // namespace dxmt
