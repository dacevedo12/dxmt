/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

// Host-native unit test: per-unit sampler-state power-on defaults.
// Reference: wined3d dlls/wined3d/stateblock.c init_default_sampler_states.

#include "d3d9_state_defaults.hpp"

#include <cstdio>

namespace {

int g_failures = 0;

constexpr unsigned kUnits = 16;

void
check(
    const DWORD (&samp)[kUnits][D3DSAMP_DMAPOFFSET + 1], unsigned unit, const char *name, D3DSAMPLERSTATETYPE s,
    DWORD expected
) {
  if (samp[unit][s] == expected)
    return;
  ++g_failures;
  printf("not ok - unit %u D3DSAMP_%-16s expected %#010x, got %#010x\n", unit, name, expected, samp[unit][s]);
}

} // namespace

int
main() {
  DWORD samp[kUnits][D3DSAMP_DMAPOFFSET + 1] = {};
  dxmt::init_default_sampler_states(samp, kUnits);

  for (unsigned i = 0; i < kUnits; ++i) {
    check(samp, i, "ADDRESSU", D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    check(samp, i, "ADDRESSV", D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    check(samp, i, "ADDRESSW", D3DSAMP_ADDRESSW, D3DTADDRESS_WRAP);
    check(samp, i, "BORDERCOLOR", D3DSAMP_BORDERCOLOR, 0);
    check(samp, i, "MAGFILTER", D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    check(samp, i, "MINFILTER", D3DSAMP_MINFILTER, D3DTEXF_POINT);
    check(samp, i, "MIPFILTER", D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    check(samp, i, "MIPMAPLODBIAS", D3DSAMP_MIPMAPLODBIAS, 0);
    check(samp, i, "MAXMIPLEVEL", D3DSAMP_MAXMIPLEVEL, 0);
    check(samp, i, "MAXANISOTROPY", D3DSAMP_MAXANISOTROPY, 1);
    check(samp, i, "SRGBTEXTURE", D3DSAMP_SRGBTEXTURE, 0);
    check(samp, i, "ELEMENTINDEX", D3DSAMP_ELEMENTINDEX, 0);
    check(samp, i, "DMAPOFFSET", D3DSAMP_DMAPOFFSET, 0);
  }

  printf("%u units x 13 states, %d failure(s)\n", kUnits, g_failures);
  return g_failures ? 1 : 0;
}
