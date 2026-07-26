#pragma once

// Folding of the per-replay diagnostic ledger into the worker frame
// statistics.
//
// These used to be private static members of class CommandQueueImpl
// (d3d12_command_queue_execute.inc). They read nothing but the promoted
// PerDrawSubTimers ledger and write nothing but the frame statistics record,
// so hoisting them into dxmt::d3d12 lets them be compiled and analyzed
// independently of the ~20k-line queue translation unit.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_perf_timers.hpp"

namespace dxmt {
struct FrameStatistics;
}

namespace dxmt::d3d12 {

// Shared exponential throttle behind every compiled-replay diagnostic log.
[[nodiscard]] bool ShouldLogCompiledReplayDiagnostic();

// Folds the per-record-type replay timers and counts into the frame ledger.
void AccumulateCompiledReplayRecordTypeStatistics(
    dxmt::FrameStatistics *stats, const PerDrawSubTimers &timers);

// Folds the binding / descriptor / snapshot counters into the frame ledger.
void AccumulateCompiledReplayCounterStatistics(
    dxmt::FrameStatistics *stats, const PerDrawSubTimers &timers);

} // namespace dxmt::d3d12
