#include "d3d12_resolve_region.hpp"

#include "log/log.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

std::optional<ResolveTextureMode>
ConvertResolveMode(D3D12_RESOLVE_MODE mode) {
  switch (mode) {
  case D3D12_RESOLVE_MODE_AVERAGE:
    return ResolveTextureMode::Average;
  case D3D12_RESOLVE_MODE_MIN:
    return ResolveTextureMode::Min;
  case D3D12_RESOLVE_MODE_MAX:
    return ResolveTextureMode::Max;
  default:
    return std::nullopt;
  }
}

uint64_t
ResolveMipLevelWidth(const D3D12_RESOURCE_DESC &desc, UINT mip_level) {
  return std::max<uint64_t>(1, desc.Width >> mip_level);
}

uint64_t
ResolveMipLevelHeight(const D3D12_RESOURCE_DESC &desc, UINT mip_level) {
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D)
    return 1;
  return std::max<uint64_t>(1, uint64_t(desc.Height) >> mip_level);
}

bool
IsFullResolveRegion(const ResolveSubresourceRecord &record, uint64_t src_width,
                    uint64_t src_height, uint64_t dst_width,
                    uint64_t dst_height) {
  if (record.dst_x || record.dst_y || record.src_rect)
    return false;
  return src_width && src_height && src_width == dst_width &&
         src_height == dst_height;
}

bool
NormalizeResolveRegion(const ResolveSubresourceRecord &record,
                       uint64_t src_width, uint64_t src_height,
                       uint64_t dst_width, uint64_t dst_height,
                       WMTScissorRect &src_rect, WMTOrigin &dst_origin,
                       WMTSize &resolve_size) {
  dst_origin = {record.dst_x, record.dst_y, 0};
  if (record.src_rect) {
    const auto &rect = *record.src_rect;
    if (rect.left < 0 || rect.top < 0 || rect.right <= rect.left ||
        rect.bottom <= rect.top) {
      WARN("D3D12CommandQueue: ResolveSubresourceRegion invalid source rect");
      return false;
    }
    src_rect = {uint64_t(rect.left), uint64_t(rect.top),
                uint64_t(rect.right - rect.left),
                uint64_t(rect.bottom - rect.top)};
  } else {
    src_rect = {0, 0, src_width, src_height};
  }

  resolve_size = {src_rect.width, src_rect.height, 1};
  if (src_rect.x > src_width || src_rect.y > src_height ||
      src_rect.width > src_width - src_rect.x ||
      src_rect.height > src_height - src_rect.y) {
    WARN("D3D12CommandQueue: ResolveSubresourceRegion source rect exceeds source subresource");
    return false;
  }
  if (dst_origin.x > dst_width || dst_origin.y > dst_height ||
      resolve_size.width > dst_width - dst_origin.x ||
      resolve_size.height > dst_height - dst_origin.y) {
    WARN("D3D12CommandQueue: ResolveSubresourceRegion destination region exceeds destination subresource");
    return false;
  }
  return true;
}

} // namespace dxmt::d3d12
