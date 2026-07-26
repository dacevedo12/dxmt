#pragma once

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_resource.hpp"

namespace dxmt::d3d12 {

[[nodiscard]] DXGI_FORMAT
D3D12DiagDescriptorFormat(const DescriptorRecord &descriptor) noexcept;

[[nodiscard]] const char *
DescriptorRecordTypeName(DescriptorRecordType type) noexcept;

void D3D12DiagLogTextureView(const char *kind, Resource &resource,
                             const DescriptorRecord &descriptor,
                             const TextureViewDescriptor &view,
                             TextureViewKey key);

void D3D12DiagLogDSVReplayDescriptor(
    const char *context, Resource &resource,
    const DescriptorRecord &descriptor, const TextureViewDescriptor &view,
    TextureViewKey key);

// Reports a descriptor whose subresource range the replay resource-state
// tracker cannot derive, so the access it covers goes untracked. Rate limited
// per process; unlike the view diagnostics above this is on unconditionally,
// because it marks a real hole in state tracking rather than a trace request.
void WarnDescriptorSubresourceRangeUnsupported(Resource &resource,
                                               const DescriptorRecord &descriptor,
                                               const char *context);

// As above, for a descriptor that resolved to an empty range.
void WarnDescriptorSubresourceRangeEmpty(Resource &resource,
                                         const DescriptorRecord &descriptor,
                                         const char *context);

} // namespace dxmt::d3d12
