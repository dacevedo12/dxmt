#include "d3d12_compiled_packet_resolve.hpp"

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_queue_diagnostics_env.hpp"

#include "log/log.hpp"

#include <algorithm>
#include <climits>
#include <variant>

namespace dxmt::d3d12 {

bool CompiledPacketTargetsTexture3DRenderTarget(
    const CompiledGraphicsPacket &packet) {
  return std::any_of(
      packet.render_state.render_targets.begin(),
      packet.render_state.render_targets.end(),
      [](const DescriptorRecord &descriptor) {
        return descriptor.has_desc &&
               descriptor.desc.rtv.ViewDimension ==
                   D3D12_RTV_DIMENSION_TEXTURE3D;
      });
}

CompiledCommandFallbackReason
ResolveCompiledIndirectDrawSignature(
    const ExecuteIndirectRecord &indirect,
    DirectIndirectOperation &indirect_operation,
    const D3D12_COMMAND_SIGNATURE_DESC *&indirect_desc,
    const D3D12_INDIRECT_ARGUMENT_DESC *&indirect_argument) {
  auto *signature = dynamic_cast<CommandSignature *>(
      indirect.command_signature.ptr());
  if (!signature)
    return CompiledCommandFallbackReason::
        NativeUnsupportedExecuteIndirect;
  const auto &arguments = signature->GetArguments();
  indirect_operation = GetDirectIndirectOperation(arguments);
  if (indirect_operation != DirectIndirectOperation::Draw &&
      indirect_operation != DirectIndirectOperation::DrawIndexed)
    return CompiledCommandFallbackReason::
        NativeUnsupportedExecuteIndirect;
  indirect_desc = &signature->GetDesc();
  indirect_argument = &arguments[0];
  return CompiledCommandFallbackReason::None;
}

CompiledCommandFallbackReason InjectCompiledNativePacketFaults(
    bool native_packet,
    const std::vector<CompiledCommandRootDescriptorTable>
        &submitted_root_tables,
    const char *pipeline_failure_message,
    const char *descriptor_failure_message) {
  constexpr const char *pipeline_fault =
      "DXMT_TEST_FAIL_NATIVE_PIPELINE_COMPILATION_AT";
  if (native_packet && ShouldInjectQueueReplayFault(
                           pipeline_fault,
                           g_test_native_pipeline_compilation_occurrence)) {
    RecordQueueReplayFault(pipeline_fault);
    WARN(pipeline_failure_message);
    return CompiledCommandFallbackReason::
        InjectedNativePipelineCompilationFailure;
  }
  constexpr const char *descriptor_fault =
      "DXMT_TEST_FAIL_NATIVE_DESCRIPTOR_LOOKUP_AT";
  if (native_packet && !submitted_root_tables.empty() &&
      ShouldInjectQueueReplayFault(
          descriptor_fault,
          g_test_native_descriptor_lookup_occurrence)) {
    RecordQueueReplayFault(descriptor_fault);
    WARN(descriptor_failure_message);
    return CompiledCommandFallbackReason::NativeMissingDescriptorBackend;
  }
  return CompiledCommandFallbackReason::None;
}

bool IsCompiledCommandListPlanValid(
    const CompiledCommandList *compiled,
    const SubmittedCompiledCommandListPlan *submitted,
    const std::vector<CommandRecord> &records) {
  if (!compiled || !submitted ||
      (compiled->fallback_records &&
       compiled->record_count != records.size()) ||
      submitted->graphics_packets.size() !=
          compiled->graphics_packets.size() ||
      submitted->compute_packets.size() !=
          compiled->compute_packets.size())
    return false;
  for (UINT node_index = 0;
       node_index < compiled->encoder_graph.nodes.size(); ++node_index) {
    const auto &node = compiled->encoder_graph.nodes[node_index];
    const auto &segment = node.work;
    if (node.predecessor_node !=
            (node_index ? node_index - 1 : UINT_MAX) ||
        node.first_source_record_index > compiled->record_count ||
        node.source_record_count >
            compiled->record_count - node.first_source_record_index)
      return false;
    if (segment.first_record_index > compiled->record_count ||
        segment.record_count >
            compiled->record_count - segment.first_record_index ||
        (!segment.record_count &&
         (segment.graphics_packet_count ||
          segment.compute_packet_count)) ||
        segment.kind == CompiledCommandSegmentKind::ElidedState)
      return false;
    if ((node.encoder_index == UINT_MAX) !=
            (node.encoder_kind == CompiledEncoderKind::None) ||
        (node.encoder_index != UINT_MAX &&
         node.encoder_index >= compiled->encoder_graph.encoder_count) ||
        (node.begins_encoder && node.encoder_index == UINT_MAX))
      return false;
    if (segment.kind == CompiledCommandSegmentKind::Graphics &&
        segment.first_graphics_packet + segment.graphics_packet_count >
            compiled->graphics_packets.size())
      return false;
    if (segment.kind == CompiledCommandSegmentKind::Compute &&
        segment.first_compute_packet + segment.compute_packet_count >
            compiled->compute_packets.size())
      return false;
    if (segment.kind == CompiledCommandSegmentKind::Indirect &&
        segment.first_indirect_packet + segment.indirect_packet_count >
            compiled->indirect_packets.size())
      return false;
    if (segment.kind == CompiledCommandSegmentKind::Indirect) {
      for (UINT i = 0; i < segment.indirect_packet_count; ++i) {
        const auto &packet = compiled->indirect_packets[
            segment.first_indirect_packet + i];
        const auto packet_count =
            packet.compute ? compiled->compute_packets.size()
                           : compiled->graphics_packets.size();
        if (packet.state_packet_index >= packet_count)
          return false;
      }
    }
    if (segment.kind == CompiledCommandSegmentKind::Barrier &&
        segment.first_barrier + segment.barrier_count >
            compiled->access_summary.barriers.size())
      return false;
    if (segment.kind == CompiledCommandSegmentKind::Transfer &&
        segment.first_transfer_packet + segment.transfer_packet_count >
            compiled->transfer_packets.size())
      return false;
    if (segment.kind == CompiledCommandSegmentKind::Control &&
        segment.first_control_packet + segment.control_packet_count >
            compiled->control_packets.size())
      return false;
  }
  return true;
}

bool IndirectPacketCapturesStateRecord(const CommandRecord &record,
                                       bool compute) {
  const auto &payload = record.payload;
  if (std::holds_alternative<PipelineStateRecord>(payload) ||
      std::holds_alternative<DescriptorHeapsRecord>(payload))
    return true;
  if (const auto *root =
          std::get_if<RootSignatureRecord>(&payload))
    return root->compute == compute;
  if (const auto *table =
          std::get_if<RootDescriptorTableRecord>(&payload))
    return table->compute == compute;
  if (const auto *constants =
          std::get_if<RootConstantsRecord>(&payload))
    return constants->compute == compute;
  if (const auto *descriptor =
          std::get_if<RootDescriptorRecord>(&payload))
    return descriptor->compute == compute;
  if (compute)
    return false;
  return std::holds_alternative<ViewportRecord>(payload) ||
         std::holds_alternative<ScissorRecord>(payload) ||
         std::holds_alternative<BlendFactorRecord>(payload) ||
         std::holds_alternative<StencilRefRecord>(payload) ||
         std::holds_alternative<PrimitiveTopologyRecord>(payload) ||
         std::holds_alternative<VertexBuffersRecord>(payload) ||
         std::holds_alternative<IndexBufferRecord>(payload) ||
         std::holds_alternative<RenderTargetsRecord>(payload);
}

} // namespace dxmt::d3d12
