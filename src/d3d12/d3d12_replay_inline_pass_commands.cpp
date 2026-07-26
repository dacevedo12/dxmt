#include "d3d12_replay_inline_pass_commands.hpp"

#include "d3d12_replay_barrier_encode.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_perf_stats.hpp"
#include "dxmt_statistics.hpp"

#include <cstdint>
#include <utility>

namespace dxmt::d3d12 {

void QueueGraphicsResourceBarrierCommand(CommandChunk *chunk,
                                         ReplayState &state,
                                         ResourceAccessBarrierBatch batch) {
  (void)chunk;
  if (batch.entries.empty())
    return;
  auto &active_batch = state.graphics_pass_batch;
  const auto command_index =
      static_cast<uint32_t>(active_batch.commands.size());
  auto encode_barrier = [entries = std::move(batch.entries)](
                            ArgumentEncodingContext &enc,
                            uint64_t &argbuf_offset) mutable {
    (void)argbuf_offset;
    EncodeInlineRenderResourceBarrierEntries(enc, entries);
  };
  ReplayGraphicsPassCommand command = {};
  using Encode = decltype(encode_barrier);
  command.encoder = AllocateReplayEncoder<
      ReplayPassEncodeCommand, ReplayOwnedEncodeCommand<Encode>>(
      active_batch.encoder_arena, std::move(encode_barrier));
  command.d3d_sequence = dxmt::apitrace::current_d3d_sequence();
  command.argument_buffer_size = 0;
  command.argument_buffer_offset = active_batch.argument_buffer_size;
  command.command_index = command_index;
  command.kind = ReplayGraphicsCommandKind::Barrier;
  active_batch.commands.push_back(std::move(command));
  if (auto *stats = dxmt::perf::currentFrameStatistics())
    stats->resource_barrier_batches_graphics_inlined++;
}

void QueueComputeResourceBarrierCommand(CommandChunk *chunk,
                                        ReplayState &state,
                                        ResourceAccessBarrierBatch batch) {
  (void)chunk;
  if (batch.entries.empty())
    return;
  auto &active_batch = state.compute_pass_batch;
  auto encode_barrier = [entries = std::move(batch.entries)](
                            ArgumentEncodingContext &enc,
                            uint64_t &argbuf_offset) mutable {
    (void)argbuf_offset;
    EncodeResourceAccessBarrierEntries(enc, entries);
    enc.resolveComputePassBarrier();
  };
  using Encode = decltype(encode_barrier);
  ReplayComputePassCommand command = {};
  command.encoder = AllocateReplayEncoder<
      ReplayPassEncodeCommand, ReplayOwnedEncodeCommand<Encode>>(
      active_batch.encoder_arena, std::move(encode_barrier));
  command.d3d_sequence = dxmt::apitrace::current_d3d_sequence();
  command.argument_buffer_offset = active_batch.argument_buffer_size;
  command.kind = ReplayComputeCommandKind::Barrier;
  active_batch.commands.push_back(std::move(command));
  if (auto *stats = dxmt::perf::currentFrameStatistics())
    stats->resource_barrier_batches_compute_inlined++;
}

} // namespace dxmt::d3d12
