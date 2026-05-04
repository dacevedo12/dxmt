#include "d3d9_trace.hpp"

#include "dxmt_queue_waits.hpp"
#include "log/log.hpp"
#include "winemetal.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace dxmt {

void
D3D9CallTrace::OnFirstCall(const char *name) {
  Logger::warn(std::string("first call: ") + name);
}

D3D9HotCounters &
D3D9HotCounters::instance() {
  static D3D9HotCounters c;
  return c;
}

bool
D3D9HotCounters::enabled() {
  // Cached on first call. The env-var lookup is non-trivial under wine
  // and we touch this from the hot path.
  static const bool en = std::getenv("DXMT_D9_HOTCOUNTERS") != nullptr;
  return en;
}

void
D3D9HotCounters::tick() {
  if (!enabled())
    return;
  using clock = std::chrono::steady_clock;
  static auto last = clock::now();
  auto now = clock::now();
  if (now - last < std::chrono::seconds(1))
    return;
  last = now;
  auto &c = instance();
  uint64_t lock_rect = c.texLockRect.exchange(0, std::memory_order_relaxed);
  uint64_t uploads = c.texUnlockUpload.exchange(0, std::memory_order_relaxed);
  uint64_t uploads_us = c.texUnlockUploadMicros.exchange(0, std::memory_order_relaxed);
  uint64_t buf_lock = c.bufLock.exchange(0, std::memory_order_relaxed);
  uint64_t pso_subs = c.psoCompileSubmits.exchange(0, std::memory_order_relaxed);
  uint64_t pso_wait = c.psoWaitFirst.exchange(0, std::memory_order_relaxed);
  uint64_t pso_us = c.psoWaitMicros.exchange(0, std::memory_order_relaxed);
  uint64_t blit_cb = c.blitCmdbufCommits.exchange(0, std::memory_order_relaxed);
  uint64_t draws = c.drawCalls.exchange(0, std::memory_order_relaxed);
  uint64_t draws_dp = c.drawCallsDP.exchange(0, std::memory_order_relaxed);
  uint64_t draws_dip = c.drawCallsDIP.exchange(0, std::memory_order_relaxed);
  uint64_t draws_up = c.drawCallsUP.exchange(0, std::memory_order_relaxed);
  uint64_t draws_upi = c.drawCallsUPI.exchange(0, std::memory_order_relaxed);
  uint64_t pres_calls = c.presentCalls.exchange(0, std::memory_order_relaxed);
  uint64_t pres_total_us = c.presentTotalMicros.exchange(0, std::memory_order_relaxed);
  uint64_t pres_encode_us = c.presentEncodeMicros.exchange(0, std::memory_order_relaxed);
  uint64_t flush_us = c.flushOpenWorkMicros.exchange(0, std::memory_order_relaxed);
  uint64_t enc_calls = c.encodeCommandsCalls.exchange(0, std::memory_order_relaxed);
  uint64_t enc_us = c.encodeCommandsMicros.exchange(0, std::memory_order_relaxed);
  uint64_t enc_records = c.encodeCommandsRecords.exchange(0, std::memory_order_relaxed);
  uint64_t pcommit_us = c.presentCmdbufCommitMicros.exchange(0, std::memory_order_relaxed);
  uint64_t pboundary_us = c.presentBoundaryMicros.exchange(0, std::memory_order_relaxed);
  uint64_t ndrawable_us = c.nextDrawableMicros.exchange(0, std::memory_order_relaxed);
  uint64_t inter_present_us = c.interPresentMicros.exchange(0, std::memory_order_relaxed);
  uint64_t max_frame_us = c.maxFrameMicros.exchange(0, std::memory_order_relaxed);
  uint64_t slow_30 = c.slowFrames30ms.exchange(0, std::memory_order_relaxed);
  uint64_t slow_100 = c.slowFrames100ms.exchange(0, std::memory_order_relaxed);
  uint64_t d3d9_calls = c.d3d9Calls.exchange(0, std::memory_order_relaxed);
  // Calling-thread wait observability: chunk-pool back-pressure,
  // PresentBoundary max-latency, cpu_coherent sync waits. Max captures
  // single-frame hitches that a per-second sum hides.
  auto &qw = dxmt::QueueWaitCounters::instance();
  uint64_t cb_us = qw.chunkBackpressureUs.exchange(0, std::memory_order_relaxed);
  uint64_t cb_max = qw.chunkBackpressureMaxUs.exchange(0, std::memory_order_relaxed);
  uint64_t cb_n = qw.chunkBackpressureCount.exchange(0, std::memory_order_relaxed);
  uint64_t fl_us = qw.frameLatencyUs.exchange(0, std::memory_order_relaxed);
  uint64_t fl_max = qw.frameLatencyMaxUs.exchange(0, std::memory_order_relaxed);
  uint64_t cf_us = qw.cpuFenceUs.exchange(0, std::memory_order_relaxed);
  uint64_t cf_max = qw.cpuFenceMaxUs.exchange(0, std::memory_order_relaxed);
  uint64_t cf_n = qw.cpuFenceCount.exchange(0, std::memory_order_relaxed);
  // winemetal-side snapshot: every wine_unix_call dxmt dispatched in the
  // last second, split per dispatch code. The aggregate is the total
  // across all 256 slots; the per-code breakdown lets us identify
  // which specific cross-WoW64 sites dominate the density — at ~5µs
  // Rosetta thunk per dispatch, a single hot code firing 6000×/frame
  // is half the frame budget. The top-N are emitted as an extra line
  // below alongside the high-level counters.
  unsigned long long unix_breakdown[256] = {};
#ifdef _WIN32
  uint64_t unix_calls = winemetal_consume_unix_call_breakdown(unix_breakdown);
#else
  uint64_t unix_calls = 0;
#endif
  char line[768];
  std::snprintf(
      line, sizeof(line),
      "hotcounters/sec: lockRect=%llu texUnlockUpload=%llu texUnlockUploadMs=%llu bufLock=%llu psoSubmit=%llu "
      "psoWaitFirst=%llu psoWaitMs=%llu blitCB=%llu draws=%llu drawsDP=%llu drawsDIP=%llu drawsUP=%llu drawsUPI=%llu "
      "present=%llu presentMs=%llu "
      "encodeMs=%llu flushMs=%llu encCmds=%llu encCmdsUs=%llu encRecs=%llu pCommitUs=%llu pBoundaryUs=%llu "
      "ndrawableUs=%llu interPresentMs=%llu d3d9Calls=%llu unixCalls=%llu "
      "maxFrameMs=%llu slow30=%llu slow100=%llu "
      "chunkBP=%llu/%lluus(max=%lluus) frameLat=%lluus(max=%lluus) cpuFence=%llu/%lluus(max=%lluus)",
      (unsigned long long)lock_rect, (unsigned long long)uploads, (unsigned long long)(uploads_us / 1000),
      (unsigned long long)buf_lock, (unsigned long long)pso_subs, (unsigned long long)pso_wait,
      (unsigned long long)(pso_us / 1000), (unsigned long long)blit_cb, (unsigned long long)draws,
      (unsigned long long)draws_dp, (unsigned long long)draws_dip, (unsigned long long)draws_up,
      (unsigned long long)draws_upi, (unsigned long long)pres_calls, (unsigned long long)(pres_total_us / 1000),
      (unsigned long long)(pres_encode_us / 1000), (unsigned long long)(flush_us / 1000), (unsigned long long)enc_calls,
      (unsigned long long)enc_us, (unsigned long long)enc_records, (unsigned long long)pcommit_us,
      (unsigned long long)pboundary_us, (unsigned long long)ndrawable_us, (unsigned long long)(inter_present_us / 1000),
      (unsigned long long)d3d9_calls, (unsigned long long)unix_calls, (unsigned long long)(max_frame_us / 1000),
      (unsigned long long)slow_30, (unsigned long long)slow_100, (unsigned long long)cb_n, (unsigned long long)cb_us,
      (unsigned long long)cb_max, (unsigned long long)fl_us, (unsigned long long)fl_max, (unsigned long long)cf_n,
      (unsigned long long)cf_us, (unsigned long long)cf_max
  );
  Logger::warn(line);
  // Top-N wine_unix_call codes for the same window. The codes are
  // indices into __wine_unix_call_funcs[] in
  // src/winemetal/unix/winemetal_unix.c — grep that table for the
  // matching function name (e.g. code=18 is _MTLDevice_newBuffer).
  // Coalescing the top sites is the highest-leverage CPU-bound lift:
  // each entry costs ~5µs of Rosetta thunk on x86_32 WoW64, so a code
  // firing 6000×/frame at 16 fps eats 30ms of every frame.
  {
    constexpr unsigned kTopN = 10;
    unsigned top_idx[kTopN] = {0};
    uint64_t top_cnt[kTopN] = {0};
    for (unsigned i = 0; i < 256; ++i) {
      uint64_t v = unix_breakdown[i];
      if (v == 0)
        continue;
      // Insertion-sort into the top-N (descending by count). N=10 ⇒
      // worst-case 10 compares per non-zero slot, ~150 non-zero slots
      // in practice (well under 256).
      for (unsigned k = 0; k < kTopN; ++k) {
        if (v > top_cnt[k]) {
          for (unsigned j = kTopN - 1; j > k; --j) {
            top_cnt[j] = top_cnt[j - 1];
            top_idx[j] = top_idx[j - 1];
          }
          top_cnt[k] = v;
          top_idx[k] = i;
          break;
        }
      }
    }
    char ubreak[512];
    int off = std::snprintf(ubreak, sizeof(ubreak), "unixCalls/sec top: ");
    for (unsigned k = 0; k < kTopN && top_cnt[k] != 0 && off < (int)sizeof(ubreak); ++k) {
      off += std::snprintf(
          ubreak + off, sizeof(ubreak) - off, "%s%u=%llu", k == 0 ? "" : " ", top_idx[k], (unsigned long long)top_cnt[k]
      );
    }
    Logger::warn(ubreak);
  }
  // Drain the 5 latency histograms and emit one summary line. Each
  // bucket reports n / mean / max / min in µs; "n=0" means no samples
  // landed in this window (which is the case until Phase-1 sweeps
  // start adding D9_HOT_SCOPE / D9_HOT_RECORD call sites).
  auto drain_hist = [](LatencyHistogram &h, uint64_t &n, uint64_t &total, uint64_t &mx, uint64_t &mn) {
    n = h.count.exchange(0, std::memory_order_relaxed);
    total = h.totalUs.exchange(0, std::memory_order_relaxed);
    mx = h.maxUs.exchange(0, std::memory_order_relaxed);
    mn = h.minUs.exchange(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
  };
  struct {
    const char *name;
    uint64_t n, total, mx, mn;
  } hists[5] = {
      {"drawEncode", 0, 0, 0, 0},   {"stateChange", 0, 0, 0, 0}, {"lockCpuPin", 0, 0, 0, 0},
      {"cmdbufSubmit", 0, 0, 0, 0}, {"texCreate", 0, 0, 0, 0},
  };
  drain_hist(c.drawEncode, hists[0].n, hists[0].total, hists[0].mx, hists[0].mn);
  drain_hist(c.stateChange, hists[1].n, hists[1].total, hists[1].mx, hists[1].mn);
  drain_hist(c.lockCpuPin, hists[2].n, hists[2].total, hists[2].mx, hists[2].mn);
  drain_hist(c.cmdbufSubmit, hists[3].n, hists[3].total, hists[3].mx, hists[3].mn);
  drain_hist(c.texCreate, hists[4].n, hists[4].total, hists[4].mx, hists[4].mn);
  char hline[640];
  int hoff = std::snprintf(hline, sizeof(hline), "latency/sec:");
  for (auto &h : hists) {
    uint64_t mean = h.n ? (h.total / h.n) : 0;
    uint64_t mn = h.n ? h.mn : 0;
    hoff += std::snprintf(
        hline + hoff, sizeof(hline) - hoff, " %s(n=%llu mean=%lluus max=%lluus min=%lluus)", h.name,
        (unsigned long long)h.n, (unsigned long long)mean, (unsigned long long)h.mx, (unsigned long long)mn
    );
  }
  Logger::warn(hline);
}

} // namespace dxmt
