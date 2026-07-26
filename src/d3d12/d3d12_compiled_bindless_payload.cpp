#include "d3d12_compiled_bindless_payload.hpp"

#include "d3d12_sampler.hpp"

namespace dxmt::d3d12 {

FrozenBindlessDescriptorPayload CaptureCompiledBindlessPayload(
    WMT::Device device, const DescriptorRecord &descriptor,
    const DXMT12_MTL4_SHADER_ARGUMENT &argument) {
  FrozenBindlessDescriptorPayload payload = {};
  if (argument.Type == SM50BindingType::Sampler) {
    payload.kind = FrozenBindlessDescriptorPayload::Kind::Sampler;
    payload.sampler = descriptor.materialized_sampler;
    if (!payload.sampler &&
        descriptor.type == DescriptorRecordType::Sampler &&
        descriptor.has_desc) {
      payload.sampler = CreateD3D12Sampler(device, descriptor.desc.sampler);
    }
    return payload;
  }
  if (argument.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE)
    payload.kind = FrozenBindlessDescriptorPayload::Kind::TextureDynamicPatch;
  return payload;
}

} // namespace dxmt::d3d12
