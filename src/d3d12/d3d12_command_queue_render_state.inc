// Render pass recipes, dynamic raster state, and input slot masks.
// Included inside class CommandQueueImpl; not a standalone translation unit.
private:
  // BuildCompiledRenderPassRecipe(), BeginCompiledRenderPass() and
  // BeginRenderPass() now live in d3d12_render_pass_attachments.hpp, and
  // EstimateDrawArgumentBufferSize() in d3d12_argument_buffer_layout.hpp;
  // unqualified calls resolve to dxmt::d3d12.
  //
  // BuildRenderPassAttachments() lives there too. Only this shim is left: it
  // supplies the Metal device the Metal views are created from, and flattens
  // the draw state — whose render target container differs between the replay
  // and compiled paths — into a span plus an optional depth stencil pointer.
  template <typename State>
  ReplayRenderPassAttachments BuildRenderPassAttachments(
      const State &state,
      const PipelineGraphicsState *graphics = nullptr) {
    return dxmt::d3d12::BuildRenderPassAttachments(
        device_->GetMTLDevice(),
        std::span<const DescriptorRecord>{state.render_targets.data(),
                                          state.render_targets.size()},
        state.depth_stencil ? &*state.depth_stencil : nullptr, graphics);
  }

  // ResolveDynamicRasterRects() and InputSlotMask() now live in
  // d3d12_render_state.hpp; unqualified calls below resolve to dxmt::d3d12.
