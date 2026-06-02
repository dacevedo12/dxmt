/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Host-native unit test: per-stage texture-stage-state power-on defaults.
// Exercises init_default_texture_stage_states() directly. Expected values
// from Wine's d3d9 conformance suite; stage 0 gets MODULATE/SELECTARG1,
// later stages DISABLE, TEXCOORDINDEX defaults to stage index.

#include "d3d9_state_defaults.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

constexpr unsigned kStages = 8;

void
check(
    const DWORD (&tss)[kStages][D3DTSS_CONSTANT + 1], unsigned stage, const char *name, D3DTEXTURESTAGESTATETYPE s,
    DWORD expected
) {
  if (tss[stage][s] == expected)
    return;
  ++g_failures;
  printf("not ok - stage %u D3DTSS_%-22s expected %#010x, got %#010x\n", stage, name, expected, tss[stage][s]);
}

} // namespace

int
main() {
  DWORD tss[kStages][D3DTSS_CONSTANT + 1] = {};
  dxmt::init_default_texture_stage_states(tss, kStages);

  for (unsigned i = 0; i < kStages; ++i) {
    check(tss, i, "COLOROP", D3DTSS_COLOROP, i ? D3DTOP_DISABLE : D3DTOP_MODULATE);
    check(tss, i, "COLORARG1", D3DTSS_COLORARG1, D3DTA_TEXTURE);
    check(tss, i, "COLORARG2", D3DTSS_COLORARG2, D3DTA_CURRENT);
    check(tss, i, "ALPHAOP", D3DTSS_ALPHAOP, i ? D3DTOP_DISABLE : D3DTOP_SELECTARG1);
    check(tss, i, "ALPHAARG1", D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    check(tss, i, "ALPHAARG2", D3DTSS_ALPHAARG2, D3DTA_CURRENT);
    check(tss, i, "BUMPENVMAT00", D3DTSS_BUMPENVMAT00, 0);
    check(tss, i, "BUMPENVMAT01", D3DTSS_BUMPENVMAT01, 0);
    check(tss, i, "BUMPENVMAT10", D3DTSS_BUMPENVMAT10, 0);
    check(tss, i, "BUMPENVMAT11", D3DTSS_BUMPENVMAT11, 0);
    check(tss, i, "TEXCOORDINDEX", D3DTSS_TEXCOORDINDEX, i);
    check(tss, i, "BUMPENVLSCALE", D3DTSS_BUMPENVLSCALE, 0);
    check(tss, i, "BUMPENVLOFFSET", D3DTSS_BUMPENVLOFFSET, 0);
    check(tss, i, "TEXTURETRANSFORMFLAGS", D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    check(tss, i, "COLORARG0", D3DTSS_COLORARG0, D3DTA_CURRENT);
    check(tss, i, "ALPHAARG0", D3DTSS_ALPHAARG0, D3DTA_CURRENT);
    check(tss, i, "RESULTARG", D3DTSS_RESULTARG, D3DTA_CURRENT);
    check(tss, i, "CONSTANT", D3DTSS_CONSTANT, 0);
  }

  printf("%u stages x 18 states, %d failure(s)\n", kStages, g_failures);
  return g_failures ? 1 : 0;
}
