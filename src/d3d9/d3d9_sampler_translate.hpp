#pragma once

#include "winemetal.h"
#include "d3d9.h"

#include <cfloat>
#include <cstdint>

// D3D9 sampler-state -> Metal WMTSamplerInfo translation. Lifted out of
// d3d9_device.cpp (like d3d9_state_defaults.hpp) so the pure conversion is
// host-testable without a Metal device. References: wined3d
// (PRIMARY) dlls/wined3d/stateblock.c sampler_desc_from_sampler_states +
// get_texture_address_mode; DXVK (secondary) src/d3d9/d3d9_util.h DecodeFilter /
// DecodeMipFilter / DecodeAddressMode and the Vulkan sampler build in
// d3d9_device.cpp.
//
// Three cells are deliberate divergences on hard Metal limits (documented at
// their sites below), not bugs:
//  - Border colour: Metal exposes only three border colours (transparent black
//    / opaque black / opaque white); an arbitrary D3DCOLOR border is quantized
//    to the nearest and cannot be reproduced exactly, and the border is read
//    through the view swizzle (so an X8 alpha=1 view returns a=1 for a
//    transparent-black border). wined3d/DXVK carry the full float4.
//  - Cube address modes: Metal cube sampling is always seamless, so both refs'
//    forced cube-CLAMP (and DXVK's legacy non-seamless cube filtering) is inert
//    here; dxmt passes the app modes through and Metal ignores them for cube
//    coordinate resolution.
//  - SetLOD + MAXMIPLEVEL compose ADDITIVELY (the SetLOD mip-base shift lives in
//    deriveSampleView, this clamp rides on top), matching DXVK; wined3d composes
//    them with MAX. The references disagree; dxmt sits on the DXVK side (noted at
//    the lod_min_clamp site).

namespace dxmt {

// D3DSAMP_SRGBTEXTURE decode: only the low bit selects sRGB sampling. wined3d
// and DXVK both mask `& 0x1` because a shipped game (Might & Magic Heroes VI)
// writes a garbage pointer-looking value expecting decode OFF and its LSB is
// clear; a plain truthiness test would turn sRGB on and wash the textures out.
// The alias itself is applied at sample-view derivation (deriveSampleView),
// which needs a Metal texture; this predicate is the host-testable rule.
inline bool
d3d9_srgb_texture_enabled(DWORD sampler_state_value) {
  return (sampler_state_value & 0x1) != 0;
}

// D3DTADDRESS_* -> WMTSamplerAddressMode. wined3d state.c sampler_texaddress
// shares this table; clamp/repeat/mirror have direct Metal equivalents.
// MIRRORONCE = clamp-after-one-mirror = Metal's MirrorClampToEdge.
inline WMTSamplerAddressMode
to_mtl_address_mode(DWORD d3dMode) {
  switch (d3dMode) {
  case D3DTADDRESS_WRAP:
    return WMTSamplerAddressModeRepeat;
  case D3DTADDRESS_MIRROR:
    return WMTSamplerAddressModeMirrorRepeat;
  case D3DTADDRESS_CLAMP:
    return WMTSamplerAddressModeClampToEdge;
  case D3DTADDRESS_BORDER:
    return WMTSamplerAddressModeClampToBorderColor;
  case D3DTADDRESS_MIRRORONCE:
    return WMTSamplerAddressModeMirrorClampToEdge;
  default:
    // An out-of-range address value (uninitialized app state) resolves to
    // WRAP: wined3d's get_texture_address_mode default arm and DXVK's
    // DecodeAddressMode both land on REPEAT (the references agree). Clamping
    // instead would tile the affected stages differently.
    return WMTSamplerAddressModeRepeat;
  }
}

// D3DTEXF_* min/mag -> Metal min/mag filter. wined3d clamps min/mag into
// [POINT, LINEAR] (stateblock.c), DXVK maps `> D3DTEXF_POINT` to LINEAR
// (d3d9_util.h); the references agree. So NONE / POINT sample nearest and
// LINEAR / ANISOTROPIC / PYRAMIDALQUAD / GAUSSIANQUAD (and any invalid value
// above LINEAR) all sample linear. The anisotropy level comes separately from
// D3DSAMP_MAXANISOTROPY (gated in sampler_info_from_d3d9_state).
inline WMTSamplerMinMagFilter
to_mtl_minmag_filter(DWORD d3dFilter) {
  switch (d3dFilter) {
  case D3DTEXF_NONE:
  case D3DTEXF_POINT:
    return WMTSamplerMinMagFilterNearest;
  default:
    return WMTSamplerMinMagFilterLinear;
  }
}

// D3DTEXF_* mip -> Metal mip filter. wined3d clamps mip into [NONE, LINEAR]
// (stateblock.c), DXVK maps `> D3DTEXF_POINT` to LINEAR (d3d9_util.h). NONE
// collapses the chain to level 0 (Metal's NotMipmapped), POINT selects the
// nearest mip, and LINEAR / ANISOTROPIC / the quad filters all trilerp. Only
// NONE disables mipmapping, so an engine that writes ANISOTROPIC into all three
// filter states keeps its mip chain instead of silently dropping to level 0.
inline WMTSamplerMipFilter
to_mtl_mip_filter(DWORD d3dFilter) {
  switch (d3dFilter) {
  case D3DTEXF_NONE:
    return WMTSamplerMipFilterNotMipmapped;
  case D3DTEXF_POINT:
    return WMTSamplerMipFilterNearest;
  default:
    return WMTSamplerMipFilterLinear;
  }
}

// Translate one D3D9 sampler-stage state row into a Metal WMTSamplerInfo. The
// row is indexed by D3DSAMP_* (1..13). LOD-bias and SRGB sampling have no
// direct Metal-sampler equivalent and are handled at sample-site time
// (sample-time bias blob / format-aware sample view), not in the sampler
// object. shadow_compare pairs a LessEqual compare with a hardware-PCF depth
// texture; not_filterable degrades to point sampling for a format Metal cannot
// linearly filter (see below).
inline WMTSamplerInfo
sampler_info_from_d3d9_state(const DWORD *state, bool shadow_compare = false, bool not_filterable = false) {
  WMTSamplerInfo info{};
  info.s_address_mode = to_mtl_address_mode(state[D3DSAMP_ADDRESSU]);
  info.t_address_mode = to_mtl_address_mode(state[D3DSAMP_ADDRESSV]);
  info.r_address_mode = to_mtl_address_mode(state[D3DSAMP_ADDRESSW]);
  info.mag_filter = to_mtl_minmag_filter(state[D3DSAMP_MAGFILTER]);
  info.min_filter = to_mtl_minmag_filter(state[D3DSAMP_MINFILTER]);
  info.mip_filter = to_mtl_mip_filter(state[D3DSAMP_MIPFILTER]);
  // Apple-silicon Metal cannot linearly filter 32-bit-float colour formats
  // (R32F / G32R32F / A32B32G32R32F); a LINEAR sampler over them is an
  // undefined combination flagged by Metal shader validation. wined3d degrades
  // MAG/MIN to POINT and MIP to NONE for formats lacking the FILTERING cap
  // (stateblock.c), reproducing native ATI / pre-G80 behaviour. Do the same
  // when the bound texture's format is not filterable: point-sampling fp32 is
  // always valid, so this is safe regardless of the exact AGX failure mode.
  // DXVK needs no equivalent (desktop Vulkan filters fp32).
  if (not_filterable) {
    info.mag_filter = WMTSamplerMinMagFilterNearest;
    info.min_filter = WMTSamplerMinMagFilterNearest;
    info.mip_filter = WMTSamplerMipFilterNotMipmapped;
  }
  // Border-colour quantization: Metal exposes only three border colours, so an
  // app that depends on a specific tinted border gets the closest match by the
  // alpha and luma of the D3DCOLOR (alpha < 0x80 -> transparent black, else
  // bright -> opaque white, else opaque black). Metal hard limit, documented in
  // the header preamble; wined3d/DXVK carry the full float4.
  DWORD bc = state[D3DSAMP_BORDERCOLOR];
  uint8_t a = (bc >> 24) & 0xFF;
  uint8_t r = (bc >> 16) & 0xFF;
  uint8_t g = (bc >> 8) & 0xFF;
  uint8_t b = (bc) & 0xFF;
  if (a < 0x80)
    info.border_color = WMTSamplerBorderColorTransparentBlack;
  else if ((uint32_t)r + g + b > 0x80 * 3)
    info.border_color = WMTSamplerBorderColorOpaqueWhite;
  else
    info.border_color = WMTSamplerBorderColorOpaqueBlack;
  // Hardware-PCF shadow stages get a LessEqual compare (D3D9 HW shadow maps are
  // fixed LessEqual; there is no D3DSAMP state to pick the func; binding a depth
  // texture implies it, matching wined3d's GL_LEQUAL). Non-shadow stages stay
  // Always (the sample op ignores it). The sampler cache key folds
  // compare_function, so a shadow and a non-shadow sampler over the same
  // D3DSAMP state get distinct entries.
  info.compare_function = shadow_compare ? WMTCompareFunctionLessEqual : WMTCompareFunctionAlways;
  // D3DSAMP_MAXMIPLEVEL: largest mip the sampler is allowed to use (0 = full
  // detail, N = skip first N levels). Metal's lod_min_clamp has the same
  // direction. wined3d wires it 1:1 (sampler_lod_min_max), DXVK folds it into
  // the sampler key as the min-LOD clamp. It composes ADDITIVELY with the
  // SetLOD mip-base shift in deriveSampleView (min level = SetLOD +
  // MAXMIPLEVEL), matching DXVK; wined3d composes the two with MAX. The
  // references disagree here and dxmt sits on the DXVK side; both-nonzero is a
  // rare app pattern with no native oracle pinning it.
  info.lod_min_clamp = static_cast<float>(state[D3DSAMP_MAXMIPLEVEL]);
  info.lod_max_clamp = FLT_MAX;
  // Anisotropy engages only when a filter mode actually selects it. Metal has
  // no separate anisotropic-filter enum - maxAnisotropy > 1 turns any linear
  // sampler anisotropic - so the D3D9 filter-mode gate must live here. wined3d
  // forces max_anisotropy = 1 unless MIN, MAG or MIP filter == ANISOTROPIC
  // (stateblock.c); DXVK gates on MINFILTER == ANISOTROPIC only. Follow wined3d
  // (the superset, matching GL-era native behaviour). Without the gate an app
  // that writes MAXANISOTROPY device-wide but selects LINEAR filters would
  // sample sharper-than-native at oblique angles. A non-filterable degrade
  // already forced point sampling, so aniso stays 1 there too.
  bool aniso_selected = state[D3DSAMP_MAGFILTER] == D3DTEXF_ANISOTROPIC ||
                        state[D3DSAMP_MINFILTER] == D3DTEXF_ANISOTROPIC ||
                        state[D3DSAMP_MIPFILTER] == D3DTEXF_ANISOTROPIC;
  uint32_t aniso = 1;
  if (aniso_selected && !not_filterable) {
    aniso = state[D3DSAMP_MAXANISOTROPY];
    if (aniso < 1)
      aniso = 1;
    if (aniso > 16)
      aniso = 16;
  }
  info.max_anisotroy = aniso;
  info.normalized_coords = true;
  info.lod_average = false;
  info.support_argument_buffers = false;
  return info;
}

} // namespace dxmt
