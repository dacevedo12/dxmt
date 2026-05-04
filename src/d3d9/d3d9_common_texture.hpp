#pragma once

#include "Metal.hpp"
#include "d3d9.h"
#include "d3d9_view_cache.hpp"
#include "dxmt_texture.hpp"
#include "rc/util_rc_ptr.hpp"

namespace dxmt {

class MTLD3D9Device;

// Internal base for the device's bound-texture array (m_textures).
// IDirect3DBaseTexture9 doesn't expose private refcounting, and the
// concrete texture types (MTLD3D9Texture, MTLD3D9CubeTexture, future
// MTLD3D9VolumeTexture) live in separate ComObject hierarchies — but
// the device-side bind path needs to (a) hold a private ref so binding
// doesn't cycle the public refcount, (b) read the Metal handle for
// useResource / setFragmentTexture, (c) compare device identity to
// reject cross-device binds, (d) tag the type for the rare paths that
// care.
//
// Each concrete texture inherits from this and forwards the virtual
// AddRefPrivate / ReleasePrivate to the ComObject base it already
// has. The forward is a non-virtual tail call inside an inline body,
// so the per-bind cost is one virtual dispatch — fine for typical
// SetTexture rates and below the threshold of caching anything
// device-side.
//
// Reference: DXVK d3d9_common_texture.h takes a different shape — a
// non-IUnknown helper held by composition inside D3D9Texture* —
// because it also wants to share lock-tracking and dirty-region state
// across resource flavours. dxmt doesn't have that surface area yet,
// so a thinner virtual interface is the right shape until LockRect
// bookkeeping needs to migrate down.
class MTLD3D9CommonTexture {
public:
  virtual ~MTLD3D9CommonTexture() = default;

  // The Metal NSObject backing the texture. Cube textures return a
  // single TextureCube handle (6 faces share storage); 2D textures
  // return their TextureType2D allocation.
  virtual WMT::Texture metalTexture() const = 0;

  // Lazily-cached texture view for per-bind format/swizzle aliasing.
  // Returns the base m_texture for a default-key request (no
  // aliasing needed, common case — zero allocation). Sample-bind
  // calls with a non-default key when D3DSAMP_SRGBTEXTURE is set,
  // when a swizzled format like L8/A8L8 is in play, or for future
  // shadow sampling (#233). Implementation lives in each concrete
  // leaf's composed D3D9ViewCache; the interface is virtual to keep
  // m_textures[N]'s Com<MTLD3D9CommonTexture, false> view path uniform.
  virtual WMT::Texture viewFor(const D3D9ViewKey &key) = 0;

  // Underlying Metal pixel format (after d3d9-to-metal mapping, swizzle
  // flag bits stripped). Cached at ctor time — no wine_unix_call on
  // the bind hot path. Used to construct sRGB-aliased view keys via
  // Recall_sRGB and to skip the alias when no sRGB pair exists.
  virtual WMTPixelFormat metalPixelFormat() const = 0;

  // The original D3D9 format. Sample-bind reads this to apply per-
  // format swizzle (D3DFMT_L8 → {R,R,R,1}, D3DFMT_A8L8 → {R,R,R,G})
  // since Metal's R8Unorm / RG8Unorm samplers don't replicate the
  // luminance the way D3D9 expects. wined3d does the same swap at
  // bind time on its sampler_view path.
  virtual D3DFORMAT d3dFormat() const = 0;

  // The dxmt::Texture wrapper backing this resource, if one exists.
  // Required by the chunk-emit path to call ctx.access<Pixel>(rc, ..., Read)
  // for fragment-input bindings — that registers the read-after-write
  // fence dependency on prior encoders that wrote to the same texture
  // as an RT. Without it the accumulated-batch path lets a sample-pass
  // run in parallel with a write-pass within the same cmdbuf (Metal
  // does not infer this dependency from raw handles). Buffer-backed
  // textures (the i386 workaround path) have no wrapper and return a
  // null Rc<>; those resources fall back to the previous behaviour and
  // can race until they migrate to dxmt::Texture.
  virtual const Rc<dxmt::Texture> &dxmtTexture() const = 0;

  // Owning device — used for the cross-device check on SetTexture
  // without forcing an AddRef/Release pair on the hot path.
  virtual MTLD3D9Device *deviceRaw() const = 0;

  // D3D9 type tag — D3DRTYPE_TEXTURE / D3DRTYPE_CUBETEXTURE /
  // D3DRTYPE_VOLUMETEXTURE. Used by the SetTexture path to validate
  // and by future per-type binding logic (cube samplers needing a
  // texturecube view, etc.).
  virtual D3DRESOURCETYPE commonTextureType() const = 0;

  // D3D9 pool tag — used by SetTexture to gate D3DPOOL_SCRATCH per
  // MSDN ("SetTexture is not allowed if the texture is created with
  // a pool type of D3DPOOL_SCRATCH"). Each leaf forwards to its
  // m_pool. Same uniform-virtual rationale as commonTextureType.
  virtual D3DPOOL commonTexturePool() const = 0;

  // AUTOGENMIPMAP lazy-flush hooks. UnlockRect on level 0 of a
  // D3DUSAGE_AUTOGENMIPMAP texture used to fire generateMipmaps on a
  // dedicated cmdbuf — for NFS:MW's loading screen that's hundreds of
  // submissions back-to-back. The new shape: UnlockRect just sets
  // m_mips_dirty; the device's draw path scans bound textures before
  // opening the next render encoder and chains a single blit encoder
  // onto the open cmdbuf for any dirty texture, then opens the render
  // encoder so the generated mips are visible to the upcoming sample.
  // Coalesces N Unlocks into one encoder boundary per draw sequence.
  virtual bool mipsDirty() const = 0;
  virtual void clearMipsDirty() = 0;

  // Forward to ComObject<>::AddRefPrivate / ReleasePrivate on the
  // concrete leaf. Required for Com<MTLD3D9CommonTexture, false> —
  // the device's m_textures array uses that as a private-ref pin so
  // SetTexture binding doesn't cycle the app-visible refcount.
  //
  // Invariant — both call paths must touch the SAME counter:
  //   - leaf.AddRef()   → ComObject::AddRef → non-virtual ComObject::AddRefPrivate
  //   - Com<Common,false>::incRef → virtual override → ComObject::AddRefPrivate
  // The override forwards to ComObject<...>::AddRefPrivate explicitly
  // for that reason. A future refactor that introduces a clamped
  // refcount or a different counter for one path silently desyncs
  // the two and will leak / double-free; the override body is the
  // single source of truth.
  virtual void AddRefPrivate() = 0;
  virtual void ReleasePrivate() = 0;
};

} // namespace dxmt
