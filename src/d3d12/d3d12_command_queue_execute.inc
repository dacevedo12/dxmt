// Compiled command list replay execution.
// Included inside class CommandQueueImpl; not a standalone translation unit.
//
// ExecuteCompiledCommandList is decomposed into single-responsibility private
// members. CompiledReplayContext carries the replay cursor (current chunk,
// record index, deferred state, perf accumulators) by reference so every
// helper observes and advances exactly the state the inline code did.

  // NativeBindingRecipeKey, NativeBindingRecipeKeyHash,
  // NativeBindingRecipeCache, ReplayBreakdownAccumulators,
  // CompiledReplayContext, CompiledGraphicsPacketPlan and
  // CompiledComputePacketPlan now live in
  // d3d12_replay_compiled_execute_types.hpp; unqualified uses below resolve to
  // dxmt::d3d12. They are plain data aggregates over already-hoisted replay
  // types and never named CommandQueueImpl; the last one blocked was
  // ReplayBreakdownAccumulators, which embeds a StallProbe
  // (d3d12_replay_stall_probe.hpp).

  // Replays a single compatibility CommandRecord and folds its cost into the
  // per-record diagnostics and the replay breakdown accumulators.
  void ReplayOneCompiledRecord(CompiledReplayContext &ctx,
                               const CommandRecord &record) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    auto &record_index = ctx.record_index;
    auto &replay_record_serial = ctx.replay_record_serial;
    const bool replay_perf = ctx.replay_perf;
    const bool replay_breakdown = ctx.replay_breakdown;
    auto &rb_by_type = ctx.rb.by_type;
    auto &rb_stall_accum = ctx.rb.stall_accum;
    auto &rb_stall_total_us = ctx.rb.stall_total_us;
    auto &rb_typed_record_interval = ctx.rb.typed_record_interval;
    const auto typed_record_begin =
        replay_perf ? clock::now() : clock::time_point{};
    DiagReplayRecordScope diag_record_scope(record.d3d_sequence,
                                            ++replay_record_serial);
    ReplayStateRecordScope state_record_scope(state, record.d3d_sequence);
    if (record.d3d_sequence != 0) {
      dxmt::apitrace::set_current_d3d_sequence(record.d3d_sequence);
    }
    const bool record_perf = replay_perf || replay_breakdown;
    const auto rb_rec0 = record_perf ? clock::now() : clock::time_point{};
    const bool stall_timing = StallDiagEnabled();
    const bool stall_log = D3D12RecordLoopStallEnabled();
    const auto stall_t0 = stall_timing ? clock::now() : clock::time_point{};
    if (stall_timing)
      stallProbe().reset();
    std::visit([&](const auto &payload) {
      if (ShouldLogCompiledReplayDiagnostic()) {
        WARN_FILE_ONLY("D3D12 queue diagnostic: replay record enter"
             " queue=", reinterpret_cast<uintptr_t>(this),
             " queueType=", desc_.Type,
             " index=", record_index,
             " sequence=", record.d3d_sequence,
             " kind=", typeid(payload).name());
      }
      ReplayRecord(chunk, state, payload);
      const auto rb_rec_us =
          record_perf ? std::chrono::duration_cast<std::chrono::microseconds>(
                            clock::now() - rb_rec0).count()
                      : 0;
      if (replay_perf)
        RecordReplayRecordPerfBucket<std::decay_t<decltype(payload)>>(
            uint64_t(rb_rec_us));
      if (replay_breakdown) {
        auto &slot = rb_by_type[typeid(payload).name()];
        slot.first++;
        slot.second += uint64_t(rb_rec_us);
      }
      if (stall_timing) {
        const auto rec_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                clock::now() - stall_t0).count();
        const auto &p = stallProbe();
        // Aggregate EVERY record (not only slow ones) for the breakdown line.
        rb_stall_accum.getPipelineUs += p.getPipelineUs;
        rb_stall_accum.psoSelectUs += p.psoSelectUs;
        rb_stall_accum.selectPsoUs += p.selectPsoUs;
        rb_stall_accum.descAccessUs += p.descAccessUs;
        rb_stall_accum.attachUs += p.attachUs;
        rb_stall_accum.bindSnapUs += p.bindSnapUs;
        rb_stall_accum.estimateUs += p.estimateUs;
        rb_stall_accum.packetUs += p.packetUs;
        rb_stall_accum.queueUs += p.queueUs;
        rb_stall_accum.emitUs += p.emitUs;
        rb_stall_total_us += rec_us;
        // Only slow records are interesting; threshold env-overridable.
        static const uint64_t thresh = []() {
          auto v = env::getEnvVar("DXMT_DIAG_STALL_US");
          uint64_t n = v.empty() ? 500 : strtoull(v.c_str(), nullptr, 10);
          return n ? n : 500;
        }();
        if (stall_log && uint64_t(rec_us) >= thresh) {
          static std::atomic<uint32_t> stall_log_count = 0;
          if (stall_log_count.fetch_add(1, std::memory_order_relaxed) < 4000)
            WARN_FILE_ONLY("DXMT stall record:"
                 " kind=", typeid(payload).name(),
                 " seq=", record.d3d_sequence,
                 " totalUs=", rec_us,
                 " getPipelineUs=", p.getPipelineUs,
                 " psoSelectUs=", p.psoSelectUs,
                 " selectPsoUs=", p.selectPsoUs,
                 " descAccessUs=", p.descAccessUs,
                 " attachUs=", p.attachUs,
                 " bindSnapUs=", p.bindSnapUs,
                 " estimateUs=", p.estimateUs,
                 " packetUs=", p.packetUs,
                 " queueUs=", p.queueUs,
                 " emitUs=", p.emitUs);
        }
      }
      if (ShouldLogCompiledReplayDiagnostic()) {
        WARN_FILE_ONLY("D3D12 queue diagnostic: replay record leave"
             " queue=", reinterpret_cast<uintptr_t>(this),
             " queueType=", desc_.Type,
             " index=", record_index,
             " sequence=", record.d3d_sequence,
             " kind=", typeid(payload).name());
      }
    }, record.payload);
    chunk = queue.CurrentChunk();
    record_index++;
    if (replay_perf)
      rb_typed_record_interval += clock::now() - typed_record_begin;
  }

  // Replays [begin, begin+count) of the compatibility record stream under one
  // fallback reason, skipping superseded state records and batching long
  // graphics runs exactly as the inline loop did.
  void ReplayCompiledRecordRange(CompiledReplayContext &ctx, UINT begin,
                                 UINT count,
                                 dxmt::CompiledFallbackReason fallback_reason) {
    auto &state = ctx.state;
    const auto &records = ctx.records;
    const auto &superseded_state_record_mask = ctx.superseded_state_record_mask;
    auto &record_index = ctx.record_index;
    const bool replay_perf = ctx.replay_perf;
    const auto previous_fallback_reason = state.compiled_fallback_reason;
    state.compiled_fallback_reason = fallback_reason;
    const UINT end = std::min<UINT>(begin + count,
                                    static_cast<UINT>(records.size()));
    for (UINT i = begin; i < end;) {
      if (superseded_state_record_mask[i]) {
        if (replay_perf)
          perDrawSubTimers().supersededStateRecordsSkipped++;
        record_index++;
        i++;
        continue;
      }
      if (IntraPassParallelEnabled() && IsGraphicsPassRunRecord(records[i])) {
        UINT j = i;
        while (j < end && IsGraphicsPassRunRecord(records[j]))
          ++j;
        if (j - i >= 8) {
          for (UINT k = i; k < j; ++k) {
            if (superseded_state_record_mask[k]) {
              if (replay_perf)
                perDrawSubTimers().supersededStateRecordsSkipped++;
              record_index++;
              continue;
            }
            ReplayOneCompiledRecord(ctx, records[k]);
          }
          i = j;
          continue;
        }
      }
      ReplayOneCompiledRecord(ctx, records[i]);
      ++i;
    }
    state.compiled_fallback_reason = previous_fallback_reason;
  }

  // ResolveCompiledGraphicsMetalPipeline(), DiagnoseCompiledGraphicsPacket(),
  // BuildCompiledGraphicsPacketBindingPlan() and
  // MaterializeActiveEncoderAttachments() now live in
  // d3d12_compiled_graphics_emit_plan.hpp; unqualified calls below resolve to
  // dxmt::d3d12. They only ever needed two things from the queue instance,
  // which the call sites now pass in: the Metal device (attachment textures and
  // encoder accesses) and native_stage_plan_cache_ (the dense descriptor
  // diagnostic's memoized root-base plans).
  //
  // The classification prologue of QueueCompiledGraphicsPacket() and
  // QueueCompiledComputePacket() lives in
  // d3d12_compiled_packet_admission.hpp as AdmitCompiledGraphicsPacket() /
  // AdmitCompiledComputePacket(). A false result means "return the reason":
  // None for a packet that is a legitimate no-op, anything else for one that
  // has to go through compatibility replay.

  // Expands a count-buffer indirect node into a compacted argument stream.
  // Returns false when the count buffer cannot be validated or allocated.
  [[nodiscard]] bool ExpandCountedIndirectArguments(
      CompiledReplayContext &ctx, const ExecuteIndirectRecord *indirect,
      const D3D12_COMMAND_SIGNATURE_DESC *indirect_desc, UINT argument_size,
      const char *count_buffer_label, Rc<Buffer> &argument_buffer,
      UINT64 &argument_base_offset, UINT &argument_stride) {
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    auto *count_resource =
        GetResource(indirect->count_buffer.ptr());
    if (!ValidateBufferRange(
            count_resource, indirect->count_buffer_offset,
            sizeof(UINT), count_buffer_label))
      return false;
    auto count_buffer = count_resource->GetBuffer();
    const UINT64 count_offset =
        count_resource->GetHeapOffset() +
        indirect->count_buffer_offset;
    Rc<Buffer> counted_arguments = new Buffer(
        UINT64(indirect->max_command_count) * argument_size,
        device_->GetDXMTDevice().device());
    auto counted_allocation = counted_arguments->allocate(
        BufferAllocationFlag::CpuInvisible |
        BufferAllocationFlag::GpuPrivate);
    if (!counted_allocation)
      return false;
    counted_arguments->rename(std::move(counted_allocation));

    // Count expansion is the dependency node of the compiled
    // work. It runs once for the entire command range; each subsequent
    // command consumes its compact slot.
    FlushPassBatches(chunk, state);
    const UINT64 source_length =
        UINT64(indirect->max_command_count - 1) *
            indirect_desc->ByteStride +
        argument_size;
    const UINT64 destination_length =
        UINT64(indirect->max_command_count) * argument_size;
    chunk->emitcc(
        [argument_buffer, argument_base_offset, source_length,
         count_buffer, count_offset, counted_arguments,
         destination_length,
         command_count = indirect->max_command_count,
         source_stride = indirect_desc->ByteStride,
         argument_size](ArgumentEncodingContext &enc) mutable {
          enc.startComputePass(0);
          auto [source_allocation, source_sub_offset] =
              enc.access<PipelineStage::Compute>(
                  argument_buffer, argument_base_offset,
                  source_length, ResourceAccess::Read);
          auto [count_allocation, count_sub_offset] =
              enc.access<PipelineStage::Compute>(
                  count_buffer, count_offset, sizeof(UINT),
                  ResourceAccess::Read);
          auto [destination_allocation,
                destination_sub_offset] =
              enc.access<PipelineStage::Compute>(
                  counted_arguments, 0, destination_length,
                  ResourceAccess::Write);
          for (UINT command_index = 0;
               command_index < command_count; ++command_index) {
            enc.emulated_cmd.PrepareCountedIndirectArguments(
                count_allocation->buffer(),
                count_sub_offset + count_offset,
                source_allocation->buffer(),
                source_sub_offset + argument_base_offset +
                    UINT64(command_index) * source_stride,
                destination_allocation->buffer(),
                destination_sub_offset +
                    UINT64(command_index) * argument_size,
                argument_size, command_index);
          }
          enc.endPass();
        });
    argument_buffer = counted_arguments;
    argument_base_offset = 0;
    argument_stride = argument_size;
    return true;
  }

  // Emits the per-command indirect draw stream of one compiled graphics packet.
  [[nodiscard]] CompiledCommandFallbackReason
  QueueCompiledGraphicsIndirectCommands(
      CompiledReplayContext &ctx, const CompiledGraphicsPacket &packet,
      const SubmittedCompiledGraphicsPacket &prepared,
      const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
      CompiledGraphicsPacketPlan &plan, const ExecuteIndirectRecord *indirect,
      const D3D12_COMMAND_SIGNATURE_DESC *indirect_desc,
      const D3D12_INDIRECT_ARGUMENT_DESC *indirect_argument) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    auto *pipeline = plan.pipeline;
    const auto *metal = plan.metal;
    const auto &metal_pso = plan.metal_pso;
    const auto &primitive = plan.primitive;
    const bool indexed = plan.indexed;
    const auto &frozen_native = plan.frozen_native;
    const auto compiled_demote_msaa_srv_mask_lo =
        plan.compiled_demote_msaa_srv_mask_lo;
    const auto compiled_demote_msaa_srv_mask_hi =
        plan.compiled_demote_msaa_srv_mask_hi;
    const auto *encoder_attachments = plan.encoder_attachments;
    const auto &binding_state = plan.binding_state;
    const auto argument_buffer_size = plan.argument_buffer_size;
    const auto &native_binding_recipe = plan.native_binding_recipe;
    const auto binding_fingerprint = plan.binding_fingerprint;
    const auto submitted_descriptor_revision =
        plan.submitted_descriptor_revision;
    const auto &vertex_binding_recipe = packet.vertex_binding_recipe;
    auto &direct_access = plan.direct_access;
    const auto &bindless_snapshot = plan.bindless_snapshot;
    const UINT argument_size =
        IndirectArgumentByteSize(*indirect_argument);
    if (!argument_size || !indirect_desc->ByteStride ||
        indirect_desc->ByteStride < argument_size)
      return CompiledCommandFallbackReason::
          NativeUnsupportedExecuteIndirect;
    if (!indirect->max_command_count)
      return CompiledCommandFallbackReason::None;
    auto *argument_resource = GetResource(indirect->arg_buffer.ptr());
    const UINT64 argument_length =
        UINT64(indirect->max_command_count - 1) *
            indirect_desc->ByteStride +
        argument_size;
    if (!ValidateBufferRange(argument_resource,
                             indirect->arg_buffer_offset,
                             argument_length,
                             "compiled indirect argument buffer"))
      return CompiledCommandFallbackReason::
          NativeUnsupportedExecuteIndirect;
    Rc<Buffer> argument_buffer = argument_resource->GetBuffer();
    UINT64 argument_base_offset =
        argument_resource->GetHeapOffset() +
        indirect->arg_buffer_offset;
    UINT argument_stride = indirect_desc->ByteStride;
    if (indirect->count_buffer) {
      if (!ExpandCountedIndirectArguments(
              ctx, indirect, indirect_desc, argument_size,
              "compiled indirect count buffer", argument_buffer,
              argument_base_offset, argument_stride))
        return CompiledCommandFallbackReason::
            NativeUnsupportedExecuteIndirect;
    }

    Rc<BufferAllocation> index_allocation;
    WMTIndexType index_type = WMTIndexTypeUInt16;
    UINT64 index_buffer_offset = 0;
    Resource *index_resource = nullptr;
    if (indexed) {
      if (!packet.input_assembler.index_buffer ||
          !IsSupportedIndexBufferFormat(
              packet.input_assembler.index_buffer->Format))
        return CompiledCommandFallbackReason::
            UnsupportedVertexIndexState;
      index_buffer_offset = ResolveBufferGpuAddress(
          packet.input_assembler.index_buffer->BufferLocation,
          index_resource);
      if (!index_resource || !index_resource->GetBuffer())
        return CompiledCommandFallbackReason::
            NativeUnsupportedDynamicResource;
      index_allocation = index_resource->GetBufferAllocation();
      if (!index_allocation)
        return CompiledCommandFallbackReason::
            NativeUnsupportedDynamicResource;
      index_type =
          GetIndexType(packet.input_assembler.index_buffer->Format);
      bool found_index = false;
      for (const auto &allocation :
           direct_access.static_buffer_allocations)
        found_index |= allocation.ptr() == index_allocation.ptr();
      for (const auto &allocation : direct_access.buffer_allocations)
        found_index |= allocation.ptr() == index_allocation.ptr();
      if (!found_index)
        direct_access.buffer_allocations.push_back(index_allocation);
    }
    RecordGraphicsPipelineResourceAccess(
        chunk, state, *pipeline, index_resource,
        submitted_snapshot.get());

    for (UINT command_index = 0;
         command_index < indirect->max_command_count;
         ++command_index) {
      ReplayDrawIndirectCompiledPacket replay_packet = {};
      auto &common = replay_packet.common;
      common.metal_pso = metal_pso;
      common.depth_stencil = metal->depth_stencil;
      common.rasterizer = metal->rasterizer;
      common.pipeline = pipeline;
      common.binding_program = packet.binding_program;
      common.binding_delta = packet.binding_delta;
      common.binding_identity =
          BuildCompiledEncoderBindingIdentity(packet);
      if (frozen_native) {
        common.binding_identity.resource_heap = frozen_native.get();
        common.binding_identity.sampler_heap = frozen_native.get();
        common.binding_identity.root_tables = nullptr;
      }
      common.test_telemetry = compiled->test_telemetry;
      common.binding_generation = binding_fingerprint;
      common.binding_content_fingerprint = binding_fingerprint;
      common.descriptor_content_revision =
          submitted_descriptor_revision;
      common.pixel_shader_demote_msaa_srv_mask_lo =
          compiled_demote_msaa_srv_mask_lo;
      common.pixel_shader_demote_msaa_srv_mask_hi =
          compiled_demote_msaa_srv_mask_hi;
      common.primitive = primitive;
      common.blend_factor = packet.render_state.blend_factor;
      common.stencil_ref = packet.render_state.stencil_ref;
      common.dynamic_state_recipe =
          packet.dynamic_render_state_recipe;
      common.viewports = packet.render_state.viewports;
      common.scissors = packet.render_state.scissors;
      common.compiled_binding_payload.emplace();
      auto &binding_payload = *common.compiled_binding_payload;
      binding_payload.pipeline_state = packet.pipeline.pipeline_state;
      binding_payload.root_signature = packet.pipeline.root_signature;
      binding_payload.root_tables = prepared.root_tables;
      binding_payload.input_assembler = packet.input_assembler;
      binding_payload.bindless_snapshot = bindless_snapshot;
      binding_payload.frozen_native = frozen_native;
      binding_payload.native_binding_recipe = native_binding_recipe;
      binding_payload.vertex_binding_recipe = vertex_binding_recipe;
      common.compiled_binding_state = binding_state;
      common.compiled_direct_access = direct_access;
      replay_packet.argument_buffer = argument_buffer;
      replay_packet.argument_offset =
          argument_base_offset +
          UINT64(command_index) * argument_stride;
      replay_packet.argument_size = argument_size;
      replay_packet.index_allocation = index_allocation;
      replay_packet.index_type = index_type;
      replay_packet.index_buffer_offset = index_buffer_offset;
      replay_packet.indexed = indexed;
      QueueCompiledGraphicsPassCommand(
          chunk, state, *encoder_attachments, argument_buffer_size,
          std::move(replay_packet),
          EncodeReplayDrawIndirectCompiled, false, false,
          indexed ? ReplayGraphicsCommandKind::DrawIndexedIndirect
                  : ReplayGraphicsCommandKind::DrawIndirect);
    }
    chunk = queue.CurrentChunk();
    return CompiledCommandFallbackReason::None;
  }

  // Emits a compiled DrawInstanced packet into the graphics pass batch.
  // `draw` is passed as a reference instead of being read back out of
  // packet.draw so the "this packet really is a non-indexed draw" invariant
  // is carried by the signature rather than by the caller's branch.
  void QueueCompiledDrawInstancedCommand(
      CompiledReplayContext &ctx, const CompiledGraphicsPacket &packet,
      const DrawInstancedRecord &draw,
      const SubmittedCompiledGraphicsPacket &prepared,
      const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
      CompiledGraphicsPacketPlan &plan) {
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    auto *pipeline = plan.pipeline;
    const auto *metal = plan.metal;
    const auto &metal_pso = plan.metal_pso;
    const auto &primitive = plan.primitive;
    const auto &frozen_native = plan.frozen_native;
    const auto compiled_demote_msaa_srv_mask_lo =
        plan.compiled_demote_msaa_srv_mask_lo;
    const auto compiled_demote_msaa_srv_mask_hi =
        plan.compiled_demote_msaa_srv_mask_hi;
    const auto *encoder_attachments = plan.encoder_attachments;
    const auto &binding_state = plan.binding_state;
    const auto argument_buffer_size = plan.argument_buffer_size;
    const auto &native_binding_recipe = plan.native_binding_recipe;
    const auto binding_fingerprint = plan.binding_fingerprint;
    const auto submitted_descriptor_revision =
        plan.submitted_descriptor_revision;
    const auto &vertex_binding_recipe = packet.vertex_binding_recipe;
    const auto &direct_access = plan.direct_access;
    const auto &bindless_snapshot = plan.bindless_snapshot;
    RecordGraphicsPipelineResourceAccess(
        chunk, state, *pipeline, nullptr, submitted_snapshot.get());
    ReplayDrawInstancedPacket replay_packet = {};
    replay_packet.common.metal_pso = metal_pso;
    replay_packet.common.depth_stencil = metal->depth_stencil;
    replay_packet.common.rasterizer = metal->rasterizer;
    replay_packet.common.pipeline = pipeline;
    replay_packet.common.binding_program = packet.binding_program;
    replay_packet.common.binding_delta = packet.binding_delta;
    replay_packet.common.binding_identity =
        BuildCompiledEncoderBindingIdentity(packet);
    if (frozen_native) {
      replay_packet.common.binding_identity.resource_heap =
          frozen_native.get();
      replay_packet.common.binding_identity.sampler_heap =
          frozen_native.get();
      replay_packet.common.binding_identity.root_tables = nullptr;
    }
    replay_packet.common.test_telemetry = compiled->test_telemetry;
    replay_packet.common.binding_generation = binding_fingerprint;
    replay_packet.common.binding_content_fingerprint =
        binding_fingerprint;
    replay_packet.common.descriptor_content_revision =
        submitted_descriptor_revision;
    replay_packet.common.pixel_shader_demote_msaa_srv_mask_lo =
        compiled_demote_msaa_srv_mask_lo;
    replay_packet.common.pixel_shader_demote_msaa_srv_mask_hi =
        compiled_demote_msaa_srv_mask_hi;
    replay_packet.common.primitive = primitive;
    replay_packet.common.blend_factor =
        packet.render_state.blend_factor;
    replay_packet.common.stencil_ref = packet.render_state.stencil_ref;
    replay_packet.common.dynamic_state_recipe =
        packet.dynamic_render_state_recipe;
    replay_packet.common.viewports = packet.render_state.viewports;
    replay_packet.common.scissors = packet.render_state.scissors;
    replay_packet.common.use_geometry = false;
    replay_packet.common.use_tessellation = false;
    replay_packet.vertex_start = draw.start_vertex_location;
    replay_packet.vertex_count = draw.vertex_count_per_instance;
    replay_packet.instance_count = draw.instance_count;
    replay_packet.base_instance = draw.start_instance_location;
    replay_packet.common.compiled_binding_payload.emplace();
    auto &binding_payload =
        *replay_packet.common.compiled_binding_payload;
    binding_payload.pipeline_state = packet.pipeline.pipeline_state;
    binding_payload.root_signature = packet.pipeline.root_signature;
    binding_payload.root_tables = prepared.root_tables;
    binding_payload.input_assembler = packet.input_assembler;
    binding_payload.bindless_snapshot = bindless_snapshot;
    binding_payload.frozen_native = frozen_native;
    binding_payload.native_binding_recipe = native_binding_recipe;
    binding_payload.vertex_binding_recipe = vertex_binding_recipe;
    replay_packet.common.compiled_binding_state = binding_state;
    replay_packet.common.compiled_direct_access = direct_access;
    QueueCompiledGraphicsPassCommand(
        chunk, state, *encoder_attachments, argument_buffer_size,
        std::move(replay_packet), EncodeReplayDrawInstancedCompiled,
        false, false, ReplayGraphicsCommandKind::Draw);
  }

  // Emits a compiled DrawIndexedInstanced packet, resolving its index buffer.
  // `draw_indexed` is passed as a reference instead of being read back out of
  // packet.draw_indexed so the "this packet really is an indexed draw"
  // invariant is carried by the signature rather than by the caller's branch.
  [[nodiscard]] CompiledCommandFallbackReason
  QueueCompiledDrawIndexedInstancedCommand(
      CompiledReplayContext &ctx, const CompiledGraphicsPacket &packet,
      const DrawIndexedInstancedRecord &draw_indexed,
      const SubmittedCompiledGraphicsPacket &prepared,
      const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
      CompiledGraphicsPacketPlan &plan) {
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    const bool native_packet = plan.native_packet;
    auto *pipeline = plan.pipeline;
    const auto *metal = plan.metal;
    const auto &metal_pso = plan.metal_pso;
    const auto &primitive = plan.primitive;
    const auto &frozen_native = plan.frozen_native;
    const auto compiled_demote_msaa_srv_mask_lo =
        plan.compiled_demote_msaa_srv_mask_lo;
    const auto compiled_demote_msaa_srv_mask_hi =
        plan.compiled_demote_msaa_srv_mask_hi;
    const auto *encoder_attachments = plan.encoder_attachments;
    const auto &binding_state = plan.binding_state;
    const auto argument_buffer_size = plan.argument_buffer_size;
    const auto &native_binding_recipe = plan.native_binding_recipe;
    const auto binding_fingerprint = plan.binding_fingerprint;
    const auto submitted_descriptor_revision =
        plan.submitted_descriptor_revision;
    const auto &vertex_binding_recipe = packet.vertex_binding_recipe;
    const auto &direct_access = plan.direct_access;
    auto &bindless_snapshot = plan.bindless_snapshot;
    if (!packet.input_assembler.index_buffer ||
        !IsSupportedIndexBufferFormat(
            packet.input_assembler.index_buffer->Format))
      return CompiledCommandFallbackReason::UnsupportedVertexIndexState;
    Resource *index_resource = nullptr;
    const auto index_binding_offset = ResolveBufferGpuAddress(
        packet.input_assembler.index_buffer->BufferLocation,
        index_resource);
    if (!index_resource || !index_resource->GetBuffer())
      return native_packet
                 ? CompiledCommandFallbackReason::NativeUnsupportedDynamicResource
                 : CompiledCommandFallbackReason::UnsupportedVertexIndexState;
    Rc<BufferAllocation> index_allocation =
        index_resource->GetBufferAllocation();
    if (!index_allocation)
      return native_packet
                 ? CompiledCommandFallbackReason::NativeUnsupportedDynamicResource
                 : CompiledCommandFallbackReason::UnsupportedVertexIndexState;
    RecordGraphicsPipelineResourceAccess(
        chunk, state, *pipeline, index_resource,
        submitted_snapshot.get());
    ReplayDrawIndexedInstancedPacket replay_packet = {};
    replay_packet.common.metal_pso = metal_pso;
    replay_packet.common.depth_stencil = metal->depth_stencil;
    replay_packet.common.rasterizer = metal->rasterizer;
    replay_packet.common.pipeline = pipeline;
    replay_packet.common.binding_program = packet.binding_program;
    replay_packet.common.binding_delta = packet.binding_delta;
    replay_packet.common.binding_identity =
        BuildCompiledEncoderBindingIdentity(packet);
    if (frozen_native) {
      replay_packet.common.binding_identity.resource_heap =
          frozen_native.get();
      replay_packet.common.binding_identity.sampler_heap =
          frozen_native.get();
      replay_packet.common.binding_identity.root_tables = nullptr;
    }
    replay_packet.common.test_telemetry = compiled->test_telemetry;
    replay_packet.common.binding_generation = binding_fingerprint;
    replay_packet.common.binding_content_fingerprint =
        binding_fingerprint;
    replay_packet.common.descriptor_content_revision =
        submitted_descriptor_revision;
    replay_packet.common.pixel_shader_demote_msaa_srv_mask_lo =
        compiled_demote_msaa_srv_mask_lo;
    replay_packet.common.pixel_shader_demote_msaa_srv_mask_hi =
        compiled_demote_msaa_srv_mask_hi;
    replay_packet.common.primitive = primitive;
    replay_packet.common.blend_factor =
        packet.render_state.blend_factor;
    replay_packet.common.stencil_ref = packet.render_state.stencil_ref;
    replay_packet.common.dynamic_state_recipe =
        packet.dynamic_render_state_recipe;
    replay_packet.common.viewports = packet.render_state.viewports;
    replay_packet.common.scissors = packet.render_state.scissors;
    replay_packet.common.use_geometry = false;
    replay_packet.common.use_tessellation = false;
    replay_packet.index_allocation = index_allocation;
    replay_packet.index_type =
        GetIndexType(packet.input_assembler.index_buffer->Format);
    replay_packet.index_binding_offset = index_binding_offset;
    replay_packet.index_offset =
        index_binding_offset +
        draw_indexed.start_index_location *
            GetIndexSize(packet.input_assembler.index_buffer->Format);
    replay_packet.index_count = draw_indexed.index_count_per_instance;
    replay_packet.start_index = draw_indexed.start_index_location;
    replay_packet.instance_count = draw_indexed.instance_count;
    replay_packet.base_vertex = draw_indexed.base_vertex_location;
    replay_packet.base_instance = draw_indexed.start_instance_location;
    replay_packet.common.compiled_binding_payload.emplace();
    auto &binding_payload =
        *replay_packet.common.compiled_binding_payload;
    binding_payload.pipeline_state = packet.pipeline.pipeline_state;
    binding_payload.root_signature = packet.pipeline.root_signature;
    binding_payload.root_tables = prepared.root_tables;
    binding_payload.input_assembler = packet.input_assembler;
    binding_payload.bindless_snapshot = std::move(bindless_snapshot);
    binding_payload.frozen_native = frozen_native;
    binding_payload.native_binding_recipe = native_binding_recipe;
    binding_payload.vertex_binding_recipe = vertex_binding_recipe;
    replay_packet.common.compiled_binding_state = binding_state;
    replay_packet.common.compiled_direct_access = direct_access;
    if (index_allocation) {
      bool found_index = false;
      for (const auto &allocation :
           replay_packet.common.compiled_direct_access
               .static_buffer_allocations) {
        if (allocation.ptr() == index_allocation.ptr()) {
          found_index = true;
          break;
        }
      }
      for (const auto &allocation :
           replay_packet.common.compiled_direct_access
               .buffer_allocations) {
        if (allocation.ptr() == index_allocation.ptr()) {
          found_index = true;
          break;
        }
      }
      if (!found_index)
        replay_packet.common.compiled_direct_access.buffer_allocations
            .push_back(index_allocation);
    }
    QueueCompiledGraphicsPassCommand(
        chunk, state, *encoder_attachments, argument_buffer_size,
        std::move(replay_packet),
        EncodeReplayDrawIndexedInstancedCompiled,
        false, false, ReplayGraphicsCommandKind::DrawIndexed);
    return CompiledCommandFallbackReason::None;
  }

  // Emits one Close-time compiled graphics packet, or reports why submission
  // has to fall back to the compatibility replay path.
  [[nodiscard]] CompiledCommandFallbackReason QueueCompiledGraphicsPacket(
      CompiledReplayContext &ctx, const CompiledGraphicsPacket &packet,
      const SubmittedCompiledGraphicsPacket &prepared,
      std::shared_ptr<GraphicsBindingSnapshot> submitted_snapshot,
      const ReplayRenderPassAttachments *encoder_attachments,
      const ExecuteIndirectRecord *indirect = nullptr) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &replay_record_serial = ctx.replay_record_serial;
    DirectIndirectOperation indirect_operation =
        DirectIndirectOperation::None;
    const D3D12_COMMAND_SIGNATURE_DESC *indirect_desc = nullptr;
    const D3D12_INDIRECT_ARGUMENT_DESC *indirect_argument = nullptr;
    auto admission_reason = CompiledCommandFallbackReason::None;
    if (!AdmitCompiledGraphicsPacket(packet, prepared, indirect,
                                     indirect_operation, indirect_desc,
                                     indirect_argument, admission_reason))
      return admission_reason;
    const auto &submitted_root_tables = *prepared.root_tables;
    DiagReplayRecordScope diag_record_scope(packet.d3d_sequence,
                                            ++replay_record_serial);
    if (packet.d3d_sequence != 0)
      dxmt::apitrace::set_current_d3d_sequence(packet.d3d_sequence);
    CompiledGraphicsPacketPlan plan;
    const bool native_packet =
        packet.pipeline.metadata.uses_native_descriptor_table_abi;
    plan.native_packet = native_packet;
    if (const auto injected = InjectCompiledNativePacketFaults(
            native_packet, submitted_root_tables,
            "D3D12CommandQueue: injected queue-time native graphics "
            "pipeline compilation failure",
            "D3D12CommandQueue: injected queue-time native graphics "
            "descriptor lookup failure");
        injected != CompiledCommandFallbackReason::None)
      return injected;
    if (const auto pipeline_reason = ResolveCompiledPacketPipeline(
            packet, native_packet, plan.pipeline, plan.root);
        pipeline_reason != CompiledCommandFallbackReason::None)
      return pipeline_reason;
    auto *pipeline = plan.pipeline;
    auto &frozen_native = plan.frozen_native;
    if (!submitted_snapshot)
      return CompiledCommandFallbackReason::SnapshotDependent;
    frozen_native =
        submitted_snapshot->frozen_native &&
                submitted_snapshot->frozen_native->ready
            ? submitted_snapshot->frozen_native
            : std::shared_ptr<SubmittedFrozenNativeDescriptorStore>{};
    if (native_packet) {
      if (!frozen_native ||
          !submitted_snapshot->frozen_native_vertex.ready ||
          !submitted_snapshot->frozen_native_pixel.ready)
        return CompiledCommandFallbackReason::
            NativeMissingDescriptorBackend;
    }
    if (const auto metal_reason = ResolveCompiledGraphicsMetalPipeline(
            packet, submitted_snapshot, indirect_operation, plan);
        metal_reason != CompiledCommandFallbackReason::None)
      return metal_reason;
    DiagnoseCompiledGraphicsPacket(ctx, native_stage_plan_cache_, packet,
                                   prepared, submitted_snapshot,
                                   submitted_root_tables, indirect, plan);
    const auto vertex_buffer_reason =
        packet.vertex_binding_recipe_reason;
    if (vertex_buffer_reason != CompiledCommandFallbackReason::None)
      return vertex_buffer_reason;
    auto &packet_attachments = plan.packet_attachments;
    if (!encoder_attachments) {
      packet_attachments.emplace(BuildRenderPassAttachments(
          packet.render_state, pipeline->GetGraphicsState()));
      encoder_attachments = &*packet_attachments;
    }
    if (!packet.dynamic_render_state_recipe ||
        !packet.dynamic_render_state_recipe->valid)
      return CompiledCommandFallbackReason::UnsupportedRenderTargetState;
    plan.encoder_attachments = encoder_attachments;
    BuildCompiledGraphicsPacketBindingPlan(ctx, device_->GetMTLDevice(), packet,
                                           prepared, submitted_snapshot,
                                           submitted_root_tables, plan);
    if (indirect) {
      return QueueCompiledGraphicsIndirectCommands(
          ctx, packet, prepared, submitted_snapshot, plan, indirect,
          indirect_desc, indirect_argument);
    }
    if (packet.draw) {
      QueueCompiledDrawInstancedCommand(ctx, packet, *packet.draw, prepared,
                                        submitted_snapshot, plan);
    } else if (packet.draw_indexed) {
      if (const auto indexed_reason = QueueCompiledDrawIndexedInstancedCommand(
              ctx, packet, *packet.draw_indexed, prepared, submitted_snapshot,
              plan);
          indexed_reason != CompiledCommandFallbackReason::None)
        return indexed_reason;
    } else {
      // Unreachable: the !draw && !draw_indexed && !indirect case already
      // returned above, and the indirect case returned before this branch.
      // Kept as an explicit contract for the analyzer and future readers.
      return CompiledCommandFallbackReason::UnsupportedCommand;
    }
    chunk = queue.CurrentChunk();
    return CompiledCommandFallbackReason::None;
  }

  // Builds the compiled dispatch packet: binding state, direct access list,
  // native binding recipe and descriptor revision for one compute packet.
  void BuildCompiledComputeDispatchPacket(
      CompiledReplayContext &ctx, const CompiledComputePacket &packet,
      const SubmittedCompiledComputePacket &prepared,
      const std::shared_ptr<GraphicsBindingSnapshot> &submitted_snapshot,
      const std::vector<CompiledCommandRootDescriptorTable>
          &submitted_root_tables,
      CompiledComputePacketPlan &plan) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    auto *test_telemetry = ctx.test_telemetry;
    auto &compute_native_binding_recipes = ctx.compute_native_binding_recipes;
    const bool native_packet = plan.native_packet;
    auto *pipeline = plan.pipeline;
    auto *root = plan.root;
    const auto *metal = plan.metal;
    const auto &frozen_native = plan.frozen_native;
    auto &argument_buffer_size = plan.argument_buffer_size;
    auto &dispatch_packet = plan.dispatch_packet;
    std::shared_ptr<const CompiledNativeBindingRecipe> native_binding_recipe;
    CompiledPacketBindingState binding_state = {};
    FillCompiledBindingState(binding_state, packet.root_constants,
                             packet.root_descriptors,
                             submitted_snapshot.get());
    RecordCompiledDescriptorBackendStats(
        dxmt::perf::enabled() ? &queue.CurrentFrameStatistics() : nullptr,
        submitted_root_tables);
    CompiledDirectAccessList direct_access = {};
    direct_access.static_buffer_allocations =
        packet.direct_buffer_allocations;
    AddCompiledSnapshotEncoderAccesses(direct_access,
                                       submitted_snapshot.get());
    RecordComputePipelineResourceAccess(
        chunk, state, *pipeline, submitted_snapshot.get());
    argument_buffer_size = EstimateComputeArgumentBufferSize(*pipeline);
    if (native_packet) {
      const auto &native_compute =
          submitted_snapshot->frozen_native_compute;
      const NativeBindingRecipeKey recipe_key = {
          packet.binding_program.get(), submitted_snapshot.get()};
      if (const auto cached =
              compute_native_binding_recipes.find(recipe_key);
          cached != compute_native_binding_recipes.end()) {
        native_binding_recipe = cached->second;
        if (test_telemetry)
          test_telemetry->submitted_native_binding_recipe_reuses
              .fetch_add(1, std::memory_order_relaxed);
      } else {
        native_binding_recipe = BuildCompiledNativeBindingRecipe(
            *pipeline, true, *frozen_native,
            {{PipelineStage::Compute, &native_compute}});
        compute_native_binding_recipes.emplace(recipe_key,
                                                native_binding_recipe);
      }
    }
    dispatch_packet.metal_pso = metal->pso;
    dispatch_packet.threadgroup_size = metal->threadgroup_size;
    dispatch_packet.pipeline = pipeline;
    dispatch_packet.dispatch = packet.dispatch;
    dispatch_packet.argument_buffer_size = argument_buffer_size;
    auto binding_identity = BuildCompiledEncoderBindingIdentity(packet);
    if (frozen_native) {
      binding_identity.resource_heap = frozen_native.get();
      binding_identity.sampler_heap = frozen_native.get();
      binding_identity.root_tables = nullptr;
    }
    dispatch_packet.compiled_binding_payload.emplace(
        CompiledDirectComputeBindingPayload{
            .packet = packet,
            .root_tables = prepared.root_tables,
            .binding_state = std::move(binding_state),
            .direct_access = std::move(direct_access),
            .root = root,
            .bindless_snapshot =
                native_packet ? nullptr : submitted_snapshot,
            .frozen_native = frozen_native,
            .native_binding_recipe = native_binding_recipe,
            .binding_identity = binding_identity,
            .descriptor_content_revision =
                submitted_snapshot
                    ? submitted_snapshot->descriptor_content_revision
                    : dxmt::DescriptorContentRevision{
                          reinterpret_cast<uintptr_t>(
                              packet.binding_program.get()),
                          reinterpret_cast<uintptr_t>(
                              prepared.root_tables.get())},
            .test_telemetry = compiled->test_telemetry});
  }

  // Emits the per-command indirect dispatch stream of one compiled packet.
  [[nodiscard]] CompiledCommandFallbackReason
  QueueCompiledDispatchIndirectCommands(
      CompiledReplayContext &ctx, const ExecuteIndirectRecord *indirect,
      const D3D12_COMMAND_SIGNATURE_DESC *indirect_desc,
      const D3D12_INDIRECT_ARGUMENT_DESC *indirect_argument,
      CompiledComputePacketPlan &plan) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto argument_buffer_size = plan.argument_buffer_size;
    auto &dispatch_packet = plan.dispatch_packet;
    const UINT argument_size =
        IndirectArgumentByteSize(*indirect_argument);
    if (!argument_size || !indirect_desc->ByteStride ||
        indirect_desc->ByteStride < argument_size)
      return CompiledCommandFallbackReason::
          NativeUnsupportedExecuteIndirect;
    if (!indirect->max_command_count)
      return CompiledCommandFallbackReason::None;
    auto *argument_resource = GetResource(indirect->arg_buffer.ptr());
    const UINT64 argument_length =
        UINT64(indirect->max_command_count - 1) *
            indirect_desc->ByteStride +
        argument_size;
    if (!ValidateBufferRange(argument_resource,
                             indirect->arg_buffer_offset,
                             argument_length,
                             "compiled indirect dispatch buffer"))
      return CompiledCommandFallbackReason::
          NativeUnsupportedExecuteIndirect;
    Rc<Buffer> argument_buffer = argument_resource->GetBuffer();
    UINT64 argument_base_offset =
        argument_resource->GetHeapOffset() +
        indirect->arg_buffer_offset;
    UINT argument_stride = indirect_desc->ByteStride;
    if (indirect->count_buffer) {
      if (!ExpandCountedIndirectArguments(
              ctx, indirect, indirect_desc, argument_size,
              "compiled indirect dispatch count", argument_buffer,
              argument_base_offset, argument_stride))
        return CompiledCommandFallbackReason::
            NativeUnsupportedExecuteIndirect;
    }
    for (UINT command_index = 0;
         command_index < indirect->max_command_count;
         ++command_index) {
      ReplayDispatchPacket indirect_packet = {};
      indirect_packet.metal_pso = dispatch_packet.metal_pso;
      indirect_packet.threadgroup_size =
          dispatch_packet.threadgroup_size;
      indirect_packet.pipeline = dispatch_packet.pipeline;
      indirect_packet.argument_buffer_size =
          dispatch_packet.argument_buffer_size;
      indirect_packet.compiled_binding_payload =
          dispatch_packet.compiled_binding_payload;
      indirect_packet.indirect_argument_buffer = argument_buffer;
      indirect_packet.indirect_argument_offset =
          argument_base_offset +
          UINT64(command_index) * argument_stride;
      indirect_packet.indirect_argument_size = argument_size;
      QueueCompiledComputePassCommand(
          chunk, state, argument_buffer_size,
          std::move(indirect_packet),
          EncodeReplayDispatchCompiled);
    }
    chunk = queue.CurrentChunk();
    return CompiledCommandFallbackReason::None;
  }

  // Emits one Close-time compiled compute packet, or reports why submission
  // has to fall back to the compatibility replay path.
  [[nodiscard]] CompiledCommandFallbackReason QueueCompiledComputePacket(
      CompiledReplayContext &ctx, const CompiledComputePacket &packet,
      const SubmittedCompiledComputePacket &prepared,
      std::shared_ptr<GraphicsBindingSnapshot> submitted_snapshot,
      const ExecuteIndirectRecord *indirect = nullptr) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    auto &replay_record_serial = ctx.replay_record_serial;
    const D3D12_COMMAND_SIGNATURE_DESC *indirect_desc = nullptr;
    const D3D12_INDIRECT_ARGUMENT_DESC *indirect_argument = nullptr;
    auto admission_reason = CompiledCommandFallbackReason::None;
    if (!AdmitCompiledComputePacket(packet, prepared, indirect, indirect_desc,
                                    indirect_argument, admission_reason))
      return admission_reason;
    const auto &submitted_root_tables = *prepared.root_tables;
    DiagReplayRecordScope diag_record_scope(packet.d3d_sequence,
                                            ++replay_record_serial);
    if (packet.d3d_sequence != 0)
      dxmt::apitrace::set_current_d3d_sequence(packet.d3d_sequence);
    CompiledComputePacketPlan plan;
    const bool native_packet =
        packet.pipeline.metadata.uses_native_descriptor_table_abi;
    plan.native_packet = native_packet;
    if (const auto injected = InjectCompiledNativePacketFaults(
            native_packet, submitted_root_tables,
            "D3D12CommandQueue: injected queue-time native compute "
            "pipeline compilation failure",
            "D3D12CommandQueue: injected queue-time native compute "
            "descriptor lookup failure");
        injected != CompiledCommandFallbackReason::None)
      return injected;
    if (const auto pipeline_reason = ResolveCompiledPacketPipeline(
            packet, native_packet, plan.pipeline, plan.root);
        pipeline_reason != CompiledCommandFallbackReason::None)
      return pipeline_reason;
    auto *pipeline = plan.pipeline;
    auto *root = plan.root;
    auto &frozen_native = plan.frozen_native;
    if (!submitted_snapshot)
      return CompiledCommandFallbackReason::SnapshotDependent;
    frozen_native =
        submitted_snapshot->frozen_native &&
                submitted_snapshot->frozen_native->ready
            ? submitted_snapshot->frozen_native
            : std::shared_ptr<SubmittedFrozenNativeDescriptorStore>{};
    if (native_packet &&
        (!frozen_native ||
         !submitted_snapshot->frozen_native_compute.ready))
      return CompiledCommandFallbackReason::
          NativeMissingDescriptorBackend;
    auto *&metal = plan.metal;
    metal = packet.pipeline.metadata.metal_compute;
    if (!metal)
      metal = pipeline->GetMetalComputeState();
    if (!metal || !metal->pso)
      return CompiledCommandFallbackReason::MissingPipelineState;
    if (native_packet) {
      D3D12DiagLogNativePacket(
          "dispatch", queue.CurrentFrameSeq(), packet.d3d_sequence,
          replay_record_serial, *pipeline, metal->pso.handle,
          submitted_root_tables,
          {{"compute", &submitted_snapshot->frozen_native_compute}});
      DiagnoseCompiledNativeStageDescriptors(
          submitted_root_tables, *pipeline, *root, PipelineStage::Compute,
          true, "dispatch", queue.CurrentFrameSeq(),
          packet.d3d_sequence, replay_record_serial,
          metal->pso.handle);
    }
    if (!indirect &&
        !ValidateComputeDispatch(metal->threadgroup_size,
                                 packet.dispatch.x, packet.dispatch.y,
                                 packet.dispatch.z))
      return CompiledCommandFallbackReason::UnsupportedCommand;
    BuildCompiledComputeDispatchPacket(ctx, packet, prepared,
                                       submitted_snapshot,
                                       submitted_root_tables, plan);
    const auto argument_buffer_size = plan.argument_buffer_size;
    auto &dispatch_packet = plan.dispatch_packet;
    if (indirect) {
      return QueueCompiledDispatchIndirectCommands(
          ctx, indirect, indirect_desc, indirect_argument, plan);
    }
    QueueCompiledComputePassCommand(
        chunk, state, argument_buffer_size,
        std::move(dispatch_packet), EncodeReplayDispatchCompiled);
    chunk = queue.CurrentChunk();
    return CompiledCommandFallbackReason::None;
  }

  // RecordCompiledFallbackRangeStats() now lives in
  // d3d12_replay_compiled_execute_stats.hpp; unqualified calls below resolve
  // to dxmt::d3d12. It only reads the replay context's own references — the
  // frame statistics reached through ctx.queue, never through the queue
  // instance — and writes the fallback classification accumulators.

  // Replays the deferred compiled state records ahead of a fallback record
  // that needs the full binding state.
  void FlushDeferredCompiledStateRecords(CompiledReplayContext &ctx,
                                         dxmt::CompiledFallbackReason reason) {
    auto &state = ctx.state;
    const auto &records = ctx.records;
    const auto &superseded_state_record_mask = ctx.superseded_state_record_mask;
    auto &record_index = ctx.record_index;
    auto &deferred_state_records = ctx.deferred_state_records;
    auto &deferred_state_is_fallback = ctx.deferred_state_is_fallback;
    auto *test_telemetry = ctx.test_telemetry;
    if (deferred_state_records.empty())
      return;
    const auto previous_reason = state.compiled_fallback_reason;
    state.compiled_fallback_reason = reason;
    UINT replayed = 0;
    for (const auto index : deferred_state_records) {
      if (index >= records.size() || superseded_state_record_mask[index])
        continue;
      record_index = index;
      ReplayOneCompiledRecord(ctx, records[index]);
      replayed++;
    }
    if (replayed && deferred_state_is_fallback && test_telemetry) {
      test_telemetry->replayed_fallback_ranges.fetch_add(
          1, std::memory_order_relaxed);
      test_telemetry->replayed_fallback_records.fetch_add(
          replayed, std::memory_order_relaxed);
    }
    deferred_state_records.clear();
    deferred_state_is_fallback = false;
    state.compiled_fallback_reason = previous_reason;
  }

  // Replays only the deferred state an indirect packet does not itself carry
  // (notably predication and ClearState effects).
  void ReplayDeferredStateNotCapturedByIndirectPacket(
      CompiledReplayContext &ctx, bool compute) {
    const auto &records = ctx.records;
    const auto &superseded_state_record_mask = ctx.superseded_state_record_mask;
    auto &record_index = ctx.record_index;
    auto &deferred_state_records = ctx.deferred_state_records;
    auto &deferred_state_is_fallback = ctx.deferred_state_is_fallback;
    if (deferred_state_records.empty())
      return;
    UINT elided = 0;
    for (const auto index : deferred_state_records) {
      if (index >= records.size() || superseded_state_record_mask[index] ||
          IndirectPacketCapturesStateRecord(records[index], compute)) {
        elided++;
        continue;
      }
      record_index = index;
      ReplayOneCompiledRecord(ctx, records[index]);
    }
    if (elided)
      dxmt::perf::recordStateRecordsElided(
          dxmt::perf::currentFrameStatistics(), elided);
    deferred_state_records.clear();
    deferred_state_is_fallback = false;
  }

  // Replays a compatibility segment, deferring pure state records until a
  // record that actually consumes the binding state shows up.
  void ReplayCompiledCompatibilityRange(CompiledReplayContext &ctx, UINT begin,
                                        UINT count,
                                        dxmt::CompiledFallbackReason reason,
                                        bool legacy_fallback) {
    auto &state = ctx.state;
    const auto &records = ctx.records;
    auto &record_index = ctx.record_index;
    auto &deferred_state_records = ctx.deferred_state_records;
    auto &deferred_state_is_fallback = ctx.deferred_state_is_fallback;
    const UINT end = std::min<UINT>(begin + count, records.size());
    const auto previous_reason = state.compiled_fallback_reason;
    state.compiled_fallback_reason = reason;
    for (UINT index = begin; index < end; ++index) {
      if (IsDeferrableCompiledStateRecord(records[index])) {
        deferred_state_records.push_back(index);
        deferred_state_is_fallback |= legacy_fallback;
        continue;
      }
      if (FallbackRecordNeedsBindingState(records[index]))
        FlushDeferredCompiledStateRecords(ctx, reason);
      record_index = index;
      if (legacy_fallback)
        RecordCompiledFallbackRangeStats(ctx, index, 1, reason);
      ReplayOneCompiledRecord(ctx, records[index]);
    }
    state.compiled_fallback_reason = previous_reason;
  }

  // Emits every compiled graphics packet of one encoder-graph segment, falling
  // back to compatibility replay per packet when the packet is not encodable.
  void ExecuteCompiledGraphicsSegment(
      CompiledReplayContext &ctx, const CompiledCommandSegment &segment,
      const std::optional<ReplayRenderPassAttachments>
          &active_encoder_attachments) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    const auto *submitted = ctx.submitted;
    const auto *submitted_descriptor_snapshots =
        ctx.submitted_descriptor_snapshots;
    auto *test_telemetry = ctx.test_telemetry;
    auto &record_index = ctx.record_index;
    auto &replay_record_serial = ctx.replay_record_serial;
    auto &deferred_state_records = ctx.deferred_state_records;
    auto &deferred_state_is_fallback = ctx.deferred_state_is_fallback;
    const bool replay_perf = ctx.replay_perf;
    auto &rb_compiled_graphics_interval = ctx.rb.compiled_graphics_interval;
    auto &rb_compiled_graphics_packets = ctx.rb.compiled_graphics_packets;
    for (UINT i = 0; i < segment.graphics_packet_count; i++) {
      const auto packet_index = segment.first_graphics_packet + i;
      const auto &packet = compiled->graphics_packets[packet_index];
      const auto &prepared = submitted->graphics_packets[packet_index];
      auto submitted_snapshot =
          submitted_descriptor_snapshots &&
                  packet_index <
                      submitted_descriptor_snapshots->graphics.size()
              ? submitted_descriptor_snapshots->graphics[packet_index]
              : std::shared_ptr<GraphicsBindingSnapshot>{};
      const auto packet_begin =
          replay_perf ? clock::now() : clock::time_point{};
      const auto fallback_reason =
          QueueCompiledGraphicsPacket(ctx, packet, prepared,
                                      std::move(submitted_snapshot),
                                      active_encoder_attachments
                                          ? &*active_encoder_attachments
                                          : nullptr);
      if (test_telemetry) {
        if (fallback_reason == CompiledCommandFallbackReason::None) {
          test_telemetry->replayed_graphics_packets.fetch_add(
              1, std::memory_order_relaxed);
        } else {
          test_telemetry->replayed_compiled_packet_fallbacks.fetch_add(
              1, std::memory_order_relaxed);
        }
      }
      if (replay_perf) {
        rb_compiled_graphics_interval += clock::now() - packet_begin;
        rb_compiled_graphics_packets++;
      }
      if (fallback_reason != CompiledCommandFallbackReason::None) {
        const auto perf_reason =
            CompiledCommandFallbackReasonToPerf(fallback_reason);
        if (fallback_reason ==
            CompiledCommandFallbackReason::UnsupportedRenderTargetState)
          FlushPassBatches(chunk, state);
        // A compiled packet is a complete semantic state package. Use
        // it as the compatibility encoder input instead of replaying
        // every preceding setter merely to reconstruct the same state.
        deferred_state_records.clear();
        deferred_state_is_fallback = false;
        ApplyCompiledGraphicsCompatibilityState(state, packet);
        const auto previous_reason = state.compiled_fallback_reason;
        state.compiled_fallback_reason = perf_reason;
        record_index = packet.record_index;
        if (fallback_reason ==
            CompiledCommandFallbackReason::UnsupportedRenderTargetState) {
          if (packet.draw) {
            const CommandRecord compatibility_record = {
                packet.d3d_sequence, CommandRecordCompileKind::Graphics,
                *packet.draw};
            ReplayOneCompiledRecord(ctx, compatibility_record);
          } else if (packet.draw_indexed) {
            const CommandRecord compatibility_record = {
                packet.d3d_sequence, CommandRecordCompileKind::Graphics,
                *packet.draw_indexed};
            ReplayOneCompiledRecord(ctx, compatibility_record);
          }
        } else if (packet.draw) {
          DiagReplayRecordScope diag_record_scope(
              packet.d3d_sequence, ++replay_record_serial);
          ReplayStateRecordScope state_record_scope(
              state, packet.d3d_sequence);
          if (packet.d3d_sequence)
            dxmt::apitrace::set_current_d3d_sequence(
                packet.d3d_sequence);
          ReplayDrawInstanced(chunk, state, *packet.draw);
          chunk = queue.CurrentChunk();
        } else if (packet.draw_indexed) {
          DiagReplayRecordScope diag_record_scope(
              packet.d3d_sequence, ++replay_record_serial);
          ReplayStateRecordScope state_record_scope(
              state, packet.d3d_sequence);
          if (packet.d3d_sequence)
            dxmt::apitrace::set_current_d3d_sequence(
                packet.d3d_sequence);
          ReplayDrawIndexedInstanced(chunk, state,
                                     *packet.draw_indexed);
          chunk = queue.CurrentChunk();
        }
        state.compiled_fallback_reason = previous_reason;
      }
    }
  }

  // Emits every compiled compute packet of one encoder-graph segment.
  void ExecuteCompiledComputeSegment(CompiledReplayContext &ctx,
                                     const CompiledCommandSegment &segment) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    const auto *submitted = ctx.submitted;
    const auto *submitted_descriptor_snapshots =
        ctx.submitted_descriptor_snapshots;
    auto *test_telemetry = ctx.test_telemetry;
    auto &record_index = ctx.record_index;
    auto &replay_record_serial = ctx.replay_record_serial;
    auto &deferred_state_records = ctx.deferred_state_records;
    auto &deferred_state_is_fallback = ctx.deferred_state_is_fallback;
    const bool replay_perf = ctx.replay_perf;
    auto &rb_compiled_compute_interval = ctx.rb.compiled_compute_interval;
    auto &rb_compiled_compute_packets = ctx.rb.compiled_compute_packets;
    for (UINT i = 0; i < segment.compute_packet_count; i++) {
      const auto packet_index = segment.first_compute_packet + i;
      const auto &packet = compiled->compute_packets[packet_index];
      const auto &prepared = submitted->compute_packets[packet_index];
      auto submitted_snapshot =
          submitted_descriptor_snapshots &&
                  packet_index <
                      submitted_descriptor_snapshots->compute.size()
              ? submitted_descriptor_snapshots->compute[packet_index]
              : std::shared_ptr<GraphicsBindingSnapshot>{};
      const auto packet_begin =
          replay_perf ? clock::now() : clock::time_point{};
      const auto fallback_reason = QueueCompiledComputePacket(
          ctx, packet, prepared, std::move(submitted_snapshot));
      if (test_telemetry) {
        if (fallback_reason == CompiledCommandFallbackReason::None) {
          test_telemetry->replayed_compute_packets.fetch_add(
              1, std::memory_order_relaxed);
        } else {
          test_telemetry->replayed_compiled_packet_fallbacks.fetch_add(
              1, std::memory_order_relaxed);
        }
      }
      if (replay_perf) {
        rb_compiled_compute_interval += clock::now() - packet_begin;
        rb_compiled_compute_packets++;
      }
      if (fallback_reason != CompiledCommandFallbackReason::None) {
        const auto perf_reason =
            CompiledCommandFallbackReasonToPerf(fallback_reason);
        deferred_state_records.clear();
        deferred_state_is_fallback = false;
        ApplyCompiledComputeCompatibilityState(state, packet);
        const auto previous_reason = state.compiled_fallback_reason;
        state.compiled_fallback_reason = perf_reason;
        record_index = packet.record_index;
        DiagReplayRecordScope diag_record_scope(
            packet.d3d_sequence, ++replay_record_serial);
        ReplayStateRecordScope state_record_scope(
            state, packet.d3d_sequence);
        if (packet.d3d_sequence)
          dxmt::apitrace::set_current_d3d_sequence(
              packet.d3d_sequence);
        FlushGraphicsBeforeCompute(chunk, state);
        ReplayDispatch(chunk, state, packet.dispatch);
        chunk = queue.CurrentChunk();
        state.compiled_fallback_reason = previous_reason;
      }
    }
  }

  // Emits every compiled ExecuteIndirect node of one encoder-graph segment.
  void ExecuteCompiledIndirectSegment(
      CompiledReplayContext &ctx, const CompiledCommandSegment &segment,
      const std::optional<ReplayRenderPassAttachments>
          &active_encoder_attachments) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    const auto *submitted = ctx.submitted;
    const auto *submitted_descriptor_snapshots =
        ctx.submitted_descriptor_snapshots;
    auto *test_telemetry = ctx.test_telemetry;
    auto &record_index = ctx.record_index;
    auto &replay_record_serial = ctx.replay_record_serial;
    for (UINT i = 0; i < segment.indirect_packet_count; i++) {
      const auto packet_index = segment.first_indirect_packet + i;
      const auto &packet = compiled->indirect_packets[packet_index];
      if (test_telemetry) {
        test_telemetry->replayed_indirect_nodes.fetch_add(
            1, std::memory_order_relaxed);
        if (packet.execute.count_buffer)
          test_telemetry->replayed_indirect_dependency_nodes.fetch_add(
              1, std::memory_order_relaxed);
        auto *signature = dynamic_cast<CommandSignature *>(
            packet.execute.command_signature.ptr());
        if (!signature ||
            GetDirectIndirectOperation(signature->GetArguments()) ==
                DirectIndirectOperation::None)
          test_telemetry->replayed_indirect_fallbacks.fetch_add(
              1, std::memory_order_relaxed);
      }
      // The packet already contains the complete state for its pipeline
      // domain. Replay only deferred state outside that domain (notably
      // predication and ClearState effects) instead of redundantly
      // rebuilding every root/IA binding before each indirect node.
      ReplayDeferredStateNotCapturedByIndirectPacket(ctx,
                                                    packet.compute);
      if (packet.compute &&
          packet.state_packet_index <
              compiled->compute_packets.size()) {
        const auto &state_packet =
            compiled->compute_packets[packet.state_packet_index];
        const auto &prepared =
            submitted->compute_packets[packet.state_packet_index];
        auto submitted_snapshot =
            submitted_descriptor_snapshots &&
                    packet.state_packet_index <
                        submitted_descriptor_snapshots->compute.size()
                ? submitted_descriptor_snapshots
                      ->compute[packet.state_packet_index]
                : std::shared_ptr<GraphicsBindingSnapshot>{};
        const auto fallback_reason = QueueCompiledComputePacket(
            ctx, state_packet, prepared, std::move(submitted_snapshot),
            &packet.execute);
        if (fallback_reason != CompiledCommandFallbackReason::None) {
          ApplyCompiledComputeCompatibilityState(state, state_packet);
          ReplayExecuteIndirect(chunk, state, packet.execute);
        }
        if (test_telemetry)
          test_telemetry->replayed_compute_packets.fetch_add(
              1, std::memory_order_relaxed);
      } else if (!packet.compute &&
                 packet.state_packet_index <
                     compiled->graphics_packets.size()) {
        const auto &state_packet =
            compiled->graphics_packets[packet.state_packet_index];
        const auto &prepared =
            submitted->graphics_packets[packet.state_packet_index];
        auto submitted_snapshot =
            submitted_descriptor_snapshots &&
                    packet.state_packet_index <
                        submitted_descriptor_snapshots->graphics.size()
                ? submitted_descriptor_snapshots
                      ->graphics[packet.state_packet_index]
                : std::shared_ptr<GraphicsBindingSnapshot>{};
        const auto fallback_reason = QueueCompiledGraphicsPacket(
            ctx, state_packet, prepared, std::move(submitted_snapshot),
            active_encoder_attachments
                ? &*active_encoder_attachments
                : nullptr,
            &packet.execute);
        if (fallback_reason != CompiledCommandFallbackReason::None) {
          ApplyCompiledGraphicsCompatibilityState(state, state_packet);
          ReplayExecuteIndirect(chunk, state, packet.execute);
        }
        if (test_telemetry)
          test_telemetry->replayed_graphics_packets.fetch_add(
              1, std::memory_order_relaxed);
      }
      record_index = packet.record_index;
      DiagReplayRecordScope diag_record_scope(
          packet.d3d_sequence, ++replay_record_serial);
      ReplayStateRecordScope state_record_scope(state,
                                                packet.d3d_sequence);
      if (packet.d3d_sequence)
        dxmt::apitrace::set_current_d3d_sequence(packet.d3d_sequence);

      // ExecuteIndirect is a compiled dynamic node. Submission only
      // patches its live resource state and emits the prepared GPU
      // argument stream; it must not manufacture a CommandRecord or
      // enter the compatibility replay visitor.
      RecordReplayResourceAccess(
          chunk, state, packet.execute.arg_buffer.ptr(),
          D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
          "ExecuteIndirect arguments");
      RecordReplayResourceAccess(
          chunk, state, packet.execute.count_buffer.ptr(),
          D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
          "ExecuteIndirect count");
      chunk = queue.CurrentChunk();
    }
  }

  // Translates one compiled barrier segment into a resource access batch.
  void ExecuteCompiledBarrierSegment(CompiledReplayContext &ctx,
                                     const CompiledCommandSegment &segment) {
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    state.access_epoch++;
    if (ReplayPerfEnabled())
      perDrawSubTimers().appBarrierTransitions +=
          segment.barrier_count;
    ResourceAccessBarrierBatch batch;
    const auto end = segment.first_barrier + segment.barrier_count;
    for (UINT barrier_index = segment.first_barrier;
         barrier_index < end; ++barrier_index) {
      const auto &barrier =
          compiled->access_summary.barriers[barrier_index];
      switch (barrier.barrier.Type) {
      case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
        ReplayTransitionBarrier(state, barrier, batch);
        break;
      case D3D12_RESOURCE_BARRIER_TYPE_UAV:
        ReplayUavBarrier(barrier, batch);
        break;
      case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
        ReplayAliasingBarrier(state, barrier, batch);
        break;
      default:
        batch.needs_separator = true;
        break;
      }
    }
    if (!batch.entries.empty() || batch.needs_separator)
      QueueOrDeferResourceAccessBarrierBatch(chunk, state,
                                             std::move(batch));
  }

  // Replays every transfer (copy / clear / resolve) packet of one segment.
  void ExecuteCompiledTransferSegment(CompiledReplayContext &ctx,
                                      const CompiledCommandSegment &segment) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    auto &replay_record_serial = ctx.replay_record_serial;
    for (UINT i = 0; i < segment.transfer_packet_count; ++i) {
      const auto &packet = compiled->transfer_packets[
          segment.first_transfer_packet + i];
      DiagReplayRecordScope diag_record_scope(
          packet.d3d_sequence, ++replay_record_serial);
      ReplayStateRecordScope state_record_scope(state,
                                                packet.d3d_sequence);
      if (packet.d3d_sequence)
        dxmt::apitrace::set_current_d3d_sequence(packet.d3d_sequence);
      switch (packet.opcode) {
      case CompiledTransferOpcode::CopyBufferRegion:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.copy_buffer_regions[
                packet.payload_index]);
        break;
      case CompiledTransferOpcode::CopyTextureRegion:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.copy_texture_regions[
                packet.payload_index]);
        break;
      case CompiledTransferOpcode::CopyResource:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.copy_resources[
                packet.payload_index]);
        break;
      case CompiledTransferOpcode::CopyTiles:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.copy_tiles[packet.payload_index]);
        break;
      case CompiledTransferOpcode::ResolveSubresource:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.resolves[packet.payload_index]);
        break;
      case CompiledTransferOpcode::ClearRenderTarget:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.clear_render_targets[
                packet.payload_index]);
        break;
      case CompiledTransferOpcode::ClearDepthStencil:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.clear_depth_stencils[
                packet.payload_index]);
        break;
      case CompiledTransferOpcode::ClearUnorderedAccess:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.clear_unordered_access[
                packet.payload_index]);
        break;
      case CompiledTransferOpcode::DiscardResource:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.discards[packet.payload_index]);
        break;
      case CompiledTransferOpcode::WriteBufferImmediate:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.write_buffer_immediate[
                packet.payload_index]);
        break;
      case CompiledTransferOpcode::TemporalUpscale:
        ReplayRecord(chunk, state,
            compiled->transfer_storage.temporal_upscales[
                packet.payload_index]);
        break;
      }
      chunk = queue.CurrentChunk();
    }
  }

  // Replays every control (query / predication / ClearState) packet of one
  // segment.
  void ExecuteCompiledControlSegment(CompiledReplayContext &ctx,
                                     const CompiledCommandSegment &segment) {
    auto &queue = ctx.queue;
    auto *&chunk = ctx.chunk;
    auto &state = ctx.state;
    const auto *compiled = ctx.compiled;
    auto &replay_record_serial = ctx.replay_record_serial;
    for (UINT i = 0; i < segment.control_packet_count; ++i) {
      const auto &packet = compiled->control_packets[
          segment.first_control_packet + i];
      DiagReplayRecordScope diag_record_scope(
          packet.d3d_sequence, ++replay_record_serial);
      ReplayStateRecordScope state_record_scope(state,
                                                packet.d3d_sequence);
      if (packet.d3d_sequence)
        dxmt::apitrace::set_current_d3d_sequence(packet.d3d_sequence);
      switch (packet.opcode) {
      case CompiledControlOpcode::ClearState:
        ReplayRecord(chunk, state,
            compiled->control_storage.clear_states[packet.payload_index]);
        break;
      case CompiledControlOpcode::BeginQuery:
        ReplayRecord(chunk, state,
            compiled->control_storage.begin_queries[packet.payload_index]);
        break;
      case CompiledControlOpcode::EndQuery:
        ReplayRecord(chunk, state,
            compiled->control_storage.end_queries[packet.payload_index]);
        break;
      case CompiledControlOpcode::ResolveQueryData:
        ReplayRecord(chunk, state,
            compiled->control_storage.resolve_queries[
                packet.payload_index]);
        break;
      case CompiledControlOpcode::Predication:
        ReplayRecord(chunk, state,
            compiled->control_storage.predications[packet.payload_index]);
        break;
      }
      chunk = queue.CurrentChunk();
    }
  }

  // AccumulateCompiledReplayStageIntervals(),
  // AccumulateCompiledReplaySubPhaseIntervals(),
  // RecordCompiledReplayFrameStatistics() and LogCompiledReplayBreakdown() now
  // live in d3d12_replay_compiled_execute_stats.hpp; unqualified calls below
  // resolve to dxmt::d3d12. The first two were blocked only on
  // ReplayBreakdownAccumulators / StallProbe, which are now namespace-scope
  // types. RecordCompiledReplayFrameStatistics() read exactly two scalars off
  // the queue instance for its diagnostic line — the queue address and
  // desc_.Type — which the call site below now passes in, so the log text is
  // unchanged.

  // Executes the Close-time encoder program node by node: materializes the
  // per-encoder attachment identity and dispatches each segment kind.
  void ExecuteCompiledEncoderGraph(CompiledReplayContext &ctx) {
    const auto *compiled = ctx.compiled;
    auto *test_telemetry = ctx.test_telemetry;
    auto &record_index = ctx.record_index;
    auto &deferred_state_records = ctx.deferred_state_records;
    auto &deferred_state_is_fallback = ctx.deferred_state_is_fallback;
    // Execute the Close-time encoder program. The source segments remain in
    // the generation only for diagnostics and compatibility accounting;
    // state-only ranges and compatible adjacent work have already been
    // folded into encoder nodes.
    UINT active_encoder_index = UINT_MAX;
    std::optional<ReplayRenderPassAttachments>
        active_encoder_attachments;
    for (const auto &encoder_node : compiled->encoder_graph.nodes) {
      const auto &segment = encoder_node.work;
      if (encoder_node.encoder_index != active_encoder_index) {
        active_encoder_index = encoder_node.encoder_index;
        active_encoder_attachments.reset();
      }
      if (encoder_node.begins_encoder &&
          encoder_node.encoder_kind == CompiledEncoderKind::Graphics) {
        MaterializeActiveEncoderAttachments(ctx, device_->GetMTLDevice(),
                                            segment,
                                            active_encoder_attachments);
      }
      record_index = segment.first_record_index;
      if (!segment.record_count && test_telemetry) {
        if (segment.kind == CompiledCommandSegmentKind::Fallback) {
          test_telemetry->replayed_empty_fallback_segments.fetch_add(
              1, std::memory_order_relaxed);
        } else {
          test_telemetry->replayed_empty_native_segments.fetch_add(
              1, std::memory_order_relaxed);
        }
      }
      switch (segment.kind) {
      case CompiledCommandSegmentKind::Graphics:
        ExecuteCompiledGraphicsSegment(ctx, segment,
                                       active_encoder_attachments);
        break;
      case CompiledCommandSegmentKind::Compute:
        ExecuteCompiledComputeSegment(ctx, segment);
        break;
      case CompiledCommandSegmentKind::Indirect:
        ExecuteCompiledIndirectSegment(ctx, segment,
                                       active_encoder_attachments);
        break;
      case CompiledCommandSegmentKind::Barrier:
        ExecuteCompiledBarrierSegment(ctx, segment);
        break;
      case CompiledCommandSegmentKind::Transfer:
        ExecuteCompiledTransferSegment(ctx, segment);
        break;
      case CompiledCommandSegmentKind::Control:
        ExecuteCompiledControlSegment(ctx, segment);
        break;
      case CompiledCommandSegmentKind::ElidedState:
        // Close-time graph construction never publishes state-only nodes.
        break;
      case CompiledCommandSegmentKind::Typed:
        ReplayCompiledCompatibilityRange(
            ctx, segment.first_record_index, segment.record_count,
            dxmt::CompiledFallbackReason::Unknown, false);
        break;
      case CompiledCommandSegmentKind::Fallback:
      default:
        ReplayCompiledCompatibilityRange(ctx, segment.first_record_index,
                                         segment.record_count,
                                         segment.perf_fallback_reason, true);
        break;
      }
    }
    if (!deferred_state_records.empty()) {
      dxmt::perf::recordStateRecordsElided(
          dxmt::perf::currentFrameStatistics(),
          deferred_state_records.size());
      deferred_state_records.clear();
      deferred_state_is_fallback = false;
    }
  }

  // Replays one submitted compiled command list: runs the Close-time encoder
  // program when the plan validates, otherwise the whole compatibility record
  // stream, then finalizes the submission's passes and query resolves.
  void ExecuteCompiledCommandList(
      const SubmittedCompiledCommandListPlan *submitted,
      const CompiledCommandDescriptorSnapshots *submitted_descriptor_snapshots,
      ReplayState &state, bool finalize_submission,
      ResourceAccessBarrierBatch &queue_pending_barriers) {
    auto &queue = device_->GetDXMTDevice().queue();
    auto *chunk = queue.CurrentChunk();
    const CompiledCommandList *compiled =
        submitted && submitted->generation ? submitted->generation.get()
                                            : nullptr;
    if (!compiled)
      return;
    static const std::vector<CommandRecord> no_fallback_records;
    const auto &records = compiled->fallback_records
                              ? *compiled->fallback_records
                              : no_fallback_records;
    if (HasPendingGraphicsPass(state)) {
      state.submission_boundary_kind = CompiledEncoderKind::Graphics;
      state.submission_boundary_telemetry =
          compiled->test_telemetry.get();
    } else if (HasPendingComputePass(state)) {
      state.submission_boundary_kind = CompiledEncoderKind::Compute;
      state.submission_boundary_telemetry =
          compiled->test_telemetry.get();
    }
    ResetReplayCommandListSemanticState(state);
    if (ShouldLogCompiledReplayDiagnostic()) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: execute compiled nodes begin"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " queueType=", desc_.Type,
           " records=", records.size());
    }
    UINT record_index = 0;
    uint64_t replay_record_serial = 0;
    auto *test_telemetry =
        compiled && compiled->test_telemetry
            ? compiled->test_telemetry.get()
            : nullptr;
    // PERF DIAG (DXMT_DIAG_REPLAY_BREAKDOWN): sub-time the replay stage to
    // attribute the 300-780ms explosion-frame replay to record-loop vs
    // FlushPassBatches vs timestamp/query materialization.
    const bool replay_perf = ReplayPerfEnabled();
    const bool replay_breakdown = D3D12RecordLoopBreakdownEnabled();
    const auto rb_t0 = replay_perf ? clock::now() : clock::time_point{};
    if (replay_perf) {
      perDrawSubTimers() = {};
      replayRttiCounters() = {};
    }
    // Perf replay sub-phase aggregation. DXMT_PERF_STATS enables aggregate
    // timers; DXMT_DIAG_STALL only adds the slow-record detail log.
    // Sums every concrete replay-record body and its named draw/dispatch
    // sub-phases. Record body totals remain attributed to the exact
    // CommandRecord overload through RecordReplayRecordPerfBucket; only named
    // implementation paths are emitted.
    // Serial replay (intra-pass gate off) → no lock needed.
    ReplayBreakdownAccumulators rb = {};
    const auto rb_superseded_mask_begin =
        replay_perf ? clock::now() : clock::time_point{};
    const auto &superseded_state_record_mask =
        compiled->superseded_state_record_mask;
    if (replay_perf)
      rb.superseded_mask_interval +=
          clock::now() - rb_superseded_mask_begin;

    NativeBindingRecipeCache graphics_native_binding_recipes;
    NativeBindingRecipeCache compute_native_binding_recipes;

    std::vector<UINT> deferred_state_records;
    if (compiled->fallback_records)
      deferred_state_records.reserve(256);
    bool deferred_state_is_fallback = false;
    CompiledReplayContext ctx{
        .queue = queue,
        .chunk = chunk,
        .state = state,
        .submitted = submitted,
        .submitted_descriptor_snapshots = submitted_descriptor_snapshots,
        .compiled = compiled,
        .test_telemetry = test_telemetry,
        .records = records,
        .superseded_state_record_mask = superseded_state_record_mask,
        .record_index = record_index,
        .replay_record_serial = replay_record_serial,
        .deferred_state_records = deferred_state_records,
        .deferred_state_is_fallback = deferred_state_is_fallback,
        .graphics_native_binding_recipes = graphics_native_binding_recipes,
        .compute_native_binding_recipes = compute_native_binding_recipes,
        .rb = rb,
        .replay_perf = replay_perf,
        .replay_breakdown = replay_breakdown,
    };

    if (IsCompiledCommandListPlanValid(compiled, submitted, records)) {
      if (compiled->encoder_graph.elided_state_record_count)
        dxmt::perf::recordStateRecordsElided(
            dxmt::perf::currentFrameStatistics(),
            compiled->encoder_graph.elided_state_record_count);
      ExecuteCompiledEncoderGraph(ctx);
    } else {
      RecordCompiledFallbackRangeStats(
          ctx, 0, static_cast<UINT>(records.size()),
          dxmt::CompiledFallbackReason::MissingCompiledEncoder);
      ReplayCompiledRecordRange(
          ctx, 0, static_cast<UINT>(records.size()),
          dxmt::CompiledFallbackReason::MissingCompiledEncoder);
    }
    if (ShouldLogCompiledReplayDiagnostic()) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: execute compiled nodes flush begin"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " queueType=", desc_.Type,
           " records=", records.size());
    }
    const auto rb_t1 = replay_perf ? clock::now() : clock::time_point{};
    // A deferred timestamp marker is an ordering point between command lists.
    // Otherwise keep compatible render/compute work open until the entire
    // ExecuteCommandLists submission has been consumed.
    if (finalize_submission || !state.pending_timestamp_markers.empty())
      FlushPassBatches(chunk, state);
    const auto rb_t2 = replay_perf ? clock::now() : clock::time_point{};
    if (finalize_submission)
      MaterializeTimestampResolves(chunk, state);
    const auto rb_t3 = replay_perf ? clock::now() : clock::time_point{};
    if (finalize_submission) {
      MaterializeCpuQueryResolves(chunk, state);
      queue_pending_barriers = TakePendingResourceBarrierBatch(state);
    }
    if (replay_perf)
      RecordCompiledReplayFrameStatistics(
          ctx, reinterpret_cast<uintptr_t>(this), desc_.Type, rb_t0, rb_t1,
          rb_t2, rb_t3);
    if (replay_breakdown)
      LogCompiledReplayBreakdown(ctx, rb_t0, rb_t1, rb_t2, rb_t3);
    if (ShouldLogCompiledReplayDiagnostic()) {
      WARN_FILE_ONLY("D3D12 queue diagnostic: replay records end"
           " queue=", reinterpret_cast<uintptr_t>(this),
           " queueType=", desc_.Type,
           " records=", records.size(),
           " touchedResources=",
           state.touched_resources ? state.touched_resources->size() : 0);
    }
  }
