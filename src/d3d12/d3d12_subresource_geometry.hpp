#pragma once

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_resource.hpp"

#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// A contiguous run of D3D12 subresource indices.
struct SubresourceRange {
  UINT first = 0;
  UINT count = 0;
};

// --- resource geometry -----------------------------------------------------

// Plane count of the resource format (always 1 for buffers).
[[nodiscard]] UINT GetPlaneCount(const Resource &resource);

// Full mip chain length implied by a resource description.
[[nodiscard]] UINT GetFullMipLevelCount(const D3D12_RESOURCE_DESC &desc);

// Mip level count of the resource, resolving MipLevels==0 to the full chain.
[[nodiscard]] UINT GetResourceMipLevelCount(const Resource &resource);

// Array slice count of the resource (1 for buffers and 3D textures).
[[nodiscard]] UINT GetResourceArraySliceCount(const Resource &resource);

// Total subresource count across mips, array slices and planes.
[[nodiscard]] UINT GetSubresourceCount(const Resource &resource);

// Depth of a 3D texture mip (1 for every other dimension).
[[nodiscard]] UINT GetMipDepth(const Resource &resource, UINT mip_slice);

// --- subresource index decomposition ---------------------------------------

// Plane index of a subresource.
[[nodiscard]] UINT GetSubresourcePlane(const Resource &resource,
                                       UINT subresource);

// Subresource index with the plane component removed.
[[nodiscard]] UINT GetSubresourceIndex(const Resource &resource,
                                       UINT subresource);

// Mip level of a subresource.
[[nodiscard]] UINT GetMipLevel(const Resource &resource, UINT subresource);

// Array slice of a subresource (0 for 3D textures).
[[nodiscard]] UINT GetArraySlice(const Resource &resource, UINT subresource);

// Composes a subresource index from its components.
[[nodiscard]] UINT MakeSubresourceIndex(const Resource &resource, UINT mip,
                                        UINT array_slice, UINT plane);

// Clamps a view's requested count against the resource total; UINT_MAX and 0
// mean "all remaining".
[[nodiscard]] UINT NormalizeViewCount(UINT requested, UINT first, UINT total);

// Texel extent of a subresource, or the extent of `box` clipped to that
// subresource when supplied. `box` comes straight from the application, so it
// is clamped rather than trusted: a box that is empty or inverted on any axis
// (D3D12 defines both as "no copy") yields a zero extent, and every non-zero
// result satisfies box origin + extent <= subresource extent. Callers must
// treat a zero component as "encode nothing".
[[nodiscard]] WMTSize GetSubresourceSize(const Resource &resource,
                                         UINT subresource,
                                         const D3D12_BOX *box);

// Validates that (subresource, plane, level, slice) addresses a real Metal
// texture subresource, warning with `op` as context when it does not.
[[nodiscard]] bool ValidateTextureSubresourceAccess(const char *op,
                                                    const Resource &resource,
                                                    dxmt::Texture *texture,
                                                    UINT subresource,
                                                    UINT plane, UINT level,
                                                    UINT slice);

// --- subresource range accumulation ----------------------------------------

// Appends one range per array slice for the requested mip/slice/plane window.
// Returns false when the window does not fit the resource.
[[nodiscard]] bool
AppendTextureSubresourceRanges(const Resource &resource, UINT first_mip,
                               UINT mip_count, UINT first_slice,
                               UINT slice_count, UINT plane,
                               std::vector<SubresourceRange> &ranges);

// Appends a single range covering every subresource of the resource.
[[nodiscard]] bool
AppendAllSubresourcesRange(const Resource &resource,
                           std::vector<SubresourceRange> &ranges);

[[nodiscard]] bool AppendDefaultRenderTargetSubresourceRanges(
    const Resource &resource, std::vector<SubresourceRange> &ranges);

[[nodiscard]] bool AppendDefaultUnorderedAccessSubresourceRanges(
    const Resource &resource, std::vector<SubresourceRange> &ranges);

[[nodiscard]] bool AppendDefaultDepthStencilSubresourceRanges(
    const Resource &resource, std::vector<SubresourceRange> &ranges);

// --- descriptor driven ranges ----------------------------------------------

[[nodiscard]] UINT
GetRenderTargetArrayLength(Resource &resource,
                           const DescriptorRecord &descriptor);

[[nodiscard]] UINT
GetRenderTargetDepthPlane(const DescriptorRecord &descriptor);

// True when the view covers the whole 2D single-sample texture, i.e. it can be
// used as a present source.
[[nodiscard]] bool IsPresentableRenderTargetView(Resource &resource,
                                                 TextureViewKey view);

void TrackPresentSourceRenderTargetView(Resource &resource,
                                        TextureViewKey view);

[[nodiscard]] UINT
GetDepthStencilArrayLength(Resource &resource,
                           const DescriptorRecord &descriptor);

[[nodiscard]] bool
GetRenderTargetSubresourceRanges(Resource &resource,
                                 const DescriptorRecord &descriptor,
                                 std::vector<SubresourceRange> &ranges);

[[nodiscard]] bool
GetDepthStencilSubresourceRanges(Resource &resource,
                                 const DescriptorRecord &descriptor,
                                 std::vector<SubresourceRange> &ranges);

[[nodiscard]] bool
GetShaderResourceSubresourceRanges(Resource &resource,
                                   const DescriptorRecord &descriptor,
                                   std::vector<SubresourceRange> &ranges);

[[nodiscard]] bool
GetUnorderedAccessSubresourceRanges(Resource &resource,
                                    const DescriptorRecord &descriptor,
                                    std::vector<SubresourceRange> &ranges);

} // namespace dxmt::d3d12
