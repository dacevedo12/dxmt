#include "d3d12_replay_pass_admission.hpp"

#include "d3d12_replay_pass_batch_ops.hpp"
#include "d3d12_replay_pass_compatibility.hpp"

namespace dxmt::d3d12 {

bool ReplayGraphicsPassContinuesSubmissionBoundary(
    const ReplayState &state, const ReplayRenderPassAttachments &attachments) {
  return state.submission_boundary_kind == CompiledEncoderKind::Graphics &&
         !HasPendingComputePass(state) && !HasPendingBlitBatch(state) &&
         state.graphics_pass_batch.active &&
         ReplayRenderPassAttachmentsMatch(state.graphics_pass_batch.attachments,
                                          attachments);
}

bool ReplayComputePassContinuesSubmissionBoundary(const ReplayState &state) {
  return state.submission_boundary_kind == CompiledEncoderKind::Compute &&
         !HasPendingGraphicsPass(state) && !HasPendingBlitBatch(state) &&
         state.compute_pass_batch.active;
}

bool ReplayGraphicsBatchRejectsAttachments(
    const ReplayGraphicsPassBatch &batch,
    const ReplayRenderPassAttachments &attachments) {
  return batch.active &&
         !ReplayRenderPassAttachmentsMatch(batch.attachments, attachments);
}

ReplayGraphicsBatchSlot ReserveReplayGraphicsBatchSlot(
    ReplayGraphicsPassBatch &batch, uint64_t argument_buffer_size,
    ReplayGraphicsCommandKind kind, bool use_geometry,
    bool use_tessellation) {
  ReplayGraphicsBatchSlot slot;
  slot.argument_buffer_offset = batch.argument_buffer_size;
  slot.command_index = static_cast<uint32_t>(batch.commands.size());
  slot.parallel_candidate = ReplayGraphicsCommandCanParallelEncode(
      kind, use_geometry, use_tessellation);
  batch.argument_buffer_size += argument_buffer_size;
  batch.use_geometry = batch.use_geometry || use_geometry;
  batch.use_tessellation = batch.use_tessellation || use_tessellation;
  return slot;
}

void FillReplayGraphicsPassCommandSlot(ReplayGraphicsPassCommand &command,
                                       const ReplayGraphicsBatchSlot &slot,
                                       uint64_t argument_buffer_size,
                                       ReplayGraphicsCommandKind kind,
                                       bool use_geometry,
                                       bool use_tessellation,
                                       bool bindless_compiled_candidate) {
  command.argument_buffer_size = argument_buffer_size;
  command.argument_buffer_offset = slot.argument_buffer_offset;
  command.command_index = slot.command_index;
  command.kind = kind;
  command.parallel_candidate = slot.parallel_candidate;
  command.use_geometry = use_geometry;
  command.use_tessellation = use_tessellation;
  command.bindless_compiled_candidate = bindless_compiled_candidate;
}

} // namespace dxmt::d3d12
