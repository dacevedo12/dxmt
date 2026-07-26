#pragma once

// Statistics folding and diagnostic reporting for one compiled command-list
// replay.
//
// AccumulateCompiledReplayStageIntervals(),
// AccumulateCompiledReplaySubPhaseIntervals(),
// RecordCompiledFallbackRangeStats(), RecordCompiledReplayFrameStatistics()
// and LogCompiledReplayBreakdown() used to be private members of class
// CommandQueueImpl (d3d12_command_queue_execute.inc). All of them only read
// the replay context's own references plus the thread-local diagnostic
// ledgers, and write nothing but frame statistics and log lines.
//
// The first two were blocked on ReplayBreakdownAccumulators / StallProbe,
// which now live at namespace scope. RecordCompiledReplayFrameStatistics()
// additionally read exactly two scalars off the queue instance — the queue
// address it prints as `queue=` and desc_.Type it prints as `queueType=` —
// which are now passed in, so the emitted log line is byte-for-byte the one
// the member produced.
//
// The env-tuned breakdown threshold in LogCompiledReplayBreakdown() stays a
// single process-wide `static const`: it is a function-local static in this
// one non-inline definition, exactly as it was in the implicitly-inline
// member.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_compiled_execute_types.hpp"
#include "d3d12_replay_perf_timers.hpp"
#include "d3d12_replay_stall_probe.hpp"
#include "dxmt_statistics.hpp"

#include <d3d12.h>

namespace dxmt {
struct FrameStatistics;
}

namespace dxmt::d3d12 {

// Folds the replay-stage interval accumulators into the frame ledger.
void AccumulateCompiledReplayStageIntervals(
    dxmt::FrameStatistics *stats, const ReplayBreakdownAccumulators &rb);

// Folds the per-draw sub-phase timers into the frame ledger.
void AccumulateCompiledReplaySubPhaseIntervals(dxmt::FrameStatistics *stats,
                                               const PerDrawSubTimers &timers,
                                               const StallProbe &stall);

// Classifies a compatibility record range into graphics / compute packets
// for the per-frame fallback statistics.
void RecordCompiledFallbackRangeStats(CompiledReplayContext &ctx, UINT begin,
                                      UINT count,
                                      dxmt::CompiledFallbackReason reason);

// Publishes the replay breakdown to the worker frame ledger and logs a
// context line for pathologically slow or PSO-starved replays.
// `queue_identity` is the recording queue's address and `queue_type` its
// D3D12_COMMAND_QUEUE_DESC::Type; both are printed verbatim and nothing else
// about the queue object is observed.
void RecordCompiledReplayFrameStatistics(CompiledReplayContext &ctx,
                                         uintptr_t queue_identity,
                                         D3D12_COMMAND_LIST_TYPE queue_type,
                                         clock::time_point rb_t0,
                                         clock::time_point rb_t1,
                                         clock::time_point rb_t2,
                                         clock::time_point rb_t3);

// DXMT_DIAG_REPLAY_BREAKDOWN: verbose per-record-type replay diagnostics.
void LogCompiledReplayBreakdown(CompiledReplayContext &ctx,
                                clock::time_point rb_t0,
                                clock::time_point rb_t1,
                                clock::time_point rb_t2,
                                clock::time_point rb_t3);

} // namespace dxmt::d3d12
