#include "d3d12_render_pass_attachments.hpp"

#include "d3d12_descriptor_diagnostics.hpp"
#include "d3d12_pipeline_write_policy.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_replay_stall_probe.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_subresource_geometry.hpp"
#include "d3d12_texture_view.hpp"
#include "dxmt_format.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <utility>

namespace dxmt::d3d12 {

namespace {

// The D3D12 and DXMT bit positions happen to coincide today; map them one by
// one anyway so a future divergence is a compile error rather than a silently
// mislabelled plane.
uint8_t DsvReadOnlyFlags(const DescriptorRecord &descriptor) {
  if (!descriptor.has_desc)
    return 0;
  uint8_t flags = 0;
  if (descriptor.desc.dsv.Flags & D3D12_DSV_FLAG_READ_ONLY_DEPTH)
    flags |= 1;
  if (descriptor.desc.dsv.Flags & D3D12_DSV_FLAG_READ_ONLY_STENCIL)
    flags |= 2;
  return flags;
}

} // namespace

ReplayRenderPassAttachments
BuildRenderPassAttachments(WMT::Device device,
                           std::span<const DescriptorRecord> render_targets,
                           const DescriptorRecord *depth_stencil,
                           const PipelineGraphicsState *graphics) {
  StallScope _ss(StallDiagEnabled(), &stallProbe().attachUs);
  ReplayRenderPassAttachments attachments = {};
  attachments.colors.reserve(render_targets.size());

  for (UINT i = 0; i < render_targets.size(); i++) {
    const auto &descriptor = render_targets[i];
    auto *resource = GetResource(descriptor.resource.ptr());
    if (resource && resource->IsReservedTexture())
      resource->EnsureTextureAllocation("RenderTarget");
    if (!resource || !resource->GetTexture())
      continue;

    auto view = CreateRenderTargetView(device, *resource, descriptor);
    TrackPresentSourceRenderTargetView(*resource, view);
    auto *texture = resource->GetTexture();
    attachments.colors.push_back({
        .texture = texture,
        .view = view,
        .slot = i,
        .array_length = GetRenderTargetArrayLength(*resource, descriptor),
        .depth_plane = GetRenderTargetDepthPlane(descriptor),
        .width = texture->width(view),
        .height = texture->height(view),
        .format = texture->pixelFormat(view),
    });
  }

  if (depth_stencil) {
    auto *resource = GetResource(depth_stencil->resource.ptr());
    if (resource && resource->IsReservedTexture())
      resource->EnsureTextureAllocation("DepthStencil");
    if (resource && resource->GetTexture()) {
      auto view = CreateDepthStencilView(device, *resource, *depth_stencil);
      if (!view)
        D3D12DiagLogDSVReplayDescriptor("BuildRenderPassAttachments empty view",
                                        *resource, *depth_stencil,
                                        TextureViewDescriptor{}, view);
      auto *texture = resource->GetTexture();
      attachments.depth_stencil = ReplayDepthStencilAttachment{
          .texture = texture,
          .view = view,
          .array_length = GetDepthStencilArrayLength(*resource, *depth_stencil),
          .width = texture->width(view),
          .height = texture->height(view),
          .format = texture->pixelFormat(view),
          .depth_access = AccessForDepthStencilPlane(
              *depth_stencil, D3D12_DSV_FLAG_READ_ONLY_DEPTH,
              PipelineWritesDepth(graphics)),
          .stencil_access = AccessForDepthStencilPlane(
              *depth_stencil, D3D12_DSV_FLAG_READ_ONLY_STENCIL,
              PipelineWritesStencil(graphics)),
          .dsv_readonly_flags = DsvReadOnlyFlags(*depth_stencil),
      };
    }
  }

  return attachments;
}

namespace {

// Metal constrains the whole pass to renderTargetWidth/renderTargetHeight.
// Both MTLRenderPassDescriptor and MTL4RenderPassDescriptor document the same
// contract: "Defaults to 0. If non-zero the value must be smaller than or
// equal to the minimum width of all attachments" (SDK MTLRenderPass.h /
// MTL4RenderPass.h). Declaring more than the smallest attachment actually has
// is out of contract, and the value is also what the D3D12 scissor rects are
// clamped against (d3d12_render_encoder_state.cpp), so an oversized value lets
// rasterization run past the end of the smaller attachment.
//
// D3D requires every simultaneously bound view to have identical dimensions
// ("All render targets must have the same size in all dimensions"), so for
// conformant input the minimum equals every attachment's extent and this is a
// no-op; it only changes behaviour for input D3D itself rejects.
void FoldRenderTargetExtent(uint32_t &current, uint32_t candidate) {
  if (!candidate)
    return;
  current = current ? std::min(current, candidate) : candidate;
}

void WarnRenderTargetExtentMismatchOnce(uint32_t width, uint32_t height,
                                        uint32_t attachment_width,
                                        uint32_t attachment_height) {
  if (width == attachment_width && height == attachment_height)
    return;
  static bool warned = false;
  if (warned)
    return;
  warned = true;
  WARN("D3D12RenderPass: attachments disagree on size, constraining the pass "
       "to the smallest one pass=",
       width, "x", height, " attachment=", attachment_width, "x",
       attachment_height);
}

} // namespace

CompiledRenderPassRecipe
BuildCompiledRenderPassRecipe(ReplayRenderPassAttachments attachments,
                              bool use_geometry, bool use_tessellation) {
  CompiledRenderPassRecipe recipe = {};
  recipe.attachments = std::move(attachments);
  recipe.use_geometry = use_geometry;
  recipe.use_tessellation = use_tessellation;
  for (const auto &color : recipe.attachments.colors) {
    recipe.render_target_count =
        std::max(recipe.render_target_count, color.slot + 1);
    if (recipe.width || recipe.height)
      WarnRenderTargetExtentMismatchOnce(recipe.width, recipe.height,
                                         color.width, color.height);
    FoldRenderTargetExtent(recipe.width, color.width);
    FoldRenderTargetExtent(recipe.height, color.height);
    recipe.array_length =
        std::max<uint32_t>(recipe.array_length, color.array_length);
    recipe.sample_count = std::max<uint32_t>(
        recipe.sample_count, color.texture->sampleCount());
  }
  if (recipe.attachments.depth_stencil) {
    const auto &depth_stencil = *recipe.attachments.depth_stencil;
    if (recipe.width || recipe.height)
      WarnRenderTargetExtentMismatchOnce(recipe.width, recipe.height,
                                         depth_stencil.width,
                                         depth_stencil.height);
    FoldRenderTargetExtent(recipe.width, depth_stencil.width);
    FoldRenderTargetExtent(recipe.height, depth_stencil.height);
    recipe.array_length =
        std::max<uint32_t>(recipe.array_length,
                           depth_stencil.array_length);
    recipe.sample_count = std::max<uint32_t>(
        recipe.sample_count, depth_stencil.texture->sampleCount());
    recipe.dsv_format = depth_stencil.format;
  }
  return recipe;
}

bool BeginCompiledRenderPass(ArgumentEncodingContext &enc,
                             CompiledRenderPassRecipe &recipe,
                             uint64_t argument_buffer_size) {
  auto &attachments = recipe.attachments;
  if (attachments.colors.empty() && !attachments.depth_stencil)
    return false;

  // The mask never reaches Metal -- neither MTLRenderPassDescriptor nor its
  // MTL4 counterpart models a read-only depth attachment. It gates the extra
  // ResourceAccess::Write endPass() publishes on the depth/stencil attachment,
  // and it is part of the encoder merge signature. Declaring the write when the
  // DSV is read-only is safe but blocks merging adjacent passes that share the
  // attachment, which is exactly the depth-prepass-then-shade shape.
  auto &info =
      *enc.startRenderPass(DepthStencilPlanarFlags(recipe.dsv_format),
                           attachments.depth_stencil
                               ? attachments.depth_stencil->dsv_readonly_flags
                               : 0,
                           recipe.render_target_count, argument_buffer_size);
  for (auto &rtv : attachments.colors) {
    auto &color = info.colors[rtv.slot];
    color.attachment = enc.access<PipelineStage::Pixel>(
        rtv.texture, rtv.view, ResourceAccess::ReadWrite);
    color.load_action = WMTLoadActionLoad;
    color.store_action = WMTStoreActionStore;
    color.depth_plane = rtv.depth_plane;
    info.tile_barrier_pso_key.color_formats[rtv.slot] = rtv.format;
  }

  if (attachments.depth_stencil) {
    const auto planar_flags =
        DepthStencilPlanarFlags(attachments.depth_stencil->format);
    if (planar_flags & 1) {
      auto &depth = info.depth;
      depth.attachment = enc.access<PipelineStage::Pixel>(
          attachments.depth_stencil->texture, attachments.depth_stencil->view,
          attachments.depth_stencil->depth_access);
      depth.level = 0;
      depth.slice = 0;
      depth.depth_plane = 0;
      depth.load_action = WMTLoadActionLoad;
      depth.store_action = WMTStoreActionStore;
    }
    if (planar_flags & 2) {
      auto &stencil = info.stencil;
      stencil.attachment = enc.access<PipelineStage::Pixel>(
          attachments.depth_stencil->texture, attachments.depth_stencil->view,
          attachments.depth_stencil->stencil_access);
      stencil.level = 0;
      stencil.slice = 0;
      stencil.depth_plane = 0;
      stencil.load_action = WMTLoadActionLoad;
      stencil.store_action = WMTStoreActionStore;
    }
  }

  info.render_target_width = recipe.width;
  info.render_target_height = recipe.height;
  info.render_target_array_length = recipe.array_length;
  info.default_raster_sample_count = recipe.sample_count;
  info.tile_barrier_pso_key.raster_sample_count = recipe.sample_count;
  info.use_geometry = info.use_geometry || recipe.use_geometry;
  info.use_tessellation = info.use_tessellation || recipe.use_tessellation;
  return true;
}

bool BeginRenderPass(ArgumentEncodingContext &enc,
                     ReplayRenderPassAttachments &attachments,
                     uint64_t argument_buffer_size, bool use_geometry,
                     bool use_tessellation) {
  auto recipe = BuildCompiledRenderPassRecipe(
      std::move(attachments), use_geometry, use_tessellation);
  const bool began =
      BeginCompiledRenderPass(enc, recipe, argument_buffer_size);
  attachments = std::move(recipe.attachments);
  return began;
}

} // namespace dxmt::d3d12
