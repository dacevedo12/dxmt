// Frozen native descriptor tables and native root table bases.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // CopyFrozenNativeDescriptorSlot and CopyFrozenNativeRootDescriptorSlot now
  // live in d3d12_frozen_native_descriptor.hpp.

  // The body lives in d3d12_native_stage_binding.hpp; the memoized stage plan
  // is the only thing it ever needed from the command queue.
  template <typename Packet>
  void BuildFrozenNativeStageBinding(
      const Packet &packet, PipelineState &pipeline, RootSignature &root,
      PipelineStage stage, bool compute,
      SubmittedFrozenNativeDescriptorStore &store,
      CompiledNativeStageBinding &out,
      GraphicsBindingSnapshot *snapshot = nullptr) {
    const auto plan =
        GetNativeRootBaseStagePlan(pipeline, root, stage, compute);
    dxmt::d3d12::BuildFrozenNativeStageBinding(packet, pipeline, root,
                                               plan.get(), stage, store, out,
                                               snapshot);
  }

  // TrackNativeDescriptorAccess lives in d3d12_native_access_track.hpp and the
  // stage walker below it in d3d12_native_stage_binding.hpp; this shim supplies
  // the Metal device handle and the memoized stage plan.
  template <PipelineStage Stage, typename State>
  void TrackNativeStageDescriptorAccesses(
      ArgumentEncodingContext &enc, const State &state,
      const PipelineState &pipeline, const RootSignature &root, bool compute) {
    const auto plan =
        GetNativeRootBaseStagePlan(pipeline, root, Stage, compute);
    dxmt::d3d12::TrackNativeStageDescriptorAccesses<Stage>(
        device_->GetMTLDevice(), enc, state, pipeline, plan.get(), compute);
  }

  // DescriptorViewDimension now lives in d3d12_descriptor_record_query.hpp.

  // The body lives in d3d12_native_stage_descriptor_diag.hpp; the memoized
  // stage plan is the only thing it ever needed from the command queue. The
  // diagnostic gate is repeated here so the plan lookup stays off the hot path
  // when dense correctness checking is disabled.
  void DiagnoseCompiledNativeStageDescriptors(
      const std::vector<CompiledCommandRootDescriptorTable> &tables,
      const PipelineState &pipeline, const RootSignature &root,
      PipelineStage stage, bool compute, const char *kind, uint64_t frame,
      uint64_t sequence, uint64_t record_serial, obj_handle_t metal_pso) {
    if (!D3D12DiagCorrectnessDenseEnabled())
      return;

    const auto plan =
        GetNativeRootBaseStagePlan(pipeline, root, stage, compute);
    dxmt::d3d12::DiagnoseCompiledNativeStageDescriptors(
        tables, pipeline, plan.get(), stage, kind, frame, sequence,
        record_serial, metal_pso);
  }

  // The body lives in d3d12_native_stage_binding.hpp; the memoized stage plan
  // is the only thing it ever needed from the command queue.
  template <typename State>
  std::vector<uint32_t>
  BuildNativeRootTableBaseWords(const State &state,
                                const PipelineState &pipeline,
                                const RootSignature &root,
                                PipelineStage want_stage, bool compute,
                                bool cbuffer) {
    const auto plan =
        GetNativeRootBaseStagePlan(pipeline, root, want_stage, compute);
    return dxmt::d3d12::BuildNativeRootTableBaseWords(state, *plan, compute,
                                                      cbuffer);
  }

  // The upload body lives in d3d12_argument_upload.hpp; the queue instance is
  // the only thing it ever needed from the command queue.
  AllocatedArgumentBufferSlice UploadNativeRootTableBases(
      ArgumentEncodingContext &enc, const std::vector<uint32_t> &root_bases) {
    return dxmt::d3d12::UploadNativeRootTableBases(
        device_->GetDXMTDevice().queue(), enc, root_bases);
  }

  // The body lives in d3d12_native_stage_binding.hpp; the memoized stage plan
  // and the argument-buffer ring owner are the only things it ever needed from
  // the command queue.
  template <typename State>
  AllocatedArgumentBufferSlice
  BuildNativeRootTableBases(ArgumentEncodingContext &enc, const State &state,
                            const PipelineState &pipeline,
                            const RootSignature &root,
                            PipelineStage want_stage, bool compute,
                            bool cbuffer) {
    const auto plan =
        GetNativeRootBaseStagePlan(pipeline, root, want_stage, compute);
    return dxmt::d3d12::BuildNativeRootTableBases(
        device_->GetDXMTDevice().queue(), enc, state, *plan, compute, cbuffer);
  }

  // BuildBindlessRootOffsets now lives in d3d12_bindless_root_offsets.hpp;
  // the argument-buffer ring owner, the Metal device and the memoized
  // bindless stage plan are the only things it ever needed from the command
  // queue, and they travel together in a SubmissionBindingContext now.

  // GetDescriptorTableBindingRecipe now lives in
  // d3d12_table_recipe_lookup.hpp; ApplyDescriptorTableBindingRecipe in
  // d3d12_table_recipe_apply.hpp.

  // The body lives in d3d12_root_parameter_apply.hpp; the Metal device handle
  // and the argument-buffer ring owner are the only things it ever needed from
  // the command queue.
  template <typename State>
  void ApplyRootDescriptorTables(ArgumentEncodingContext &enc,
                                 const State &state,
                                 const PipelineState &pipeline, bool compute) {
    dxmt::d3d12::ApplyRootDescriptorTables(device_->GetMTLDevice(),
                                           device_->GetDXMTDevice().queue(),
                                           enc, state, pipeline, compute);
  }
