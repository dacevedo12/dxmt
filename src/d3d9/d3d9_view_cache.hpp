#pragma once

#include "Metal.hpp"
#include "winemetal.h"

#include <cstdint>
#include <unordered_map>

namespace dxmt {

// Per-d3d9-texture view cache. d3d9 binds the raw `m_texture` to
// setFragmentTexture / render-pass attachments today; that's fine
// when no aliasing is needed but blocks D3DSAMP_SRGBTEXTURE (per-stage
// sRGB sample-time decode) and D3DRS_SRGBWRITEENABLE (per-PSO sRGB
// write encode), both of which require a Metal texture view with a
// different pixel format than the parent.
//
// The d3d11 path uses a richer dxmt::Texture / dxmt::TextureView pair
// (src/dxmt/dxmt_texture.cpp:192-216) keyed off TextureAllocation. d3d9
// doesn't use TextureAllocation and migrating it is out of scope; this
// is the parallel structure tailored for d3d9's flat ownership.
//
// Default key: zero-initialised (pixel_format=0, identity swizzle,
// mip 0..0xFFFF, slice 0..0xFFFF). A default key returns the base
// texture handle without creating a Metal view — common-case zero
// cost. Non-default keys hit the unordered_map and create on miss.
//
// Cache lifetime: owned by the texture (composition). Views are held
// by strong WMT::Reference<WMT::Texture> so they outlive any encoder
// that binds them; the cache is freed when the texture is.
struct D3D9ViewKey {
  // Metal pixel format the view should expose. 0 (WMTPixelFormatInvalid)
  // means "use the parent's format" — common for swizzle-only views.
  uint32_t pixel_format = 0;
  // Channel swizzle. Zero-initialised value is {Zero, Zero, Zero,
  // Zero} — sentinel that the cache recognises as "no swizzle
  // override; use identity {R,G,B,A}". Callers either leave the
  // struct zero-initialised or supply explicit channels.
  WMTTextureSwizzleChannels swizzle{};
  // Mip range. {0, 0xFFFF} means "all levels of the parent". Any
  // narrower range creates a view limited to those levels.
  uint16_t mip_start = 0;
  uint16_t mip_count = 0xFFFF;
  // Array slice range, same convention as mip range. For 2D textures
  // the parent has 1 slice; cube textures have 6; 2D-arrays have N.
  uint16_t slice_start = 0;
  uint16_t slice_count = 0xFFFF;

  bool
  is_default() const {
    return pixel_format == 0 && swizzle.r == WMTTextureSwizzleZero && swizzle.g == WMTTextureSwizzleZero &&
           swizzle.b == WMTTextureSwizzleZero && swizzle.a == WMTTextureSwizzleZero && mip_start == 0 &&
           mip_count == 0xFFFF && slice_start == 0 && slice_count == 0xFFFF;
  }

  bool
  operator==(const D3D9ViewKey &other) const {
    return pixel_format == other.pixel_format && swizzle.r == other.swizzle.r && swizzle.g == other.swizzle.g &&
           swizzle.b == other.swizzle.b && swizzle.a == other.swizzle.a && mip_start == other.mip_start &&
           mip_count == other.mip_count && slice_start == other.slice_start && slice_count == other.slice_count;
  }
};

struct D3D9ViewKeyHash {
  size_t
  operator()(const D3D9ViewKey &k) const noexcept {
    // FNV-1a over the 16-byte key. The struct is densely packed so a
    // byte-by-byte hash is fine; std::hash_combine isn't worth pulling
    // in for this.
    uint64_t h = 0xcbf29ce484222325ull;
    auto mix = [&](uint64_t v) {
      h ^= v;
      h *= 0x100000001b3ull;
    };
    mix(static_cast<uint64_t>(k.pixel_format));
    uint32_t sw = static_cast<uint32_t>(k.swizzle.r) | (static_cast<uint32_t>(k.swizzle.g) << 8) |
                  (static_cast<uint32_t>(k.swizzle.b) << 16) | (static_cast<uint32_t>(k.swizzle.a) << 24);
    mix(static_cast<uint64_t>(sw));
    mix((static_cast<uint64_t>(k.mip_start) << 48) | (static_cast<uint64_t>(k.mip_count) << 32) |
        (static_cast<uint64_t>(k.slice_start) << 16) | static_cast<uint64_t>(k.slice_count));
    return static_cast<size_t>(h);
  }
};

class D3D9ViewCache {
public:
  // The caller passes the parent texture's handle and texture type
  // (WMTTextureType2D / Cube / 3D / 2DArray). Format is implied by the
  // parent — keys that leave pixel_format=0 inherit it.
  D3D9ViewCache() = default;

  void
  reset(WMT::Texture base, WMTTextureType type, WMTPixelFormat parent_format) {
    m_base = base;
    m_type = type;
    m_parent_format = parent_format;
    // Query the parent's actual mip/slice extents once. Per-bind
    // viewFor() resolves the 0xFFFF "all" sentinels against these so
    // newTextureView never sees the raw sentinel — Metal validates
    // (start + count) against the parent's real range and rejects
    // anything beyond it.
    m_mip_count = base != nullptr ? static_cast<uint16_t>(base.mipmapLevelCount()) : uint16_t{1};
    // Metal exposes cube faces as 6 "slices" through newTextureView's
    // NSRange even though MTLTexture.arrayLength reports 1 for a plain
    // Cube. Derive the slice count from the texture type, scaling by
    // arrayLength for the array variants.
    const uint16_t array_len = base != nullptr ? static_cast<uint16_t>(base.arrayLength()) : uint16_t{1};
    switch (type) {
    case WMTTextureTypeCube:
      m_slice_count = 6;
      break;
    case WMTTextureTypeCubeArray:
      m_slice_count = static_cast<uint16_t>(6u * (array_len ? array_len : 1u));
      break;
    case WMTTextureType2DArray:
    case WMTTextureType2DMultisampleArray:
      m_slice_count = array_len ? array_len : uint16_t{1};
      break;
    default:
      m_slice_count = 1;
      break;
    }
    m_cache.clear();
  }

  // Returns the base texture for default keys (zero allocation cost),
  // or a cached/freshly-created texture view for non-default keys.
  // The returned WMT::Texture is non-owning — caller must hold the
  // cache (and therefore this texture's parent) alive while the view
  // is in use. Views are retained inside the cache via
  // WMT::Reference<WMT::Texture>.
  WMT::Texture
  viewFor(const D3D9ViewKey &key) {
    if (key.is_default())
      return m_base;
    auto it = m_cache.find(key);
    if (it != m_cache.end())
      return it->second;
    WMTTextureSwizzleChannels swizzle = key.swizzle;
    // Sentinel zero swizzle → identity. wined3d's GLSL alias does the
    // same: an "unspecified" swizzle reads through unchanged.
    if (swizzle.r == WMTTextureSwizzleZero && swizzle.g == WMTTextureSwizzleZero &&
        swizzle.b == WMTTextureSwizzleZero && swizzle.a == WMTTextureSwizzleZero) {
      swizzle = WMTTextureSwizzleChannels{
          WMTTextureSwizzleRed, WMTTextureSwizzleGreen, WMTTextureSwizzleBlue, WMTTextureSwizzleAlpha
      };
    }
    WMTPixelFormat fmt = key.pixel_format != 0 ? static_cast<WMTPixelFormat>(key.pixel_format) : m_parent_format;
    uint64_t gpu_id = 0;
    // Resolve the 0xFFFF "all remaining" sentinels against the parent's
    // real extents — Metal's NSMakeRange(start, count) is bounds-checked
    // and rejects any range past the parent's mip/slice count. Without
    // this, default-shape keys that picked up a non-default
    // pixel_format / swizzle (the common sRGB / luminance-swizzle path)
    // hit _mtlValidateArgumentsForTextureViewOnDevice with count=65535.
    const uint16_t lvl_start = key.mip_start;
    const uint16_t lvl_count =
        (key.mip_count == 0xFFFF) ? static_cast<uint16_t>(m_mip_count - lvl_start) : key.mip_count;
    const uint16_t slc_start = key.slice_start;
    const uint16_t slc_count =
        (key.slice_count == 0xFFFF) ? static_cast<uint16_t>(m_slice_count - slc_start) : key.slice_count;
    auto view = m_base.newTextureView(fmt, m_type, lvl_start, lvl_count, slc_start, slc_count, swizzle, gpu_id);
    if (view == nullptr)
      return m_base; // Fallback: aliasing failed (e.g. format pair
                     // unsupported at runtime). Callers degrade
                     // gracefully — sRGB read on an unaliasable format
                     // just samples linear.
    auto raw = view; // hold ref until insert
    m_cache.emplace(key, std::move(view));
    return raw;
  }

private:
  WMT::Texture m_base;
  WMTTextureType m_type = WMTTextureType2D;
  WMTPixelFormat m_parent_format = static_cast<WMTPixelFormat>(0);
  // Cached parent extents — populated at reset() so per-bind viewFor()
  // resolves the 0xFFFF sentinels without firing wine_unix_calls into
  // the metal property accessors on the hot path.
  uint16_t m_mip_count = 1;
  uint16_t m_slice_count = 1;
  std::unordered_map<D3D9ViewKey, WMT::Reference<WMT::Texture>, D3D9ViewKeyHash> m_cache;
};

} // namespace dxmt
