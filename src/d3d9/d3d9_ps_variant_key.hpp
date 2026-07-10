/*
 * Copyright (c) 2026 GameSir Labs and contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <cstdint>

// D3D9 pixel-shader variant-key de-specialisation. Two continuously-
// valued render states used to bake into the PS variant key and mint a
// cold MTLFunction / PSO link per value:
//   - D3DRS_ALPHAREF (0..255, animated for foliage / LOD fades), and
//   - the D3DTSS_BUMPENV* matrix / luminance scale + offset (unbounded
//     floats, animated for water ripple, heat shimmer, rain sheen).
// DXVK keeps only the BOUNDED axes in the key (the alpha compare FUNC,
// ~8 values) and feeds the numeric ref + bump-env through a shared PS
// uniform buffer the shader reads at runtime: src/d3d9/d3d9_fixed_function.cpp
// D3D9SharedPS (per-stage BumpEnvMat/LScale/LOffset) and
// src/dxso/dxso_compiler.cpp emitBem read that block; the alpha ref is
// normalised against the fragment alpha the same way. dxmt routes both
// through the bool-buffer tail the fog colour + table params already ride.
//
// This header carries the pure key logic that keeps the variant key
// INVARIANT under the numeric alpha ref and the bump-env values: the
// numbers ride the uniform, only the bounded alpha FUNC / fog / sampler
// / dual-source / flat gates may move the key. It is header-only and free
// of D3D9 / airconv headers so the host unit tier can exercise it without
// a device.

namespace dxmt {

// Generated fixed-function pixel shader (combiner) variant key. Ports the
// FNV-1a walk ffpPixelFunction used inline, minus the alpha ref: the ref
// rides the bool-buffer tail, so distinct D3DRS_ALPHAREF values collapse
// to one generated combiner. The bounded axes (combiner table, fog mode,
// alpha FUNC, sampler kinds, point-sprite, specular, flat) still key the
// variant.
inline uint64_t
ffp_ps_variant_key(
    const uint32_t (*stages)[3], bool specular_enable, bool point_sprite, int fog_mode, bool fog_coord_w,
    uint32_t alpha_func, uint32_t sampler_kind_key, bool flat_shading,
    // D3DRS_MULTISAMPLEMASK enable: a 1-bit gate (mask value rides the tail,
    // never the key), so a masked-MSAA draw compiles a coverage-emitting
    // combiner distinct from the plain one.
    bool emit_sample_mask,
    // Per-attachment 8-bit-UNORM snap mask (bit 0 = rt0 is LINEAR 8-bit unorm).
    // Unlike the alpha ref, this DOES move the key: the snap changes the emitted
    // combiner (rint round-to-even on the color store), so a float-target and an
    // 8-bit-unorm-target draw must not share one generated function. It is a
    // bounded axis, so it keys like the fog/alpha FUNC gates, not the ref.
    uint32_t unorm_snap_mask,
    // Accepted only to make the de-specialisation explicit and regression-
    // proof: the alpha ref rides the bool-buffer tail and must never enter
    // the key. The device omits it (default); the host test feeds distinct
    // values and asserts the key never moves.
    uint32_t /*alpha_ref*/ = 0
) {
  uint64_t key = 0xcbf29ce484222325ull;
  auto mix = [&](uint64_t v) {
    key ^= v;
    key *= 0x100000001b3ull;
  };
  for (int i = 0; i < 8; ++i)
    for (int j = 0; j < 3; ++j)
      mix(stages[i][j]);
  mix(uint32_t(fog_mode + 1) | (alpha_func << 8) | (fog_coord_w ? 1u << 24 : 0u) | (point_sprite ? 1u << 25 : 0u) |
      (specular_enable ? 1u << 26 : 0u));
  mix(sampler_kind_key);
  mix(flat_shading ? 2u : 1u);
  mix(emit_sample_mask ? 1u : 0u);
  mix(unorm_snap_mask);
  return key;
}

// Bytecode (SM1-3) pixel shader variant key. Ports the FNV-1a walk
// MTLD3D9PixelShaderModule::compileVariant used inline, minus the alpha
// ref and the bump-env constants: both ride the shared PS uniform tail,
// so distinct alpha refs / bump-env matrices collapse to one variant per
// module. The bounded axes (alpha FUNC, sampler kinds, point-sprite, fog
// mode, dual-source, flat) still key the variant. bem_stage_mask is NOT
// keyed: the codegen branches on the TexBem/TexBemL/Bem opcodes carried by
// the module's own bytecode, which is constant across every variant of a
// module, so it never needs to move the key.
inline uint64_t
programmable_ps_variant_key(
    uint32_t alpha_test_func, const uint8_t samp_kinds[16], bool point_sprite, int fog_mode, bool fog_coord_w,
    bool dual_source, bool flat_shading,
    // D3DRS_MULTISAMPLEMASK enable: a 1-bit gate (mask value rides the tail,
    // never the key), so a masked-MSAA draw compiles a coverage-emitting
    // variant distinct from the plain one.
    bool emit_sample_mask,
    // Per-attachment 8-bit-UNORM snap mask (bit i = oC<i> targets a LINEAR
    // 8-bit unorm attachment). Keys the variant like the other bounded gates:
    // the snap changes the emitted color store (rint round-to-even), so a
    // shader used on both an 8-bit-unorm and a float RT must fork one metallib
    // per mask. Unlike the alpha ref / bump-env below it is not a runtime tail.
    uint32_t unorm_snap_mask,
    // Accepted only to make the de-specialisation explicit and regression-
    // proof: the alpha ref and the bump-env matrix / luminance scale +
    // offset ride the shared PS uniform tail and must never enter the key.
    // The device omits them (defaults); the host test feeds distinct values
    // and asserts the key never moves.
    uint32_t /*alpha_test_ref*/ = 0, const float * /*bem_mat*/ = nullptr, const float * /*bem_lum*/ = nullptr
) {
  uint64_t key = 0xcbf29ce484222325ull;
  auto mix = [&](uint64_t v) {
    key ^= v;
    key *= 0x100000001b3ull;
  };
  mix(alpha_test_func & 0xFFu);
  for (uint32_t i = 0; i < 16; ++i)
    mix(samp_kinds[i] & 0xFFu);
  mix(point_sprite ? 1u : 0u);
  mix(fog_mode < 0 ? 0xFFu : (uint32_t)(fog_mode & 0xFFu));
  mix(fog_coord_w ? 1u : 0u);
  mix(dual_source ? 1u : 0u);
  mix(flat_shading ? 3u : 2u);
  mix(emit_sample_mask ? 1u : 0u);
  mix(unorm_snap_mask);
  return key;
}

} // namespace dxmt
