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

// The Metal-independent SetCursorProperties reject matrix, lifted out of
// MTLD3D9Device so the pure format / power-of-two / hotspot / display-mode
// decision is host-testable without a device (the d3d9_create_validation.hpp
// shape). The device keeps the null-pointer guard (a surface pointer, not pure
// data), the GetAdapterDisplayMode fetch (feeds the mode dimensions in here),
// and the hardware-cursor realisation. References: wined3d (PRIMARY)
// dlls/d3d9/device.c d3d9_device_SetCursorProperties (the display-mode
// dimension gate); DXVK (secondary) src/d3d9/d3d9_device.cpp
// SetCursorProperties (A8R8G8B8, power-of-two, hotspot-in-bitmap).

namespace dxmt {

// Returns D3D_OK when every Metal-independent cursor gate passes, else
// D3DERR_INVALIDCALL. displayModeWidth/Height come from
// GetAdapterDisplayMode; the runtime validates the bitmap against them even
// on a windowed swapchain.
inline HRESULT
validate_cursor_properties(
    D3DFORMAT format, UINT width, UINT height, UINT xHotSpot, UINT yHotSpot, UINT displayModeWidth,
    UINT displayModeHeight
) {
  // Cursor bitmaps are A8R8G8B8 only (DXVK).
  if (format != D3DFMT_A8R8G8B8)
    return D3DERR_INVALIDCALL;
  // Dimensions must be powers of two (DXVK). A zero extent passes the bit test
  // (0 & -1 == 0) and is caught by the display-mode gate below only if the mode
  // is also zero, so it stays legal here exactly as DXVK leaves it.
  if ((width && (width & (width - 1))) || (height && (height & (height - 1))))
    return D3DERR_INVALIDCALL;
  // The hotspot must lie within the bitmap.
  if ((width && xHotSpot > width - 1) || (height && yHotSpot > height - 1))
    return D3DERR_INVALIDCALL;
  // The bitmap must fit the display mode, even on a windowed swapchain.
  if (width > displayModeWidth || height > displayModeHeight)
    return D3DERR_INVALIDCALL;
  return D3D_OK;
}

} // namespace dxmt
