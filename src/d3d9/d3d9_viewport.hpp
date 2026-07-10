#pragma once

#include "winemetal.h"
#include "d3d9.h"

#include <algorithm>
#include <cstdint>

// Pure D3D9 -> Metal viewport / scissor conversion, factored out of the device
// apply path so the host unit tier can exercise the depth clamp, the half-pixel
// origin shift, and the scissor/viewport intersect without a Metal device (the
// d3d9_image_lock.hpp / d3d9_update_mip.hpp shape). References: wined3d (PRIMARY)
// dlls/wined3d/state.c viewport_miscpart / scissorrect; DXVK (secondary)
// src/d3d9/d3d9_device.cpp SetViewport / SetScissorRect.

namespace dxmt {

// D3D9 pixel centers at integers; Metal at (i+0.5, j+0.5). Shift viewport +0.5.
// No Y-flip needed (Metal NDC y-axis matches D3D9). Clamp MinZ/MaxZ to [0,1].
// If clamping collapses the range, nudge zfar 1/65536 above znear, except at
// the 1.0 ceiling where they stay equal (a degenerate depth range there is
// harmless; DXVK does the same).
inline WMTViewport
wmt_viewport_from_d3d9(const D3DVIEWPORT9 &vp) {
  WMTViewport out;
  out.originX = static_cast<double>(vp.X) + 0.5;
  out.originY = static_cast<double>(vp.Y) + 0.5;
  out.width = static_cast<double>(vp.Width);
  out.height = static_cast<double>(vp.Height);
  out.znear = std::clamp(static_cast<double>(vp.MinZ), 0.0, 1.0);
  out.zfar = std::clamp(static_cast<double>(vp.MaxZ), 0.0, 1.0);
  if (out.zfar <= out.znear)
    out.zfar = std::min(1.0, out.znear + 1.0 / 65536.0);
  return out;
}

// D3D9 carries a scissor rect plus a separate enable flag; Metal has no "off"
// mode, so a disabled scissor falls back to the viewport bounds. An enabled
// scissor is always intersected with the viewport, and an empty intersection
// collapses to a zero-extent rect rather than an inverted one (Metal rejects a
// scissor wider than its attachment; the apply path clamps the outer bound).
inline WMTScissorRect
wmt_scissor_from_d3d9(const RECT &sr, const D3DVIEWPORT9 &vp, bool scissor_enabled) {
  const int64_t vp_l = static_cast<int64_t>(vp.X);
  const int64_t vp_t = static_cast<int64_t>(vp.Y);
  const int64_t vp_r = vp_l + static_cast<int64_t>(vp.Width);
  const int64_t vp_b = vp_t + static_cast<int64_t>(vp.Height);
  int64_t l = vp_l, t = vp_t, r = vp_r, b = vp_b;
  if (scissor_enabled) {
    l = std::max<int64_t>(vp_l, sr.left);
    t = std::max<int64_t>(vp_t, sr.top);
    r = std::min<int64_t>(vp_r, sr.right);
    b = std::min<int64_t>(vp_b, sr.bottom);
    if (r < l)
      r = l;
    if (b < t)
      b = t;
  }
  WMTScissorRect out;
  out.x = static_cast<uint64_t>(l);
  out.y = static_cast<uint64_t>(t);
  out.width = static_cast<uint64_t>(r - l);
  out.height = static_cast<uint64_t>(b - t);
  return out;
}

} // namespace dxmt
