// ExecuteIndirect direct and CPU fallback replay paths.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  void ReplayExecuteIndirect(CommandChunk *chunk, ReplayState &state,
                             const ExecuteIndirectRecord &record) {
    if (!PredicationAllows(chunk, state))
      return;

    auto *signature =
        dynamic_cast<CommandSignature *>(record.command_signature.ptr());
    if (!signature) {
      WARN("D3D12CommandQueue: ExecuteIndirect skipped for foreign command signature");
      return;
    }

    const auto &desc = signature->GetDesc();
    const auto &arguments = signature->GetArguments();
    const auto plan = PlanExecuteIndirectDispatch(*signature);
    if (!plan.valid)
      return;

    if (plan.direct_operation != DirectIndirectOperation::None &&
        ReplayExecuteIndirectDirect(chunk, state, record, desc, arguments[0],
                                    plan.direct_operation))
      return;

    FlushPassBatches(chunk, state);
    ReplayExecuteIndirectCpuFallback(chunk, state, record, *signature);
  }

  bool ReplayExecuteIndirectDirect(
      CommandChunk *chunk, ReplayState &state,
      const ExecuteIndirectRecord &record,
      const D3D12_COMMAND_SIGNATURE_DESC &desc,
      const D3D12_INDIRECT_ARGUMENT_DESC &argument,
      DirectIndirectOperation operation) {
    const UINT command_count = record.max_command_count;
    if (!command_count)
      return true;

    const auto layout = ComputeIndirectArgumentStreamLayout(
        argument, desc.ByteStride, command_count);
    if (!layout.valid)
      return true;
    const UINT argument_size = layout.argument_size;

    auto *arg_resource = GetResource(record.arg_buffer.ptr());
    if (!ValidateBufferRange(arg_resource, record.arg_buffer_offset,
                             layout.source_length, "indirect argument buffer"))
      return true;

    Rc<Buffer> arg_buffer = arg_resource->GetBuffer();
    const UINT64 arg_base_offset =
        arg_resource->GetHeapOffset() + record.arg_buffer_offset;
    const UINT64 arg_heap_offset = arg_resource->GetHeapOffset();

    Rc<Buffer> count_buffer;
    UINT64 count_offset = 0;
    UINT64 count_heap_offset = 0;
    Rc<Buffer> counted_args;
    if (record.count_buffer) {
      auto *count_resource = GetResource(record.count_buffer.ptr());
      if (!ValidateBufferRange(count_resource, record.count_buffer_offset,
                               sizeof(UINT), "indirect count buffer"))
        return true;

      count_buffer = count_resource->GetBuffer();
      count_heap_offset = count_resource->GetHeapOffset();
      count_offset =
          count_resource->GetHeapOffset() + record.count_buffer_offset;

      counted_args = new Buffer(layout.counted_length,
                                device_->GetDXMTDevice().device());
      auto counted_allocation = counted_args->allocate(
          BufferAllocationFlag::CpuInvisible |
          BufferAllocationFlag::GpuPrivate);
      if (!counted_allocation) {
        WARN("D3D12CommandQueue: ExecuteIndirect failed to allocate counted argument buffer");
        return true;
      }
      counted_args->rename(std::move(counted_allocation));
    }

    LogExecuteIndirectDirect(operation, command_count, desc.ByteStride,
                             argument_size, record.arg_buffer.ptr(),
                             arg_heap_offset, record.arg_buffer_offset,
                             arg_base_offset, record.count_buffer.ptr(),
                             count_heap_offset, record.count_buffer_offset,
                             count_offset);

    // Expand a counted ExecuteIndirect into a private argument stream once.
    // The old path prepared one command at a time and wrapped every indirect
    // draw in its own compute + render pass. Besides the severe encoder churn,
    // those artificial render-pass boundaries can discard tile-local
    // attachment contents on TBDR hardware. Once the count predicate has been
    // applied, the resulting zeroed commands are normal indirect arguments and
    // can use the existing graphics/compute pass batching paths.
    const bool prepared_counted_stream = bool(count_buffer);
    if (prepared_counted_stream) {
      FlushPassBatches(chunk, state);
      const UINT64 source_length = layout.source_length;
      const UINT64 destination_length = layout.counted_length;
      chunk->emitcc(
          [arg_buffer, arg_base_offset, source_length, count_buffer,
           count_offset, counted_args, destination_length, command_count,
           stride = desc.ByteStride,
           argument_size](ArgumentEncodingContext &enc) mutable {
            ExpandCountedIndirectArgumentStream(
                enc, arg_buffer, arg_base_offset, source_length, count_buffer,
                count_offset, counted_args, destination_length, command_count,
                stride, argument_size);
          });
      arg_buffer = counted_args;
      count_buffer = nullptr;
      counted_args = nullptr;
    }

    for (UINT command_index = 0; command_index < command_count;
         command_index++) {
      const UINT64 arg_offset = IndirectCommandArgumentOffset(
          prepared_counted_stream, arg_base_offset, command_index,
          desc.ByteStride, argument_size);
      const UINT64 counted_offset = UINT64(command_index) * argument_size;

      switch (operation) {
      case DirectIndirectOperation::Draw:
        ReplayDrawInstancedIndirect(chunk, state, arg_buffer, arg_offset,
                                    count_buffer, count_offset, counted_args,
                                    counted_offset, argument_size,
                                    command_index);
        break;
      case DirectIndirectOperation::DrawIndexed:
        ReplayDrawIndexedInstancedIndirect(
            chunk, state, arg_buffer, arg_offset, count_buffer, count_offset,
            counted_args, counted_offset, argument_size, command_index);
        break;
      case DirectIndirectOperation::Dispatch:
        FlushGraphicsBeforeCompute(chunk, state);
        ReplayDispatchIndirect(chunk, state, arg_buffer, arg_offset,
                               count_buffer, count_offset, counted_args,
                               counted_offset, argument_size, command_index);
        break;
      default:
        return false;
      }
    }
    return true;
  }

  void ReplayExecuteIndirectCpuFallback(CommandChunk *chunk, ReplayState &state,
                                        const ExecuteIndirectRecord &record,
                                        const CommandSignature &signature) {
    UINT command_count = record.max_command_count;
    if (record.count_buffer) {
      MaterializeTimestampResolvesForCpuRead(
          chunk, state, record.count_buffer.ptr(), record.count_buffer_offset,
          sizeof(UINT));
      UINT count = 0;
      if (!ReadBufferBytes(record.count_buffer.ptr(), record.count_buffer_offset,
                           &count, sizeof(count), "indirect count buffer")) {
        WARN("D3D12CommandQueue: ExecuteIndirect complex command signature requires CPU-visible buffers until GPU state-command lowering is implemented");
        return;
      }
      command_count = std::min(command_count, count);
    }
    if (!command_count)
      return;

    const auto &desc = signature.GetDesc();
    const auto &arguments = signature.GetArguments();

    std::vector<uint8_t> command(desc.ByteStride);
    for (UINT command_index = 0; command_index < command_count;
         command_index++) {
      const UINT64 command_offset =
          record.arg_buffer_offset + UINT64(command_index) * desc.ByteStride;
      MaterializeTimestampResolvesForCpuRead(
          chunk, state, record.arg_buffer.ptr(), command_offset,
          command.size());
      if (!ReadBufferBytes(record.arg_buffer.ptr(), command_offset,
                           command.data(), command.size(),
                           "indirect argument buffer")) {
        WARN("D3D12CommandQueue: ExecuteIndirect complex command signature requires CPU-visible buffers until GPU state-command lowering is implemented");
        return;
      }

      size_t argument_offset = 0;
      for (const auto &argument : arguments) {
        const auto span = LocateIndirectArgument(
            command.data(), command.size(), argument_offset, argument);
        if (!span.valid)
          return;

        const auto *bytes = span.bytes;
        const bool compute = CurrentPipelineIsCompute(state);
        switch (argument.Type) {
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW:
          ReplayDrawInstanced(chunk, state, DecodeIndirectDrawArguments(bytes));
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED:
          ReplayDrawIndexedInstanced(chunk, state,
                                     DecodeIndirectDrawIndexedArguments(bytes));
          break;
        case D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH:
          ReplayDispatch(chunk, state, DecodeIndirectDispatchArguments(bytes));
          break;
        default:
          // Everything that only mutates the replay state - vertex/index
          // views and the root arguments - lowers without the queue.
          if (!ApplyIndirectStateArgument(state, argument, bytes, compute))
            return;
          break;
        }
        argument_offset += span.size;
      }
    }
  }

  void ReplayDrawInstancedIndirect(
      CommandChunk *chunk, ReplayState &state, Rc<Buffer> arg_buffer,
      UINT64 arg_offset, Rc<Buffer> count_buffer, UINT64 count_offset,
      Rc<Buffer> counted_args, UINT64 counted_offset,
      UINT argument_size, UINT command_index) {
    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: indirect draw skipped without graphics pipeline state");
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
      WARN("D3D12CommandQueue: indirect draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    const auto metal_pso = SelectGraphicsPipelineState(*metal, state.topology);
    if (!metal_pso) {
      WARN("D3D12CommandQueue: indirect draw skipped because selected Metal graphics PSO is unavailable");
      return;
    }
    const auto lowering =
        LowerIndirectDrawTopology(*metal, state.topology, false);
    if (!lowering.ok)
      return;
    auto primitive = lowering.primitive;
    auto geometry_counts = lowering.geometry_counts;
    auto control_point_count = lowering.control_point_count;
    auto viewports = state.viewports;
    auto scissors = state.scissors;
      auto attachments = BuildRenderPassAttachments(state,
                                                    pipeline->GetGraphicsState());
      if (!ResolveDynamicRasterRects(viewports, scissors, "indirect draw"))
        return;
      RecordGraphicsPipelineResourceAccess(chunk, state, *pipeline, nullptr);
      const auto argument_buffer_size =
          EstimateGraphicsArgumentBufferSize(*pipeline, metal->use_geometry,
                                             metal->use_tessellation);

    IndirectDrawEncodeParams encode_params = {};
    encode_params.metal_pso = metal_pso;
    encode_params.use_geometry = metal->use_geometry;
    encode_params.use_tessellation = metal->use_tessellation;
    encode_params.demote_msaa_srv_mask_lo = demote_msaa_srv_mask_lo;
    encode_params.demote_msaa_srv_mask_hi = demote_msaa_srv_mask_hi;
    encode_params.depth_stencil = metal->depth_stencil;
    encode_params.rasterizer = metal->rasterizer;
    encode_params.tess_threads_per_patch = metal->tess_threads_per_patch;
    encode_params.tess_num_output_control_point_element =
        metal->tess_num_output_control_point_element;
    encode_params.pipeline = pipeline;
    encode_params.replay_state = CloneReplayStateWithoutBatch(state);
    encode_params.primitive = primitive;
    encode_params.geometry_counts = geometry_counts;
    encode_params.control_point_count = control_point_count;
    encode_params.blend_factor = state.blend_factor;
    encode_params.stencil_ref = state.stencil_ref;
    encode_params.arg_buffer = arg_buffer;
    encode_params.arg_offset = arg_offset;
    encode_params.count_buffer = count_buffer;
    encode_params.counted_args = counted_args;
    encode_params.counted_offset = counted_offset;
    encode_params.argument_size = argument_size;
    encode_params.max_object_threadgroups =
        device_->GetDXMTDevice().maxObjectThreadgroups();
    encode_params.viewports = std::move(viewports);
    encode_params.scissors = std::move(scissors);

    auto encode_draw = [ctx = BindingContext(),
                        params = std::move(encode_params)](
                           ArgumentEncodingContext &enc,
                           uint64_t &argbuf_offset) mutable {
      EncodeIndirectDraw(ctx, enc, params, argbuf_offset);
    };

    const bool has_count = count_buffer.ptr();
    if (D3D12ReplayGraphicsBatchingEnabled() && !has_count) {
      QueueGraphicsPassCommand(chunk, state, std::move(attachments),
                               argument_buffer_size, std::move(encode_draw),
                               metal->use_geometry, metal->use_tessellation,
                               ReplayGraphicsCommandKind::DrawIndirect);
      return;
    }

    FlushPassBatches(chunk, state);
    chunk->emitcc([attachments = std::move(attachments),
                   argument_buffer_size, arg_buffer, arg_offset, count_buffer,
                   count_offset, counted_args, counted_offset, argument_size,
                   command_index, use_geometry = metal->use_geometry,
                   use_tessellation = metal->use_tessellation,
                   encode = std::move(encode_draw)](
                      ArgumentEncodingContext &enc) mutable {
      if (count_buffer) {
        PrepareCountedIndirectArguments(
            enc, arg_buffer, arg_offset, count_buffer, count_offset,
            counted_args, counted_offset, argument_size, command_index);
      }

      if (!BeginRenderPass(enc, attachments, argument_buffer_size,
                           use_geometry, use_tessellation))
        return;
      uint64_t argbuf_offset = 0;
      enc.setArgumentBufferWriteRange(0, argument_buffer_size);
      enc.bumpVisibilityResultOffset();
      encode(enc, argbuf_offset);
      enc.endPass();
    });
  }

  void ReplayDrawIndexedInstancedIndirect(
      CommandChunk *chunk, ReplayState &state, Rc<Buffer> arg_buffer,
      UINT64 arg_offset, Rc<Buffer> count_buffer, UINT64 count_offset,
      Rc<Buffer> counted_args, UINT64 counted_offset,
      UINT argument_size, UINT command_index) {
    if (!state.index_buffer)
      return;

    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped without graphics pipeline state");
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
      WARN("D3D12CommandQueue: indirect indexed draw skipped because Metal graphics PSO is unavailable");
      return;
    }

    UINT64 index_resource_offset = 0;
    auto *index_resource = LookupBufferResourceByGpuVirtualAddress(
        state.index_buffer->BufferLocation, &index_resource_offset);
    if (!index_resource || !index_resource->GetBufferAllocation()) {
      static std::atomic_flag warned_missing_indirect_index_buffer =
          ATOMIC_FLAG_INIT;
      if (!warned_missing_indirect_index_buffer.test_and_set(
              std::memory_order_relaxed))
        WARN("D3D12CommandQueue: indirect indexed draw skipped because index "
             "buffer binding is unavailable (further warnings suppressed)");
      return;
    }
    if (!IsSupportedIndexBufferFormat(state.index_buffer->Format)) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped because index buffer format is unsupported format=",
           uint32_t(state.index_buffer->Format));
      return;
    }

    Rc<BufferAllocation> index_allocation = index_resource->GetBufferAllocation();
    const auto metal_pso = SelectGraphicsPipelineState(
        *metal, state.topology, state.index_buffer->Format);
    if (!metal_pso) {
      WARN("D3D12CommandQueue: indirect indexed draw skipped because selected Metal graphics PSO is unavailable");
      return;
    }
    const auto lowering =
        LowerIndirectDrawTopology(*metal, state.topology, true);
    if (!lowering.ok)
      return;
    auto primitive = lowering.primitive;
    auto geometry_counts = lowering.geometry_counts;
    auto control_point_count = lowering.control_point_count;
    const auto index_type = GetIndexType(state.index_buffer->Format);
    const UINT64 index_offset =
        index_resource->GetHeapOffset() + index_resource_offset;
      auto attachments = BuildRenderPassAttachments(state,
                                                    pipeline->GetGraphicsState());
      auto viewports = state.viewports;
      auto scissors = state.scissors;
      if (!ResolveDynamicRasterRects(viewports, scissors, "indirect indexed draw"))
        return;
      RecordGraphicsPipelineResourceAccess(chunk, state, *pipeline, index_resource);
      const auto argument_buffer_size =
          EstimateGraphicsArgumentBufferSize(*pipeline, metal->use_geometry,
                                             metal->use_tessellation);

    IndirectDrawEncodeParams encode_params = {};
    encode_params.metal_pso = metal_pso;
    encode_params.use_geometry = metal->use_geometry;
    encode_params.use_tessellation = metal->use_tessellation;
    encode_params.demote_msaa_srv_mask_lo = demote_msaa_srv_mask_lo;
    encode_params.demote_msaa_srv_mask_hi = demote_msaa_srv_mask_hi;
    encode_params.depth_stencil = metal->depth_stencil;
    encode_params.rasterizer = metal->rasterizer;
    encode_params.tess_threads_per_patch = metal->tess_threads_per_patch;
    encode_params.tess_num_output_control_point_element =
        metal->tess_num_output_control_point_element;
    encode_params.pipeline = pipeline;
    encode_params.replay_state = CloneReplayStateWithoutBatch(state);
    encode_params.index_allocation = index_allocation;
    encode_params.primitive = primitive;
    encode_params.geometry_counts = geometry_counts;
    encode_params.control_point_count = control_point_count;
    encode_params.index_type = index_type;
    encode_params.index_offset = index_offset;
    encode_params.blend_factor = state.blend_factor;
    encode_params.stencil_ref = state.stencil_ref;
    encode_params.arg_buffer = arg_buffer;
    encode_params.arg_offset = arg_offset;
    encode_params.count_buffer = count_buffer;
    encode_params.counted_args = counted_args;
    encode_params.counted_offset = counted_offset;
    encode_params.argument_size = argument_size;
    encode_params.max_object_threadgroups =
        device_->GetDXMTDevice().maxObjectThreadgroups();
    encode_params.viewports = std::move(viewports);
    encode_params.scissors = std::move(scissors);

    auto encode_draw = [ctx = BindingContext(),
                        params = std::move(encode_params)](
                           ArgumentEncodingContext &enc,
                           uint64_t &argbuf_offset) mutable {
      EncodeIndirectDrawIndexed(ctx, enc, params, argbuf_offset);
    };

    const bool has_count = count_buffer.ptr();
    if (D3D12ReplayGraphicsBatchingEnabled() && !has_count) {
      QueueGraphicsPassCommand(chunk, state, std::move(attachments),
                               argument_buffer_size, std::move(encode_draw),
                               metal->use_geometry, metal->use_tessellation,
                               ReplayGraphicsCommandKind::DrawIndexedIndirect);
      return;
    }

    FlushPassBatches(chunk, state);
    chunk->emitcc([attachments = std::move(attachments),
                   argument_buffer_size, arg_buffer, arg_offset, count_buffer,
                   count_offset, counted_args, counted_offset, argument_size,
                   command_index, use_geometry = metal->use_geometry,
                   use_tessellation = metal->use_tessellation,
                   encode = std::move(encode_draw)](
                      ArgumentEncodingContext &enc) mutable {
      if (count_buffer) {
        PrepareCountedIndirectArguments(
            enc, arg_buffer, arg_offset, count_buffer, count_offset,
            counted_args, counted_offset, argument_size, command_index);
      }

      if (!BeginRenderPass(enc, attachments, argument_buffer_size,
                           use_geometry, use_tessellation))
        return;
      uint64_t argbuf_offset = 0;
      enc.setArgumentBufferWriteRange(0, argument_buffer_size);
      enc.bumpVisibilityResultOffset();
      encode(enc, argbuf_offset);
      enc.endPass();
    });
  }

  void ReplayDispatchIndirect(
      CommandChunk *chunk, ReplayState &state, Rc<Buffer> arg_buffer,
      UINT64 arg_offset, Rc<Buffer> count_buffer, UINT64 count_offset,
      Rc<Buffer> counted_args, UINT64 counted_offset,
      UINT argument_size, UINT command_index) {
    auto *pipeline = GetPipelineState(state.pipeline_state.ptr());
    if (!pipeline) {
      WARN("D3D12CommandQueue: indirect dispatch skipped without compute pipeline state");
      return;
    }

      auto *metal = [&]{ StallScope _ss(StallDiagEnabled(), &stallProbe().psoSelectUs); return pipeline->GetMetalComputeState(); }();
      if (!metal || !metal->pso) {
        WARN("D3D12CommandQueue: indirect dispatch skipped because Metal compute PSO is unavailable");
        return;
      }

      RecordComputePipelineResourceAccess(chunk, state, *pipeline);
      const auto argument_buffer_size = EstimateComputeArgumentBufferSize(*pipeline);

      IndirectDispatchEncodeParams encode_params = {};
      encode_params.metal_pso = metal->pso;
      encode_params.threadgroup_size = metal->threadgroup_size;
      encode_params.pipeline = pipeline;
      encode_params.replay_state = CloneReplayStateWithoutBatch(state);
      encode_params.argument_buffer_size = argument_buffer_size;
      encode_params.arg_buffer = arg_buffer;
      encode_params.arg_offset = arg_offset;
      encode_params.count_buffer = count_buffer;
      encode_params.counted_args = counted_args;
      encode_params.counted_offset = counted_offset;
      encode_params.argument_size = argument_size;
      encode_params.command_index = command_index;

      auto encode_dispatch = [ctx = BindingContext(),
                              params = std::move(encode_params)](
                                 ArgumentEncodingContext &enc,
                                 uint64_t &argbuf_offset) mutable {
        EncodeIndirectDispatch(ctx, enc, params, argbuf_offset);
      };

    const bool has_count = count_buffer.ptr();
    if (!has_count) {
      QueueComputePassCommand(chunk, state, argument_buffer_size,
                              std::move(encode_dispatch));
      return;
    }

    FlushPassBatches(chunk, state);
    chunk->emitcc([argument_buffer_size, arg_buffer, arg_offset,
                   count_buffer, count_offset, counted_args, counted_offset,
                   argument_size, command_index,
                   encode = std::move(encode_dispatch)](
                      ArgumentEncodingContext &enc) mutable {
      PrepareCountedIndirectArguments(
          enc, arg_buffer, arg_offset, count_buffer, count_offset,
          counted_args, counted_offset, argument_size, command_index);
      enc.startComputePass(argument_buffer_size);
      uint64_t argbuf_offset = 0;
      enc.setArgumentBufferWriteRange(0, argument_buffer_size);
      encode(enc, argbuf_offset);
      enc.endPass();
    });
  }

