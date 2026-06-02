/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Unit test: texture_upload_layout (no device/GPU). Staging byte math for
// a texture upload. The regression of record: a 3D (volume) upload must
// stage EVERY depth slice, not just the first. Computing the total from
// width x height only left volume textures with one slice written and the
// rest zero (color-grade LUTs came out all-black). wined3d carries a
// slice pitch through its upload (texture.c wined3d_texture_get_pitch).

#include "d3d9_texture_upload.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

void
check(const char *name, dxmt::TextureUploadLayout got, uint32_t bpi, size_t total) {
  if (got.bytes_per_image == bpi && got.total_bytes == total) {
    printf("ok - %-26s bpi=%u total=%zu\n", name, bpi, total);
    return;
  }
  ++g_failures;
  printf(
      "not ok - %-22s expected bpi=%u total=%zu, got bpi=%u total=%zu\n", name, bpi, total, got.bytes_per_image,
      got.total_bytes
  );
}

} // namespace

int
main() {
  using dxmt::texture_upload_layout;

  // 2D RGBA8 256x256: pitch 1024, height 256, one slice.
  check("2d_256", texture_upload_layout(1024, 256, 1, false), 1024u * 256u, static_cast<size_t>(1024) * 256);
  // 3D RGBA8 32x32x32 (the color LUT): pitch 128, height 32, depth 32.
  // The bug (depth ignored) staged 128*32 = 4096; correct is x32.
  check("3d_32cube", texture_upload_layout(128, 32, 32, false), 128u * 32u, static_cast<size_t>(128) * 32 * 32);
  // depth 0 is treated as a single slice.
  check("depth0_is_one", texture_upload_layout(128, 32, 0, false), 128u * 32u, static_cast<size_t>(128) * 32);
  // Compressed: row count rounds up to whole 4x4 block rows (16 -> 4).
  check("compressed_rows", texture_upload_layout(64, 16, 1, true), 64u * 4u, static_cast<size_t>(64) * 4);
  // Compressed 3D scales by depth too.
  check("compressed_3d", texture_upload_layout(64, 16, 8, true), 64u * 4u, static_cast<size_t>(64) * 4 * 8);

  printf("\n%d failure(s)\n", g_failures);
  return g_failures ? 1 : 0;
}
