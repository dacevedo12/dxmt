#pragma once

#include "com/com_pointer.hpp"
#include "d3d12_device.hpp"
#include <d3d12.h>
#include <cstddef>
#include <span>
#include <vector>

namespace dxmt::d3d12 {

struct RootSignatureRange {
  D3D12_DESCRIPTOR_RANGE_TYPE range_type = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  UINT base_shader_register = 0;
  UINT register_space = 0;
  UINT descriptor_count = 0;
  UINT offset_in_descriptors_from_table_start = 0;
  D3D12_DESCRIPTOR_RANGE_FLAGS flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
};

struct RootSignatureParameter {
  D3D12_ROOT_PARAMETER_TYPE parameter_type = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
  std::vector<RootSignatureRange> ranges;
  D3D12_ROOT_DESCRIPTOR1 descriptor = {};
  D3D12_ROOT_CONSTANTS constants = {};
  D3D12_ROOT_DESCRIPTOR_FLAGS descriptor_flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
};

class RootSignature {
public:
  virtual ~RootSignature() = default;

  virtual ULONG STDMETHODCALLTYPE AddRefPrivate() = 0;
  virtual void STDMETHODCALLTYPE ReleasePrivate() = 0;
  virtual const D3D12_VERSIONED_ROOT_SIGNATURE_DESC &GetVersionedDesc() const = 0;
  virtual std::span<const std::byte> GetSerializedBlob() const = 0;
  virtual std::span<const RootSignatureParameter> GetParameters() const = 0;
  virtual std::span<const D3D12_STATIC_SAMPLER_DESC> GetStaticSamplers() const = 0;
};

Com<ID3D12RootSignature> CreateRootSignatureFromBlob(IMTLD3D12Device *device,
                                                      std::span<const std::byte> blob);

HRESULT CreateRootSignatureDeserializer(std::span<const std::byte> blob,
                                         REFIID iid, void **deserializer);

HRESULT CreateRootSignatureDeserializerFromSubobjectInLibrary(
    std::span<const std::byte> library_blob, const WCHAR *subobject_name,
    REFIID iid, void **deserializer);

HRESULT SerializeVersionedRootSignature(
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *root_signature_desc,
    ID3DBlob **blob, ID3DBlob **error_blob);

} // namespace dxmt::d3d12
