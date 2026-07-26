#include "d3d12_compiled_packet_admission.hpp"

#include "d3d12_compiled_packet_resolve.hpp"

namespace dxmt::d3d12 {

bool AdmitCompiledGraphicsPacket(
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const ExecuteIndirectRecord *indirect,
    DirectIndirectOperation &indirect_operation,
    const D3D12_COMMAND_SIGNATURE_DESC *&indirect_desc,
    const D3D12_INDIRECT_ARGUMENT_DESC *&indirect_argument,
    CompiledCommandFallbackReason &reason) {
  if (CompiledPacketTargetsTexture3DRenderTarget(packet)) {
    reason = CompiledCommandFallbackReason::UnsupportedRenderTargetState;
    return false;
  }
  if (packet.compatibility_reason !=
      CompiledCommandFallbackReason::None) {
    reason = packet.compatibility_reason;
    return false;
  }
  if (prepared.prepare_reason != CompiledCommandFallbackReason::None) {
    reason = prepared.prepare_reason;
    return false;
  }
  if (!prepared.root_tables) {
    reason = CompiledCommandFallbackReason::NativeMissingDescriptorBackend;
    return false;
  }
  if (indirect) {
    if (const auto signature_reason = ResolveCompiledIndirectDrawSignature(
            *indirect, indirect_operation, indirect_desc, indirect_argument);
        signature_reason != CompiledCommandFallbackReason::None) {
      reason = signature_reason;
      return false;
    }
  }
  if (!packet.draw && !packet.draw_indexed && !indirect) {
    reason = CompiledCommandFallbackReason::UnsupportedCommand;
    return false;
  }
  if (packet.draw &&
      (!packet.draw->vertex_count_per_instance ||
       !packet.draw->instance_count)) {
    reason = CompiledCommandFallbackReason::None;
    return false;
  }
  if (packet.draw_indexed &&
      (!packet.draw_indexed->index_count_per_instance ||
       !packet.draw_indexed->instance_count)) {
    reason = CompiledCommandFallbackReason::None;
    return false;
  }
  return true;
}

bool AdmitCompiledComputePacket(
    const CompiledComputePacket &packet,
    const SubmittedCompiledComputePacket &prepared,
    const ExecuteIndirectRecord *indirect,
    const D3D12_COMMAND_SIGNATURE_DESC *&indirect_desc,
    const D3D12_INDIRECT_ARGUMENT_DESC *&indirect_argument,
    CompiledCommandFallbackReason &reason) {
  if (packet.compatibility_reason !=
      CompiledCommandFallbackReason::None) {
    reason = packet.compatibility_reason;
    return false;
  }
  if (prepared.prepare_reason != CompiledCommandFallbackReason::None) {
    reason = prepared.prepare_reason;
    return false;
  }
  if (!prepared.root_tables) {
    reason = CompiledCommandFallbackReason::NativeMissingDescriptorBackend;
    return false;
  }
  if (indirect) {
    auto *signature = dynamic_cast<CommandSignature *>(
        indirect->command_signature.ptr());
    if (!signature ||
        GetDirectIndirectOperation(signature->GetArguments()) !=
            DirectIndirectOperation::Dispatch) {
      reason =
          CompiledCommandFallbackReason::NativeUnsupportedExecuteIndirect;
      return false;
    }
    indirect_desc = &signature->GetDesc();
    indirect_argument = &signature->GetArguments()[0];
  }
  if (!indirect &&
      (!packet.dispatch.x || !packet.dispatch.y ||
       !packet.dispatch.z)) {
    reason = CompiledCommandFallbackReason::None;
    return false;
  }
  return true;
}

} // namespace dxmt::d3d12
