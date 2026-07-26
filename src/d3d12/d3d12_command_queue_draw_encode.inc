// Draw/dispatch packet encoding and draw replay entry points.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  void RecordVertexBufferAccess(CommandChunk *chunk, ReplayState &state,
                                const PipelineGraphicsState *graphics_state) {
    if (!graphics_state)
      return;

    uint32_t slot_mask = 0;
    for (const auto &element : graphics_state->input_elements) {
      if (element.InputSlot < 32)
        slot_mask |= 1u << element.InputSlot;
    }
    if (!slot_mask)
      return;

    const auto max_slot = 32u - __builtin_clz(slot_mask);
    for (UINT slot = 0; slot < max_slot; slot++) {
      if (!(slot_mask & (1u << slot)))
        continue;
      const auto &vertex_buffer = state.vertex_buffers[slot];
      if (!vertex_buffer)
        continue;
      const auto &resolved = state.resolved_vertex_buffers[slot];
      if (resolved.valid && resolved.address == vertex_buffer->BufferLocation &&
          resolved.d3d_resource) {
        RecordReplayResourceAccess(
            chunk, state, resolved.d3d_resource.ptr(),
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
            "vertex buffer");
      }
    }
  }

    void RecordRenderAttachmentAccess(CommandChunk *chunk, ReplayState &state,
                                      const PipelineGraphicsState *graphics) {
      for (const auto &descriptor : state.render_targets) {
        if (auto *resource = GetResource(descriptor.resource.ptr())) {
          RecordDescriptorResourceAccessRanges(
              chunk, state, *resource, D3D12_RESOURCE_STATE_RENDER_TARGET, descriptor,
              "render target", GetRenderTargetSubresourceRanges);
        }
      }

    if (!state.depth_stencil)
      return;

    const int depth_access = AccessForDepthStencilPlane(
        *state.depth_stencil, D3D12_DSV_FLAG_READ_ONLY_DEPTH,
        PipelineWritesDepth(graphics));
    const int stencil_access = AccessForDepthStencilPlane(
        *state.depth_stencil, D3D12_DSV_FLAG_READ_ONLY_STENCIL,
        PipelineWritesStencil(graphics));
    const auto desired =
        DepthStencilResourceStateForAccess(depth_access, stencil_access);
      if (auto *resource = GetResource(state.depth_stencil->resource.ptr())) {
        RecordDescriptorResourceAccessRanges(
              chunk, state, *resource, desired, *state.depth_stencil, "depth stencil",
            GetDepthStencilSubresourceRanges);
      }
    }

  bool RecordGraphicsSnapshotDescriptorAccess(
      CommandChunk *chunk, ReplayState &state,
      const GraphicsBindingSnapshot *snapshot) {
    // Bindless snapshots are state tokens: they deliberately do not own a
    // DescriptorRecord copy. Their resources must be resolved by the live
    // binding plan (which can then consume the heap change journal safely).
    if (!snapshot || snapshot->bindless)
      return false;

    const bool cache_on = DescAccessCacheEnabled();
    const auto fingerprint = snapshot->resource_access_fingerprint;
    if (cache_on) {
      const auto cached = state.da_cache.find(fingerprint);
      const bool hit = cached != state.da_cache.end() &&
                       cached->second == state.access_epoch;
      if (hit && !DescAccessVerify()) {
        if (ReplayPerfEnabled())
          perDrawSubTimers().descAccessHits++;
        return true;
      }
      if (ReplayPerfEnabled())
        perDrawSubTimers().descAccessMiss++;
    }

    if (snapshot->native_descriptor_recipe) {
      const auto &recipe_entries = snapshot->native_descriptor_recipe->entries;
      assert(recipe_entries.size() ==
             snapshot->native_descriptor_indices.size());
      for (size_t i = 0; i < recipe_entries.size(); ++i) {
        const auto &entry = recipe_entries[i];
        if (snapshot->native_descriptor_indices[i] == UINT32_MAX ||
            entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
          continue;
        const auto range_type =
            static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(entry.range_type);
        RecordDescriptorResourceAccess(
            chunk, state, static_cast<PipelineStage>(entry.stage), range_type,
            SnapshotNativeDescriptor(*snapshot, i),
            DescriptorRangeTypeName(range_type));
      }
    }
    for (const auto &access : snapshot->native_descriptor_accesses) {
      RecordDescriptorResourceAccess(
          chunk, state, access.stage, access.range_type,
          snapshot->descriptor_records->records[access.descriptor_index],
          DescriptorRangeTypeName(access.range_type));
    }
    for (const auto &entry : snapshot->entries) {
      if (entry.kind != GraphicsBindingSnapshotEntry::Kind::Descriptor ||
          !entry.has_descriptor ||
          entry.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
        continue;
      RecordDescriptorResourceAccess(
          chunk, state, entry.stage, entry.range_type,
          SnapshotDescriptor(*snapshot, entry),
          DescriptorRangeTypeName(entry.range_type));
    }
    if (cache_on)
      state.da_cache[fingerprint] = state.access_epoch;
    return true;
  }

  void RecordGraphicsPipelineResourceAccess(
      CommandChunk *chunk, ReplayState &state, PipelineState &pipeline,
      Resource *index_resource,
      const GraphicsBindingSnapshot *binding_snapshot = nullptr) {
    StallScope _ss(StallDiagEnabled(), &stallProbe().descAccessUs);
    RecordRenderAttachmentAccess(chunk, state, pipeline.GetGraphicsState());
    if (!RecordGraphicsSnapshotDescriptorAccess(chunk, state,
                                                binding_snapshot))
      RecordPipelineDescriptorAccess(chunk, state, pipeline, false);
    RecordVertexBufferAccess(chunk, state, pipeline.GetGraphicsState());
    if (index_resource) {
      RecordReplayResourceAccess(chunk, state, index_resource->GetD3D12Resource(),
                                 D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                 "index buffer");
    }
    if (state.predication_buffer) {
      RecordReplayResourceAccess(chunk, state, state.predication_buffer.ptr(),
                                 D3D12_RESOURCE_STATE_PREDICATION,
                                 "predication buffer");
    }
  }

  void RecordComputePipelineResourceAccess(
      CommandChunk *chunk, ReplayState &state, PipelineState &pipeline,
      const GraphicsBindingSnapshot *binding_snapshot = nullptr) {
    if (!RecordGraphicsSnapshotDescriptorAccess(chunk, state,
                                                binding_snapshot))
      RecordPipelineDescriptorAccess(chunk, state, pipeline, true);
    if (state.predication_buffer) {
      RecordReplayResourceAccess(chunk, state, state.predication_buffer.ptr(),
                                 D3D12_RESOURCE_STATE_PREDICATION,
                                 "predication buffer");
    }
  }

  // EncodeDynamicRenderState / EncodeRenderPipelineStateIfChanged live in
  // d3d12_render_encoder_state.hpp.

  // ReplayDrawPacketCommon / ReplayDrawInstancedPacket /
  // ReplayDrawIndexedInstancedPacket / ReplayDrawIndirectCompiledPacket live
  // in d3d12_replay_draw_packet_types.hpp.

  // BuildCompiledDynamicRenderStateRecipe lives in
  // d3d12_dynamic_render_recipe.hpp; EncodeCompiledDynamicRenderState lives in
  // d3d12_render_encoder_state.hpp.

  void EncodeCompiledDirectGraphicsBindings(
      ArgumentEncodingContext &enc,
      const CompiledDirectGraphicsBindingPayload &payload,
      ReplayDrawPacketCommon &common, uint64_t &argbuf_offset,
      const CompiledBindingDelta *binding_delta = nullptr) {
    if (!common.pipeline)
      return;
    if (common.pipeline->GetShaderAbiVersion() !=
            DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE &&
        !common.pipeline->UsesBindlessMirror())
      return;
    BindlessMirrorDrawDiag diag = {};
    EncodeCompiledGraphicsBindings(enc, payload, *common.pipeline,
                                   argbuf_offset, binding_delta,
                                   &diag, common.test_telemetry.get());
    diag.path = "compiled-direct";
    common.bindless_diag = diag;
  }

  // FinalizeReplayDrawBindingFingerprint and CompiledAttachmentPayloadSafe
  // live in d3d12_replay_draw_packet_ops.hpp.

  void EncodeReplayDrawCommonState(ArgumentEncodingContext &enc,
                                   ReplayDrawPacketCommon &packet,
                                   uint64_t &argbuf_offset) {
    auto *perf_stats = dxmt::perf::frameStatisticsForContext(enc);
    dxmt::perf::ScopedFrameDuration common_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_common_interval);

    // Dependency planning precedes command emission, matching D3DMetal's
    // BuildEncoderSubmissionOrder -> ReplayEncoders split. A compiled binding
    // program may discover a Fragment -> PreRaster edge; split while the draw
    // is still command-free, then publish the access list into the reopened
    // pass so its caches and fence waits belong to the encoder that draws.
    std::optional<CompiledBindingDelta> compiled_delta;
    if (packet.compiled_binding_payload) {
      auto resolve_delta = [&]() {
        return ResolveRenderEncoderBindingDelta(
            packet.binding_identity, packet.binding_delta,
            packet.descriptor_content_revision,
            enc.currentRenderEncoder()->binding_state_cache);
      };
      compiled_delta = resolve_delta();
      if (CompiledDirectAccessListRequiresReverseBoundary(
              enc, packet.compiled_direct_access)) {
        enc.splitRenderPassForReverseStage();
        compiled_delta = resolve_delta();
      }
      /*
       * Binding deltas only control Metal binding commands. Resource accesses
       * are ordered semantics of every D3D command and must be published even
       * when an encoder can reuse the previous immutable binding program.
       */
      dxmt::perf::ScopedFrameDuration retain_scope(
          perf_stats,
          &dxmt::FrameStatistics::frame_compiled_draw_retain_interval);
      RetainFrozenNativeStoreForGpu(
          enc, packet.compiled_binding_payload->frozen_native);
      PublishCompiledDirectAccessListForEncode(
          enc, packet.compiled_direct_access,
          packet.test_telemetry.get());

      /*
       * Resolve every dependency before recording encoder-local state. If a
       * future access-plan regression leaves a reverse-stage bit behind, the
       * split happens before PSO/dynamic/binding commands so the reopened
       * Metal encoder cannot inherit a fictitious cached state.
       */
      enc.resolveRenderPassBarrier();
      compiled_delta = resolve_delta();
    }

    {
      dxmt::perf::ScopedFrameDuration pipeline_scope(
          perf_stats,
          &dxmt::FrameStatistics::frame_compiled_draw_pipeline_interval);
      EncodeRenderPipelineStateIfChanged(enc, packet.metal_pso);
    }
    auto *render_encoder = enc.currentRenderEncoder();
    render_encoder->pixel_shader_demote_msaa_srv_mask_lo =
        packet.pixel_shader_demote_msaa_srv_mask_lo;
    render_encoder->pixel_shader_demote_msaa_srv_mask_hi =
        packet.pixel_shader_demote_msaa_srv_mask_hi;
    auto &cache = render_encoder->dynamic_state_cache;
    const auto stencil_ref_u8 = static_cast<uint8_t>(packet.stencil_ref);
    if (packet.depth_stencil &&
        (!cache.depth_stencil_valid ||
         cache.depth_stencil.handle != packet.depth_stencil.handle ||
         cache.stencil_ref != stencil_ref_u8)) {
      dxmt::perf::ScopedFrameDuration dsso_scope(
          perf_stats,
          &dxmt::FrameStatistics::frame_compiled_draw_dsso_interval);
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
      cmd.type = WMTRenderCommandSetDSSO;
      cmd.dsso = packet.depth_stencil;
      cmd.stencil_ref = stencil_ref_u8;
      cache.depth_stencil = packet.depth_stencil;
      cache.stencil_ref = stencil_ref_u8;
      cache.depth_stencil_valid = true;
    }
    if (!cache.rasterizer_valid ||
        std::memcmp(&cache.rasterizer, &packet.rasterizer,
                    sizeof(packet.rasterizer)) != 0) {
      dxmt::perf::ScopedFrameDuration rasterizer_scope(
          perf_stats,
          &dxmt::FrameStatistics::frame_compiled_draw_rasterizer_interval);
      auto &rs = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
      rs = packet.rasterizer;
      cache.rasterizer = packet.rasterizer;
      cache.rasterizer_valid = true;
    }

    auto &binding_cache = render_encoder->binding_state_cache;
    if (packet.compiled_binding_payload) {
      assert(compiled_delta);
      const auto &resolved_delta = *compiled_delta;
      if (!resolved_delta.dirty_fields) {
        if (packet.test_telemetry)
          packet.test_telemetry->encoder_binding_program_hits.fetch_add(
              1, std::memory_order_relaxed);
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_draw_binding_generation_hits);
      } else {
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_draw_binding_misses);
        {
          dxmt::perf::ScopedFrameDuration binding_scope(
              perf_stats,
              &dxmt::FrameStatistics::frame_compiled_draw_binding_snapshot_interval);
          if (packet.test_telemetry) {
            auto &counter =
                resolved_delta.full_bind
                    ? packet.test_telemetry->encoder_full_binding_programs
                    : packet.test_telemetry->encoder_delta_binding_programs;
            counter.fetch_add(1, std::memory_order_relaxed);
          }
          EncodeCompiledDirectGraphicsBindings(
              enc, *packet.compiled_binding_payload,
              packet, argbuf_offset, &resolved_delta);
        }
        binding_cache.binding_program = packet.binding_identity.program;
        binding_cache.resource_heap = packet.binding_identity.resource_heap;
        binding_cache.sampler_heap = packet.binding_identity.sampler_heap;
        binding_cache.root_tables = packet.binding_identity.root_tables;
        binding_cache.root_constants = packet.binding_identity.root_constants;
        binding_cache.root_descriptors =
            packet.binding_identity.root_descriptors;
        binding_cache.vertex_bindings =
            packet.binding_identity.vertex_bindings;
        binding_cache.descriptor_content_revision =
            packet.descriptor_content_revision;
        binding_cache.valid = true;
      }
      {
        dxmt::perf::ScopedFrameDuration dynamic_state_scope(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_draw_dynamic_state_interval);
        if (packet.dynamic_state_recipe)
          EncodeCompiledDynamicRenderState(enc,
                                           *packet.dynamic_state_recipe);
        else
          EncodeDynamicRenderState(enc, packet.viewports, packet.scissors,
                                   packet.blend_factor, packet.stencil_ref);
      }
      return;
    }

    // Interpreted binding application mutates the same Metal encoder state but
    // has no Close-time program predecessor. Never allow a later compiled
    // packet to consume a delta across this compatibility boundary.
    if (binding_cache.binding_program) {
      binding_cache.binding_program = nullptr;
      binding_cache.valid = false;
    }

    bool generation_hit = false;
    bool fingerprint_hit = false;
    bool bindless_snapshot = false;
    BindlessMirrorDrawDiag bindless_diag = {};
    bool skip_binding_snapshot = false;
    {
      dxmt::perf::ScopedFrameDuration binding_gate_scope(
          perf_stats,
          &dxmt::FrameStatistics::frame_compiled_draw_binding_gate_interval);
      generation_hit =
          binding_cache.valid &&
          binding_cache.graphics_generation == packet.binding_generation &&
          binding_cache.descriptor_content_revision ==
              packet.descriptor_content_revision &&
          binding_cache.content_fingerprint ==
              packet.binding_content_fingerprint;
      fingerprint_hit =
          binding_cache.valid && !generation_hit &&
          binding_cache.content_fingerprint ==
              packet.binding_content_fingerprint;
      bindless_snapshot =
          packet.pipeline && packet.pipeline->UsesBindlessMirror() &&
          packet.binding_snapshot && packet.binding_snapshot->bindless &&
          !packet.use_geometry &&
          !packet.use_tessellation;
      if (D3D12DiagBindingRecipeCacheEnabled()) {
        auto &stats = BindingRecipeDiagStats();
        if (generation_hit)
          stats.gate_generation_hits.fetch_add(1, std::memory_order_relaxed);
        else if (fingerprint_hit)
          stats.gate_fingerprint_hits.fetch_add(1, std::memory_order_relaxed);
        else
          stats.gate_misses.fetch_add(1, std::memory_order_relaxed);
      }
      if (generation_hit)
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_draw_binding_generation_hits);
      else if (fingerprint_hit)
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_draw_binding_fingerprint_hits);
      else
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_draw_binding_misses);
      bindless_diag.uses_bindless_mirror =
          packet.pipeline && packet.pipeline->UsesBindlessMirror();
	    bindless_diag.bindless_bound =
	        packet.binding_snapshot && packet.binding_snapshot->bindless &&
	        !packet.use_geometry && !packet.use_tessellation;
      bindless_diag.path =
          BindlessMirrorDiagPathName(bindless_diag.bindless_bound,
                                     true);
      skip_binding_snapshot =
          !bindless_snapshot && (generation_hit || fingerprint_hit);
      if (skip_binding_snapshot)
        dxmt::perf::addFrameCounter(
            perf_stats,
            &dxmt::FrameStatistics::frame_compiled_draw_binding_snapshot_skipped);
    }
    if (!skip_binding_snapshot && packet.pipeline &&
               packet.binding_snapshot) {
	      ApplyGraphicsBindingSnapshot(enc, *packet.binding_snapshot,
	                                   *packet.pipeline, packet.use_geometry,
	                                   packet.use_tessellation, argbuf_offset,
	                                   &bindless_diag);
    }
    packet.bindless_diag = bindless_diag;
    if (!generation_hit) {
      binding_cache.graphics_generation = packet.binding_generation;
      binding_cache.descriptor_content_revision =
          packet.descriptor_content_revision;
      binding_cache.content_fingerprint = packet.binding_content_fingerprint;
      binding_cache.valid = true;
    }
    {
      dxmt::perf::ScopedFrameDuration dynamic_state_scope(
          perf_stats,
          &dxmt::FrameStatistics::frame_compiled_draw_dynamic_state_interval);
      if (packet.dynamic_state_recipe)
        EncodeCompiledDynamicRenderState(enc, *packet.dynamic_state_recipe);
      else
        EncodeDynamicRenderState(enc, packet.viewports, packet.scissors,
                                 packet.blend_factor, packet.stencil_ref);
    }
  }

  // Begin/EndReplayDrawVisibilityQuery live in
  // d3d12_draw_visibility_scope.hpp.

  // The draw bodies of the three EncodeReplayDraw*Packet members below —
  // everything after the shared encoder state has been applied — now live in
  // d3d12_replay_draw_body_encode.hpp; unqualified calls resolve to
  // dxmt::d3d12. They only reached the queue instance for the frame sequence
  // the bindless-mirror diagnostic logs, so they take the dxmt::CommandQueue
  // and read it at the same point the member did. EncodeReplayDrawCommonState
  // itself stays here: it still calls EncodeCompiledDirectGraphicsBindings and
  // ApplyGraphicsBindingSnapshot, both CommandQueueImpl members.

  void EncodeReplayDrawInstancedPacket(ArgumentEncodingContext &enc,
                                       ReplayDrawInstancedPacket &packet,
                                       uint64_t &argbuf_offset) {
    auto *perf_stats = dxmt::perf::frameStatisticsForContext(enc);
    auto &common = packet.common;
    const bool compiled_direct = common.compiled_binding_payload.has_value();
    dxmt::perf::ScopedFrameDuration encode_scope(
        perf_stats,
        compiled_direct
            ? &dxmt::FrameStatistics::frame_compiled_draw_direct_encode_interval
            : &dxmt::FrameStatistics::frame_compiled_draw_replay_encode_interval);
    dxmt::perf::addFrameCounter(
        perf_stats,
        compiled_direct
            ? &dxmt::FrameStatistics::frame_compiled_draw_direct_packets
            : &dxmt::FrameStatistics::frame_compiled_draw_replay_packets);
    dxmt::perf::addFrameCounter(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_nonindexed_packets);

    EncodeReplayDrawCommonState(enc, common, argbuf_offset);
    if (enc.argumentBufferOverflowed())
      return;

    EncodeReplayDrawInstancedBody(enc, device_->GetDXMTDevice().queue(), packet,
                                  argbuf_offset, perf_stats);
  }

  static void EncodeReplayDrawInstancedCompiled(
      CommandQueueImpl &queue, ArgumentEncodingContext &enc,
      ReplayDrawInstancedPacket &payload,
      uint64_t &argbuf_offset) {
    queue.EncodeReplayDrawInstancedPacket(enc, payload, argbuf_offset);
  }

  void EncodeReplayDrawIndirectCompiledPacket(
      ArgumentEncodingContext &enc,
      ReplayDrawIndirectCompiledPacket &packet,
      uint64_t &argbuf_offset) {
    auto *perf_stats = dxmt::perf::frameStatisticsForContext(enc);
    dxmt::perf::ScopedFrameDuration encode_scope(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_direct_encode_interval);
    dxmt::perf::addFrameCounter(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_direct_packets);
    dxmt::perf::addFrameCounter(
        perf_stats,
        packet.indexed
            ? &dxmt::FrameStatistics::frame_compiled_draw_indexed_packets
            : &dxmt::FrameStatistics::frame_compiled_draw_nonindexed_packets);

    EncodeReplayDrawCommonState(enc, packet.common, argbuf_offset);
    if (enc.argumentBufferOverflowed() || !packet.common.primitive)
      return;

    EncodeReplayDrawIndirectCompiledBody(enc, packet);
  }

  static void EncodeReplayDrawIndirectCompiled(
      CommandQueueImpl &queue, ArgumentEncodingContext &enc,
      ReplayDrawIndirectCompiledPacket &payload,
      uint64_t &argbuf_offset) {
    queue.EncodeReplayDrawIndirectCompiledPacket(enc, payload,
                                                 argbuf_offset);
  }

  void EncodeReplayDrawIndexedInstancedPacket(
      ArgumentEncodingContext &enc, ReplayDrawIndexedInstancedPacket &packet,
      uint64_t &argbuf_offset) {
    auto *perf_stats = dxmt::perf::frameStatisticsForContext(enc);
    auto &common = packet.common;
    enc.retainResource(packet.index_allocation->buffer());
    const bool compiled_direct = common.compiled_binding_payload.has_value();
    dxmt::perf::ScopedFrameDuration encode_scope(
        perf_stats,
        compiled_direct
            ? &dxmt::FrameStatistics::frame_compiled_draw_direct_encode_interval
            : &dxmt::FrameStatistics::frame_compiled_draw_replay_encode_interval);
    dxmt::perf::addFrameCounter(
        perf_stats,
        compiled_direct
            ? &dxmt::FrameStatistics::frame_compiled_draw_direct_packets
            : &dxmt::FrameStatistics::frame_compiled_draw_replay_packets);
    dxmt::perf::addFrameCounter(
        perf_stats,
        &dxmt::FrameStatistics::frame_compiled_draw_indexed_packets);

    if (!compiled_direct) {
      dxmt::perf::ScopedFrameDuration retain_scope(
          perf_stats,
          &dxmt::FrameStatistics::frame_compiled_draw_retain_interval);
      enc.retainAllocation(packet.index_allocation.ptr());
    }

    EncodeReplayDrawCommonState(enc, common, argbuf_offset);
    if (enc.argumentBufferOverflowed())
      return;

    EncodeReplayDrawIndexedInstancedBody(enc, device_->GetDXMTDevice().queue(),
                                         packet, argbuf_offset, perf_stats);
  }

  static void EncodeReplayDrawIndexedInstancedCompiled(
      CommandQueueImpl &queue, ArgumentEncodingContext &enc,
      ReplayDrawIndexedInstancedPacket &payload,
      uint64_t &argbuf_offset) {
    queue.EncodeReplayDrawIndexedInstancedPacket(enc, payload,
                                                 argbuf_offset);
  }

  void ReplayDrawInstanced(CommandChunk *chunk, ReplayState &state,
                           const DrawInstancedRecord &record) {
    if (!record.vertex_count_per_instance || !record.instance_count)
      return;
    if (!PredicationAllows(chunk, state))
      return;

    auto *pipeline = [&] {
      StallScope _s(StallDiagEnabled(), &stallProbe().getPipelineUs);
      return GetPipelineState(state.pipeline_state.ptr());
    }();
    if (!pipeline) {
      WARN("D3D12CommandQueue: draw skipped without graphics pipeline state");
      return;
    }

    const auto [demote_msaa_srv_mask_lo, demote_msaa_srv_mask_hi] =
        PixelShaderSingleSampleMsaaSRVDemoteMask(state, *pipeline);
    auto *metal = [&] {
      StallScope _ss(StallDiagEnabled(), &stallProbe().psoSelectUs);
      return pipeline->GetMetalGraphicsState(demote_msaa_srv_mask_lo,
                                             demote_msaa_srv_mask_hi);
    }();
    if (!metal || !metal->pso) {
      if (ReplayPerfEnabled())
        perDrawSubTimers().graphicsPsoUnavailable++;
      WARN("D3D12CommandQueue: draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    const auto metal_pso = [&] {
      StallScope _s(StallDiagEnabled(), &stallProbe().selectPsoUs);
      return SelectGraphicsPipelineState(*metal, state.topology);
    }();
    if (!metal_pso) {
      WARN("D3D12CommandQueue: draw skipped because selected Metal graphics PSO is unavailable");
      return;
    }
    std::optional<WMTPrimitiveType> primitive;
    std::optional<std::pair<uint32_t, uint32_t>> geometry_counts;
    std::optional<uint32_t> control_point_count;
    if (metal->use_tessellation) {
      control_point_count = GetPatchControlPointCount(state.topology);
      if (!control_point_count) {
        // TODO(d3d12): support non-patch topologies with tessellation PSOs if needed.
        WARN("D3D12CommandQueue: tessellation draw skipped because primitive topology is not a patch list topology=",
             uint32_t(state.topology));
        return;
      }
    } else if (metal->use_geometry) {
      geometry_counts = GetGeometryVertexCount(state.topology);
      if (!geometry_counts) {
        WARN("D3D12CommandQueue: geometry draw skipped because primitive topology is unsupported topology=",
             uint32_t(state.topology));
        return;
      }
    } else {
      primitive = GetPrimitiveType(state.topology);
      if (!primitive) {
        WARN("D3D12CommandQueue: draw skipped because primitive topology is unsupported topology=",
             uint32_t(state.topology));
        return;
      }
    }
    RecordReplayBindingSignaturePerf(state);
    const bool rb_draw = ReplayPerfEnabled();
    const auto rb_attach0 = rb_draw ? clock::now() : clock::time_point{};
    auto attachments = BuildRenderPassAttachments(state,
                                                  pipeline->GetGraphicsState());
    if (rb_draw)
      perDrawSubTimers().attach +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - rb_attach0).count();
    auto viewports = state.viewports;
    auto scissors = state.scissors;
    if (!ResolveDynamicRasterRects(viewports, scissors, "draw"))
      return;
    const auto descriptor_content_revision =
        GetDescriptorContentRevision();
    std::shared_ptr<GraphicsBindingSnapshot> binding_snapshot =
        FindCachedGraphicsBindingSnapshot(state, *pipeline,
                                          descriptor_content_revision);
    const auto rb_desc0 = rb_draw ? clock::now() : clock::time_point{};
    RecordGraphicsPipelineResourceAccess(
        chunk, state, *pipeline, nullptr, binding_snapshot.get());
    const bool bindless_replay_path =
        pipeline->UsesBindlessMirror() && !metal->use_geometry &&
        !metal->use_tessellation;
    const bool compiled_attachment_payload_safe =
        CompiledAttachmentPayloadSafe(attachments);
    {
      StallScope _s(StallDiagEnabled(), &stallProbe().bindSnapUs);
      binding_snapshot = GetOrCaptureGraphicsBindingSnapshot(
          chunk, state, *pipeline, attachments,
          descriptor_content_revision, std::move(binding_snapshot));
    }
    if (rb_draw)
      perDrawSubTimers().desc +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - rb_desc0).count();
    DebugLogDrawState("draw", state, *pipeline, *metal, attachments,
                      viewports, scissors, &record, nullptr, 0, 0);
    DebugEncodeIAReadbacks(chunk, "draw", state, *pipeline, &record, nullptr,
                           0);
    DebugEncodeCBVReadbacks(chunk, "draw", state, *pipeline,
                            state.current_record_d3d_sequence);
    DrawVisibilityRetirementWork visibility_retirement = {};
    auto visibility_query = D3D12DiagCreateDrawVisibilityQuery(
        "draw", pipeline->GetShaderCacheKey(),
        DiagCurrentReplayRecordSequence(),
        record.vertex_count_per_instance, 0, record.instance_count,
        &visibility_retirement);
    if (visibility_query)
      EnqueueRetirement(chunk, std::move(visibility_retirement));
    const auto argument_buffer_size = [&] {
      StallScope _s(StallDiagEnabled(), &stallProbe().estimateUs);
      return EstimateGraphicsArgumentBufferSize(*pipeline, metal->use_geometry,
                                                metal->use_tessellation) +
             ((metal->use_geometry || metal->use_tessellation)
                  ? EstimateDrawArgumentBufferSize(false)
                  : 0);
    }();
    const auto _pkt_t0 = StallDiagEnabled() ? clock::now() : clock::time_point{};
    ReplayDrawInstancedPacket packet = {};
    packet.common.metal_pso = metal_pso;
    packet.common.depth_stencil = metal->depth_stencil;
    packet.common.rasterizer = metal->rasterizer;
    packet.common.pipeline = pipeline;
    packet.common.binding_snapshot = std::move(binding_snapshot);
    packet.common.binding_generation = state.graphics_binding_generation;
    packet.common.descriptor_content_revision =
        descriptor_content_revision;
    packet.common.pixel_shader_demote_msaa_srv_mask_lo =
        demote_msaa_srv_mask_lo;
    packet.common.pixel_shader_demote_msaa_srv_mask_hi =
        demote_msaa_srv_mask_hi;
    packet.common.primitive = primitive;
    packet.common.geometry_counts = geometry_counts;
    packet.common.control_point_count = control_point_count;
    packet.common.blend_factor = state.blend_factor;
    packet.common.stencil_ref = state.stencil_ref;
    packet.common.visibility_query = std::move(visibility_query);
    packet.common.viewports = std::move(viewports);
    packet.common.scissors = std::move(scissors);
    packet.common.max_object_threadgroups =
        device_->GetDXMTDevice().maxObjectThreadgroups();
    packet.common.tess_threads_per_patch = metal->tess_threads_per_patch;
    packet.common.tess_num_output_control_point_element =
        metal->tess_num_output_control_point_element;
    packet.common.use_geometry = metal->use_geometry;
    packet.common.use_tessellation = metal->use_tessellation;
    FinalizeReplayDrawBindingFingerprint(packet.common);
    packet.vertex_start = record.start_vertex_location;
    packet.vertex_count = record.vertex_count_per_instance;
    packet.instance_count = record.instance_count;
    packet.base_instance = record.start_instance_location;

    if (StallDiagEnabled())
      stallProbe().packetUs +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - _pkt_t0).count();

    {
      StallScope _q(StallDiagEnabled(), &stallProbe().queueUs);
      if (D3D12ReplayGraphicsBatchingEnabled()) {
        if (bindless_replay_path && compiled_attachment_payload_safe) {
          QueueCompiledGraphicsPassCommand(
              chunk, state, std::move(attachments), argument_buffer_size,
              std::move(packet), EncodeReplayDrawInstancedCompiled,
              metal->use_geometry, metal->use_tessellation,
              ReplayGraphicsCommandKind::Draw);
        } else {
          auto encode_draw = [this, packet = std::move(packet)](
                                 ArgumentEncodingContext &enc,
                                 uint64_t &argbuf_offset) mutable {
            EncodeReplayDrawInstancedPacket(enc, packet, argbuf_offset);
          };
          QueueGraphicsPassCommand(chunk, state, std::move(attachments),
                                   argument_buffer_size, std::move(encode_draw),
                                   metal->use_geometry, metal->use_tessellation,
                                   ReplayGraphicsCommandKind::Draw, false);
        }
        return;
      }

      auto encode_draw = [this, packet = std::move(packet)](
                             ArgumentEncodingContext &enc,
                             uint64_t &argbuf_offset) mutable {
        EncodeReplayDrawInstancedPacket(enc, packet, argbuf_offset);
      };
      FlushPassBatches(chunk, state);
      EmitSingleGraphicsPass(chunk, std::move(attachments), argument_buffer_size,
                             DiagCurrentReplayRecordSequence(),
                             std::move(encode_draw), metal->use_geometry,
                             metal->use_tessellation);
    }
  }

  void ReplayDrawIndexedInstanced(CommandChunk *chunk, ReplayState &state,
                                  const DrawIndexedInstancedRecord &record) {
    if (!record.index_count_per_instance || !record.instance_count ||
        !state.index_buffer)
      return;
    if (!PredicationAllows(chunk, state))
      return;

    auto *pipeline = [&] {
      StallScope _s(StallDiagEnabled(), &stallProbe().getPipelineUs);
      return GetPipelineState(state.pipeline_state.ptr());
    }();
    if (!pipeline) {
      WARN("D3D12CommandQueue: indexed draw skipped without graphics pipeline state");
      return;
    }

    const auto [demote_msaa_srv_mask_lo, demote_msaa_srv_mask_hi] =
        PixelShaderSingleSampleMsaaSRVDemoteMask(state, *pipeline);
    auto *metal = [&] {
      StallScope _ss(StallDiagEnabled(), &stallProbe().psoSelectUs);
      return pipeline->GetMetalGraphicsState(demote_msaa_srv_mask_lo,
                                             demote_msaa_srv_mask_hi);
    }();
    if (!metal || !metal->pso) {
      if (ReplayPerfEnabled())
        perDrawSubTimers().graphicsPsoUnavailable++;
      WARN("D3D12CommandQueue: indexed draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    const auto &resolved_index = state.resolved_index_buffer;
    auto *index_resource =
        resolved_index.valid &&
                resolved_index.address == state.index_buffer->BufferLocation
            ? resolved_index.resource
            : nullptr;
    if (!index_resource || !resolved_index.allocation) {
      static std::atomic_flag warned_missing_index_buffer = ATOMIC_FLAG_INIT;
      if (!warned_missing_index_buffer.test_and_set(std::memory_order_relaxed))
        WARN("D3D12CommandQueue: indexed draw skipped because index buffer "
             "binding is unavailable (further warnings suppressed)");
      return;
    }
    if (!IsSupportedIndexBufferFormat(state.index_buffer->Format)) {
      WARN("D3D12CommandQueue: indexed draw skipped because index buffer format is unsupported format=",
           uint32_t(state.index_buffer->Format));
      return;
    }

    Rc<BufferAllocation> index_allocation = resolved_index.allocation;
    const auto metal_pso = [&] {
      StallScope _s(StallDiagEnabled(), &stallProbe().selectPsoUs);
      return SelectGraphicsPipelineState(*metal, state.topology,
                                         state.index_buffer->Format);
    }();
    if (!metal_pso) {
      WARN("D3D12CommandQueue: indexed draw skipped because selected Metal graphics PSO is unavailable");
      return;
    }
    std::optional<WMTPrimitiveType> primitive;
    std::optional<std::pair<uint32_t, uint32_t>> geometry_counts;
    std::optional<uint32_t> control_point_count;
    if (metal->use_tessellation) {
      control_point_count = GetPatchControlPointCount(state.topology);
      if (!control_point_count) {
        // TODO(d3d12): support non-patch topologies with tessellation PSOs if needed.
        WARN("D3D12CommandQueue: tessellation indexed draw skipped because primitive topology is not a patch list topology=",
             uint32_t(state.topology));
        return;
      }
    } else if (metal->use_geometry) {
      geometry_counts = GetGeometryVertexCount(state.topology);
      if (!geometry_counts) {
        WARN("D3D12CommandQueue: geometry indexed draw skipped because primitive topology is unsupported topology=",
             uint32_t(state.topology));
        return;
      }
    } else {
      primitive = GetPrimitiveType(state.topology);
      if (!primitive) {
        WARN("D3D12CommandQueue: indexed draw skipped because primitive topology is unsupported topology=",
             uint32_t(state.topology));
        return;
      }
    }
    const auto index_type = GetIndexType(state.index_buffer->Format);
    const UINT64 index_binding_offset = resolved_index.binding_offset;
    const UINT64 index_resource_offset =
        index_binding_offset - index_resource->GetHeapOffset();
    const UINT64 index_offset =
        index_binding_offset +
        record.start_index_location * GetIndexSize(state.index_buffer->Format);
    RecordReplayBindingSignaturePerf(state);
    const bool rb_draw = ReplayPerfEnabled();
    const auto rb_da0 = rb_draw ? clock::now() : clock::time_point{};
    auto attachments = BuildRenderPassAttachments(state,
                                                  pipeline->GetGraphicsState());
    if (rb_draw)
      perDrawSubTimers().attach +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - rb_da0).count();
    auto viewports = state.viewports;
    auto scissors = state.scissors;
    if (!ResolveDynamicRasterRects(viewports, scissors, "indexed draw"))
      return;
    const auto descriptor_content_revision =
        GetDescriptorContentRevision();
    std::shared_ptr<GraphicsBindingSnapshot> binding_snapshot =
        FindCachedGraphicsBindingSnapshot(state, *pipeline,
                                          descriptor_content_revision);
    const auto rb_ra0 = rb_draw ? clock::now() : clock::time_point{};
    RecordGraphicsPipelineResourceAccess(
        chunk, state, *pipeline, index_resource, binding_snapshot.get());
    const bool bindless_replay_path =
        pipeline->UsesBindlessMirror() && !metal->use_geometry &&
        !metal->use_tessellation;
    const bool compiled_attachment_payload_safe =
        CompiledAttachmentPayloadSafe(attachments);
    {
      StallScope _s(StallDiagEnabled(), &stallProbe().bindSnapUs);
      binding_snapshot = GetOrCaptureGraphicsBindingSnapshot(
          chunk, state, *pipeline, attachments,
          descriptor_content_revision, std::move(binding_snapshot));
    }
    if (rb_draw)
      perDrawSubTimers().desc +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - rb_ra0).count();
    DebugLogDrawState("indexed", state, *pipeline, *metal, attachments,
                      viewports, scissors, nullptr, &record,
                      index_resource_offset, index_offset);
    DebugEncodeIAReadbacks(chunk, "indexed", state, *pipeline, nullptr,
                           &record, index_offset);
    DebugEncodeCBVReadbacks(chunk, "indexed", state, *pipeline,
                            state.current_record_d3d_sequence);
    DrawVisibilityRetirementWork visibility_retirement = {};
    auto visibility_query = D3D12DiagCreateDrawVisibilityQuery(
        "indexed", pipeline->GetShaderCacheKey(),
        DiagCurrentReplayRecordSequence(), 0,
        record.index_count_per_instance, record.instance_count,
        &visibility_retirement);
    if (visibility_query)
      EnqueueRetirement(chunk, std::move(visibility_retirement));
    const auto argument_buffer_size = [&] {
      StallScope _s(StallDiagEnabled(), &stallProbe().estimateUs);
      return EstimateGraphicsArgumentBufferSize(*pipeline, metal->use_geometry,
                                                metal->use_tessellation) +
             ((metal->use_geometry || metal->use_tessellation)
                  ? EstimateDrawArgumentBufferSize(true)
                  : 0);
    }();
    const auto _pkt_t0 = StallDiagEnabled() ? clock::now() : clock::time_point{};
    ReplayDrawIndexedInstancedPacket packet = {};
    packet.common.metal_pso = metal_pso;
    packet.common.depth_stencil = metal->depth_stencil;
    packet.common.rasterizer = metal->rasterizer;
    packet.common.pipeline = pipeline;
    packet.common.binding_snapshot = std::move(binding_snapshot);
    packet.common.binding_generation = state.graphics_binding_generation;
    packet.common.descriptor_content_revision =
        descriptor_content_revision;
    packet.common.pixel_shader_demote_msaa_srv_mask_lo =
        demote_msaa_srv_mask_lo;
    packet.common.pixel_shader_demote_msaa_srv_mask_hi =
        demote_msaa_srv_mask_hi;
    packet.common.primitive = primitive;
    packet.common.geometry_counts = geometry_counts;
    packet.common.control_point_count = control_point_count;
    packet.common.blend_factor = state.blend_factor;
    packet.common.stencil_ref = state.stencil_ref;
    packet.common.visibility_query = std::move(visibility_query);
    packet.common.viewports = std::move(viewports);
    packet.common.scissors = std::move(scissors);
    packet.common.max_object_threadgroups =
        device_->GetDXMTDevice().maxObjectThreadgroups();
    packet.common.tess_threads_per_patch = metal->tess_threads_per_patch;
    packet.common.tess_num_output_control_point_element =
        metal->tess_num_output_control_point_element;
    packet.common.use_geometry = metal->use_geometry;
    packet.common.use_tessellation = metal->use_tessellation;
    packet.index_allocation = index_allocation;
    packet.index_type = index_type;
    packet.index_binding_offset = index_binding_offset;
    packet.index_offset = index_offset;
    packet.index_count = record.index_count_per_instance;
    packet.start_index = record.start_index_location;
    packet.instance_count = record.instance_count;
    packet.base_vertex = record.base_vertex_location;
    packet.base_instance = record.start_instance_location;
    FinalizeReplayDrawBindingFingerprint(packet);

    if (StallDiagEnabled())
      stallProbe().packetUs +=
          std::chrono::duration_cast<std::chrono::microseconds>(
              clock::now() - _pkt_t0).count();

    {
      StallScope _q(StallDiagEnabled(), &stallProbe().queueUs);
      if (D3D12ReplayGraphicsBatchingEnabled()) {
        if (bindless_replay_path && compiled_attachment_payload_safe) {
          QueueCompiledGraphicsPassCommand(
              chunk, state, std::move(attachments), argument_buffer_size,
              std::move(packet), EncodeReplayDrawIndexedInstancedCompiled,
              metal->use_geometry, metal->use_tessellation,
              ReplayGraphicsCommandKind::DrawIndexed);
        } else {
          auto encode_draw = [this, packet = std::move(packet)](
                                 ArgumentEncodingContext &enc,
                                 uint64_t &argbuf_offset) mutable {
            EncodeReplayDrawIndexedInstancedPacket(enc, packet, argbuf_offset);
          };
          QueueGraphicsPassCommand(
              chunk, state, std::move(attachments), argument_buffer_size,
              std::move(encode_draw), metal->use_geometry,
              metal->use_tessellation, ReplayGraphicsCommandKind::DrawIndexed,
              false);
        }
        return;
      }

      auto encode_draw = [this, packet = std::move(packet)](
                             ArgumentEncodingContext &enc,
                             uint64_t &argbuf_offset) mutable {
        EncodeReplayDrawIndexedInstancedPacket(enc, packet, argbuf_offset);
      };
      FlushPassBatches(chunk, state);
      EmitSingleGraphicsPass(chunk, std::move(attachments), argument_buffer_size,
                             DiagCurrentReplayRecordSequence(),
                             std::move(encode_draw), metal->use_geometry,
                             metal->use_tessellation);
    }
  }

  void ReplayDispatch(CommandChunk *chunk, ReplayState &state,
                      const DispatchRecord &record) {
    if (!record.x || !record.y || !record.z)
      return;
    if (!PredicationAllows(chunk, state))
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: dispatch skipped without compute pipeline state");
      return;
    }

    auto *metal = [&]{ StallScope _ss(StallDiagEnabled(), &stallProbe().psoSelectUs); return pipeline->GetMetalComputeState(); }();
    if (!metal || !metal->pso) {
      if (ReplayPerfEnabled()) {
        auto &timers = perDrawSubTimers();
        timers.computePsoUnavailable++;
        if (timers.firstComputePsoUnavailableKey.empty())
          timers.firstComputePsoUnavailableKey = pipeline->GetShaderCacheKey();
      }
      WARN("D3D12CommandQueue: dispatch skipped because Metal compute PSO is unavailable");
      return;
    }

    if (!ValidateComputeDispatch(metal->threadgroup_size, record.x, record.y,
                                 record.z))
      return;

    RecordComputePipelineResourceAccess(chunk, state, *pipeline);
    RecordBindlessMirrorDiagDispatch();
    const auto argument_buffer_size =
        EstimateComputeArgumentBufferSize(*pipeline);
    ReplayDispatchPacket dispatch_packet = {};
    dispatch_packet.metal_pso = metal->pso;
    dispatch_packet.threadgroup_size = metal->threadgroup_size;
    dispatch_packet.pipeline = pipeline;
    dispatch_packet.dispatch = record;
    dispatch_packet.argument_buffer_size = argument_buffer_size;
    dispatch_packet.replay_state.emplace(CloneReplayStateWithoutBatch(state));
    QueueComputePassCommand(
        chunk, state, argument_buffer_size,
        [this, dispatch_packet = std::move(dispatch_packet)](
            ArgumentEncodingContext &enc, uint64_t &argbuf_offset) mutable {
          EncodeReplayDispatchPacket(enc, dispatch_packet, argbuf_offset);
        });
  }

