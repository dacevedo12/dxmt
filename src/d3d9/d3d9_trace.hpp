#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>

namespace dxmt {

// First-call trace for D3D9 entry points: each D9_TRACE site emits a
// single Logger::warn the first time it is reached, so unimplemented
// or rarely-touched paths never go silent.
class D3D9CallTrace {
public:
  static void OnFirstCall(const char *name);
  // Diagnostic: when DXMT_D9_TRACE_ALL is set, D9_TRACE logs roughly every
  // 65536th call per site via OnEveryCall, so a busy-wait that hammers one
  // entry point shows up as that name repeating in the log tail. trace_all()
  // is a cached env check; both are no-ops in a normal run.
  static void OnEveryCall(const char *name);
  static bool trace_all();
};

// Per-second latency histogram: stores count + sum + min + max of µs
// samples observed in the last drain window. Min/mean/max read out of
// tick() and reset back so the next window is fresh.
//
// Hot path cost when DXMT_LOG_LEVEL=debug is enabled: 2 relaxed
// fetch_add + 2 CAS loops (bounded by the per-sample comparison).
// Cost when disabled: gated by D3D9HotCounters::enabled(), zero ops.
struct LatencyHistogram {
  std::atomic<uint64_t> count{0};
  std::atomic<uint64_t> totalUs{0};
  std::atomic<uint64_t> maxUs{0};
  std::atomic<uint64_t> minUs{std::numeric_limits<uint64_t>::max()};

  void
  record(uint64_t us) {
    count.fetch_add(1, std::memory_order_relaxed);
    totalUs.fetch_add(us, std::memory_order_relaxed);
    uint64_t cur = maxUs.load(std::memory_order_relaxed);
    while (us > cur && !maxUs.compare_exchange_weak(cur, us, std::memory_order_relaxed))
      ;
    cur = minUs.load(std::memory_order_relaxed);
    while (us < cur && !minUs.compare_exchange_weak(cur, us, std::memory_order_relaxed))
      ;
  }
};

// Hot-path counters. Bumped by the hot sites (Lock/Unlock, replaceRegion,
// PSO compile + wait, blit submission). Drained once per second from the
// Present path, logging a single line. Env-gated by DXMT_LOG_LEVEL=debug
// so production runs stay quiet.
struct D3D9HotCounters {
  std::atomic<uint64_t> texLockRect{0};
  // texUnlockUpload counts MANAGED-pool UnlockRect events that staged
  // bytes into m_uploadRing + queued a blit-copy onto the open
  // cmdbuf. The micro counter brackets the staging-and-record window
  // (memcpy + ring allocate + push_back) — replaceRegion's wall-clock
  // is gone now that the upload rides the open cmdbuf.
  std::atomic<uint64_t> texUnlockUpload{0};
  std::atomic<uint64_t> texUnlockUploadMicros{0};
  std::atomic<uint64_t> bufLock{0};
  std::atomic<uint64_t> psoCompileSubmits{0};
  std::atomic<uint64_t> psoWaitFirst{0};
  std::atomic<uint64_t> psoWaitMicros{0};
  std::atomic<uint64_t> blitCmdbufCommits{0};
  std::atomic<uint64_t> drawCalls{0};
  // Per-entry-point draw counters — bump on every API call (including
  // those rejected for INVALIDCALL) so the divergence from drawCalls
  // (which only counts successfully-queued draws) flags games hitting
  // a validation gate hot. drawCalls = drawCallsDP + drawCallsDIP +
  // drawCallsUP + drawCallsUPI - (rejected count). Useful for
  // identifying whether a workload is bound-stream or UP-heavy
  // before deciding which path to optimize.
  std::atomic<uint64_t> drawCallsDP{0};
  std::atomic<uint64_t> drawCallsDIP{0};
  std::atomic<uint64_t> drawCallsUP{0};
  std::atomic<uint64_t> drawCallsUPI{0};
  // Per-frame Present-path timings. presentCalls counts Present
  // invocations. The micro counters bracket the common 1-fps suspects:
  // flushOpenWorkMicros covers the chunk-emit work done pre-Present
  // (drainPendingClear + flushDeferredBlitWork); encode is the
  // Presenter::encodeCommands window (nextDrawable + present-blit
  // encode, combined — if this dominates we drill in next iteration);
  // total is the outer Present window so anything not in a named bucket
  // is visible.
  std::atomic<uint64_t> presentCalls{0};
  std::atomic<uint64_t> presentTotalMicros{0};
  std::atomic<uint64_t> presentEncodeMicros{0};
  std::atomic<uint64_t> flushOpenWorkMicros{0};
  std::atomic<uint64_t> encodeCommandsCalls{0};
  std::atomic<uint64_t> encodeCommandsMicros{0};
  std::atomic<uint64_t> encodeCommandsRecords{0};
  // Per-Present commit timing for the presenter's cmdbuf. If
  // nextDrawable spins on a busy drawable pool or GPU back-pressure
  // stalls the commit, it shows up here even though the broader
  // presentEncode bracket misses it.
  std::atomic<uint64_t> presentCmdbufCommitMicros{0};
  // Wall-clock for PresentBoundary specifically — the
  // frame_latency_fence_.wait that blocks the calling thread until
  // frame N-max_latency_ retires. Splits the pCommitUs bucket so we can
  // tell whether the calling thread is blocked on chunk-pool back-
  // pressure (CommitCurrentChunk) or GPU completion (PresentBoundary).
  std::atomic<uint64_t> presentBoundaryMicros{0};
  std::atomic<uint64_t> nextDrawableMicros{0};
  // Wall-clock microseconds spent OUTSIDE Present — i.e. the gap
  // between one Present's return and the next Present's entry. If the
  // 1-fps cap is the game itself sleeping (or waiting on something
  // not in dxmt), this counter tracks 950+ ms/sec while every other
  // bracket stays small. Bumped from MTLD3D9SwapChain::Present using a
  // per-swapchain "last Present return" timestamp.
  std::atomic<uint64_t> interPresentMicros{0};
  // Per-frame stutter detection. interPresentMicros accumulates the
  // gap between consecutive Present calls (the time the game's render
  // thread spent OUTSIDE Present); the per-second sum hides single-
  // frame hitches. maxFrameMicros tracks the largest single gap seen
  // in the window; slowFrames30ms / slowFrames100ms count how many
  // frames crossed those thresholds. A 60fps row with maxFrameMicros
  // ~16 000 and slow=0 is genuinely smooth; same row with maxFrame
  // 250 000 + slow30=8 is one visible stutter per 7s.
  std::atomic<uint64_t> maxFrameMicros{0};
  std::atomic<uint64_t> slowFrames30ms{0};
  std::atomic<uint64_t> slowFrames100ms{0};
  // Cumulative D3D9 entry-point invocations per second (sum across all
  // entry points). Diagnoses "the cap is in the game's frame loop"
  // vs "the cap is in dxmt": if d3d9Calls is in the thousands but
  // present is 1/sec, the game is spinning on D3D9 work that's not
  // resulting in frame progress — points the finger at game-side
  // logic. If d3d9Calls is also tiny, the game's main loop itself is
  // gated by something outside D3D9 entirely.
  std::atomic<uint64_t> d3d9Calls{0};

  // Latency histograms — five buckets covering the per-call hot paths
  // that the cumulative-microsecond counters above can't resolve into
  // a distribution. Each histogram exposes min / mean (totalUs/count) /
  // max / count per drain window.
  //   drawEncode      — per-DrawPrimitive call (CALLING-thread: entry to queued into chunk).
  //   stateChange     — per state setter (SetRenderState / SamplerState / TSS / ...).
  //   lockCpuPin      — per LockRect CPU-pin step (malloc / page-faulting prepass).
  //   cmdbufSubmit    — per commitCurrentChunk wall time.
  //   texCreate       — per CreateTexture (Metal newTexture call).
  //   drawResolve     — per-draw ENCODE-thread cost: the whole Draw block in
  //                     the chunk lambda (ResolveBatchedDrawForChunk + PSO wait
  //                     + emit). drawEncode is the calling-thread half;
  //                     drawResolve is the encode-thread half — together they
  //                     account for 100% of per-draw work. The PSO-wait portion
  //                     of drawResolve is separately isolated by psoWaitMicros.
  // Instrumentation is per-sweep work; the histograms themselves are
  // landed here so each sweep just adds D9_HOT_RECORD call sites.
  LatencyHistogram drawEncode;
  LatencyHistogram stateChange;
  LatencyHistogram lockCpuPin;
  LatencyHistogram cmdbufSubmit;
  LatencyHistogram texCreate;
  LatencyHistogram drawResolve;

  // Live-object gauges — CURRENT count of alive d3d9 resources (NOT
  // per-second; ctor +1, dtor -1). tick() READS these (no reset). A count
  // that climbs monotonically across a gameplay capture identifies a
  // resource type that isn't being freed. Flat counts with growing memory
  // indicate working-set / per-resource-size growth instead.
  std::atomic<int64_t> liveTextures{0};
  std::atomic<int64_t> liveSurfaces{0};
  std::atomic<int64_t> liveVertexBuffers{0};
  std::atomic<int64_t> liveIndexBuffers{0};

  static D3D9HotCounters &instance();
  static bool enabled();
  static void tick(); // call once per Present
};

} // namespace dxmt

#define D9_TRACE(name)                                                                                                 \
  do {                                                                                                                 \
    static std::atomic<bool> _dxmt_seen{false};                                                                        \
    bool _dxmt_expected = false;                                                                                       \
    if (_dxmt_seen.compare_exchange_strong(_dxmt_expected, true, std::memory_order_acq_rel)) {                         \
      ::dxmt::D3D9CallTrace::OnFirstCall(name);                                                                        \
    }                                                                                                                  \
    if (::dxmt::D3D9CallTrace::trace_all()) {                                                                          \
      static std::atomic<uint32_t> _dxmt_hot{0};                                                                       \
      if ((_dxmt_hot.fetch_add(1, std::memory_order_relaxed) & 0xFFFFu) == 0)                                          \
        ::dxmt::D3D9CallTrace::OnEveryCall(name);                                                                      \
    }                                                                                                                  \
    if (::dxmt::D3D9HotCounters::enabled())                                                                            \
      ::dxmt::D3D9HotCounters::instance().d3d9Calls.fetch_add(1, std::memory_order_relaxed);                           \
  } while (0)

#define D9_HOT_BUMP(field)                                                                                             \
  do {                                                                                                                 \
    if (::dxmt::D3D9HotCounters::enabled())                                                                            \
      ::dxmt::D3D9HotCounters::instance().field.fetch_add(1, std::memory_order_relaxed);                               \
  } while (0)

#define D9_HOT_ADD(field, n)                                                                                           \
  do {                                                                                                                 \
    if (::dxmt::D3D9HotCounters::enabled())                                                                            \
      ::dxmt::D3D9HotCounters::instance().field.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);        \
  } while (0)

// Record a single µs sample into a LatencyHistogram. Same gating
// discipline as D9_HOT_BUMP: zero cost when DXMT_LOG_LEVEL=debug is
// unset.
#define D9_HOT_RECORD(field, us)                                                                                       \
  do {                                                                                                                 \
    if (::dxmt::D3D9HotCounters::enabled())                                                                            \
      ::dxmt::D3D9HotCounters::instance().field.record(static_cast<uint64_t>(us));                                     \
  } while (0)

// Live-object gauge adjust. UNCONDITIONAL (not enabled()-gated) so the count
// stays balanced regardless of when DXMT_LOG_LEVEL=debug was sampled; resource
// ctor/dtor is not a hot path, so the lone relaxed atomic is free in practice.
#define D9_GAUGE_ADD(field, delta)                                                                                     \
  ::dxmt::D3D9HotCounters::instance().field.fetch_add((delta), std::memory_order_relaxed)

// RAII scope timer — declare once at the top of the hot function:
//   D9_HOT_SCOPE(drawEncode);
// On scope exit, records the elapsed µs into the named histogram.
// Gates on construction: if DXMT_LOG_LEVEL=debug is off, the timer
// holds a null hist pointer, skips the clock read, and the dtor is
// a single null-check + return. Clock read only happens when the
// counters are actually being collected.
namespace dxmt {
struct HotScopeTimer {
  ::dxmt::LatencyHistogram *hist;
  std::chrono::steady_clock::time_point start;
  HotScopeTimer(::dxmt::LatencyHistogram &h) {
    if (::dxmt::D3D9HotCounters::enabled()) {
      hist = &h;
      start = std::chrono::steady_clock::now();
    } else {
      hist = nullptr;
    }
  }
  ~HotScopeTimer() {
    if (!hist)
      return;
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    hist->record(static_cast<uint64_t>(us));
  }
};
} // namespace dxmt

#define D9_HOT_SCOPE(field)                                                                                            \
  ::dxmt::HotScopeTimer _dxmt_hot_scope_##field {                                                                      \
    ::dxmt::D3D9HotCounters::instance().field                                                                          \
  }
