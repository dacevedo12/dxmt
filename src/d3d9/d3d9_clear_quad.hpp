#pragma once

#include "Metal.hpp"
#include "dxmt_command.hpp"
#include "dxmt_texture.hpp"
#include <array>
#include <unordered_map>

namespace dxmt {

class ArgumentEncodingContext;

// d3d9-private scissored clear quad for Clear's sub-rect regions
// (viewport / scissor / pRects). The shared ClearRenderTargetContext
// serves d3d11 ClearView, whose spec only allows colour and depth-only
// DSV formats; D3D9 Clear also clips depth and stencil on the combined
// formats every D3D9 depth buffer lowers to, so the d3d9 path needs a
// wider helper without changing what d3d11 encodes. Like the
// swapchain's Presenter, it mints its own PSOs against functions from
// the device's InternalCommandLibrary (vs_clear_rt / fs_clear_rt_*);
// only the compiled library data is shared.
//
// Usage (encode thread only; methods take the ArgumentEncodingContext
// the chunk lambda receives): begin or beginDepthStencil opens a
// Load/Store render pass on the target view and binds the PSO + DSSO,
// clear() draws one scissored fullscreen triangle per region, end()
// closes the pass. The begin/end texture state is single-threaded
// encode-side state, mirroring ClearRenderTargetContext's shape.
class D3D9ClearQuad {
public:
  D3D9ClearQuad(WMT::Device device, InternalCommandLibrary &lib);

  void begin(ArgumentEncodingContext &ctx, Rc<Texture> texture, TextureViewKey view);

  // Depth/stencil variant; accepts combined depth-stencil formats.
  // write_flags selects the planes to clear (bit 0 depth, bit 1
  // stencil); the stencil value writes through the Replace stencil op
  // driven by stencil_ref, the depth value rides clear()'s colour
  // argument in .x (the lane fs_clear_rt_depth exports).
  void beginDepthStencil(
      ArgumentEncodingContext &ctx, Rc<Texture> texture, TextureViewKey view, unsigned write_flags, uint8_t stencil_ref
  );

  void clear(
      ArgumentEncodingContext &ctx, uint32_t offset_x, uint32_t offset_y, uint32_t width, uint32_t height,
      const std::array<float, 4> &color
  );

  void end(ArgumentEncodingContext &ctx);

private:
  WMT::Device device_;
  WMT::Reference<WMT::Function> vs_clear_;
  WMT::Reference<WMT::Function> fs_clear_float_;
  WMT::Reference<WMT::Function> fs_clear_uint_;
  WMT::Reference<WMT::Function> fs_clear_sint_;
  WMT::Reference<WMT::Function> fs_clear_depth_;
  WMT::Reference<WMT::DepthStencilState> depth_readonly_state_;
  WMT::Reference<WMT::DepthStencilState> depth_write_state_;
  WMT::Reference<WMT::DepthStencilState> depth_stencil_write_state_;
  WMT::Reference<WMT::DepthStencilState> stencil_write_state_;
  // Keyed on (format << 8) | sample count: the PSO bakes
  // raster_sample_count in, so an MSAA target can't share the
  // single-sample entry of the same format. A given key maps to
  // exactly one pipeline descriptor (colour, depth-only or combined
  // depth-stencil follow from the format itself). Unlike the bytecode
  // and PSO caches, this key is a bijective PACK of a small finite
  // state space, not a lossy 64-bit hash: the sample count (1/2/4/8,
  // always < 256) occupies the low byte and the WMTPixelFormat enum
  // (values far below 2^56) the rest, so distinct (format, sample
  // count) pairs can never share a key. It therefore needs no full-key
  // verify on a hit; the pack itself is the proof.
  std::unordered_map<uint64_t, WMT::Reference<WMT::RenderPipelineState>> pso_cache_;
  Rc<Texture> clearing_texture_;
  TextureViewKey clearing_texture_view_;
};

} // namespace dxmt
