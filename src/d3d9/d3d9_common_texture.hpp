#pragma once

#include "Metal.hpp"
#include "d3d9.h"
#include "dxmt_texture.hpp"
#include "rc/util_rc_ptr.hpp"

namespace dxmt {

class MTLD3D9Device;

// Internal base for the device's bound-texture array; enables private
// refcounting, Metal handle reads, device identity checks, and type tags
// without exposing IDirect3DBaseTexture9 public refcount.
// Each concrete texture forwards AddRefPrivate/ReleasePrivate calls,
// keeping per-bind cost to one virtual dispatch.
class MTLD3D9CommonTexture {
public:
  virtual ~MTLD3D9CommonTexture() = default;

  // The Metal NSObject backing the texture. Cube textures return a
  // single TextureCube handle (6 faces share storage); 2D textures
  // return their TextureType2D allocation.
  virtual WMT::Texture metalTexture() const = 0;

  // Underlying Metal pixel format (after d3d9-to-metal mapping, swizzle
  // flag bits stripped). Cached at ctor time; no wine_unix_call on
  // the bind hot path. Used to construct sRGB-aliased view keys via
  // Recall_sRGB and to skip the alias when no sRGB pair exists.
  virtual WMTPixelFormat metalPixelFormat() const = 0;

  // The original D3D9 format. Sample-bind reads this to apply per-
  // format swizzle (D3DFMT_L8 → {R,R,R,1}, D3DFMT_A8L8 → {R,R,R,G})
  // since Metal's R8Unorm / RG8Unorm samplers don't replicate the
  // luminance the way D3D9 expects. wined3d does the same swap at
  // bind time on its sampler_view path.
  virtual D3DFORMAT d3dFormat() const = 0;

  // The dxmt::Texture wrapper backing this resource; always non-null for a live texture. Chunk-emit calls
  // ctx.access<Pixel> on it for read-after-write fences and view resolution. Buffer-backed textures route through
  // Texture::wrapBuffer (closing UAF if buffer freed before GPU completion).
  virtual const Rc<dxmt::Texture> &dxmtTexture() const = 0;

  // Owning device; used for the cross-device check on SetTexture
  // without forcing an AddRef/Release pair on the hot path.
  virtual MTLD3D9Device *deviceRaw() const = 0;

  // D3D9 type tag: D3DRTYPE_TEXTURE / D3DRTYPE_CUBETEXTURE /
  // D3DRTYPE_VOLUMETEXTURE. Used by the SetTexture path to validate
  // and by future per-type binding logic (cube samplers needing a
  // texturecube view, etc.).
  virtual D3DRESOURCETYPE commonTextureType() const = 0;

  // D3D9 pool tag; used by SetTexture to gate D3DPOOL_SCRATCH per
  // MSDN ("SetTexture is not allowed if the texture is created with
  // a pool type of D3DPOOL_SCRATCH"). Each leaf forwards to its
  // m_pool. Same uniform-virtual rationale as commonTextureType.
  virtual D3DPOOL commonTexturePool() const = 0;

  // Per-texture LOD floor: D3D9's SetLOD(N) on a MANAGED texture says
  // the runtime is allowed to sample only mips N..(level_count-1).
  // The sample-bind path reads this and derives a mip-clamped view via
  // dxmt::Texture::checkViewUseMipRange so the Metal sampler can't reach
  // the excluded levels. Concrete leaves forward their m_lod field.
  // wined3d threads the same value through
  // wined3d_texture_create_sampler_view_object's NSRange-equivalent.
  virtual uint32_t commonTextureLod() const = 0;

  // AUTOGENMIPMAP lazy-flush: UnlockRect sets m_mips_dirty; the device's
  // draw path chains a single blit encoder for all dirty textures,
  // coalescing N Unlocks into one encoder boundary per draw.
  virtual bool mipsDirty() const = 0;
  virtual void clearMipsDirty() = 0;

  // Forward to ComObject<>::AddRefPrivate / ReleasePrivate on the
  // concrete leaf. Invariant: both call paths must touch the SAME counter
  // to prevent refcount desync, leaks, or double-frees.
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

} // namespace dxmt
