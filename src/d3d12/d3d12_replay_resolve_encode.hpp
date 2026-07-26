#pragma once

// ResolveSubresource() replay: validation, region normalization and the
// encoded Metal resolve.
//
// These helpers used to be private members of CommandQueueImpl
// (d3d12_command_queue_copy_clear.inc). They only touch namespace-level
// resource / format helpers and the command chunk; the one piece of queue
// identity they used to reach for through `this` — device_->GetMTLDevice(),
// needed for the DXGI format query — is now an explicit parameter.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_texture.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Creates a single-subresource view used as resolve source or destination.
[[nodiscard]] TextureViewKey CreateResolveView(Resource &resource,
                                               UINT subresource,
                                               WMTPixelFormat format,
                                               WMTTextureUsage intended_usage);

// Replays one ResolveSubresource() record.
void ReplayResolveSubresource(WMT::Device device, CommandChunk *chunk,
                              const ResolveSubresourceRecord &record);

} // namespace dxmt::d3d12
