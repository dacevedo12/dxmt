#include "d3d12_queue_diagnostics_report.hpp"

#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_resource.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace dxmt::d3d12 {

std::string
D3D12DiagRootBaseWords(const std::vector<uint32_t> &words) {
  std::ostringstream out;
  out << '[';
  for (size_t i = 0; i < words.size(); i++) {
    if (i)
      out << ',';
    out << words[i];
  }
  out << ']';
  return out.str();
}

void
D3D12DiagLogNativePacket(
    const char *kind, uint64_t frame, uint64_t sequence,
    uint64_t record_serial, const PipelineState &pipeline,
    obj_handle_t metal_pso,
    const std::vector<CompiledCommandRootDescriptorTable> &tables,
    std::initializer_list<std::pair<const char *,
                                    const CompiledNativeStageBinding *>> stages) {
  if (!D3D12DiagRootCauseDenseEnabled())
    return;

  static std::atomic<uint64_t> packet_count = 0;
  static std::atomic<uint64_t> anomalous_packet_count = 0;
  const auto packet_id = packet_count.fetch_add(1, std::memory_order_relaxed) + 1;
  uint64_t scanned = 0;
  uint64_t null_records = 0;
  uint64_t buffer_records = 0;
  uint64_t texture_records = 0;
  uint64_t sampler_records = 0;
  uint64_t anomalous_records = 0;
  uint64_t truncated_tables = 0;
  const auto max_slots = D3D12DiagRootCauseDenseMaxSlots();

  for (const auto &table : tables) {
    if (!table.mirror)
      continue;
    const auto current_generation =
        table.mirror->backendResourceTableGeneration();
    const auto available = table.heap_index < table.heap_count
                               ? table.heap_count - table.heap_index
                               : 0u;
    const auto requested = std::min(table.descriptor_count, available);
    const auto remaining = scanned < max_slots ? max_slots - scanned : 0;
    const auto count = std::min<uint64_t>(requested, remaining);
    if (count < requested)
      truncated_tables++;

    for (uint32_t local = 0; local < count; local++) {
      const uint32_t slot = table.heap_index + local;
      const auto meta = table.mirror->slotMeta(slot);
      if (!meta || meta->kind == DescriptorBackendSlotKind::Empty) {
        null_records++;
        scanned++;
        continue;
      }
      if (meta->kind == DescriptorBackendSlotKind::Texture) {
        texture_records++;
        scanned++;
        continue;
      }
      if (meta->kind == DescriptorBackendSlotKind::Sampler) {
        sampler_records++;
        scanned++;
        continue;
      }

      buffer_records++;
      scanned++;
      const auto descriptor = table.mirror->bufferDescriptorRecord(slot);
      if (!descriptor) {
        anomalous_records++;
        WARN_FILE_ONLY(
            "D3D12 root-cause: native descriptor anomaly",
            " packet=", packet_id, " kind=", kind, " frame=", frame,
            " sequence=", sequence, " recordSerial=", record_serial,
            " pso=", pipeline.GetShaderCacheKey(),
            " tableRoot=", table.root_parameter_index, " slot=", slot,
            " reason=missing-buffer-record");
        continue;
      }
      const auto resource =
          table.mirror->backendResourceRecord(descriptor->resource_index);
      const auto flags = DiagnoseNativeBufferDescriptor(*descriptor, resource);
      if (!flags)
        continue;
      anomalous_records++;
      WARN_FILE_ONLY(
          "D3D12 root-cause: native descriptor anomaly",
          " packet=", packet_id, " kind=", kind, " frame=", frame,
          " sequence=", sequence, " recordSerial=", record_serial,
          " pso=", pipeline.GetShaderCacheKey(),
          " metalPso=", uint64_t(metal_pso),
          " tableRoot=", table.root_parameter_index,
          " tableBase=", table.heap_index, " tableCount=", table.descriptor_count,
          " slot=", slot, " diagFlags=0x", std::hex, flags, std::dec,
          " descriptorFlags=0x", std::hex, descriptor->flags, std::dec,
          " resourceIndex=", descriptor->resource_index,
          " byteOffset=", descriptor->byte_offset,
          " byteSize=", descriptor->byte_size,
          " resourceGpuAddress=", resource ? resource->gpu_address : 0,
          " resourceByteSize=", resource ? resource->byte_size : 0,
          " resourceGeneration=", resource ? resource->generation : 0,
          " capturedTableGeneration=", table.buffer_resource_table_generation,
          " currentTableGeneration=", current_generation,
          " allocation=", resource ? uint64_t(resource->allocation.handle) : 0);
    }

    WARN_FILE_ONLY(
        "D3D12 root-cause: native table",
        " packet=", packet_id, " kind=", kind, " frame=", frame,
        " sequence=", sequence, " recordSerial=", record_serial,
        " pso=", pipeline.GetShaderCacheKey(),
        " root=", table.root_parameter_index,
        " heapType=", uint32_t(table.heap_type),
        " base=", table.heap_index, " count=", table.descriptor_count,
        " heapCount=", table.heap_count,
        " descriptorTableGpuAddress=", table.descriptor_table_gpu_address,
        " bufferRecordGpuAddress=", table.buffer_descriptor_record_gpu_address,
        " resourceTableGpuAddress=", table.buffer_resource_table_gpu_address,
        " capturedGeneration=", table.buffer_resource_table_generation,
        " currentGeneration=", current_generation);
  }

  for (const auto &[stage, binding] : stages) {
    if (!binding)
      continue;
    WARN_FILE_ONLY(
        "D3D12 root-cause: native root bases",
        " packet=", packet_id, " kind=", kind, " frame=", frame,
        " sequence=", sequence, " recordSerial=", record_serial,
        " pso=", pipeline.GetShaderCacheKey(), " stage=", stage,
        " cbuffer=", D3D12DiagRootBaseWords(binding->cbuffer_root_bases),
        " resource=", D3D12DiagRootBaseWords(binding->resource_root_bases));
  }

  if (anomalous_records)
    anomalous_packet_count.fetch_add(1, std::memory_order_relaxed);
  WARN_FILE_ONLY(
      "D3D12 root-cause: native packet",
      " packet=", packet_id, " kind=", kind, " frame=", frame,
      " sequence=", sequence, " recordSerial=", record_serial,
      " pso=", pipeline.GetShaderCacheKey(), " metalPso=", uint64_t(metal_pso),
      " tables=", tables.size(), " scanned=", scanned,
      " null=", null_records, " buffer=", buffer_records,
      " texture=", texture_records, " sampler=", sampler_records,
      " anomalies=", anomalous_records,
      " truncatedTables=", truncated_tables,
      " totalAnomalousPackets=",
      anomalous_packet_count.load(std::memory_order_relaxed));
}

std::string
D3D12DiagHexBytes(const uint8_t *bytes, size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  const auto count = std::min<size_t>(size, 64);
  for (size_t i = 0; i < count; i++) {
    if (i)
      out << ' ';
    out << std::setw(2) << uint32_t(bytes[i]);
  }
  return out.str();
}

Rc<VisibilityResultQuery>
D3D12DiagCreateDrawVisibilityQuery(
    const char *kind, const std::string &pso,
    uint64_t d3d_sequence, uint32_t vertex_count, uint32_t index_count,
    uint32_t instance_count, DrawVisibilityRetirementWork *retirement) {
  static std::atomic<uint32_t> log_count = 0;
  const bool target_pso = DiagIsTargetCompositePso(pso);
  if (!target_pso &&
      !D3D12DiagShouldLog(log_count, D3D12DiagDrawVisibilityEnabled()))
    return nullptr;

  Rc<VisibilityResultQuery> query = new VisibilityResultQuery();
  *retirement = {
      .query = query,
      .kind = kind,
      .pso = pso,
      .d3d_sequence = d3d_sequence,
      .record_serial = DiagCurrentReplayRecordSerial(),
      .vertex_count = vertex_count,
      .index_count = index_count,
      .instance_count = instance_count,
  };
  return query;
}

std::string
D3D12DiagFloatWords(const uint8_t *bytes, size_t size) {
  std::ostringstream out;
  const auto count = std::min<size_t>(size / sizeof(float), 16);
  for (size_t i = 0; i < count; i++) {
    float value = 0.0f;
    std::memcpy(&value, bytes + i * sizeof(value), sizeof(value));
    if (i)
      out << ',';
    out << value;
  }
  return out.str();
}

std::string
D3D12DiagFloatsAt(const uint8_t *bytes, size_t size, size_t byte_offset) {
  if (byte_offset + 4 * sizeof(float) > size)
    return "oob";
  std::ostringstream out;
  for (size_t i = 0; i < 4; i++) {
    float value = 0.0f;
    std::memcpy(&value, bytes + byte_offset + i * sizeof(float), sizeof(value));
    if (i)
      out << ',';
    out << value;
  }
  return out.str();
}

std::string
D3D12DiagIndexWords(const uint8_t *bytes, size_t size,
                    DXGI_FORMAT format) {
  std::ostringstream out;
  const size_t index_size = format == DXGI_FORMAT_R16_UINT ? 2 : 4;
  const auto count = std::min<size_t>(size / index_size, 32);
  for (size_t i = 0; i < count; i++) {
    uint32_t value = 0;
    if (index_size == 2) {
      uint16_t v = 0;
      std::memcpy(&v, bytes + i * index_size, sizeof(v));
      value = v;
    } else {
      std::memcpy(&value, bytes + i * index_size, sizeof(value));
    }
    if (i)
      out << ',';
    out << value;
  }
  return out.str();
}

const uint8_t *
D3D12DiagMappedAllocationBytes(BufferAllocation *allocation,
                               uint64_t offset, uint64_t requested,
                               uint64_t &available) {
  available = 0;
  if (!allocation ||
      allocation->flags().test(BufferAllocationFlag::CpuInvisible) ||
      offset >= allocation->length())
    return nullptr;
  auto *base = static_cast<const uint8_t *>(
      allocation->mappedMemory(allocation->currentSuballocation()));
  if (!base)
    return nullptr;
  available = std::min<uint64_t>(requested, allocation->length() - offset);
  return base + offset;
}

void D3D12DiagLogCompiledTargetInputs(
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const PipelineState &pipeline, uint64_t frame, uint64_t record_serial) {
  const auto &key = pipeline.GetShaderCacheKey();
  if (!D3D12DiagRootCauseTargetMatches(key))
    return;
  static std::atomic<uint32_t> sample_count = 0;
  const auto sample = sample_count.fetch_add(1, std::memory_order_relaxed);
  if (sample >= D3D12DiagRootCauseTargetSampleLimit())
    return;

  const auto *graphics = pipeline.GetGraphicsState();
  const auto draw_vertex_count =
      packet.draw ? packet.draw->vertex_count_per_instance : 0;
  const auto draw_start_vertex =
      packet.draw ? packet.draw->start_vertex_location : 0;
  const auto index_count =
      packet.draw_indexed ? packet.draw_indexed->index_count_per_instance : 0;
  const auto start_index =
      packet.draw_indexed ? packet.draw_indexed->start_index_location : 0;
  const auto base_vertex =
      packet.draw_indexed ? packet.draw_indexed->base_vertex_location : 0;
  WARN_FILE_ONLY(
      "D3D12 root-cause: compiled target draw",
      " sample=", sample, " frame=", frame,
      " sequence=", packet.d3d_sequence,
      " recordSerial=", record_serial, " pso=", key,
      " recordIndex=", packet.record_index,
      " topology=", uint32_t(packet.render_state.topology),
      " inputElements=", graphics ? graphics->input_elements.size() : 0,
      " drawVertexCount=", draw_vertex_count,
      " drawStartVertex=", draw_start_vertex,
      " indexCount=", index_count, " startIndex=", start_index,
      " baseVertex=", base_vertex,
      " instanceCount=", packet.draw ? packet.draw->instance_count
                                      : packet.draw_indexed
                                            ? packet.draw_indexed->instance_count
                                            : 0);

  if (!prepared.root_tables)
    return;
  for (const auto &table : *prepared.root_tables) {
    const auto current_generation =
        table.mirror ? table.mirror->backendResourceTableGeneration() : 0;
    WARN_FILE_ONLY(
        "D3D12 root-cause: compiled target root table",
        " sample=", sample, " pso=", key,
        " root=", table.root_parameter_index,
        " heapType=", uint32_t(table.heap_type),
        " base=0x", std::hex, table.base_descriptor.ptr, std::dec,
        " heapIndex=", table.heap_index,
        " descriptorCount=", table.descriptor_count,
        " heapCount=", table.heap_count,
        " tableOffset=", table.table_offset,
        " tableStride=", table.table_entry_stride,
        " rootTableBase=", table.root_table_base_descriptor_index,
        " descriptorTableGpu=0x", std::hex,
        table.descriptor_table_gpu_address,
        " descriptorEntryGpu=0x", table.descriptor_table_entry_gpu_address,
        " bufferRecordGpu=0x", table.buffer_descriptor_record_gpu_address,
        " bufferResourceGpu=0x", table.buffer_resource_table_gpu_address,
        std::dec,
        " capturedGeneration=", table.buffer_resource_table_generation,
        " currentGeneration=", current_generation,
        " resolved=", table.resolved,
        " tableBackendReady=", table.descriptor_table_backend_ready,
        " recordStorageReady=", table.native_descriptor_record_storage_ready,
        " resourceTableReady=", table.native_buffer_resource_table_ready,
        " rootBaseReady=", table.native_root_table_base_ready);
  }

  for (const auto &constants : packet.root_constants) {
    WARN_FILE_ONLY(
        "D3D12 root-cause: compiled target root constants",
        " sample=", sample, " pso=", key,
        " root=", constants.root_parameter_index,
        " dstOffset=", constants.dst_offset,
        " count=", constants.values.size(),
        " values=", D3D12DiagRootBaseWords(constants.values));
  }

  if (graphics) {
    for (size_t i = 0; i < graphics->input_elements.size(); i++) {
      const auto &element = graphics->input_elements[i];
      WARN_FILE_ONLY(
          "D3D12 root-cause: compiled target input element",
          " sample=", sample, " pso=", key, " index=", i,
          " semantic=", element.SemanticName ? element.SemanticName : "<null>",
          " semanticIndex=", element.SemanticIndex,
          " format=", uint32_t(element.Format),
          " slot=", element.InputSlot,
          " alignedOffset=", element.AlignedByteOffset,
          " slotClass=", uint32_t(element.InputSlotClass),
          " stepRate=", element.InstanceDataStepRate);
    }
  }

  for (const auto &vb : packet.input_assembler.vertex_buffers) {
    UINT64 resource_offset = 0;
    auto *resource = LookupBufferResourceByGpuVirtualAddress(
        vb.view.BufferLocation, &resource_offset);
    auto allocation = resource ? resource->GetBufferAllocation() : nullptr;
    const uint64_t allocation_offset =
        resource ? resource->GetHeapOffset() + resource_offset : 0;
    uint64_t mapped_size = 0;
    const auto *mapped = D3D12DiagMappedAllocationBytes(
        allocation, allocation_offset,
        std::min<uint64_t>(vb.view.SizeInBytes, 256), mapped_size);
    WARN_FILE_ONLY(
        "D3D12 root-cause: compiled target vertex buffer",
        " sample=", sample, " pso=", key, " slot=", vb.slot,
        " gpuVa=", vb.view.BufferLocation,
        " stride=", vb.view.StrideInBytes,
        " viewSize=", vb.view.SizeInBytes,
        " resource=", uint64_t(resource ? resource->GetD3D12Resource() : nullptr),
        " resourceOffset=", resource_offset,
        " resourceWidth=", resource ? resource->GetResourceDesc().Width : 0,
        " heapOffset=", resource ? resource->GetHeapOffset() : 0,
        " allocation=", allocation ? uint64_t(allocation->buffer().handle) : 0,
        " allocationGpuAddress=", allocation ? allocation->gpuAddress() : 0,
        " allocationLength=", allocation ? allocation->length() : 0,
        " currentSuballocation=", allocation ? allocation->currentSuballocation() : 0,
        " cpuMapped=", mapped != nullptr,
        " sampleBytes=", mapped_size,
        " floats=", mapped ? D3D12DiagFloatWords(mapped, mapped_size) : "-",
        " hex=", mapped ? D3D12DiagHexBytes(mapped, mapped_size) : "-");
  }

  if (packet.input_assembler.index_buffer) {
    const auto &view = *packet.input_assembler.index_buffer;
    UINT64 resource_offset = 0;
    auto *resource = LookupBufferResourceByGpuVirtualAddress(
        view.BufferLocation, &resource_offset);
    auto allocation = resource ? resource->GetBufferAllocation() : nullptr;
    const uint64_t index_size = view.Format == DXGI_FORMAT_R16_UINT ? 2 : 4;
    const uint64_t allocation_offset =
        (resource ? resource->GetHeapOffset() + resource_offset : 0) +
        uint64_t(start_index) * index_size;
    const uint64_t max_index_bytes =
        view.SizeInBytes > uint64_t(start_index) * index_size
            ? view.SizeInBytes - uint64_t(start_index) * index_size
            : 0;
    uint64_t mapped_size = 0;
    const auto *mapped = D3D12DiagMappedAllocationBytes(
        allocation, allocation_offset,
        std::min<uint64_t>(max_index_bytes, 256), mapped_size);
    WARN_FILE_ONLY(
        "D3D12 root-cause: compiled target index buffer",
        " sample=", sample, " pso=", key,
        " gpuVa=", view.BufferLocation, " format=", uint32_t(view.Format),
        " viewSize=", view.SizeInBytes,
        " resourceOffset=", resource_offset,
        " heapOffset=", resource ? resource->GetHeapOffset() : 0,
        " allocation=", allocation ? uint64_t(allocation->buffer().handle) : 0,
        " cpuMapped=", mapped != nullptr, " sampleBytes=", mapped_size,
        " indices=", mapped ? D3D12DiagIndexWords(mapped, mapped_size, view.Format) : "-",
        " hex=", mapped ? D3D12DiagHexBytes(mapped, mapped_size) : "-");
  }

  for (const auto &root : packet.root_descriptors) {
    UINT64 resource_offset = 0;
    auto *resource = LookupBufferResourceByGpuVirtualAddress(
        root.address, &resource_offset);
    auto allocation = resource ? resource->GetBufferAllocation() : nullptr;
    const uint64_t allocation_offset =
        resource ? resource->GetHeapOffset() + resource_offset : 0;
    uint64_t mapped_size = 0;
    const auto *mapped = D3D12DiagMappedAllocationBytes(
        allocation, allocation_offset, 256, mapped_size);
    WARN_FILE_ONLY(
        "D3D12 root-cause: compiled target root descriptor",
        " sample=", sample, " pso=", key,
        " root=", root.root_parameter_index,
        " type=", uint32_t(root.parameter_type), " gpuVa=", root.address,
        " resource=", uint64_t(resource ? resource->GetD3D12Resource() : nullptr),
        " resourceOffset=", resource_offset,
        " resourceWidth=", resource ? resource->GetResourceDesc().Width : 0,
        " heapOffset=", resource ? resource->GetHeapOffset() : 0,
        " allocation=", allocation ? uint64_t(allocation->buffer().handle) : 0,
        " cpuMapped=", mapped != nullptr, " sampleBytes=", mapped_size,
        " floats=", mapped ? D3D12DiagFloatWords(mapped, mapped_size) : "-",
        " hex=", mapped ? D3D12DiagHexBytes(mapped, mapped_size) : "-");
  }
}

const char *
D3D12FillModeName(D3D12_FILL_MODE mode) {
  switch (mode) {
  case D3D12_FILL_MODE_WIREFRAME:
    return "wireframe";
  case D3D12_FILL_MODE_SOLID:
    return "solid";
  default:
    return "unknown";
  }
}

const char *
D3D12CullModeName(D3D12_CULL_MODE mode) {
  switch (mode) {
  case D3D12_CULL_MODE_NONE:
    return "none";
  case D3D12_CULL_MODE_FRONT:
    return "front";
  case D3D12_CULL_MODE_BACK:
    return "back";
  default:
    return "unknown";
  }
}

const char *
D3D12TextureCopyTypeName(D3D12_TEXTURE_COPY_TYPE type) {
  switch (type) {
  case D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX:
    return "subresource";
  case D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT:
    return "placed_footprint";
  default:
    return "unknown";
  }
}

const char *
PipelineStageName(PipelineStage stage) {
  switch (stage) {
  case PipelineStage::Pixel:
    return "pixel";
  case PipelineStage::Compute:
    return "compute";
  case PipelineStage::Geometry:
    return "geometry";
  case PipelineStage::Hull:
    return "hull";
  case PipelineStage::Domain:
    return "domain";
  case PipelineStage::Vertex:
  default:
    return "vertex";
  }
}

} // namespace dxmt::d3d12
