#include "d3d12_replay_draw_packet_ops.hpp"

#include "d3d12_binding_fingerprint.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

void
FinalizeReplayDrawBindingFingerprint(ReplayDrawPacketCommon &common) {
  common.binding_content_fingerprint =
      common.binding_snapshot ? common.binding_snapshot->content_fingerprint
                              : 0;
  HashGraphicsBindingValue(common.binding_content_fingerprint,
                           common.metal_pso.handle);
  HashGraphicsBindingValue(common.binding_content_fingerprint,
                           common.use_geometry);
  HashGraphicsBindingValue(common.binding_content_fingerprint,
                           common.use_tessellation);
  HashGraphicsBindingValue(common.binding_content_fingerprint,
                           common.pixel_shader_demote_msaa_srv_mask_lo);
  HashGraphicsBindingValue(common.binding_content_fingerprint,
                           common.pixel_shader_demote_msaa_srv_mask_hi);
}

void
FinalizeReplayDrawBindingFingerprint(
    ReplayDrawIndexedInstancedPacket &packet) {
  FinalizeReplayDrawBindingFingerprint(packet.common);
  const auto index_buffer = packet.index_allocation->buffer();
  HashGraphicsBindingValue(packet.common.binding_content_fingerprint,
                           index_buffer.handle);
  HashGraphicsBindingValue(packet.common.binding_content_fingerprint,
                           packet.index_binding_offset);
  HashGraphicsBindingValue(packet.common.binding_content_fingerprint,
                           packet.index_type);
}

bool
CompiledAttachmentPayloadSafe(
    const ReplayRenderPassAttachments &attachments) {
  return std::none_of(
      attachments.colors.begin(), attachments.colors.end(),
      [](const ReplayRenderTargetAttachment &attachment) {
        return attachment.texture &&
               attachment.texture->textureType(attachment.view) ==
                   WMTTextureType3D;
      });
}

} // namespace dxmt::d3d12
