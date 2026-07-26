#include "d3d12_binding_fingerprint.hpp"

namespace dxmt::d3d12 {

void
HashGraphicsBindingDescriptor(uint64_t &hash,
                              const DescriptorRecord &descriptor) {
  HashGraphicsBindingValue(hash, descriptor.type);
  HashGraphicsBindingValue(hash, descriptor.has_desc);
  HashGraphicsBindingPointer(hash, descriptor.resource.ptr());
  HashGraphicsBindingPointer(hash, descriptor.counter_resource.ptr());

  if (!descriptor.has_desc)
    return;

  switch (descriptor.type) {
  case DescriptorRecordType::ConstantBufferView:
    HashGraphicsBindingValue(hash, descriptor.desc.cbv);
    break;
  case DescriptorRecordType::ShaderResourceView:
    HashGraphicsBindingValue(hash, descriptor.desc.srv);
    break;
  case DescriptorRecordType::UnorderedAccessView:
    HashGraphicsBindingValue(hash, descriptor.desc.uav);
    break;
  case DescriptorRecordType::Sampler:
    HashGraphicsBindingValue(hash, descriptor.desc.sampler);
    break;
  default:
    break;
  }
}

} // namespace dxmt::d3d12
