#include "d3d9_format.hpp"

namespace dxmt {

// Vendor-defined FOURCC depth aliases — not in d3d9types.h. Apps
// hardcode these as the ENABLE-flag pattern: CheckDeviceFormat for
// (DEPTHSTENCIL, INTZ); if it returns OK the app issues
// CreateTexture(D3DUSAGE_DEPTHSTENCIL, INTZ) and uses the texture
// as both DSV and SRV.
//
// References:
//   - INTZ: https://aras-p.info/texts/D3D9GPUHacks.html (NVIDIA's
//     sampleable-depth extension; DXVK src/d3d9/d3d9_format.h:38).
//   - DF24/DF16: ATI's depth-only sampler aliases. Same shape as INTZ
//     but no stencil aspect.
//   - wined3d include/wine/wined3d.h:275 (WINED3DFMT_INTZ etc).
//
// These predate the dxmt formal D3DFORMAT enum so we cast through the
// raw FOURCC constant. Same pattern DXVK uses.
namespace {
// FOURCC sampleable-depth aliases moved to d3d9_format.hpp so other
// translation units (CheckDeviceFormat in d3d9_interface.cpp) can see
// them.
// 'NULL' FOURCC — vendor-defined sentinel for an RT slot the app
// wants bound but never written. Common shadow-map shape: bind a
// 1×1 NULL color RT alongside a real DS, render with COLORWRITEENABLE
// = 0. dxmt drops the slot entirely at render-pass + PSO build
// time; the texture object is a placeholder so the surface still
// exists for SetRenderTarget / GetRenderTarget. Reference: DXVK
// src/d3d9/d3d9_format.h:88 (D3D9Format::NULL_FORMAT).
constexpr D3DFORMAT D3DFMT_NULL = static_cast<D3DFORMAT>(MAKEFOURCC('N', 'U', 'L', 'L'));
} // namespace

WMTPixelFormat
D3DFormatToMetal(D3DFORMAT format, D3D9FormatUsage usage) {
  switch (format) {
  // 32-bpp colour. Metal has BGRX8Unorm (BGRA8 with alpha forced to 1
  // on read, matching D3D9's "X is ignored on read, undefined on
  // write" contract for X8R8G8B8). A8R8G8B8 carries a real alpha
  // channel and uses the plain BGRA8 alias. Reference:
  // MGL/MGLTextures.m line ~140 (UNSIGNED_INT_8_8_8_8 / BGRA → BGRA8).
  case D3DFMT_X8R8G8B8:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatBGRX8Unorm;
  case D3DFMT_A8R8G8B8:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatBGRA8Unorm;

  // A8B8G8R8 → RGBA8Unorm (channel order matches Metal's R-first
  // layout). Metal lacks an RGBX variant, so X8B8G8R8 reuses RGBA8
  // and the X channel is undefined on sampler reads — flag in the
  // table for any future contributor adding swizzle support.
  case D3DFMT_X8B8G8R8:
  case D3DFMT_A8B8G8R8:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGBA8Unorm;

  // 16-bpp colour. R5G6B5 has a direct Metal equivalent. The 1555
  // variants share a single Metal format (alpha bit is ignored for
  // X1 reads).
  case D3DFMT_R5G6B5:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatB5G6R5Unorm;
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatBGR5A1Unorm;

  // 10-10-10-2. D3D9 stores A2R10G10B10 with A in the high bits and
  // B in the low — that's exactly Metal's BGR10A2 dword layout.
  // A2B10G10R10 reverses R and B, matching Metal's RGB10A2.
  case D3DFMT_A2R10G10B10:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatBGR10A2Unorm;
  case D3DFMT_A2B10G10R10:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGB10A2Unorm;

  // Single-channel.
  case D3DFMT_A8:
    // A8 samples as (0, 0, 0, A) on D3D9 and the same on Metal — no
    // swizzle needed; A8Unorm carries native alpha-only semantics.
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatA8Unorm;
  case D3DFMT_L8:
    // D3DFMT_L8 samples as (L, L, L, 1) per spec. The base storage is
    // R8Unorm; the per-stage sample-bind path applies a {R,R,R,1}
    // swizzle via the texture-view cache (D3DFormatSamplerSwizzle).
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatR8Unorm;
  case D3DFMT_A8L8:
    // D3DFMT_A8L8 stores L in byte 0 (MTL R) and A in byte 1 (MTL G),
    // samples as (L, L, L, A). RG8Unorm with the {R,R,R,G} swizzle
    // applied at view time delivers the spec shape.
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRG8Unorm;
  case D3DFMT_L16:
    // 16-bit luminance. Storage is R16Unorm; samples as (L, L, L, 1)
    // via {R,R,R,One} swizzle (registered in D3DFormatSamplerSwizzle).
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatR16Unorm;

  // Signed normalised bump-map formats. D3D9 ENVMAP samplers used these
  // pre-SM3; SM2 / SM3 PS sampling reads the signed value directly.
  // Channel layout matches Metal's signed-norm formats 1:1.
  case D3DFMT_V8U8:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRG8Snorm;
  case D3DFMT_V16U16:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRG16Snorm;
  case D3DFMT_Q8W8V8U8:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGBA8Snorm;

  // FP16 / FP32 colour. Common for HDR-class RTs and shader scratch
  // surfaces. Reject if asked for as a depth alias.
  case D3DFMT_R16F:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatR16Float;
  case D3DFMT_G16R16F:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRG16Float;
  case D3DFMT_A16B16G16R16F:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGBA16Float;
  case D3DFMT_R32F:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatR32Float;
  case D3DFMT_G32R32F:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRG32Float;
  case D3DFMT_A32B32G32R32F:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGBA32Float;

  // Depth-stencil. project_apple_ds_format_alias auto-memory:
  // Apple Silicon rejects Depth24Unorm_Stencil8 at descriptor
  // validation; alias D24S8 onto Depth32Float_Stencil8.
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24FS8:
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth32Float_Stencil8;
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth16Unorm;
  case D3DFMT_D32:
  case D3DFMT_D32F_LOCKABLE:
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth32Float;

  // BC-compressed colour. D3D9's DXT names map to Metal's BCN names:
  //   - DXT1: 4 bits/texel, opaque or 1-bit alpha (BC1_RGBA covers both
  //     — the 1-bit alpha encoding is decided per-block by the encoder).
  //   - DXT2/DXT3: 8 bits/texel, explicit 4-bit alpha (BC2). DXT2 is the
  //     pre-multiplied-alpha variant of DXT3; the storage is identical
  //     and the pre-mult is a shader/sampler concern, not a format one.
  //     Same Metal format covers both.
  //   - DXT4/DXT5: 8 bits/texel, interpolated alpha (BC3). DXT4 is the
  //     pre-multiplied variant of DXT5; same storage.
  //
  // BC formats are sampler-only: Apple Silicon rejects them as render-
  // targets at descriptor-validation. Reject the RenderTarget path; the
  // DepthStencil path is also a no for obvious reasons.
  case D3DFMT_DXT1:
    return usage != D3D9FormatUsage::SampleableTexture ? WMTPixelFormatInvalid : WMTPixelFormatBC1_RGBA;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
    return usage != D3D9FormatUsage::SampleableTexture ? WMTPixelFormatInvalid : WMTPixelFormatBC2_RGBA;
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return usage != D3D9FormatUsage::SampleableTexture ? WMTPixelFormatInvalid : WMTPixelFormatBC3_RGBA;

  // Sampleable-depth FOURCC aliases. Same Metal storage as the
  // matching native depth format; legality on the SampleableTexture
  // path is what makes them special — apps bind these as DSV and SRV
  // on the same texture (shadow-map render then PCF sample). The
  // RenderTarget path stays rejected because the FOURCC is depth, not
  // colour.
  case D3DFMT_INTZ:
    // D24S8 alias on read; Apple Silicon already aliases D24S8 onto
    // Depth32Float_Stencil8. Carry the stencil aspect because INTZ is
    // formally a depth+stencil format even though most games sample
    // only the depth plane.
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth32Float_Stencil8;
  case D3DFMT_DF24:
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth32Float;
  case D3DFMT_DF16:
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth16Unorm;

  default:
    return WMTPixelFormatInvalid;
  }
}

uint32_t
D3DFormatBytesPerPixel(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8:
  case D3DFMT_L8:
    return 1;
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A8L8:
  case D3DFMT_L16:
  case D3DFMT_V8U8:
  case D3DFMT_R16F:
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
  // FOURCC depth aliases. Apps don't normally LockRect these (the
  // point is GPU-side sampling), but a software-readback path needs a
  // stride.
  case D3DFMT_DF16:
    return 2;
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8B8G8R8:
  case D3DFMT_A8B8G8R8:
  case D3DFMT_A2R10G10B10:
  case D3DFMT_A2B10G10R10:
  case D3DFMT_V16U16:
  case D3DFMT_Q8W8V8U8:
  case D3DFMT_G16R16:
  case D3DFMT_G16R16F:
  case D3DFMT_R32F:
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24FS8:
  case D3DFMT_D32:
  case D3DFMT_D32F_LOCKABLE:
  // INTZ apparent BPP is 4 (24-bit depth + 8-bit stencil) per wined3d
  // utils.c:142, but Apple Metal aliases the storage onto
  // Depth32Float_Stencil8 — actually 8 bytes per texel. Today this
  // function only feeds LockRect's stride math, and INTZ textures
  // skip LockRect entirely (no cpu_ptr is allocated). If a future
  // readback path sizes a staging buffer by bpp*w*h, INTZ will
  // under-allocate by half — sizing must come from the Metal storage
  // descriptor, not this table.
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
    return 4;
  case D3DFMT_A16B16G16R16:
  case D3DFMT_A16B16G16R16F:
  case D3DFMT_G32R32F:
    return 8;
  case D3DFMT_A32B32G32R32F:
    return 16;
  default:
    return 0;
  }
}

uint32_t
D3DFormatRowPitch(D3DFORMAT format, uint32_t width) {
  switch (format) {
  case D3DFMT_DXT1:
    // 8 bytes per 4×4 block.
    return ((width + 3u) / 4u) * 8u;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    // 16 bytes per 4×4 block.
    return ((width + 3u) / 4u) * 16u;
  default: {
    uint32_t bpp = D3DFormatBytesPerPixel(format);
    return bpp == 0 ? 0u : width * bpp;
  }
  }
}

uint32_t
D3DFormatRowCount(D3DFORMAT format, uint32_t height) {
  if (IsCompressedFormat(format))
    return (height + 3u) / 4u;
  // Unsupported uncompressed formats fall through to height; callers
  // multiply by D3DFormatRowPitch which is 0 for those, yielding a
  // zero-byte allocation.
  return height;
}

bool
IsDepthStencilFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_D16_LOCKABLE:
  case D3DFMT_D32:
  case D3DFMT_D15S1:
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D16:
  case D3DFMT_D32F_LOCKABLE:
  case D3DFMT_D24FS8:
  case D3DFMT_D32_LOCKABLE:
  case D3DFMT_S8_LOCKABLE:
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
  case D3DFMT_DF16:
    return true;
  default:
    return false;
  }
}

bool
IsCompressedFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_DXT1:
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return true;
  default:
    return false;
  }
}

bool
HasStencilAspect(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_D24S8:
  case D3DFMT_D24FS8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D15S1:
  case D3DFMT_S8_LOCKABLE:
  // INTZ is the D24S8 alias — same dual-aspect storage. DF24/DF16
  // are depth-only so they stay out.
  case D3DFMT_INTZ:
    return true;
  default:
    return false;
  }
}

bool
IsNullFormat(D3DFORMAT format) {
  return format == D3DFMT_NULL;
}

float
DepthBiasScale(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
    return float(1 << 16);
  case D3DFMT_D15S1:
    return float(1 << 15);
  // All 24-bit fixed-point depth formats (with or without stencil) —
  // app's bias is sized in D24 LSBs even when we alias to D32F_S8 on
  // Apple Silicon.
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D24FS8:
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
  case D3DFMT_DF16: // 16-bit float depth — apps treat like D24 in practice
    return float(1 << 24);
  case D3DFMT_D32:
  case D3DFMT_D32_LOCKABLE:
    return float(int64_t(1) << 32);
  case D3DFMT_D32F_LOCKABLE:
    // D3D9 float depth: bias added literally to the float depth value;
    // Metal's float-depth r is dynamic per fragment so this is at best
    // an approximation. Matches DXVK's non-forceUnorm path.
    return float(1 << 23);
  default:
    return 1.0f;
  }
}

WMTTextureSwizzleChannels
D3DFormatSamplerSwizzle(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_L8:
    // (L, L, L, 1) — base is R8Unorm.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleOne};
  case D3DFMT_L16:
    // (L, L, L, 1) — base is R16Unorm. Same swizzle shape as L8.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleOne};
  case D3DFMT_A8L8:
    // (L, L, L, A) — base is RG8Unorm with R=L, G=A.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleGreen};
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
  case D3DFMT_DF16:
    // Depth-as-texture aliases. Aliased onto Depth32Float_Stencil8 (INTZ) or
    // Depth32Float (DF24/DF16); Metal samples those as `depth2d<float>` which
    // returns a scalar red lane. wined3d (utils.c COLOR_FIXUP "XXXX") expands
    // that scalar across rgba at the texture-view level so apps reading .gggg
    // or .yyyy on the sample result (NFS:MW soft-particle smoke does .yyyy)
    // see the depth value, not zero. The shader-side splat in airconv covers
    // the common .r read but not arbitrary swizzles, hence the view-level
    // replicate.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed};
  default:
    // {Zero,Zero,Zero,Zero} is the D3D9ViewKey "no override" sentinel;
    // the cache returns the parent texture verbatim and skips view
    // creation. Most formats deliver D3D9-correct channels via the
    // natural Metal sampler order so identity-needed sites read this
    // value.
    return {WMTTextureSwizzleZero, WMTTextureSwizzleZero, WMTTextureSwizzleZero, WMTTextureSwizzleZero};
  }
}

} // namespace dxmt
