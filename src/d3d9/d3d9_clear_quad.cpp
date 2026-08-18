#include "d3d9_clear_quad.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"

namespace dxmt {

D3D9ClearQuad::D3D9ClearQuad(WMT::Device device, InternalCommandLibrary &lib) : device_(device) {
  auto library = lib.getLibrary();
  vs_clear_ = library.newFunction("vs_clear_rt");
  fs_clear_depth_ = library.newFunction("fs_clear_rt_depth");
  fs_clear_float_ = library.newFunction("fs_clear_rt_float");
  fs_clear_sint_ = library.newFunction("fs_clear_rt_sint");
  fs_clear_uint_ = library.newFunction("fs_clear_rt_uint");

  WMTDepthStencilInfo ds_info;
  ds_info.front_stencil.enabled = false;
  ds_info.back_stencil.enabled = false;
  ds_info.depth_compare_function = WMTCompareFunctionAlways;
  ds_info.depth_write_enabled = false;

  depth_readonly_state_ = device.newDepthStencilState(ds_info);

  ds_info.depth_write_enabled = true;

  depth_write_state_ = device.newDepthStencilState(ds_info);

  // Stencil clears write through the stencil op rather than a shader
  // export (MSL has none): compare Always + Replace on every outcome,
  // with the clear value supplied as the stencil reference. Same shape
  // as DepthStencilBlitContext's copy state.
  ds_info.front_stencil.enabled = true;
  ds_info.front_stencil.stencil_compare_function = WMTCompareFunctionAlways;
  ds_info.front_stencil.depth_stencil_pass_op = WMTStencilOperationReplace;
  ds_info.front_stencil.depth_fail_op = WMTStencilOperationReplace;
  ds_info.front_stencil.stencil_fail_op = WMTStencilOperationReplace;
  ds_info.front_stencil.write_mask = 0xFF;
  ds_info.front_stencil.read_mask = 0;
  ds_info.back_stencil = ds_info.front_stencil;

  depth_stencil_write_state_ = device.newDepthStencilState(ds_info);

  ds_info.depth_write_enabled = false;

  stencil_write_state_ = device.newDepthStencilState(ds_info);
}

void
D3D9ClearQuad::begin(ArgumentEncodingContext &ctx, Rc<Texture> texture, TextureViewKey view) {
  assert(!clearing_texture_);

  WMT::Reference<WMT::Error> err;

  auto format = texture->pixelFormat(view);
  auto dsv_flag = DepthStencilPlanarFlags(format);

  // Depth/stencil targets go through beginDepthStencil, which carries
  // the stencil plumbing this colour entry point doesn't bind.
  if (dsv_flag)
    return;

  // enforce array render target for now
  view = texture->checkViewUseArray(view, true);

  uint64_t pso_key = (uint64_t(format) << 8) | texture->sampleCount();
  if (!pso_cache_.contains(pso_key)) {
    WMTRenderPipelineInfo pipeline_info;
    WMT::InitializeRenderPipelineInfo(pipeline_info);
    pipeline_info.raster_sample_count = texture->sampleCount();
    pipeline_info.vertex_function = vs_clear_;
    if (IsIntegerFormat(format)) {
      pipeline_info.colors[0].pixel_format = format;
      if (MTLGetUnsignedIntegerFormat(format) == format) {
        pipeline_info.fragment_function = fs_clear_uint_;
      } else {
        pipeline_info.fragment_function = fs_clear_sint_;
      }
    } else {
      pipeline_info.colors[0].pixel_format = format;
      pipeline_info.fragment_function = fs_clear_float_;
    }
    pipeline_info.rasterization_enabled = true;
    pipeline_info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    auto pso = device_.newRenderPipelineState(pipeline_info, err);
    if (pso == nullptr) {
      ERR("Failed to create d3d9 clear-quad PSO of format ", format, ": ", err.description().getUTF8String());
    }
    pso_cache_.emplace(pso_key, std::move(pso));
  }

  WMT::RenderPipelineState pso = pso_cache_.at(pso_key);

  if (!pso)
    return;

  auto width = texture->width(view);
  auto height = texture->height(view);
  auto array_length = texture->arrayLength(view);
  auto &pass_info = *ctx.startRenderPass(0, 0, 1, 0);

  auto &color = pass_info.colors[0];
  color.attachment = ctx.access<PipelineStage::Pixel>(texture, view, ResourceAccess::Write);
  color.depth_plane = 0;
  color.load_action = WMTLoadActionLoad;
  color.store_action = WMTStoreActionStore;

  pass_info.render_target_width = width;
  pass_info.render_target_height = height;
  pass_info.render_target_array_length = array_length;
  pass_info.default_raster_sample_count = texture->sampleCount();

  auto &setpso = ctx.encodeRenderCommand<wmtcmd_render_setpso>();
  setpso.type = WMTRenderCommandSetPSO;
  setpso.pso = pso;

  auto &setvp = ctx.encodeRenderCommand<wmtcmd_render_setviewport>();
  setvp.type = WMTRenderCommandSetViewport;
  setvp.viewport = {0.0, 0.0, (double)width, (double)height, 0.0, 1.0};

  auto &setdsso = ctx.encodeRenderCommand<wmtcmd_render_setdsso>();
  setdsso.type = WMTRenderCommandSetDSSO;
  setdsso.dsso = depth_readonly_state_.handle;
  setdsso.stencil_ref = 0;

  clearing_texture_ = std::move(texture);
  clearing_texture_view_ = view;
}

void
D3D9ClearQuad::beginDepthStencil(
    ArgumentEncodingContext &ctx, Rc<Texture> texture, TextureViewKey view, unsigned write_flags, uint8_t stencil_ref
) {
  assert(!clearing_texture_);

  WMT::Reference<WMT::Error> err;

  auto format = texture->pixelFormat(view);
  auto dsv_flag = DepthStencilPlanarFlags(format);

  // fs_clear_rt_depth exports depth, so the pipeline demands a depth
  // attachment; stencil-only formats would need a no-export fragment.
  // No D3D9 depth-stencil format lowers to one, so don't carry it.
  if (!(dsv_flag & 1))
    return;

  write_flags &= dsv_flag;
  if (!write_flags)
    return;

  // enforce array render target for now
  view = texture->checkViewUseArray(view, true);

  uint64_t pso_key = (uint64_t(format) << 8) | texture->sampleCount();
  if (!pso_cache_.contains(pso_key)) {
    WMTRenderPipelineInfo pipeline_info;
    WMT::InitializeRenderPipelineInfo(pipeline_info);
    pipeline_info.raster_sample_count = texture->sampleCount();
    pipeline_info.vertex_function = vs_clear_;
    pipeline_info.fragment_function = fs_clear_depth_;
    pipeline_info.depth_pixel_format = format;
    if (dsv_flag & 2)
      pipeline_info.stencil_pixel_format = format;
    pipeline_info.rasterization_enabled = true;
    pipeline_info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    auto pso = device_.newRenderPipelineState(pipeline_info, err);
    if (pso == nullptr) {
      ERR("Failed to create d3d9 clear-quad depth-stencil PSO of format ", format, ": ",
          err.description().getUTF8String());
    }
    pso_cache_.emplace(pso_key, std::move(pso));
  }

  WMT::RenderPipelineState pso = pso_cache_.at(pso_key);

  if (!pso)
    return;

  auto width = texture->width(view);
  auto height = texture->height(view);
  auto array_length = texture->arrayLength(view);
  auto &pass_info = *ctx.startRenderPass(dsv_flag, 0, 0, 0);

  auto &depth = pass_info.depth;
  depth.attachment = ctx.access<PipelineStage::Pixel>(texture, view, ResourceAccess::Write);
  depth.depth_plane = 0;
  depth.load_action = WMTLoadActionLoad;
  depth.store_action = WMTStoreActionStore;
  if (dsv_flag & 2) {
    // The pass carries the stencil plane whenever the PSO declares it,
    // even for a depth-only write; the depth-stencil state keeps the
    // untouched plane intact.
    auto &stencil = pass_info.stencil;
    stencil.attachment = ctx.access<PipelineStage::Pixel>(texture, view, ResourceAccess::Write);
    stencil.depth_plane = 0;
    stencil.load_action = WMTLoadActionLoad;
    stencil.store_action = WMTStoreActionStore;
  }

  pass_info.render_target_width = width;
  pass_info.render_target_height = height;
  pass_info.render_target_array_length = array_length;
  pass_info.default_raster_sample_count = texture->sampleCount();

  auto &setpso = ctx.encodeRenderCommand<wmtcmd_render_setpso>();
  setpso.type = WMTRenderCommandSetPSO;
  setpso.pso = pso;

  auto &setvp = ctx.encodeRenderCommand<wmtcmd_render_setviewport>();
  setvp.type = WMTRenderCommandSetViewport;
  setvp.viewport = {0.0, 0.0, (double)width, (double)height, 0.0, 1.0};

  auto &setdsso = ctx.encodeRenderCommand<wmtcmd_render_setdsso>();
  setdsso.type = WMTRenderCommandSetDSSO;
  switch (write_flags) {
  case 1:
    setdsso.dsso = depth_write_state_.handle;
    break;
  case 2:
    setdsso.dsso = stencil_write_state_.handle;
    break;
  default:
    setdsso.dsso = depth_stencil_write_state_.handle;
    break;
  }
  setdsso.stencil_ref = stencil_ref;

  clearing_texture_ = std::move(texture);
  clearing_texture_view_ = view;
}

void
D3D9ClearQuad::clear(
    ArgumentEncodingContext &ctx, uint32_t offset_x, uint32_t offset_y, uint32_t width, uint32_t height,
    const std::array<float, 4> &color
) {
  if (!clearing_texture_)
    return;
  auto &setscr = ctx.encodeRenderCommand<wmtcmd_render_setscissorrect>();
  setscr.type = WMTRenderCommandSetScissorRect;
  setscr.scissor_rect = {offset_x, offset_y, width, height};

  auto &setcolor = ctx.encodeRenderCommand<wmtcmd_render_setbytes>();
  setcolor.type = WMTRenderCommandSetFragmentBytes;
  void *temp = ctx.allocate_cpu_heap(sizeof(color), 16);
  memcpy(temp, color.data(), sizeof(color));
  setcolor.bytes.set(temp);
  setcolor.length = sizeof(color);
  setcolor.index = 0;

  auto &draw = ctx.encodeRenderCommand<wmtcmd_render_draw>();
  draw.type = WMTRenderCommandDraw;
  draw.primitive_type = WMTPrimitiveTypeTriangle;
  draw.vertex_start = 0;
  draw.vertex_count = 3;
  draw.base_instance = 0;
  draw.instance_count = ctx.currentRenderEncoder()->render_target_array_length;
}

void
D3D9ClearQuad::end(ArgumentEncodingContext &ctx) {
  if (!clearing_texture_)
    return;
  ctx.endPass();
  clearing_texture_ = nullptr;
  clearing_texture_view_ = 0;
}

} // namespace dxmt
