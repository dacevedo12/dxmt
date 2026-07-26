#include "d3d12_retire_diag_work.hpp"

#include "d3d12_queue_diagnostics_report.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "log/log.hpp"
#include "util_noexcept.hpp"

namespace dxmt::d3d12 {

void
RetireDrawVisibilityWork(DrawVisibilityRetirementWork &work,
                         GpuCompletionStatus status) noexcept {
  dxmt::invokeNoexcept("draw visibility retirement", [&work, status]() {
    uint64_t value = 0;
    const bool ready =
        status == GpuCompletionStatus::Complete && work.query &&
        work.query->getValue(&value);
    INFO("D3D12 diagnostic: draw visibility",
         " kind=", work.kind,
         " sequence=", work.d3d_sequence,
         " recordSerial=", work.record_serial,
         " pso=", work.pso,
         " ready=", uint32_t(ready),
         " visibleSamples=", ready ? value : 0,
         " vertexCount=", work.vertex_count,
         " indexCount=", work.index_count,
         " instanceCount=", work.instance_count);
  });
}

void
RetireIndexReadbackWork(IndexReadbackRetirementWork &work,
                        GpuCompletionStatus status) noexcept {
  if (status != GpuCompletionStatus::Complete)
    return;
  dxmt::invokeNoexcept("index readback retirement", [&work]() {
    const auto *mapped = work.buffer.mapped();
    INFO("D3D12 diagnostic: IA index readback",
         " kind=", work.kind,
         " sequence=", work.d3d_sequence,
         " pso=", work.key_prefix,
         " format=", uint32_t(work.format),
         " startIndex=", work.start_index,
         " indexCount=", work.index_count,
         " baseVertex=", work.base_vertex,
         " bytes=", work.size,
         " indices=",
         D3D12DiagIndexWords(mapped, work.size, work.format),
         " hex=", D3D12DiagHexBytes(mapped, work.size));
  });
}

void
RetireVertexReadbackWork(VertexReadbackRetirementWork &work,
                         GpuCompletionStatus status) noexcept {
  if (status != GpuCompletionStatus::Complete)
    return;
  dxmt::invokeNoexcept("vertex readback retirement", [&work]() {
    const auto *mapped = work.buffer.mapped();
    INFO("D3D12 diagnostic: IA vertex readback",
         " kind=", work.kind,
         " sequence=", work.d3d_sequence,
         " pso=", work.key_prefix,
         " slot=", work.slot,
         " stride=", work.stride,
         " viewSize=", work.view_size,
         " resourceOffset=", work.resource_offset,
         " heapOffset=", work.heap_offset,
         " vertexOffset=", work.vertex_offset,
         " bytes=", work.size,
         " floats=", D3D12DiagFloatWords(mapped, work.size),
         " hex=", D3D12DiagHexBytes(mapped, work.size));
  });
}

void
RetireConstantBufferReadbackWork(ConstantBufferReadbackRetirementWork &work,
                                 GpuCompletionStatus status) noexcept {
  if (status != GpuCompletionStatus::Complete)
    return;
  dxmt::invokeNoexcept("constant-buffer readback retirement", [&work]() {
    const auto *mapped = work.buffer.mapped();
    if (work.log_readback) {
      INFO("D3D12 diagnostic: CBV readback",
           " kind=", work.kind,
           " sequence=", work.d3d_sequence,
           " pso=", work.key_prefix,
           " stage=", work.stage,
           " root=", work.root_index,
           " slot=", work.slot,
           " address=", work.address,
           " declaredSize=", work.declared_size,
           " resourceOffset=", work.resource_offset,
           " heapOffset=", work.heap_offset,
           " bytes=", work.size,
           " el18=", D3D12DiagFloatsAt(mapped, work.size,
                                       kCbvReadbackProbeElement18ByteOffset),
           " el68=", D3D12DiagFloatsAt(mapped, work.size,
                                       kCbvReadbackProbeElement68ByteOffset),
           " floats=", D3D12DiagFloatWords(mapped, work.size),
           " hex=", D3D12DiagHexBytes(mapped, work.size));
    }
    if (work.record_snapshot && work.resource_object_id) {
      dxmt::apitrace::record_resource_bytes_snapshot(
          work.resource_object_id, work.resource_offset,
          work.resource_offset + work.size, mapped,
          work.draw_record_sequence);
    }
  });
}

} // namespace dxmt::d3d12
