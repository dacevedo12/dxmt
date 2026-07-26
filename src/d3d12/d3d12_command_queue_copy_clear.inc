// Copy, resolve, tile copy, clear, and discard replay.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  void ReplayCopyResource(CommandChunk *chunk, ReplayState &state,
                          const CopyResourceRecord &record) {
    auto *dst = GetResource(record.dst.ptr());
    auto *src = GetResource(record.src.ptr());
    if (!dst || !src)
      return;

    const auto plan = PlanCopyResource(*dst, *src);
    if (plan.kind == CopyResourceKind::Buffer) {
      CopyBufferRegionRecord copy = {};
      copy.dst = record.dst;
      copy.src = record.src;
      copy.byte_count = plan.byte_count;
      ReplayCopyBufferRegion(chunk, state, copy);
      return;
    }
    if (plan.kind != CopyResourceKind::Texture)
      return;

    CopyTextureRegionRecord copy = {};
    copy.dst.resource = record.dst;
    copy.dst.type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy.src.resource = record.src;
    copy.src.type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    for (UINT i = 0; i < plan.subresource_count; i++) {
      copy.dst.subresource_index = i;
      copy.src.subresource_index = i;
      ReplayCopyTextureRegion(chunk, state, copy);
    }
  }

  // CreateResolveView() and ReplayResolveSubresource() now live in
  // d3d12_replay_resolve_encode.hpp; ReplayResolveSubresource() takes the Metal
  // device explicitly, so callers pass device_->GetMTLDevice().

  void ReplayCopyTextureRegion(CommandChunk *chunk, ReplayState &state,
                               const CopyTextureRegionRecord &record) {
    const bool perf_enabled = ReplayPerfEnabled();
    const auto lookup_begin =
        perf_enabled ? clock::now() : clock::time_point{};
    if (perf_enabled)
      perDrawSubTimers().copyTextureCalls++;
    auto *dst = GetResource(record.dst.resource.ptr());
    auto *src = GetResource(record.src.resource.ptr());
    const auto lookup_end =
        perf_enabled ? clock::now() : clock::time_point{};
    if (perf_enabled) {
      perDrawSubTimers().copyTextureLookupUs +=
          std::chrono::duration_cast<std::chrono::microseconds>(lookup_end -
                                                                lookup_begin)
              .count();
    }
    if (!dst || !src)
      return;

    const UINT src_subresource =
        record.src.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
            ? record.src.subresource_index
            : 0;
    const UINT dst_subresource =
        record.dst.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX
            ? record.dst.subresource_index
            : 0;

    const auto ensure_begin =
        perf_enabled ? clock::now() : clock::time_point{};
    if (dst->IsReservedTexture())
      dst->EnsureTextureAllocation("CopyTextureRegion dst");
    if (src->IsReservedTexture())
      src->EnsureTextureAllocation("CopyTextureRegion src");
    const auto ensure_end =
        perf_enabled ? clock::now() : clock::time_point{};
    if (perf_enabled) {
      perDrawSubTimers().copyTextureEnsureAllocationUs +=
          std::chrono::duration_cast<std::chrono::microseconds>(ensure_end -
                                                                ensure_begin)
              .count();
    }
    if (dst->GetTextureAllocation() && src->GetTextureAllocation() &&
        dst->GetTexture() && src->GetTexture()) {
      auto plan = PlanTextureRegionCopy(device_->GetMTLDevice(), *dst, *src,
                                        record, dst_subresource,
                                        src_subresource);
      if (plan.kind == TextureRegionCopyKind::Skip)
        return;
      Rc<Texture> dst_texture = std::move(plan.dst_texture);
      Rc<Texture> src_texture = std::move(plan.src_texture);
      const UINT dst_slice = plan.dst_slice;
      const UINT dst_level = plan.dst_level;
      const UINT src_slice = plan.src_slice;
      const UINT src_level = plan.src_level;
      const auto src_origin = plan.src_origin;
      const auto dst_origin = plan.dst_origin;
      const auto size = plan.size;

      if (plan.kind == TextureRegionCopyKind::BlockReinterpret) {
        const UINT row_pitch = plan.block_layout.row_pitch;
        const UINT image_pitch = plan.block_layout.image_pitch;
        const WMTSize dst_size = plan.block_layout.dst_size;

        WMTBufferInfo staging_info = {};
        staging_info.length = plan.block_layout.staging_length;
        staging_info.options = WMTResourceHazardTrackingModeUntracked;
        auto staging = device_->GetMTLDevice().newBuffer(staging_info);
        if (!staging) {
          WARN("D3D12CommandQueue: failed to allocate block reinterpret copy buffer");
          return;
        }

        QueueBlitCommand(
            chunk, state, {record.src.resource.ptr()},
            {record.dst.resource.ptr()},
            [dst_texture = std::move(dst_texture),
             src_texture = std::move(src_texture),
             staging = WMT::Reference<WMT::Buffer>(staging), dst_slice,
             dst_level, src_slice, src_level, src_origin, dst_origin, size,
             dst_size, row_pitch,
             image_pitch](ArgumentEncodingContext &enc) {
              RetainDirectBufferForGpu(enc, staging);
              auto src = enc.access(src_texture, src_level, src_slice,
                                    ResourceAccess::Read);
              auto dst = enc.access(dst_texture, dst_level, dst_slice,
                                    ResourceAccess::Write);

              auto &to_buffer = enc.encodeBlitCommand<
                  wmtcmd_blit_copy_from_texture_to_buffer>();
              to_buffer.type = WMTBlitCommandCopyFromTextureToBuffer;
              to_buffer.src = src;
              to_buffer.slice = src_slice;
              to_buffer.level = src_level;
              to_buffer.origin = src_origin;
              to_buffer.size = size;
              to_buffer.dst = staging;
              to_buffer.offset = 0;
              to_buffer.bytes_per_row = row_pitch;
              to_buffer.bytes_per_image = image_pitch;

              auto &from_buffer = enc.encodeBlitCommand<
                  wmtcmd_blit_copy_from_buffer_to_texture>();
              from_buffer.type = WMTBlitCommandCopyFromBufferToTexture;
              from_buffer.src = staging;
              from_buffer.src_offset = 0;
              from_buffer.bytes_per_row = row_pitch;
              from_buffer.bytes_per_image = image_pitch;
              from_buffer.size = dst_size;
              from_buffer.dst = dst;
              from_buffer.slice = dst_slice;
              from_buffer.level = dst_level;
              from_buffer.origin = dst_origin;
            });
        return;
      }

      const auto queue_begin =
          perf_enabled ? clock::now() : clock::time_point{};
      if (perf_enabled) {
        perDrawSubTimers().copyTexturePrepareUs +=
            std::chrono::duration_cast<std::chrono::microseconds>(
                queue_begin - ensure_end)
                .count();
      }
      QueueBlitCommand(
          chunk, state, {record.src.resource.ptr()}, {record.dst.resource.ptr()},
          [dst_texture = std::move(dst_texture),
           src_texture = std::move(src_texture), dst_slice, dst_level,
           src_slice, src_level, src_origin, dst_origin,
           size](ArgumentEncodingContext &enc) {
        auto src = enc.access(src_texture, src_level, src_slice,
                              ResourceAccess::Read);
        auto dst = enc.access(dst_texture, dst_level, dst_slice,
                              ResourceAccess::Write);
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
        copy.type = WMTBlitCommandCopyFromTextureToTexture;
        copy.src = src;
        copy.src_slice = src_slice;
        copy.src_level = src_level;
        copy.src_origin = src_origin;
        copy.src_size = size;
        copy.dst = dst;
        copy.dst_slice = dst_slice;
        copy.dst_level = dst_level;
        copy.dst_origin = dst_origin;
        if (D3D12HangDenseEnabled() || D3D12DiagTextureCopyEnabled()) {
          static std::atomic<uint32_t> log_count = 0;
          if (D3D12HangDenseEnabled() || D3D12DiagShouldLog(log_count, true)) {
            WARN("D3D12 hang-diag texture copy encode",
                 " dst_texture=", uint64_t(dst),
                 " src_texture=", uint64_t(src),
                 " dst_level=", uint32_t(dst_level),
                 " dst_slice=", uint32_t(dst_slice),
                 " src_level=", uint32_t(src_level),
                 " src_slice=", uint32_t(src_slice),
                 " dst_origin=", uint32_t(dst_origin.x), ",",
                 uint32_t(dst_origin.y), ",", uint32_t(dst_origin.z),
                 " src_origin=", uint32_t(src_origin.x), ",",
                 uint32_t(src_origin.y), ",", uint32_t(src_origin.z),
                 " size=", uint32_t(size.width), "x",
                 uint32_t(size.height), "x", uint32_t(size.depth),
                 " nullSrc=", src ? 0u : 1u,
                 " nullDst=", dst ? 0u : 1u);
          }
        }
      });
      if (perf_enabled) {
        perDrawSubTimers().copyTextureQueueUs +=
            std::chrono::duration_cast<std::chrono::microseconds>(
                clock::now() - queue_begin)
                .count();
        perDrawSubTimers().copyTextureQueued++;
      }
      return;
    }

    ReplayBufferTextureCopy(chunk, state, record, *dst, *src);
  }

  void ReplayBufferTextureCopy(CommandChunk *chunk, ReplayState &state,
                               const CopyTextureRegionRecord &record,
                               Resource &dst, Resource &src) {
    auto plan =
        PlanBufferTextureCopy(device_->GetMTLDevice(), record, dst, src);
    if (plan.kind == BufferTextureCopyKind::Skip)
      return;

    const bool dst_is_buffer = plan.dst_is_buffer;
    Rc<Buffer> buffer = std::move(plan.buffer);
    Rc<Texture> texture = std::move(plan.texture);
    const auto read_view = plan.read_view;
    const UINT64 buffer_offset = plan.buffer_offset;
    const UINT row_pitch = plan.row_pitch;
    const UINT image_pitch = plan.image_pitch;
    const auto size = plan.size;
    const auto origin = plan.origin;
    const UINT slice = plan.slice;
    const UINT level = plan.level;
    const UINT plane = plan.plane;
    const bool emulated_d24 = plan.emulated_d24;
    const DXGI_FORMAT footprint_format = plan.footprint_format;
    const DXGI_FORMAT resource_format = plan.resource_format;
    const uint32_t texture_format = plan.texture_format;
    const UINT footprint_row_count = plan.footprint_row_count;
    const UINT footprint_block_height = plan.footprint_block_height;

    if (plan.kind == BufferTextureCopyKind::DepthStencilPlane) {
      FlushPassBatches(chunk, state);
      chunk->emitcc([dst_is_buffer, buffer = std::move(buffer),
                     texture = std::move(texture), read_view, buffer_offset,
                     row_pitch, image_pitch, size, origin, slice, level,
                     plane, emulated_d24](ArgumentEncodingContext &enc) mutable {
        if (dst_is_buffer) {
          if (emulated_d24 && plane == 0) {
            enc.blit_depth_stencil_cmd.copyFromTexture(
                texture, level, slice, buffer, buffer_offset, image_pitch,
                row_pitch, image_pitch, true);
          } else {
            enc.blit_depth_stencil_cmd.copyPlaneToBuffer(
                texture, read_view, buffer, buffer_offset, image_pitch,
                row_pitch, image_pitch, plane == 1, origin, size);
          }
        } else {
          enc.blit_depth_stencil_cmd.copyPlaneFromBuffer(
              buffer, buffer_offset, image_pitch, row_pitch, image_pitch,
              texture, level, slice, plane == 1, origin, size);
        }
      });
      return;
    }

    QueueBlitCommand(
        chunk, state, {record.src.resource.ptr()}, {record.dst.resource.ptr()},
        [dst_is_buffer, buffer = std::move(buffer),
         texture = std::move(texture),
         buffer_offset, row_pitch, image_pitch, size, origin, slice,
         level, footprint_format, resource_format, texture_format,
         footprint_row_count, footprint_block_height](
                       ArgumentEncodingContext &enc) {
      if (dst_is_buffer) {
        auto src = enc.access(texture, level, slice, ResourceAccess::Read);
        auto [dst, dst_offset] =
            enc.access(buffer, buffer_offset, image_pitch, ResourceAccess::Write);
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
        copy.type = WMTBlitCommandCopyFromTextureToBuffer;
        copy.src = src;
        copy.slice = slice;
        copy.level = level;
        copy.origin = origin;
        copy.size = size;
        copy.dst = dst->buffer();
        copy.offset = dst_offset + buffer_offset;
        copy.bytes_per_row = row_pitch;
        copy.bytes_per_image = image_pitch;
        if (D3D12HangDenseEnabled() || D3D12DiagTextureCopyEnabled()) {
          static std::atomic<uint32_t> log_count = 0;
          if (D3D12HangDenseEnabled() || D3D12DiagShouldLog(log_count, true)) {
            WARN("D3D12 hang-diag buffer-texture copy encode",
                 " direction=texture_to_buffer",
                 " srcTex=", uint64_t(src),
                 " dstBuf=", uint64_t(copy.dst),
                 " access_offset=", uint64_t(dst_offset),
                 " buffer_offset=", uint64_t(buffer_offset),
                 " metal_offset=", uint64_t(copy.offset),
                 " row_pitch=", uint32_t(row_pitch),
                 " image_pitch=", uint32_t(image_pitch),
                 " row_count=", uint32_t(footprint_row_count),
                 " block_height=", uint32_t(footprint_block_height),
                 " size=", uint32_t(size.width), "x", uint32_t(size.height),
                 "x", uint32_t(size.depth),
                 " origin=", uint32_t(origin.x), ",", uint32_t(origin.y), ",",
                 uint32_t(origin.z),
                 " slice=", uint32_t(slice), " level=", uint32_t(level),
                 " format=", uint32_t(footprint_format),
                 " resource_format=", uint32_t(resource_format),
                 " texture_format=", uint32_t(texture_format),
                 " nullSrc=", src ? 0u : 1u,
                 " nullDst=", copy.dst ? 0u : 1u);
          }
        }
      } else {
        auto [src, src_offset] =
            enc.access(buffer, buffer_offset, image_pitch, ResourceAccess::Read);
        auto dst = enc.access(texture, level, slice, ResourceAccess::Write);
        auto &copy =
            enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        copy.type = WMTBlitCommandCopyFromBufferToTexture;
        copy.src = src->buffer();
        copy.src_offset = src_offset + buffer_offset;
        copy.bytes_per_row = row_pitch;
        copy.bytes_per_image = image_pitch;
        copy.size = size;
        copy.dst = dst;
        copy.slice = slice;
        copy.level = level;
        copy.origin = origin;
        if (D3D12HangDenseEnabled() || D3D12DiagTextureCopyEnabled()) {
          static std::atomic<uint32_t> log_count = 0;
          if (D3D12HangDenseEnabled() || D3D12DiagShouldLog(log_count, true)) {
            WARN("D3D12 hang-diag buffer-texture copy encode",
                 " direction=buffer_to_texture",
                 " srcBuf=", uint64_t(copy.src),
                 " dstTex=", uint64_t(dst),
                 " access_offset=", uint64_t(src_offset),
                 " buffer_offset=", uint64_t(buffer_offset),
                 " metal_offset=", uint64_t(copy.src_offset),
                 " row_pitch=", uint32_t(row_pitch),
                 " image_pitch=", uint32_t(image_pitch),
                 " row_count=", uint32_t(footprint_row_count),
                 " block_height=", uint32_t(footprint_block_height),
                 " size=", uint32_t(size.width), "x", uint32_t(size.height),
                 "x", uint32_t(size.depth),
                 " origin=", uint32_t(origin.x), ",", uint32_t(origin.y), ",",
                 uint32_t(origin.z),
                 " slice=", uint32_t(slice), " level=", uint32_t(level),
                 " format=", uint32_t(footprint_format),
                 " resource_format=", uint32_t(resource_format),
                 " texture_format=", uint32_t(texture_format),
                 " nullSrc=", copy.src ? 0u : 1u,
                 " nullDst=", dst ? 0u : 1u);
          }
        }
      }
    });
  }

  void ReplayCopyBufferTiles(CommandChunk *chunk, ReplayState &state,
                             const CopyTilesRecord &record,
                             Resource &tiled, Resource &linear,
                             bool buffer_to_tiled) {
    std::vector<BufferTileCopy> copies;
    if (!PlanBufferTileCopies(device_.ptr(), record, tiled, linear, copies))
      return;

    Rc<Buffer> linear_buffer = linear.GetBuffer();
    const UINT64 linear_heap_offset = linear.GetHeapOffset();
    for (const auto &copy : copies) {
      Rc<Buffer> src_buffer = buffer_to_tiled ? linear_buffer
                                              : copy.tiled_buffer;
      Rc<Buffer> dst_buffer = buffer_to_tiled ? copy.tiled_buffer
                                              : linear_buffer;
      const UINT64 src_offset = buffer_to_tiled
                                    ? linear_heap_offset + copy.linear_offset
                                    : copy.tiled_offset;
      const UINT64 dst_offset = buffer_to_tiled
                                    ? copy.tiled_offset
                                    : linear_heap_offset + copy.linear_offset;
      QueueBlitCommand(
          chunk, state,
          buffer_to_tiled
              ? std::initializer_list<ID3D12Resource *>{record.buffer.ptr()}
              : std::initializer_list<ID3D12Resource *>{record.tiled_resource.ptr()},
          buffer_to_tiled
              ? std::initializer_list<ID3D12Resource *>{record.tiled_resource.ptr()}
              : std::initializer_list<ID3D12Resource *>{record.buffer.ptr()},
          [src_buffer, dst_buffer, src_offset, dst_offset,
           byte_count = copy.byte_count](ArgumentEncodingContext &enc) {
            auto [src_allocation, src_sub_offset] = enc.access(
                src_buffer, src_offset, byte_count, ResourceAccess::Read);
            auto [dst_allocation, dst_sub_offset] = enc.access(
                dst_buffer, dst_offset, byte_count, ResourceAccess::Write);
            auto &blit = enc.encodeBlitCommand<
                wmtcmd_blit_copy_from_buffer_to_buffer>();
            blit.type = WMTBlitCommandCopyFromBufferToBuffer;
            blit.src = src_allocation->buffer();
            blit.src_offset = src_sub_offset + src_offset;
            blit.dst = dst_allocation->buffer();
            blit.dst_offset = dst_sub_offset + dst_offset;
            blit.copy_length = byte_count;
          });
    }
  }

  void ReplayCopyTiles(CommandChunk *chunk, ReplayState &state,
                       const CopyTilesRecord &record) {
    auto *tiled = GetResource(record.tiled_resource.ptr());
    auto *buffer_resource = GetResource(record.buffer.ptr());
    if (!tiled || !buffer_resource || !tiled->IsReserved() ||
        !buffer_resource->GetBuffer()) {
      WARN("D3D12CommandQueue: CopyTiles requires reserved resource and buffer"
           " tiled=", record.tiled_resource.ptr(),
           " buffer=", record.buffer.ptr(),
           " flags=", record.flags);
      return;
    }

    const auto direction = ResolveCopyTilesDirection(record.flags);
    if (!direction.valid)
      return;
    const bool buffer_to_texture = direction.buffer_to_tiled;

    if (tiled->GetKind() == ResourceKind::ReservedBuffer) {
      ReplayCopyBufferTiles(chunk, state, record, *tiled, *buffer_resource,
                            buffer_to_texture);
      return;
    }
    if (!tiled->IsReservedTexture())
      return;

    const auto *tiling = tiled->GetTiling();
    if (!tiling || record.start.Subresource >= tiling->subresources.size()) {
      WARN("D3D12CommandQueue: CopyTiles invalid tiled resource tiling"
           " subresource=", record.start.Subresource);
      return;
    }
    const auto &sub = tiling->subresources[record.start.Subresource];
    if (sub.plane != 0 || IsPackedTileSubresource(sub)) {
      WARN("D3D12CommandQueue: TODO CopyTiles unsupported packed/planar sparse texture region"
           " plane=", sub.plane,
           " packed=", IsPackedTileSubresource(sub),
           " subresource=", record.start.Subresource);
      return;
    }

    if (!tiled->EnsureTextureAllocation("CopyTiles"))
      return;
    Rc<Texture> texture = Rc<Texture>(tiled->GetTexture(sub.plane));
    Rc<Buffer> buffer = buffer_resource->GetBuffer();
    if (!texture || !buffer)
      return;

    MTL_DXGI_FORMAT_DESC format = {};
    if (FAILED(MTLQueryDXGIFormat(device_->GetMTLDevice(),
                                  tiled->GetResourceDesc().Format, format))) {
      WARN("D3D12CommandQueue: TODO CopyTiles unsupported format format=",
           uint32_t(tiled->GetResourceDesc().Format));
      return;
    }

    std::vector<TextureTileCopy> ops;
    if (!PlanTextureTileCopies(*tiled, *tiling, record,
                               buffer_resource->GetHeapOffset(), format, ops))
      return;
    if (ops.empty())
      return;

    QueueBlitCommand(
        chunk, state,
        buffer_to_texture ? std::initializer_list<ID3D12Resource *>{record.buffer.ptr()}
                          : std::initializer_list<ID3D12Resource *>{record.tiled_resource.ptr()},
        buffer_to_texture ? std::initializer_list<ID3D12Resource *>{record.tiled_resource.ptr()}
                          : std::initializer_list<ID3D12Resource *>{record.buffer.ptr()},
        [texture = std::move(texture), buffer = std::move(buffer),
         ops = std::move(ops), buffer_to_texture](
                      ArgumentEncodingContext &enc) mutable {
      for (const auto &op : ops) {
        if (buffer_to_texture) {
          auto [src, src_offset] =
              enc.access(buffer, op.buffer_offset,
                         D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,
                         ResourceAccess::Read);
          auto dst =
              enc.access(texture, op.level, op.slice, ResourceAccess::Write);
          auto &copy =
              enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
          copy.type = WMTBlitCommandCopyFromBufferToTexture;
          copy.src = src->buffer();
          copy.src_offset = src_offset + op.buffer_offset;
          copy.bytes_per_row = op.row_pitch;
          copy.bytes_per_image = op.image_pitch;
          copy.size = op.size;
          copy.dst = dst;
          copy.slice = op.slice;
          copy.level = op.level;
          copy.origin = op.origin;
        } else {
          auto src =
              enc.access(texture, op.level, op.slice, ResourceAccess::Read);
          auto [dst, dst_offset] =
              enc.access(buffer, op.buffer_offset,
                         D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES,
                         ResourceAccess::Write);
          auto &copy =
              enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
          copy.type = WMTBlitCommandCopyFromTextureToBuffer;
          copy.src = src;
          copy.slice = op.slice;
          copy.level = op.level;
          copy.origin = op.origin;
          copy.size = op.size;
          copy.dst = dst->buffer();
          copy.offset = dst_offset + op.buffer_offset;
          copy.bytes_per_row = op.row_pitch;
          copy.bytes_per_image = op.image_pitch;
        }
      }
    });
  }

  // ReplayClearRenderTarget(), ReplayClearDepthStencil(),
  // ReplayClearUnorderedAccess() and ReplayDiscardResource() now live in
  // d3d12_replay_clear_encode.hpp; the three clear entry points take the
  // Metal device explicitly, so callers pass device_->GetMTLDevice().
