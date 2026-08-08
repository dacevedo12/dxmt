/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "d3d9.h"

#include <cstdint>

// Pure gamma-ramp helpers factored out of the swapchain, so the identity
// synthesis and the windowed-vs-fullscreen apply decision stand free of any
// Metal presenter (the d3d9_viewport.hpp /
// d3d9_create_validation.hpp shape). References: wined3d (PRIMARY)
// dlls/wined3d/swapchain.c wined3d_swapchain_set_gamma_ramp pushes the ramp to
// the OS output (display-wide, no windowed gate); DXVK (secondary)
// src/d3d9/d3d9_swapchain.cpp SetGammaRamp (:699-735) applies the blitter LUT
// only when the ramp is non-identity and !Windowed, else clears it. dxmt
// follows DXVK because it advertises only D3DCAPS2_FULLSCREENGAMMA, so gamma is
// a fullscreen-exclusive display ramp here.

namespace dxmt {

// The D3D9 identity ramp entry: index i maps linearly across [0, 65535] as
// i * 257 (257 * 255 == 65535). DXVK's MapGammaControlPoint(i / 255.0f) rounds
// to the same uint16. One source of truth for both synthesizing a
// Get-before-Set response and detecting an identity Set the presenter can skip.
inline WORD
IdentityGammaEntry(uint32_t i) {
  return static_cast<WORD>(i * 257);
}

// Fill a ramp with the identity so a Get before any Set still round-trips a
// sensible value (the value native returns off a freshly created device).
inline void
SynthesizeIdentityGammaRamp(D3DGAMMARAMP &ramp) {
  for (uint32_t i = 0; i < 256; ++i) {
    WORD v = IdentityGammaEntry(i);
    ramp.red[i] = v;
    ramp.green[i] = v;
    ramp.blue[i] = v;
  }
}

// True when every channel is the identity ramp. An identity Set is a no-op the
// presenter can skip (F3-D2; DXVK does the same identity detect before pushing).
inline bool
IsIdentityGammaRamp(const D3DGAMMARAMP &ramp) {
  for (uint32_t i = 0; i < 256; ++i) {
    WORD v = IdentityGammaEntry(i);
    if (ramp.red[i] != v || ramp.green[i] != v || ramp.blue[i] != v)
      return false;
  }
  return true;
}

// The stored ramp is applied to the present only in fullscreen and only when it
// is non-identity. A windowed present shows no gamma (native
// gamma is the fullscreen display ramp D3DCAPS2_FULLSCREENGAMMA names), and an
// identity ramp changes nothing; either way the presenter runs LUT-free. DXVK
// gates its blitter LUT the same way (d3d9_swapchain.cpp SetGammaRamp).
inline bool
ShouldApplyGammaRamp(const D3DGAMMARAMP &ramp, bool windowed) {
  return !windowed && !IsIdentityGammaRamp(ramp);
}

} // namespace dxmt
