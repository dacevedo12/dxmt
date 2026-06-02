#pragma once

#include "winemetal.h"
#include "d3d9.h"

namespace dxmt {

// FOURCC sampleable-depth aliases. Not in the public d3d9types.h enum
// but games (and wined3d's directx.c format table) treat them as
// first-class D3DFORMATs. The MAKEFOURCC bit-pattern is part of the
// stable contract: apps build them inline via MAKEFOURCC('I','N','T','Z')
// at probe time, so dxmt only needs values that match.
constexpr D3DFORMAT D3DFMT_INTZ = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
constexpr D3DFORMAT D3DFMT_DF24 = static_cast<D3DFORMAT>(MAKEFOURCC('D', 'F', '2', '4'));
constexpr D3DFORMAT D3DFMT_DF16 = static_cast<D3DFORMAT>(MAKEFOURCC('D', 'F', '1', '6'));

// D3DFORMAT → MTLPixelFormat lowering. Returns WMTPixelFormatInvalid for
// unsupported formats. Usage argument selects alias for ambiguous formats
// (e.g. D24S8 → D32FS8 for DS, typeless for texture). Apple Silicon:
// D3DFMT_D24S8 must alias to Depth32Float_Stencil8.
enum class D3D9FormatUsage {
  RenderTarget,
  DepthStencil,
  SampleableTexture,
};

WMTPixelFormat D3DFormatToMetal(D3DFORMAT format, D3D9FormatUsage usage);

// Bytes per pixel for tightly-packed surface formats. Used by LockRect
// stride calculation and software readback paths. Returns 0 for
// compressed / unsupported formats: callers must check.
uint32_t D3DFormatBytesPerPixel(D3DFORMAT format);

// Bytes per row of texels for the given format and width. For DXT-
// compressed formats this is bytes per row of 4×4 BLOCKS, with the
// width rounded up to a multiple of 4 first. Returns 0 for unsupported
// formats. Used by MANAGED-pool LockRect mirror sizing and by the
// replaceRegion bytesPerRow argument on UnlockRect.
uint32_t D3DFormatRowPitch(D3DFORMAT format, uint32_t width);

// Number of rows in a level of the given format and height. For DXT
// formats this is rows of 4×4 BLOCKS (height rounded up to a multiple
// of 4 then divided by 4). For uncompressed formats this is just
// height. Returns 0 for unsupported formats.
uint32_t D3DFormatRowCount(D3DFORMAT format, uint32_t height);

// True for any D3DFMT_D* depth or D3DFMT_S* stencil format. The d3d9
// runtime gates depth formats off of certain create paths (notably
// CreateOffscreenPlainSurface: plain surfaces have no DS bind role),
// even though the same format may be sampler-legal as a shadow-map
// alias for CreateTexture. Centralised so the gate doesn't get
// re-derived per call site.
bool IsDepthStencilFormat(D3DFORMAT format);

// True for depth formats that do HARDWARE PCF when bound as a sampled
// texture (DF24/DF16 + native depth like D24S8): i.e. texld returns the
// filtered depth comparison, not raw depth. = IsDepthStencilFormat minus
// INTZ (which is raw-depth). Drives the sample_compare shader variant +
// LessEqual sampler in the draw path.
bool IsHardwarePCFDepthFormat(D3DFORMAT format);

// True for the BC-compressed DXT* formats (DXT1..DXT5). Compressed
// textures need block-aligned LockRect math (the math itself is a
// future commit) and can't be render-targets on Apple Silicon; both
// CreateTexture and CreateOffscreenPlainSurface gate their RT-usage
// promotion off this. Apple Metal also rejects compressed mip levels
// below 4×4; the clamp hasn't been needed yet: when a caller passes
// Levels=0 against a compressed format, this is the place to add
// `max_levels = log2(max(W,H)/4) + 1`.
bool IsCompressedFormat(D3DFORMAT format);

// Subset of IsDepthStencilFormat: true only for formats that carry a
// clearable stencil aspect. D3DFMT_D24X8 is depth-stencil-shaped on the
// Metal side (we alias to Depth32Float_Stencil8) but D3D9 forbids
// stencil clears against it; D3DFMT_D32 / D16 have no stencil at all.
// Used to gate D3DCLEAR_STENCIL.
bool HasStencilAspect(D3DFORMAT format);

// Per-DS-format scale factor for D3DRS_DEPTHBIAS. D3D9 specifies bias
// in normalized depth-buffer space, but Metal (like Vulkan/GL) applies
// `bias * r` where r is the minimum resolvable difference for the
// current DS attachment. To make Metal match D3D9 semantics, multiply
// the app-supplied bias by 1/r. DXVK uses the same per-format table
// (d3d9_util.h::GetDepthBufferRValue). On Apple Silicon D24S8 aliases
// to Depth32Float_Stencil8, but the app's bias is sized in D24 units,
// so we return the D24 scale anyway.
float DepthBiasScale(D3DFORMAT format);

// True for the 'NULL' FOURCC sentinel: a colour render-target slot
// the app binds but never writes (shadow-map depth-only passes are
// the canonical shape: bind a 1×1 NULL RT alongside the real DS and
// rasterise with COLORWRITEENABLE = 0). The format has no Metal
// pixel-format equivalent; render-pass build and PSO build both skip
// the slot.
bool IsNullFormat(D3DFORMAT format);

// Per-D3DFORMAT sample-time channel swizzle. Identity (R,G,B,A) for
// most formats; D3DFMT_L8 returns (R,R,R,1) and D3DFMT_A8L8 returns
// (R,R,R,G) so a sampler reading the R8/RG8 storage produces the
// luminance shape D3D9 promises (wined3d does the same swap on its
// sampler-view path). Returned channels feed the dxmt::Texture view
// descriptor's swizzle (via checkViewUseSwizzle) on the per-stage
// sample-bind path; {Zero,Zero,Zero,Zero} is the "no override"
// sentinel the caller translates to identity.
WMTTextureSwizzleChannels D3DFormatSamplerSwizzle(D3DFORMAT format);

} // namespace dxmt
