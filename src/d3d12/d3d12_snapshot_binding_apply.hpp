#pragma once

// Replay of a captured GraphicsBindingSnapshot onto a render encoder: static
// samplers, descriptor and root-constant entries, vertex buffers and the
// per-stage bindless/native wiring.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_compiled_encode.inc). A snapshot is self-contained, so
// everything queue-owned it still needs arrives in a SubmissionBindingContext.

#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_submission_binding_context.hpp"
#include "dxmt_context.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

/** Binds everything the snapshot captured for one deferred draw. An ordinary
 *  bindless snapshot whose vertex and pixel tables were both frozen at capture
 *  skips the interpreted descriptor walk entirely: residency, vertex buffers
 *  and the frozen upload are all such a draw needs. */
void ApplyGraphicsBindingSnapshot(const SubmissionBindingContext &ctx,
                                  ArgumentEncodingContext &enc,
                                  const GraphicsBindingSnapshot &snapshot,
                                  const PipelineState &pipeline,
                                  bool use_geometry, bool use_tessellation,
                                  uint64_t &argbuf_offset,
                                  BindlessMirrorDrawDiag *draw_diag = nullptr);

} // namespace dxmt::d3d12
