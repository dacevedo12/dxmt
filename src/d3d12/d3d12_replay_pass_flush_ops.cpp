#include "d3d12_replay_pass_flush_ops.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_replay_barrier_encode.hpp"
#include "d3d12_replay_pass_batch_ops.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_perf_timers.hpp"
#include "d3d12_replay_stall_probe.hpp"
#include "dxmt_apitrace.hpp"
#include "dxmt_context.hpp"
#include "dxmt_statistics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>

namespace dxmt::d3d12 {

void EmitTimestampMarkers(CommandChunk *chunk, ReplayState &state) {
  if (state.pending_timestamp_markers.empty())
    return;
  const bool _rb = ReplayPerfEnabled();
  if (_rb) perDrawSubTimers().emitTsMarkersCalls++;
  ScopeAccum _rbacc{_rb, _rb ? clock::now() : clock::time_point{},
                    &perDrawSubTimers().emitTsMarkersUs};

  auto markers = std::move(state.pending_timestamp_markers);
  state.pending_timestamp_markers = {};
  for (auto &marker : markers) {
    chunk->emitcc([query = std::move(marker.query)](
                      ArgumentEncodingContext &enc) mutable {
      enc.sampleTimestamp(std::move(query));
    });
  }
}

void FlushBlitBatch(CommandChunk *chunk, ReplayState &state) {
  auto &batch = state.blit_batch;
  if (!HasPendingBlitBatch(state))
    return;
  const bool _rb = ReplayPerfEnabled();
  if (_rb) perDrawSubTimers().flushBlitCalls++;
  ScopeAccum _rbacc{_rb, _rb ? clock::now() : clock::time_point{},
                    &perDrawSubTimers().flushBlitUs};

  if (_rb) {
    auto &timers = perDrawSubTimers();
    timers.blitBatchCommands += batch.commands.size();
    timers.blitBatchCommandsMax =
        std::max<uint64_t>(timers.blitBatchCommandsMax,
                           batch.commands.size());
    timers.blitBatchReads += batch.reads.size();
    timers.blitBatchWrites += batch.writes.size();
  }

  auto barriers = TakeMatchingPendingResourceBarriers(
      state, batch.reads, batch.writes);
  const auto separator_begin = _rb ? clock::now() : clock::time_point{};
  RecordSeparatorOnlyBarrier(barriers);
  if (_rb) {
    perDrawSubTimers().blitSeparatorUs +=
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now() - separator_begin)
            .count();
  }

  const auto emit_begin = _rb ? clock::now() : clock::time_point{};
  ReplayCommandStorage<ReplayBlitPassCommand> command_storage{
      std::move(batch.encoder_arena), std::move(batch.commands)};
  chunk->emitcc([command_storage = std::move(command_storage),
                 barrier_entries = std::move(barriers.entries)](
                    ArgumentEncodingContext &enc) mutable {
    enc.startBlitPass();
    EncodeResourceAccessBarrierEntries(enc, barrier_entries);
    for (auto &command : command_storage.commands) {
      if (command.d3d_sequence != 0)
        dxmt::apitrace::set_current_d3d_sequence(command.d3d_sequence);
      command.encoder->Encode(enc);
    }
    enc.endPass();
  });
  if (_rb) {
    perDrawSubTimers().blitEmitCommandUs +=
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now() - emit_begin)
            .count();
  }

  const auto reset_begin = _rb ? clock::now() : clock::time_point{};
  batch = {};
  if (_rb) {
    perDrawSubTimers().blitBatchResetUs +=
        std::chrono::duration_cast<std::chrono::microseconds>(
            clock::now() - reset_begin)
            .count();
  }
}

} // namespace dxmt::d3d12
