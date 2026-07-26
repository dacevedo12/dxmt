#pragma once

#include "d3d12_command_list.hpp"
#include "dxmt_command.hpp"
#include "winemetal.h"

#include <cstdint>
#include <d3d12.h>
#include <optional>

namespace dxmt::d3d12 {

// Maps a D3D12 resolve mode onto the Metal-side resolve mode. Returns
// std::nullopt for modes that have no emulation path.
[[nodiscard]] std::optional<ResolveTextureMode>
ConvertResolveMode(D3D12_RESOLVE_MODE mode);

// Mip dimensions of a resolve source/destination subresource. Height is
// clamped to one for 1D textures, matching the D3D12 subresource layout.
[[nodiscard]] uint64_t ResolveMipLevelWidth(const D3D12_RESOURCE_DESC &desc,
                                            UINT mip_level);

[[nodiscard]] uint64_t ResolveMipLevelHeight(const D3D12_RESOURCE_DESC &desc,
                                             UINT mip_level);

// True when the record resolves the whole subresource one-to-one, which is
// the only shape the fast Metal resolve path can express.
[[nodiscard]] bool IsFullResolveRegion(const ResolveSubresourceRecord &record,
                                       uint64_t src_width, uint64_t src_height,
                                       uint64_t dst_width, uint64_t dst_height);

// Expands the (optional) source rect of a resolve record into an explicit
// source rect, destination origin and resolve extent, validating that both
// regions stay inside their subresource. Returns false and warns when the
// requested region is invalid; the out parameters are then unspecified.
[[nodiscard]] bool
NormalizeResolveRegion(const ResolveSubresourceRecord &record,
                       uint64_t src_width, uint64_t src_height,
                       uint64_t dst_width, uint64_t dst_height,
                       WMTScissorRect &src_rect, WMTOrigin &dst_origin,
                       WMTSize &resolve_size);

} // namespace dxmt::d3d12
