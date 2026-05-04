#pragma once

#include <atomic>
#include <cstdint>

namespace dxmt {

// Calling-thread wait observability. The queue has three places where the
// calling thread blocks:
//   chunk_ongoing.wait        — chunk-pool back-pressure, encode worker
//                               can't keep up with the calling thread's
//                               CommitCurrentChunk cadence.
//   frame_latency_fence_.wait — PresentBoundary max-latency throttle, the
//                               calling thread is ahead of the GPU by
//                               max_latency_ frames.
//   cpu_coherent.wait         — d3d9 sync paths (StretchRect /
//                               GetRenderTargetData / dtor / Reset) wait
//                               for an explicit seq retirement.
//
// Per-second totals catch the steady-state cost; per-second maxima catch
// single-frame stalls that are invisible in a sum (a 1.3 s hitch that
// happens once shows up as +1300 ms here but only +130 ms in a 10 s
// steady-state total).
//
// Bumped from dxmt::CommandQueue; read once/second from d3d9_trace.cpp's
// hotcounters tick. Both ends use plain atomic ops with relaxed ordering —
// the numbers are diagnostic, not load-bearing.
struct QueueWaitCounters {
  std::atomic<uint64_t> chunkBackpressureUs{0};
  std::atomic<uint64_t> chunkBackpressureMaxUs{0};
  std::atomic<uint64_t> chunkBackpressureCount{0};
  std::atomic<uint64_t> frameLatencyUs{0};
  std::atomic<uint64_t> frameLatencyMaxUs{0};
  std::atomic<uint64_t> cpuFenceUs{0};
  std::atomic<uint64_t> cpuFenceMaxUs{0};
  std::atomic<uint64_t> cpuFenceCount{0};

  static QueueWaitCounters &instance() {
    static QueueWaitCounters c;
    return c;
  }

  static void bump_max(std::atomic<uint64_t> &slot, uint64_t v) {
    uint64_t cur = slot.load(std::memory_order_relaxed);
    while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
  }
};

} // namespace dxmt
