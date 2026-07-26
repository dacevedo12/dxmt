#include "d3d12_replay_temporal_upscale.hpp"

#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_replay_state_ops.hpp"
#include "d3d12_resource.hpp"
#include "log/log.hpp"

#include <cstdint>
#include <utility>

namespace dxmt::d3d12 {

void
ReplayTemporalUpscale(WMT::Device device,
                      std::vector<CachedTemporalScaler> &scaler_cache,
                      CommandChunk *chunk,
                      const TemporalUpscaleRecord &record) {
  auto *color_resource = GetResource(record.color.ptr());
  auto *output_resource = GetResource(record.output.ptr());
  auto *depth_resource = GetResource(record.depth.ptr());
  auto *motion_resource = GetResource(record.motion_vector.ptr());
  auto *exposure_resource = GetResource(record.exposure_texture.ptr());

  if (!color_resource || !output_resource || !depth_resource ||
      !motion_resource) {
    WARN("D3D12CommandQueue: temporal upscale skipped foreign resource");
    EmitPassSeparator(chunk);
    return;
  }
  if ((color_resource->IsReservedTexture() &&
       !color_resource->EnsureTextureAllocation("TemporalUpscale color")) ||
      (output_resource->IsReservedTexture() &&
       !output_resource->EnsureTextureAllocation("TemporalUpscale output")) ||
      (depth_resource->IsReservedTexture() &&
       !depth_resource->EnsureTextureAllocation("TemporalUpscale depth")) ||
      (motion_resource->IsReservedTexture() &&
       !motion_resource->EnsureTextureAllocation("TemporalUpscale motion")) ||
      (exposure_resource && exposure_resource->IsReservedTexture() &&
       !exposure_resource->EnsureTextureAllocation("TemporalUpscale exposure"))) {
    WARN("D3D12CommandQueue: temporal upscale skipped unmaterialized reserved texture");
    EmitPassSeparator(chunk);
    return;
  }

  Rc<Texture> input = Rc<Texture>(color_resource->GetTexture());
  Rc<Texture> output = Rc<Texture>(output_resource->GetTexture());
  Rc<Texture> depth = Rc<Texture>(depth_resource->GetTexture());
  Rc<Texture> motion_vector = Rc<Texture>(motion_resource->GetTexture());
  Rc<Texture> exposure;
  if (exposure_resource)
    exposure = Rc<Texture>(exposure_resource->GetTexture());

  if (!input || !output || !depth || !motion_vector ||
      (record.exposure_texture && !exposure)) {
    WARN("D3D12CommandQueue: temporal upscale skipped non-texture resource");
    EmitPassSeparator(chunk);
    return;
  }

  if (!record.input_content_width || !record.input_content_height ||
      record.input_content_width > input->width() ||
      record.input_content_height > input->height()) {
    WARN("D3D12CommandQueue: temporal upscale invalid input content size ",
         record.input_content_width, "x", record.input_content_height,
         " for input texture ", input->width(), "x", input->height());
    EmitPassSeparator(chunk);
    return;
  }

  const uint32_t motion_width =
      record.motion_vector_width ? record.motion_vector_width
                                 : motion_vector->width();
  const uint32_t motion_height =
      record.motion_vector_height ? record.motion_vector_height
                                  : motion_vector->height();
  if (!motion_width || !motion_height) {
    WARN("D3D12CommandQueue: temporal upscale invalid motion vector size");
    EmitPassSeparator(chunk);
    return;
  }

  const WMTPixelFormat motion_vector_source_format =
      TemporalUpscaleMotionVectorSourceFormat(motion_vector->pixelFormat());
  if (motion_vector_source_format == WMTPixelFormatInvalid) {
    WARN("D3D12CommandQueue: temporal upscale invalid motion vector format ",
         motion_vector->pixelFormat());
    EmitPassSeparator(chunk);
    return;
  }
  const WMTPixelFormat motion_texture_format =
      TemporalUpscaleMotionTextureFormat(
          motion_vector_source_format, record.motion_vector_in_display_res);

  Rc<TemporalScaler> scaler;
  Rc<Texture> mv_downscaled;
  for (auto &entry : scaler_cache) {
    if (bool(record.auto_exposure) != entry.auto_exposure)
      continue;
    if (bool(record.motion_vector_in_display_res) !=
        entry.motion_vector_in_display_res)
      continue;
    if (input->width() != entry.input_width ||
        input->height() != entry.input_height)
      continue;
    if (motion_width != entry.motion_width ||
        motion_height != entry.motion_height)
      continue;
    if (output->width() != entry.output_width ||
        output->height() != entry.output_height)
      continue;
    if (input->pixelFormat() != entry.color_pixel_format ||
        output->pixelFormat() != entry.output_pixel_format ||
        depth->pixelFormat() != entry.depth_pixel_format ||
        motion_texture_format != entry.motion_texture_pixel_format)
      continue;

    scaler = entry.scaler;
    mv_downscaled = entry.mv_downscaled;
    break;
  }

  if (!scaler) {
    CachedTemporalScaler entry = {};
    entry.color_pixel_format = input->pixelFormat();
    entry.output_pixel_format = output->pixelFormat();
    entry.depth_pixel_format = depth->pixelFormat();
    entry.motion_texture_pixel_format = motion_texture_format;
    entry.auto_exposure = record.auto_exposure;
    entry.motion_vector_in_display_res = record.motion_vector_in_display_res;
    entry.input_width = input->width();
    entry.input_height = input->height();
    entry.motion_width = motion_width;
    entry.motion_height = motion_height;
    entry.output_width = output->width();
    entry.output_height = output->height();

    WMTFXTemporalScalerInfo info = {};
    info.color_format = entry.color_pixel_format;
    info.output_format = entry.output_pixel_format;
    info.depth_format = entry.depth_pixel_format;
    info.motion_format = entry.motion_texture_pixel_format;
    info.input_width = entry.input_width;
    info.input_height = entry.input_height;
    info.output_width = entry.output_width;
    info.output_height = entry.output_height;
    info.input_content_min_scale = 1.0f;
    info.input_content_max_scale = 3.0f;
    info.auto_exposure = entry.auto_exposure;
    info.input_content_properties_enabled = true;
    info.requires_synchronous_initialization = true;
    entry.scaler = new TemporalScaler(device, info);

    if (record.motion_vector_in_display_res) {
      WMTTextureInfo tex_info = {};
      tex_info.width = entry.input_width;
      tex_info.height = entry.input_height;
      tex_info.depth = 1;
      tex_info.array_length = 1;
      tex_info.mipmap_level_count = 1;
      tex_info.pixel_format = WMTPixelFormatRG32Float;
      tex_info.sample_count = 1;
      tex_info.type = WMTTextureType2D;
      tex_info.usage =
          WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
      tex_info.options = WMTResourceStorageModePrivate;
      entry.mv_downscaled = new Texture(tex_info, device);
      Flags<TextureAllocationFlag> flags;
      flags.set(TextureAllocationFlag::GpuPrivate);
      entry.mv_downscaled->rename(entry.mv_downscaled->allocate(flags));
    }

    scaler = entry.scaler;
    mv_downscaled = entry.mv_downscaled;
    scaler_cache.push_back(std::move(entry));
  }

  WMTFXTemporalScalerProps props = {};
  props.input_content_width = record.input_content_width;
  props.input_content_height = record.input_content_height;
  props.reset = record.in_reset;
  props.depth_reversed = record.depth_reversed;
  props.motion_vector_scale_x = record.motion_vector_scale_x;
  props.motion_vector_scale_y = record.motion_vector_scale_y;
  props.jitter_offset_x = record.jitter_offset_x;
  props.jitter_offset_y = record.jitter_offset_y;
  props.pre_exposure = record.pre_exposure;

  chunk->emitcc([input = std::move(input), output = std::move(output),
                 depth = std::move(depth),
                 motion_vector = std::move(motion_vector),
                 exposure = std::move(exposure), scaler = std::move(scaler),
                 props, motion_vector_source_format,
                 mv_downscaled = std::move(mv_downscaled)](
                    ArgumentEncodingContext &enc) mutable {
    auto mv_view = motion_vector->createView(
        {.format = motion_vector_source_format,
         .type = WMTTextureType2D,
         .firstMiplevel = 0,
         .miplevelCount = 1,
         .firstArraySlice = 0,
         .arraySize = 1});
    auto &scaler_info = enc.currentFrameStatistics().last_scaler_info;
    scaler_info.type = ScalerType::Temporal;
    scaler_info.auto_exposure = exposure == nullptr;
    scaler_info.input_width = props.input_content_width;
    scaler_info.input_height = props.input_content_height;
    scaler_info.output_width = output->width();
    scaler_info.output_height = output->height();
    scaler_info.motion_vector_highres = mv_downscaled != nullptr;

    if (scaler_info.motion_vector_highres) {
      enc.mv_scale_cmd.dispatch(motion_vector, mv_view, mv_downscaled, 0,
                                props.motion_vector_scale_x,
                                props.motion_vector_scale_y);
      WMTFXTemporalScalerProps downscaled_props = props;
      downscaled_props.motion_vector_scale_x = 1.0f;
      downscaled_props.motion_vector_scale_y = 1.0f;
      enc.upscaleTemporal(input, output, depth, mv_downscaled, 0, exposure,
                          scaler, downscaled_props);
    } else {
      enc.upscaleTemporal(input, output, depth, motion_vector, mv_view,
                          exposure, scaler, props);
    }
  });
}

} // namespace dxmt::d3d12
