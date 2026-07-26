#pragma once

// Packet-local operations on the namespace-level replay draw packets.
//
// Extracted from class CommandQueueImpl (d3d12_command_queue_draw_encode.inc).
// Everything here reads and writes packet / attachment data only; no queue
// instance state is involved.

#include "d3d12_replay_draw_packet_types.hpp"
#include "d3d12_replay_pass_types.hpp"

namespace dxmt::d3d12 {

// Seeds the packet's binding content fingerprint from the binding snapshot and
// mixes in the draw-invariant state that selects a Metal encoder binding
// program: the PSO handle, the geometry/tessellation path bits and the pixel
// shader MSAA SRV demote masks.
void FinalizeReplayDrawBindingFingerprint(ReplayDrawPacketCommon &common);

// Same, plus the index buffer identity (Metal buffer handle, binding offset and
// index type) of an indexed packet.
void FinalizeReplayDrawBindingFingerprint(
    ReplayDrawIndexedInstancedPacket &packet);

// True when every color attachment of the pass can go through the compiled
// direct payload. 3D render targets are excluded: the compiled path does not
// carry the depth plane selection.
[[nodiscard]] bool CompiledAttachmentPayloadSafe(
    const ReplayRenderPassAttachments &attachments);

} // namespace dxmt::d3d12
