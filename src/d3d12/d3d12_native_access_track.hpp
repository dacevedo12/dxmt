#pragma once

// Encoder residency/hazard tracking for one native-ABI descriptor.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_native_binding.inc). It stays a template because the
// encoder access helper is parameterized on the pipeline stage; the only piece
// of the command queue it ever used was the Metal device handle, which is now
// passed in.

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_legacy_binding_encode.hpp"
#include "dxmt_context.hpp"

#include <cstdint>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Declares the resource accesses `descriptor` implies for `Stage`. Sampler
// ranges touch no resource and are ignored.
template <PipelineStage Stage>
void TrackNativeDescriptorAccess(WMT::Device device,
                                 ArgumentEncodingContext &enc,
                                 const DescriptorRecord &descriptor,
                                 D3D12_DESCRIPTOR_RANGE_TYPE range_type,
                                 const DXMT12_MTL4_SHADER_ARGUMENT *argument) {
  switch (range_type) {
  case D3D12_DESCRIPTOR_RANGE_TYPE_CBV: {
    ConstantBufferBinding binding = {};
    if (!MakeConstantBufferBindingFromDescriptor(descriptor, binding) ||
        !binding.buffer)
      return;
    const auto length =
        descriptor.has_desc ? descriptor.desc.cbv.SizeInBytes : 0u;
    if (length)
      enc.access<Stage>(binding.buffer, binding.offset, length,
                        ResourceAccess::Read);
    return;
  }
  case D3D12_DESCRIPTOR_RANGE_TYPE_SRV: {
    ResourceViewBinding binding = {};
    if (!MakeShaderResourceBindingFromDescriptor(device, descriptor, argument,
                                                 binding))
      return;
    if (binding.buffer) {
      if (binding.viewId)
        enc.access<Stage>(binding.buffer, binding.viewId, ResourceAccess::Read);
      else
        enc.access<Stage>(binding.buffer, binding.slice.byteOffset,
                          binding.slice.byteLength, ResourceAccess::Read);
    } else if (binding.texture && binding.viewId) {
      enc.access<Stage>(binding.texture, binding.viewId, ResourceAccess::Read);
    }
    return;
  }
  case D3D12_DESCRIPTOR_RANGE_TYPE_UAV: {
    UnorderedAccessViewBinding binding = {};
    if (!MakeUnorderedAccessBindingFromDescriptor(device, descriptor, argument,
                                                  binding))
      return;
    bool read = argument && ((argument->Flags >> 10) & 1);
    bool write = argument && ((argument->Flags >> 10) & 2);
    if (!read && !write) {
      read = true;
      write = true;
    }
    const int flags = (read ? ResourceAccess::Read : 0) |
                      (write ? ResourceAccess::Write : 0) | ResourceAccess::UAV;
    if (binding.buffer) {
      if (binding.viewId)
        enc.access<Stage>(binding.buffer, binding.viewId, flags);
      else
        enc.access<Stage>(binding.buffer, binding.slice.byteOffset,
                          binding.slice.byteLength, flags);
    } else if (binding.texture && binding.viewId) {
      enc.access<Stage>(binding.texture, binding.viewId, flags);
    }
    if (binding.counter)
      enc.access<Stage>(binding.counter, 0, sizeof(uint32_t),
                        ResourceAccess::All);
    return;
  }
  case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER:
    return;
  }
}

} // namespace dxmt::d3d12
