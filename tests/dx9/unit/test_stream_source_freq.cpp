/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Unit test: SetStreamSourceFreq validation gate. Tests from Wine's
// d3d9 conformance suite (dlls/d3d9/tests/visual.c). Note: native D3D9
// allows plain frequency != 1 and INSTANCEDATA|0; GPU clamps step >= 1.

#include "d3d9_validation.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

struct Case {
  const char *name;
  UINT stream;
  UINT setting;
  HRESULT expected;
};

// Verbatim from Wine visual.c stream_test (lines ~12262-12291).
const Case g_cases[] = {
    {"plain freq 1, stream 1", 1, 1, D3D_OK},
    {"INSTANCEDATA|1 on stream 0", 0, D3DSTREAMSOURCE_INSTANCEDATA | 1, D3DERR_INVALIDCALL},
    {"zero setting", 1, 0, D3DERR_INVALIDCALL},
    {"plain freq 2 (no flags)", 1, 2, D3D_OK},
    {"INDEXEDDATA | 0", 1, D3DSTREAMSOURCE_INDEXEDDATA | 0, D3D_OK},
    {"INSTANCEDATA | 0", 1, D3DSTREAMSOURCE_INSTANCEDATA | 0, D3D_OK},
    {"INSTANCEDATA | INDEXEDDATA", 1, D3DSTREAMSOURCE_INSTANCEDATA | D3DSTREAMSOURCE_INDEXEDDATA | 0,
     D3DERR_INVALIDCALL},
};

} // namespace

int
main() {
  const size_t n = sizeof(g_cases) / sizeof(g_cases[0]);
  for (size_t i = 0; i < n; ++i) {
    const Case &c = g_cases[i];
    HRESULT got = dxmt::validate_stream_source_freq(c.stream, c.setting);
    if (got == c.expected)
      continue;
    ++g_failures;
    printf(
        "not ok - %-28s (stream %u, setting %#010x): expected %#010lx, got %#010lx\n", c.name, c.stream, c.setting,
        (unsigned long)c.expected, (unsigned long)got
    );
  }

  printf("%zu case(s), %d failure(s)\n", n, g_failures);
  return g_failures ? 1 : 0;
}
