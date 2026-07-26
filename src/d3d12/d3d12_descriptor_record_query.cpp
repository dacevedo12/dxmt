#include "d3d12_descriptor_record_query.hpp"

#include "d3d12_queue_replay_helpers.hpp"

namespace dxmt::d3d12 {

uint32_t
DescriptorViewDimension(const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc)
    return 0;
  if (descriptor.type == DescriptorRecordType::ShaderResourceView)
    return uint32_t(descriptor.desc.srv.ViewDimension);
  if (descriptor.type == DescriptorRecordType::UnorderedAccessView)
    return uint32_t(descriptor.desc.uav.ViewDimension);
  return 0;
}

bool
ShaderResourceViewIsMultisampledTexture(const DescriptorRecord &descriptor) {
  if (descriptor.type != DescriptorRecordType::ShaderResourceView ||
      !descriptor.resource)
    return false;

  const auto *resource = GetResource(descriptor.resource.ptr());
  if (!resource ||
      resource->GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return false;

  if (!descriptor.has_desc)
    return resource->GetResourceDesc().SampleDesc.Count > 1;

  const auto &srv = descriptor.desc.srv;
  return srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMS ||
         srv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
}

void
SetPixelShaderMsaaSrvDemoteBit(uint64_t &mask_lo, uint64_t &mask_hi,
                               UINT binding_slot) {
  if (binding_slot < kPixelShaderMsaaSrvDemoteMaskBits)
    mask_lo |= uint64_t(1) << binding_slot;
  else if (binding_slot < 2 * kPixelShaderMsaaSrvDemoteMaskBits)
    mask_hi |= uint64_t(1)
               << (binding_slot - kPixelShaderMsaaSrvDemoteMaskBits);
}

} // namespace dxmt::d3d12
