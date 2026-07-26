#pragma once

// Render pass attachment identity, the compiled pass recipe derived from it,
// and the encoder-side pass start.
//
// These used to be private members of CommandQueueImpl
// (d3d12_command_queue_render_state.inc). Everything but the attachment build
// was already static; the attachment build reached through `this` only for
// device_->GetMTLDevice(), needed to create the Metal views, which is now an
// explicit parameter. Keeping the "describe the attachments" half separate
// from the "apply the description to an encoder" half is what lets the
// pass-emitting closures drop their `this` capture.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "Metal.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "dxmt_context.hpp"

#include <cstdint>
#include <span>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct PipelineGraphicsState;

// Resolves the render target and depth stencil descriptors of one draw state
// into the immutable attachment identity a render pass is built from.
//
// Reserved textures are materialized on the way through, so this is
// submission-dynamic and must run before the attachments are frozen into a
// packet. Descriptors whose resource is gone, or which never got a texture,
// are skipped without shifting the remaining slots. `depth_stencil` is null
// when the state has no bound DSV; `graphics` may be null, in which case the
// depth/stencil planes are assumed writable.
[[nodiscard]] ReplayRenderPassAttachments
BuildRenderPassAttachments(WMT::Device device,
                           std::span<const DescriptorRecord> render_targets,
                           const DescriptorRecord *depth_stencil,
                           const PipelineGraphicsState *graphics);

// Folds the attachment extents, array lengths and sample counts into the
// pass-wide values Metal needs up front. Takes ownership of `attachments`.
[[nodiscard]] CompiledRenderPassRecipe
BuildCompiledRenderPassRecipe(ReplayRenderPassAttachments attachments,
                              bool use_geometry, bool use_tessellation);

// Starts the Metal render pass described by `recipe`. Returns false without
// touching the encoder when the recipe has no attachment at all, which is the
// caller's signal to skip the whole pass.
[[nodiscard]] bool BeginCompiledRenderPass(ArgumentEncodingContext &enc,
                                           CompiledRenderPassRecipe &recipe,
                                           uint64_t argument_buffer_size);

// BuildCompiledRenderPassRecipe + BeginCompiledRenderPass for callers that
// hold the attachments directly. `attachments` is moved into the temporary
// recipe and moved back out again, so it stays valid (and unchanged) whether
// or not the pass began.
[[nodiscard]] bool BeginRenderPass(ArgumentEncodingContext &enc,
                                   ReplayRenderPassAttachments &attachments,
                                   uint64_t argument_buffer_size,
                                   bool use_geometry = false,
                                   bool use_tessellation = false);

} // namespace dxmt::d3d12
