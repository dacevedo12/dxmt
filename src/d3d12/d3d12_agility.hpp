#pragma once

#include <d3d12.h>

#ifndef __ID3D12SDKConfiguration1_INTERFACE_DEFINED__
MIDL_INTERFACE("8aaf9303-ad25-48b9-9a57-d9c37e009d9f")
ID3D12SDKConfiguration1 : public ID3D12SDKConfiguration {
  virtual HRESULT STDMETHODCALLTYPE CreateDeviceFactory(UINT sdk_version,
                                                        LPCSTR sdk_path,
                                                        REFIID riid,
                                                        void **factory) = 0;
  virtual void STDMETHODCALLTYPE FreeUnusedSDKs() = 0;
};
__CRT_UUID_DECL(ID3D12SDKConfiguration1, 0x8aaf9303, 0xad25, 0x48b9,
                0x9a, 0x57, 0xd9, 0xc3, 0x7e, 0x00, 0x9d, 0x9f)
#endif

#ifndef __ID3D12DeviceFactory_INTERFACE_DEFINED__
typedef enum D3D12_DEVICE_FACTORY_FLAGS {
  D3D12_DEVICE_FACTORY_FLAG_NONE = 0,
  D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_EXISTING_DEVICE = 0x1,
  D3D12_DEVICE_FACTORY_FLAG_ALLOW_RETURNING_INCOMPATIBLE_EXISTING_DEVICE = 0x2,
  D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON = 0x4,
} D3D12_DEVICE_FACTORY_FLAGS;
MIDL_INTERFACE("61f307d3-d34e-4e7c-8374-3ba4de23cccb")
ID3D12DeviceFactory : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE InitializeFromGlobalState() = 0;
  virtual HRESULT STDMETHODCALLTYPE ApplyToGlobalState() = 0;
  virtual HRESULT STDMETHODCALLTYPE SetFlags(D3D12_DEVICE_FACTORY_FLAGS flags) = 0;
  virtual D3D12_DEVICE_FACTORY_FLAGS STDMETHODCALLTYPE GetFlags() = 0;
  virtual HRESULT STDMETHODCALLTYPE GetConfigurationInterface(REFCLSID clsid,
                                                              REFIID riid,
                                                              void **object) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnableExperimentalFeatures(UINT feature_count,
                                                               const IID *iids,
                                                               void *configs,
                                                               UINT *config_sizes) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateDevice(IUnknown *adapter,
                                                 D3D_FEATURE_LEVEL feature_level,
                                                 REFIID riid,
                                                 void **device) = 0;
};
__CRT_UUID_DECL(ID3D12DeviceFactory, 0x61f307d3, 0xd34e, 0x4e7c, 0x83,
                0x74, 0x3b, 0xa4, 0xde, 0x23, 0xcc, 0xcb)
#endif

#ifndef __ID3D12DeviceConfiguration_INTERFACE_DEFINED__
typedef enum D3D12_DEVICE_FLAGS {
  D3D12_DEVICE_FLAG_NONE = 0,
  D3D12_DEVICE_FLAG_DEBUG_LAYER_ENABLED = 0x1,
} D3D12_DEVICE_FLAGS;

typedef struct D3D12_DEVICE_CONFIGURATION_DESC {
  D3D12_DEVICE_FLAGS Flags;
  UINT GpuBasedValidationFlags;
  UINT SDKVersion;
  UINT NumEnabledExperimentalFeatures;
} D3D12_DEVICE_CONFIGURATION_DESC;

MIDL_INTERFACE("78dbf87b-f766-422b-a61c-c8c446bdb9ad")
ID3D12DeviceConfiguration : public IUnknown {
#ifdef WIDL_EXPLICIT_AGGREGATE_RETURNS
  virtual D3D12_DEVICE_CONFIGURATION_DESC *STDMETHODCALLTYPE
  GetDesc(D3D12_DEVICE_CONFIGURATION_DESC *__ret) = 0;
  D3D12_DEVICE_CONFIGURATION_DESC STDMETHODCALLTYPE GetDesc() {
    D3D12_DEVICE_CONFIGURATION_DESC ret;
    return *GetDesc(&ret);
  }
#else
  virtual D3D12_DEVICE_CONFIGURATION_DESC STDMETHODCALLTYPE GetDesc() = 0;
#endif
  virtual HRESULT STDMETHODCALLTYPE GetEnabledExperimentalFeatures(GUID *guids,
                                                                   UINT guid_count) = 0;
  virtual HRESULT STDMETHODCALLTYPE SerializeVersionedRootSignature(
      const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob,
      ID3DBlob **error_blob) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateVersionedRootSignatureDeserializer(
      const void *blob, SIZE_T size, REFIID riid, void **deserializer) = 0;
};
__CRT_UUID_DECL(ID3D12DeviceConfiguration, 0x78dbf87b, 0xf766, 0x422b,
                0xa6, 0x1c, 0xc8, 0xc4, 0x46, 0xbd, 0xb9, 0xad)
#endif

#ifndef __ID3D12DeviceConfiguration1_INTERFACE_DEFINED__
MIDL_INTERFACE("ed342442-6343-4e16-bb82-a3a577874e56")
ID3D12DeviceConfiguration1 : public ID3D12DeviceConfiguration {
  virtual HRESULT STDMETHODCALLTYPE
  CreateVersionedRootSignatureDeserializerFromSubobjectInLibrary(
      const void *library_blob, SIZE_T size, LPCWSTR root_signature_subobject_name,
      REFIID riid, void **deserializer) = 0;
};
__CRT_UUID_DECL(ID3D12DeviceConfiguration1, 0xed342442, 0x6343, 0x4e16,
                0xbb, 0x82, 0xa3, 0xa5, 0x77, 0x87, 0x4e, 0x56)
#endif

#ifndef D3D_ROOT_SIGNATURE_VERSION_1_2
#define D3D_ROOT_SIGNATURE_VERSION_1_2 static_cast<D3D_ROOT_SIGNATURE_VERSION>(0x3)
#endif

namespace dxmt::d3d12 {

static constexpr GUID kCLSID_D3D12SDKConfiguration = {
    0x7cda6aca, 0xa03e, 0x49c8, {0x94, 0x58, 0x03, 0x34, 0xd2, 0x0e, 0x07, 0xce}};
static constexpr GUID kCLSID_D3D12DeviceFactory = {
    0x114863bf, 0xc386, 0x4aee, {0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55}};
static constexpr GUID kCLSID_D3D12DeviceRemovedExtendedData = {
    0x4a75bbc4, 0x9ff4, 0x4ad8, {0x9f, 0x18, 0xab, 0xae, 0x84, 0xdc, 0x5f, 0xf2}};

constexpr UINT kAgilitySdkVersion = 616;

} // namespace dxmt::d3d12
