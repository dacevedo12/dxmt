/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// D3D9 point-size de-specialisation. DXVK feeds the point size and its
// clamp bounds as a uniform, so ONE pipeline serves every size: see
// dxvk/src/d3d9/d3d9_fixed_function.cpp GetPointSizeInfoVS (the size,
// D3DRS_POINTSIZE_MIN/_MAX and the POINTSCALE distance attenuation all
// read the render-state block) and dxso_compiler.cpp emitPsize (the
// programmable epilogue clamps against the same block). dxmt used to
// bake those floats into a per-value MTLFunction variant, which turned
// a continuously-varying render state into an unbounded pipeline axis.
//
// This header carries the pure logic that keeps the variant key
// INVARIANT under the numeric point-size render states: the numbers ride
// the uniform, only the point-vs-nonpoint topology / shader-writes-oPts /
// POINTSCALEENABLE gates may move the key. It is header-only and free of
// D3D9 headers so the host unit tier can exercise it without a device.

namespace dxmt {

// The single value the injecting programmable-VS point-size variant
// folds into its layout key. Distinct sizes now share this bit instead
// of minting one variant per float value.
static constexpr uint64_t kPointSizeVariantSentinel = 0x9e3779b97f4a7c15ull;

// FFP-VS point-size key terms. The point-size output presence, the
// POINTSCALEENABLE attenuation and a declared PSIZE attribute may key
// the generated vertex function; the numeric size/bounds/scale factors
// never do. Kept distinct from the other ffpVertexFunction key terms.
static constexpr uint64_t kFfpPointSizeSentinel = 0x1c9e6d3b2a5f8741ull;
// Distinct from the texcoord_index_key multiplier in ffpVertexFunction: both
// POINTSCALEENABLE (a render state) and TEXCOORDINDEX (a texture-stage state)
// fold into the VS key, and neither moves the layout fingerprint, so a shared
// constant would let {point_scale, tci=0} XOR-collide with {no point_scale,
// tci=1} onto one key.
static constexpr uint64_t kFfpPointScaleSentinel = 0x2545f4914f6cdd1dull;
static constexpr uint64_t kFfpPointPerVertexSentinel = 0x94d049bb133111ebull;

// The clamp bounds the injecting VS reads from the point-size uniform.
// min carries the raw D3DRS_POINTSIZE_MIN (a negative is pulled up to 0),
// max clamps to the Apple point-size ceiling (511), matching the
// fixed-function block a9730f3f uploads so both vertex paths clamp
// identically. A point clamped to size 0 (an app setting both the size
// and the minimum to 0) rasterises to nothing, the way real Windows and
// DXVK (GetPointSizeInfoVS uses the raw minimum) draw it; flooring the
// minimum at 1 would draw a stray pixel there, wined3d's aliased-point
// behaviour rather than the hardware's. Raw NaN bounds fall back to the
// hardware defaults.
struct D3D9PointSizeParams {
  float size; // raw D3DRS_POINTSIZE, uniform lane x
  float min;  // clamp lower bound, uniform lane y
  float max;  // clamp upper bound, uniform lane z
};

inline D3D9PointSizeParams
compute_point_size_params(uint32_t size_bits, uint32_t min_bits, uint32_t max_bits) {
  D3D9PointSizeParams p;
  std::memcpy(&p.size, &size_bits, sizeof(float));
  float mn, mx;
  std::memcpy(&mn, &min_bits, sizeof(float));
  std::memcpy(&mx, &max_bits, sizeof(float));
  p.min = std::isfinite(mn) ? (mn > 0.0f ? mn : 0.0f) : 1.0f;
  p.max = std::isfinite(mx) && mx >= 1.0f ? (mx > 511.0f ? 511.0f : mx) : 511.0f;
  return p;
}

// The point size a non-oPts-writing point draw ends up rasterising:
// clamp(size, min, max) the way the generated VS's fmax/fmin epilogue
// does. Used to gate injection off the 1.0 hardware default.
inline float
clamp_point_size(const D3D9PointSizeParams &p) {
  float s = p.size;
  if (!(s >= p.min)) // also pulls NaN up to min
    s = p.min;
  if (s > p.max)
    s = p.max;
  return s;
}

// Whether the programmable VS must emit [[point_size]] for this draw.
// Non-point draws never do (the topology gate a9730f3f kept). A VS that
// writes its own oPts always injects so the epilogue clamps it against
// the uniform bounds; otherwise inject only when the clamped render
// state leaves the 1.0 default, so ordinary triangle-adjacent point
// draws keep the base variant. The decision is independent of the
// numeric size beyond that single "is it the default" test.
inline bool
inject_point_size(bool is_point_list, bool vs_writes_point_size, uint32_t size_bits, uint32_t min_bits, uint32_t max_bits) {
  if (!is_point_list)
    return false;
  if (vs_writes_point_size)
    return true;
  float clamped = clamp_point_size(compute_point_size_params(size_bits, min_bits, max_bits));
  return clamped > 1.0f || clamped < 1.0f;
}

// The programmable VS variant key contribution. The point-size render
// state reaches the key ONLY through the injection bit, never through
// the size value, so distinct sizes collapse to one pipeline variant.
inline uint64_t
point_size_variant_key(uint64_t layout_key, bool inject) {
  return inject ? (layout_key ^ kPointSizeVariantSentinel) : layout_key;
}

// The FFP-VS variant key contribution. Only the point-vs-nonpoint
// topology, the POINTSCALEENABLE flag and a declared PSIZE attribute may
// move the key. The numeric size / min / max / scale-A/B/C bits are
// accepted here only to make the invariant explicit: they are ignored,
// so two point draws differing solely in those numbers share a variant.
inline uint64_t
ffp_point_size_variant_key(
    uint64_t base, bool is_point_list, bool point_scale_enable, bool has_psize, uint32_t /*size_bits*/,
    uint32_t /*min_bits*/, uint32_t /*max_bits*/, uint32_t /*scale_a_bits*/, uint32_t /*scale_b_bits*/,
    uint32_t /*scale_c_bits*/
) {
  if (!is_point_list)
    return base;
  uint64_t key = base ^ kFfpPointSizeSentinel;
  if (point_scale_enable)
    key ^= kFfpPointScaleSentinel;
  if (has_psize)
    key ^= kFfpPointPerVertexSentinel;
  return key;
}

} // namespace dxmt
