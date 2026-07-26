// Draw-state debug logging and IA/CBV readback dumps.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // The report itself never touched this queue beyond the Metal device handle
  // it needs for DXGI format translation, so it lives in
  // d3d12_queue_draw_state_dump.cpp.
  void DebugLogDrawState(const char *kind, const ReplayState &state,
                         PipelineState &pipeline,
                         const PipelineMetalGraphicsState &metal,
                         const ReplayRenderPassAttachments &attachments,
                         const std::vector<D3D12_VIEWPORT> &viewports,
                         const std::vector<D3D12_RECT> &scissors,
                         const DrawInstancedRecord *draw,
                         const DrawIndexedInstancedRecord *indexed_draw,
                         UINT64 index_resource_offset,
                         UINT64 index_offset) {
    LogReplayDrawState(device_->GetMTLDevice(), kind, state, pipeline, metal,
                       attachments, viewports, scissors, draw, indexed_draw,
                       index_resource_offset, index_offset);
  }


  void DebugEncodeIAReadbacks(CommandChunk *chunk, const char *kind,
                              const ReplayState &state,
                              PipelineState &pipeline,
                              const DrawInstancedRecord *draw,
                              const DrawIndexedInstancedRecord *indexed_draw,
                              UINT64 index_offset) {
    static std::atomic<uint32_t> log_count = 0;
    if (!D3D12DiagShouldLog(log_count, D3D12DiagIAReadbackEnabled()))
      return;

    const auto *graphics = pipeline.GetGraphicsState();
    if (!graphics)
      return;

    const auto sample_limit = D3D12DiagIAReadbackBytes();
    const auto &cache_key = pipeline.GetShaderCacheKey();
    const std::string key_prefix = ShaderCacheKeyPrefix(cache_key);
    const auto d3d_sequence = DiagCurrentReplayRecordSequence();

    if (indexed_draw && state.index_buffer) {
      UINT64 index_resource_offset = 0;
      auto *resource = LookupBufferResourceByGpuVirtualAddress(
          state.index_buffer->BufferLocation, &index_resource_offset);
      if (resource && resource->GetBufferAllocation()) {
        const auto index_size = GetIndexSize(state.index_buffer->Format);
        const auto max_bytes =
            uint64_t(indexed_draw->index_count_per_instance) * index_size;
        const auto size =
            std::min<uint64_t>({sample_limit, max_bytes,
                                state.index_buffer->SizeInBytes});
        if (size) {
          const auto staging = AllocateDiagnosticReadbackStaging(
              device_->GetMTLDevice(), size);
          if (staging.buffer && staging.mapped) {
            Rc<BufferAllocation> allocation = resource->GetBufferAllocation();
            EncodeDiagnosticReadbackBlit(chunk, allocation, staging.buffer,
                                         index_offset, size);
            EnqueueRetirement(
                chunk,
                IndexReadbackRetirementWork{
                    .buffer = DiagnosticReadbackBuffer(staging.buffer,
                                                       staging.mapped),
                    .key_prefix = key_prefix,
                    .kind = kind,
                    .d3d_sequence = d3d_sequence,
                    .format = state.index_buffer->Format,
                    .size = size,
                    .start_index = indexed_draw->start_index_location,
                    .index_count = indexed_draw->index_count_per_instance,
                    .base_vertex = indexed_draw->base_vertex_location,
                });
          }
        }
      }
    }

    const auto slot_mask = InputSlotMask(graphics);
    const auto max_slot = InputSlotMaskWidth(slot_mask);
    for (UINT slot = 0; slot < max_slot; slot++) {
      if (!(slot_mask & (1u << slot)))
        continue;
      const auto &vertex_buffer = state.vertex_buffers[slot];
      if (!vertex_buffer)
        continue;

      const auto &view = *vertex_buffer;
      UINT64 resource_offset = 0;
      auto *resource =
          LookupBufferResourceByGpuVirtualAddress(view.BufferLocation,
                                                  &resource_offset);
      if (!resource || !resource->GetBufferAllocation() || !view.SizeInBytes)
        continue;

      const UINT64 vertex_offset =
          ReadbackVertexOffset(draw, indexed_draw, view.StrideInBytes);
      if (vertex_offset >= view.SizeInBytes)
        continue;

      const auto size =
          std::min<uint64_t>(sample_limit, view.SizeInBytes - vertex_offset);
      if (!size)
        continue;

      const auto staging =
          AllocateDiagnosticReadbackStaging(device_->GetMTLDevice(), size);
      if (!staging.buffer || !staging.mapped)
        continue;

      Rc<BufferAllocation> allocation = resource->GetBufferAllocation();
      const auto src_offset =
          resource->GetHeapOffset() + resource_offset + vertex_offset;
      EncodeDiagnosticReadbackBlit(chunk, allocation, staging.buffer,
                                   src_offset, size);
      EnqueueRetirement(
          chunk,
          VertexReadbackRetirementWork{
              .buffer =
                  DiagnosticReadbackBuffer(staging.buffer, staging.mapped),
              .key_prefix = key_prefix,
              .kind = kind,
              .d3d_sequence = d3d_sequence,
              .slot = slot,
              .stride = view.StrideInBytes,
              .view_size = view.SizeInBytes,
              .resource_offset = resource_offset,
              .vertex_offset = vertex_offset,
              .heap_offset = resource->GetHeapOffset(),
              .size = size,
          });
    }
  }

  void DebugEncodeCBVReadbacks(CommandChunk *chunk, const char *kind,
                               const ReplayState &state,
                               PipelineState &pipeline,
                               uint64_t draw_record_sequence) {
    static std::atomic<uint32_t> log_count = 0;
    const bool diag_readback = D3D12DiagCBVReadbackEnabled();
    const bool snapshot_readback = D3D12ApitraceGpuCbvSnapshotEnabled();
    if (!diag_readback && !snapshot_readback)
      return;

    auto *root = state.graphics_root_signature_impl;
    if (!root)
      return;

    const auto &cache_key = pipeline.GetShaderCacheKey();
    // Gate: the target composite PSO (bee0b725) gets a bounded number of readbacks
    // so we can dump its LIVE cbuffer - including dynamically-indexed elements
    // beyond the bound view - without the unbounded per-draw readback that
    // stalls/crashes the game.
    const bool target_pso = DiagIsTargetCompositePso(cache_key);
    static std::atomic<uint32_t> target_count = 0;
    bool log_readback = false;
    if (target_pso) {
      log_readback =
          diag_readback &&
          target_count.fetch_add(1, std::memory_order_relaxed) < 8;
    } else if (diag_readback) {
      log_readback = D3D12DiagShouldLog(log_count, true);
    }
    const bool snapshot_target = snapshot_readback && target_pso;
    if (!log_readback && !snapshot_target)
      return;

    const std::string key_prefix = ShaderCacheKeyPrefix(cache_key);
    const auto d3d_sequence = DiagCurrentReplayRecordSequence();
    const bool record_snapshot = snapshot_target && draw_record_sequence != 0;
    if (snapshot_target && !record_snapshot) {
      static std::atomic<uint32_t> missing_sequence_log_count = 0;
      if (D3D12DiagShouldLog(missing_sequence_log_count, true)) {
        WARN("D3D12 diagnostic: CBV readback snapshot skipped without draw record sequence",
             " kind=", kind,
             " sequence=", d3d_sequence,
             " currentD3DSequence=", dxmt::apitrace::current_d3d_sequence(),
             " pso=", key_prefix);
      }
    }
    if (!log_readback && !record_snapshot)
      return;
    const auto sample_limit = D3D12DiagCBVReadbackBytes();
    const auto parameters = root->GetParameters();

    for (UINT root_index = 0; root_index < parameters.size(); root_index++) {
      const auto &parameter = parameters[root_index];
      struct CBVReadbackTarget {
        PipelineStage stage;
        UINT slot;
        DescriptorRecord descriptor;
      };
      std::vector<CBVReadbackTarget> cbvs;
      if (parameter.parameter_type ==
          D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
        const auto base = GetTableHandle(state, false, root_index);
        if (!base.ptr)
          continue;
        UINT running_offset = 0;
        for (const auto &range : parameter.ranges) {
          const auto range_offset =
              DescriptorRangeOffset(range, running_offset);
          const auto count =
              range.descriptor_count == UINT_MAX
                  ? ReflectedDescriptorRangeCount(
                        pipeline, range, parameter.visibility, false)
                  : range.descriptor_count;
          if (range.range_type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
            for (UINT i = 0; i < count; i++) {
              auto descriptor = GetBoundDescriptorRecordInRange(
                  state, base, range_offset, i, count,
                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
              if (!descriptor)
                continue;
              ForEachVisibleStage(
                  parameter.visibility, false, [&](PipelineStage stage) {
                    auto slot = ResolveShaderBindingSlot(
                        pipeline, stage, SM50BindingType::ConstantBuffer,
                        range.base_shader_register + i,
                        range.register_space);
                    if (slot)
                      cbvs.push_back({stage, *slot, *descriptor});
                  });
            }
          }
          if (range.descriptor_count != UINT_MAX)
            running_offset = range_offset + range.descriptor_count;
        }
      } else if (parameter.parameter_type == D3D12_ROOT_PARAMETER_TYPE_CBV) {
        if (root_index >= ReplayState::kMaxRootParameters)
          continue;
        const auto &root_cbv = state.graphics_cbv_roots[root_index];
        if (!root_cbv.valid)
          continue;

        Resource *resource = nullptr;
        const auto offset = ResolveBufferGpuAddress(root_cbv.address, resource);
        if (!resource || !resource->GetBuffer())
          continue;

        DescriptorRecord descriptor = {};
        descriptor.type = DescriptorRecordType::ConstantBufferView;
        descriptor.resource = resource->GetD3D12Resource();
        descriptor.has_desc = true;
        descriptor.desc.cbv.BufferLocation = root_cbv.address;
        descriptor.desc.cbv.SizeInBytes =
            UINT(std::min<UINT64>(resource->GetResourceDesc().Width - offset,
                                  UINT_MAX));

        ForEachVisibleStage(parameter.visibility, false,
                            [&](PipelineStage stage) {
                              auto slot = ResolveShaderBindingSlot(
                                  pipeline, stage,
                                  SM50BindingType::ConstantBuffer,
                                  parameter.descriptor.ShaderRegister,
                                  parameter.descriptor.RegisterSpace);
                              if (slot)
                                cbvs.push_back({stage, *slot, descriptor});
                            });
      }

      for (const auto &[stage, slot, descriptor] : cbvs) {
        if (descriptor.type != DescriptorRecordType::ConstantBufferView ||
            !descriptor.has_desc)
          continue;

        Resource *resource = nullptr;
        const auto resource_offset = ResolveBufferGpuAddress(
            descriptor.desc.cbv.BufferLocation, resource);
        if (!resource || !resource->GetBufferAllocation())
          continue;

        // Read up to sample_limit from the UNDERLYING RESOURCE, not capped by the bound
        // CBV SizeInBytes. Shaders can DYNAMICALLY INDEX a mapped cbuffer beyond the
        // small bound view (the FH4 homepage CG composite binds a 256B CBV but reads a
        // transform palette at element 68 = +1088 bytes); to see the value the GPU
        // actually reads we must read the resource range past the bound view.
        const auto resource_width = resource->GetResourceDesc().Width;
        const auto remaining = resource_width > resource_offset
                                   ? resource_width - resource_offset
                                   : UINT64{0};
        const auto size = std::min<UINT64>(sample_limit, remaining);
        if (!size)
          continue;
        const auto resource_object_id =
            record_snapshot
                ? dxmt::apitrace::object_id(resource->GetD3D12Resource())
                : 0;

        const auto staging =
            AllocateDiagnosticReadbackStaging(device_->GetMTLDevice(), size);
        if (!staging.buffer || !staging.mapped)
          continue;

        Rc<BufferAllocation> allocation = resource->GetBufferAllocation();
        const auto src_offset = resource->GetHeapOffset() + resource_offset;
        EncodeDiagnosticReadbackBlit(chunk, allocation, staging.buffer,
                                     src_offset, size);
        EnqueueRetirement(
            chunk,
            ConstantBufferReadbackRetirementWork{
                .buffer =
                    DiagnosticReadbackBuffer(staging.buffer, staging.mapped),
                .key_prefix = key_prefix,
                .kind = kind,
                .d3d_sequence = d3d_sequence,
                .root_index = root_index,
                .slot = slot,
                .stage = uint32_t(stage),
                .address = descriptor.desc.cbv.BufferLocation,
                .declared_size = descriptor.desc.cbv.SizeInBytes,
                .resource_offset = resource_offset,
                .heap_offset = resource->GetHeapOffset(),
                .size = size,
                .log_readback = log_readback,
                .record_snapshot = record_snapshot,
                .resource_object_id = resource_object_id,
                .draw_record_sequence = draw_record_sequence,
            });
      }
    }
    }

