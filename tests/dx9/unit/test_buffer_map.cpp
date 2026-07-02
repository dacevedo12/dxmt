/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Unit test: buffer map-mode selection, Lock-flag sanitisation, and
// staged-buffer dirty-range tracking (no device/GPU). Expected values
// mined from DXVK d3d9_common_buffer.h / d3d9_device.cpp, the de-facto
// D3D9 specification for the direct-vs-staged buffer split.

#include "d3d9_buffer_map.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void
check(const char *name, bool ok) {
  if (ok) {
    printf("ok - %s\n", name);
    return;
  }
  ++g_failures;
  printf("not ok - %s\n", name);
}

void
check_range(const char *name, dxmt::D3D9BufferRange got, uint32_t min, uint32_t max) {
  if (got.min == min && got.max == max) {
    printf("ok - %-30s [%u,%u)\n", name, min, max);
    return;
  }
  ++g_failures;
  printf("not ok - %-26s expected [%u,%u), got [%u,%u)\n", name, min, max, got.min, got.max);
}

} // namespace

int
main() {
  using namespace dxmt;

  // ---- Map mode ----
  check(
      "map default+dynamic is direct",
      determine_buffer_map_mode(D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY) == D3D9BufferMapMode::Direct
  );
  check(
      "map default static is buffer",
      determine_buffer_map_mode(D3DPOOL_DEFAULT, D3DUSAGE_WRITEONLY) == D3D9BufferMapMode::Buffer
  );
  check(
      "map managed is buffer",
      determine_buffer_map_mode(D3DPOOL_MANAGED, D3DUSAGE_WRITEONLY) == D3D9BufferMapMode::Buffer
  );
  check("map sysmem is buffer", determine_buffer_map_mode(D3DPOOL_SYSTEMMEM, 0) == D3D9BufferMapMode::Buffer);
  check(
      "map managed+dynamic is buffer",
      determine_buffer_map_mode(D3DPOOL_MANAGED, D3DUSAGE_DYNAMIC) == D3D9BufferMapMode::Buffer
  );

  // ---- Flag sanitisation ----
  // DISCARD survives alone in DEFAULT.
  check(
      "discard survives alone in default",
      sanitize_buffer_lock_flags(D3DLOCK_DISCARD, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, false) & D3DLOCK_DISCARD
  );
  // DISCARD drops when NOOVERWRITE rides along.
  check(
      "discard drops with nooverwrite",
      !(sanitize_buffer_lock_flags(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, false) &
        D3DLOCK_DISCARD)
  );
  // DISCARD drops when READONLY rides along.
  check(
      "discard drops with readonly",
      !(sanitize_buffer_lock_flags(D3DLOCK_DISCARD | D3DLOCK_READONLY, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, false) &
        D3DLOCK_DISCARD)
  );
  // DISCARD / NOOVERWRITE drop outside DEFAULT.
  check(
      "discard drops in managed",
      !(sanitize_buffer_lock_flags(D3DLOCK_DISCARD, D3DPOOL_MANAGED, 0, false) & D3DLOCK_DISCARD)
  );
  check(
      "nooverwrite drops in sysmem",
      !(sanitize_buffer_lock_flags(D3DLOCK_NOOVERWRITE, D3DPOOL_SYSTEMMEM, 0, false) & D3DLOCK_NOOVERWRITE)
  );
  check(
      "nooverwrite survives in default",
      sanitize_buffer_lock_flags(D3DLOCK_NOOVERWRITE, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, false) & D3DLOCK_NOOVERWRITE
  );
  // DONOTWAIT drops for DYNAMIC, survives otherwise.
  check(
      "donotwait drops for dynamic",
      !(sanitize_buffer_lock_flags(D3DLOCK_DONOTWAIT, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, false) & D3DLOCK_DONOTWAIT)
  );
  check(
      "donotwait survives for static default",
      sanitize_buffer_lock_flags(D3DLOCK_DONOTWAIT, D3DPOOL_DEFAULT, 0, false) & D3DLOCK_DONOTWAIT
  );
  // READONLY survives only in MANAGED.
  check(
      "readonly survives in managed",
      sanitize_buffer_lock_flags(D3DLOCK_READONLY, D3DPOOL_MANAGED, 0, false) & D3DLOCK_READONLY
  );
  check(
      "readonly drops in sysmem",
      !(sanitize_buffer_lock_flags(D3DLOCK_READONLY, D3DPOOL_SYSTEMMEM, 0, false) & D3DLOCK_READONLY)
  );

  // A software-vertex-processing-only device never honours DISCARD or
  // NOOVERWRITE, regardless of pool and usage: native keeps the lock
  // pointer stable there.
  check(
      "swvp_drops_discard",
      !(sanitize_buffer_lock_flags(D3DLOCK_DISCARD, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, true) & D3DLOCK_DISCARD)
  );
  check(
      "swvp_drops_nooverwrite",
      !(sanitize_buffer_lock_flags(D3DLOCK_NOOVERWRITE, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC, true) &
        D3DLOCK_NOOVERWRITE)
  );
  check(
      "readonly drops in default",
      !(sanitize_buffer_lock_flags(D3DLOCK_READONLY, D3DPOOL_DEFAULT, 0, false) & D3DLOCK_READONLY)
  );

  // ---- respectUserBounds ----
  check(
      "managed sub-range respects bounds",
      buffer_lock_respects_bounds(0, 64, D3DPOOL_MANAGED, 0)
  );
  check(
      "dynamic sub-range respects bounds",
      buffer_lock_respects_bounds(0, 64, D3DPOOL_DEFAULT, D3DUSAGE_DYNAMIC)
  );
  check(
      "sysmem static ignores bounds",
      !buffer_lock_respects_bounds(0, 64, D3DPOOL_SYSTEMMEM, 0)
  );
  check(
      "zero size ignores bounds",
      !buffer_lock_respects_bounds(0, 0, D3DPOOL_MANAGED, 0)
  );
  check(
      "discard ignores bounds",
      !buffer_lock_respects_bounds(D3DLOCK_DISCARD, 64, D3DPOOL_MANAGED, 0)
  );

  // ---- Dirty range ----
  // MANAGED honours the sub-range.
  check_range(
      "managed sub-range", buffer_lock_dirty_range(0, 100, 200, 1024, D3DPOOL_MANAGED, 0), 100, 300
  );
  // The sub-range is capped at the buffer end.
  check_range(
      "sub-range clamped to end", buffer_lock_dirty_range(0, 900, 400, 1024, D3DPOOL_MANAGED, 0), 900, 1024
  );
  // SYSTEMMEM static ignores the sub-range and takes the whole buffer.
  check_range(
      "sysmem whole buffer", buffer_lock_dirty_range(0, 100, 200, 1024, D3DPOOL_SYSTEMMEM, 0), 0, 1024
  );
  // Zero size takes the whole buffer.
  check_range(
      "zero size whole buffer", buffer_lock_dirty_range(0, 0, 0, 1024, D3DPOOL_MANAGED, 0), 0, 1024
  );
  // An OffsetToLock past the end (which the runtime does not reject) must
  // clamp to the end and degenerate to empty, not wrap the subtraction.
  check_range(
      "offset past end is empty", buffer_lock_dirty_range(0, 2000, 64, 1024, D3DPOOL_MANAGED, 0), 1024, 1024
  );
  // Offset in range with offset + size past the end clamps to the end.
  check_range(
      "offset+size past end clamps", buffer_lock_dirty_range(0, 1000, 500, 1024, D3DPOOL_MANAGED, 0), 1000, 1024
  );

  // ---- Conjoin ----
  {
    D3D9BufferRange r;
    check("fresh range empty", r.empty());
    r.conjoin({100, 200});
    check_range("conjoin into empty", r, 100, 200);
    r.conjoin({300, 400});
    check_range("conjoin disjoint grows span", r, 100, 400);
    r.conjoin({50, 120});
    check_range("conjoin lower grows min", r, 50, 400);
    r.clear();
    check("cleared range empty", r.empty());
  }

  // ---- updateDirtyRange ----
  check("default updates dirty", buffer_lock_updates_dirty(0, D3DPOOL_DEFAULT));
  check(
      "default ignores no-dirty-update",
      buffer_lock_updates_dirty(D3DLOCK_NO_DIRTY_UPDATE, D3DPOOL_DEFAULT)
  );
  check(
      "managed honours no-dirty-update",
      !buffer_lock_updates_dirty(D3DLOCK_NO_DIRTY_UPDATE, D3DPOOL_MANAGED)
  );
  check(
      "readonly skips dirty update",
      !buffer_lock_updates_dirty(D3DLOCK_READONLY, D3DPOOL_MANAGED)
  );
  check("managed plain updates dirty", buffer_lock_updates_dirty(0, D3DPOOL_MANAGED));

  // ---- Lock count / outer-unlock upload ----
  {
    D3D9BufferLockCount lc;
    check("first lock count 1", lc.increment() == 1);
    check("nested lock count 2", lc.increment() == 2);
    check("inner unlock count 1", lc.decrement() == 1);
    check("outer unlock count 0", lc.decrement() == 0);
    check("unbalanced unlock clamps at 0", lc.decrement() == 0);
  }
  check("upload on outer unlock with dirty", buffer_unlock_should_upload(0, /*dirty_empty=*/false));
  check("no upload while still locked", !buffer_unlock_should_upload(1, /*dirty_empty=*/false));
  check("no upload when clean", !buffer_unlock_should_upload(0, /*dirty_empty=*/true));

  printf("\n%d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
