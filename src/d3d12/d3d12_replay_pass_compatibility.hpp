#pragma once

// Render-pass compatibility predicates used by the replay batcher: whether two
// attachment sets describe the same Metal render pass, whether a barrier batch
// touches the attachments of an open pass, and whether a graphics command is
// eligible for parallel encoding.
//
// These helpers used to be private static members of the anonymous-namespace
// class CommandQueueImpl (d3d12_command_queue_pass_batching.inc). They only
// read the promoted replay pass types and the barrier batch value types, so
// hoisting them to dxmt::d3d12 lets them be compiled and analyzed
// independently.

#include "d3d12_replay_pass_types.hpp"
#include "d3d12_resource_barrier_batch.hpp"
#include "dxmt_texture.hpp"

namespace dxmt::d3d12 {

// True when both attachment sets would produce an identical render pass.
[[nodiscard]] bool
ReplayRenderPassAttachmentsMatch(const ReplayRenderPassAttachments &lhs,
                                 const ReplayRenderPassAttachments &rhs);

// True when a texture barrier entry's subresource falls inside a view's mip
// and array ranges.
[[nodiscard]] bool
TextureSubresourceOverlapsView(const ResourceAccessBarrierEntry &entry,
                               TextureViewKey view);

// True when the batch would touch any attachment of the open render pass (or
// demands a separator), meaning it cannot be deferred into that pass.
[[nodiscard]] bool ResourceBarrierTouchesRenderPassAttachments(
    const ResourceAccessBarrierBatch &batch,
    const ReplayRenderPassAttachments &attachments);

// Only plain (non-indirect) draws without geometry/tessellation lowering can
// be encoded in parallel.
[[nodiscard]] bool
ReplayGraphicsCommandCanParallelEncode(ReplayGraphicsCommandKind kind,
                                       bool use_geometry,
                                       bool use_tessellation);

} // namespace dxmt::d3d12
