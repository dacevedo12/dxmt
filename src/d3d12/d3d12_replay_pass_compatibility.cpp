#include "d3d12_replay_pass_compatibility.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

bool
ReplayRenderPassAttachmentsMatch(const ReplayRenderPassAttachments &lhs,
                                 const ReplayRenderPassAttachments &rhs) {
  if (lhs.colors.size() != rhs.colors.size())
    return false;
  for (size_t i = 0; i < lhs.colors.size(); i++) {
    const auto &a = lhs.colors[i];
    const auto &b = rhs.colors[i];
    if (a.slot != b.slot || a.array_length != b.array_length ||
        a.depth_plane != b.depth_plane || a.width != b.width ||
        a.height != b.height || a.format != b.format ||
        a.texture.ptr() != b.texture.ptr() ||
        uint64_t(a.view) != uint64_t(b.view))
      return false;
  }
  if (lhs.depth_stencil.has_value() != rhs.depth_stencil.has_value())
    return false;
  if (lhs.depth_stencil) {
    const auto &a = *lhs.depth_stencil;
    const auto &b = *rhs.depth_stencil;
    if (a.array_length != b.array_length || a.width != b.width ||
        a.height != b.height || a.format != b.format ||
        a.depth_access != b.depth_access ||
        a.stencil_access != b.stencil_access ||
        // Not implied by the accesses above: a writable DSV drawn with
        // DepthWriteMask=ZERO also yields Read, so two attachments can agree on
        // depth_access while disagreeing on whether the binding is read-only.
        // The mask decides whether the batch's pass declares a depth write, so
        // it has to be part of what makes a batch continuable.
        a.dsv_readonly_flags != b.dsv_readonly_flags ||
        a.texture.ptr() != b.texture.ptr() ||
        uint64_t(a.view) != uint64_t(b.view))
      return false;
  }
  return true;
}

bool
TextureSubresourceOverlapsView(const ResourceAccessBarrierEntry &entry,
                               TextureViewKey view) {
  if (!entry.texture)
    return false;
  return entry.level >= view.mip_start && entry.level < view.mip_end &&
         entry.slice >= view.array_start && entry.slice < view.array_end;
}

bool
ResourceBarrierTouchesRenderPassAttachments(
    const ResourceAccessBarrierBatch &batch,
    const ReplayRenderPassAttachments &attachments) {
  if (batch.needs_separator)
    return true;
  for (const auto &entry : batch.entries) {
    if (!entry.texture)
      continue;
    for (const auto &color : attachments.colors) {
      if (entry.texture.ptr() == color.texture.ptr() &&
          TextureSubresourceOverlapsView(entry, color.view))
        return true;
    }
    if (attachments.depth_stencil &&
        entry.texture.ptr() == attachments.depth_stencil->texture.ptr() &&
        TextureSubresourceOverlapsView(entry, attachments.depth_stencil->view))
      return true;
  }
  return false;
}

bool
ReplayGraphicsCommandCanParallelEncode(ReplayGraphicsCommandKind kind,
                                       bool use_geometry,
                                       bool use_tessellation) {
  return (kind == ReplayGraphicsCommandKind::Draw ||
          kind == ReplayGraphicsCommandKind::DrawIndexed) &&
         !use_geometry && !use_tessellation;
}

} // namespace dxmt::d3d12
