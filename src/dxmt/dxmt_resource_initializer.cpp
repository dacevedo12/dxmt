#include "Metal.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_format.hpp"
#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"
#include "util_math.hpp"
#include <cstdint>
#include <mutex>

namespace dxmt {

static bool
ValidateInitializerRenderTargetAttachment(const WMTColorAttachmentInfo &attachment) {
  if (!attachment.texture)
    return true;

  WMT::Texture texture{attachment.texture};
  auto actual_usage = texture.usage();
  if (actual_usage & WMTTextureUsageRenderTarget)
    return true;

  WARN(
      "ResourceInitializer guard: color attachment missing WMTTextureUsageRenderTarget",
      " actual_usage=", uint32_t(actual_usage),
      " texture=", attachment.texture,
      " level=", attachment.level,
      " slice=", attachment.slice,
      " depth_plane=", attachment.depth_plane,
      " load_action=", uint32_t(attachment.load_action),
      " store_action=", uint32_t(attachment.store_action)
  );
  return false;
}

#define ALLOC_BLIT(type, cmd)                                                                                          \
  type *cmd = nullptr;                                                                                                 \
  if (!allocateBlit(&cmd)) {                                                                                           \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_CLEAR(info)                                                                                              \
  WMTRenderPassInfo *info;                                                                                             \
  if (!allocateClear(&info)) {                                                                                         \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_GPU(buffer, size)                                                                                        \
  WMT::Buffer buffer;                                                                                                  \
  size_t buffer##_offset;                                                                                              \
  if (!(buffer = allocateGpuHeap(size, buffer##_offset))) {                                                            \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_ZERO(buffer, size)                                                                                       \
  WMT::Buffer buffer;                                                                                                  \
  if (!(buffer = allocateZeroBuffer(size))) {                                                                          \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define RETAIN(allocation)                                                                                             \
  if (!retainAllocation(allocation)) {                                                                                 \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

ResourceInitializer::ResourceInitializer(WMT::Device device) :
    device_(device),
    gpu_command_heap_allocator(StagingBufferBlockAllocator(
        device, WMTResourceStorageModeManaged | WMTResourceHazardTrackingModeUntracked, false
    )) {
  upload_queue_ = device.newCommandQueue(kResourceInitializerChunks);
  upload_queue_event_ = device.newSharedEvent();

  cpu_command_heap_size = kResourceInitializerCpuCommandHeapSize;
  cpu_command_heap = malloc(cpu_command_heap_size);
  reset();
}

ResourceInitializer::~ResourceInitializer() {
  std::lock_guard<dxmt::mutex> lock(mutex_);
  for (size_t slot = 0; slot < in_flight_batches_.size(); slot++)
    retireInFlightBatch(slot);
  ref_tracker.clear();
  free(cpu_command_heap);
}

uint64_t
ResourceInitializer::initWithZero(BufferAllocation *buffer, uint64_t offset, uint64_t length) {
  std::lock_guard<dxmt::mutex> lock(mutex_);

  do {
    RETAIN(buffer);
    ALLOC_BLIT(wmtcmd_blit_fillbuffer, fill);
    fill->type = WMTBlitCommandFillBuffer;
    fill->buffer = buffer->buffer();
    fill->offset = offset;
    fill->length = length;
    fill->value = 0;
    recordDiagnosticResource(buffer);

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initDepthStencilWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level, uint32_t dsv_planar,
    float depth, uint8_t stencil
) {
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);

  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    RETAIN(allocation);
    ALLOC_CLEAR(info);

    info->render_target_array_length = 1;
    info->render_target_width = width_sub;
    info->render_target_height = height_sub;
    info->default_raster_sample_count = texture->sampleCount();
    if (dsv_planar & 1) {
      info->depth.texture = allocation->texture();
      info->depth.clear_depth = depth;
      info->depth.load_action = WMTLoadActionClear;
      info->depth.store_action = WMTStoreActionStore;
      info->depth.slice = slice;
      info->depth.level = level;
    }
    if (dsv_planar & 2) {
      info->stencil.texture = allocation->texture();
      info->stencil.clear_stencil = stencil;
      info->stencil.load_action = WMTLoadActionClear;
      info->stencil.store_action = WMTStoreActionStore;
      info->stencil.slice = slice;
      info->stencil.level = level;
      info->stencil.depth_plane = 0;
    }
    recordDiagnosticResource(texture, allocation->texture());

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initRenderTargetWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level,
    WMTClearColor color
) {
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);

  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    RETAIN(allocation);
    ALLOC_CLEAR(info);

    info->render_target_array_length =
        texture->textureType() == WMTTextureType3D
            ? std::max(1u, texture->depth() >> level)
            : 1;
    info->render_target_width = width_sub;
    info->render_target_height = height_sub;
    info->default_raster_sample_count = texture->sampleCount();
    info->colors[0].texture = allocation->texture();
    info->colors[0].load_action = WMTLoadActionClear;
    info->colors[0].clear_color = color;
    info->colors[0].store_action = WMTStoreActionStore;
    info->colors[0].slice = slice;
    info->colors[0].level = level;
    recordDiagnosticResource(texture, allocation->texture());

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level
) {
  if (auto dsv_planar = DepthStencilPlanarFlags(texture->pixelFormat())) {
    return initDepthStencilWithZero(texture, allocation, slice, level, dsv_planar);
  }
  if (texture->usage() & WMTTextureUsageRenderTarget) {
    return initRenderTargetWithZero(texture, allocation, slice, level);
  }

  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);
  auto depth_sub = std::max(1u, texture->depth() >> level);

  auto block_size = 1u;

  switch (texture->pixelFormat()) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC2_RGBA_sRGB:
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC3_RGBA_sRGB:
  case WMTPixelFormatBC4_RSnorm:
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC5_RGUnorm:
  case WMTPixelFormatBC5_RGSnorm:
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC7_RGBAUnorm:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    block_size = 4u;
    break;
  default:
    break;
  }

  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  size_t texel_size = MTLGetTexelSize(texture->pixelFormat());
  size_t bytes_per_row_needed = texel_size * align(width_sub, block_size) / block_size;
  size_t bytes_per_image_needed = bytes_per_row_needed * align(height_sub, block_size) / block_size;
  size_t total_bytes_needed = bytes_per_image_needed * depth_sub;

  std::lock_guard<dxmt::mutex> lock(mutex_);

  do {
    ALLOC_ZERO(zero, total_bytes_needed);
    RETAIN(allocation);
    ALLOC_BLIT(wmtcmd_blit_copy_from_buffer_to_texture, copy);

    copy->type = WMTBlitCommandCopyFromBufferToTexture;
    copy->src = zero;
    copy->src_offset = 0;
    copy->bytes_per_row = bytes_per_row_needed;
    copy->bytes_per_image = is_3d_tex ? bytes_per_image_needed : 0;
    copy->size = {width_sub, height_sub, depth_sub};
    copy->dst = allocation->texture();
    copy->slice = slice;
    copy->level = level;
    copy->origin = {0, 0, 0};
    recordDiagnosticResource(texture, allocation->texture());

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initWithData(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level, const void *data,
    size_t row_pitch, size_t depth_pitch
) {
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);
  auto depth_sub = std::max(1u, texture->depth() >> level);

  auto block_size = 1u;

  switch (texture->pixelFormat()) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC2_RGBA_sRGB:
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC3_RGBA_sRGB:
  case WMTPixelFormatBC4_RSnorm:
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC5_RGUnorm:
  case WMTPixelFormatBC5_RGSnorm:
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC7_RGBAUnorm:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    block_size = 4u;
    break;
  default:
    break;
  }

  bool is_1d_tex = (texture->textureType() == WMTTextureType1D) || (texture->textureType() == WMTTextureType1DArray);
  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  size_t texel_size = MTLGetTexelSize(texture->pixelFormat());
  size_t bytes_per_row_needed = texel_size * align(width_sub, block_size) / block_size;
  size_t bytes_per_row_increment = is_1d_tex ? bytes_per_row_needed : row_pitch;
  size_t bytes_per_row_valid = is_1d_tex ? bytes_per_row_needed : std::min(row_pitch, bytes_per_row_needed);
  size_t bytes_per_image_needed = bytes_per_row_needed * align(height_sub, block_size) / block_size;
  size_t bytes_per_image_increment = is_3d_tex ? depth_pitch : bytes_per_image_needed;
  size_t bytes_per_image_valid = is_3d_tex ? std::min(depth_pitch, bytes_per_image_needed) : bytes_per_image_needed;
  size_t total_bytes_needed = bytes_per_image_needed * depth_sub;

  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    RETAIN(allocation);
    ALLOC_BLIT(wmtcmd_blit_copy_from_buffer_to_texture, copy);
    ALLOC_GPU(temp, total_bytes_needed);

    for (auto depth = 0u; depth < depth_sub; depth++) {
      if (bytes_per_row_increment != bytes_per_row_needed) {
        for (auto row = 0u; row < (align(height_sub, block_size) / block_size); row++) {
          auto offset = temp_offset + depth * bytes_per_image_needed + row * bytes_per_row_needed;
          auto length = bytes_per_row_valid;
          auto src_data = ptr_add(data, depth * bytes_per_image_increment + row * bytes_per_row_increment);
          temp.updateContents(offset, src_data, length);
        }
      } else {
        auto offset = temp_offset + depth * bytes_per_image_needed;
        auto length = bytes_per_image_valid;
        auto src_data = ptr_add(data, depth * bytes_per_image_increment);
        temp.updateContents(offset, src_data, length);
      }
    }

    copy->type = WMTBlitCommandCopyFromBufferToTexture;
    copy->src = temp;
    copy->src_offset = temp_offset;
    copy->bytes_per_row = bytes_per_row_needed;
    copy->bytes_per_image = is_3d_tex ? bytes_per_image_needed : 0;
    copy->size = {width_sub, height_sub, depth_sub};
    copy->dst = allocation->texture();
    copy->slice = slice;
    copy->level = level;
    copy->origin = {0, 0, 0};
    recordDiagnosticResource(texture, allocation->texture());

  } while (0);

  return current_seq_id_;
}

std::uint64_t
ResourceInitializer::flushInternal() {
  auto pool = WMT::MakeAutoreleasePool();

  auto seq_id = current_seq_id_++;
  auto slot = (seq_id - 1) % in_flight_batches_.size();
  retireInFlightBatch(slot);

  auto cmdbuf = upload_queue_.commandBuffer();
  encode(cmdbuf);
  cmdbuf.encodeSignalEvent(upload_queue_event_, seq_id);
  cmdbuf.commit();

  auto &batch = in_flight_batches_[slot];
  batch.command_buffer = cmdbuf;
  batch.event_id = seq_id;
  ref_tracker.transferTo(batch.allocations);
  batch.resource_diagnostics = std::move(pending_resource_diagnostics_);
  batch.operation_diagnostics = std::move(pending_operation_diagnostics_);
  reset();
  cached_coherent_seq_id = upload_queue_event_.signaledValue();
  gpu_command_heap_allocator.free_blocks(cached_coherent_seq_id);
  return seq_id;
}

void
ResourceInitializer::retireInFlightBatch(size_t slot) {
  auto &batch = in_flight_batches_[slot];
  if (!batch.event_id)
    return;

  if (upload_queue_event_.signaledValue() < batch.event_id) {
    if (!perf::enabled()) {
      upload_queue_event_.waitUntilSignaledValue(batch.event_id, UINT64_MAX);
    } else if (!upload_queue_event_.waitUntilSignaledValue(batch.event_id, 1000)) {
      WARN(
          "ResourceInitializer in-flight batch stall: slot=", slot,
          " commandBuffer=", batch.command_buffer.handle,
          " event=", upload_queue_event_.handle,
          " target=", batch.event_id,
          " current=", upload_queue_event_.signaledValue(),
          " allocationCount=", batch.allocations.size(),
          " resourceCount=", batch.resource_diagnostics.size(),
          " operationCount=", batch.operation_diagnostics.size()
      );
      for (size_t i = 0; i < batch.allocations.size(); i++) {
        WARN(
            "ResourceInitializer in-flight allocation: target=", batch.event_id,
            " index=", i,
            " allocation=", batch.allocations[i]
        );
      }
      for (size_t i = 0; i < batch.resource_diagnostics.size(); i++) {
        WARN(
            "ResourceInitializer in-flight resource: target=", batch.event_id,
            " index=", i,
            " ", batch.resource_diagnostics[i]
        );
      }
      for (size_t i = 0; i < batch.operation_diagnostics.size(); i++) {
        WARN(
            "ResourceInitializer in-flight operation: target=", batch.event_id,
            " index=", i,
            " ", batch.operation_diagnostics[i]
        );
      }
      upload_queue_event_.waitUntilSignaledValue(batch.event_id, UINT64_MAX);
      WARN(
          "ResourceInitializer in-flight batch recovered: slot=", slot,
          " commandBuffer=", batch.command_buffer.handle,
          " event=", upload_queue_event_.handle,
          " target=", batch.event_id,
          " current=", upload_queue_event_.signaledValue()
      );
    }
  }

  batch.command_buffer = nullptr;
  for (auto *allocation : batch.allocations)
    allocation->decRef();
  batch.allocations.clear();
  batch.resource_diagnostics.clear();
  batch.operation_diagnostics.clear();
  batch.event_id = 0;
}

uint64_t
ResourceInitializer::flushToWait() {
  std::lock_guard<dxmt::mutex> lock(mutex_);

  if (idle()) {
    gpu_command_heap_allocator.free_blocks(cached_coherent_seq_id);
    if (cached_coherent_seq_id == current_seq_id_ - 1)
      return 0;
    cached_coherent_seq_id = upload_queue_event_.signaledValue();
    if (cached_coherent_seq_id == current_seq_id_ - 1)
      return 0;
    return current_seq_id_ - 1;
  }

  return flushInternal();
}

void
ResourceInitializer::reset() {
  cpu_command_heap_offset = 0;

  clear_render_pass_head.next = nullptr;
  clear_render_pass_tail = &clear_render_pass_head;

  blit_cmd_head.type = WMTBlitCommandNop;
  blit_cmd_head.next.set(nullptr);
  blit_cmd_tail = (wmtcmd_base *)&blit_cmd_head;

  ref_tracker.clear();
  pending_diagnostic_resource_handles_.clear();
  pending_resource_diagnostics_.clear();
  pending_operation_diagnostics_.clear();
}

void
ResourceInitializer::recordDiagnosticResource(const Texture *texture, WMT::Texture allocation) {
  if (!perf::enabled() || !allocation)
    return;
  if (!pending_diagnostic_resource_handles_.insert(allocation.handle).second)
    return;

  pending_resource_diagnostics_.push_back(str::format(
      "kind=Texture descriptor=", texture,
      " allocation=", allocation.handle,
      " format=", uint32_t(texture->pixelFormat()),
      " type=", uint32_t(texture->textureType()),
      " usage=", uint32_t(texture->usage()),
      " width=", texture->width(),
      " height=", texture->height(),
      " depth=", texture->depth(),
      " arrayLength=", texture->arrayLength(),
      " samples=", texture->sampleCount()
  ));
}

void
ResourceInitializer::recordDiagnosticResource(const BufferAllocation *allocation) {
  if (!perf::enabled() || !allocation)
    return;
  auto buffer = allocation->buffer();
  if (!buffer || !pending_diagnostic_resource_handles_.insert(buffer.handle).second)
    return;

  pending_resource_diagnostics_.push_back(str::format(
      "kind=Buffer allocation=", allocation,
      " buffer=", buffer.handle,
      " length=", allocation->length(),
      " options=", uint64_t(allocation->resourceOptions()),
      " gpuAddress=", allocation->gpuAddress()
  ));
}

void
ResourceInitializer::encode(WMT::CommandBuffer cmdbuf) {

  auto clear_pass = clear_render_pass_head.next;
  while (clear_pass) {
    if (!ValidateInitializerRenderTargetAttachment(clear_pass->info.colors[0])) {
      clear_pass = clear_pass->next;
      continue;
    }
    if (perf::enabled()) {
      const auto &info = clear_pass->info;
      std::string operation = str::format(
          "kind=ClearRenderPass width=", info.render_target_width,
          " height=", info.render_target_height,
          " arrayLength=", uint32_t(info.render_target_array_length),
          " samples=", uint32_t(info.default_raster_sample_count)
      );
      for (uint32_t i = 0; i < 8; i++) {
        const auto &color = info.colors[i];
        if (!color.texture)
          continue;
        operation += str::format(
            " color[", i, "]={texture=", color.texture,
            ",level=", color.level,
            ",slice=", color.slice,
            ",depthPlane=", color.depth_plane,
            ",load=", uint32_t(color.load_action),
            ",store=", uint32_t(color.store_action),
            ",clear=", color.clear_color.r, ",", color.clear_color.g,
            ",", color.clear_color.b, ",", color.clear_color.a, "}"
        );
      }
      if (info.depth.texture) {
        operation += str::format(
            " depth={texture=", info.depth.texture,
            ",level=", info.depth.level,
            ",slice=", info.depth.slice,
            ",depthPlane=", info.depth.depth_plane,
            ",load=", uint32_t(info.depth.load_action),
            ",store=", uint32_t(info.depth.store_action),
            ",clear=", info.depth.clear_depth, "}"
        );
      }
      if (info.stencil.texture) {
        operation += str::format(
            " stencil={texture=", info.stencil.texture,
            ",level=", info.stencil.level,
            ",slice=", info.stencil.slice,
            ",depthPlane=", info.stencil.depth_plane,
            ",load=", uint32_t(info.stencil.load_action),
            ",store=", uint32_t(info.stencil.store_action),
            ",clear=", uint32_t(info.stencil.clear_stencil), "}"
        );
      }
      pending_operation_diagnostics_.push_back(std::move(operation));
    }
    auto r = cmdbuf.renderCommandEncoder(clear_pass->info);
    r.endEncoding();
    clear_pass = clear_pass->next;
  }

  if (blit_cmd_head.next.ptr) {
    if (perf::enabled()) {
      for (const auto *base = reinterpret_cast<const wmtcmd_base *>(blit_cmd_head.next.ptr);
           base;
           base = reinterpret_cast<const wmtcmd_base *>(base->next.ptr)) {
        switch (static_cast<WMTBlitCommandType>(base->type)) {
        case WMTBlitCommandNop:
          pending_operation_diagnostics_.push_back("kind=BlitNop");
          break;
        case WMTBlitCommandCopyFromBufferToBuffer: {
          const auto *cmd = reinterpret_cast<const wmtcmd_blit_copy_from_buffer_to_buffer *>(base);
          pending_operation_diagnostics_.push_back(str::format(
              "kind=CopyBufferToBuffer src=", cmd->src,
              " srcOffset=", cmd->src_offset,
              " dst=", cmd->dst,
              " dstOffset=", cmd->dst_offset,
              " length=", cmd->copy_length
          ));
          break;
        }
        case WMTBlitCommandCopyFromBufferToTexture: {
          const auto *cmd = reinterpret_cast<const wmtcmd_blit_copy_from_buffer_to_texture *>(base);
          pending_operation_diagnostics_.push_back(str::format(
              "kind=CopyBufferToTexture src=", cmd->src,
              " srcOffset=", cmd->src_offset,
              " bytesPerRow=", cmd->bytes_per_row,
              " bytesPerImage=", cmd->bytes_per_image,
              " size=", cmd->size.width, "x", cmd->size.height, "x", cmd->size.depth,
              " dst=", cmd->dst,
              " slice=", cmd->slice,
              " level=", cmd->level,
              " origin=", cmd->origin.x, ",", cmd->origin.y, ",", cmd->origin.z
          ));
          break;
        }
        case WMTBlitCommandCopyFromTextureToBuffer: {
          const auto *cmd = reinterpret_cast<const wmtcmd_blit_copy_from_texture_to_buffer *>(base);
          pending_operation_diagnostics_.push_back(str::format(
              "kind=CopyTextureToBuffer src=", cmd->src,
              " slice=", cmd->slice,
              " level=", cmd->level,
              " origin=", cmd->origin.x, ",", cmd->origin.y, ",", cmd->origin.z,
              " size=", cmd->size.width, "x", cmd->size.height, "x", cmd->size.depth,
              " dst=", cmd->dst,
              " dstOffset=", cmd->offset,
              " bytesPerRow=", cmd->bytes_per_row,
              " bytesPerImage=", cmd->bytes_per_image
          ));
          break;
        }
        case WMTBlitCommandCopyFromTextureToTexture: {
          const auto *cmd = reinterpret_cast<const wmtcmd_blit_copy_from_texture_to_texture *>(base);
          pending_operation_diagnostics_.push_back(str::format(
              "kind=CopyTextureToTexture src=", cmd->src,
              " srcSlice=", cmd->src_slice,
              " srcLevel=", cmd->src_level,
              " srcOrigin=", cmd->src_origin.x, ",", cmd->src_origin.y, ",", cmd->src_origin.z,
              " size=", cmd->src_size.width, "x", cmd->src_size.height, "x", cmd->src_size.depth,
              " dst=", cmd->dst,
              " dstSlice=", cmd->dst_slice,
              " dstLevel=", cmd->dst_level,
              " dstOrigin=", cmd->dst_origin.x, ",", cmd->dst_origin.y, ",", cmd->dst_origin.z
          ));
          break;
        }
        case WMTBlitCommandGenerateMipmaps: {
          const auto *cmd = reinterpret_cast<const wmtcmd_blit_generate_mipmaps *>(base);
          pending_operation_diagnostics_.push_back(str::format(
              "kind=GenerateMipmaps texture=", cmd->texture
          ));
          break;
        }
        case WMTBlitCommandWaitForFence:
        case WMTBlitCommandUpdateFence: {
          const auto *cmd = reinterpret_cast<const wmtcmd_blit_fence_op *>(base);
          pending_operation_diagnostics_.push_back(str::format(
              "kind=", base->type == WMTBlitCommandWaitForFence ? "WaitForFence" : "UpdateFence",
              " fence=", cmd->fence
          ));
          break;
        }
        case WMTBlitCommandFillBuffer: {
          const auto *cmd = reinterpret_cast<const wmtcmd_blit_fillbuffer *>(base);
          pending_operation_diagnostics_.push_back(str::format(
              "kind=FillBuffer buffer=", cmd->buffer,
              " offset=", cmd->offset,
              " length=", cmd->length,
              " value=", uint32_t(cmd->value)
          ));
          break;
        }
        case WMTBlitCommandResolveCounters: {
          const auto *cmd = reinterpret_cast<const wmtcmd_blit_resolvecounters *>(base);
          pending_operation_diagnostics_.push_back(str::format(
              "kind=ResolveCounters sampleBuffer=", cmd->sample_buffer,
              " start=", cmd->start,
              " length=", cmd->len,
              " dst=", cmd->dst_buffer,
              " dstOffset=", cmd->dst_offset
          ));
          break;
        }
        case WMTBlitCommandResourceStateBarrier:
          pending_operation_diagnostics_.push_back("kind=ResourceStateBarrier");
          break;
        default:
          pending_operation_diagnostics_.push_back(str::format(
              "kind=UnknownBlit type=", uint32_t(base->type)
          ));
          break;
        }
      }
    }
    auto b = cmdbuf.blitCommandEncoder();
    b.encodeCommands(&blit_cmd_head);
    b.endEncoding();
  }
}

WMT::Buffer
ResourceInitializer::allocateGpuHeap(size_t size, size_t &offset) {
  auto [block, offset_] = gpu_command_heap_allocator.allocate(
      current_seq_id_, cached_coherent_seq_id, size, kResourceInitializerGpuUploadHeapAlignment
  );
  offset = offset_;
  return block.buffer;
}

bool
ResourceInitializer::retainAllocation(Allocation *allocation) {
  constexpr size_t block_size = 0x20;
  while (unlikely(!ref_tracker.track(allocation))) {
    auto temp = allocateCpuHeap<intptr_t[block_size]>();
    if (!temp)
      return false;
    ref_tracker.addStorage(temp, block_size * sizeof(intptr_t));
  }
  return true;
}

WMT::Buffer
ResourceInitializer::allocateZeroBuffer(size_t size) {
  if (zero_buffer_size_ < size) {
    if (zero_buffer_size_) {
      flushInternal(); // keep a reference of old zero buffer
      zero_buffer_size_ = 0;
      return {};
    }

    wmtcmd_blit_fillbuffer *fill = nullptr;
    if (!allocateBlit(&fill)) {
      return {};
    }

    WMTBufferInfo buffer_info = {};
    buffer_info.length = size;
    buffer_info.memory.set(nullptr);
    buffer_info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
    zero_buffer_ = device_.newBuffer(buffer_info);
    zero_buffer_size_ = size;

    fill->type = WMTBlitCommandFillBuffer;
    fill->buffer = zero_buffer_;
    fill->length = size;
    fill->offset = 0;
    fill->value = 0;
  }
  return zero_buffer_;
}

} // namespace dxmt
