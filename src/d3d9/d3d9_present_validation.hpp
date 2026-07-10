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

// The D3DPRESENT_PARAMETERS accept/reject matrix, kept apart from
// d3d9_interface.cpp (which pulls in Metal and wsi) so that deciding whether a
// set of parameters is valid needs neither a device nor a Metal context: the
// rule set is a pure function of its arguments and reads as one, in the shape of
// d3d9_viewport.hpp and d3d9_create_validation.hpp. The canonicalisation pass
// (CanonicalisePresentParams) stays in the .cpp: it depends on D3DFormatToMetal
// and the wsi adapter-mode list, so it could not follow. Reference: wined3d
// (PRIMARY) dlls/d3d9/device.c wined3d_swapchain_desc_from_d3d9; DXVK
// (secondary) src/d3d9/d3d9_swapchain.cpp swap-effect / backbuffer-count gates.

namespace dxmt {

// Spec-shape validation for D3DPRESENT_PARAMETERS. Rejects out-of-range
// SwapEffect (Ex adds FLIPEX), BackBufferCount (cap 3 non-Ex / 30 Ex, plus the
// COPY single-backbuffer rule), and PresentationInterval. Multisampling is not
// validated here: the backbuffer is presented single-sampled regardless, so an
// MSAA request is accepted and downgraded rather than rejected. Pure read.
//
// Returns the name of the field that failed, or nullptr when the params are
// acceptable, so a caller can say which rule rejected a create or a Reset. A
// bare D3DERR_INVALIDCALL out of CreateDevice is indistinguishable from a dozen
// other rejections in a log, and the app reports it as a flat failure.
inline const char *
PresentParamsRejectReason(const D3DPRESENT_PARAMETERS &p, bool isEx) {
  D3DSWAPEFFECT highestSwapEffect = isEx ? D3DSWAPEFFECT_FLIPEX : D3DSWAPEFFECT_COPY;
  UINT highestBackBufferCount = isEx ? 30 : 3;

  if (p.SwapEffect == 0 || p.SwapEffect > highestSwapEffect)
    return "SwapEffect";
  if (p.BackBufferCount > highestBackBufferCount)
    return "BackBufferCount";
  if (p.SwapEffect == D3DSWAPEFFECT_COPY && p.BackBufferCount > 1)
    return "BackBufferCount (COPY takes a single backbuffer)";

  switch (p.PresentationInterval) {
  case D3DPRESENT_INTERVAL_DEFAULT:
  case D3DPRESENT_INTERVAL_ONE:
  case D3DPRESENT_INTERVAL_TWO:
  case D3DPRESENT_INTERVAL_THREE:
  case D3DPRESENT_INTERVAL_FOUR:
  case D3DPRESENT_INTERVAL_IMMEDIATE:
    break;
  default:
    return "PresentationInterval";
  }
  return nullptr;
}

} // namespace dxmt
