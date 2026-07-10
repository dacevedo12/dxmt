#include "d3d9_format.hpp"

namespace dxmt {

// Vendor-defined FOURCC depth aliases: INTZ (NVIDIA sampleable-depth),
// DF24/DF16 (ATI depth-only). Apps check via CheckDeviceFormat and create
// with D3DUSAGE_DEPTHSTENCIL. Cast through raw FOURCC constant per DXVK.
namespace {
// NULL FOURCC: render-target placeholder for shadow-depth-only passes.
// App binds but never writes (COLORWRITEENABLE=0). dxmt drops at
// render-pass + PSO build; texture exists for Get/SetRenderTarget.
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
  // layout). Metal lacks an RGBX variant, so X8B8G8R8 reuses RGBA8;
  // the sampler swizzle table forces its alpha reads to 1.
  case D3DFMT_X8B8G8R8:
  case D3DFMT_A8B8G8R8:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGBA8Unorm;

  // 16-bit-per-channel UNORM. Metal RG16Unorm / RGBA16Unorm are 1:1
  // with the D3D9 layout. CheckDeviceFormat advertises both as
  // colour render-target and sampleable; prior to this entry,
  // CreateTexture would silently fail at format lowering after the
  // probe said OK. G16R16 is a common motion-vector / velocity
  // intermediate format in this era.
  case D3DFMT_G16R16:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRG16Unorm;
  case D3DFMT_A16B16G16R16:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGBA16Unorm;

  // 16-bpp colour. R5G6B5 has a direct Metal equivalent. The 1555
  // variants share a single Metal format (alpha bit is ignored for
  // X1 reads).
  case D3DFMT_R5G6B5:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatB5G6R5Unorm;
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatBGR5A1Unorm;

  // 4-4-4-4. No native Metal ARGB4444; Metal only has ABGR4Unorm, fixed up
  // with the {G,B,A,R} sampler swizzle in D3DFormatSamplerSwizzle. Mirrors how
  // MoltenVK realises VK_FORMAT_A4R4G4B4 on Metal. X4 ignores alpha on read.
  case D3DFMT_X4R4G4B4:
  case D3DFMT_A4R4G4B4:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatABGR4Unorm;

  // 10-10-10-2. D3D9 stores A2R10G10B10 with A in the high bits and
  // B in the low: that's exactly Metal's BGR10A2 dword layout.
  // A2B10G10R10 reverses R and B, matching Metal's RGB10A2.
  case D3DFMT_A2R10G10B10:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatBGR10A2Unorm;
  case D3DFMT_A2B10G10R10:
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGB10A2Unorm;

  // Single-channel.
  case D3DFMT_A8:
    // A8 samples as (0, 0, 0, A) on D3D9 and the same on Metal; no
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
  case D3DFMT_Q16W16V16U16:
    // 4-channel signed-norm bump, a 1:1 match for Metal RGBA16Snorm (identity
    // channel order, no swizzle). d9vk / DXVK map it straight to
    // VK_FORMAT_R16G16B16A16_SNORM; wined3d supports it. Sampler-only in every
    // reference (never a render target).
    return usage == D3D9FormatUsage::DepthStencil ? WMTPixelFormatInvalid : WMTPixelFormatRGBA16Snorm;

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

  // Depth-stencil. Apple Silicon rejects Depth24Unorm_Stencil8 at
  // descriptor validation, so every 24-bit-depth D3D9 alias lowers to
  // Depth32Float_Stencil8 regardless of the stencil pad width
  // (D24X8 = no stencil but the create path treats it as a
  // depth-stencil because BindFlags include DS and the format
  // physically carries those 8 bits; D24X4S4 = 4-bit pad + 4-bit
  // stencil, same underlying storage on Apple). D32FS8 is also
  // permissive about D24FS8's float-depth contract: the float
  // layout matches.
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D24FS8:
  // D15S1 carries a stencil aspect (HasStencilAspect), so it joins the
  // combined depth-stencil alias rather than a depth-only format: the
  // 15-bit depth precision is over-provisioned, the same trade the
  // 24-bit aliases already make on Apple Silicon.
  case D3DFMT_D15S1:
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth32Float_Stencil8;
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth16Unorm;
  case D3DFMT_D32:
  case D3DFMT_D32F_LOCKABLE:
  case D3DFMT_D32_LOCKABLE:
    return usage == D3D9FormatUsage::RenderTarget ? WMTPixelFormatInvalid : WMTPixelFormatDepth32Float;

  // BC-compressed: DXT1→BC1, DXT2/3→BC2, DXT4/5→BC3. Sampler-only;
  // Apple Silicon rejects as render-targets at descriptor validation.
  case D3DFMT_DXT1:
    return usage != D3D9FormatUsage::SampleableTexture ? WMTPixelFormatInvalid : WMTPixelFormatBC1_RGBA;
  case D3DFMT_DXT2:
  case D3DFMT_DXT3:
    return usage != D3D9FormatUsage::SampleableTexture ? WMTPixelFormatInvalid : WMTPixelFormatBC2_RGBA;
  case D3DFMT_DXT4:
  case D3DFMT_DXT5:
    return usage != D3D9FormatUsage::SampleableTexture ? WMTPixelFormatInvalid : WMTPixelFormatBC3_RGBA;
  // ATI1N/ATI2N (3Dc): BC4/BC5 storage, spec channel shape restored by
  // D3DFormatSamplerSwizzle. Sampler-only like the DXTn family.
  case D3DFMT_ATI1:
    return usage != D3D9FormatUsage::SampleableTexture ? WMTPixelFormatInvalid : WMTPixelFormatBC4_RUnorm;
  case D3DFMT_ATI2:
    return usage != D3D9FormatUsage::SampleableTexture ? WMTPixelFormatInvalid : WMTPixelFormatBC5_RGUnorm;

  // Sampleable-depth FOURCC aliases. Same Metal storage as the
  // matching native depth format; legality on the SampleableTexture
  // path is what makes them special: apps bind these as DSV and SRV
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
  // 3Dc FOURCCs: the app-facing linear fiction is 1 byte per pixel (see
  // IsCompressedFormat's note); the real BC payload never surfaces here.
  case D3DFMT_ATI1:
  case D3DFMT_ATI2:
    return 1;
  case D3DFMT_A8:
  case D3DFMT_L8:
  // CPU-only scratchable formats with a 1-byte packed layout (see
  // IsScratchableUnsupportedFormat): the paletted P8, the 3-3-2 packed R3G3B2
  // and the 4-4 luminance-alpha A4L4. No Metal texture backs them; the bpp
  // feeds only the host-mirror LockRect pitch. Matches DXVK
  // GetUnsupportedFormatInfo / wined3d byte_count.
  case D3DFMT_P8:
  case D3DFMT_R3G3B2:
  case D3DFMT_A4L4:
    return 1;
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_X4R4G4B4:
  case D3DFMT_A4R4G4B4:
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
  // Packed YUV (a 2x1 macro-pixel carries Y0/U/Y1/V in 4 bytes, so 2 bytes
  // per pixel). Metal has no 422 format, so these exist only as CPU-only
  // SCRATCH resources; the bytes-per-pixel feeds their host-mirror pitch.
  case D3DFMT_YUY2:
  case D3DFMT_UYVY:
  // CPU-only scratchable formats with a 2-byte packed layout: paletted A8P8,
  // 3-3-2-plus-alpha A8R3G3B2 and the CxV8U8 bump variant. Host-mirror pitch
  // only. Matches DXVK GetUnsupportedFormatInfo / wined3d byte_count.
  case D3DFMT_A8P8:
  case D3DFMT_A8R3G3B2:
  case D3DFMT_CxV8U8:
    return 2;
  // Padded 24-bit RGB: CPU-only scratchable (no Metal RGB8 texture), 3-byte
  // host-mirror layout (DXVK r8b8g8 elementSize 3 / wined3d byte_count 3).
  case D3DFMT_R8G8B8:
    return 3;
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
  case D3DFMT_D24X4S4:
  case D3DFMT_D24FS8:
  case D3DFMT_D32:
  case D3DFMT_D32F_LOCKABLE:
  // INTZ apparent BPP is 4 (24-bit depth + 8-bit stencil) per wined3d
  // utils.c, but Apple Metal aliases the storage onto
  // Depth32Float_Stencil8: actually 8 bytes per texel. Today this
  // function only feeds LockRect's stride math, and INTZ textures
  // skip LockRect entirely (no cpu_ptr is allocated). If a future
  // readback path sizes a staging buffer by bpp*w*h, INTZ will
  // under-allocate by half: sizing must come from the Metal storage
  // descriptor, not this table.
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
    return 4;
  case D3DFMT_A16B16G16R16:
  case D3DFMT_A16B16G16R16F:
  case D3DFMT_G32R32F:
  case D3DFMT_Q16W16V16U16:
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
D3DFormatLockPitch(D3DFORMAT format, uint32_t width) {
  // Block-compressed pitches are already a multiple of 8, so the round-up
  // is a no-op for them and exact for the uncompressed bytes-per-row.
  uint32_t tight = D3DFormatRowPitch(format, width);
  return (tight + 3u) & ~3u;
}

// GPU-transfer geometry: bytes per block-row and block-row count for the
// Metal copy of a (possibly) block-compressed texture. Identical to
// RowPitch / RowCount for every format except the 3Dc FOURCCs, whose
// app-facing helpers speak the linear fiction while the Metal texture is
// real BC4 / BC5: transfers read the mirror as the contiguous block
// stream applications actually write.
uint32_t
D3DFormatMetalTransferPitch(D3DFORMAT format, uint32_t width) {
  switch (format) {
  case D3DFMT_ATI1:
    return ((width + 3u) / 4u) * 8u;
  case D3DFMT_ATI2:
    return ((width + 3u) / 4u) * 16u;
  default:
    return D3DFormatRowPitch(format, width);
  }
}

uint32_t
D3DFormatMetalTransferRows(D3DFORMAT format, uint32_t height) {
  switch (format) {
  case D3DFMT_ATI1:
  case D3DFMT_ATI2:
    return (height + 3u) / 4u;
  default:
    return D3DFormatRowCount(format, height);
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
  // S8_LOCKABLE (stencil-only) is intentionally absent: Metal has no
  // stencil-only pixel format, so D3DFormatToMetal cannot lower it and
  // the cap probe reports it unsupported rather than advertising a
  // format that fails at creation.
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
  case D3DFMT_DF16:
    return true;
  default:
    return false;
  }
}

bool
IsHardwarePCFDepthFormat(D3DFORMAT format) {
  // HW-PCF formats return filtered depth comparison on sample. The
  // vendor trio INTZ, DF16 and DF24 is blacklisted: those read back
  // raw depth (DXVK d3d9_common_texture.cpp keeps the same list;
  // FETCH4 exists so shaders filter the DF formats themselves).
  // Drives sample_compare codegen + the LessEqual sampler pairing.
  return IsDepthStencilFormat(format) && format != D3DFMT_INTZ && format != D3DFMT_DF16 && format != D3DFMT_DF24;
}

bool
IsMetalNonFilterableFormat(D3DFORMAT format) {
  // Apple-silicon Metal marks the 32-bit-float colour formats
  // sample-without-filter (Metal feature tables; MoltenVK marks R32Float /
  // RG32Float / RGBA32Float non-filterable on Apple GPUs). fp16 (R16F / G16R16F
  // / A16B16G16R16F) and the unorm formats filter fine, so only the fp32 trio
  // is listed. Depth formats are unaffected (Depth32Float filters and compares
  // on Apple GPUs; the HW-PCF path is validated).
  switch (format) {
  case D3DFMT_R32F:
  case D3DFMT_G32R32F:
  case D3DFMT_A32B32G32R32F:
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
  // ATI1N/ATI2N are deliberately NOT here: the app-facing contract for the
  // 3Dc FOURCCs is a linear 1-byte-per-pixel fiction (wined3d utils.c marks
  // them BROKEN_PITCH with byte_count 1, and wine's lockrect-offset test
  // pins pitch = width and offset = y * width + x), so every lock, mirror
  // and pitch computation treats them as plain 1-byte formats. Only the
  // Metal transfer uses their real BC block geometry, through
  // D3DFormatMetalTransferPitch / Rows.
  default:
    return false;
  }
}

bool
IsScratchableUnsupportedFormat(D3DFORMAT format) {
  // Formats Metal cannot realize as a sampled texture, but which the D3D9
  // runtime still creates as a system-memory SCRATCH copy (never bound or
  // sampled). CheckDeviceFormat reports them NOTAVAILABLE; the create paths back
  // them with a host mirror alone, the same shape as a DXTn SCRATCH volume, and
  // gate the create to D3DPOOL_SCRATCH. This mirrors DXVK's GetUnsupportedFormatInfo
  // set (the in-code comment there: the list exists "to allow for the creation
  // of offscreen plain surfaces"), which likewise permits only SCRATCH: packed
  // YUV, the paletted formats, padded 24-bit RGB, the 8-bit-packed colours, the
  // CxV8U8 bump variant and A4L4. Each has a defined bytes-per-pixel above so the
  // mirror pitch is nonzero (the create paths reject a zero-pitch format).
  switch (format) {
  case D3DFMT_YUY2:
  case D3DFMT_UYVY:
  case D3DFMT_P8:
  case D3DFMT_A8P8:
  case D3DFMT_R8G8B8:
  case D3DFMT_R3G3B2:
  case D3DFMT_A8R3G3B2:
  case D3DFMT_CxV8U8:
  case D3DFMT_A4L4:
    return true;
  default:
    return false;
  }
}

bool
IsVolumeTextureFormat(D3DFORMAT format) {
  // A 3D texture must lower to a Metal pixel format that Metal can allocate as a
  // volume. Depth/stencil have no sampleable 3D form; the block-compressed DXTn
  // + 3Dc FOURCCs and packed YUV have no 3D BC / 422 Metal format and take the
  // SCRATCH-blob mirror path instead. Everything else that maps is a volume.
  // Shared by the D3DRTYPE_VOLUMETEXTURE probe and CreateVolumeTexture so the
  // two never disagree (native + DXVK/wined3d advertise every mapped color
  // format for TEXTURE_3D).
  if (IsDepthStencilFormat(format) || IsCompressedFormat(format) || Is3DcFormat(format) ||
      IsScratchableUnsupportedFormat(format))
    return false;
  return D3DFormatToMetal(format, D3D9FormatUsage::SampleableTexture) != WMTPixelFormatInvalid;
}

uint32_t
D3DFormatBlockWidth(D3DFORMAT format) {
  if (IsCompressedFormat(format))
    return 4u;
  switch (format) {
  // A packed-YUV macropixel spans two horizontal texels (one chroma pair
  // shared by two luma samples), so a lock must start and end on a pair.
  case D3DFMT_YUY2:
  case D3DFMT_UYVY:
    return 2u;
  default:
    return 1u;
  }
}

uint32_t
D3DFormatBlockHeight(D3DFORMAT format) {
  // DXTn pack 4 rows per block; packed-YUV and plain formats are one row.
  return IsCompressedFormat(format) ? 4u : 1u;
}

bool
HasStencilAspect(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_D24S8:
  case D3DFMT_D24FS8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D15S1:
  // INTZ is the D24S8 alias: same dual-aspect storage. DF24/DF16
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

bool
isColorRTFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A2R10G10B10:
  case D3DFMT_A2B10G10R10:
  case D3DFMT_A16B16G16R16:
  case D3DFMT_A16B16G16R16F:
  case D3DFMT_R16F:
  case D3DFMT_G16R16F:
  case D3DFMT_R32F:
  case D3DFMT_G32R32F:
  case D3DFMT_A32B32G32R32F:
  case D3DFMT_G16R16:
  // ABGR pair: RGBA8Unorm is Metal's native layout; DXVK and wined3d
  // both report these RT-capable.
  case D3DFMT_A8B8G8R8:
  case D3DFMT_X8B8G8R8:
    return true;
  // D3DFMT_R8G8B8 (24-bit RGB) is intentionally not listed: Metal has
  // no padded-RGB8 pixel format and Vulkan/DXVK also reject it. Apps
  // that probe and get NOTAVAILABLE fall back to a 32-bit variant
  // (typically X8R8G8B8 -> BGRX8Unorm).
  default:
    return false;
  }
}

bool
IsFloatColorFormat(D3DFORMAT format) {
  // The fp16 and fp32 colour formats. They are valid render targets and RGB
  // conversion destinations, but carry no fixed-function blit cap, so they are
  // not valid CheckDeviceFormatConversion sources (wined3d utils.c).
  switch (format) {
  case D3DFMT_R16F:
  case D3DFMT_G16R16F:
  case D3DFMT_A16B16G16R16F:
  case D3DFMT_R32F:
  case D3DFMT_G32R32F:
  case D3DFMT_A32B32G32R32F:
    return true;
  default:
    return false;
  }
}

bool
IsGetDCCompatibleFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_A8R8G8B8:
    return true;
  default:
    return false;
  }
}

bool
IsFrontBufferReadbackSourceFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
  case D3DFMT_X8R8G8B8:
  case D3DFMT_R5G6B5:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5:
  case D3DFMT_A2R10G10B10:
    return true;
  default:
    return false;
  }
}

void
DecodeFrontBufferPixelToBGRA8(D3DFORMAT format, const uint8_t *src, uint8_t out[4]) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
    // Already BGRA8 in memory; carry the alpha through.
    out[0] = src[0];
    out[1] = src[1];
    out[2] = src[2];
    out[3] = src[3];
    return;
  case D3DFMT_X8R8G8B8:
    // Same BGR bytes; the X byte the source never wrote reads opaque.
    out[0] = src[0];
    out[1] = src[1];
    out[2] = src[2];
    out[3] = 0xFF;
    return;
  case D3DFMT_R5G6B5: {
    const uint16_t v = static_cast<uint16_t>(src[0] | (src[1] << 8));
    const uint32_t r5 = (v >> 11) & 0x1F;
    const uint32_t g6 = (v >> 5) & 0x3F;
    const uint32_t b5 = v & 0x1F;
    out[0] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    out[1] = static_cast<uint8_t>((g6 << 2) | (g6 >> 4));
    out[2] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    out[3] = 0xFF;
    return;
  }
  case D3DFMT_X1R5G5B5:
  case D3DFMT_A1R5G5B5: {
    const uint16_t v = static_cast<uint16_t>(src[0] | (src[1] << 8));
    const uint32_t r5 = (v >> 10) & 0x1F;
    const uint32_t g5 = (v >> 5) & 0x1F;
    const uint32_t b5 = v & 0x1F;
    out[0] = static_cast<uint8_t>((b5 << 3) | (b5 >> 2));
    out[1] = static_cast<uint8_t>((g5 << 3) | (g5 >> 2));
    out[2] = static_cast<uint8_t>((r5 << 3) | (r5 >> 2));
    // X1 reads opaque; A1 expands the single bit to 0x00 / 0xFF.
    out[3] = (format == D3DFMT_A1R5G5B5 && !(v & 0x8000)) ? 0x00 : 0xFF;
    return;
  }
  case D3DFMT_A2R10G10B10: {
    const uint32_t v = src[0] | (src[1] << 8) | (src[2] << 16) | (static_cast<uint32_t>(src[3]) << 24);
    // Narrow each 10-bit channel to its high 8 bits; expand the 2-bit alpha
    // by replication (a2 * 0x55 spans 0x00..0xFF).
    out[0] = static_cast<uint8_t>((v & 0x3FF) >> 2);
    out[1] = static_cast<uint8_t>(((v >> 10) & 0x3FF) >> 2);
    out[2] = static_cast<uint8_t>(((v >> 20) & 0x3FF) >> 2);
    out[3] = static_cast<uint8_t>(((v >> 30) & 0x3) * 0x55);
    return;
  }
  default:
    // Unreachable: frontBufferReadback gates on IsFrontBufferReadbackSourceFormat
    // before ever calling this. Leave opaque black so a future caller that skips
    // the gate gets a defined pixel rather than uninitialized bytes.
    out[0] = 0;
    out[1] = 0;
    out[2] = 0;
    out[3] = 0xFF;
    return;
  }
}

float
DepthBiasScale(D3DFORMAT format) {
  // Metal applies the bias as depth_bias * r, where r is the LOWERED Metal
  // format's minimum resolvable difference, so this returns 1/r to preserve
  // D3D9's normalized, format-independent bias. Apple lowers every D3D9 depth
  // format (D3DFormatToMetal) to one of two Metal formats, so the scale follows
  // the LOWERED format, not the D3D9 bit width:
  //   Depth16Unorm                        -> r = 2^-16 -> 1 << 16
  //   Depth32Float / Depth32Float_Stencil8 -> float r, taken as the 2^-23
  //     constant DXVK uses for float depth (its true per-fragment r is dynamic,
  //     so this is an approximation) -> 1 << 23
  // A 15/24/32-bit format that lowers to Depth32Float therefore takes 1 << 23,
  // NOT its own fixed-point 2^n: a native-width scale would over- or under-bias
  // and shift the depth-test boundary, mis-ordering biased geometry.
  switch (format) {
  case D3DFMT_D16:
  case D3DFMT_D16_LOCKABLE:
  case D3DFMT_DF16:
    return float(1 << 16);
  case D3DFMT_D15S1:
  case D3DFMT_D24S8:
  case D3DFMT_D24X8:
  case D3DFMT_D24X4S4:
  case D3DFMT_D24FS8:
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
  case D3DFMT_D32:
  case D3DFMT_D32_LOCKABLE:
  case D3DFMT_D32F_LOCKABLE:
    return float(1 << 23);
  default:
    return 1.0f;
  }
}

bool
D3DFormatHasNoAlpha(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_X8R8G8B8:
  case D3DFMT_X8B8G8R8:
  case D3DFMT_X1R5G5B5:
  case D3DFMT_X4R4G4B4:
    return true;
  default:
    return false;
  }
}

WMTTextureSwizzleChannels
D3DFormatSamplerSwizzle(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_X8R8G8B8:
    // D3D9 spec: X reads as 1. Metal's BGRX8 drops synthetic high-bit
    // on read, so force alpha=One at view time to match spec.
  case D3DFMT_X8B8G8R8:
    // (R, G, B, 1): base is RGBA8Unorm; Metal has no RGBX variant.
    // Same spec rationale as X8R8G8B8; without the swizzle Metal
    // returns the raw alpha byte.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleOne};
  case D3DFMT_L8:
    // (L, L, L, 1): base is R8Unorm.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleOne};
  case D3DFMT_L16:
    // (L, L, L, 1): base is R16Unorm. Same swizzle shape as L8.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleOne};
  case D3DFMT_A8L8:
    // (L, L, L, A): base is RG8Unorm with R=L, G=A.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleGreen};
  case D3DFMT_A4R4G4B4:
    // (G, B, A, R) over ABGR4Unorm: matches MoltenVK's VK_FORMAT_A4R4G4B4 map.
    return {WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha, WMTTextureSwizzleRed};
  case D3DFMT_X4R4G4B4:
    // Same mapping; X4 reads alpha as 1 (spec, mirrors X8R8G8B8).
    return {WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha, WMTTextureSwizzleOne};
  case D3DFMT_X1R5G5B5:
    // BGR5A1 stores the X bit as real alpha; spec says X reads as 1.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleOne};
  case D3DFMT_INTZ:
  case D3DFMT_DF24:
  case D3DFMT_DF16:
    // Depth-as-texture: Metal returns scalar red; replicate across RGBA
    // at view level for apps reading .gggg or .yyyy (soft-particle fades).
    // INTZ = RRRR is 3-way confirmed (d9vk INTZ swizzle RRRR; wined3d "XXXX"
    // fixup utils.c). DOC-DIV (C2-O10, refs split) for DF16/DF24: d9vk + DXVK
    // use (R,0,0,1), but wined3d has no DF16/DF24 support at all (grep: zero
    // hits), so there is no primary to break the tie and ATI hardware behavior
    // is the only oracle. Held as RRRR (consistent with INTZ) pending a
    // native/ATI-hardware oracle.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed, WMTTextureSwizzleRed};
  case D3DFMT_ATI1:
    // (R, 0, 0, 1) over BC4 (DXVK d3d9_format.cpp).
    return {WMTTextureSwizzleRed, WMTTextureSwizzleZero, WMTTextureSwizzleZero, WMTTextureSwizzleOne};
  case D3DFMT_ATI2:
    // (G, R, 1, 1) over BC5: 3Dc stores the channels swapped relative to
    // BC5's RG order (DXVK d3d9_format.cpp).
    return {WMTTextureSwizzleGreen, WMTTextureSwizzleRed, WMTTextureSwizzleOne, WMTTextureSwizzleOne};
  case D3DFMT_V8U8:
  case D3DFMT_V16U16:
  case D3DFMT_G16R16:
  case D3DFMT_G16R16F:
  case D3DFMT_G32R32F:
    // Two-channel formats: D3D9 samples the absent B/A as 1, but Metal's RG
    // base returns 0 for B. Force (R, G, 1, 1) (DXVK d3d9_format.cpp; wined3d
    // utils.c XY1W, applied on d3d9 via the legacy-unbound-color flag). Missing
    // 1 in .b is what BUMPENVMAPLUMINANCE reads off a V8U8 bump map.
    return {WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleOne, WMTTextureSwizzleOne};
  case D3DFMT_R16F:
  case D3DFMT_R32F:
    // Single-channel float: D3D9 samples the absent G/B as 1 (Metal returns 0).
    // Force (R, 1, 1, 1) (DXVK; wined3d X11W).
    return {WMTTextureSwizzleRed, WMTTextureSwizzleOne, WMTTextureSwizzleOne, WMTTextureSwizzleOne};
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
