#include "d3d12_subresource_geometry.hpp"

#include "dxmt_format.hpp"
#include "log/log.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

UINT
GetMipLevel(const Resource &resource, UINT subresource) {
  const UINT mip_levels = GetResourceMipLevelCount(resource);
  return mip_levels ? GetSubresourceIndex(resource, subresource) % mip_levels : 0;
}

UINT
GetArraySlice(const Resource &resource, UINT subresource) {
  const auto &desc = resource.GetResourceDesc();
  const UINT mip_levels = GetResourceMipLevelCount(resource);
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
             ? 0
             : GetSubresourceIndex(resource, subresource) / mip_levels;
}

UINT
GetSubresourceCount(const Resource &resource) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;
  return GetResourceMipLevelCount(resource) *
         GetResourceArraySliceCount(resource) *
         GetPlaneCount(resource);
}

UINT
GetPlaneCount(const Resource &resource) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  return traits.planeCount ? traits.planeCount : 1;
}

UINT
GetFullMipLevelCount(const D3D12_RESOURCE_DESC &desc) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;

  UINT64 width = std::max<UINT64>(desc.Width, 1);
  UINT height = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
                    ? 1
                    : std::max<UINT>(desc.Height, 1);
  UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
                   ? std::max<UINT>(desc.DepthOrArraySize, 1)
                   : 1;
  // The chain ends when the *largest* dimension reaches one, so halve that
  // single value rather than all three in lockstep: same count, and the
  // termination argument is visible to a reader and to the analyzer, which
  // could not see it through three interleaved std::max clamps.
  UINT64 extent = std::max({width, UINT64(height), UINT64(depth)});
  UINT levels = 1;
  while (extent > 1) {
    extent >>= 1;
    levels++;
  }
  return levels;
}

UINT
GetResourceMipLevelCount(const Resource &resource) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return 1;
  return desc.MipLevels ? desc.MipLevels : GetFullMipLevelCount(desc);
}

UINT
GetResourceArraySliceCount(const Resource &resource) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 1;
  return std::max<UINT>(desc.DepthOrArraySize, 1);
}

UINT
GetSubresourcePlane(const Resource &resource, UINT subresource) {
  const auto plane_count = GetPlaneCount(resource);
  if (plane_count <= 1)
    return 0;
  const UINT base_count = GetResourceMipLevelCount(resource) *
      GetResourceArraySliceCount(resource);
  return base_count ? subresource / base_count : 0;
}

UINT
GetSubresourceIndex(const Resource &resource, UINT subresource) {
  const UINT mip_levels = GetResourceMipLevelCount(resource);
  const UINT plane_count = GetPlaneCount(resource);
  const UINT base_count = mip_levels * GetResourceArraySliceCount(resource);
  if (plane_count <= 1)
    return subresource;
  return subresource % base_count;
}

UINT
MakeSubresourceIndex(const Resource &resource, UINT mip, UINT array_slice,
                     UINT plane) {
  const UINT mip_levels = GetResourceMipLevelCount(resource);
  const UINT array_size = GetResourceArraySliceCount(resource);
  const UINT base_count = mip_levels * array_size;
  return plane * base_count + array_slice * mip_levels + mip;
}

bool
AppendTextureSubresourceRanges(const Resource &resource, UINT first_mip,
                               UINT mip_count, UINT first_slice,
                               UINT slice_count, UINT plane,
                               std::vector<SubresourceRange> &ranges) {
  const UINT mip_levels = GetResourceMipLevelCount(resource);
  const UINT array_size = GetResourceArraySliceCount(resource);
  if (plane >= GetPlaneCount(resource) || first_mip >= mip_levels ||
      mip_count == 0 || mip_count > mip_levels - first_mip ||
      first_slice >= array_size || slice_count == 0 ||
      slice_count > array_size - first_slice)
    return false;

  for (UINT slice = first_slice; slice < first_slice + slice_count; ++slice) {
    ranges.push_back({MakeSubresourceIndex(resource, first_mip, slice, plane),
                      mip_count});
  }
  return true;
}

bool
AppendAllSubresourcesRange(const Resource &resource,
                           std::vector<SubresourceRange> &ranges) {
  const UINT count = GetSubresourceCount(resource);
  if (!count)
    return false;
  ranges.push_back({0, count});
  return true;
}

bool
AppendDefaultRenderTargetSubresourceRanges(
    const Resource &resource, std::vector<SubresourceRange> &ranges) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return AppendAllSubresourcesRange(resource, ranges);

  const UINT array_size = GetResourceArraySliceCount(resource);
  const UINT plane_count = GetPlaneCount(resource);
  if (!array_size || !plane_count)
    return false;

  for (UINT plane = 0; plane < plane_count; plane++) {
    if (!AppendTextureSubresourceRanges(resource, 0, 1, 0, array_size, plane,
                                        ranges))
      return false;
  }
  return true;
}

bool
AppendDefaultUnorderedAccessSubresourceRanges(
    const Resource &resource, std::vector<SubresourceRange> &ranges) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return AppendAllSubresourcesRange(resource, ranges);

  const UINT array_size = GetResourceArraySliceCount(resource);
  const UINT plane_count = GetPlaneCount(resource);
  if (!array_size || !plane_count)
    return false;

  for (UINT plane = 0; plane < plane_count; plane++) {
    if (!AppendTextureSubresourceRanges(resource, 0, 1, 0, array_size, plane,
                                        ranges))
      return false;
  }
  return true;
}

bool
AppendDefaultDepthStencilSubresourceRanges(
    const Resource &resource, std::vector<SubresourceRange> &ranges) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return AppendAllSubresourcesRange(resource, ranges);

  const UINT array_size = GetResourceArraySliceCount(resource);
  if (!array_size)
    return false;

  return AppendTextureSubresourceRanges(resource, 0, 1, 0, array_size, 0,
                                        ranges);
}

WMTSize
GetSubresourceSize(const Resource &resource, UINT subresource,
                   const D3D12_BOX *box) {
  const auto &desc = resource.GetResourceDesc();
  const auto mip = GetMipLevel(resource, subresource);
  const auto &traits = GetDXGIFormatTraits(desc.Format);
  const UINT plane = GetSubresourcePlane(resource, subresource);
  const UINT subsample_x =
      plane < traits.planeCount ? traits.planes[plane].subsampleXLog2 : 0;
  const UINT subsample_y =
      plane < traits.planeCount ? traits.planes[plane].subsampleYLog2 : 0;
  const WMTSize extent = {
      std::max<UINT64>(1, desc.Width >> (mip + subsample_x)),
      std::max<UINT64>(1, desc.Height >> (mip + subsample_y)),
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? std::max<UINT64>(1, desc.DepthOrArraySize >> mip)
          : 1};
  if (!box)
    return extent;

  // D3D12_BOX members are UINT and unvalidated at record time. An inverted box
  // would wrap the subtraction into a ~4e9 copy extent and a box larger than
  // the subresource would run the blit past its end, so clip both ends against
  // the subresource first. Clipping cannot shrink a valid box, and D3D12
  // defines an empty box as a no-op and an out-of-bounds box as undefined
  // (with "no copy" among the sanctioned outcomes).
  const UINT64 left = std::min<UINT64>(box->left, extent.width);
  const UINT64 right = std::min<UINT64>(box->right, extent.width);
  const UINT64 top = std::min<UINT64>(box->top, extent.height);
  const UINT64 bottom = std::min<UINT64>(box->bottom, extent.height);
  const UINT64 front = std::min<UINT64>(box->front, extent.depth);
  const UINT64 back = std::min<UINT64>(box->back, extent.depth);
  if (right <= left || bottom <= top || back <= front)
    return {0, 0, 0};
  return {right - left, bottom - top, back - front};
}

bool
ValidateTextureSubresourceAccess(const char *op, const Resource &resource,
                                 dxmt::Texture *texture, UINT subresource,
                                 UINT plane, UINT level, UINT slice) {
  if (!texture) {
    WARN("D3D12CommandQueue: ", op, " missing texture plane=", plane);
    return false;
  }
  if (subresource >= GetSubresourceCount(resource)) {
    WARN("D3D12CommandQueue: ", op, " subresource out of range subresource=",
         subresource, " count=", GetSubresourceCount(resource));
    return false;
  }
  if (plane >= GetPlaneCount(resource)) {
    WARN("D3D12CommandQueue: ", op, " plane out of range plane=", plane,
         " count=", GetPlaneCount(resource));
    return false;
  }
  if (level >= texture->miplevelCount()) {
    WARN("D3D12CommandQueue: ", op, " mip level out of range level=", level,
         " count=", texture->miplevelCount(), " subresource=", subresource,
         " plane=", plane);
    return false;
  }
  if (texture->textureType() != WMTTextureType3D &&
      slice >= texture->arrayLength()) {
    WARN("D3D12CommandQueue: ", op, " array slice out of range slice=", slice,
         " count=", texture->arrayLength(), " subresource=", subresource,
         " plane=", plane);
    return false;
  }
  return true;
}

UINT
GetRenderTargetArrayLength(Resource &resource,
                           const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc) {
    const auto &resource_desc = resource.GetResourceDesc();
    if (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D)
      return GetMipDepth(resource, 0);
    if (resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ||
        resource_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D)
      return resource_desc.DepthOrArraySize;
    return 1;
  }

  switch (descriptor.desc.rtv.ViewDimension) {
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    return descriptor.desc.rtv.Texture1DArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    return descriptor.desc.rtv.Texture2DArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    return descriptor.desc.rtv.Texture2DMSArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE3D: {
    const auto &texture3d = descriptor.desc.rtv.Texture3D;
    const UINT depth = GetMipDepth(resource, texture3d.MipSlice);
    return NormalizeViewCount(texture3d.WSize, texture3d.FirstWSlice, depth);
  }
  default:
    return 1;
  }
}

UINT
GetRenderTargetDepthPlane(const DescriptorRecord &descriptor) {
  return descriptor.has_desc && descriptor.desc.rtv.ViewDimension ==
                                    D3D12_RTV_DIMENSION_TEXTURE3D
             ? descriptor.desc.rtv.Texture3D.FirstWSlice
             : 0;
}

bool
IsPresentableRenderTargetView(Resource &resource, TextureViewKey view) {
  auto *texture = resource.GetTexture();
  if (!texture || !uint64_t(view))
    return false;

  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
      desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1)
    return false;

  if (texture->textureType(view) != WMTTextureType2D)
    return false;

  return view.mip_start == 0 && view.mip_end == 1 &&
         view.array_start == 0 && view.array_end == 1 &&
         texture->width(view) == texture->width() &&
         texture->height(view) == texture->height();
}

void
TrackPresentSourceRenderTargetView(Resource &resource, TextureViewKey view) {
  if (IsPresentableRenderTargetView(resource, view))
    resource.SetPresentSourceView(view);
}

UINT
GetDepthStencilArrayLength(Resource &resource, const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc)
    return resource.GetTexture() ? resource.GetTexture()->arrayLength() : 1;

  switch (descriptor.desc.dsv.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    return descriptor.desc.dsv.Texture1DArray.ArraySize;
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return descriptor.desc.dsv.Texture2DArray.ArraySize;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    return descriptor.desc.dsv.Texture2DMSArray.ArraySize;
  default:
    return 1;
  }
}

bool
GetRenderTargetSubresourceRanges(Resource &resource,
                                 const DescriptorRecord &descriptor,
                                 std::vector<SubresourceRange> &ranges) {
  if (resource.GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return AppendAllSubresourcesRange(resource, ranges);
  if (!descriptor.has_desc)
    return AppendDefaultRenderTargetSubresourceRanges(resource, ranges);

  const auto &rtv = descriptor.desc.rtv;
  switch (rtv.ViewDimension) {
  case D3D12_RTV_DIMENSION_TEXTURE1D:
    return AppendTextureSubresourceRanges(resource, rtv.Texture1D.MipSlice, 1, 0,
                                          1, 0, ranges);
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    return AppendTextureSubresourceRanges(
        resource, rtv.Texture1DArray.MipSlice, 1,
        rtv.Texture1DArray.FirstArraySlice, rtv.Texture1DArray.ArraySize, 0,
        ranges);
  case D3D12_RTV_DIMENSION_TEXTURE2D:
    return AppendTextureSubresourceRanges(resource, rtv.Texture2D.MipSlice, 1, 0,
                                          1, rtv.Texture2D.PlaneSlice, ranges);
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    return AppendTextureSubresourceRanges(
        resource, rtv.Texture2DArray.MipSlice, 1,
        rtv.Texture2DArray.FirstArraySlice, rtv.Texture2DArray.ArraySize,
        rtv.Texture2DArray.PlaneSlice, ranges);
  case D3D12_RTV_DIMENSION_TEXTURE2DMS:
    return AppendTextureSubresourceRanges(resource, 0, 1, 0, 1, 0, ranges);
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    return AppendTextureSubresourceRanges(
        resource, 0, 1, rtv.Texture2DMSArray.FirstArraySlice,
        rtv.Texture2DMSArray.ArraySize, 0, ranges);
  case D3D12_RTV_DIMENSION_TEXTURE3D:
    return AppendTextureSubresourceRanges(resource, rtv.Texture3D.MipSlice, 1, 0,
                                          1, 0, ranges);
  default:
    return false;
  }
}

bool
GetDepthStencilSubresourceRanges(Resource &resource,
                                 const DescriptorRecord &descriptor,
                                 std::vector<SubresourceRange> &ranges) {
  if (!descriptor.has_desc)
    return AppendDefaultDepthStencilSubresourceRanges(resource, ranges);

  const auto &dsv = descriptor.desc.dsv;
  switch (dsv.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1D:
    return AppendTextureSubresourceRanges(resource, dsv.Texture1D.MipSlice, 1, 0,
                                          1, 0, ranges);
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    return AppendTextureSubresourceRanges(
        resource, dsv.Texture1DArray.MipSlice, 1,
        dsv.Texture1DArray.FirstArraySlice, dsv.Texture1DArray.ArraySize, 0,
        ranges);
  case D3D12_DSV_DIMENSION_TEXTURE2D:
    return AppendTextureSubresourceRanges(resource, dsv.Texture2D.MipSlice, 1, 0,
                                          1, 0, ranges);
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return AppendTextureSubresourceRanges(
        resource, dsv.Texture2DArray.MipSlice, 1,
        dsv.Texture2DArray.FirstArraySlice, dsv.Texture2DArray.ArraySize, 0,
        ranges);
  case D3D12_DSV_DIMENSION_TEXTURE2DMS:
    return AppendTextureSubresourceRanges(resource, 0, 1, 0, 1, 0, ranges);
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    return AppendTextureSubresourceRanges(
        resource, 0, 1, dsv.Texture2DMSArray.FirstArraySlice,
        dsv.Texture2DMSArray.ArraySize, 0, ranges);
  default:
    return false;
  }
}

bool
GetShaderResourceSubresourceRanges(Resource &resource,
                                   const DescriptorRecord &descriptor,
                                   std::vector<SubresourceRange> &ranges) {
  if (resource.GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return AppendAllSubresourcesRange(resource, ranges);
  if (!descriptor.has_desc)
    return AppendAllSubresourcesRange(resource, ranges);

  const auto &srv = descriptor.desc.srv;
  switch (srv.ViewDimension) {
  case D3D12_SRV_DIMENSION_BUFFER:
    return AppendAllSubresourcesRange(resource, ranges);
  case D3D12_SRV_DIMENSION_TEXTURE1D:
    return AppendTextureSubresourceRanges(
        resource, srv.Texture1D.MostDetailedMip,
        NormalizeViewCount(srv.Texture1D.MipLevels,
                           srv.Texture1D.MostDetailedMip,
                           GetResourceMipLevelCount(resource)),
        0, 1, 0, ranges);
  case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
    return AppendTextureSubresourceRanges(
        resource, srv.Texture1DArray.MostDetailedMip,
        NormalizeViewCount(srv.Texture1DArray.MipLevels,
                           srv.Texture1DArray.MostDetailedMip,
                           GetResourceMipLevelCount(resource)),
        srv.Texture1DArray.FirstArraySlice,
        NormalizeViewCount(srv.Texture1DArray.ArraySize,
                           srv.Texture1DArray.FirstArraySlice,
                           resource.GetResourceDesc().DepthOrArraySize),
        0, ranges);
  case D3D12_SRV_DIMENSION_TEXTURE2D:
    return AppendTextureSubresourceRanges(
        resource, srv.Texture2D.MostDetailedMip,
        NormalizeViewCount(srv.Texture2D.MipLevels,
                           srv.Texture2D.MostDetailedMip,
                           GetResourceMipLevelCount(resource)),
        0, 1, srv.Texture2D.PlaneSlice, ranges);
  case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
    return AppendTextureSubresourceRanges(
        resource, srv.Texture2DArray.MostDetailedMip,
        NormalizeViewCount(srv.Texture2DArray.MipLevels,
                           srv.Texture2DArray.MostDetailedMip,
                           GetResourceMipLevelCount(resource)),
        srv.Texture2DArray.FirstArraySlice,
        NormalizeViewCount(srv.Texture2DArray.ArraySize,
                           srv.Texture2DArray.FirstArraySlice,
                           resource.GetResourceDesc().DepthOrArraySize),
        srv.Texture2DArray.PlaneSlice, ranges);
  case D3D12_SRV_DIMENSION_TEXTURE2DMS:
    return AppendTextureSubresourceRanges(resource, 0, 1, 0, 1, 0, ranges);
  case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
    return AppendTextureSubresourceRanges(
        resource, 0, 1, srv.Texture2DMSArray.FirstArraySlice,
        NormalizeViewCount(srv.Texture2DMSArray.ArraySize,
                           srv.Texture2DMSArray.FirstArraySlice,
                           resource.GetResourceDesc().DepthOrArraySize),
        0, ranges);
  case D3D12_SRV_DIMENSION_TEXTURE3D:
    return AppendTextureSubresourceRanges(
        resource, srv.Texture3D.MostDetailedMip,
        NormalizeViewCount(srv.Texture3D.MipLevels,
                           srv.Texture3D.MostDetailedMip,
                           GetResourceMipLevelCount(resource)),
        0, 1, 0, ranges);
  case D3D12_SRV_DIMENSION_TEXTURECUBE:
    return AppendTextureSubresourceRanges(
        resource, srv.TextureCube.MostDetailedMip,
        NormalizeViewCount(srv.TextureCube.MipLevels,
                           srv.TextureCube.MostDetailedMip,
                           GetResourceMipLevelCount(resource)),
        0, std::min<UINT>(6, resource.GetResourceDesc().DepthOrArraySize), 0,
        ranges);
  case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
    return AppendTextureSubresourceRanges(
        resource, srv.TextureCubeArray.MostDetailedMip,
        NormalizeViewCount(srv.TextureCubeArray.MipLevels,
                           srv.TextureCubeArray.MostDetailedMip,
                           GetResourceMipLevelCount(resource)),
        srv.TextureCubeArray.First2DArrayFace,
        NormalizeViewCount(srv.TextureCubeArray.NumCubes * 6,
                           srv.TextureCubeArray.First2DArrayFace,
                           resource.GetResourceDesc().DepthOrArraySize),
        0, ranges);
  default:
    return false;
  }
}

bool
GetUnorderedAccessSubresourceRanges(Resource &resource,
                                    const DescriptorRecord &descriptor,
                                    std::vector<SubresourceRange> &ranges) {
  if (resource.GetResourceDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return AppendAllSubresourcesRange(resource, ranges);
  if (!descriptor.has_desc)
    return AppendDefaultUnorderedAccessSubresourceRanges(resource, ranges);

  const auto &uav = descriptor.desc.uav;
  switch (uav.ViewDimension) {
  case D3D12_UAV_DIMENSION_BUFFER:
    return AppendAllSubresourcesRange(resource, ranges);
  case D3D12_UAV_DIMENSION_TEXTURE1D:
    return AppendTextureSubresourceRanges(resource, uav.Texture1D.MipSlice, 1, 0,
                                          1, 0, ranges);
  case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
    return AppendTextureSubresourceRanges(
        resource, uav.Texture1DArray.MipSlice, 1,
        uav.Texture1DArray.FirstArraySlice,
        NormalizeViewCount(uav.Texture1DArray.ArraySize,
                           uav.Texture1DArray.FirstArraySlice,
                           resource.GetResourceDesc().DepthOrArraySize),
        0, ranges);
  case D3D12_UAV_DIMENSION_TEXTURE2D:
    return AppendTextureSubresourceRanges(resource, uav.Texture2D.MipSlice, 1, 0,
                                          1, uav.Texture2D.PlaneSlice, ranges);
  case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
    return AppendTextureSubresourceRanges(
        resource, uav.Texture2DArray.MipSlice, 1,
        uav.Texture2DArray.FirstArraySlice,
        NormalizeViewCount(uav.Texture2DArray.ArraySize,
                           uav.Texture2DArray.FirstArraySlice,
                           resource.GetResourceDesc().DepthOrArraySize),
        uav.Texture2DArray.PlaneSlice, ranges);
  case D3D12_UAV_DIMENSION_TEXTURE3D:
    return AppendTextureSubresourceRanges(resource, uav.Texture3D.MipSlice, 1, 0,
                                          1, 0, ranges);
  default:
    return false;
  }
}

UINT
NormalizeViewCount(UINT requested, UINT first, UINT total) {
  if (first >= total)
    return 1;
  const UINT remaining = total - first;
  if (requested == UINT_MAX || requested == 0)
    return remaining;
  return std::min(requested, remaining);
}

UINT
GetMipDepth(const Resource &resource, UINT mip_slice) {
  const auto &desc = resource.GetResourceDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
    return 1;
  return static_cast<UINT>(std::max<UINT64>(1, desc.DepthOrArraySize >> mip_slice));
}

} // namespace dxmt::d3d12
