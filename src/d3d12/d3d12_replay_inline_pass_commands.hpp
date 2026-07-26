#pragma once

// Pass commands that are appended to (or emitted around) a replay batch
// without consulting the recording queue.
//
// QueueGraphicsResourceBarrierCommand(), QueueComputeResourceBarrierCommand()
// and EmitSingleComputePass() used to be private members of class
// CommandQueueImpl (d3d12_command_queue_pass_queue.inc). None of them reads a
// CommandQueueImpl instance member or names `this`:
//
//   - the two barrier appenders only allocate an encoder out of the batch's
//     own arena and push a ReplayPassEncodeCommand onto it; their encode
//     lambdas capture the moved barrier entry vector by value and nothing
//     else, so the closure carries no queue pointer;
//   - EmitSingleComputePass()'s emitcc closure captures only its own
//     parameters (size, sequence, the forwarded encode callable), which is
//     exactly why it never needed the `this` capture its graphics counterpart
//     (still in the fragment) requires for BeginRenderPass().
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_render_pass_attachments.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_resource_barrier_batch.hpp"
#include "dxmt_apitrace.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"

#include <cstdint>
#include <utility>

namespace dxmt::d3d12 {

// Appends the barrier batch to the open graphics pass batch as an inline
// encode command. `chunk` is unused; it only keeps the call site symmetrical
// with the other pass helpers.
void QueueGraphicsResourceBarrierCommand(CommandChunk *chunk,
                                         ReplayState &state,
                                         ResourceAccessBarrierBatch batch);

// Compute counterpart: appends the barrier batch to the open compute pass
// batch and resolves the compute pass barrier inline.
void QueueComputeResourceBarrierCommand(CommandChunk *chunk,
                                        ReplayState &state,
                                        ResourceAccessBarrierBatch batch);

// Emits one standalone compute pass carrying a single encode callable, used
// when compute batching is disabled.
template <typename Fn>
void EmitSingleComputePass(CommandChunk *chunk, uint64_t argument_buffer_size,
                           uint64_t d3d_sequence, Fn &&fn) {
  chunk->emitcc([argument_buffer_size, d3d_sequence,
                 encode = std::forward<Fn>(fn)](
                    ArgumentEncodingContext &enc) mutable {
    enc.startComputePass(argument_buffer_size);
    uint64_t argbuf_offset = 0;
    enc.setArgumentBufferWriteRange(0, argument_buffer_size);
    if (d3d_sequence != 0)
      dxmt::apitrace::set_current_d3d_sequence(d3d_sequence);
    encode(enc, argbuf_offset);
    enc.endPass();
  });
}

// Graphics counterpart, used when graphics batching is disabled or a single
// draw cannot join the open batch. BeginRenderPass() is a free function now,
// so this closure captures only the moved attachments and its own parameters —
// the `this` capture it used to need as a CommandQueueImpl member is gone.
template <typename Fn>
void EmitSingleGraphicsPass(CommandChunk *chunk,
                            ReplayRenderPassAttachments attachments,
                            uint64_t argument_buffer_size,
                            uint64_t d3d_sequence, Fn &&fn,
                            bool use_geometry = false,
                            bool use_tessellation = false) {
  chunk->emitcc([attachments = std::move(attachments), argument_buffer_size,
                 use_geometry, use_tessellation, d3d_sequence,
                 encode = std::forward<Fn>(fn)](
                    ArgumentEncodingContext &enc) mutable {
    if (!BeginRenderPass(enc, attachments, argument_buffer_size,
                         use_geometry, use_tessellation))
      return;
    uint64_t argbuf_offset = 0;
    enc.setArgumentBufferWriteRange(0, argument_buffer_size);
    if (d3d_sequence != 0) {
      dxmt::apitrace::set_current_d3d_sequence(d3d_sequence);
    }
    enc.bumpVisibilityResultOffset();
    encode(enc, argbuf_offset);
    enc.endPass();
  });
}

} // namespace dxmt::d3d12
