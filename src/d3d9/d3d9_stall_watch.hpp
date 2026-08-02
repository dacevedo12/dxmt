#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>

#include "dxmt_context.hpp"
#include "wsi_platform.hpp"
#include "log/log.hpp"
#include "util_string.hpp"

namespace dxmt::diag {

// Stall attribution. A long frame says nothing about who was stalled: kernel
// traces name the faulting address but never the writer, and per-span timers
// only ever bill the spans that already exist, so a stall in code between them
// reads as unattributed either way. What settles it is knowing whether the
// calling thread was inside this layer at all when it stopped.
//
// api_depth is non-zero exactly while a public entry point is open, and api_seq
// counts entries so a watchdog can tell "parked inside one long call" from
// "still being called, just slowly". A watchdog thread samples them only when
// the frame heartbeat stops advancing, and never touches the calling thread:
// suspending it to sample black-screens the app, because during a load the
// heartbeat legitimately stops and the sampler then suspends forever.
//
// The counters are per-thread and written with plain stores. Atomics here cost
// two read-modify-writes per entry point, which at tens of thousands of calls
// per frame is enough to change the behaviour being measured: an instrument
// that costs that much stalls the app in menus that are otherwise smooth. Each
// thread publishes the address of its own block once, and the watchdog reads
// those blocks without synchronisation. The race is benign by construction,
// because a sampler only needs a recent value, never a coherent one.
struct ApiThreadState {
  uint64_t seq = 0;
  uint32_t depth = 0;
  bool published = false;
  // Name of the outermost entry point currently open on this thread. It costs
  // one plain store because the string is a literal the compiler materialises
  // at the call site (__builtin_FUNCTION as a default argument), so naming the
  // stalling call needs no per-entry-point edits and no lookup at runtime.
  const char *fn = nullptr;
};

inline constexpr size_t kMaxWatchedThreads = 32;
inline std::atomic<ApiThreadState *> watched_threads[kMaxWatchedThreads] = {};
inline std::atomic<size_t> watched_thread_count{0};
inline thread_local ApiThreadState api_tls;
inline std::atomic<uint64_t> frame_heartbeat{0};

inline void
publishThreadState(ApiThreadState *state) {
  const size_t slot = watched_thread_count.fetch_add(1, std::memory_order_relaxed);
  if (slot < kMaxWatchedThreads)
    watched_threads[slot].store(state, std::memory_order_release);
}

// A plain bool, written once before any entry point can run and only read
// afterwards. It deliberately is not a function-local static: that spells a
// guard-variable load into every entry point in every build, and a per-call
// cost in the disabled path is exactly what this instrument must never add.
inline bool stall_watch_enabled = false;

inline void
initStallWatch() {
  const char *e = std::getenv("DXMT_D9_STALL_WATCH");
  stall_watch_enabled = e && e[0] == '1';
  dxmt::stall_probe_enabled = stall_watch_enabled;
  dxmt::deptrack_probe_enabled = stall_watch_enabled;
  dxmt::fence_high_water_enabled = stall_watch_enabled;
  WMT::wrap_registry_enabled = stall_watch_enabled;
  if (!stall_watch_enabled)
    return;
  // Matched scratch for the tracker-store comparison. The heap arena comes
  // from the same allocator the fence sets do; the reserved one is a plain
  // read-write mapping. Both are written once here so neither is fresh commit
  // when the probe first reaches it.
  constexpr uint32_t arena_pages = 1024;
  constexpr size_t arena_bytes = static_cast<size_t>(arena_pages) * DXMT_PAGE_SIZE;
  void *heap = wsi::aligned_malloc(arena_bytes, DXMT_PAGE_SIZE);
  void *reserved = VirtualAlloc(nullptr, arena_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!heap || !reserved)
    return;
  std::memset(heap, 0, arena_bytes);
  std::memset(reserved, 0, arena_bytes);
  auto **blocks = static_cast<uint64_t **>(wsi::aligned_malloc(arena_pages * sizeof(uint64_t *), 64));
  if (blocks) {
    for (uint32_t i = 0; i < arena_pages; ++i) {
      blocks[i] = static_cast<uint64_t *>(::operator new(DXMT_PAGE_SIZE));
      std::memset(blocks[i], 0, DXMT_PAGE_SIZE);
    }
    dxmt::probe_arena_blocks = blocks;
  }
  dxmt::probe_arena_heap = static_cast<uint64_t *>(heap);
  dxmt::probe_arena_reserved = static_cast<uint64_t *>(reserved);
  dxmt::probe_arena_pages = arena_pages;

  // Committed up front and handed out by a cursor bump, so the comparison
  // against the allocator call is between two ways of obtaining a block rather
  // than between two regions.
  if (void *pool = VirtualAlloc(nullptr, arena_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
    std::memset(pool, 0, arena_bytes);
    dxmt::probe_pool_base = static_cast<uint8_t *>(pool);
    dxmt::probe_pool_pages = arena_pages;
  }

  // Committed and then left alone: the store control that walks it must find
  // every page untouched, so it is the one thing here that is never memset.
  if (void *virgin = VirtualAlloc(nullptr, arena_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) {
    dxmt::probe_virgin_base = static_cast<uint8_t *>(virgin);
    dxmt::probe_virgin_pages = arena_pages;
  }

  // Address space claimed once so the recycle below never makes the kernel
  // search for a range; what is left is the map edit itself, which is the part
  // an allocator that has to extend cannot avoid.
  if (void *vm = VirtualAlloc(nullptr, arena_bytes, MEM_RESERVE, PAGE_READWRITE)) {
    static uint8_t *vm_base = static_cast<uint8_t *>(vm);
    dxmt::probe_vm_recycle = [](uint32_t page) {
      uint8_t *addr = vm_base + static_cast<size_t>(page) * DXMT_PAGE_SIZE;
      if (void *committed = VirtualAlloc(addr, DXMT_PAGE_SIZE, MEM_COMMIT, PAGE_READWRITE)) {
        volatile uint64_t *slot = static_cast<volatile uint64_t *>(committed);
        *slot = page;
        VirtualFree(addr, DXMT_PAGE_SIZE, MEM_DECOMMIT);
      }
    };
    dxmt::probe_vm_query = [] {
      MEMORY_BASIC_INFORMATION info;
      VirtualQuery(vm_base, &info, sizeof(info));
    };
  }
}

inline void
api_enter(const char *fn) {
  if (!stall_watch_enabled)
    return;
  ApiThreadState &s = api_tls;
  if (!s.published) {
    s.published = true;
    publishThreadState(&s);
  }
  ++s.seq;
  // Only the outermost call names the episode: the nested re-entries are
  // forwarding paths (Reset drives the same setters an app calls), and the
  // outer name is the one that says what the app asked for.
  if (s.depth == 0)
    s.fn = fn;
  ++s.depth;
}

// Re-labels the open entry point as it moves through its own phases, so a
// stall inside one long call names the step it is parked in rather than just
// the call. One plain store to a literal.
inline void
mark(const char *label) {
  if (!stall_watch_enabled)
    return;
  api_tls.fn = label;
}

inline void
api_exit() {
  if (!stall_watch_enabled)
    return;
  --api_tls.depth;
}

// One chunk is one Metal command buffer, so this counts the command buffers a
// frame costs and names what forced each one past the first. A frame needs only
// the one Present commits; every extra is some call that could not be expressed
// inside the open chunk, and each brings a fresh set of per-chunk tracking
// allocations with it. Labels are the same literals api_enter stores, so the
// tally compares pointers rather than strings.
inline constexpr size_t kMaxCommitReasons = 8;

struct FrameCommitTally {
  const char *reason[kMaxCommitReasons] = {};
  uint32_t count[kMaxCommitReasons] = {};
  uint32_t chunks = 0;
};

inline FrameCommitTally frame_commits;

inline void
recordChunkCommit() {
  if (!stall_watch_enabled)
    return;
  ++frame_commits.chunks;
  const char *fn = api_tls.fn ? api_tls.fn : "-";
  for (size_t i = 0; i < kMaxCommitReasons; ++i) {
    if (frame_commits.reason[i] == fn) {
      ++frame_commits.count[i];
      return;
    }
    if (!frame_commits.reason[i]) {
      frame_commits.reason[i] = fn;
      frame_commits.count[i] = 1;
      return;
    }
  }
}

// Watches frame progress and reports every episode where it stops for longer
// than kStallReportMs. The report is deliberately about the calling thread's
// position rather than the elapsed time: api_depth at the moment progress
// stopped separates "parked inside one of our calls" from "the app is away in
// its own code", and the api_seq delta across the episode separates "parked in
// one long call" from "still calling us, just slowly". Reading is all it does;
// the calling thread is never suspended.
// Renders the per-entry-point sample histogram, busiest first.
inline std::string
fnReport(const char *(&names)[8], uint32_t (&hits)[8]) {
  std::string out;
  for (size_t rank = 0; rank < 3; ++rank) {
    size_t best = 8;
    for (size_t i = 0; i < 8; ++i) {
      if (names[i] && hits[i] > 0 && (best == 8 || hits[i] > hits[best]))
        best = i;
    }
    if (best == 8)
      break;
    if (!out.empty())
      out += ",";
    out += names[best];
    out += ":";
    out += std::to_string(hits[best]);
    hits[best] = 0;
  }
  return out.empty() ? std::string("-") : out;
}

inline void
startStallWatchdog() {
  if (!stall_watch_enabled)
    return;
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      constexpr uint64_t kPollMs = 20;
      constexpr uint64_t kStallReportMs = 200;
      auto totalSeq = [] {
        uint64_t total = 0;
        const size_t n = std::min(watched_thread_count.load(std::memory_order_relaxed), kMaxWatchedThreads);
        for (size_t i = 0; i < n; ++i) {
          if (ApiThreadState *s = watched_threads[i].load(std::memory_order_acquire))
            total += s->seq;
        }
        return total;
      };
      auto insideFn = []() -> const char * {
        const size_t n = std::min(watched_thread_count.load(std::memory_order_relaxed), kMaxWatchedThreads);
        for (size_t i = 0; i < n; ++i) {
          if (ApiThreadState *s = watched_threads[i].load(std::memory_order_acquire); s && s->depth > 0)
            return s->fn ? s->fn : "?";
        }
        return nullptr;
      };
      // Which entry point the episode sat in, by sample count. Literals, so
      // pointer identity is enough to bucket them.
      const char *fn_names[8] = {};
      uint32_t fn_hits[8] = {};
      uint64_t last_beat = frame_heartbeat.load(std::memory_order_relaxed);
      uint64_t stalled_ms = 0;
      uint64_t seq_at_stall_start = 0;
      uint32_t polls_inside = 0;
      uint32_t polls_total = 0;
      bool marker_written = false;
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        const uint64_t beat = frame_heartbeat.load(std::memory_order_relaxed);
        if (beat != last_beat) {
          if (stalled_ms >= kStallReportMs) {
            // insidePermille is the share of the episode some thread spent
            // within a public entry point. It is the whole point of the
            // instrument: elapsed time alone cannot say whether the app was
            // waiting on this layer or merely running its own code slowly.
            const uint32_t permille = polls_total ? (polls_inside * 1000u) / polls_total : 0u;
            Logger::warn(
                str::format(
                    "d3d9 stall: ", stalled_ms, "ms  insidePermille=", permille, " (", polls_inside, "/", polls_total,
                    ")  apiCallsDuring=", totalSeq() - seq_at_stall_start, "  in=", fnReport(fn_names, fn_hits)
                )
            );
          }
          last_beat = beat;
          stalled_ms = 0;
          marker_written = false;
          polls_inside = 0;
          polls_total = 0;
          for (size_t i = 0; i < 8; ++i) {
            fn_names[i] = nullptr;
            fn_hits[i] = 0;
          }
          continue;
        }
        if (stalled_ms == 0) {
          seq_at_stall_start = totalSeq();
          polls_inside = 0;
          polls_total = 0;
        }
        // Drop a marker the moment an episode is long enough to be one of the
        // interesting ones, so a host-side watcher can capture thread stacks
        // while it is still happening. A stack is the only thing left that can
        // name a wait inside the driver: everything reachable from this side
        // has been measured and cleared. Written once per episode, and only a
        // few bytes, because the whole point is not to perturb the thing being
        // caught.
        if (stalled_ms == kStallReportMs && !marker_written) {
          marker_written = true;
          if (const char *dir = std::getenv("DXMT_LOG_PATH")) {
            std::string path(dir);
            if (!path.empty() && *path.rbegin() != '/')
              path += '/';
            path += "dxmt_stall_marker";
            if (FILE *marker = std::fopen(path.c_str(), "w")) {
              std::fprintf(marker, "%lu\n", (unsigned long)::GetCurrentProcessId());
              std::fclose(marker);
            }
          }
        }
        ++polls_total;
        if (const char *fn = insideFn()) {
          ++polls_inside;
          for (size_t i = 0; i < 8; ++i) {
            if (fn_names[i] == nullptr) {
              fn_names[i] = fn;
              fn_hits[i] = 1;
              break;
            }
            if (fn_names[i] == fn) {
              ++fn_hits[i];
              break;
            }
          }
        }
        stalled_ms += kPollMs;
      }
    }).detach();
  });
}

} // namespace dxmt::diag
