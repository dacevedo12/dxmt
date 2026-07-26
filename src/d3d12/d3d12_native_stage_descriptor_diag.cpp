#include "d3d12_native_stage_descriptor_diag.hpp"

#include "d3d12_compiled_binding_tables.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_descriptor_record_query.hpp"
#include "d3d12_descriptor_table_access.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_shader_binding.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>
#include <ios>
#include <optional>

namespace dxmt::d3d12 {

void
DiagnoseCompiledNativeStageDescriptors(
    const std::vector<CompiledCommandRootDescriptorTable> &tables,
    const PipelineState &pipeline, const NativeRootBaseStagePlan *plan,
    PipelineStage stage, const char *kind, uint64_t frame, uint64_t sequence,
    uint64_t record_serial, obj_handle_t metal_pso) {
  if (!D3D12DiagCorrectnessDenseEnabled())
    return;

  const auto *shader = FindShaderForStage(pipeline, stage);
  const auto *arguments = shader ? shader->resourceArgumentInfo() : nullptr;
  const auto argument_count = shader ? shader->reflection().NumArguments : 0u;
  if (!plan || !arguments || !argument_count)
    return;

  uint32_t scanned = 0;
  uint32_t type_mismatches = 0;
  uint32_t array_mismatches = 0;
  uint32_t invalid_buffers = 0;
  uint32_t missing_records = 0;
  static std::atomic<uint32_t> detail_count = 0;

  for (const auto &entry : plan->entries) {
    if (entry.cbuffer || entry.argument_index >= argument_count ||
        scanned >= 4096)
      continue;
    const auto *table =
        FindCompiledRootTable(tables, entry.root_index, entry.heap_type);
    auto *heap = table
                     ? dynamic_cast<DescriptorHeap *>(table->owning_heap.ptr())
                     : nullptr;
    if (!table || !table->mirror || !heap)
      continue;

    const auto &argument = arguments[entry.argument_index];
    const bool expects_texture =
        argument.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE;
    const bool expects_array =
        argument.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY;
    const auto count = std::min<uint32_t>(entry.range_count, 4096 - scanned);
    for (uint32_t local = 0; local < count; local++, scanned++) {
      if (entry.descriptor_index + local >= entry.descriptor_count)
        break;
      const auto descriptor = GetBoundDescriptorRecordInRangeFromHeap(
          heap, table->base_descriptor, entry.range_offset,
          entry.descriptor_index + local, entry.descriptor_count,
          entry.heap_type);
      if (!descriptor) {
        missing_records++;
        continue;
      }

      const auto slot = descriptor->heap_index;
      const auto meta = table->mirror->slotMeta(slot);
      const bool actual_texture =
          meta && meta->kind == DescriptorBackendSlotKind::Texture;
      uint32_t diag_flags = 0;
      if (!expects_texture && meta &&
          meta->kind == DescriptorBackendSlotKind::Buffer) {
        const auto native = table->mirror->bufferDescriptorRecord(slot);
        const auto backend = native
                                 ? table->mirror->backendResourceRecord(
                                       native->resource_index)
                                 : std::nullopt;
        diag_flags = native
                         ? DiagnoseNativeBufferDescriptor(*native, backend)
                         : NativeDescriptorDiagnosticMissingResource;
        if (diag_flags)
          invalid_buffers++;
      }

      const auto shape = GetDescriptorTextureViewShape(*descriptor);
      const bool array_mismatch =
          expects_texture && actual_texture &&
          (shape == DescriptorTextureViewShape::Array ||
           shape == DescriptorTextureViewShape::NonArray) &&
          ((shape == DescriptorTextureViewShape::Array) != expects_array);
      const bool type_mismatch = expects_texture != actual_texture &&
                                 meta &&
                                 meta->kind != DescriptorBackendSlotKind::Empty;
      if (array_mismatch)
        array_mismatches++;
      if (type_mismatch)
        type_mismatches++;
      if (!array_mismatch && !type_mismatch && !diag_flags)
        continue;

      const auto detail = detail_count.fetch_add(1, std::memory_order_relaxed);
      if (detail >= 256)
        continue;
      WARN_FILE_ONLY(
          "D3D12 correctness: native descriptor mismatch",
          " kind=", kind, " frame=", frame, " sequence=", sequence,
          " recordSerial=", record_serial,
          " pso=", pipeline.GetShaderCacheKey(),
          " metalPso=", uint64_t(metal_pso), " stage=", uint32_t(stage),
          " root=", entry.root_index, " argument=", entry.argument_index,
          " argumentSlot=", argument.SM50BindingSlot,
          " argumentFlags=0x", std::hex, uint32_t(argument.Flags), std::dec,
          " descriptorLocal=", entry.argument_local_start + local,
          " heapSlot=", slot,
          " heapKind=", meta ? uint32_t(meta->kind) : UINT32_MAX,
          " recordType=", uint32_t(descriptor->type),
          " viewDimension=", DescriptorViewDimension(*descriptor),
          " expectsTexture=", expects_texture,
          " expectsArray=", expects_array,
          " actualShape=", uint32_t(shape),
          " arrayMismatch=", array_mismatch,
          " typeMismatch=", type_mismatch,
          " nativeDiagFlags=0x", std::hex, diag_flags, std::dec,
          " resource=", reinterpret_cast<uintptr_t>(descriptor->resource.ptr()),
          " slotVersion=", descriptor->slot_version.sequence,
          " slotGeneration=", meta ? meta->generation : 0);
    }
  }

  if (type_mismatches || array_mismatches || invalid_buffers ||
      missing_records) {
    WARN_FILE_ONLY(
        "D3D12 correctness: native stage summary",
        " kind=", kind, " frame=", frame, " sequence=", sequence,
        " recordSerial=", record_serial,
        " pso=", pipeline.GetShaderCacheKey(),
        " metalPso=", uint64_t(metal_pso), " stage=", uint32_t(stage),
        " scanned=", scanned, " typeMismatch=", type_mismatches,
        " arrayMismatch=", array_mismatches,
        " invalidBuffer=", invalid_buffers,
        " missingRecord=", missing_records);
  }
}

} // namespace dxmt::d3d12
