#include "d3d12_replay_batching_policy.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "util_env.hpp"

#include <variant>

namespace dxmt::d3d12 {

bool D3D12ReplayComputeBatchingEnabled() {
  static const bool enabled =
      D3D12EnabledEnvDefaultOn("DXMT_D3D12_COMPUTE_BATCHING");
  return enabled;
}

bool D3D12ReplayGraphicsBatchingEnabled() {
  return !D3D12DiagIAReadbackEnabled() && !D3D12DiagCBVReadbackEnabled() &&
         !D3D12DiagDrawVisibilityEnabled();
}

bool IntraPassParallelEnabled() {
  static const bool on = env::getEnvVar("DXMT_INTRAPASS_PARALLEL") == "1";
  return on;
}

bool IsGraphicsPassRunRecord(const CommandRecord &record) {
  return std::holds_alternative<DrawInstancedRecord>(record.payload) ||
         std::holds_alternative<DrawIndexedInstancedRecord>(record.payload) ||
         std::holds_alternative<PipelineStateRecord>(record.payload) ||
         std::holds_alternative<ViewportRecord>(record.payload) ||
         std::holds_alternative<ScissorRecord>(record.payload) ||
         std::holds_alternative<BlendFactorRecord>(record.payload) ||
         std::holds_alternative<StencilRefRecord>(record.payload) ||
         std::holds_alternative<PrimitiveTopologyRecord>(record.payload) ||
         std::holds_alternative<VertexBuffersRecord>(record.payload) ||
         std::holds_alternative<IndexBufferRecord>(record.payload) ||
         std::holds_alternative<RootSignatureRecord>(record.payload) ||
         std::holds_alternative<DescriptorHeapsRecord>(record.payload) ||
         std::holds_alternative<RootDescriptorTableRecord>(record.payload) ||
         std::holds_alternative<RootConstantsRecord>(record.payload) ||
         std::holds_alternative<RootDescriptorRecord>(record.payload);
}

bool IsGraphicsStateSetterRecord(const CommandRecord &record) {
  return std::holds_alternative<PipelineStateRecord>(record.payload) ||
         std::holds_alternative<ViewportRecord>(record.payload) ||
         std::holds_alternative<ScissorRecord>(record.payload) ||
         std::holds_alternative<BlendFactorRecord>(record.payload) ||
         std::holds_alternative<StencilRefRecord>(record.payload) ||
         std::holds_alternative<PrimitiveTopologyRecord>(record.payload) ||
         std::holds_alternative<VertexBuffersRecord>(record.payload) ||
         std::holds_alternative<IndexBufferRecord>(record.payload) ||
         std::holds_alternative<RootSignatureRecord>(record.payload) ||
         std::holds_alternative<DescriptorHeapsRecord>(record.payload) ||
         std::holds_alternative<RootDescriptorTableRecord>(record.payload) ||
         std::holds_alternative<RootConstantsRecord>(record.payload) ||
         std::holds_alternative<RootDescriptorRecord>(record.payload);
}

bool IsDeferrableCompiledStateRecord(const CommandRecord &record) {
  return IsGraphicsStateSetterRecord(record) ||
         std::holds_alternative<ClearStateRecord>(record.payload) ||
         std::holds_alternative<RenderTargetsRecord>(record.payload) ||
         std::holds_alternative<PredicationRecord>(record.payload);
}

bool FallbackRecordNeedsBindingState(const CommandRecord &record) {
  return std::holds_alternative<DrawInstancedRecord>(record.payload) ||
         std::holds_alternative<DrawIndexedInstancedRecord>(record.payload) ||
         std::holds_alternative<DispatchRecord>(record.payload) ||
         std::holds_alternative<ExecuteIndirectRecord>(record.payload);
}

} // namespace dxmt::d3d12
