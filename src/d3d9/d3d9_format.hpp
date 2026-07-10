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
// ATI1N and ATI2N (3Dc) one- and two-channel 4x4 block compression, the
// FOURCCs games probe inline for normal maps. Lowered to BC4/BC5 with the
// DXVK swizzles (d3d9_format.cpp): ATI1 samples (R, 0, 0, 1), ATI2 swaps to
// (G, R, 1, 1). Volumes stay on the SCRATCH blob path like every compressed
// format; Metal has no 3D BC textures.
constexpr D3DFORMAT D3DFMT_ATI1 = static_cast<D3DFORMAT>(MAKEFOURCC('A', 'T', 'I', '1'));
constexpr D3DFORMAT D3DFMT_ATI2 = static_cast<D3DFORMAT>(MAKEFOURCC('A', 'T', 'I', '2'));

inline bool
Is3DcFormat(D3DFORMAT format) {
  return format == D3DFMT_ATI1 || format == D3DFMT_ATI2;
}

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

// True for the X-formats: colour formats whose alpha channel reads as a
// constant 1.0. dxmt backs these on Metal formats that carry a live alpha
// byte (BGRX8Unorm keeps BGRA8 storage, X8B8G8R8 reuses RGBA8, the X1/X4
// 16-bit formats keep their real alpha bits), so a destination-alpha blend
// would sample that stored byte instead of the spec-mandated 1.0. On a
// render target with no alpha channel D3D9 hardware degenerates DESTALPHA to
// one and INVDESTALPHA to zero; the blend translator normalises them for
// these targets. Reference: d9vk d3d9_device.cpp (m_alphaSwizzleRTs).
bool D3DFormatHasNoAlpha(D3DFORMAT format);

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

// The LockRect pitch the D3D9 runtime reports to apps: the tight row
// bytes rounded up to a 4-byte boundary. Some apps depend on the exact
// value, not just the alignment. Matches wined3d_format_calculate_pitch
// (align to the device surface alignment, 4) and DXVK's align(pitch, 4);
// keep it distinct from any Metal linear-texture row alignment, which is a
// device artifact that must not leak into the reported pitch. Returns 0
// when the format is unsupported (D3DFormatRowPitch returned 0).
uint32_t D3DFormatLockPitch(D3DFORMAT format, uint32_t width);

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

// True for colour formats Apple-silicon Metal cannot linearly filter: the
// 32-bit-float trio R32F / G32R32F / A32B32G32R32F. A LINEAR sampler over one
// of these is an undefined combination (flagged by Metal shader validation), so
// the sampler translation degrades MAG/MIN/MIP to point for them (wined3d does
// the same for formats lacking the FILTERING cap) and CheckDeviceFormat denies
// D3DUSAGE_QUERY_FILTER. Single source of truth for both the honored behaviour
// and the advertised cap.
bool IsMetalNonFilterableFormat(D3DFORMAT format);

// True for the BC-compressed DXT* formats (DXT1..DXT5). Compressed
// textures need block-aligned LockRect math (the math itself is a
// future commit) and can't be render-targets on Apple Silicon; both
// CreateTexture and CreateOffscreenPlainSurface gate their RT-usage
// promotion off this. Apple Metal also rejects compressed mip levels
// below 4×4; the clamp hasn't been needed yet: when a caller passes
// Levels=0 against a compressed format, this is the place to add
// `max_levels = log2(max(W,H)/4) + 1`.
bool IsCompressedFormat(D3DFORMAT format);

// Metal-copy geometry; equals RowPitch / RowCount except for the 3Dc
// FOURCCs (linear app fiction over real BC storage, see the .cpp notes).
uint32_t D3DFormatMetalTransferPitch(D3DFORMAT format, uint32_t width);
uint32_t D3DFormatMetalTransferRows(D3DFORMAT format, uint32_t height);

// True for formats Metal cannot realize (no equivalent MTLPixelFormat) but that
// D3D9 still permits as a system-memory SCRATCH resource: packed YUV (YUY2,
// UYVY), the paletted formats (P8, A8P8), padded 24-bit RGB (R8G8B8), the
// 8-bit-packed colour formats (R3G3B2, A8R3G3B2), the CxV8U8 bump variant and
// A4L4. The create paths build these mirror-only (no backing dxmt::Texture);
// any non-SCRATCH pool stays D3DERR_INVALIDCALL, and CheckDeviceFormat keeps
// reporting them NOTAVAILABLE. Mirrors DXVK's GetUnsupportedFormatInfo path,
// which makes exactly this set creatable CPU-only "to allow for the creation of
// offscreen plain surfaces" (d3d9_format.cpp) and gates it to D3DPOOL_SCRATCH.
bool IsScratchableUnsupportedFormat(D3DFORMAT format);

// Block dimensions in texels: DXTn are 4x4, packed-YUV (YUY2/UYVY) are 2x1
// (a macropixel spans two horizontal texels), every other format is 1x1.
// LockBox uses these to reject partial-block boxes and to step the byte
// offset in whole blocks. Mirrors wined3d's per-format block_width/
// block_height table and DXVK's block-extent info.
uint32_t D3DFormatBlockWidth(D3DFORMAT format);
uint32_t D3DFormatBlockHeight(D3DFORMAT format);

// Formats GetDC accepts: the uncompressed RGB color formats GDI can paint into.
// D3DKMTCreateDCFromMemory maps each to a DIB bit-depth, so the surface's
// stored pitch and this format agree. Matches DXVK d3d9_format.h
// IsSurfaceGetDCCompatibleFormat and the set wined3d's get_dc accepts.
bool IsGetDCCompatibleFormat(D3DFORMAT format);

// The D3D9 display formats a backbuffer can be presented in, and therefore the
// only source formats GetFrontBufferData can convert into its A8R8G8B8
// destination: the two 32-bit BGRA formats plus the 16-bit and 10-bit packed
// color formats. Any other source has no defined front-buffer decode.
bool IsFrontBufferReadbackSourceFormat(D3DFORMAT format);

// Decode one front-buffer source pixel (an IsFrontBufferReadbackSourceFormat
// format) into 32-bit BGRA8, the in-memory byte order of D3DFMT_A8R8G8B8
// (out[0]=B, out[1]=G, out[2]=R, out[3]=A, i.e. the little-endian 0xAARRGGBB
// dword). The 5-, 6- and 10-bit channels are bit-expanded to 8 bits; the
// alpha-less formats (X8, R5G6B5, X1) read alpha as fully opaque the way the
// surface samples. src points at D3DFormatBytesPerPixel(format) source bytes.
// GetFrontBufferData converts the front buffer this way, matching wined3d's
// converting blt and DXVK's blit-convert temp image.
void DecodeFrontBufferPixelToBGRA8(D3DFORMAT format, const uint8_t *src, uint8_t out[4]);

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

// The color formats D3D9 advertises as render-target capable. Shared by the
// caps probes (CheckDeviceFormat / CheckDepthStencilMatch) and the
// RENDERTARGET-usage create gate so a create cannot succeed for a format the
// caps deny (native + DXVK reject such creates). Excludes the swizzle-permuted
// storage formats (A4R4G4B4 etc.) a sampler reads through a channel swap, which
// have no correct render-into path.
bool isColorRTFormat(D3DFORMAT format);

// The fp16/fp32 color formats. Valid render targets and RGB conversion
// destinations, but not CheckDeviceFormatConversion sources (no blit cap).
bool IsFloatColorFormat(D3DFORMAT format);

// The color formats CreateVolumeTexture and the D3DRTYPE_VOLUMETEXTURE probe
// accept: any format that lowers to a Metal pixel format and is realizable as a
// 3D texture. Excludes depth/stencil (Metal has no sampleable 3D depth), the
// block-compressed DXTn + 3Dc FOURCCs and packed YUV (no 3D BC / 422 format;
// those take the SCRATCH-blob mirror path). Both the probe and the create gate
// consult this one predicate so the two cannot drift (native + DXVK/wined3d
// advertise every mapped color format for TEXTURE_3D).
bool IsVolumeTextureFormat(D3DFORMAT format);

// True for a D3D9 format whose Metal storage HAS an sRGB sibling that must not
// be used for D3DSAMP_SRGBTEXTURE / D3DUSAGE_QUERY_SRGBREAD, because that
// sibling would gamma-decode a non-color lane. The only such format is
// D3DFMT_A8L8: it lowers to RG8Unorm, whose RG8Unorm_sRGB sibling decodes BOTH
// lanes including the alpha stored in G, whereas native D3D9 (GL
// sLUMINANCE8_ALPHA8) and DXVK decode luminance only and give A8L8 no sRGB pair.
// The SRGBREAD probe, the sample-bind sRGB alias and the PixelFormatView create
// hint all consult this so dxmt advertises and realizes A8L8 with no sRGB read,
// matching the references. Kept a pure D3DFORMAT predicate (no Recall_sRGB call)
// so the format layer stays host-linkable; callers still apply Recall_sRGB to
// obtain the aliased format for every other format.
inline bool
D3D9FormatSuppressSRGBRead(D3DFORMAT format) {
  return format == D3DFMT_A8L8;
}

} // namespace dxmt
