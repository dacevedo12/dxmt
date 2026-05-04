#include "d3d9_tex_debug.hpp"

#include "log/log.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

namespace dxmt {

bool
D3D9TexDebug::enabled() {
  static const bool en = []() {
    const char *v = std::getenv("DXMT_D9_TEX_DEBUG");
    return v && v[0] && v[0] != '0';
  }();
  return en;
}

namespace {
// One dedupe set per call site so a Lock pattern doesn't shadow a
// Create pattern (or vice versa). Mutex is taken only on first-time
// per tuple; subsequent calls bail before locking.
std::mutex g_mtx;
std::unordered_set<uint64_t> g_seen_create;
std::unordered_set<uint64_t> g_seen_lock;
std::unordered_set<uint64_t> g_seen_draw;
} // namespace

void
D3D9TexDebug::log_create_once(
    uint32_t pool, uint32_t usage, uint32_t format, uint32_t width, uint32_t height, uint32_t levels, const char *fn
) {
  // Tuple key = (pool, usage, format). Width/Height/Levels are diagnostic
  // payload — repeating the same tuple with different dimensions is
  // expected and we don't want to spam.
  uint64_t key = (static_cast<uint64_t>(pool) << 48) | (static_cast<uint64_t>(usage) << 24) |
                 static_cast<uint64_t>(format & 0xFFFFFF);
  {
    std::lock_guard<std::mutex> g(g_mtx);
    if (!g_seen_create.insert(key).second)
      return;
  }
  char line[256];
  std::snprintf(
      line, sizeof(line), "[D9_TEX] %s pool=%u usage=0x%x format=%u w=%u h=%u levels=%u", fn, pool, usage, format,
      width, height, levels
  );
  Logger::warn(line);
}

void
D3D9TexDebug::log_lock_once(uint32_t pool, uint32_t usage, uint32_t format, uint32_t flags) {
  uint64_t key = (static_cast<uint64_t>(pool) << 56) | (static_cast<uint64_t>(usage & 0xFF) << 48) |
                 (static_cast<uint64_t>(format & 0xFFFF) << 32) | (flags & 0xFFFFFFFFu);
  {
    std::lock_guard<std::mutex> g(g_mtx);
    if (!g_seen_lock.insert(key).second)
      return;
  }
  char line[256];
  std::snprintf(
      line, sizeof(line), "[D9_TEX] LockRect pool=%u usage=0x%x format=%u flags=0x%x", pool, usage, format, flags
  );
  Logger::warn(line);
}

void
D3D9TexDebug::log_draw_once(const DrawSignature &s) {
  // FNV-1a over the whole tuple. Dedupe at this granularity: the same
  // game tends to recycle a small handful of state shapes, so the log
  // converges quickly even on a busy frame.
  uint64_t key = 0xcbf29ce484222325ull;
  auto mix = [&](uint64_t v) {
    key ^= v;
    key *= 0x100000001b3ull;
  };
  mix(s.prim_type);
  mix(s.alpha_blend_enabled);
  mix(s.src_blend_rgb);
  mix(s.dst_blend_rgb);
  mix(s.blend_op_rgb);
  mix(s.alpha_test_enabled);
  mix(s.alpha_func);
  mix(s.alpha_ref);
  mix(s.z_enabled);
  mix(s.z_write_enabled);
  mix(s.z_func);
  mix(s.cull_mode);
  mix(s.fill_mode);
  mix(s.stage0_mag_filter);
  mix(s.stage0_min_filter);
  mix(s.stage0_mip_filter);
  mix(s.stage0_addr_u);
  mix(s.stage0_addr_v);
  mix(s.stage0_texture_format);
  mix(s.stage0_texture_type);
  mix(static_cast<uint64_t>(s.vs_present) | (static_cast<uint64_t>(s.ps_present) << 1));
  {
    std::lock_guard<std::mutex> g(g_mtx);
    if (!g_seen_draw.insert(key).second)
      return;
  }
  char line[512];
  std::snprintf(
      line, sizeof(line),
      "[D9_DRAW] prim=%u ab=%u src=%u dst=%u op=%u atest=%u afn=%u aref=%u "
      "z=%u zw=%u zfn=%u cull=%u fill=%u | s0 mag=%u min=%u mip=%u u=%u v=%u "
      "fmt=%u type=%u | vs=%u ps=%u",
      s.prim_type, s.alpha_blend_enabled, s.src_blend_rgb, s.dst_blend_rgb, s.blend_op_rgb, s.alpha_test_enabled,
      s.alpha_func, s.alpha_ref, s.z_enabled, s.z_write_enabled, s.z_func, s.cull_mode, s.fill_mode,
      s.stage0_mag_filter, s.stage0_min_filter, s.stage0_mip_filter, s.stage0_addr_u, s.stage0_addr_v,
      s.stage0_texture_format, s.stage0_texture_type, s.vs_present ? 1u : 0u, s.ps_present ? 1u : 0u
  );
  Logger::warn(line);
}

} // namespace dxmt
