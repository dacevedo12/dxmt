#pragma once

#include <cstdint>

namespace dxmt {

// Texture-pathway diagnostic logging for the D3D9 path.
//
// d3d9-era games are black boxes; when a visual regression shows up
// (NFS:MW's 3D-world flicker, LoL's slideshow) we want to know *which*
// texture pool / usage / format combos the game is actually exercising
// before we commit to a fix. Two log call sites: CreateTexture-class
// entries (first-time per (Pool, Usage, Format)) and LockRect entries
// (first-time per (Pool, Usage, Format, Flags)).
//
// Gated by DXMT_D9_TEX_DEBUG=1. When unset, every call below folds to
// a single cached enabled() check — near-zero overhead in hot paths
// (LockRect fires hundreds of times per frame during streaming).
//
// Keep in tree as durable infra per feedback_d3d9_debug_infra.md.
class D3D9TexDebug {
public:
  static bool enabled();

  static void log_create_once(
      uint32_t pool, uint32_t usage, uint32_t format, uint32_t width, uint32_t height, uint32_t levels, const char *fn
  );
  static void log_lock_once(uint32_t pool, uint32_t usage, uint32_t format, uint32_t flags);

  // Per-draw signature dedupe — first time we see a (prim, blend, alpha-
  // test, depth, stage-0 sampler+texture) tuple, dump its full state.
  // Repeat draws with the same signature are silent. Lets us inventory
  // the actual state shapes a game exercises without per-call spam.
  struct DrawSignature {
    uint32_t prim_type;
    uint32_t alpha_blend_enabled;
    uint32_t src_blend_rgb;
    uint32_t dst_blend_rgb;
    uint32_t blend_op_rgb;
    uint32_t alpha_test_enabled;
    uint32_t alpha_func;
    uint32_t alpha_ref;
    uint32_t z_enabled;
    uint32_t z_write_enabled;
    uint32_t z_func;
    uint32_t cull_mode;
    uint32_t fill_mode;
    uint32_t stage0_mag_filter;
    uint32_t stage0_min_filter;
    uint32_t stage0_mip_filter;
    uint32_t stage0_addr_u;
    uint32_t stage0_addr_v;
    uint32_t stage0_texture_format; // D3DFORMAT
    uint32_t stage0_texture_type;   // D3DRESOURCETYPE
    bool vs_present;
    bool ps_present;
  };
  static void log_draw_once(const DrawSignature &sig);
};

} // namespace dxmt

#define D9_TEX_CREATE(width, height, levels, usage, format, pool, fn)                                                  \
  do {                                                                                                                 \
    if (::dxmt::D3D9TexDebug::enabled())                                                                               \
      ::dxmt::D3D9TexDebug::log_create_once(                                                                           \
          static_cast<uint32_t>(pool), static_cast<uint32_t>(usage), static_cast<uint32_t>(format),                    \
          static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(levels), (fn)             \
      );                                                                                                               \
  } while (0)

#define D9_TEX_LOCK(pool, usage, format, flags)                                                                        \
  do {                                                                                                                 \
    if (::dxmt::D3D9TexDebug::enabled())                                                                               \
      ::dxmt::D3D9TexDebug::log_lock_once(                                                                             \
          static_cast<uint32_t>(pool), static_cast<uint32_t>(usage), static_cast<uint32_t>(format),                    \
          static_cast<uint32_t>(flags)                                                                                 \
      );                                                                                                               \
  } while (0)

#define D9_TEX_DRAW(sig)                                                                                               \
  do {                                                                                                                 \
    if (::dxmt::D3D9TexDebug::enabled())                                                                               \
      ::dxmt::D3D9TexDebug::log_draw_once(sig);                                                                        \
  } while (0)
