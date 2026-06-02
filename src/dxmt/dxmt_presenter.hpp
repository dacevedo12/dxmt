#pragma once
#include "Metal.hpp"
#include "dxmt_command.hpp"
#include "rc/util_rc.hpp"
#include "util_cpu_fence.hpp"
#include "winemetal.h"
#include <atomic>
#include <cstdint>

namespace dxmt {

struct DXMTPresentMetadata {
  float edr_scale;
  float max_content_luminance;
  float max_display_luminance;
  uint32_t alpha_mode;
  float background_color[4];
};

constexpr uint32_t DXMT_GAMMA_CP_COUNT = 1024;

struct DXMTGammaRamp {
  float red[DXMT_GAMMA_CP_COUNT];
  float green[DXMT_GAMMA_CP_COUNT];
  float blue[DXMT_GAMMA_CP_COUNT];
  uint64_t version;
};

class Presenter : public RcObject {
public:
  Presenter(
      WMT::Device device, WMT::MetalLayer layer, InternalCommandLibrary &lib, float scale_factor, uint8_t sample_count
  );

  bool changeLayerProperties(
      WMTPixelFormat format, WMTColorSpace colorspace, double width, double height, uint8_t sample_count
  );

  bool changeLayerColorSpace(WMTColorSpace colorspace);

  void changeHDRMetadata(const WMTHDRMetadata *metadata);

  void changeGammaRamp(const DXMTGammaRamp *gamma_ramp);

  // Compositing mode for the final present blit, following DXGI_ALPHA_MODE:
  // 0 UNSPECIFIED / 2 STRAIGHT scale the output by the backbuffer alpha,
  // 1 PREMULTIPLIED adds it, 3 IGNORE presents the colour opaque. The
  // upstream default (0) suits DXGI layer compositing, where the backbuffer
  // alpha is a real coverage value. A D3D9 present is an opaque copy and the
  // backbuffer alpha is scene scratch, so the d3d9 swapchain selects IGNORE.
  void setPresentAlphaMode(uint32_t mode) { present_alpha_mode_ = mode; }

  class PresentState {
  public:
    DXMTPresentMetadata metadata;
    uint64_t frame_id;
    Presenter *presenter;
    PresentState(DXMTPresentMetadata metadata, uint64_t frame_id, Presenter *presenter) :
        metadata(metadata),
        frame_id(frame_id),
        presenter(presenter) {}
    PresentState(const PresentState &copy) = delete;
    PresentState(PresentState &&move)  {
      metadata = move.metadata;
      frame_id = move.frame_id;
      presenter = move.presenter;
      move.presenter = nullptr;
    };
    ~PresentState() {
      if (presenter) {
        presenter->frame_presented_.signal(frame_id);
        presenter = nullptr;
      }
    };
  };

  PresentState synchronizeLayerProperties();

  WMT::MetalDrawable encodeCommands(
      WMT::CommandBuffer cmdbuf, WMT::Texture backbuffer, DXMTPresentMetadata metadata,
      std::function<void(WMT::RenderCommandEncoder)> &&wait_fences,
      std::function<void(WMT::RenderCommandEncoder)> &&update_fences,
      // Optional out-param: microseconds spent inside layer_.nextDrawable.
      // Useful to disambiguate "encode is slow" from "GPU back-pressure
      // blocks drawable acquisition"; the latter shows up as a multi-
      // hundred-millisecond duration. Default nullptr ignores.
      uint64_t *out_next_drawable_us = nullptr
  );

private:
  void buildRenderPipelineState(bool is_pq, bool with_hdr_metadata, bool is_ms, bool gamma_enable);

  WMT::Device device_;
  WMT::MetalLayer layer_;
  InternalCommandLibrary &lib_;
  WMTLayerProps layer_props_;
  uint32_t sample_count_;
  WMTColorSpace colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata hdr_metadata_;
  bool has_hdr_metadata_ = false;
  WMTPixelFormat source_format_ = WMTPixelFormatInvalid;
  uint64_t display_setting_version_ = 0;
  WMTColorSpace display_colorspace_ = WMTColorSpaceSRGB;
  WMTHDRMetadata display_hdr_metadata_;
  WMTEDRValue display_edr_value_{0.0, 1.0};
  uint64_t gamma_version_ = 0;
  std::array<float, DXMT_GAMMA_CP_COUNT * 4> gamma_lut_rgba_;
  WMT::Reference<WMT::Texture> gamma_lut_texture_;
  // Set by changeGammaRamp (calling thread) when gamma_lut_rgba_ holds new
  // data; the actual gamma_lut_texture_ upload is deferred to
  // synchronizeLayerProperties, which runs it after the present drain so the
  // shared-storage texture is never written while the GPU samples it.
  bool gamma_lut_dirty_ = false;
  WMT::Reference<WMT::RenderPipelineState> present_blit_;
  WMT::Reference<WMT::RenderPipelineState> present_scale_;
  // Whether the live present PSO was built with the gamma function constant
  // set; i.e. whether its fragment shader samples the gamma LUT at texture
  // index 1. Set alongside the PSO rebuild in synchronizeLayerProperties and
  // read in encodeCommands so the LUT is bound only when the shader uses it;
  // shares the PSOs' calling-thread-build / encode-thread-read handoff.
  bool gamma_enabled_ = false;
  // DXGI_ALPHA_MODE for the present composite (see setPresentAlphaMode). The
  // default 0 preserves the upstream DXGI path; d3d9 overrides it to 3 (IGNORE).
  uint32_t present_alpha_mode_ = 0;
  std::atomic_flag pso_valid = 0;
  uint64_t frame_requested_ = 0;
  CpuFence frame_presented_ = 0;

  // Present blit-vs-scale trace (DXMT_LOG_LEVEL=trace): encodeCommands logs
  // backbuffer vs live-drawable size whenever either changes, to catch
  // sharpness regressions after a windowed<->fullscreen mode change.
  // Zero-cost above trace level; last_* dedupe to one line per transition.
  uint64_t dbg_last_bb_w_ = 0, dbg_last_bb_h_ = 0;
  uint64_t dbg_last_live_w_ = 0, dbg_last_live_h_ = 0;
};
} // namespace dxmt
