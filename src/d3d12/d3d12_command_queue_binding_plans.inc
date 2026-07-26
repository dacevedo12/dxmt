// Bindless mirror and native root base stage plans, recipe cache.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // Plan value types (BindlessMirrorWindow / BindlessMirror*StagePlan* /
  // NativeRootBase*StagePlan*) and the stateless shader-argument queries
  // (IsBindlessTextureMirrorArgument, CountBindlessTextureMirrorFieldPairs,
  // FindBindlessMirrorArgument) now live in d3d12_bindless_mirror_plan.hpp.
  //
  // The binding-recipe diagnostic counters
  // (DescriptorTableBindingRecipeDiagStats, BindingRecipeDiagStats,
  // BindingRecipeDiagNowNs / NsToMs / AvgNs) now live in
  // d3d12_binding_diagnostics.hpp, together with the process-wide
  // bindless-mirror diagnostic singleton and its reporters:
  // BindlessMirrorDiagEnabled, BindlessMirrorDiagShouldLog,
  // BindlessMirrorDiagStats, BindlessMirrorDiagStatsInstance,
  // DumpBindlessMirrorShaderDxbc, LogBindlessMirrorDiagSummary,
  // RecordBindlessMirrorDiagDraw, RecordBindlessMirrorDiagDispatch and
  // AddBindlessMirrorDiagBufTable.

  // LogBindingRecipeDiagSummary, MaybeLogBindingRecipeDiagSummary,
  // DescriptorTableBindingRecipeBlobHeader,
  // kD3D12BindingRecipeCacheVersion,
  // BuildDescriptorTableBindingRecipeCachePath,
  // BuildDescriptorTableBindingRecipeKey,
  // LoadDescriptorTableBindingRecipe and
  // StoreDescriptorTableBindingRecipe now live in
  // d3d12_binding_recipe_cache.hpp.

  // ReflectedDescriptorRangeCount now lives in d3d12_stage_plan_build.hpp.

  // ApplyStaticSamplers itself now lives in d3d12_legacy_binding_encode.hpp;
  // this shim only supplies the Metal device.
  void ApplyStaticSamplers(ArgumentEncodingContext &enc,
                           const PipelineState &pipeline,
                           const RootSignature &root, bool compute) {
    dxmt::d3d12::ApplyStaticSamplers(device_->GetMTLDevice(), enc, pipeline,
                                     root, compute);
  }

  // BuildDescriptorTableBindingRecipe now lives in
  // d3d12_stage_plan_build.hpp.

  // BindlessMirrorDrawDiag, BindlessMirrorDiagPathName,
  // BindlessMirrorDiagShaderDumpDir, BindlessMirrorDiagShaderDumpKey and
  // BindlessMirrorShaderSha1 now live in d3d12_binding_diagnostics.hpp.
  //
  // BindlessMirrorDiagProbe and the BindlessRootOffsetIssue flags also live in
  // d3d12_binding_diagnostics.hpp; DiagnoseBindlessRootOffsetGap,
  // ProbeBindlessMirrorTextureBinding and ProbeBindlessMirrorSamplerBinding in
  // d3d12_bindless_window_probe.hpp. The shims below only supply the current
  // frame sequence (and, for the texture probe, the Metal device) that the log
  // lines report.
  void RecordBindlessMirrorDiagDraw(const PipelineState *pipeline,
                                    const BindlessMirrorDrawDiag &diag) {
    dxmt::d3d12::RecordBindlessMirrorDiagDraw(
        device_->GetDXMTDevice().queue().CurrentFrameSeq(), pipeline, diag);
  }

  void DiagnoseBindlessRootOffsetGap(
      const char *path, const PipelineState *pipeline, PipelineStage stage,
      const DXMT12_MTL4_SHADER_ARGUMENT &arg, uint32_t issue_flags,
      BindlessMirrorDrawDiag *draw_diag) {
    dxmt::d3d12::DiagnoseBindlessRootOffsetGap(
        device_->GetDXMTDevice().queue().CurrentFrameSeq(), path, pipeline,
        stage, arg, issue_flags, draw_diag);
  }

  bool ProbeBindlessMirrorTextureBinding(const BindlessMirrorDiagProbe &probe,
                                         BindlessMirrorDrawDiag *draw_diag) {
    return dxmt::d3d12::ProbeBindlessMirrorTextureBinding(
        device_->GetMTLDevice(),
        device_->GetDXMTDevice().queue().CurrentFrameSeq(), probe, draw_diag);
  }

  bool ProbeBindlessMirrorSamplerBinding(const BindlessMirrorDiagProbe &probe,
                                         BindlessMirrorDrawDiag *draw_diag) {
    return dxmt::d3d12::ProbeBindlessMirrorSamplerBinding(
        device_->GetDXMTDevice().queue().CurrentFrameSeq(), probe, draw_diag);
  }

  // BuildBindlessMirrorStagePlan and BuildNativeRootBaseStagePlan now live in
  // d3d12_stage_plan_build.hpp, and the lock/lookup/validate/evict shell around
  // them in d3d12_stage_plan_cache.hpp. Only the queue-owned cache storage is
  // still reached through `this`, so these two shims are all that is left.
  std::shared_ptr<const BindlessMirrorStagePlan>
  GetBindlessMirrorStagePlan(const PipelineState &pipeline,
                             const RootSignature &root,
                             PipelineStage want_stage, bool compute) {
    return dxmt::d3d12::GetBindlessMirrorStagePlan(
        bindless_stage_plan_cache_, pipeline, root, want_stage, compute);
  }

  std::shared_ptr<const NativeRootBaseStagePlan>
  GetNativeRootBaseStagePlan(const PipelineState &pipeline,
                             const RootSignature &root,
                             PipelineStage want_stage, bool compute) {
    return dxmt::d3d12::GetNativeRootBaseStagePlan(
        native_stage_plan_cache_, pipeline, root, want_stage, compute);
  }
