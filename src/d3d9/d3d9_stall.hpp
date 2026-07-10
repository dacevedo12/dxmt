#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace dxmt {

// Self-profiler for the 32-bit memory-pressure stall, gated on DXMT_D9_STALL_MS
// (frame-interval threshold in milliseconds, 0 = off). An external sampler
// cannot symbolicate this layer under x86 translation, so any per-draw stall
// reads as anonymous translated code; the only way to attribute it is to time
// the hot operations from the inside. The accumulators below are summed across
// a frame and reported by the Present path when the frame overruns.
inline uint32_t d9StallThresholdMs() {
  static const uint32_t v = []() -> uint32_t {
    if (const char *e = std::getenv("DXMT_D9_STALL_MS"); e && e[0]) {
      char *end = nullptr;
      unsigned long ms = std::strtoul(e, &end, 10);
      if (end != e)
        return static_cast<uint32_t>(ms);
    }
    return 0;
  }();
  return v;
}

// Master gate for one-shot diagnostic logging that a normal run should never
// emit (device-teardown high-water reports and the like). Read once, same
// lazy-static getenv shape as d9StallThresholdMs; any non-empty, non-"0"
// value enables it.
inline bool d9DebugEnabled() {
  static const bool on = []() {
    const char *e = std::getenv("DXMT_D9_DEBUG");
    return e && e[0] && !(e[0] == '0' && e[1] == '\0');
  }();
  return on;
}

// Boolean gate for the present-path tracer, env DXMT_D9_PRESENTDBG. Logs which
// backing each Present hands to the drawable and via which path, correlated
// against each backbuffer (re)build, so a one-frame stale present at a scene
// cut can be attributed to a specific resource. Same lazy-static shape.
inline bool d9PresentDbgEnabled() {
  static const bool on = []() {
    const char *e = std::getenv("DXMT_D9_PRESENTDBG");
    return e && e[0] && !(e[0] == '0' && e[1] == '\0');
  }();
  return on;
}

struct D9StallCounters {
  std::atomic<uint64_t> lock_ns{0};
  std::atomic<uint32_t> lock_count{0};
  std::atomic<uint32_t> draw_count{0};
  // v4 pre-present decomposition. commit = calling-thread chunk-ring
  // backpressure (CommitCurrentChunk blocking on a free chunk); pso_wait =
  // encode-thread cold PSO-link wait; create = resource-creation bodies;
  // upload = the staging ring memcpy + queue cost; record = per-draw
  // capture + queue cost.
  std::atomic<uint64_t> commit_ns{0};
  std::atomic<uint32_t> commit_count{0};
  std::atomic<uint64_t> pso_wait_ns{0};
  std::atomic<uint32_t> pso_wait_count{0};
  std::atomic<uint64_t> create_ns{0};
  std::atomic<uint32_t> create_count{0};
  std::atomic<uint64_t> upload_ns{0};
  std::atomic<uint64_t> upload_bytes{0};
  std::atomic<uint64_t> record_ns{0};
  // v6 encode-thread decomposition. resolve = ResolveBatchedDrawForChunk
  // (PSO/DSSO/sampler cache lookups, const uploads, view resolution);
  // emit = the per-draw Metal encoding (EmitCommonRenderSetup +
  // EmitDrawCommand). Together with pso_wait these cover the encode
  // thread's walker loop, which an external sampler reads only as
  // anonymous translated code.
  std::atomic<uint64_t> resolve_ns{0};
  std::atomic<uint32_t> resolve_count{0};
  std::atomic<uint64_t> emit_ns{0};
  // v5 API-poll counters + max-gap tracker. The poll counts diagnose spin
  // (an app hammering GetData / GetRasterStatus / TestCooperativeLevel while
  // the GPU catches up); the max gap is the largest hole between two
  // consecutive D3D9 events in the frame, tagged with the draw index at that
  // moment, which separates spin (many events, no big gap) from a lump of
  // game CPU between two calls (one big gap).
  std::atomic<uint32_t> getdata_count{0};
  std::atomic<uint32_t> getdata_false_count{0};
  std::atomic<uint32_t> issue_count{0};
  std::atomic<uint32_t> raster_count{0};
  std::atomic<uint32_t> tcl_count{0};
  std::atomic<uint64_t> max_gap_ns{0};
  std::atomic<uint32_t> max_gap_draw{0};
  // Deliberately non-atomic: written only by D3D9 entry points, which for a
  // single-threaded app are one thread. A multithreaded app can race this;
  // we accept it as benign for a diagnostic (a torn read mis-attributes at
  // most one frame's gap). Touched by the gated d9NoteApiEvent and re-stamped
  // at Present exit; NOT cleared by reset(), so the next frame's first gap
  // spans the Present-return-to-first-API-call window (a post-present lump).
  std::chrono::steady_clock::time_point last_api_event{};
  void reset() {
    lock_ns.store(0, std::memory_order_relaxed);
    lock_count.store(0, std::memory_order_relaxed);
    draw_count.store(0, std::memory_order_relaxed);
    commit_ns.store(0, std::memory_order_relaxed);
    commit_count.store(0, std::memory_order_relaxed);
    pso_wait_ns.store(0, std::memory_order_relaxed);
    pso_wait_count.store(0, std::memory_order_relaxed);
    create_ns.store(0, std::memory_order_relaxed);
    create_count.store(0, std::memory_order_relaxed);
    upload_ns.store(0, std::memory_order_relaxed);
    upload_bytes.store(0, std::memory_order_relaxed);
    record_ns.store(0, std::memory_order_relaxed);
    resolve_ns.store(0, std::memory_order_relaxed);
    resolve_count.store(0, std::memory_order_relaxed);
    emit_ns.store(0, std::memory_order_relaxed);
    getdata_count.store(0, std::memory_order_relaxed);
    getdata_false_count.store(0, std::memory_order_relaxed);
    issue_count.store(0, std::memory_order_relaxed);
    raster_count.store(0, std::memory_order_relaxed);
    tcl_count.store(0, std::memory_order_relaxed);
    max_gap_ns.store(0, std::memory_order_relaxed);
    max_gap_draw.store(0, std::memory_order_relaxed);
    // last_api_event is intentionally NOT reset here: Present exit re-stamps
    // it so the next frame's first gap measures the post-present window.
  }
};
inline D9StallCounters g_d9stall;

// Record a D3D9 API event for the max-gap tracker: the largest hole between
// two consecutive events in a frame, tagged with the draw index at that
// point. Diagnoses whether a pre-present stall is API spin (many events, no
// big gap) or a lump of game CPU between two calls (one big gap). Called
// from draw entry, Lock return, GetData, GetRasterStatus,
// TestCooperativeLevel, Present entry, and the staging uploads. Gated: one
// branch when off. See last_api_event for the accepted multithread race.
inline void d9NoteApiEvent() {
  if (!d9StallThresholdMs())
    return;
  auto now = std::chrono::steady_clock::now();
  if (g_d9stall.last_api_event.time_since_epoch().count()) {
    auto gap = std::chrono::duration_cast<std::chrono::nanoseconds>(now - g_d9stall.last_api_event).count();
    if (static_cast<uint64_t>(gap) > g_d9stall.max_gap_ns.load(std::memory_order_relaxed)) {
      g_d9stall.max_gap_ns.store(static_cast<uint64_t>(gap), std::memory_order_relaxed);
      g_d9stall.max_gap_draw.store(g_d9stall.draw_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
  }
  g_d9stall.last_api_event = now;
}

// Gated atomic increment for an API-poll counter.
inline void d9NotePoll(std::atomic<uint32_t> &counter) {
  if (d9StallThresholdMs())
    counter.fetch_add(1, std::memory_order_relaxed);
}

// RAII accumulator for a Lock entry point. A non-DISCARD Lock of a resource the
// GPU still references force-submits open work and waits on the completion
// event; this captures that calling-thread cost. No-op when profiling is off.
struct D9StallLockTimer {
  std::chrono::steady_clock::time_point t0;
  bool on;
  D9StallLockTimer() : on(d9StallThresholdMs() != 0) {
    if (on)
      t0 = std::chrono::steady_clock::now();
  }
  ~D9StallLockTimer() {
    if (!on)
      return;
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count();
    g_d9stall.lock_ns.fetch_add(static_cast<uint64_t>(ns), std::memory_order_relaxed);
    g_d9stall.lock_count.fetch_add(1, std::memory_order_relaxed);
    d9NoteApiEvent();
  }
};

inline void d9NoteDraw() {
  if (!d9StallThresholdMs())
    return;
  g_d9stall.draw_count.fetch_add(1, std::memory_order_relaxed);
  d9NoteApiEvent();
}

// Generic RAII accumulator for a scoped cost: on destruction adds the
// elapsed ns to *ns and, when count is non-null, bumps it. The atomics
// keep it safe on the encode thread too (the PSO cold-compile wait). No-op
// when profiling is off: the timepoint read is skipped so an unset gate
// stays zero-overhead. Backs the commit / pso-wait / create / record /
// upload timers; the lock timer keeps its own type for its distinct meaning.
struct D9StallScope {
  std::chrono::steady_clock::time_point t0;
  std::atomic<uint64_t> *ns;
  std::atomic<uint32_t> *count;
  bool on;
  D9StallScope(std::atomic<uint64_t> *ns_acc, std::atomic<uint32_t> *count_acc = nullptr)
      : ns(ns_acc), count(count_acc), on(d9StallThresholdMs() != 0) {
    if (on)
      t0 = std::chrono::steady_clock::now();
  }
  ~D9StallScope() {
    if (!on)
      return;
    auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - t0)
                       .count();
    ns->fetch_add(static_cast<uint64_t>(elapsed), std::memory_order_relaxed);
    if (count)
      count->fetch_add(1, std::memory_order_relaxed);
  }
};

// Add staged bytes to the upload accumulator (paired with a D9StallScope on
// upload_ns at the call site). Gated like d9NoteDraw so it is free when off.
inline void d9NoteUploadBytes(uint64_t bytes) {
  if (d9StallThresholdMs())
    g_d9stall.upload_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

} // namespace dxmt
