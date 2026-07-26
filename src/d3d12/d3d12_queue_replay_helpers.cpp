#include "d3d12_queue_replay_helpers.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_context.hpp"
#include "log/log.hpp"

#include <atomic>
#include <cstring>

namespace dxmt::d3d12 {

void
EmitSparseTextureMappingBarrier(
    CommandChunk *chunk, Rc<Texture> texture,
    std::vector<SparseTileBarrierSubresource> subresources) {
  if (!chunk || !texture || subresources.empty())
    return;
  chunk->emitcc([texture = std::move(texture),
                 subresources = std::move(subresources)](
                    ArgumentEncodingContext &enc) mutable {
    enc.startBlitPass();
    auto &barrier =
        enc.encodeBlitCommand<wmtcmd_blit_resource_state_barrier>();
    barrier.type = WMTBlitCommandResourceStateBarrier;
    for (const auto &subresource : subresources)
      enc.access(texture, subresource.level, subresource.slice,
                 ResourceAccess::All);
    enc.endPass();
    RecordTileMappingBarrier();
  });
}

Resource *
GetResource(ID3D12Resource *resource) {
  if (ReplayBreakdownEnabled())
    replayRttiCounters().getResource++;
  if (!resource)
    return nullptr;
  // Non-RTTI downcast via private IID (no AddRef) — replaces dynamic_cast on the
  // replay hot path. A non-dxmt resource returns E_NOINTERFACE (out stays null),
  // so this preserves dynamic_cast's null-on-mismatch semantics. See
  // IID_DXMTResourceDowncast in d3d12_resource.hpp.
  Resource *out = nullptr;
  resource->QueryInterface(IID_DXMTResourceDowncast,
                           reinterpret_cast<void **>(&out));
  return out;
}

PipelineState *
GetPipelineState(ID3D12PipelineState *pipeline_state) {
  if (ReplayBreakdownEnabled())
    replayRttiCounters().getPipeline++;
  if (!pipeline_state)
    return nullptr;
  PipelineState *out = nullptr;
  pipeline_state->QueryInterface(IID_DXMTPipelineStateDowncast,
                                 reinterpret_cast<void **>(&out));
  return out;
}

bool
ReadBufferBytes(ID3D12Resource *resource, UINT64 offset, void *dst,
                UINT64 size, const char *context) {
  auto *d3d12_resource = GetResource(resource);
  if (!d3d12_resource || !d3d12_resource->GetBufferAllocation() || !dst)
    return false;
  if (offset > d3d12_resource->GetResourceDesc().Width ||
      size > d3d12_resource->GetResourceDesc().Width - offset) {
    WARN("D3D12CommandQueue: ", context, " read exceeds buffer bounds");
    return false;
  }
  auto *mapped = d3d12_resource->GetBufferAllocation()->mappedMemory(0);
  if (!mapped) {
    WARN("D3D12CommandQueue: ", context,
         " requires a CPU-visible buffer for initial support");
    return false;
  }
  std::memcpy(dst,
              static_cast<const char *>(mapped) +
                  d3d12_resource->GetHeapOffset() + offset,
              size);
  return true;
}

bool
ValidateBufferRange(Resource *resource, UINT64 offset, UINT64 size,
                    const char *context) {
  if (!resource || !resource->GetBufferAllocation())
    return false;
  const UINT64 width = resource->GetResourceDesc().Width;
  if (offset > width || size > width - offset) {
    WARN("D3D12CommandQueue: ", context, " exceeds buffer bounds");
    return false;
  }
  return true;
}

void
ResolveQueryDataToCpuBufferStatic(const void *command_list_identity,
                                  ID3D12QueryHeap *query_heap,
                                  D3D12_QUERY_TYPE type, UINT start_index,
                                  UINT query_count, Resource *dst,
                                  ID3D12Resource *dst_identity,
                                  UINT64 dst_buffer_offset,
                                  const QueryResolveSnapshot &snapshot,
                                  const char *context,
                                  uintptr_t queue_id) {
  if (!dst)
    return;

  std::vector<uint8_t> data;
  if (!ResolveQuerySnapshot(snapshot, data)) {
    WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData final failed"
         " context=", context,
         " queue=", queue_id,
         " queryType=", type,
         " start=", start_index,
         " count=", query_count);
    return;
  }
  if (!ValidateBufferRange(dst, dst_buffer_offset, data.size(),
                           "query resolve"))
    return;
  if (!data.empty())
    dst->GetBufferAllocation()->updateContents(
        dst->GetHeapOffset() + dst_buffer_offset, data.data(),
        data.size());
  dxmt::apitrace::record_resolve_query_data_result(
      command_list_identity, query_heap, static_cast<uint32_t>(type), start_index,
      query_count, dst_identity, dst_buffer_offset,
      data.data(), data.size());
  static std::atomic<uint32_t> log_count = 0;
  if (D3D12DiagShouldLog(log_count, D3D12QueryFallbackStatsEnabled()))
    WARN_FILE_ONLY("D3D12 queue diagnostic: ResolveQueryData leave"
         " context=", context,
         " queue=", queue_id,
         " bytes=", data.size());
}

void ResolveQueryDataToCpuBufferStatic(
    const ResolveQueryDataRecord &record,
    const QueryResolveSnapshot &snapshot, const char *context,
    uintptr_t queue_id) {
  ResolveQueryDataToCpuBufferStatic(
      reinterpret_cast<const void *>(record.command_list_identity),
      record.heap.ptr(), record.type,
      record.start_index, record.query_count, GetResource(record.dst_buffer.ptr()),
      record.dst_buffer.ptr(), record.dst_buffer_offset, snapshot, context,
      queue_id);
}

RootSignature *
GetRootSignature(ID3D12RootSignature *root_signature) {
  return GetDXMTRootSignature(root_signature);
}

} // namespace dxmt::d3d12
