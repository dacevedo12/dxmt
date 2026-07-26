#pragma once

// PERF DIAG (DXMT_DIAG_STALL): per-RECORD-instance stall localization probe and
// the two RAII microsecond accumulators built on top of it.
//
// StallProbe, stallProbe(), StallDiagEnabled(), StallScope and ScopeAccum used
// to be private nested types / private static members of class CommandQueueImpl
// (d3d12_command_queue_pass_queue.inc). Like PerDrawSubTimers they are a pure
// counter ledger plus two scope guards: nothing here reads a queue instance
// member, yet replay fragments all over the queue poke at them, which made them
// a blocker for moving replay logic into standalone translation units.
//
// The probe is one `thread_local` instance per thread for the whole program,
// exactly as before: the static member function was implicitly inline, so its
// function-local `static thread_local` already had a single program-wide
// definition. The definition now lives in d3d12_replay_stall_probe.cpp, which
// keeps that guarantee independent of inlining.
//
// Unqualified `stallProbe()` / `StallDiagEnabled()` calls and unqualified
// `StallProbe` / `StallScope` / `ScopeAccum` uses inside CommandQueueImpl still
// resolve here, because the class lives in this same dxmt::d3d12 namespace.

#include "dxmt_statistics.hpp"

#include <chrono>
#include <cstdint>

namespace dxmt::d3d12 {

// PERF DIAG (DXMT_DIAG_STALL): per-RECORD-instance stall localization. The
// real wall is NOT uniform per-draw descAccess (~0.14%) but a synchronous
// first-use stall on SOME DrawIndexed/Dispatch records (per-draw cost varies
// ~800x). This probe times the candidate sub-phases of one record so a slow
// record (> threshold) self-reports WHERE it stalled: PSO compile-on-demand,
// descriptor access, attachment build, or emit/flush back-pressure (record
// thread blocking on the full chunk ring). us accumulators are reset per
// record by ReplayRecord and read back after the record completes.
struct StallProbe {
  uint64_t psoSelectUs = 0;    // GetMetalGraphicsState (PSO compile-on-demand)
  uint64_t getPipelineUs = 0;  // GetPipelineState downcast
  uint64_t selectPsoUs = 0;    // SelectGraphicsPipelineState (topology/index variant)
  uint64_t descAccessUs = 0;   // RecordGraphicsPipelineResourceAccess (hazard)
  uint64_t attachUs = 0;       // BuildRenderPassAttachments
  uint64_t bindSnapUs = 0;     // GraphicsPassBatchNeedsBindingSnapshot + Capture
  uint64_t estimateUs = 0;     // EstimateGraphicsArgumentBufferSize
  uint64_t packetUs = 0;       // packet build + encode-closure capture (struct moves)
  uint64_t queueUs = 0;        // QueueGraphicsPassCommand / EmitSingleGraphicsPass
  uint64_t emitUs = 0;         // FlushPassBatches (chunk-ring back-pressure)
  void reset() { *this = {}; }
};

// The per-thread stall probe. Same single thread_local instance the
// queue-private accessor returned.
StallProbe &stallProbe();

[[nodiscard]] bool StallDiagEnabled();

struct StallScope {
  bool on; clock::time_point t0; uint64_t *dst;
  StallScope(bool e, uint64_t *d) : on(e), t0(e ? clock::now() : clock::time_point{}), dst(d) {}
  ~StallScope() { if (on) *dst += std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0).count(); }
};

// RAII accumulator for sub-function timing (handles multiple return paths).
struct ScopeAccum {
  bool on;
  clock::time_point t0;
  uint64_t *dst;
  ~ScopeAccum() {
    if (on)
      *dst += std::chrono::duration_cast<std::chrono::microseconds>(
                  clock::now() - t0).count();
  }
};

} // namespace dxmt::d3d12
