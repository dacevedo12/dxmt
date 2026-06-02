
#include <algorithm>
#include <chrono>
#include "Metal.hpp"
#include "dxmt_format.hpp"
#include "dxmt_presenter.hpp"
#include "log/log.hpp"
#include "util_likely.hpp"

namespace dxmt {

Presenter::Presenter(WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count) :
    device_(device),
    layer_(layer),
    lib_(lib),
    sample_count_(sample_count) {
  layer_.getProps(layer_props_);
  layer_props_.device = device;
  layer_props_.opaque = true;
  layer_props_.display_sync_enabled = false;
  layer_props_.framebuffer_only = false; // how strangely setting it true results in worse performance
  layer_props_.contents_scale = layer_props_.contents_scale * scale_factor;

  WMTTextureInfo texture_info;
  texture_info.type = WMTTextureType2D;
  texture_info.pixel_format = WMTPixelFormatRGBA32Float;
  texture_info.usage = WMTTextureUsageShaderRead;
  texture_info.options = WMTResourceStorageModeShared;
  texture_info.width = DXMT_GAMMA_CP_COUNT;
  texture_info.height = 1;
  texture_info.depth = 1;
  texture_info.mipmap_level_count = 1;
  texture_info.sample_count = 1;
  texture_info.array_length = 1;
  gamma_lut_texture_ = device.newTexture(texture_info);
}

bool
Presenter::changeLayerProperties(
    WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
) {
  bool should_invalidated = colorspace_ != colorspace;
  should_invalidated |= source_format_ != format;
  source_format_ = format;
  should_invalidated |= sample_count_ != sample_count;
  sample_count_ = sample_count;
  layer_props_.pixel_format = Forget_sRGB(format);
  layer_props_.drawable_height = height;
  layer_props_.drawable_width = width;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear(); // defer changes
  else
    layer_.setProps(layer_props_);
  return should_invalidated;
}

bool
Presenter::changeLayerColorSpace(WMTColorSpace colorspace) {
  bool should_invalidated = colorspace_ != colorspace;
  colorspace_ = colorspace;
  if (should_invalidated)
    pso_valid.clear();
  return should_invalidated;
}

void
Presenter::changeHDRMetadata(const WMTHDRMetadata *metadata) {
  if (metadata) {
    has_hdr_metadata_ = true;
    hdr_metadata_ = *metadata;
    pso_valid.clear();
  } else {
    if (has_hdr_metadata_) {
      has_hdr_metadata_ = false;
      pso_valid.clear();
    }
  }
}

void
Presenter::changeGammaRamp(const DXMTGammaRamp *gamma_ramp) {
  if (!gamma_ramp && gamma_version_ != 0) {
    gamma_version_ = 0;
    pso_valid.clear();
    return;
  }
  if (gamma_ramp && gamma_version_ != gamma_ramp->version) {
    gamma_version_ = gamma_ramp->version;
    for (uint32_t i = 0; i < DXMT_GAMMA_CP_COUNT; i++) {
      gamma_lut_rgba_[i * 4 + 0] = std::clamp(gamma_ramp->red[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 1] = std::clamp(gamma_ramp->green[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 2] = std::clamp(gamma_ramp->blue[i], 0.f, 1.f);
      gamma_lut_rgba_[i * 4 + 3] = 1.0f;
    }
    // Defer the texture upload to synchronizeLayerProperties. Writing the
    // shared-storage LUT here (calling thread) races the encode thread / GPU
    // sampling it for an in-flight present; torn gamma during animated
    // fades. pso_valid.clear() already forces synchronizeLayerProperties to
    // rebuild after frame_presented_.wait() has drained every in-flight
    // present, so the upload rides that existing drain for free.
    gamma_lut_dirty_ = true;
    pso_valid.clear();
  }
}

Presenter::PresentState
Presenter::synchronizeLayerProperties() {
  uint64_t display_setting_version = 0;

  WMTQueryDisplaySettingForLayer(
      layer_.handle, &display_setting_version, &display_colorspace_, &display_hdr_metadata_, &display_edr_value_
  );

  if (display_setting_version != display_setting_version_) {
    display_setting_version_ = display_setting_version;
    pso_valid.clear();
  }

  auto final_colorspace = display_setting_version_ > 0 ? display_colorspace_ : colorspace_;
  auto is_hdr = WMT_COLORSPACE_IS_HDR(final_colorspace);
  auto hdr_metadata = display_setting_version_ > 0 ? &display_hdr_metadata_
                      : has_hdr_metadata_          ? &hdr_metadata_
                                                   : nullptr;
  if (unlikely(!pso_valid.test_and_set())) {
    frame_presented_.wait(frame_requested_);
    // Drained: no present is in flight, so the shared-storage gamma LUT can
    // be uploaded without racing a GPU read (see changeGammaRamp).
    if (gamma_lut_dirty_) {
      gamma_lut_texture_.replaceRegion(
          {0, 0, 0}, {DXMT_GAMMA_CP_COUNT, 1, 1}, 0, 0, gamma_lut_rgba_.data(),
          DXMT_GAMMA_CP_COUNT * sizeof(float) * 4, 0
      );
      gamma_lut_dirty_ = false;
    }
    gamma_enabled_ = gamma_version_ != 0;
    buildRenderPipelineState(final_colorspace == WMTColorSpaceHDR_PQ, is_hdr && hdr_metadata != nullptr, sample_count_ > 1, gamma_enabled_);
    layer_.setProps(layer_props_);
    layer_.setColorSpace(final_colorspace);
  }

  DXMTPresentMetadata metadata;

  metadata.edr_scale = 1.0;
  metadata.max_content_luminance = 10000;
  metadata.max_display_luminance = display_edr_value_.maximum_potential_edr_color_component_value * 100;
  metadata.alpha_mode = present_alpha_mode_;
  metadata.background_color[0] = 0.0f;
  metadata.background_color[1] = 0.0f;
  metadata.background_color[2] = 0.0f;
  metadata.background_color[3] = 1.0f;

  if (is_hdr) {
    metadata.edr_scale = display_edr_value_.maximum_edr_color_component_value /
                display_edr_value_.maximum_potential_edr_color_component_value;

    if (hdr_metadata) {
      metadata.max_content_luminance = std::max<uint32_t>(
          {(uint32_t)metadata.max_display_luminance, hdr_metadata->max_content_light_level,
           hdr_metadata->max_mastering_luminance, hdr_metadata->max_frame_average_light_level}
      );
    }
  }

  if (final_colorspace == WMTColorSpaceHDR_scRGB)
    metadata.edr_scale *= 0.8;

  return {metadata, ++frame_requested_, this};
}

WMT::MetalDrawable
Presenter::encodeCommands(
    WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
    std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
    std::function<void(WMT::RenderCommandEncoder)> &&update_fences, uint64_t *out_next_drawable_us
) {
  auto t0 = std::chrono::steady_clock::now();
  auto drawable = layer_.nextDrawable();
  if (out_next_drawable_us) {
    *out_next_drawable_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
  }
  auto drawable_tex = drawable.texture();

  WMTRenderPassInfo info;
  WMT::InitializeRenderPassInfo(info);
  info.colors[0].load_action = WMTLoadActionClear;
  info.colors[0].clear_color = {0.0, 0.0, 0.0, 1.0};
  info.colors[0].store_action = WMTStoreActionStore;
  info.colors[0].texture = drawable_tex;
  info.render_target_width = drawable_tex.width();
  info.render_target_height = drawable_tex.height();
  info.render_target_array_length = drawable_tex.arrayLength();
  info.default_raster_sample_count = 1;
  auto encoder = cmdbuf.renderCommandEncoder(info);
  wait_fences(encoder);
  encoder.setFragmentTexture(backbuffer, 0);
  // Only bind the gamma LUT when the live PSO variant samples it. When no
  // gamma ramp is set, fs_present_quad is compiled without the gamma path
  // and never reads texture index 1; binding it anyway leaves an unused
  // binding that MTL_DEBUG_LAYER's end-encoding validation flags.
  if (gamma_enabled_)
    encoder.setFragmentTexture(gamma_lut_texture_, 1);

  // Present target size = this frame's live drawable, not layer_props_.
  // The swapchain mutates layer_props_.drawable_* on the calling thread
  // (its per-Present resize poll); reading it here on the encode thread
  // would be a cross-thread race. The drawable we just acquired is the
  // authoritative render-target extent: and matching the viewport/blit
  // decision to it is what we actually render into this frame.
  double width = (double)drawable_tex.width();
  double height = (double)drawable_tex.height();

  encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
  bool size_matched = backbuffer.width() == drawable_tex.width() && backbuffer.height() == drawable_tex.height();
  encoder.setRenderPipelineState(size_matched ? present_blit_ : present_scale_);
  encoder.setViewport({0, 0, width, height, 0, 1});

  if (unlikely(DXMT_LOG_ENABLED(dxmt::LogLevel::Trace))) {
    // Backbuffer vs live drawable only; the cached layer_props_ extent is
    // logged race-free on the calling thread by d3d9_swapchain's "layer
    // extent:" line; reading it here on the encode thread would reintroduce
    // a cross-thread read of a calling-thread-mutated field. Deduped to one
    // line per size transition so the per-event trace tier isn't spammed.
    uint64_t bw = backbuffer.width(), bh = backbuffer.height();
    uint64_t lw = drawable_tex.width(), lh = drawable_tex.height();
    if (bw != dbg_last_bb_w_ || bh != dbg_last_bb_h_ || lw != dbg_last_live_w_ || lh != dbg_last_live_h_) {
      dbg_last_bb_w_ = bw, dbg_last_bb_h_ = bh;
      dbg_last_live_w_ = lw, dbg_last_live_h_ = lh;
      TRACE(
          "present: backbuffer ", bw, "x", bh, " live-drawable ", lw, "x", lh, " -> ",
          size_matched ? "BLIT (1:1)" : "SCALE (bilinear)"
      );
    }
  }
  encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
  update_fences(encoder);
  encoder.endEncoding();

  return drawable;
}

constexpr uint32_t kPresentFCIndex_BackbufferSizeMatched = 0x100;
constexpr uint32_t kPresentFCIndex_HDRPQ = 0x101;
constexpr uint32_t kPresentFCIndex_WithHDRMetadata = 0x102;
constexpr uint32_t kPresentFCIndex_BackbufferIsSRGB = 0x103;
constexpr uint32_t kPresentFCIndex_BackbufferIsMS = 0x104;
constexpr uint32_t kPresentFCIndex_GammaEnabled = 0x105;

void
Presenter::buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable) {
  auto pool = WMT::MakeAutoreleasePool();

  auto library = lib_.getLibrary();

  uint32_t true_data = true, false_data = false;
  WMTFunctionConstant constants[6];
  constants[0].data.set(&true_data);
  constants[0].type = WMTDataTypeBool;
  constants[0].index = kPresentFCIndex_BackbufferSizeMatched;
  constants[1].data.set(is_pq ? &true_data : &false_data);
  constants[1].type = WMTDataTypeBool;
  constants[1].index = kPresentFCIndex_HDRPQ;
  constants[2].data.set(with_hdr_metadata ? &true_data : &false_data);
  constants[2].type = WMTDataTypeBool;
  constants[2].index = kPresentFCIndex_WithHDRMetadata;
  constants[3].data.set(Is_sRGBVariant(source_format_) ? &true_data : &false_data);
  constants[3].type = WMTDataTypeBool;
  constants[3].index = kPresentFCIndex_BackbufferIsSRGB;
  constants[4].data.set(is_ms ? &true_data : &false_data);
  constants[4].type = WMTDataTypeBool;
  constants[4].index = kPresentFCIndex_BackbufferIsMS;
  constants[5].data.set(gamma_enable ? &true_data : &false_data);
  constants[5].type  = WMTDataTypeBool;
  constants[5].index = kPresentFCIndex_GammaEnabled;

  WMT::Reference<WMT::Error> error;
  auto vs_present_quad = library.newFunction("vs_present_quad");
  auto fs_present_quad = library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);

  constants[0].data.set(&false_data);
  auto fs_present_quad_scaled =
      library.newFunctionWithConstants("fs_present_quad", constants, std::size(constants), error);
  {
    WMTRenderPipelineInfo present_pipeline;
    WMT::InitializeRenderPipelineInfo(present_pipeline);
    present_pipeline.colors[0].pixel_format = layer_props_.pixel_format;
    present_pipeline.vertex_function = vs_present_quad;
    present_pipeline.fragment_function = fs_present_quad;
    present_blit_ = device_.newRenderPipelineState(present_pipeline, error);
    present_pipeline.fragment_function = fs_present_quad_scaled;
    present_scale_ = device_.newRenderPipelineState(present_pipeline, error);
  }
}

} // namespace dxmt
