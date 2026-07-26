#include "d3d12_command_list.hpp"
#include "d3d12_compiled_descriptor_range.hpp"

#include "com/com_guid.hpp"
#include "com/com_object.hpp"
#include "com/com_private_data.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_query.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_format.hpp"
#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace dxmt::d3d12 {

const char *CompiledCommandSegmentKindName(CompiledCommandSegmentKind kind) {
  switch (kind) {
  case CompiledCommandSegmentKind::Graphics:
    return "graphics";
  case CompiledCommandSegmentKind::Compute:
    return "compute";
  case CompiledCommandSegmentKind::Indirect:
    return "indirect";
  case CompiledCommandSegmentKind::Barrier:
    return "barrier";
  case CompiledCommandSegmentKind::Transfer:
    return "transfer";
  case CompiledCommandSegmentKind::Control:
    return "control";
  case CompiledCommandSegmentKind::ElidedState:
    return "elided-state";
  case CompiledCommandSegmentKind::Typed:
    return "typed";
  case CompiledCommandSegmentKind::Fallback:
    return "fallback";
  }
  return "unknown";
}

const char *CompiledCommandFallbackReasonName(
    CompiledCommandFallbackReason reason) {
  switch (reason) {
  case CompiledCommandFallbackReason::None:
    return "none";
  case CompiledCommandFallbackReason::ConservativeCompiler:
    return "conservative_compiler";
  case CompiledCommandFallbackReason::LegacyPipelineState:
    return "legacy_pipeline_state";
  case CompiledCommandFallbackReason::NonBindlessPipelineState:
    return "non_bindless_pipeline_state";
  case CompiledCommandFallbackReason::GeometryPipeline:
    return "geometry_pipeline";
  case CompiledCommandFallbackReason::TessellationPipeline:
    return "tessellation_pipeline";
  case CompiledCommandFallbackReason::MissingPipelineState:
    return "missing_pipeline_state";
  case CompiledCommandFallbackReason::MissingRootSignature:
    return "missing_root_signature";
  case CompiledCommandFallbackReason::ExecuteIndirect:
    return "execute_indirect";
  case CompiledCommandFallbackReason::UnsupportedBarrier:
    return "unsupported_barrier";
  case CompiledCommandFallbackReason::CopyOrResolve:
    return "copy_or_resolve";
  case CompiledCommandFallbackReason::ClearOrDiscard:
    return "clear_or_discard";
  case CompiledCommandFallbackReason::QueryOrPredication:
    return "query_or_predication";
  case CompiledCommandFallbackReason::SnapshotDependent:
    return "snapshot_dependent";
  case CompiledCommandFallbackReason::TemporalUpscale:
    return "temporal_upscale";
  case CompiledCommandFallbackReason::NativeUnsupportedRootSignature:
    return "native_unsupported_root_signature";
  case CompiledCommandFallbackReason::NativeUnsupportedDescriptorRange:
    return "native_unsupported_descriptor_range";
  case CompiledCommandFallbackReason::NativeDescriptorNullBase:
    return "native_descriptor_null_base";
  case CompiledCommandFallbackReason::NativeDescriptorMixedHeap:
    return "native_descriptor_mixed_heap";
  case CompiledCommandFallbackReason::NativeDescriptorNoRange:
    return "native_descriptor_no_range";
  case CompiledCommandFallbackReason::NativeDescriptorInvalidHandle:
    return "native_descriptor_invalid_handle";
  case CompiledCommandFallbackReason::NativeDescriptorHeapTail:
    return "native_descriptor_heap_tail";
  case CompiledCommandFallbackReason::NativeDescriptorAmbiguousRange:
    return "native_descriptor_ambiguous_range";
  case CompiledCommandFallbackReason::NativeDescriptorBackendGeneration:
    return "native_descriptor_backend_generation";
  case CompiledCommandFallbackReason::NativeUnsupportedRootDescriptor:
    return "native_unsupported_root_descriptor";
  case CompiledCommandFallbackReason::NativeUnsupportedGeometryPipeline:
    return "native_unsupported_geometry_pipeline";
  case CompiledCommandFallbackReason::NativeUnsupportedTessellationPipeline:
    return "native_unsupported_tessellation_pipeline";
  case CompiledCommandFallbackReason::NativeUnsupportedExecuteIndirect:
    return "native_unsupported_execute_indirect";
  case CompiledCommandFallbackReason::NativeUnsupportedDynamicResource:
    return "native_unsupported_dynamic_resource";
  case CompiledCommandFallbackReason::NativeMissingDescriptorBackend:
    return "native_missing_descriptor_backend";
  case CompiledCommandFallbackReason::NativeShaderAbiMismatch:
    return "native_shader_abi_mismatch";
  case CompiledCommandFallbackReason::NativeResidencyUnsupported:
    return "native_residency_unsupported";
  case CompiledCommandFallbackReason::InjectedNativePacketAllocationFailure:
    return "injected_native_packet_allocation_failure";
  case CompiledCommandFallbackReason::InjectedNativeSegmentFinalizationFailure:
    return "injected_native_segment_finalization_failure";
  case CompiledCommandFallbackReason::InjectedNativePipelineCompilationFailure:
    return "injected_native_pipeline_compilation_failure";
  case CompiledCommandFallbackReason::UnsupportedRootSignature:
    return "unsupported_root_signature";
  case CompiledCommandFallbackReason::UnsupportedDescriptorTable:
    return "unsupported_descriptor_table";
  case CompiledCommandFallbackReason::UnsupportedRootDescriptor:
    return "unsupported_root_descriptor";
  case CompiledCommandFallbackReason::UnsupportedRootConstants:
    return "unsupported_root_constants";
  case CompiledCommandFallbackReason::UnsupportedVertexIndexState:
    return "unsupported_vertex_index_state";
  case CompiledCommandFallbackReason::UnsupportedRenderTargetState:
    return "unsupported_render_target_state";
  case CompiledCommandFallbackReason::UnsupportedArgumentTable:
    return "unsupported_argument_table";
  case CompiledCommandFallbackReason::UnsupportedCommand:
    return "unsupported_command";
  }
  return "unknown";
}

dxmt::CompiledFallbackReason
CompiledCommandFallbackReasonToPerf(CompiledCommandFallbackReason reason) {
  switch (reason) {
  case CompiledCommandFallbackReason::None:
    return dxmt::CompiledFallbackReason::Unknown;
  case CompiledCommandFallbackReason::ConservativeCompiler:
  case CompiledCommandFallbackReason::UnsupportedCommand:
  case CompiledCommandFallbackReason::CopyOrResolve:
  case CompiledCommandFallbackReason::ClearOrDiscard:
  case CompiledCommandFallbackReason::QueryOrPredication:
  case CompiledCommandFallbackReason::SnapshotDependent:
  case CompiledCommandFallbackReason::TemporalUpscale:
    return dxmt::CompiledFallbackReason::MissingCompiledEncoder;
  case CompiledCommandFallbackReason::NativeUnsupportedRootSignature:
    return dxmt::CompiledFallbackReason::NativeUnsupportedRootSignature;
  case CompiledCommandFallbackReason::NativeUnsupportedDescriptorRange:
    return dxmt::CompiledFallbackReason::NativeUnsupportedDescriptorRange;
  case CompiledCommandFallbackReason::NativeDescriptorNullBase:
    return dxmt::CompiledFallbackReason::NativeDescriptorNullBase;
  case CompiledCommandFallbackReason::NativeDescriptorMixedHeap:
    return dxmt::CompiledFallbackReason::NativeDescriptorMixedHeap;
  case CompiledCommandFallbackReason::NativeDescriptorNoRange:
    return dxmt::CompiledFallbackReason::NativeDescriptorNoRange;
  case CompiledCommandFallbackReason::NativeDescriptorInvalidHandle:
    return dxmt::CompiledFallbackReason::NativeDescriptorInvalidHandle;
  case CompiledCommandFallbackReason::NativeDescriptorHeapTail:
    return dxmt::CompiledFallbackReason::NativeDescriptorHeapTail;
  case CompiledCommandFallbackReason::NativeDescriptorAmbiguousRange:
    return dxmt::CompiledFallbackReason::NativeDescriptorAmbiguousRange;
  case CompiledCommandFallbackReason::NativeDescriptorBackendGeneration:
    return dxmt::CompiledFallbackReason::NativeDescriptorBackendGeneration;
  case CompiledCommandFallbackReason::NativeUnsupportedRootDescriptor:
    return dxmt::CompiledFallbackReason::NativeUnsupportedRootDescriptor;
  case CompiledCommandFallbackReason::NativeUnsupportedGeometryPipeline:
    return dxmt::CompiledFallbackReason::NativeUnsupportedGeometryPipeline;
  case CompiledCommandFallbackReason::NativeUnsupportedTessellationPipeline:
    return dxmt::CompiledFallbackReason::NativeUnsupportedTessellationPipeline;
  case CompiledCommandFallbackReason::NativeUnsupportedExecuteIndirect:
    return dxmt::CompiledFallbackReason::NativeUnsupportedExecuteIndirect;
  case CompiledCommandFallbackReason::NativeUnsupportedDynamicResource:
    return dxmt::CompiledFallbackReason::NativeUnsupportedDynamicResource;
  case CompiledCommandFallbackReason::NativeMissingDescriptorBackend:
    return dxmt::CompiledFallbackReason::NativeMissingDescriptorBackend;
  case CompiledCommandFallbackReason::NativeShaderAbiMismatch:
    return dxmt::CompiledFallbackReason::NativeShaderAbiMismatch;
  case CompiledCommandFallbackReason::NativeResidencyUnsupported:
    return dxmt::CompiledFallbackReason::NativeResidencyUnsupported;
  case CompiledCommandFallbackReason::InjectedNativePacketAllocationFailure:
  case CompiledCommandFallbackReason::InjectedNativeSegmentFinalizationFailure:
  case CompiledCommandFallbackReason::InjectedNativePipelineCompilationFailure:
    return dxmt::CompiledFallbackReason::MissingCompiledEncoder;
  case CompiledCommandFallbackReason::LegacyPipelineState:
    return dxmt::CompiledFallbackReason::LegacyPath;
  case CompiledCommandFallbackReason::NonBindlessPipelineState:
  case CompiledCommandFallbackReason::MissingPipelineState:
    return dxmt::CompiledFallbackReason::UnsupportedPipeline;
  case CompiledCommandFallbackReason::GeometryPipeline:
  case CompiledCommandFallbackReason::TessellationPipeline:
    return dxmt::CompiledFallbackReason::GeometryOrTessellation;
  case CompiledCommandFallbackReason::MissingRootSignature:
  case CompiledCommandFallbackReason::UnsupportedRootSignature:
    return dxmt::CompiledFallbackReason::UnsupportedRootSignature;
  case CompiledCommandFallbackReason::ExecuteIndirect:
    return dxmt::CompiledFallbackReason::Indirect;
  case CompiledCommandFallbackReason::UnsupportedBarrier:
    return dxmt::CompiledFallbackReason::ResourceBarrier;
  case CompiledCommandFallbackReason::UnsupportedDescriptorTable:
    return dxmt::CompiledFallbackReason::UnsupportedDescriptorTable;
  case CompiledCommandFallbackReason::UnsupportedRootDescriptor:
    return dxmt::CompiledFallbackReason::UnsupportedRootDescriptor;
  case CompiledCommandFallbackReason::UnsupportedRootConstants:
    return dxmt::CompiledFallbackReason::UnsupportedRootConstants;
  case CompiledCommandFallbackReason::UnsupportedVertexIndexState:
    return dxmt::CompiledFallbackReason::UnsupportedVertexIndexState;
  case CompiledCommandFallbackReason::UnsupportedRenderTargetState:
    return dxmt::CompiledFallbackReason::UnsupportedRenderTargetState;
  case CompiledCommandFallbackReason::UnsupportedArgumentTable:
    return dxmt::CompiledFallbackReason::UnsupportedArgumentTable;
  }
  return dxmt::CompiledFallbackReason::Unknown;
}

namespace {

std::atomic<uint32_t> g_apitrace_record_diag_log_count = 0;
thread_local uint64_t g_current_command_record_d3d_sequence = 0;

// Test-only, process-local fault points. The runner launches each scenario in
// a fresh process, so occurrence counts stay deterministic without affecting
// production behavior when the variables are absent.
std::atomic<uint64_t> g_test_native_packet_allocation_occurrence = 0;
std::atomic<uint64_t> g_test_native_segment_finalization_occurrence = 0;

static bool
ShouldInjectCompiledCommandFault(const std::string &setting,
                                 std::atomic<uint64_t> &occurrence) {
  if (setting.empty())
    return false;
  if (setting == "always" || setting == "all") {
    occurrence.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  char *end = nullptr;
  const auto target = std::strtoull(setting.c_str(), &end, 0);
  if (end == setting.c_str() || *end || !target)
    return false;
  return occurrence.fetch_add(1, std::memory_order_relaxed) + 1 == target;
}

static void
RecordInjectedCompiledCommandFault(const char *name) {
  const auto path = env::getEnvVar("DXMT_TEST_FAULT_MARKER");
  if (path.empty())
    return;
  FILE *marker = std::fopen(path.c_str(), "a");
  if (!marker)
    return;
  std::fprintf(marker, "%s\n", name);
  std::fclose(marker);
}

// --- Copy-source snapshot deduplication (BUG-008 IO blow-up fix) ---
// SnapshotCopySourceBuffer captures a copy's source-buffer span into the trace when an app keeps
// upload buffers persistently mapped and writes directly instead of using Unmap as a synchronization
// point. Dedup by (resource, offset, end) -> last-emitted content fingerprint avoids re-snapshotting
// unchanged copy source spans in streaming-heavy traces.
struct CopySnapshotKey {
  const void *resource;
  uint64_t offset;
  uint64_t length;
  bool operator==(const CopySnapshotKey &other) const {
    return resource == other.resource && offset == other.offset &&
           length == other.length;
  }
};
struct CopySnapshotKeyHash {
  size_t operator()(const CopySnapshotKey &key) const {
    uint64_t h = 1469598103934665603ull; // FNV-1a offset basis
    auto mix = [&h](uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        h ^= (v & 0xff);
        h *= 1099511628211ull; // FNV-1a prime
        v >>= 8;
      }
    };
    mix(reinterpret_cast<uintptr_t>(key.resource));
    mix(key.offset);
    mix(key.length);
    return static_cast<size_t>(h);
  }
};
std::mutex g_copy_snapshot_dedup_mutex;
std::unordered_map<CopySnapshotKey, uint64_t, CopySnapshotKeyHash>
    g_copy_snapshot_dedup;

// FNV-1a 64-bit over the span; a 64-bit fingerprint is enough for change-detection here -- a
// collision (~2^-64) merely skips one redundant snapshot, never corrupts a real change because a
// genuinely different span would have to hash identically AND share (resource,offset,end).
static uint64_t FnvHashSpan(const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < size; ++i) {
    h ^= bytes[i];
    h *= 1099511628211ull;
  }
  return h;
}

#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
using ApitraceRenderPassResolveSubresources =
    std::vector<std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc>>;

static dxmt::apitrace::RenderPassClearValue
ToApitraceRenderPassClearValue(const D3D12_CLEAR_VALUE &value) {
  dxmt::apitrace::RenderPassClearValue result = {};
  result.format = static_cast<uint32_t>(value.Format);
  for (uint32_t index = 0; index < 4; ++index)
    result.color[index] = value.Color[index];
  result.depth = value.DepthStencil.Depth;
  result.stencil = value.DepthStencil.Stencil;
  return result;
}

static dxmt::apitrace::RenderPassBeginningAccessDesc
ToApitraceRenderPassBeginningAccess(
    const D3D12_RENDER_PASS_BEGINNING_ACCESS &access) {
  dxmt::apitrace::RenderPassBeginningAccessDesc result = {};
  result.type = static_cast<uint32_t>(access.Type);
  if (access.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR)
    result.clear = ToApitraceRenderPassClearValue(access.Clear.ClearValue);
  return result;
}

static dxmt::apitrace::RenderPassEndingAccessDesc
ToApitraceRenderPassEndingAccess(
    const D3D12_RENDER_PASS_ENDING_ACCESS &access,
    std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc> &subresources) {
  dxmt::apitrace::RenderPassEndingAccessDesc result = {};
  result.type = static_cast<uint32_t>(access.Type);
  if (access.Type != D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE)
    return result;

  result.src_resource = access.Resolve.pSrcResource;
  result.dst_resource = access.Resolve.pDstResource;
  result.subresource_count = access.Resolve.SubresourceCount;
  result.format = static_cast<uint32_t>(access.Resolve.Format);
  result.resolve_mode = static_cast<uint32_t>(access.Resolve.ResolveMode);
  result.preserve_resolve_source = access.Resolve.PreserveResolveSource;
  subresources.reserve(access.Resolve.SubresourceCount);
  for (UINT index = 0;
       access.Resolve.pSubresourceParameters &&
       index < access.Resolve.SubresourceCount;
       ++index) {
    const auto &src = access.Resolve.pSubresourceParameters[index];
    dxmt::apitrace::RenderPassResolveSubresourceDesc dst = {};
    dst.src_subresource = src.SrcSubresource;
    dst.dst_subresource = src.DstSubresource;
    dst.dst_x = src.DstX;
    dst.dst_y = src.DstY;
    dst.has_src_rect = true;
    dst.src_left = src.SrcRect.left;
    dst.src_top = src.SrcRect.top;
    dst.src_right = src.SrcRect.right;
    dst.src_bottom = src.SrcRect.bottom;
    subresources.push_back(dst);
  }
  result.subresource_count = static_cast<uint32_t>(subresources.size());
  result.subresources = subresources.data();
  return result;
}

static dxmt::apitrace::RenderPassRenderTargetDesc
ToApitraceRenderPassRenderTarget(
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC &render_target,
    std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc> &subresources) {
  dxmt::apitrace::RenderPassRenderTargetDesc result = {};
  result.cpu_descriptor = render_target.cpuDescriptor.ptr;
  result.beginning_access =
      ToApitraceRenderPassBeginningAccess(render_target.BeginningAccess);
  result.ending_access =
      ToApitraceRenderPassEndingAccess(render_target.EndingAccess, subresources);
  return result;
}

static dxmt::apitrace::RenderPassDepthStencilDesc
ToApitraceRenderPassDepthStencil(
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC &depth_stencil,
    std::array<std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc>, 2> &subresources) {
  dxmt::apitrace::RenderPassDepthStencilDesc result = {};
  result.cpu_descriptor = depth_stencil.cpuDescriptor.ptr;
  result.depth_beginning_access =
      ToApitraceRenderPassBeginningAccess(depth_stencil.DepthBeginningAccess);
  result.stencil_beginning_access =
      ToApitraceRenderPassBeginningAccess(depth_stencil.StencilBeginningAccess);
  result.depth_ending_access =
      ToApitraceRenderPassEndingAccess(depth_stencil.DepthEndingAccess, subresources[0]);
  result.stencil_ending_access =
      ToApitraceRenderPassEndingAccess(depth_stencil.StencilEndingAccess, subresources[1]);
  return result;
}

static bool
IsRenderPassPreserveOrNoAccess(D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE type) {
  return type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE ||
         type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
}

static bool
IsRenderPassPreserveOrNoAccess(D3D12_RENDER_PASS_ENDING_ACCESS_TYPE type) {
  return type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE ||
         type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
}

#endif

StoredTextureCopyLocation
StoreTextureCopyLocation(const D3D12_TEXTURE_COPY_LOCATION &location) {
  StoredTextureCopyLocation stored = {};
  stored.resource = location.pResource;
  stored.type = location.Type;
  if (location.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
    stored.placed_footprint = location.PlacedFootprint;
  else
    stored.subresource_index = location.SubresourceIndex;
  return stored;
}

StoredResourceBarrier
StoreResourceBarrier(const D3D12_RESOURCE_BARRIER &barrier) {
  StoredResourceBarrier stored = {};
  stored.barrier = barrier;
  switch (barrier.Type) {
  case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
    stored.resource = barrier.Transition.pResource;
    break;
  case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
    stored.resource_before = barrier.Aliasing.pResourceBefore;
    stored.resource_after = barrier.Aliasing.pResourceAfter;
    break;
  case D3D12_RESOURCE_BARRIER_TYPE_UAV:
    stored.resource = barrier.UAV.pResource;
    break;
  }
  return stored;
}

static bool
IsWriteResourceState(D3D12_RESOURCE_STATES state) {
  static constexpr UINT WriteStateBits =
      UINT(D3D12_RESOURCE_STATE_RENDER_TARGET) |
      UINT(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
      UINT(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
      UINT(D3D12_RESOURCE_STATE_COPY_DEST) |
      UINT(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
      UINT(D3D12_RESOURCE_STATE_STREAM_OUT);
  return (static_cast<UINT>(state) & WriteStateBits) != 0;
}

static bool
IsValidTransitionState(D3D12_RESOURCE_STATES state) {
  if (state == D3D12_RESOURCE_STATE_COMMON)
    return true;

  if (!IsWriteResourceState(state))
    return true;

  const auto bits = static_cast<UINT>(state);
  return (bits & (bits - 1)) == 0;
}

static bool
IsValidResourceBarrier(const D3D12_RESOURCE_BARRIER &barrier) {
  const auto flags = static_cast<UINT>(barrier.Flags);
  const auto split_flags =
      UINT(D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) |
      UINT(D3D12_RESOURCE_BARRIER_FLAG_END_ONLY);
  if (flags & ~split_flags || flags == split_flags)
    return false;

  switch (barrier.Type) {
  case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
    return barrier.Transition.pResource &&
           IsValidTransitionState(barrier.Transition.StateBefore) &&
           IsValidTransitionState(barrier.Transition.StateAfter);
  case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
  case D3D12_RESOURCE_BARRIER_TYPE_UAV:
    return barrier.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE;
  default:
    return false;
  }
}

struct CompiledCommandBuildState {
  Com<ID3D12PipelineState> pipeline_state;
  Com<ID3D12RootSignature> compute_root_signature;
  Com<ID3D12RootSignature> graphics_root_signature;
  CompiledCommandDescriptorHeaps descriptor_heaps;
  std::vector<Com<ID3D12DescriptorHeap>> descriptor_heap_records;
  CompiledImmutableVector<Com<ID3D12DescriptorHeap>>
      descriptor_heap_snapshot;
  bool descriptor_heap_snapshot_dirty = false;
  std::vector<CompiledCommandRootDescriptorTable> compute_root_tables;
  std::vector<CompiledCommandRootDescriptorTable> graphics_root_tables;
  std::vector<CompiledCommandRootConstants> compute_root_constants;
  std::vector<CompiledCommandRootConstants> graphics_root_constants;
  std::vector<CompiledCommandRootDescriptor> compute_root_descriptors;
  std::vector<CompiledCommandRootDescriptor> graphics_root_descriptors;
  CompiledImmutableVector<CompiledCommandRootDescriptorTable>
      compute_root_table_snapshot;
  CompiledImmutableVector<CompiledCommandRootDescriptorTable>
      graphics_root_table_snapshot;
  CompiledImmutableVector<CompiledCommandRootConstants>
      compute_root_constant_snapshot;
  CompiledImmutableVector<CompiledCommandRootConstants>
      graphics_root_constant_snapshot;
  CompiledImmutableVector<CompiledCommandRootDescriptor>
      compute_root_descriptor_snapshot;
  CompiledImmutableVector<CompiledCommandRootDescriptor>
      graphics_root_descriptor_snapshot;
  std::optional<CompiledCommandPipelineMetadata> compute_pipeline_metadata;
  std::optional<CompiledCommandPipelineMetadata> graphics_pipeline_metadata;
  std::array<std::optional<D3D12_VERTEX_BUFFER_VIEW>,
             D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT>
      vertex_buffers;
  std::optional<D3D12_INDEX_BUFFER_VIEW> index_buffer;
  std::uint64_t vertex_buffer_dirty_mask = 0;
  bool index_buffer_dirty = false;
  CompiledCommandInputAssemblerState input_assembler_snapshot;
  bool predication_active = false;
  std::vector<DescriptorRecord> render_targets;
  std::optional<DescriptorRecord> depth_stencil;
  std::vector<D3D12_VIEWPORT> viewports;
  std::vector<D3D12_RECT> scissors;
  std::array<FLOAT, 4> blend_factor = {1.0f, 1.0f, 1.0f, 1.0f};
  UINT stencil_ref = 0;
  D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
  CompiledCommandRenderState render_state_snapshot;
  std::uint32_t compute_dirty_domains = 0;
  std::uint32_t graphics_dirty_domains = 0;
  std::uint64_t compute_root_table_dirty_mask = 0;
  std::uint64_t graphics_root_table_dirty_mask = 0;
  std::uint64_t compute_root_constant_dirty_mask = 0;
  std::uint64_t graphics_root_constant_dirty_mask = 0;
  std::uint64_t compute_root_descriptor_dirty_mask = 0;
  std::uint64_t graphics_root_descriptor_dirty_mask = 0;
};

static std::uint64_t RootParameterBit(UINT root_parameter_index) {
  return root_parameter_index < 64 ? 1ull << root_parameter_index : 0;
}

static void MarkCompiledDirty(CompiledCommandBuildState &state,
                              bool compute, std::uint32_t domains) {
  auto &dirty = compute ? state.compute_dirty_domains
                        : state.graphics_dirty_domains;
  dirty |= domains;
}

static CompiledCommandStateDelta
GetCompiledStateDelta(const CompiledCommandBuildState &state, bool compute) {
  CompiledCommandStateDelta delta = {};
  delta.dirty_domains = compute ? state.compute_dirty_domains
                                : state.graphics_dirty_domains;
  delta.root_table_dirty_mask =
      compute ? state.compute_root_table_dirty_mask
              : state.graphics_root_table_dirty_mask;
  delta.root_constant_dirty_mask =
      compute ? state.compute_root_constant_dirty_mask
              : state.graphics_root_constant_dirty_mask;
  delta.root_descriptor_dirty_mask =
      compute ? state.compute_root_descriptor_dirty_mask
              : state.graphics_root_descriptor_dirty_mask;
  return delta;
}

static void ClearCompiledStateDelta(CompiledCommandBuildState &state,
                                    bool compute) {
  if (compute) {
    state.compute_dirty_domains = 0;
    state.compute_root_table_dirty_mask = 0;
    state.compute_root_constant_dirty_mask = 0;
    state.compute_root_descriptor_dirty_mask = 0;
  } else {
    state.graphics_dirty_domains = 0;
    state.graphics_root_table_dirty_mask = 0;
    state.graphics_root_constant_dirty_mask = 0;
    state.graphics_root_descriptor_dirty_mask = 0;
  }
}

static PipelineState *
GetCompiledPipelineState(ID3D12PipelineState *pipeline_state) {
  return dynamic_cast<PipelineState *>(pipeline_state);
}

static bool
CompiledPipelineHasStage(const PipelineState &pipeline,
                         PipelineShaderStage stage) {
  const auto &shaders = pipeline.GetDxilShaders();
  return std::any_of(shaders.begin(), shaders.end(),
                     [stage](const PipelineDxilShader &shader) {
    return shader.stage == stage;
  });
}

static bool
CompiledPipelineUsesGeometry(const PipelineState &pipeline) {
  return CompiledPipelineHasStage(pipeline, PipelineShaderStage::Geometry);
}

static bool
CompiledPipelineUsesTessellation(const PipelineState &pipeline) {
  return CompiledPipelineHasStage(pipeline, PipelineShaderStage::Hull) ||
         CompiledPipelineHasStage(pipeline, PipelineShaderStage::Domain);
}

static ID3D12RootSignature *
ResolveCompiledRootSignature(const CompiledCommandBuildState &state,
                             bool compute) {
  return compute ? state.compute_root_signature.ptr()
                 : state.graphics_root_signature.ptr();
}

static UINT
CompiledDescriptorRangeOffset(const RootSignatureRange &range,
                              UINT running_offset) {
  return range.offset_in_descriptors_from_table_start == UINT_MAX
             ? running_offset
             : range.offset_in_descriptors_from_table_start;
}

static D3D12_DESCRIPTOR_HEAP_TYPE
CompiledDescriptorHeapTypeForRange(D3D12_DESCRIPTOR_RANGE_TYPE range_type) {
  return range_type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER
             ? D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
             : D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
}

static bool
CompiledRootParameterHasDescriptorRanges(
    const RootSignatureParameter &parameter,
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type,
    UINT *descriptor_count, bool *has_unbounded_range) {
  if (parameter.parameter_type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
    return false;

  bool has_matching_range = false;
  UINT running_offset = 0;
  UINT max_descriptor_count = 0;
  bool unbounded = false;
  for (const auto &range : parameter.ranges) {
    const auto range_heap_type =
        CompiledDescriptorHeapTypeForRange(range.range_type);
    const auto range_offset =
        CompiledDescriptorRangeOffset(range, running_offset);
    if (range_heap_type == heap_type) {
      has_matching_range = true;
      if (range.descriptor_count == UINT_MAX) {
        unbounded = true;
      } else {
        if (range_offset + range.descriptor_count < range_offset)
          return false;
        max_descriptor_count = std::max(
            max_descriptor_count, range_offset + range.descriptor_count);
      }
    }
    if (range.descriptor_count != UINT_MAX)
      running_offset = range_offset + range.descriptor_count;
  }

  if (descriptor_count)
    *descriptor_count = max_descriptor_count;
  if (has_unbounded_range)
    *has_unbounded_range = unbounded;
  return has_matching_range;
}

static CompiledCommandFallbackReason
CompiledNativeEligibilityFallbackReason(
    NativeShaderAbiEligibilityReason reason) {
  switch (reason) {
  case NativeShaderAbiEligibilityReason::None:
    return CompiledCommandFallbackReason::None;
  case NativeShaderAbiEligibilityReason::UnsupportedRootSignature:
    return CompiledCommandFallbackReason::NativeUnsupportedRootSignature;
  case NativeShaderAbiEligibilityReason::UnsupportedDescriptorRange:
    return CompiledCommandFallbackReason::NativeUnsupportedDescriptorRange;
  case NativeShaderAbiEligibilityReason::UnsupportedRootDescriptor:
    return CompiledCommandFallbackReason::NativeUnsupportedRootDescriptor;
  case NativeShaderAbiEligibilityReason::UnsupportedGeometryPipeline:
    return CompiledCommandFallbackReason::NativeUnsupportedGeometryPipeline;
  case NativeShaderAbiEligibilityReason::UnsupportedTessellationPipeline:
    return CompiledCommandFallbackReason::NativeUnsupportedTessellationPipeline;
  case NativeShaderAbiEligibilityReason::ShaderAbiMismatch:
    return CompiledCommandFallbackReason::NativeShaderAbiMismatch;
  }
  return CompiledCommandFallbackReason::NativeShaderAbiMismatch;
}

static CompiledCommandFallbackReason
CompiledPipelineFallbackReasonFromMetadata(
    const CompiledCommandPipelineMetadata &metadata, bool compute) {
  if (!metadata.has_pipeline_state)
    return CompiledCommandFallbackReason::MissingPipelineState;
  if (!metadata.has_dxmt_pipeline)
    return CompiledCommandFallbackReason::LegacyPipelineState;
  if (!metadata.type_matches)
    return CompiledCommandFallbackReason::LegacyPipelineState;
  if (!compute && metadata.uses_geometry)
    return CompiledCommandFallbackReason::NativeUnsupportedGeometryPipeline;
  if (!compute && metadata.uses_tessellation)
    return CompiledCommandFallbackReason::NativeUnsupportedTessellationPipeline;
  if (!metadata.ordinary_compiled) {
    const auto native_reason = CompiledNativeEligibilityFallbackReason(
        metadata.native_eligibility_reason);
    return native_reason != CompiledCommandFallbackReason::None
               ? native_reason
               : CompiledCommandFallbackReason::NonBindlessPipelineState;
  }
  if (!metadata.has_root_signature)
    return CompiledCommandFallbackReason::NativeUnsupportedRootSignature;
  if (metadata.uses_native_descriptor_table_abi &&
      metadata.native_eligibility_reason !=
          NativeShaderAbiEligibilityReason::None)
    return CompiledCommandFallbackReason::NativeShaderAbiMismatch;
  if (metadata.ordinary_compiled && !metadata.metal_pso_ready)
    return CompiledCommandFallbackReason::LegacyPipelineState;
  return CompiledCommandFallbackReason::None;
}

static void HashCompiledBindingLayoutValue(std::uint64_t &hash,
                                           std::uint64_t value) {
  hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
}

static void HashCompiledShaderArgumentLayout(
    std::uint64_t &hash, const DXMT12_MTL4_SHADER_ARGUMENT *arguments,
    std::uint32_t count) {
  HashCompiledBindingLayoutValue(hash, count);
  for (std::uint32_t index = 0; arguments && index < count; ++index) {
    const auto &argument = arguments[index];
    HashCompiledBindingLayoutValue(hash,
                                   static_cast<std::uint32_t>(argument.Type));
    HashCompiledBindingLayoutValue(hash, argument.SM50BindingSlot);
    HashCompiledBindingLayoutValue(hash,
                                   static_cast<std::uint32_t>(argument.Flags));
    HashCompiledBindingLayoutValue(hash, argument.StructurePtrOffset);
    HashCompiledBindingLayoutValue(hash, argument.RegisterSpace);
    HashCompiledBindingLayoutValue(hash, argument.RegisterLowerBound);
    HashCompiledBindingLayoutValue(hash, argument.RegisterCount);
    HashCompiledBindingLayoutValue(hash, argument.CBufferSizeInVec4);
  }
}

static std::uint64_t
CompiledPipelineBindingLayoutFingerprint(const PipelineState &pipeline) {
  std::uint64_t hash = 1469598103934665603ull;
  HashCompiledBindingLayoutValue(
      hash, static_cast<std::uint32_t>(pipeline.GetShaderAbiVersion()));
  for (const auto &shader : pipeline.GetDxilShaders()) {
    const auto &reflection = shader.reflection();
    HashCompiledBindingLayoutValue(hash,
                                   static_cast<std::uint32_t>(shader.stage));
    HashCompiledBindingLayoutValue(hash,
                                   reflection.ConstanttBufferTableBindIndex);
    HashCompiledBindingLayoutValue(hash, reflection.ArgumentBufferBindIndex);
    HashCompiledBindingLayoutValue(hash, reflection.ConstantBufferSlotMask);
    HashCompiledBindingLayoutValue(hash, reflection.SamplerSlotMask);
    HashCompiledBindingLayoutValue(hash, reflection.UAVSlotMask);
    HashCompiledBindingLayoutValue(hash, reflection.SRVSlotMaskLo);
    HashCompiledBindingLayoutValue(hash, reflection.SRVSlotMaskHi);
    HashCompiledBindingLayoutValue(hash, reflection.ArgumentTableQwords);
    HashCompiledShaderArgumentLayout(hash, shader.constantBufferInfo(),
                                     reflection.NumConstantBuffers);
    HashCompiledShaderArgumentLayout(hash, shader.resourceArgumentInfo(),
                                     reflection.NumArguments);
  }
  return hash;
}

static std::uint64_t
CompiledPipelineVertexLayoutFingerprint(const PipelineState &pipeline) {
  std::uint64_t hash = 1469598103934665603ull;
  const auto *graphics = pipeline.GetGraphicsState();
  if (!graphics)
    return hash;
  HashCompiledBindingLayoutValue(hash, graphics->input_elements.size());
  for (const auto &element : graphics->input_elements) {
    HashCompiledBindingLayoutValue(hash, element.SemanticIndex);
    HashCompiledBindingLayoutValue(hash, static_cast<std::uint32_t>(
                                             element.Format));
    HashCompiledBindingLayoutValue(hash, element.InputSlot);
    HashCompiledBindingLayoutValue(hash, element.AlignedByteOffset);
    HashCompiledBindingLayoutValue(
        hash, static_cast<std::uint32_t>(element.InputSlotClass));
    HashCompiledBindingLayoutValue(hash, element.InstanceDataStepRate);
  }
  return hash;
}

static CompiledCommandPipelineMetadata
BuildCompiledPipelineMetadata(const CompiledCommandBuildState &state,
                              bool compute) {
  CompiledCommandPipelineMetadata metadata = {};
  const auto expected_type = compute ? PipelineStateType::Compute
                                     : PipelineStateType::Graphics;
  metadata.type = expected_type;
  metadata.has_pipeline_state = bool(state.pipeline_state);
  if (!metadata.has_pipeline_state) {
    metadata.perf_fallback_reason =
        dxmt::CompiledFallbackReason::UnsupportedPipeline;
    return metadata;
  }

  auto *pipeline = GetCompiledPipelineState(state.pipeline_state.ptr());
  metadata.pipeline = pipeline;
  metadata.has_dxmt_pipeline = pipeline != nullptr;
  if (!pipeline) {
    metadata.perf_fallback_reason = dxmt::CompiledFallbackReason::LegacyPath;
    return metadata;
  }

  metadata.type = pipeline->GetType();
  metadata.binding_layout_fingerprint =
      CompiledPipelineBindingLayoutFingerprint(*pipeline);
  metadata.vertex_layout_fingerprint =
      CompiledPipelineVertexLayoutFingerprint(*pipeline);
  metadata.shader_abi_version = pipeline->GetShaderAbiVersion();
  // The shader ABI is selected when the PSO is created, against the PSO's
  // root signature. A command list may bind a distinct but compatible root
  // signature object; re-running ABI eligibility against that object's
  // identity/layout can incorrectly demote a PSO whose native artifact is
  // already valid. Packet materialization below still validates the live
  // command-list root table layout and handles before encoding.
  metadata.native_eligibility_reason = GetNativeShaderAbiEligibility(
      pipeline->GetDxilShaders(),
      GetDXMTRootSignature(pipeline->GetRootSignature()));
  metadata.uses_bindless_mirror_abi =
      metadata.shader_abi_version == DXMT12_MTL4_SHADER_ABI_BINDLESS_MIRROR;
  metadata.uses_native_descriptor_table_abi =
      metadata.shader_abi_version ==
      DXMT12_MTL4_SHADER_ABI_NATIVE_DESCRIPTOR_TABLE;
  metadata.type_matches = metadata.type == expected_type;
  metadata.has_root_signature =
      ResolveCompiledRootSignature(state, compute) != nullptr;
  metadata.uses_bindless_mirror = pipeline->UsesBindlessMirror();
  if (!compute) {
    metadata.uses_geometry = CompiledPipelineUsesGeometry(*pipeline);
    metadata.uses_tessellation = CompiledPipelineUsesTessellation(*pipeline);
  }
  metadata.ordinary_native =
      metadata.type_matches && metadata.has_root_signature &&
      metadata.uses_native_descriptor_table_abi &&
      metadata.native_eligibility_reason ==
          NativeShaderAbiEligibilityReason::None &&
      (compute || (!metadata.uses_geometry && !metadata.uses_tessellation));
  metadata.ordinary_bindless =
      metadata.type_matches && metadata.has_root_signature &&
      metadata.uses_bindless_mirror &&
      (compute || (!metadata.uses_geometry && !metadata.uses_tessellation));
  metadata.ordinary_compiled =
      metadata.ordinary_native || metadata.ordinary_bindless;
  if (metadata.ordinary_compiled) {
    if (compute) {
      metadata.metal_compute = pipeline->GetMetalComputeState();
      metadata.metal_pso_ready =
          metadata.metal_compute && metadata.metal_compute->pso;
    } else {
      metadata.metal_graphics = pipeline->GetMetalGraphicsState();
      metadata.metal_pso_ready =
          metadata.metal_graphics && metadata.metal_graphics->pso &&
          !metadata.metal_graphics->use_geometry &&
          !metadata.metal_graphics->use_tessellation;
    }
  }
  metadata.perf_fallback_reason = CompiledCommandFallbackReasonToPerf(
      CompiledPipelineFallbackReasonFromMetadata(metadata, compute));
  return metadata;
}

static const CompiledCommandPipelineMetadata &
GetCompiledPipelineMetadata(CompiledCommandBuildState &state, bool compute) {
  auto &cached = compute ? state.compute_pipeline_metadata
                         : state.graphics_pipeline_metadata;
  if (!cached)
    cached = BuildCompiledPipelineMetadata(state, compute);
  return *cached;
}

static bool
SetCompiledDescriptorHeaps(CompiledCommandBuildState &state,
                           const DescriptorHeapsRecord &record) {
  CompiledCommandDescriptorHeaps next = {};
  state.descriptor_heap_records = record.heaps;
  state.descriptor_heap_snapshot_dirty = true;
  for (const auto &heap : record.heaps) {
    auto *descriptor_heap = dynamic_cast<DescriptorHeap *>(heap.ptr());
    if (!descriptor_heap)
      continue;
    const auto &desc = descriptor_heap->GetDescriptorHeapDesc();
    if (!(desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE))
      continue;
    if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
      next.cbv_srv_uav = heap;
    else if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
      next.sampler = heap;
  }
  if (next.cbv_srv_uav.ptr() == state.descriptor_heaps.cbv_srv_uav.ptr() &&
      next.sampler.ptr() == state.descriptor_heaps.sampler.ptr()) {
    return false;
  }
  state.descriptor_heaps = std::move(next);
  state.compute_root_tables.clear();
  state.graphics_root_tables.clear();
  return true;
}

static bool
UpsertCompiledRootDescriptorTable(
    std::vector<CompiledCommandRootDescriptorTable> &tables,
    const RootDescriptorTableRecord &record) {
  for (auto &table : tables) {
    if (table.root_parameter_index == record.root_parameter_index) {
      if (table.base_descriptor.ptr == record.base_descriptor.ptr)
        return false;
      table = CompiledCommandRootDescriptorTable();
      table.base_descriptor = record.base_descriptor;
      table.root_parameter_index = record.root_parameter_index;
      return true;
    }
  }
  CompiledCommandRootDescriptorTable table = {};
  table.root_parameter_index = record.root_parameter_index;
  table.base_descriptor = record.base_descriptor;
  tables.push_back(table);
  return true;
}

static void
UpsertCompiledRootConstants(
    std::vector<CompiledCommandRootConstants> &constants,
    const RootConstantsRecord &record) {
  // No root signature is reachable from here, but D3D12 caps a whole root
  // signature at D3D12_MAX_ROOT_COST DWORDs and every 32-bit root constant
  // costs one DWORD, so no legal (dst_offset, count) pair can reach past that
  // cap. Evaluating the window in 64-bit and refusing the rest keeps the
  // std::copy calls below inside `merged`: the previous 32-bit adds wrapped
  // for a hostile dst_offset, producing a `local_size` far smaller than
  // `record.dst_offset - local_begin`.
  const uint64_t record_end =
      uint64_t(record.dst_offset) + record.values.size();
  if (record_end > D3D12_MAX_ROOT_COST) {
    static std::atomic<uint32_t> log_count = 0;
    if (log_count.fetch_add(1, std::memory_order_relaxed) < 8) {
      WARN("D3D12CommandList: root constants destination range is outside the"
           " root signature and was dropped rootParameterIndex=",
           record.root_parameter_index,
           " num32BitValuesToSet=", record.values.size(),
           " destOffsetIn32BitValues=", record.dst_offset);
    }
    return;
  }
  for (auto &entry : constants) {
    if (entry.root_parameter_index == record.root_parameter_index) {
      // Every stored entry went through the same cap above, so both ends fit
      // in D3D12_MAX_ROOT_COST and the merged window cannot overflow.
      const uint64_t entry_end =
          uint64_t(entry.dst_offset) + entry.values.size();
      const auto local_begin =
          std::min<UINT>(entry.dst_offset, record.dst_offset);
      const auto local_size = static_cast<UINT>(
          std::max<uint64_t>(entry_end, record_end) - local_begin);
      CompiledImmutableVector<UINT> merged;
      merged.resize(local_size, 0);
      std::copy(entry.values.begin(), entry.values.end(),
                merged.begin() + (entry.dst_offset - local_begin));
      std::copy(record.values.begin(), record.values.end(),
                merged.begin() + (record.dst_offset - local_begin));
      entry.dst_offset = local_begin;
      entry.values = std::move(merged);
      return;
    }
  }
  CompiledCommandRootConstants entry = {};
  entry.root_parameter_index = record.root_parameter_index;
  entry.dst_offset = record.dst_offset;
  entry.values = record.values;
  constants.push_back(std::move(entry));
}

static void
UpsertCompiledRootDescriptor(
    std::vector<CompiledCommandRootDescriptor> &descriptors,
    const RootDescriptorRecord &record) {
  for (auto &entry : descriptors) {
    if (entry.root_parameter_index == record.root_parameter_index &&
        entry.parameter_type == record.parameter_type) {
      entry.address = record.address;
      return;
    }
  }
  descriptors.push_back(CompiledCommandRootDescriptor{
      record.parameter_type, record.root_parameter_index, record.address});
}

static void
UpdateCompiledCommandBuildState(CompiledCommandBuildState &state,
                                const CommandRecordPayload &payload) {
  std::visit([&]<typename Record>(const Record &record) {
  if constexpr (std::is_same_v<Record, PipelineStateRecord>) {
    state.pipeline_state = record.pipeline_state;
    state.compute_pipeline_metadata.reset();
    state.graphics_pipeline_metadata.reset();
    MarkCompiledDirty(state, true, CompiledCommandStateDomainPipeline);
    MarkCompiledDirty(state, false, CompiledCommandStateDomainPipeline);
  } else if constexpr (std::is_same_v<Record, ClearStateRecord>) {
    state = CompiledCommandBuildState{};
    state.pipeline_state = record.pipeline_state;
    state.compute_dirty_domains = ~0u;
    state.graphics_dirty_domains = ~0u;
    state.compute_root_table_dirty_mask = ~0ull;
    state.graphics_root_table_dirty_mask = ~0ull;
    state.compute_root_constant_dirty_mask = ~0ull;
    state.graphics_root_constant_dirty_mask = ~0ull;
    state.compute_root_descriptor_dirty_mask = ~0ull;
    state.graphics_root_descriptor_dirty_mask = ~0ull;
  } else if constexpr (std::is_same_v<Record, DescriptorHeapsRecord>) {
    if (SetCompiledDescriptorHeaps(state, record)) {
      MarkCompiledDirty(
          state, true, CompiledCommandStateDomainDescriptorHeaps |
                           CompiledCommandStateDomainRootTables);
      MarkCompiledDirty(
          state, false, CompiledCommandStateDomainDescriptorHeaps |
                            CompiledCommandStateDomainRootTables);
      state.compute_root_table_dirty_mask = ~0ull;
      state.graphics_root_table_dirty_mask = ~0ull;
    }
  } else if constexpr (std::is_same_v<Record, RootSignatureRecord>) {
    if (record.compute) {
      state.compute_root_signature = record.root_signature;
      state.compute_root_tables.clear();
      state.compute_root_constants.clear();
      state.compute_root_descriptors.clear();
      state.compute_pipeline_metadata.reset();
      MarkCompiledDirty(
          state, true, CompiledCommandStateDomainRootSignature |
                           CompiledCommandStateDomainRootTables |
                           CompiledCommandStateDomainRootConstants |
                           CompiledCommandStateDomainRootDescriptors);
      state.compute_root_table_dirty_mask = ~0ull;
      state.compute_root_constant_dirty_mask = ~0ull;
      state.compute_root_descriptor_dirty_mask = ~0ull;
    } else {
      state.graphics_root_signature = record.root_signature;
      state.graphics_root_tables.clear();
      state.graphics_root_constants.clear();
      state.graphics_root_descriptors.clear();
      state.graphics_pipeline_metadata.reset();
      MarkCompiledDirty(
          state, false, CompiledCommandStateDomainRootSignature |
                            CompiledCommandStateDomainRootTables |
                            CompiledCommandStateDomainRootConstants |
                            CompiledCommandStateDomainRootDescriptors);
      state.graphics_root_table_dirty_mask = ~0ull;
      state.graphics_root_constant_dirty_mask = ~0ull;
      state.graphics_root_descriptor_dirty_mask = ~0ull;
    }
  } else if constexpr (std::is_same_v<Record, RootDescriptorTableRecord>) {
    auto &tables = record.compute ? state.compute_root_tables
                                   : state.graphics_root_tables;
    if (UpsertCompiledRootDescriptorTable(tables, record)) {
      MarkCompiledDirty(state, record.compute,
                        CompiledCommandStateDomainRootTables);
      auto &mask = record.compute ? state.compute_root_table_dirty_mask
                                   : state.graphics_root_table_dirty_mask;
      mask |= RootParameterBit(record.root_parameter_index);
    }
  } else if constexpr (std::is_same_v<Record, RootConstantsRecord>) {
    auto &constants = record.compute ? state.compute_root_constants
                                      : state.graphics_root_constants;
    UpsertCompiledRootConstants(constants, record);
    MarkCompiledDirty(state, record.compute,
                      CompiledCommandStateDomainRootConstants);
    auto &mask = record.compute ? state.compute_root_constant_dirty_mask
                                 : state.graphics_root_constant_dirty_mask;
    mask |= RootParameterBit(record.root_parameter_index);
  } else if constexpr (std::is_same_v<Record, RootDescriptorRecord>) {
    auto &descriptors = record.compute ? state.compute_root_descriptors
                                        : state.graphics_root_descriptors;
    UpsertCompiledRootDescriptor(descriptors, record);
    MarkCompiledDirty(state, record.compute,
                      CompiledCommandStateDomainRootDescriptors);
    auto &mask = record.compute ? state.compute_root_descriptor_dirty_mask
                                 : state.graphics_root_descriptor_dirty_mask;
    mask |= RootParameterBit(record.root_parameter_index);
  } else if constexpr (std::is_same_v<Record, PredicationRecord>) {
    state.predication_active = record.buffer.ptr() != nullptr;
  } else if constexpr (std::is_same_v<Record, VertexBuffersRecord>) {
    for (UINT i = 0;
         i < record.view_count &&
         record.start_slot + i < state.vertex_buffers.size();
         i++) {
      const auto slot = record.start_slot + i;
      if (i < record.views.size()) {
        state.vertex_buffers[slot] = record.views[i];
      } else {
        state.vertex_buffers[slot].reset();
      }
      if (slot < 64)
        state.vertex_buffer_dirty_mask |= 1ull << slot;
    }
    MarkCompiledDirty(state, false,
                      CompiledCommandStateDomainInputAssembler);
  } else if constexpr (std::is_same_v<Record, IndexBufferRecord>) {
    state.index_buffer = record.view;
    state.index_buffer_dirty = true;
    MarkCompiledDirty(state, false,
                      CompiledCommandStateDomainInputAssembler);
  } else if constexpr (std::is_same_v<Record, RenderTargetsRecord>) {
    state.render_targets = record.render_targets;
    state.depth_stencil = record.depth_stencil;
    MarkCompiledDirty(state, false,
                      CompiledCommandStateDomainRenderTargets);
  } else if constexpr (std::is_same_v<Record, ViewportRecord>) {
    state.viewports = record.viewports;
    MarkCompiledDirty(state, false, CompiledCommandStateDomainViewports);
  } else if constexpr (std::is_same_v<Record, ScissorRecord>) {
    state.scissors = record.rects;
    MarkCompiledDirty(state, false, CompiledCommandStateDomainScissors);
  } else if constexpr (std::is_same_v<Record, BlendFactorRecord>) {
    state.blend_factor = record.blend_factor;
    MarkCompiledDirty(state, false, CompiledCommandStateDomainBlendFactor);
  } else if constexpr (std::is_same_v<Record, StencilRefRecord>) {
    state.stencil_ref = record.stencil_ref;
    MarkCompiledDirty(state, false, CompiledCommandStateDomainStencilRef);
  } else if constexpr (std::is_same_v<Record, PrimitiveTopologyRecord>) {
    state.topology = record.topology;
    MarkCompiledDirty(state, false, CompiledCommandStateDomainTopology);
  }
  }, payload);
}

static void
FillCompiledPipelineBinding(CompiledCommandPipelineBinding &binding,
                            const CompiledCommandBuildState &state,
                            bool compute,
                            const CompiledCommandPipelineMetadata &metadata) {
  auto *root_signature =
      ResolveCompiledRootSignature(state, compute);
  binding.pipeline_state = state.pipeline_state;
  binding.root_signature = root_signature;
  binding.root_signature_impl = GetDXMTRootSignature(root_signature);
  binding.metadata = metadata;
  binding.pipeline_state_pending = !state.pipeline_state;
  binding.root_signature_pending = !root_signature;
  binding.bindless_candidate = binding.metadata.ordinary_compiled;
}

static void RefreshCompiledInputAssemblerSnapshot(
    CompiledCommandBuildState &state) {
  CompiledCommandInputAssemblerState result = {};
  result.index_buffer = state.index_buffer;
  for (UINT slot = 0; slot < state.vertex_buffers.size(); slot++) {
    const auto &vertex_buffer = state.vertex_buffers[slot];
    if (vertex_buffer)
      result.vertex_buffers.push_back(
          CompiledCommandVertexBuffer{slot, *vertex_buffer});
  }
  state.input_assembler_snapshot = std::move(result);
}

static void
ClearCompiledInputAssemblerDirtyState(CompiledCommandBuildState &state) {
  state.vertex_buffer_dirty_mask = 0;
  state.index_buffer_dirty = false;
}

static DescriptorHeap *
GetCompiledDescriptorHeap(const CompiledCommandDescriptorHeaps &heaps,
                          D3D12_DESCRIPTOR_HEAP_TYPE heap_type) {
  const auto &heap = heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                         ? heaps.sampler
                         : heaps.cbv_srv_uav;
  return dynamic_cast<DescriptorHeap *>(heap.ptr());
}

static CompiledCommandFallbackReason
MaterializeCompiledRootDescriptorTable(
    CompiledCommandRootDescriptorTable &table,
    const CompiledCommandDescriptorHeaps &heaps,
    const RootSignatureParameter &parameter) {
  if (!table.base_descriptor.ptr)
    return CompiledCommandFallbackReason::NativeDescriptorNullBase;

  D3D12_DESCRIPTOR_HEAP_TYPE heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
  UINT descriptor_count = 0;
  bool has_unbounded_resource_range = false;
  const bool has_resource_ranges = CompiledRootParameterHasDescriptorRanges(
      parameter, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, &descriptor_count,
      &has_unbounded_resource_range);
  UINT sampler_descriptor_count = 0;
  bool has_unbounded_sampler_range = false;
  const bool has_sampler_ranges = CompiledRootParameterHasDescriptorRanges(
      parameter, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
      &sampler_descriptor_count, &has_unbounded_sampler_range);
  if (has_resource_ranges && has_sampler_ranges)
    return CompiledCommandFallbackReason::NativeDescriptorMixedHeap;
  bool has_unbounded_range = false;
  if (has_resource_ranges) {
    heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    has_unbounded_range = has_unbounded_resource_range;
  } else if (has_sampler_ranges) {
    heap_type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    descriptor_count = sampler_descriptor_count;
    has_unbounded_range = has_unbounded_sampler_range;
  } else {
    return CompiledCommandFallbackReason::NativeDescriptorNoRange;
  }

  auto *heap = GetCompiledDescriptorHeap(heaps, heap_type);
  if (!heap)
    return CompiledCommandFallbackReason::NativeMissingDescriptorBackend;

  auto *mirror = heap->GetMirror();
  if (!mirror || !mirror->descriptorTableBackendReady())
    return CompiledCommandFallbackReason::NativeMissingDescriptorBackend;
  if (heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
      !mirror->textureViewPoolBaseResourceID())
    return CompiledCommandFallbackReason::NativeMissingDescriptorBackend;

  const auto *base = heap->GetDescriptorRecord(table.base_descriptor);
  if (!base || !base->shader_visible || base->heap_type != heap_type)
    return CompiledCommandFallbackReason::NativeDescriptorInvalidHandle;
  if (base->heap_index >= base->heap_count)
    return CompiledCommandFallbackReason::NativeDescriptorHeapTail;

  // An unbounded D3D12 range extends only as far as the currently bound heap.
  // The native ABI consumes shader-reflected finite spans, so retaining the
  // heap remainder here is both sufficient for validation and equivalent to
  // the legacy replay path's bounded reflection walk.
  if (has_unbounded_range)
    descriptor_count = base->heap_count - base->heap_index;

  table.heap_type = heap_type;
  table.heap_index = base->heap_index;
  table.descriptor_count = descriptor_count;
  table.heap_count = base->heap_count;
  table.table_offset =
      base->heap_index * table.table_entry_stride;
  table.root_table_base_descriptor_index = table.heap_index;
  table.descriptor_table_gpu_address = mirror->descriptorTableGpuAddress();
  table.descriptor_table_entry_gpu_address =
      table.descriptor_table_gpu_address + table.table_offset;
  table.buffer_descriptor_record_gpu_address =
      mirror->bufferDescriptorRecordGpuAddress();
  table.buffer_resource_table_gpu_address =
      mirror->bufferResourceTableGpuAddress();
  table.buffer_resource_table_generation =
      mirror->backendResourceTableGeneration();
  table.owning_heap = heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER
                          ? heaps.sampler
                          : heaps.cbv_srv_uav;
  table.mirror = mirror;
  table.resolved = true;
  table.descriptor_table_backend_ready = mirror->descriptorTableBackendReady();
  table.native_descriptor_record_storage_ready =
      mirror->nativeDescriptorRecordStorageReady();
  table.native_buffer_resource_table_ready =
      heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
      mirror->bufferResourceTableBuffer() &&
      mirror->bufferResourceTableGpuAddress();
  table.native_root_table_base_ready = true;
  return CompiledCommandFallbackReason::None;
}

static CompiledGraphicsPacket
BuildCompiledGraphicsPacket(const CommandRecord &record, UINT record_index,
                            CompiledCommandBuildState &state,
                            const CompiledCommandPipelineMetadata &metadata) {
  if (state.descriptor_heap_snapshot_dirty) {
    state.descriptor_heap_snapshot = state.descriptor_heap_records;
    state.descriptor_heap_snapshot_dirty = false;
  }
  if (state.graphics_dirty_domains & CompiledCommandStateDomainRootTables)
    state.graphics_root_table_snapshot = state.graphics_root_tables;
  if (state.graphics_dirty_domains & CompiledCommandStateDomainRootConstants)
    state.graphics_root_constant_snapshot = state.graphics_root_constants;
  if (state.graphics_dirty_domains &
      CompiledCommandStateDomainRootDescriptors)
    state.graphics_root_descriptor_snapshot = state.graphics_root_descriptors;
  if (state.graphics_dirty_domains & CompiledCommandStateDomainInputAssembler)
    RefreshCompiledInputAssemblerSnapshot(state);
  if (state.graphics_dirty_domains & CompiledCommandStateDomainRenderTargets) {
    state.render_state_snapshot.render_targets = state.render_targets;
    state.render_state_snapshot.depth_stencil = state.depth_stencil;
  }
  if (state.graphics_dirty_domains & CompiledCommandStateDomainViewports)
    state.render_state_snapshot.viewports = state.viewports;
  if (state.graphics_dirty_domains & CompiledCommandStateDomainScissors)
    state.render_state_snapshot.scissors = state.scissors;
  state.render_state_snapshot.blend_factor = state.blend_factor;
  state.render_state_snapshot.stencil_ref = state.stencil_ref;
  state.render_state_snapshot.topology = state.topology;

  CompiledGraphicsPacket packet = {};
  packet.record_index = record_index;
  packet.d3d_sequence = record.d3d_sequence;
  FillCompiledPipelineBinding(packet.pipeline, state, false, metadata);
  packet.descriptor_heaps = state.descriptor_heaps;
  packet.descriptor_heaps.all = state.descriptor_heap_snapshot;
  packet.root_tables = state.graphics_root_table_snapshot;
  packet.root_constants = state.graphics_root_constant_snapshot;
  packet.root_descriptors = state.graphics_root_descriptor_snapshot;
  packet.input_assembler = state.input_assembler_snapshot;
  packet.input_assembler.vertex_buffer_dirty_mask =
      state.vertex_buffer_dirty_mask;
  packet.input_assembler.index_buffer_dirty = state.index_buffer_dirty;
  packet.render_state = state.render_state_snapshot;
  packet.state_delta = GetCompiledStateDelta(state, false);
  if (const auto *draw = std::get_if<DrawInstancedRecord>(&record.payload))
    packet.draw = *draw;
  else if (const auto *draw_indexed =
               std::get_if<DrawIndexedInstancedRecord>(&record.payload))
    packet.draw_indexed = *draw_indexed;
  return packet;
}

static CompiledComputePacket
BuildCompiledComputePacket(const CommandRecord &record, UINT record_index,
                           CompiledCommandBuildState &state,
                           const CompiledCommandPipelineMetadata &metadata) {
  if (state.descriptor_heap_snapshot_dirty) {
    state.descriptor_heap_snapshot = state.descriptor_heap_records;
    state.descriptor_heap_snapshot_dirty = false;
  }
  if (state.compute_dirty_domains & CompiledCommandStateDomainRootTables)
    state.compute_root_table_snapshot = state.compute_root_tables;
  if (state.compute_dirty_domains & CompiledCommandStateDomainRootConstants)
    state.compute_root_constant_snapshot = state.compute_root_constants;
  if (state.compute_dirty_domains & CompiledCommandStateDomainRootDescriptors)
    state.compute_root_descriptor_snapshot = state.compute_root_descriptors;

  CompiledComputePacket packet = {};
  packet.record_index = record_index;
  packet.d3d_sequence = record.d3d_sequence;
  FillCompiledPipelineBinding(packet.pipeline, state, true, metadata);
  packet.descriptor_heaps = state.descriptor_heaps;
  packet.descriptor_heaps.all = state.descriptor_heap_snapshot;
  packet.root_tables = state.compute_root_table_snapshot;
  packet.root_constants = state.compute_root_constant_snapshot;
  packet.root_descriptors = state.compute_root_descriptor_snapshot;
  packet.state_delta = GetCompiledStateDelta(state, true);
  if (const auto *dispatch = std::get_if<DispatchRecord>(&record.payload))
    packet.dispatch = *dispatch;
  return packet;
}

static bool IsCompiledIndirectCompute(const ExecuteIndirectRecord &record) {
  auto *signature = dynamic_cast<CommandSignature *>(
      record.command_signature.ptr());
  if (!signature)
    return false;
  return std::any_of(
      signature->GetArguments().begin(), signature->GetArguments().end(),
      [](const D3D12_INDIRECT_ARGUMENT_DESC &argument) {
        return argument.Type == D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
      });
}

static CompiledIndirectPacket
BuildCompiledIndirectPacket(const CommandRecord &record, UINT record_index,
                            CompiledCommandBuildState &state) {
  CompiledIndirectPacket packet = {};
  packet.record_index = record_index;
  packet.d3d_sequence = record.d3d_sequence;
  packet.execute = std::get<ExecuteIndirectRecord>(record.payload);
  packet.compute = IsCompiledIndirectCompute(packet.execute);
  const auto &metadata = GetCompiledPipelineMetadata(state, packet.compute);
  if (packet.compute) {
    packet.compute_state = BuildCompiledComputePacket(
        record, record_index, state, metadata);
  } else {
    packet.graphics_state = BuildCompiledGraphicsPacket(
        record, record_index, state, metadata);
  }
  return packet;
}

static void
AppendCompiledFallbackSegment(
    std::vector<CompiledCommandSegment> &segments,
    UINT &unexpected_container_growths, UINT record_index,
    CompiledCommandFallbackReason reason) {
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.kind == CompiledCommandSegmentKind::Fallback &&
        last.fallback_reason == reason &&
        last.first_record_index + last.record_count == record_index) {
      last.record_count++;
      return;
    }
  }

  dxmt::perf::ScopedCodeTimer append_timer(
      dxmt::PerfCodePath::CompiledBuildSegmentAppend);
  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::Fallback;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  segment.fallback_reason = reason;
  segment.perf_fallback_reason = CompiledCommandFallbackReasonToPerf(reason);
  if (segments.size() == segments.capacity())
    unexpected_container_growths++;
  segments.push_back(segment);
}

static void
AppendCompiledTypedSegment(std::vector<CompiledCommandSegment> &segments,
                           UINT &unexpected_container_growths,
                           UINT record_index) {
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.kind == CompiledCommandSegmentKind::Typed &&
        last.first_record_index + last.record_count == record_index) {
      last.record_count++;
      return;
    }
  }

  dxmt::perf::ScopedCodeTimer append_timer(
      dxmt::PerfCodePath::CompiledBuildSegmentAppend);
  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::Typed;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  if (segments.size() == segments.capacity())
    unexpected_container_growths++;
  segments.push_back(segment);
}

static void AppendCompiledElidedStateSegment(
    std::vector<CompiledCommandSegment> &segments,
    UINT &unexpected_container_growths, UINT record_index) {
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.kind == CompiledCommandSegmentKind::ElidedState &&
        last.first_record_index + last.record_count == record_index) {
      last.record_count++;
      return;
    }
  }
  dxmt::perf::ScopedCodeTimer append_timer(
      dxmt::PerfCodePath::CompiledBuildSegmentAppend);
  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::ElidedState;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  if (segments.size() == segments.capacity())
    unexpected_container_growths++;
  segments.push_back(segment);
}

static void AppendCompiledBarrierSegment(
    std::vector<CompiledCommandSegment> &segments,
    UINT &unexpected_container_growths, UINT record_index,
    UINT first_barrier, UINT barrier_count) {
  dxmt::perf::ScopedCodeTimer append_timer(
      dxmt::PerfCodePath::CompiledBuildSegmentAppend);
  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::Barrier;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  segment.first_barrier = first_barrier;
  segment.barrier_count = barrier_count;
  if (segments.size() == segments.capacity())
    unexpected_container_growths++;
  segments.push_back(segment);
}

template <typename Record>
static auto &CompiledTransferPayloads(CompiledTransferStorage &storage) {
  if constexpr (std::is_same_v<Record, CopyBufferRegionRecord>)
    return storage.copy_buffer_regions;
  else if constexpr (std::is_same_v<Record, CopyTextureRegionRecord>)
    return storage.copy_texture_regions;
  else if constexpr (std::is_same_v<Record, CopyResourceRecord>)
    return storage.copy_resources;
  else if constexpr (std::is_same_v<Record, CopyTilesRecord>)
    return storage.copy_tiles;
  else if constexpr (std::is_same_v<Record, ResolveSubresourceRecord>)
    return storage.resolves;
  else if constexpr (std::is_same_v<Record, ClearRenderTargetRecord>)
    return storage.clear_render_targets;
  else if constexpr (std::is_same_v<Record, ClearDepthStencilRecord>)
    return storage.clear_depth_stencils;
  else if constexpr (std::is_same_v<Record, ClearUnorderedAccessRecord>)
    return storage.clear_unordered_access;
  else if constexpr (std::is_same_v<Record, DiscardResourceRecord>)
    return storage.discards;
  else if constexpr (std::is_same_v<Record, WriteBufferImmediateRecord>)
    return storage.write_buffer_immediate;
  else
    return storage.temporal_upscales;
}

template <typename Record>
static constexpr CompiledTransferOpcode CompiledTransferOpcodeFor() {
  if constexpr (std::is_same_v<Record, CopyBufferRegionRecord>)
    return CompiledTransferOpcode::CopyBufferRegion;
  else if constexpr (std::is_same_v<Record, CopyTextureRegionRecord>)
    return CompiledTransferOpcode::CopyTextureRegion;
  else if constexpr (std::is_same_v<Record, CopyResourceRecord>)
    return CompiledTransferOpcode::CopyResource;
  else if constexpr (std::is_same_v<Record, CopyTilesRecord>)
    return CompiledTransferOpcode::CopyTiles;
  else if constexpr (std::is_same_v<Record, ResolveSubresourceRecord>)
    return CompiledTransferOpcode::ResolveSubresource;
  else if constexpr (std::is_same_v<Record, ClearRenderTargetRecord>)
    return CompiledTransferOpcode::ClearRenderTarget;
  else if constexpr (std::is_same_v<Record, ClearDepthStencilRecord>)
    return CompiledTransferOpcode::ClearDepthStencil;
  else if constexpr (std::is_same_v<Record, ClearUnorderedAccessRecord>)
    return CompiledTransferOpcode::ClearUnorderedAccess;
  else if constexpr (std::is_same_v<Record, DiscardResourceRecord>)
    return CompiledTransferOpcode::DiscardResource;
  else if constexpr (std::is_same_v<Record, WriteBufferImmediateRecord>)
    return CompiledTransferOpcode::WriteBufferImmediate;
  else
    return CompiledTransferOpcode::TemporalUpscale;
}

template <typename Record>
static void AppendCompiledTransferPacket(
    CompiledCommandList &compiled, std::vector<CompiledCommandSegment> &segments,
    UINT record_index, std::uint64_t d3d_sequence, const Record &record) {
  auto &payloads =
      CompiledTransferPayloads<Record>(compiled.transfer_storage).mutableView();
  const auto payload_index = static_cast<UINT>(payloads.size());
  payloads.push_back(record);
  auto &packets = compiled.transfer_packets.mutableView();
  const auto packet_index = static_cast<UINT>(packets.size());
  packets.push_back(CompiledTransferPacket{
      CompiledTransferOpcodeFor<Record>(), payload_index, record_index,
      d3d_sequence});
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.kind == CompiledCommandSegmentKind::Transfer &&
        last.first_record_index + last.record_count == record_index &&
        last.first_transfer_packet + last.transfer_packet_count ==
            packet_index) {
      last.record_count++;
      last.transfer_packet_count++;
      return;
    }
  }
  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::Transfer;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  segment.first_transfer_packet = packet_index;
  segment.transfer_packet_count = 1;
  if (segments.size() == segments.capacity())
    compiled.unexpected_container_growths++;
  segments.push_back(segment);
}

template <typename Fn>
static bool VisitCompiledTransferRecord(const CommandRecord &record, Fn &&fn) {
  return std::visit([&]<typename Record>(const Record &payload) {
    if constexpr (std::is_same_v<Record, CopyBufferRegionRecord> ||
                  std::is_same_v<Record, CopyTextureRegionRecord> ||
                  std::is_same_v<Record, CopyResourceRecord> ||
                  std::is_same_v<Record, CopyTilesRecord> ||
                  std::is_same_v<Record, ResolveSubresourceRecord> ||
                  std::is_same_v<Record, ClearRenderTargetRecord> ||
                  std::is_same_v<Record, ClearDepthStencilRecord> ||
                  std::is_same_v<Record, ClearUnorderedAccessRecord> ||
                  std::is_same_v<Record, DiscardResourceRecord> ||
                  std::is_same_v<Record, WriteBufferImmediateRecord> ||
                  std::is_same_v<Record, TemporalUpscaleRecord>) {
      fn(payload);
      return true;
    }
    return false;
  }, record.payload);
}

template <typename Record>
static auto &CompiledControlPayloads(CompiledControlStorage &storage) {
  if constexpr (std::is_same_v<Record, ClearStateRecord>)
    return storage.clear_states;
  else if constexpr (std::is_same_v<Record, BeginQueryRecord>)
    return storage.begin_queries;
  else if constexpr (std::is_same_v<Record, EndQueryRecord>)
    return storage.end_queries;
  else if constexpr (std::is_same_v<Record, ResolveQueryDataRecord>)
    return storage.resolve_queries;
  else
    return storage.predications;
}

template <typename Record>
static constexpr CompiledControlOpcode CompiledControlOpcodeFor() {
  if constexpr (std::is_same_v<Record, ClearStateRecord>)
    return CompiledControlOpcode::ClearState;
  else if constexpr (std::is_same_v<Record, BeginQueryRecord>)
    return CompiledControlOpcode::BeginQuery;
  else if constexpr (std::is_same_v<Record, EndQueryRecord>)
    return CompiledControlOpcode::EndQuery;
  else if constexpr (std::is_same_v<Record, ResolveQueryDataRecord>)
    return CompiledControlOpcode::ResolveQueryData;
  else
    return CompiledControlOpcode::Predication;
}

template <typename Record>
static void AppendCompiledControlPacket(
    CompiledCommandList &compiled, std::vector<CompiledCommandSegment> &segments,
    UINT record_index, std::uint64_t d3d_sequence, const Record &record) {
  auto &payloads =
      CompiledControlPayloads<Record>(compiled.control_storage).mutableView();
  const auto payload_index = static_cast<UINT>(payloads.size());
  payloads.push_back(record);
  auto &packets = compiled.control_packets.mutableView();
  const auto packet_index = static_cast<UINT>(packets.size());
  packets.push_back(CompiledControlPacket{
      CompiledControlOpcodeFor<Record>(), payload_index, record_index,
      d3d_sequence});
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.kind == CompiledCommandSegmentKind::Control &&
        last.first_record_index + last.record_count == record_index &&
        last.first_control_packet + last.control_packet_count == packet_index) {
      last.record_count++;
      last.control_packet_count++;
      return;
    }
  }
  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::Control;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  segment.first_control_packet = packet_index;
  segment.control_packet_count = 1;
  if (segments.size() == segments.capacity())
    compiled.unexpected_container_growths++;
  segments.push_back(segment);
}

template <typename Fn>
static bool VisitCompiledControlRecord(const CommandRecord &record, Fn &&fn) {
  return std::visit([&]<typename Record>(const Record &payload) {
    if constexpr (std::is_same_v<Record, ClearStateRecord> ||
                  std::is_same_v<Record, BeginQueryRecord> ||
                  std::is_same_v<Record, EndQueryRecord> ||
                  std::is_same_v<Record, ResolveQueryDataRecord> ||
                  std::is_same_v<Record, PredicationRecord>) {
      fn(payload);
      return true;
    }
    return false;
  }, record.payload);
}

static void
AppendCompiledGraphicsSegment(
    std::vector<CompiledCommandSegment> &segments,
    std::vector<CompiledGraphicsPacket> &graphics_packets,
    UINT &unexpected_container_growths, UINT record_index,
    CompiledGraphicsPacket &&packet) {
  dxmt::perf::ScopedCodeTimer append_timer(
      dxmt::PerfCodePath::CompiledBuildSegmentAppend);
  const auto packet_index = static_cast<UINT>(graphics_packets.size());
  if (graphics_packets.size() == graphics_packets.capacity())
    unexpected_container_growths++;
  graphics_packets.push_back(std::move(packet));
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.kind == CompiledCommandSegmentKind::Graphics &&
        last.first_record_index + last.record_count == record_index &&
        last.first_graphics_packet + last.graphics_packet_count ==
            packet_index) {
      last.record_count++;
      last.graphics_packet_count++;
      return;
    }
  }

  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::Graphics;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  segment.first_graphics_packet = packet_index;
  segment.graphics_packet_count = 1;
  if (segments.size() == segments.capacity())
    unexpected_container_growths++;
  segments.push_back(segment);
}

static void
AppendCompiledComputeSegment(
    std::vector<CompiledCommandSegment> &segments,
    std::vector<CompiledComputePacket> &compute_packets,
    UINT &unexpected_container_growths, UINT record_index,
    CompiledComputePacket &&packet) {
  dxmt::perf::ScopedCodeTimer append_timer(
      dxmt::PerfCodePath::CompiledBuildSegmentAppend);
  const auto packet_index = static_cast<UINT>(compute_packets.size());
  if (compute_packets.size() == compute_packets.capacity())
    unexpected_container_growths++;
  compute_packets.push_back(std::move(packet));
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.kind == CompiledCommandSegmentKind::Compute &&
        last.first_record_index + last.record_count == record_index &&
        last.first_compute_packet + last.compute_packet_count == packet_index) {
      last.record_count++;
      last.compute_packet_count++;
      return;
    }
  }

  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::Compute;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  segment.first_compute_packet = packet_index;
  segment.compute_packet_count = 1;
  if (segments.size() == segments.capacity())
    unexpected_container_growths++;
  segments.push_back(segment);
}

static void
AppendCompiledIndirectSegment(
    std::vector<CompiledCommandSegment> &segments,
    std::vector<CompiledIndirectPacket> &indirect_packets,
    UINT &unexpected_container_growths, UINT record_index,
    CompiledIndirectPacket &&packet) {
  dxmt::perf::ScopedCodeTimer append_timer(
      dxmt::PerfCodePath::CompiledBuildSegmentAppend);
  const auto packet_index = static_cast<UINT>(indirect_packets.size());
  if (indirect_packets.size() == indirect_packets.capacity())
    unexpected_container_growths++;
  indirect_packets.push_back(std::move(packet));
  if (!segments.empty()) {
    auto &last = segments.back();
    if (last.kind == CompiledCommandSegmentKind::Indirect &&
        last.first_record_index + last.record_count == record_index &&
        last.first_indirect_packet + last.indirect_packet_count ==
            packet_index && packet_index &&
        indirect_packets[packet_index - 1].compute ==
            indirect_packets[packet_index].compute) {
      last.record_count++;
      last.indirect_packet_count++;
      return;
    }
  }

  CompiledCommandSegment segment = {};
  segment.kind = CompiledCommandSegmentKind::Indirect;
  segment.first_record_index = record_index;
  segment.record_count = 1;
  segment.first_indirect_packet = packet_index;
  segment.indirect_packet_count = 1;
  if (segments.size() == segments.capacity())
    unexpected_container_growths++;
  segments.push_back(segment);
}

struct CompiledResourceStateKey {
  ID3D12Resource *resource = nullptr;
  UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  bool operator==(const CompiledResourceStateKey &other) const {
    return resource == other.resource && subresource == other.subresource;
  }
};

struct CompiledResourceStateKeyHash {
  size_t operator()(const CompiledResourceStateKey &key) const {
    auto hash = std::hash<ID3D12Resource *>{}(key.resource);
    hash ^= size_t(key.subresource) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
    return hash;
  }
};

class CompiledAccessSummaryBuilder {
public:
  CompiledAccessSummaryBuilder(UINT barrier_record_count,
                               UINT barrier_count) {
    summary_.barrier_ranges.reserve(barrier_record_count);
    summary_.barriers.reserve(barrier_count);
    summary_.resource_state_deltas.reserve(barrier_count);
    size_t table_size = 1;
    const auto required_slots = std::max<size_t>(2, size_t(barrier_count) * 2);
    while (table_size < required_slots)
      table_size <<= 1;
    resource_state_delta_indices_.resize(table_size, 0);
  }

  void Append(UINT record_index, const ResourceBarrierRecord &record) {
    const auto epoch = ++summary_.final_barrier_epoch;
    const auto first_barrier = static_cast<UINT>(summary_.barriers.size());
    summary_.barriers.insert(summary_.barriers.end(), record.barriers.begin(),
                             record.barriers.end());
    summary_.barrier_ranges.push_back(CompiledCommandBarrierRange{
        record_index, first_barrier, static_cast<UINT>(record.barriers.size()),
        epoch});

    for (const auto &stored : record.barriers) {
      if (stored.barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
          !stored.resource)
        continue;
      const auto &transition = stored.barrier.Transition;
      const CompiledResourceStateKey key = {
          stored.resource.ptr(), transition.Subresource};
      const auto mask = resource_state_delta_indices_.size() - 1;
      auto slot = CompiledResourceStateKeyHash{}(key) & mask;
      size_t found_index = SIZE_MAX;
      while (resource_state_delta_indices_[slot]) {
        const auto candidate_index =
            resource_state_delta_indices_[slot] - 1;
        const auto &candidate =
            summary_.resource_state_deltas[candidate_index];
        if (candidate.resource.ptr() == key.resource &&
            candidate.subresource == key.subresource) {
          found_index = candidate_index;
          break;
        }
        slot = (slot + 1) & mask;
      }
      if (found_index == SIZE_MAX) {
        const auto index = summary_.resource_state_deltas.size();
        summary_.resource_state_deltas.push_back(
            CompiledCommandResourceStateDelta{
                stored.resource, transition.Subresource,
                transition.StateBefore, transition.StateAfter, epoch, epoch});
        resource_state_delta_indices_[slot] = index + 1;
      } else {
        auto &delta = summary_.resource_state_deltas[found_index];
        delta.export_state = transition.StateAfter;
        delta.last_epoch = epoch;
      }
    }
  }

  CompiledCommandAccessSummary Finish() {
    return std::move(summary_);
  }

private:
  CompiledCommandAccessSummary summary_;
  CompiledImmutableVector<size_t> resource_state_delta_indices_;
};

static UINT CountCompiledImmutableStateReuses(
    const CompiledCommandList &compiled) {
  UINT reuses = 0;
  for (size_t i = 1; i < compiled.graphics_packets.size(); ++i) {
    const auto &previous = compiled.graphics_packets[i - 1];
    const auto &current = compiled.graphics_packets[i];
    reuses += previous.root_tables.identity() == current.root_tables.identity();
    reuses += previous.root_constants.identity() ==
              current.root_constants.identity();
    reuses += previous.root_descriptors.identity() ==
              current.root_descriptors.identity();
    reuses += previous.input_assembler.vertex_buffers.identity() ==
              current.input_assembler.vertex_buffers.identity();
    reuses += previous.render_state.render_targets.identity() ==
              current.render_state.render_targets.identity();
    reuses += previous.render_state.viewports.identity() ==
              current.render_state.viewports.identity();
    reuses += previous.render_state.scissors.identity() ==
              current.render_state.scissors.identity();
  }
  for (size_t i = 1; i < compiled.compute_packets.size(); ++i) {
    const auto &previous = compiled.compute_packets[i - 1];
    const auto &current = compiled.compute_packets[i];
    reuses += previous.root_tables.identity() == current.root_tables.identity();
    reuses += previous.root_constants.identity() ==
              current.root_constants.identity();
    reuses += previous.root_descriptors.identity() ==
              current.root_descriptors.identity();
  }
  return reuses;
}

static bool CompiledRenderTargetViewEqual(
    const D3D12_RENDER_TARGET_VIEW_DESC &lhs,
    const D3D12_RENDER_TARGET_VIEW_DESC &rhs) {
  if (lhs.Format != rhs.Format || lhs.ViewDimension != rhs.ViewDimension)
    return false;
  switch (lhs.ViewDimension) {
  case D3D12_RTV_DIMENSION_BUFFER:
    return lhs.Buffer.FirstElement == rhs.Buffer.FirstElement &&
           lhs.Buffer.NumElements == rhs.Buffer.NumElements;
  case D3D12_RTV_DIMENSION_TEXTURE1D:
    return lhs.Texture1D.MipSlice == rhs.Texture1D.MipSlice;
  case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
    return lhs.Texture1DArray.MipSlice == rhs.Texture1DArray.MipSlice &&
           lhs.Texture1DArray.FirstArraySlice ==
               rhs.Texture1DArray.FirstArraySlice &&
           lhs.Texture1DArray.ArraySize == rhs.Texture1DArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE2D:
    return lhs.Texture2D.MipSlice == rhs.Texture2D.MipSlice &&
           lhs.Texture2D.PlaneSlice == rhs.Texture2D.PlaneSlice;
  case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
    return lhs.Texture2DArray.MipSlice == rhs.Texture2DArray.MipSlice &&
           lhs.Texture2DArray.FirstArraySlice ==
               rhs.Texture2DArray.FirstArraySlice &&
           lhs.Texture2DArray.ArraySize == rhs.Texture2DArray.ArraySize &&
           lhs.Texture2DArray.PlaneSlice == rhs.Texture2DArray.PlaneSlice;
  case D3D12_RTV_DIMENSION_TEXTURE2DMS:
    return true;
  case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
    return lhs.Texture2DMSArray.FirstArraySlice ==
               rhs.Texture2DMSArray.FirstArraySlice &&
           lhs.Texture2DMSArray.ArraySize == rhs.Texture2DMSArray.ArraySize;
  case D3D12_RTV_DIMENSION_TEXTURE3D:
    return lhs.Texture3D.MipSlice == rhs.Texture3D.MipSlice &&
           lhs.Texture3D.FirstWSlice == rhs.Texture3D.FirstWSlice &&
           lhs.Texture3D.WSize == rhs.Texture3D.WSize;
  default:
    return false;
  }
}

static bool CompiledDepthStencilViewEqual(
    const D3D12_DEPTH_STENCIL_VIEW_DESC &lhs,
    const D3D12_DEPTH_STENCIL_VIEW_DESC &rhs) {
  if (lhs.Format != rhs.Format || lhs.ViewDimension != rhs.ViewDimension ||
      lhs.Flags != rhs.Flags)
    return false;
  switch (lhs.ViewDimension) {
  case D3D12_DSV_DIMENSION_TEXTURE1D:
    return lhs.Texture1D.MipSlice == rhs.Texture1D.MipSlice;
  case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
    return lhs.Texture1DArray.MipSlice == rhs.Texture1DArray.MipSlice &&
           lhs.Texture1DArray.FirstArraySlice ==
               rhs.Texture1DArray.FirstArraySlice &&
           lhs.Texture1DArray.ArraySize == rhs.Texture1DArray.ArraySize;
  case D3D12_DSV_DIMENSION_TEXTURE2D:
    return lhs.Texture2D.MipSlice == rhs.Texture2D.MipSlice;
  case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
    return lhs.Texture2DArray.MipSlice == rhs.Texture2DArray.MipSlice &&
           lhs.Texture2DArray.FirstArraySlice ==
               rhs.Texture2DArray.FirstArraySlice &&
           lhs.Texture2DArray.ArraySize == rhs.Texture2DArray.ArraySize;
  case D3D12_DSV_DIMENSION_TEXTURE2DMS:
    return true;
  case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
    return lhs.Texture2DMSArray.FirstArraySlice ==
               rhs.Texture2DMSArray.FirstArraySlice &&
           lhs.Texture2DMSArray.ArraySize == rhs.Texture2DMSArray.ArraySize;
  default:
    return false;
  }
}

static bool CompiledAttachmentIdentityEqual(const DescriptorRecord &lhs,
                                            const DescriptorRecord &rhs) {
  if (lhs.type != rhs.type || lhs.resource.ptr() != rhs.resource.ptr() ||
      lhs.has_desc != rhs.has_desc)
    return false;
  // Descriptor handles and slot versions identify the D3D descriptor source,
  // not the Metal attachment. Like D3DMetal's active render encoder, keep the
  // encoder open when two immutable descriptor snapshots resolve to the same
  // resource and exact subresource view.
  if (!lhs.has_desc)
    return true;
  if (lhs.type == DescriptorRecordType::RenderTargetView)
    return CompiledRenderTargetViewEqual(lhs.desc.rtv, rhs.desc.rtv);
  if (lhs.type == DescriptorRecordType::DepthStencilView)
    return CompiledDepthStencilViewEqual(lhs.desc.dsv, rhs.desc.dsv);
  return false;
}

static bool CompiledRenderAttachmentsCompatible(
    const CompiledCommandRenderState &lhs,
    const CompiledCommandRenderState &rhs) {
  if (lhs.render_targets.size() != rhs.render_targets.size())
    return false;
  for (size_t i = 0; i < lhs.render_targets.size(); ++i) {
    if (!CompiledAttachmentIdentityEqual(lhs.render_targets[i],
                                         rhs.render_targets[i]))
      return false;
  }
  if (lhs.depth_stencil.has_value() != rhs.depth_stencil.has_value())
    return false;
  return !lhs.depth_stencil ||
         CompiledAttachmentIdentityEqual(*lhs.depth_stencil,
                                         *rhs.depth_stencil);
}

static UINT CompiledAttachmentMipLevel(const DescriptorRecord &attachment) {
  if (!attachment.has_desc)
    return 0;
  if (attachment.type == DescriptorRecordType::RenderTargetView) {
    const auto &rtv = attachment.desc.rtv;
    switch (rtv.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE1D:
      return rtv.Texture1D.MipSlice;
    case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
      return rtv.Texture1DArray.MipSlice;
    case D3D12_RTV_DIMENSION_TEXTURE2D:
      return rtv.Texture2D.MipSlice;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
      return rtv.Texture2DArray.MipSlice;
    case D3D12_RTV_DIMENSION_TEXTURE3D:
      return rtv.Texture3D.MipSlice;
    default:
      return 0;
    }
  }
  if (attachment.type == DescriptorRecordType::DepthStencilView) {
    const auto &dsv = attachment.desc.dsv;
    switch (dsv.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE1D:
      return dsv.Texture1D.MipSlice;
    case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
      return dsv.Texture1DArray.MipSlice;
    case D3D12_DSV_DIMENSION_TEXTURE2D:
      return dsv.Texture2D.MipSlice;
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
      return dsv.Texture2DArray.MipSlice;
    default:
      return 0;
    }
  }
  return 0;
}

static std::pair<UINT64, UINT64> CompiledAttachmentExtent(
    const CompiledCommandRenderState &state) {
  const DescriptorRecord *attachment = nullptr;
  for (const auto &target : state.render_targets) {
    if (target.resource) {
      attachment = &target;
      break;
    }
  }
  if (!attachment && state.depth_stencil && state.depth_stencil->resource)
    attachment = &*state.depth_stencil;
  auto *resource = attachment
                       ? dynamic_cast<Resource *>(attachment->resource.ptr())
                       : nullptr;
  if (!resource)
    return {};
  const auto &desc = resource->GetResourceDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
    return {};
  const UINT mip = CompiledAttachmentMipLevel(*attachment);
  return {std::max<UINT64>(1, desc.Width >> mip),
          desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D
              ? 1
              : std::max<UINT64>(1, UINT64(desc.Height) >> mip)};
}

static std::shared_ptr<const CompiledDynamicRenderStateRecipe>
BuildCompiledDynamicRenderStateRecipe(
    const CompiledCommandRenderState &state) {
  auto recipe = std::make_shared<CompiledDynamicRenderStateRecipe>();
  recipe->blend_factor = state.blend_factor;
  recipe->stencil_ref = static_cast<uint8_t>(state.stencil_ref);
  if (state.viewports.empty())
    return recipe;
  const auto [target_width, target_height] = CompiledAttachmentExtent(state);
  if (!target_width || !target_height)
    return recipe;

  auto &viewports = recipe->viewports.mutableView();
  viewports.reserve(state.viewports.size());
  for (const auto &viewport : state.viewports) {
    viewports.push_back({viewport.TopLeftX, viewport.TopLeftY, viewport.Width,
                         viewport.Height, viewport.MinDepth,
                         viewport.MaxDepth});
  }

  auto source_scissors = state.scissors.copy();
  if (source_scissors.empty()) {
    source_scissors.reserve(state.viewports.size());
    for (const auto &viewport : state.viewports) {
      source_scissors.push_back(
          {static_cast<LONG>(std::max(0.0f, viewport.TopLeftX)),
           static_cast<LONG>(std::max(0.0f, viewport.TopLeftY)),
           static_cast<LONG>(
               std::max(0.0f, viewport.TopLeftX + viewport.Width)),
           static_cast<LONG>(
               std::max(0.0f, viewport.TopLeftY + viewport.Height))});
    }
  }

  auto &scissors = recipe->scissors.mutableView();
  scissors.resize(state.viewports.size());
  const size_t count =
      std::min(source_scissors.size(), state.viewports.size());
  for (size_t index = 0; index < count; ++index) {
    const auto &rect = source_scissors[index];
    const int64_t left = std::max<int64_t>(0, rect.left);
    const int64_t top = std::max<int64_t>(0, rect.top);
    const int64_t right = std::max<int64_t>(left, rect.right);
    const int64_t bottom = std::max<int64_t>(top, rect.bottom);
    const uint64_t x = std::min<uint64_t>(left, target_width);
    const uint64_t y = std::min<uint64_t>(top, target_height);
    const uint64_t clamped_right =
        std::min<uint64_t>(right, target_width);
    const uint64_t clamped_bottom =
        std::min<uint64_t>(bottom, target_height);
    scissors[index] = {x, y, clamped_right - x, clamped_bottom - y};
  }
  recipe->valid = true;
  return recipe;
}

static void BuildCompiledDynamicRenderStateRecipes(
    CompiledCommandList &compiled) {
  const CompiledGraphicsPacket *previous = nullptr;
  for (auto &packet : compiled.graphics_packets.mutableView()) {
    if (previous &&
        previous->render_state.viewports.identity() ==
            packet.render_state.viewports.identity() &&
        previous->render_state.scissors.identity() ==
            packet.render_state.scissors.identity() &&
        previous->render_state.blend_factor ==
            packet.render_state.blend_factor &&
        previous->render_state.stencil_ref == packet.render_state.stencil_ref &&
        CompiledRenderAttachmentsCompatible(previous->render_state,
                                            packet.render_state)) {
      packet.dynamic_render_state_recipe =
          previous->dynamic_render_state_recipe;
    } else {
      packet.dynamic_render_state_recipe =
          BuildCompiledDynamicRenderStateRecipe(packet.render_state);
      compiled.close_dynamic_render_state_recipes++;
    }
    previous = &packet;
  }
}

static CompiledCommandFallbackReason BuildCompiledVertexBindingRecipe(
    const CompiledGraphicsPacket &packet,
    std::shared_ptr<const CompiledVertexBindingRecipe> &out) {
  auto *pipeline = packet.pipeline.metadata.pipeline;
  const auto *graphics = pipeline ? pipeline->GetGraphicsState() : nullptr;
  if (!graphics)
    return CompiledCommandFallbackReason::UnsupportedVertexIndexState;

  auto recipe = std::make_shared<CompiledVertexBindingRecipe>();
  for (const auto &element : graphics->input_elements) {
    if (element.InputSlot < 32)
      recipe->slot_mask |= 1u << element.InputSlot;
  }
  uint32_t populated_slot_mask = 0;
  auto &bindings = recipe->bindings.mutableView();
  bindings.reserve(packet.input_assembler.vertex_buffers.size());
  for (const auto &vb : packet.input_assembler.vertex_buffers) {
    if (vb.slot < 32)
      populated_slot_mask |= 1u << vb.slot;
    if (vb.slot >= 32 || !(recipe->slot_mask & (1u << vb.slot)))
      continue;
    CompiledVertexBindingRecipe::Binding binding = {};
    binding.slot = vb.slot;
    binding.stride = vb.view.StrideInBytes;
    if (vb.view.BufferLocation && vb.view.SizeInBytes) {
      UINT64 offset = 0;
      auto *resource = LookupBufferResourceByGpuVirtualAddress(
          vb.view.BufferLocation, &offset);
      if (!resource || !resource->GetBuffer() ||
          !resource->GetBufferAllocation())
        return packet.pipeline.metadata.uses_native_descriptor_table_abi
                   ? CompiledCommandFallbackReason::
                         NativeUnsupportedDynamicResource
                   : CompiledCommandFallbackReason::
                         UnsupportedVertexIndexState;
      binding.offset = resource->GetHeapOffset() + offset;
      binding.buffer = Rc<Buffer>(resource->GetBuffer());
      binding.allocation =
          Rc<BufferAllocation>(resource->GetBufferAllocation());
    }
    bindings.push_back(std::move(binding));
  }
  recipe->cleared_slot_mask =
      recipe->slot_mask &
      uint32_t(packet.input_assembler.vertex_buffer_dirty_mask) &
      ~populated_slot_mask;
  out = std::move(recipe);
  return CompiledCommandFallbackReason::None;
}

static void BuildCompiledVertexBindingRecipes(CompiledCommandList &compiled) {
  const CompiledGraphicsPacket *previous = nullptr;
  for (auto &packet : compiled.graphics_packets.mutableView()) {
    const bool can_reuse =
        previous &&
        previous->pipeline.metadata.vertex_layout_fingerprint ==
            packet.pipeline.metadata.vertex_layout_fingerprint &&
        previous->input_assembler.vertex_buffers.identity() ==
            packet.input_assembler.vertex_buffers.identity() &&
        previous->input_assembler.vertex_buffer_dirty_mask ==
            packet.input_assembler.vertex_buffer_dirty_mask;
    if (can_reuse) {
      packet.vertex_binding_recipe = previous->vertex_binding_recipe;
      packet.vertex_binding_recipe_reason =
          previous->vertex_binding_recipe_reason;
    } else {
      packet.vertex_binding_recipe_reason =
          BuildCompiledVertexBindingRecipe(packet,
                                           packet.vertex_binding_recipe);
      compiled.close_vertex_binding_recipes++;
    }
    previous = &packet;
  }
}

template <typename Packet>
static bool CompiledBindingProgramMatchesPacket(
    const CompiledBindingProgram &program, const Packet &packet,
    bool compute) {
  if (program.compute != compute ||
      program.pipeline.root_signature.ptr() !=
          packet.pipeline.root_signature.ptr() ||
      program.pipeline.metadata.shader_abi_version !=
          packet.pipeline.metadata.shader_abi_version ||
      program.pipeline.metadata.binding_layout_fingerprint !=
          packet.pipeline.metadata.binding_layout_fingerprint)
    return false;
  return true;
}

template <typename Packet>
static CompiledBindingDelta BuildCompiledBindingDelta(
    const Packet *previous_packet,
    const CompiledBindingProgram *previous_program, const Packet &packet,
    bool compute) {
  CompiledBindingDelta delta = {};
  const bool layout_changed =
      !previous_program || previous_program->compute != compute ||
      previous_program->pipeline.metadata.shader_abi_version !=
          packet.pipeline.metadata.shader_abi_version ||
      previous_program->pipeline.metadata.binding_layout_fingerprint !=
          packet.pipeline.metadata.binding_layout_fingerprint ||
      previous_program->pipeline.root_signature.ptr() !=
          packet.pipeline.root_signature.ptr();
  delta.full_bind = layout_changed;
  if (layout_changed)
    delta.dirty_fields |= CompiledBindingDirtyPipelineLayout;

  const bool resource_heap_changed =
      !previous_packet ||
      previous_packet->descriptor_heaps.cbv_srv_uav.ptr() !=
                       packet.descriptor_heaps.cbv_srv_uav.ptr();
  const bool sampler_heap_changed =
      !previous_packet ||
      previous_packet->descriptor_heaps.sampler.ptr() !=
                       packet.descriptor_heaps.sampler.ptr();
  if (resource_heap_changed)
    delta.dirty_fields |= CompiledBindingDirtyResourceHeap;
  if (sampler_heap_changed)
    delta.dirty_fields |= CompiledBindingDirtySamplerHeap;

  if (layout_changed || resource_heap_changed || sampler_heap_changed ||
      !previous_packet ||
      previous_packet->root_tables.identity() != packet.root_tables.identity()) {
    delta.dirty_fields |= CompiledBindingDirtyRootTables;
    delta.root_table_dirty_mask = packet.state_delta.root_table_dirty_mask;
    if (!delta.root_table_dirty_mask)
      delta.root_table_dirty_mask = UINT64_MAX;
  }
  if (layout_changed || !previous_packet ||
      previous_packet->root_constants.identity() !=
          packet.root_constants.identity()) {
    delta.dirty_fields |= CompiledBindingDirtyRootConstants;
    delta.root_constant_dirty_mask =
        packet.state_delta.root_constant_dirty_mask;
    if (!delta.root_constant_dirty_mask)
      delta.root_constant_dirty_mask = UINT64_MAX;
  }
  if (layout_changed || !previous_packet ||
      previous_packet->root_descriptors.identity() !=
          packet.root_descriptors.identity()) {
    delta.dirty_fields |= CompiledBindingDirtyRootDescriptors;
    delta.root_descriptor_dirty_mask =
        packet.state_delta.root_descriptor_dirty_mask;
    if (!delta.root_descriptor_dirty_mask)
      delta.root_descriptor_dirty_mask = UINT64_MAX;
  }
  if constexpr (std::is_same_v<Packet, CompiledGraphicsPacket>) {
    const auto previous_slots =
        previous_packet && previous_packet->vertex_binding_recipe
            ? previous_packet->vertex_binding_recipe->slot_mask
            : 0;
    const auto current_slots = packet.vertex_binding_recipe
                                   ? packet.vertex_binding_recipe->slot_mask
                                   : 0;
    if (layout_changed || !previous_packet ||
        previous_packet->vertex_binding_recipe.get() !=
            packet.vertex_binding_recipe.get()) {
      delta.dirty_fields |= CompiledBindingDirtyVertexBuffers;
      delta.vertex_buffer_dirty_mask =
          previous_slots | current_slots |
          static_cast<std::uint32_t>(
              packet.input_assembler.vertex_buffer_dirty_mask);
    }
  }
  return delta;
}

template <typename Packet>
static void BuildCompiledBindingProgramsForPackets(
    CompiledCommandList &compiled, std::vector<Packet> &packets,
    bool compute) {
  std::shared_ptr<const CompiledBindingProgram> previous_program;
  const Packet *previous_packet = nullptr;
  for (auto &packet : packets) {
    packet.binding_delta = BuildCompiledBindingDelta(
        previous_packet, previous_program.get(), packet, compute);
    packet.binding_delta.base_program = previous_program;
    if (previous_packet) {
      packet.binding_delta.base_resource_heap_identity =
          previous_packet->descriptor_heaps.cbv_srv_uav.ptr();
      packet.binding_delta.base_sampler_heap_identity =
          previous_packet->descriptor_heaps.sampler.ptr();
      packet.binding_delta.base_root_tables_identity =
          previous_packet->root_tables.identity();
      packet.binding_delta.base_root_constants_identity =
          previous_packet->root_constants.identity();
      packet.binding_delta.base_root_descriptors_identity =
          previous_packet->root_descriptors.identity();
      if constexpr (std::is_same_v<Packet, CompiledGraphicsPacket>)
        packet.binding_delta.base_vertex_bindings_identity =
            previous_packet->vertex_binding_recipe.get();
    }
    if (previous_program &&
        CompiledBindingProgramMatchesPacket(*previous_program, packet,
                                            compute)) {
      packet.binding_program = previous_program;
      compiled.close_binding_program_reuses++;
      previous_packet = &packet;
      continue;
    }

    auto program = std::make_shared<CompiledBindingProgram>();
    program->pipeline = packet.pipeline;
    program->compute = compute;
    packet.binding_program = program;
    previous_program = std::move(program);
    previous_packet = &packet;
    compiled.close_binding_programs++;
    if (packet.binding_delta.full_bind)
      compiled.close_full_binding_programs++;
    else
      compiled.close_delta_binding_programs++;
  }
}

static void BuildCompiledBindingPrograms(CompiledCommandList &compiled) {
  BuildCompiledBindingProgramsForPackets(
      compiled, compiled.graphics_packets.mutableView(), false);
  BuildCompiledBindingProgramsForPackets(
      compiled, compiled.compute_packets.mutableView(), true);
}

static void AddCompiledDirectBufferAllocation(
    std::vector<Rc<BufferAllocation>> &allocations,
    D3D12_GPU_VIRTUAL_ADDRESS address) {
  if (!address)
    return;
  UINT64 offset = 0;
  auto *resource =
      LookupBufferResourceByGpuVirtualAddress(address, &offset);
  (void)offset;
  auto *allocation = resource ? resource->GetBufferAllocation() : nullptr;
  if (!allocation)
    return;
  const bool exists = std::any_of(
      allocations.begin(), allocations.end(),
      [&](const Rc<BufferAllocation> &existing) {
        return existing.ptr() == allocation;
      });
  if (!exists)
    allocations.push_back(Rc<BufferAllocation>(allocation));
}

static void BuildCompiledDirectAccessPlans(CompiledCommandList &compiled) {
  const void *previous_graphics_roots = nullptr;
  const CompiledVertexBindingRecipe *previous_vertex_recipe = nullptr;
  CompiledImmutableVector<Rc<BufferAllocation>> previous_graphics_plan;
  for (auto &packet : compiled.graphics_packets.mutableView()) {
    if (previous_graphics_roots == packet.root_descriptors.identity() &&
        previous_vertex_recipe == packet.vertex_binding_recipe.get()) {
      packet.direct_buffer_allocations = previous_graphics_plan;
      continue;
    }
    std::vector<Rc<BufferAllocation>> allocations;
    allocations.reserve(packet.root_descriptors.size() +
                        (packet.vertex_binding_recipe
                             ? packet.vertex_binding_recipe->bindings.size()
                             : 0));
    for (const auto &root : packet.root_descriptors)
      AddCompiledDirectBufferAllocation(allocations, root.address);
    if (packet.vertex_binding_recipe) {
      for (const auto &binding : packet.vertex_binding_recipe->bindings) {
        if (!binding.allocation)
          continue;
        const bool exists = std::any_of(
            allocations.begin(), allocations.end(),
            [&](const Rc<BufferAllocation> &allocation) {
              return allocation.ptr() == binding.allocation.ptr();
            });
        if (!exists)
          allocations.push_back(binding.allocation);
      }
    }
    packet.direct_buffer_allocations = std::move(allocations);
    previous_graphics_roots = packet.root_descriptors.identity();
    previous_vertex_recipe = packet.vertex_binding_recipe.get();
    previous_graphics_plan = packet.direct_buffer_allocations;
    compiled.close_direct_access_plans++;
  }

  const void *previous_compute_roots = nullptr;
  CompiledImmutableVector<Rc<BufferAllocation>> previous_compute_plan;
  for (auto &packet : compiled.compute_packets.mutableView()) {
    if (previous_compute_roots == packet.root_descriptors.identity()) {
      packet.direct_buffer_allocations = previous_compute_plan;
      continue;
    }
    std::vector<Rc<BufferAllocation>> allocations;
    allocations.reserve(packet.root_descriptors.size());
    for (const auto &root : packet.root_descriptors)
      AddCompiledDirectBufferAllocation(allocations, root.address);
    packet.direct_buffer_allocations = std::move(allocations);
    previous_compute_roots = packet.root_descriptors.identity();
    previous_compute_plan = packet.direct_buffer_allocations;
    compiled.close_direct_access_plans++;
  }
}

static bool CanMergeCompiledEncoderNodes(
    const CompiledCommandList &compiled, const CompiledEncoderNode &lhs,
    const CompiledCommandSegment &rhs) {
  if (lhs.work.kind != rhs.kind)
    return false;
  if (rhs.kind == CompiledCommandSegmentKind::Compute) {
    return lhs.work.first_compute_packet + lhs.work.compute_packet_count ==
           rhs.first_compute_packet;
  }
  if (rhs.kind != CompiledCommandSegmentKind::Graphics ||
      lhs.work.first_graphics_packet + lhs.work.graphics_packet_count !=
          rhs.first_graphics_packet ||
      !lhs.work.graphics_packet_count || !rhs.graphics_packet_count)
    return false;
  const auto &previous = compiled.graphics_packets[
      lhs.work.first_graphics_packet + lhs.work.graphics_packet_count - 1];
  const auto &next = compiled.graphics_packets[rhs.first_graphics_packet];
  return CompiledRenderAttachmentsCompatible(previous.render_state,
                                             next.render_state);
}

static bool CompiledTransitionMatchesAttachment(
    const StoredResourceBarrier &stored, const DescriptorRecord &attachment) {
  auto *resource = stored.resource.ptr();
  if (!resource || attachment.resource.ptr() != resource)
    return false;
  const UINT subresource = stored.barrier.Transition.Subresource;
  if (subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
      !attachment.has_desc)
    return true;

  const auto desc = resource->GetDesc();
  const UINT mip_levels = std::max<UINT>(1, desc.MipLevels);
  const UINT array_size =
      desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
          ? 1
          : std::max<UINT>(1, desc.DepthOrArraySize);
  const UINT subresources_per_plane = mip_levels * array_size;
  if (!subresources_per_plane)
    return true;
  const UINT plane = subresource / subresources_per_plane;
  const UINT plane_subresource = subresource % subresources_per_plane;
  const UINT array_slice = plane_subresource / mip_levels;
  const UINT mip = plane_subresource % mip_levels;

  UINT attachment_mip = 0;
  UINT first_array_slice = 0;
  UINT array_slice_count = 1;
  UINT attachment_plane = 0;
  bool has_exact_plane = false;
  if (attachment.type == DescriptorRecordType::RenderTargetView) {
    const auto &rtv = attachment.desc.rtv;
    switch (rtv.ViewDimension) {
    case D3D12_RTV_DIMENSION_TEXTURE1D:
      attachment_mip = rtv.Texture1D.MipSlice;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE1DARRAY:
      attachment_mip = rtv.Texture1DArray.MipSlice;
      first_array_slice = rtv.Texture1DArray.FirstArraySlice;
      array_slice_count = rtv.Texture1DArray.ArraySize;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2D:
      attachment_mip = rtv.Texture2D.MipSlice;
      attachment_plane = rtv.Texture2D.PlaneSlice;
      has_exact_plane = true;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DARRAY:
      attachment_mip = rtv.Texture2DArray.MipSlice;
      first_array_slice = rtv.Texture2DArray.FirstArraySlice;
      array_slice_count = rtv.Texture2DArray.ArraySize;
      attachment_plane = rtv.Texture2DArray.PlaneSlice;
      has_exact_plane = true;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMS:
      break;
    case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY:
      first_array_slice = rtv.Texture2DMSArray.FirstArraySlice;
      array_slice_count = rtv.Texture2DMSArray.ArraySize;
      break;
    case D3D12_RTV_DIMENSION_TEXTURE3D:
      attachment_mip = rtv.Texture3D.MipSlice;
      break;
    default:
      return true;
    }
  } else if (attachment.type == DescriptorRecordType::DepthStencilView) {
    const auto &dsv = attachment.desc.dsv;
    switch (dsv.ViewDimension) {
    case D3D12_DSV_DIMENSION_TEXTURE1D:
      attachment_mip = dsv.Texture1D.MipSlice;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE1DARRAY:
      attachment_mip = dsv.Texture1DArray.MipSlice;
      first_array_slice = dsv.Texture1DArray.FirstArraySlice;
      array_slice_count = dsv.Texture1DArray.ArraySize;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2D:
      attachment_mip = dsv.Texture2D.MipSlice;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DARRAY:
      attachment_mip = dsv.Texture2DArray.MipSlice;
      first_array_slice = dsv.Texture2DArray.FirstArraySlice;
      array_slice_count = dsv.Texture2DArray.ArraySize;
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMS:
      break;
    case D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY:
      first_array_slice = dsv.Texture2DMSArray.FirstArraySlice;
      array_slice_count = dsv.Texture2DMSArray.ArraySize;
      break;
    default:
      return true;
    }
  } else {
    return true;
  }

  return mip == attachment_mip &&
         array_slice >= first_array_slice &&
         array_slice - first_array_slice < array_slice_count &&
         (!has_exact_plane || plane == attachment_plane);
}

static bool CompiledBarrierMatchesAttachments(
    const StoredResourceBarrier &stored,
    const CompiledCommandRenderState &render_state) {
  if (!stored.resource)
    return false;
  for (const auto &target : render_state.render_targets) {
    if (stored.barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
            ? CompiledTransitionMatchesAttachment(stored, target)
            : target.resource.ptr() == stored.resource.ptr())
      return true;
  }
  return render_state.depth_stencil &&
         (stored.barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
              ? CompiledTransitionMatchesAttachment(
                    stored, *render_state.depth_stencil)
              : render_state.depth_stencil->resource.ptr() ==
                    stored.resource.ptr());
}

static bool CompiledBarrierCanInlineIntoEncoder(
    const CompiledCommandList &compiled,
    const CompiledCommandSegment &barrier_segment,
    CompiledEncoderKind encoder_kind,
    const CompiledCommandRenderState *render_state) {
  if (barrier_segment.kind != CompiledCommandSegmentKind::Barrier ||
      encoder_kind == CompiledEncoderKind::None)
    return false;
  const UINT end = barrier_segment.first_barrier +
                   barrier_segment.barrier_count;
  if (end > compiled.access_summary.barriers.size())
    return false;
  for (UINT index = barrier_segment.first_barrier; index < end; ++index) {
    const auto &stored = compiled.access_summary.barriers[index];
    switch (stored.barrier.Type) {
    case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
    case D3D12_RESOURCE_BARRIER_TYPE_UAV:
      if (!stored.resource)
        return false;
      if (encoder_kind == CompiledEncoderKind::Graphics && render_state &&
          CompiledBarrierMatchesAttachments(stored, *render_state))
        return false;
      break;
    case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
    default:
      return false;
    }
  }
  return true;
}

static void AnnotateCompiledEncoderLifetimes(CompiledCommandList &compiled) {
  auto &nodes = compiled.encoder_graph.nodes.mutableView();
  CompiledEncoderKind active_kind = CompiledEncoderKind::None;
  UINT active_encoder = UINT_MAX;
  const CompiledCommandRenderState *active_render_state = nullptr;

  auto begin_encoder = [&](CompiledEncoderNode &node,
                           CompiledEncoderKind kind) {
    active_kind = kind;
    active_encoder = compiled.encoder_graph.encoder_count++;
    node.encoder_kind = kind;
    node.encoder_index = active_encoder;
    node.begins_encoder = true;
  };

  for (auto &node : nodes) {
    const auto &segment = node.work;
    if (segment.kind == CompiledCommandSegmentKind::Graphics &&
        segment.graphics_packet_count) {
      const auto &first = compiled.graphics_packets[
          segment.first_graphics_packet];
      if (active_kind != CompiledEncoderKind::Graphics ||
          !active_render_state ||
          !CompiledRenderAttachmentsCompatible(*active_render_state,
                                               first.render_state)) {
        begin_encoder(node, CompiledEncoderKind::Graphics);
      } else {
        node.encoder_kind = active_kind;
        node.encoder_index = active_encoder;
      }
      active_render_state = &compiled.graphics_packets[
          segment.first_graphics_packet + segment.graphics_packet_count - 1]
                                 .render_state;
      continue;
    }
    if (segment.kind == CompiledCommandSegmentKind::Compute &&
        segment.compute_packet_count) {
      if (active_kind != CompiledEncoderKind::Compute)
        begin_encoder(node, CompiledEncoderKind::Compute);
      else {
        node.encoder_kind = active_kind;
        node.encoder_index = active_encoder;
      }
      active_render_state = nullptr;
      continue;
    }
    if (segment.kind == CompiledCommandSegmentKind::Indirect &&
        segment.indirect_packet_count) {
      const auto &first = compiled.indirect_packets[
          segment.first_indirect_packet];
      if (first.compute) {
        if (active_kind != CompiledEncoderKind::Compute)
          begin_encoder(node, CompiledEncoderKind::Compute);
        else {
          node.encoder_kind = active_kind;
          node.encoder_index = active_encoder;
        }
        active_render_state = nullptr;
      } else if (first.state_packet_index <
                 compiled.graphics_packets.size()) {
        const auto &render_state =
            compiled.graphics_packets[first.state_packet_index].render_state;
        if (active_kind != CompiledEncoderKind::Graphics ||
            !active_render_state ||
            !CompiledRenderAttachmentsCompatible(*active_render_state,
                                                 render_state)) {
          begin_encoder(node, CompiledEncoderKind::Graphics);
        } else {
          node.encoder_kind = active_kind;
          node.encoder_index = active_encoder;
        }
        const auto &last = compiled.indirect_packets[
            segment.first_indirect_packet +
            segment.indirect_packet_count - 1];
        active_render_state =
            last.state_packet_index < compiled.graphics_packets.size()
                ? &compiled.graphics_packets[last.state_packet_index]
                       .render_state
                : &render_state;
      } else {
        active_kind = CompiledEncoderKind::None;
        active_encoder = UINT_MAX;
        active_render_state = nullptr;
      }
      continue;
    }
    if (segment.kind == CompiledCommandSegmentKind::Barrier &&
        CompiledBarrierCanInlineIntoEncoder(compiled, segment, active_kind,
                                            active_render_state)) {
      node.encoder_kind = active_kind;
      node.encoder_index = active_encoder;
      compiled.encoder_graph.inlined_barrier_node_count++;
      continue;
    }
    active_kind = CompiledEncoderKind::None;
    active_encoder = UINT_MAX;
    active_render_state = nullptr;
  }

  for (UINT index = 0; index < nodes.size(); ++index) {
    auto &node = nodes[index];
    if (node.encoder_index == UINT_MAX)
      continue;
    node.ends_encoder = index + 1 == nodes.size() ||
                        nodes[index + 1].encoder_index != node.encoder_index;
  }
}

static void BuildCompiledEncoderGraph(CompiledCommandList &compiled) {
  compiled.encoder_graph = {};
  compiled.encoder_graph.nodes.reserve(compiled.segments.size());
  auto &nodes = compiled.encoder_graph.nodes.mutableView();
  UINT elided_since_work = 0;
  UINT first_elided_record = 0;

  for (const auto &segment : compiled.segments) {
    if (segment.kind == CompiledCommandSegmentKind::ElidedState) {
      if (!elided_since_work)
        first_elided_record = segment.first_record_index;
      elided_since_work += segment.record_count;
      compiled.encoder_graph.elided_state_record_count +=
          segment.record_count;
      continue;
    }

    if (elided_since_work && !nodes.empty() &&
        CanMergeCompiledEncoderNodes(compiled, nodes.back(), segment)) {
      auto &node = nodes.back();
      node.work.record_count += segment.record_count;
      node.work.graphics_packet_count += segment.graphics_packet_count;
      node.work.compute_packet_count += segment.compute_packet_count;
      node.source_record_count =
          segment.first_record_index + segment.record_count -
          node.first_source_record_index;
      node.elided_state_record_count += elided_since_work;
      elided_since_work = 0;
      continue;
    }

    CompiledEncoderNode node = {};
    node.work = segment;
    node.predecessor_node =
        nodes.empty() ? UINT_MAX : static_cast<UINT>(nodes.size() - 1);
    node.first_source_record_index =
        elided_since_work ? first_elided_record : segment.first_record_index;
    node.source_record_count =
        segment.first_record_index + segment.record_count -
        node.first_source_record_index;
    node.elided_state_record_count = elided_since_work;
    nodes.push_back(std::move(node));
    elided_since_work = 0;
  }

  if (elided_since_work && !nodes.empty()) {
    auto &node = nodes.back();
    node.source_record_count =
        first_elided_record + elided_since_work -
        node.first_source_record_index;
    node.elided_state_record_count += elided_since_work;
  }
  AnnotateCompiledEncoderLifetimes(compiled);
}

struct CompiledStorageAllocationEventSnapshot {
  std::uint64_t nodes = 0;
  std::uint64_t state = 0;
  std::uint64_t access = 0;

  std::uint64_t Total() const { return nodes + state + access; }
};

static CompiledStorageAllocationEventSnapshot
CompiledStorageAllocationEventCount() {
  CompiledStorageAllocationEventSnapshot result;
  result.nodes =
      CompiledImmutableVector<CompiledCommandSegment>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CompiledEncoderNode>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CompiledGraphicsPacket>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CompiledComputePacket>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CompiledIndirectPacket>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CompiledTransferPacket>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CopyBufferRegionRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CopyTextureRegionRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CopyResourceRecord>::ThreadAllocationEventCount() +
      CompiledImmutableVector<CopyTilesRecord>::ThreadAllocationEventCount() +
      CompiledImmutableVector<ResolveSubresourceRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<ClearRenderTargetRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<ClearDepthStencilRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<ClearUnorderedAccessRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<DiscardResourceRecord>::ThreadAllocationEventCount() +
      CompiledImmutableVector<WriteBufferImmediateRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<TemporalUpscaleRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<CompiledControlPacket>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<ClearStateRecord>::ThreadAllocationEventCount() +
      CompiledImmutableVector<BeginQueryRecord>::ThreadAllocationEventCount() +
      CompiledImmutableVector<EndQueryRecord>::ThreadAllocationEventCount() +
      CompiledImmutableVector<ResolveQueryDataRecord>::
          ThreadAllocationEventCount() +
      CompiledImmutableVector<PredicationRecord>::ThreadAllocationEventCount() +
      CompiledImmutableVector<std::uint8_t>::ThreadAllocationEventCount();
  result.state =
      CompiledImmutableVector<CompiledCommandRootDescriptorTable>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<CompiledCommandRootConstants>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<CompiledCommandRootDescriptor>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<CompiledCommandVertexBuffer>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<DescriptorRecord>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<D3D12_VIEWPORT>::ThreadAllocationEventCount() +
         CompiledImmutableVector<D3D12_RECT>::ThreadAllocationEventCount() +
         CompiledImmutableVector<WMTViewport>::ThreadAllocationEventCount() +
         CompiledImmutableVector<WMTScissorRect>::ThreadAllocationEventCount() +
         CompiledImmutableVector<CompiledVertexBindingRecipe::Binding>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<Rc<BufferAllocation>>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<Com<ID3D12DescriptorHeap>>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<UINT>::ThreadAllocationEventCount();
  result.access =
      CompiledImmutableVector<CompiledCommandBarrierRange>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<StoredResourceBarrier>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<CompiledCommandResourceStateDelta>::
             ThreadAllocationEventCount() +
         CompiledImmutableVector<size_t>::ThreadAllocationEventCount();
  return result;
}

static void FinalizeCompiledStorageAllocationEvents(
    CompiledCommandList &compiled,
    const CompiledStorageAllocationEventSnapshot &before) {
  const auto after = CompiledStorageAllocationEventCount();
  compiled.node_storage_allocation_events =
      static_cast<UINT>(after.nodes - before.nodes);
  compiled.state_storage_allocation_events =
      static_cast<UINT>(after.state - before.state);
  compiled.access_storage_allocation_events =
      static_cast<UINT>(after.access - before.access);
  compiled.storage_allocation_events =
      compiled.node_storage_allocation_events +
      compiled.state_storage_allocation_events +
      compiled.access_storage_allocation_events;
}

static bool IsCompiledGraphicsStateSetterRecord(const CommandRecord &record) {
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

static bool IsCompiledPacketCapturedStateRecord(const CommandRecord &record) {
  return IsCompiledGraphicsStateSetterRecord(record) ||
         std::holds_alternative<RenderTargetsRecord>(record.payload);
}

static bool LaterCompiledVertexBufferRecordCoversSlot(
    const std::vector<CommandRecord> &records, UINT begin, UINT end,
    UINT slot) {
  for (UINT i = begin; i < end; ++i) {
    if (const auto *vb =
            std::get_if<VertexBuffersRecord>(&records[i].payload)) {
      if (slot >= vb->start_slot && slot < vb->start_slot + vb->view_count)
        return true;
    }
  }
  return false;
}

static bool CompiledGraphicsStateSetterSupersededInRun(
    const std::vector<CommandRecord> &records, UINT index, UINT run_end) {
  const auto &payload = records[index].payload;
  for (UINT i = index + 1; i < run_end; ++i) {
    const auto &later = records[i].payload;
    if ((std::holds_alternative<PipelineStateRecord>(payload) &&
         std::holds_alternative<PipelineStateRecord>(later)) ||
        (std::holds_alternative<ViewportRecord>(payload) &&
         std::holds_alternative<ViewportRecord>(later)) ||
        (std::holds_alternative<ScissorRecord>(payload) &&
         std::holds_alternative<ScissorRecord>(later)) ||
        (std::holds_alternative<BlendFactorRecord>(payload) &&
         std::holds_alternative<BlendFactorRecord>(later)) ||
        (std::holds_alternative<StencilRefRecord>(payload) &&
         std::holds_alternative<StencilRefRecord>(later)) ||
        (std::holds_alternative<PrimitiveTopologyRecord>(payload) &&
         std::holds_alternative<PrimitiveTopologyRecord>(later)) ||
        (std::holds_alternative<IndexBufferRecord>(payload) &&
         std::holds_alternative<IndexBufferRecord>(later)))
      return true;
    if (const auto *table =
            std::get_if<RootDescriptorTableRecord>(&payload)) {
      if (const auto *later_table =
              std::get_if<RootDescriptorTableRecord>(&later)) {
        if (table->compute == later_table->compute &&
            table->root_parameter_index == later_table->root_parameter_index)
          return true;
      }
    }
    if (const auto *descriptor =
            std::get_if<RootDescriptorRecord>(&payload)) {
      if (const auto *later_descriptor =
              std::get_if<RootDescriptorRecord>(&later)) {
        if (descriptor->compute == later_descriptor->compute &&
            descriptor->parameter_type == later_descriptor->parameter_type &&
            descriptor->root_parameter_index ==
                later_descriptor->root_parameter_index)
          return true;
      }
    }
  }

  if (const auto *vb = std::get_if<VertexBuffersRecord>(&payload)) {
    if (!vb->view_count)
      return true;
    for (UINT slot = vb->start_slot; slot < vb->start_slot + vb->view_count;
         ++slot) {
      if (!LaterCompiledVertexBufferRecordCoversSlot(
              records, index + 1, run_end, slot))
        return false;
    }
    return true;
  }
  return false;
}

static CompiledImmutableVector<std::uint8_t>
BuildCompiledSupersededGraphicsStateRecordMask(
    const std::vector<CommandRecord> &records) {
  CompiledImmutableVector<std::uint8_t> result;
  auto &skip = result.mutableView();
  skip.assign(records.size(), 0);
  for (UINT i = 0; i < records.size();) {
    if (!IsCompiledGraphicsStateSetterRecord(records[i])) {
      ++i;
      continue;
    }
    UINT run_end = i + 1;
    while (run_end < records.size() &&
           IsCompiledGraphicsStateSetterRecord(records[run_end]))
      ++run_end;
    for (UINT k = i; k < run_end; ++k) {
      if (CompiledGraphicsStateSetterSupersededInRun(records, k, run_end))
        skip[k] = 1;
    }
    i = run_end;
  }
  return result;
}

static void PreMaterializeCompiledRootTables(CompiledCommandList &compiled);

static std::shared_ptr<CompiledCommandList>
BuildCompiledCommandList(const std::vector<CommandRecord> &records,
                         WMT::Device device, bool force_compatibility,
                         UINT graphics_packet_count,
                         UINT compute_packet_count,
                         UINT barrier_record_count,
                         UINT barrier_count) {
  (void)device;
  const auto allocation_events_before =
      CompiledStorageAllocationEventCount();
  dxmt::perf::ScopedCodeTimer loop_timer(
      dxmt::PerfCodePath::CompiledBuildLoopDispatch);
  auto compiled = std::make_shared<CompiledCommandList>();
  static std::atomic<std::uint64_t> next_generation = 0;
  compiled->generation =
      next_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  compiled->record_count = static_cast<UINT>(records.size());
  compiled->superseded_state_record_mask =
      BuildCompiledSupersededGraphicsStateRecordMask(records);
  compiled->segments.reserve(records.size());
  const auto indirect_packet_count = std::count_if(
      records.begin(), records.end(), [](const CommandRecord &record) {
        return std::holds_alternative<ExecuteIndirectRecord>(record.payload);
      });
  // Every indirect node also contributes one ordinary immutable state packet
  // so submission freezing and binding-program compilation cover it.
  compiled->graphics_packets.reserve(graphics_packet_count +
                                      indirect_packet_count);
  compiled->compute_packets.reserve(compute_packet_count +
                                    indirect_packet_count);
  compiled->indirect_packets.reserve(indirect_packet_count);
  // Compilation owns these stores exclusively. Hold their mutable views once
  // so per-record appends do not repeatedly execute the immutable wrapper's
  // cross-thread uniqueness check; the stores become immutable when this
  // function publishes the generation.
  auto &segments = compiled->segments.mutableView();
  auto &graphics_packets = compiled->graphics_packets.mutableView();
  auto &compute_packets = compiled->compute_packets.mutableView();
  auto &indirect_packets = compiled->indirect_packets.mutableView();
  CompiledAccessSummaryBuilder access_summary_builder(barrier_record_count,
                                                      barrier_count);
  if (force_compatibility) {
    for (UINT record_index = 0; record_index < records.size(); ++record_index) {
      if (records[record_index].compile_kind ==
          CommandRecordCompileKind::Barrier)
        access_summary_builder.Append(
            record_index,
            std::get<ResourceBarrierRecord>(records[record_index].payload));
      AppendCompiledTypedSegment(segments,
                                 compiled->unexpected_container_growths,
                                 record_index);
    }
    compiled->access_summary = access_summary_builder.Finish();
    PreMaterializeCompiledRootTables(*compiled);
    BuildCompiledDynamicRenderStateRecipes(*compiled);
    BuildCompiledVertexBindingRecipes(*compiled);
    BuildCompiledBindingPrograms(*compiled);
    BuildCompiledDirectAccessPlans(*compiled);
    BuildCompiledEncoderGraph(*compiled);
    FinalizeCompiledStorageAllocationEvents(*compiled,
                                            allocation_events_before);
    return compiled;
  }

  constexpr const char *packet_fault_name =
      "DXMT_TEST_FAIL_NATIVE_PACKET_ALLOCATION_AT";
  const auto packet_fault_setting = env::getEnvVar(packet_fault_name);
  constexpr const char *finalization_fault_name =
      "DXMT_TEST_FAIL_NATIVE_SEGMENT_FINALIZATION_AT";
  const auto finalization_fault_setting =
      env::getEnvVar(finalization_fault_name);
  CompiledCommandBuildState state = {};
  UINT next_barrier_index = 0;
  for (UINT record_index = 0; record_index < records.size(); record_index++) {
    const auto &record = records[record_index];
    if (record.compile_kind == CommandRecordCompileKind::Barrier)
      access_summary_builder.Append(
          record_index, std::get<ResourceBarrierRecord>(record.payload));
    if (record.compile_kind == CommandRecordCompileKind::Graphics) {
      const CompiledCommandPipelineMetadata *metadata_ptr = nullptr;
      {
        dxmt::perf::ScopedCodeTimer metadata_timer(
            dxmt::PerfCodePath::CompiledBuildPipelineMetadata);
        metadata_ptr = &GetCompiledPipelineMetadata(state, false);
      }
      const auto &metadata = *metadata_ptr;
      const auto reason = state.predication_active
                              ? CompiledCommandFallbackReason::QueryOrPredication
                              : CompiledPipelineFallbackReasonFromMetadata(
                                    metadata, false);
      if (reason == CompiledCommandFallbackReason::None) {
        if (ShouldInjectCompiledCommandFault(
                packet_fault_setting,
                g_test_native_packet_allocation_occurrence)) {
          RecordInjectedCompiledCommandFault(packet_fault_name);
          WARN("D3D12CommandList: injected native packet allocation "
               "failure at graphics record ",
               record_index);
          AppendCompiledFallbackSegment(
              segments, compiled->unexpected_container_growths, record_index,
              CompiledCommandFallbackReason::
                  InjectedNativePacketAllocationFailure);
        } else {
          CompiledGraphicsPacket packet;
          {
            dxmt::perf::ScopedCodeTimer packet_timer(
                dxmt::PerfCodePath::CompiledBuildPacketStateCopy);
            packet = BuildCompiledGraphicsPacket(record, record_index, state,
                                                 metadata);
          }
          AppendCompiledGraphicsSegment(
              segments, graphics_packets,
              compiled->unexpected_container_growths, record_index,
              std::move(packet));
          ClearCompiledInputAssemblerDirtyState(state);
          ClearCompiledStateDelta(state, false);
        }
      } else {
        CompiledGraphicsPacket packet;
        {
          dxmt::perf::ScopedCodeTimer packet_timer(
              dxmt::PerfCodePath::CompiledBuildPacketStateCopy);
          packet = BuildCompiledGraphicsPacket(record, record_index, state,
                                               metadata);
          packet.compatibility_reason = reason;
        }
        AppendCompiledGraphicsSegment(
            segments, graphics_packets,
            compiled->unexpected_container_growths, record_index,
            std::move(packet));
        ClearCompiledInputAssemblerDirtyState(state);
        ClearCompiledStateDelta(state, false);
      }
    } else if (record.compile_kind == CommandRecordCompileKind::Compute) {
      const CompiledCommandPipelineMetadata *metadata_ptr = nullptr;
      {
        dxmt::perf::ScopedCodeTimer metadata_timer(
            dxmt::PerfCodePath::CompiledBuildPipelineMetadata);
        metadata_ptr = &GetCompiledPipelineMetadata(state, true);
      }
      const auto &metadata = *metadata_ptr;
      const auto reason = state.predication_active
                              ? CompiledCommandFallbackReason::QueryOrPredication
                              : CompiledPipelineFallbackReasonFromMetadata(
                                    metadata, true);
      if (reason == CompiledCommandFallbackReason::None) {
        if (ShouldInjectCompiledCommandFault(
                packet_fault_setting,
                g_test_native_packet_allocation_occurrence)) {
          RecordInjectedCompiledCommandFault(packet_fault_name);
          WARN("D3D12CommandList: injected native packet allocation "
               "failure at compute record ",
               record_index);
          AppendCompiledFallbackSegment(
              segments, compiled->unexpected_container_growths, record_index,
              CompiledCommandFallbackReason::
                  InjectedNativePacketAllocationFailure);
        } else {
          CompiledComputePacket packet;
          {
            dxmt::perf::ScopedCodeTimer packet_timer(
                dxmt::PerfCodePath::CompiledBuildPacketStateCopy);
            packet = BuildCompiledComputePacket(record, record_index, state,
                                                metadata);
          }
          AppendCompiledComputeSegment(
              segments, compute_packets,
              compiled->unexpected_container_growths, record_index,
              std::move(packet));
          ClearCompiledStateDelta(state, true);
        }
      } else {
        CompiledComputePacket packet;
        {
          dxmt::perf::ScopedCodeTimer packet_timer(
              dxmt::PerfCodePath::CompiledBuildPacketStateCopy);
          packet = BuildCompiledComputePacket(record, record_index, state,
                                              metadata);
          packet.compatibility_reason = reason;
        }
        AppendCompiledComputeSegment(
            segments, compute_packets,
            compiled->unexpected_container_growths, record_index,
            std::move(packet));
        ClearCompiledStateDelta(state, true);
      }
    } else if (std::holds_alternative<ExecuteIndirectRecord>(
                   record.payload)) {
      CompiledIndirectPacket packet;
      {
        dxmt::perf::ScopedCodeTimer packet_timer(
            dxmt::PerfCodePath::CompiledBuildPacketStateCopy);
        packet = BuildCompiledIndirectPacket(record, record_index, state);
      }
      if (packet.compute && packet.compute_state) {
        packet.state_packet_index =
            static_cast<UINT>(compute_packets.size());
        compute_packets.push_back(std::move(*packet.compute_state));
        packet.compute_state.reset();
      } else if (!packet.compute && packet.graphics_state) {
        packet.state_packet_index =
            static_cast<UINT>(graphics_packets.size());
        graphics_packets.push_back(std::move(*packet.graphics_state));
        packet.graphics_state.reset();
      }
      AppendCompiledIndirectSegment(
          segments, indirect_packets,
          compiled->unexpected_container_growths, record_index,
          std::move(packet));
      if (indirect_packets.back().compute) {
        ClearCompiledStateDelta(state, true);
      } else {
        ClearCompiledInputAssemblerDirtyState(state);
        ClearCompiledStateDelta(state, false);
      }
    } else if (const auto *barriers =
                   std::get_if<ResourceBarrierRecord>(&record.payload)) {
      AppendCompiledBarrierSegment(
          segments, compiled->unexpected_container_growths, record_index,
          next_barrier_index, static_cast<UINT>(barriers->barriers.size()));
      next_barrier_index += static_cast<UINT>(barriers->barriers.size());
    } else if (VisitCompiledTransferRecord(record, [&](const auto &payload) {
                 AppendCompiledTransferPacket(
                     *compiled, segments, record_index, record.d3d_sequence,
                     payload);
               })) {
    } else if (VisitCompiledControlRecord(record, [&](const auto &payload) {
                 AppendCompiledControlPacket(
                     *compiled, segments, record_index, record.d3d_sequence,
                     payload);
               })) {
    } else if (IsCompiledPacketCapturedStateRecord(record)) {
      AppendCompiledElidedStateSegment(
          segments, compiled->unexpected_container_growths, record_index);
    } else {
      AppendCompiledTypedSegment(segments,
                                 compiled->unexpected_container_growths,
                                 record_index);
    }
    {
      dxmt::perf::ScopedCodeTimer state_timer(
          dxmt::PerfCodePath::CompiledBuildStateUpdate);
      UpdateCompiledCommandBuildState(state, record.payload);
    }
  }

  const bool has_compiled_segment = std::any_of(
      compiled->segments.begin(), compiled->segments.end(),
      [](const CompiledCommandSegment &segment) {
        return segment.kind != CompiledCommandSegmentKind::Fallback;
      });
  const bool injected_finalization_failure =
      has_compiled_segment &&
      ShouldInjectCompiledCommandFault(
          finalization_fault_setting,
          g_test_native_segment_finalization_occurrence);
  if (injected_finalization_failure) {
    RecordInjectedCompiledCommandFault(finalization_fault_name);
    WARN("D3D12CommandList: injected native segment finalization failure");
  }

  if (injected_finalization_failure) {
    dxmt::perf::ScopedCodeTimer fallback_timer(
        dxmt::PerfCodePath::CompiledBuildFallbackRewrite);
    for (auto &segment : compiled->segments) {
      if (segment.kind == CompiledCommandSegmentKind::Fallback)
        continue;
      segment.kind = CompiledCommandSegmentKind::Fallback;
      segment.graphics_packet_count = 0;
      segment.compute_packet_count = 0;
      segment.indirect_packet_count = 0;
      segment.fallback_reason = CompiledCommandFallbackReason::
          InjectedNativeSegmentFinalizationFailure;
      segment.perf_fallback_reason = CompiledCommandFallbackReasonToPerf(
          segment.fallback_reason);
    }
    // Every compiled segment was rewritten above. Retaining unreachable
    // packets would still let submission snapshot their descriptor state
    // before fallback replay, which is precisely what this fault path must
    // avoid.
    compiled->graphics_packets.clear();
    compiled->compute_packets.clear();
    compiled->indirect_packets.clear();
  }

  compiled->access_summary = access_summary_builder.Finish();
  compiled->immutable_state_reuses =
      CountCompiledImmutableStateReuses(*compiled);
  PreMaterializeCompiledRootTables(*compiled);
  BuildCompiledDynamicRenderStateRecipes(*compiled);
  BuildCompiledVertexBindingRecipes(*compiled);
  BuildCompiledBindingPrograms(*compiled);
  BuildCompiledDirectAccessPlans(*compiled);
  BuildCompiledEncoderGraph(*compiled);
  FinalizeCompiledStorageAllocationEvents(*compiled,
                                          allocation_events_before);

  return compiled;
}

static CompiledCommandFallbackReason MaterializeSubmittedRootTables(
    std::vector<CompiledCommandRootDescriptorTable> &tables,
    const CompiledCommandDescriptorHeaps &heaps, RootSignature *root) {
  if (!root)
    return CompiledCommandFallbackReason::NativeUnsupportedRootSignature;

  const auto parameters = root->GetParameters();
  for (auto &table : tables) {
    if (table.root_parameter_index >= parameters.size())
      return CompiledCommandFallbackReason::NativeUnsupportedRootSignature;
    const auto &parameter = parameters[table.root_parameter_index];
    if (parameter.parameter_type != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
      return CompiledCommandFallbackReason::NativeDescriptorNoRange;

    table.resolved = false;
    table.owning_heap = nullptr;
    table.mirror = nullptr;
    const auto reason =
        MaterializeCompiledRootDescriptorTable(table, heaps, parameter);
    if (reason != CompiledCommandFallbackReason::None)
      return reason;
  }
  return CompiledCommandFallbackReason::None;
}

static CompiledCommandFallbackReason RefreshSubmittedRootTables(
    std::vector<CompiledCommandRootDescriptorTable> &tables) {
  for (auto &table : tables) {
    auto *mirror = table.mirror;
    if (!table.resolved || !table.owning_heap || !mirror)
      return CompiledCommandFallbackReason::NativeMissingDescriptorBackend;
    if (!mirror->descriptorTableBackendReady())
      return CompiledCommandFallbackReason::NativeMissingDescriptorBackend;
    if (table.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        !mirror->textureViewPoolBaseResourceID())
      return CompiledCommandFallbackReason::NativeMissingDescriptorBackend;

    table.descriptor_table_gpu_address = mirror->descriptorTableGpuAddress();
    table.descriptor_table_entry_gpu_address =
        table.descriptor_table_gpu_address + table.table_offset;
    table.buffer_descriptor_record_gpu_address =
        mirror->bufferDescriptorRecordGpuAddress();
    table.buffer_resource_table_gpu_address =
        mirror->bufferResourceTableGpuAddress();
    table.buffer_resource_table_generation =
        mirror->backendResourceTableGeneration();
    table.descriptor_table_backend_ready =
        mirror->descriptorTableBackendReady();
    table.native_descriptor_record_storage_ready =
        mirror->nativeDescriptorRecordStorageReady();
    table.native_buffer_resource_table_ready =
        table.heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV &&
        mirror->bufferResourceTableBuffer() &&
        mirror->bufferResourceTableGpuAddress();
    table.native_root_table_base_ready = true;
  }
  return CompiledCommandFallbackReason::None;
}

static void PreMaterializeCompiledRootTables(CompiledCommandList &compiled) {
  struct Key {
    const void *table_identity = nullptr;
    ID3D12DescriptorHeap *resource_heap = nullptr;
    ID3D12DescriptorHeap *sampler_heap = nullptr;
    RootSignature *root = nullptr;
    bool operator==(const Key &) const = default;
  };
  struct KeyHash {
    size_t operator()(const Key &key) const {
      size_t hash = std::hash<const void *>{}(key.table_identity);
      auto mix = [&](const void *value) {
        hash ^= std::hash<const void *>{}(value) + 0x9e3779b97f4a7c15ull +
                (hash << 6) + (hash >> 2);
      };
      mix(key.resource_heap);
      mix(key.sampler_heap);
      mix(key.root);
      return hash;
    }
  };
  struct Result {
    CompiledImmutableVector<CompiledCommandRootDescriptorTable> tables;
    bool ready = false;
  };
  std::unordered_map<Key, Result, KeyHash> cache;

  auto prepare_packet = [&](auto &packet) {
    auto *root = GetDXMTRootSignature(packet.pipeline.root_signature.ptr());
    const Key key = {packet.root_tables.identity(),
                     packet.descriptor_heaps.cbv_srv_uav.ptr(),
                     packet.descriptor_heaps.sampler.ptr(), root};
    if (const auto found = cache.find(key); found != cache.end()) {
      if (found->second.ready) {
        packet.root_tables = found->second.tables;
        packet.root_tables_close_materialized = true;
      }
      return;
    }

    Result result;
    auto &tables = result.tables.mutableView();
    tables = packet.root_tables.copy();
    result.ready =
        MaterializeSubmittedRootTables(tables, packet.descriptor_heaps, root) ==
        CompiledCommandFallbackReason::None;
    if (result.ready) {
      packet.root_tables = result.tables;
      packet.root_tables_close_materialized = true;
      compiled.close_materialized_root_table_sets++;
    }
    cache.emplace(key, std::move(result));
  };

  for (auto &packet : compiled.graphics_packets.mutableView())
    prepare_packet(packet);
  for (auto &packet : compiled.compute_packets.mutableView())
    prepare_packet(packet);
}

std::shared_ptr<SubmittedCompiledCommandListPlan>
PrepareSubmittedCompiledCommandListImpl(
    std::shared_ptr<const CompiledCommandList> compiled) {
  auto plan = std::make_shared<SubmittedCompiledCommandListPlan>();
  plan->generation = std::move(compiled);
  if (!plan->generation)
    return plan;

  struct MaterializedTableKey {
    const void *table_identity = nullptr;
    ID3D12DescriptorHeap *resource_heap = nullptr;
    ID3D12DescriptorHeap *sampler_heap = nullptr;
    RootSignature *root = nullptr;

    bool operator==(const MaterializedTableKey &) const = default;
  };
  struct MaterializedTableKeyHash {
    size_t operator()(const MaterializedTableKey &key) const {
      size_t hash = std::hash<const void *>{}(key.table_identity);
      auto mix = [&](const void *value) {
        hash ^= std::hash<const void *>{}(value) + 0x9e3779b97f4a7c15ull +
                (hash << 6) + (hash >> 2);
      };
      mix(key.resource_heap);
      mix(key.sampler_heap);
      mix(key.root);
      return hash;
    }
  };
  struct MaterializedTables {
    std::shared_ptr<const std::vector<CompiledCommandRootDescriptorTable>>
        tables;
    CompiledCommandFallbackReason reason =
        CompiledCommandFallbackReason::None;
  };
  std::unordered_map<MaterializedTableKey, MaterializedTables,
                     MaterializedTableKeyHash>
      materialized_table_cache;
  auto materialize_tables = [&](const auto &packet,
                                RootSignature *root) -> MaterializedTables {
    const MaterializedTableKey key = {
        packet.root_tables.identity(),
        packet.descriptor_heaps.cbv_srv_uav.ptr(),
        packet.descriptor_heaps.sampler.ptr(), root};
    if (const auto found = materialized_table_cache.find(key);
        found != materialized_table_cache.end())
      return found->second;
    auto tables = std::make_shared<
        std::vector<CompiledCommandRootDescriptorTable>>(
        packet.root_tables.copy());
    const bool close_materialized = packet.root_tables_close_materialized;
    const auto reason = close_materialized
                            ? RefreshSubmittedRootTables(*tables)
                            : MaterializeSubmittedRootTables(
                                  *tables, packet.descriptor_heaps, root);
    if (plan->generation->test_telemetry) {
      auto &counter =
          close_materialized
              ? plan->generation->test_telemetry
                    ->submitted_root_table_fast_patches
              : plan->generation->test_telemetry
                    ->submitted_root_table_full_materializations;
      counter.fetch_add(1, std::memory_order_relaxed);
    }
    MaterializedTables result = {std::move(tables), reason};
    materialized_table_cache.emplace(key, result);
    return result;
  };
  plan->graphics_packets.reserve(plan->generation->graphics_packets.size());
  plan->compute_packets.reserve(plan->generation->compute_packets.size());
  for (const auto &packet : plan->generation->graphics_packets) {
    SubmittedCompiledGraphicsPacket submitted = {};
    if (packet.compatibility_reason !=
        CompiledCommandFallbackReason::None) {
      submitted.prepare_reason = packet.compatibility_reason;
      plan->graphics_packets.push_back(std::move(submitted));
      continue;
    }
    auto *root = GetDXMTRootSignature(packet.pipeline.root_signature.ptr());
    const auto materialized = materialize_tables(packet, root);
    submitted.root_tables = materialized.tables;
    submitted.prepare_reason = materialized.reason;
    plan->graphics_packets.push_back(std::move(submitted));
    if (plan->generation->test_telemetry) {
      plan->generation->test_telemetry->submitted_graphics_packets.fetch_add(
          1, std::memory_order_relaxed);
      if (materialized.reason != CompiledCommandFallbackReason::None)
        plan->generation->test_telemetry->submission_prepare_failures.fetch_add(
            1, std::memory_order_relaxed);
    }
  }

  for (const auto &packet : plan->generation->compute_packets) {
    SubmittedCompiledComputePacket submitted = {};
    if (packet.compatibility_reason !=
        CompiledCommandFallbackReason::None) {
      submitted.prepare_reason = packet.compatibility_reason;
      plan->compute_packets.push_back(std::move(submitted));
      continue;
    }
    auto *root = GetDXMTRootSignature(packet.pipeline.root_signature.ptr());
    const auto materialized = materialize_tables(packet, root);
    submitted.root_tables = materialized.tables;
    submitted.prepare_reason = materialized.reason;
    plan->compute_packets.push_back(std::move(submitted));
    if (plan->generation->test_telemetry) {
      plan->generation->test_telemetry->submitted_compute_packets.fetch_add(
          1, std::memory_order_relaxed);
      if (materialized.reason != CompiledCommandFallbackReason::None)
        plan->generation->test_telemetry->submission_prepare_failures.fetch_add(
            1, std::memory_order_relaxed);
    }
  }
  return plan;
}

static bool
IsCompiledWorkRecord(const CommandRecordPayload &payload) {
  return std::holds_alternative<DrawInstancedRecord>(payload) ||
         std::holds_alternative<DrawIndexedInstancedRecord>(payload) ||
         std::holds_alternative<DispatchRecord>(payload);
}

static bool
SegmentCompilesWorkRecord(const CompiledCommandSegment &segment,
                          const CommandRecordPayload &payload,
                          UINT record_index) {
  if (!segment.record_count ||
      record_index < segment.first_record_index ||
      record_index - segment.first_record_index >= segment.record_count)
    return false;
  if (std::holds_alternative<DispatchRecord>(payload))
    return segment.kind == CompiledCommandSegmentKind::Compute &&
           segment.compute_packet_count != 0;
  return segment.kind == CompiledCommandSegmentKind::Graphics &&
         segment.graphics_packet_count != 0;
}

static void
ApplyExecutionPathTestConfig(
    CompiledCommandList &compiled, const std::vector<CommandRecord> &records,
    const dxmt::d3d12::test::ExecutionPathConfig &config) {
  using dxmt::d3d12::test::ExecutionPathFlagInjectEmptyFallbackSegment;
  using dxmt::d3d12::test::ExecutionPathFlagInjectEmptyNativeSegment;
  using dxmt::d3d12::test::ExecutionPathMode;

  compiled.test_path_mode = config.mode;
  compiled.test_telemetry =
      std::make_shared<CompiledCommandTestTelemetry>();

  for (UINT record_index = 0; record_index < records.size(); ++record_index) {
    const auto &payload = records[record_index].payload;
    if (!IsCompiledWorkRecord(payload))
      continue;
    compiled.test_work_record_count++;
    const bool selected = std::any_of(
        compiled.segments.begin(), compiled.segments.end(),
        [&](const CompiledCommandSegment &segment) {
          return SegmentCompilesWorkRecord(segment, payload, record_index);
        });
    if (selected)
      compiled.test_compiled_work_record_count++;
  }
  compiled.test_native_requirement_satisfied =
      config.mode != ExecutionPathMode::NativeCompiled ||
      (compiled.test_work_record_count != 0 &&
       compiled.test_compiled_work_record_count ==
           compiled.test_work_record_count);

  UINT empty_record_index = compiled.record_count;
  UINT work_seen = 0;
  for (UINT record_index = 0; record_index < records.size(); ++record_index) {
    if (!IsCompiledWorkRecord(records[record_index].payload))
      continue;
    if (++work_seen == 2) {
      empty_record_index = record_index;
      break;
    }
  }
  auto insertion = std::find_if(
      compiled.segments.begin(), compiled.segments.end(),
      [&](const CompiledCommandSegment &segment) {
        return segment.first_record_index >= empty_record_index;
      });
  if (config.flags & ExecutionPathFlagInjectEmptyNativeSegment) {
    CompiledCommandSegment segment = {};
    segment.kind = CompiledCommandSegmentKind::Compute;
    segment.first_record_index = empty_record_index;
    segment.record_count = 0;
    segment.first_compute_packet =
        static_cast<UINT>(compiled.compute_packets.size());
    insertion = compiled.segments.insert(insertion, segment);
    ++insertion;
  }
  if (config.flags & ExecutionPathFlagInjectEmptyFallbackSegment) {
    CompiledCommandSegment segment = {};
    segment.kind = CompiledCommandSegmentKind::Fallback;
    segment.first_record_index = empty_record_index;
    segment.record_count = 0;
    segment.fallback_reason =
        CompiledCommandFallbackReason::ConservativeCompiler;
    segment.perf_fallback_reason = CompiledCommandFallbackReasonToPerf(
        segment.fallback_reason);
    compiled.segments.insert(insertion, segment);
  }
  if (config.flags & (ExecutionPathFlagInjectEmptyNativeSegment |
                      ExecutionPathFlagInjectEmptyFallbackSegment))
    BuildCompiledEncoderGraph(compiled);
}

static dxmt::d3d12::test::ExecutionPathStats
BuildExecutionPathTestStats(const CompiledCommandList &compiled) {
  using dxmt::d3d12::test::ExecutionPathSegmentKind;
  using dxmt::d3d12::test::kExecutionPathMaxTracedSegments;
  dxmt::d3d12::test::ExecutionPathStats stats = {};
  stats.mode = compiled.test_path_mode;
  stats.command_list_generation = compiled.generation;
  stats.record_count = compiled.record_count;
  stats.work_record_count = compiled.test_work_record_count;
  stats.compiled_work_record_count =
      compiled.test_compiled_work_record_count;
  stats.native_requirement_satisfied =
      compiled.test_native_requirement_satisfied ? 1u : 0u;
  stats.retained_graphics_packets =
      static_cast<UINT>(compiled.graphics_packets.size());
  stats.retained_compute_packets =
      static_cast<UINT>(compiled.compute_packets.size());
  // Native root-base storage is submission-scoped and intentionally absent
  // from the immutable Close generation.
  stats.has_native_root_base_buffer = 0;
  stats.unexpected_container_growths = compiled.unexpected_container_growths;
  stats.storage_allocation_events = compiled.storage_allocation_events;
  stats.node_storage_allocation_events =
      compiled.node_storage_allocation_events;
  stats.state_storage_allocation_events =
      compiled.state_storage_allocation_events;
  stats.access_storage_allocation_events =
      compiled.access_storage_allocation_events;
  stats.immutable_state_reuses = compiled.immutable_state_reuses;
  stats.close_materialized_root_table_sets =
      compiled.close_materialized_root_table_sets;
  stats.close_dynamic_render_state_recipes =
      compiled.close_dynamic_render_state_recipes;
  stats.close_vertex_binding_recipes =
      compiled.close_vertex_binding_recipes;
  stats.close_direct_access_plans = compiled.close_direct_access_plans;
  stats.close_binding_programs = compiled.close_binding_programs;
  stats.close_binding_program_reuses =
      compiled.close_binding_program_reuses;
  stats.close_full_binding_programs =
      compiled.close_full_binding_programs;
  stats.close_delta_binding_programs =
      compiled.close_delta_binding_programs;
  auto count_state_delta = [&](const CompiledCommandStateDelta &delta) {
    stats.state_delta_packets++;
    if (!delta.dirty_domains && !delta.root_table_dirty_mask &&
        !delta.root_constant_dirty_mask &&
        !delta.root_descriptor_dirty_mask)
      stats.zero_state_delta_packets++;
  };
  for (const auto &packet : compiled.graphics_packets)
    count_state_delta(packet.state_delta);
  for (const auto &packet : compiled.compute_packets)
    count_state_delta(packet.state_delta);
  stats.compiled_barrier_ranges =
      static_cast<UINT>(compiled.access_summary.barrier_ranges.size());
  stats.compiled_barriers =
      static_cast<UINT>(compiled.access_summary.barriers.size());
  stats.compiled_resource_state_deltas = static_cast<UINT>(
      compiled.access_summary.resource_state_deltas.size());
  stats.encoder_graph_node_count =
      static_cast<UINT>(compiled.encoder_graph.nodes.size());
  stats.encoder_graph_elided_state_records =
      compiled.encoder_graph.elided_state_record_count;
  stats.encoder_group_count = compiled.encoder_graph.encoder_count;
  stats.encoder_inlined_barrier_nodes =
      compiled.encoder_graph.inlined_barrier_node_count;
  for (const auto &node : compiled.encoder_graph.nodes) {
    if (!node.begins_encoder)
      continue;
    stats.graphics_encoder_node_count +=
        node.encoder_kind == CompiledEncoderKind::Graphics;
    stats.compute_encoder_node_count +=
        node.encoder_kind == CompiledEncoderKind::Compute;
  }
  stats.segment_count = static_cast<UINT>(compiled.segments.size());
  stats.traced_segment_count =
      std::min<UINT>(stats.segment_count, kExecutionPathMaxTracedSegments);
  for (UINT segment_index = 0; segment_index < compiled.segments.size();
       ++segment_index) {
    const auto &segment = compiled.segments[segment_index];
    if (segment_index < stats.traced_segment_count) {
      auto &kind = stats.segment_kinds[segment_index];
      switch (segment.kind) {
      case CompiledCommandSegmentKind::Graphics:
        kind = ExecutionPathSegmentKind::Graphics;
        break;
      case CompiledCommandSegmentKind::Compute:
        kind = ExecutionPathSegmentKind::Compute;
        break;
      case CompiledCommandSegmentKind::Indirect:
        // The public test ABI predates dedicated dynamic nodes. They are
        // compiled submission work, so expose them as native rather than as
        // legacy fallback. The aggregate packet counters below still retain
        // the graphics/compute distinction for mixed indirect segments.
        kind = ExecutionPathSegmentKind::Graphics;
        break;
      case CompiledCommandSegmentKind::Barrier:
        kind = ExecutionPathSegmentKind::Typed;
        break;
      case CompiledCommandSegmentKind::Transfer:
        kind = ExecutionPathSegmentKind::Typed;
        break;
      case CompiledCommandSegmentKind::Control:
        kind = ExecutionPathSegmentKind::Typed;
        break;
      case CompiledCommandSegmentKind::ElidedState:
        // The public test ABI predates Close-time-elided state nodes. They are
        // neither replay nor fallback work; expose them as typed ordering
        // nodes while keeping them out of the selected typed-node counter.
        kind = ExecutionPathSegmentKind::Typed;
        break;
      case CompiledCommandSegmentKind::Typed:
        kind = ExecutionPathSegmentKind::Typed;
        break;
      case CompiledCommandSegmentKind::Fallback:
      default:
        kind = ExecutionPathSegmentKind::Fallback;
        break;
      }
      stats.segment_first_record_indices[segment_index] =
          segment.first_record_index;
      stats.segment_record_counts[segment_index] = segment.record_count;
    }
    if (!segment.record_count) {
      if (segment.kind == CompiledCommandSegmentKind::Fallback)
        stats.empty_fallback_segments++;
      else
        stats.empty_native_segments++;
      continue;
    }
    switch (segment.kind) {
    case CompiledCommandSegmentKind::Graphics:
      stats.graphics_segments++;
      stats.selected_graphics_packets += segment.graphics_packet_count;
      break;
    case CompiledCommandSegmentKind::Compute:
      stats.compute_segments++;
      stats.selected_compute_packets += segment.compute_packet_count;
      break;
    case CompiledCommandSegmentKind::Indirect:
      {
        UINT graphics_packets = 0;
        UINT compute_packets = 0;
        for (UINT i = 0; i < segment.indirect_packet_count; i++) {
          const auto &packet = compiled.indirect_packets[
              segment.first_indirect_packet + i];
          if (packet.compute)
            compute_packets++;
          else
            graphics_packets++;
        }
        if (graphics_packets)
          stats.graphics_segments++;
        if (compute_packets)
          stats.compute_segments++;
        stats.selected_graphics_packets += graphics_packets;
        stats.selected_compute_packets += compute_packets;
      }
      break;
    case CompiledCommandSegmentKind::Barrier:
      break;
    case CompiledCommandSegmentKind::Transfer:
      break;
    case CompiledCommandSegmentKind::Control:
      break;
    case CompiledCommandSegmentKind::ElidedState:
      break;
    case CompiledCommandSegmentKind::Typed:
      stats.selected_typed_nodes += segment.record_count;
      break;
    case CompiledCommandSegmentKind::Fallback:
    default:
      stats.fallback_segments++;
      break;
    }
  }
  if (compiled.test_telemetry) {
    const auto &telemetry = *compiled.test_telemetry;
    stats.replayed_graphics_packets =
        telemetry.replayed_graphics_packets.load(std::memory_order_acquire);
    stats.replayed_compute_packets =
        telemetry.replayed_compute_packets.load(std::memory_order_acquire);
    stats.encoder_attachment_materializations =
        telemetry.encoder_attachment_materializations.load(
            std::memory_order_acquire);
    stats.submitted_root_table_fast_patches =
        telemetry.submitted_root_table_fast_patches.load(
            std::memory_order_acquire);
    stats.submitted_root_table_full_materializations =
        telemetry.submitted_root_table_full_materializations.load(
            std::memory_order_acquire);
    stats.submitted_native_binding_plan_reuses =
        telemetry.submitted_native_binding_plan_reuses.load(
            std::memory_order_acquire);
    stats.submitted_native_binding_recipe_reuses =
        telemetry.submitted_native_binding_recipe_reuses.load(
            std::memory_order_acquire);
    stats.replayed_fallback_ranges =
        telemetry.replayed_fallback_ranges.load(std::memory_order_acquire);
    stats.replayed_fallback_records =
        telemetry.replayed_fallback_records.load(std::memory_order_acquire);
    stats.replayed_compiled_packet_fallbacks =
        telemetry.replayed_compiled_packet_fallbacks.load(
            std::memory_order_acquire);
    stats.replayed_indirect_nodes =
        telemetry.replayed_indirect_nodes.load(std::memory_order_acquire);
    stats.replayed_indirect_dependency_nodes =
        telemetry.replayed_indirect_dependency_nodes.load(
            std::memory_order_acquire);
    stats.replayed_indirect_fallbacks =
        telemetry.replayed_indirect_fallbacks.load(
            std::memory_order_acquire);
    stats.encoder_full_binding_programs =
        telemetry.encoder_full_binding_programs.load(
            std::memory_order_acquire);
    stats.encoder_delta_binding_programs =
        telemetry.encoder_delta_binding_programs.load(
            std::memory_order_acquire);
    stats.encoder_binding_program_hits =
        telemetry.encoder_binding_program_hits.load(
            std::memory_order_acquire);
    stats.encoder_native_binding_ops =
        telemetry.encoder_native_binding_ops.load(std::memory_order_acquire);
    stats.encoder_native_binding_ops_skipped =
        telemetry.encoder_native_binding_ops_skipped.load(
            std::memory_order_acquire);
    stats.encoder_resource_plan_publications =
        telemetry.encoder_resource_plan_publications.load(
            std::memory_order_acquire);
    stats.encoder_resource_plan_reuses =
        telemetry.encoder_resource_plan_reuses.load(
            std::memory_order_acquire);
    stats.replayed_compatibility_packets =
        stats.replayed_compiled_packet_fallbacks;
    stats.legacy_replay_records = stats.replayed_fallback_records;
    if (compiled.test_path_mode ==
            dxmt::d3d12::test::ExecutionPathMode::NativeCompiled &&
        stats.replayed_compiled_packet_fallbacks)
      stats.native_requirement_satisfied = 0;
    stats.replayed_empty_native_segments =
        telemetry.replayed_empty_native_segments.load(
            std::memory_order_acquire);
    stats.replayed_empty_fallback_segments =
        telemetry.replayed_empty_fallback_segments.load(
            std::memory_order_acquire);
    stats.submitted_graphics_packets =
        telemetry.submitted_graphics_packets.load(std::memory_order_acquire);
    stats.submitted_compute_packets =
        telemetry.submitted_compute_packets.load(std::memory_order_acquire);
    stats.submission_prepare_failures =
        telemetry.submission_prepare_failures.load(std::memory_order_acquire);
    stats.submitted_descriptor_snapshots =
        telemetry.submitted_descriptor_snapshots.load(
            std::memory_order_acquire);
    stats.submitted_descriptor_entries =
        telemetry.submitted_descriptor_entries.load(std::memory_order_acquire);
    stats.submitted_unique_descriptor_snapshots =
        telemetry.submitted_unique_descriptor_snapshots.load(
            std::memory_order_acquire);
    stats.submitted_unique_descriptor_records =
        telemetry.submitted_unique_descriptor_records.load(
            std::memory_order_acquire);
    stats.submitted_descriptor_record_reuses =
        telemetry.submitted_descriptor_record_reuses.load(
            std::memory_order_acquire);
    stats.generic_descriptor_span_lookups =
        telemetry.generic_descriptor_span_lookups.load(
            std::memory_order_acquire);
    stats.generic_descriptor_span_unique =
        telemetry.generic_descriptor_span_unique.load(
            std::memory_order_acquire);
    stats.generic_descriptor_span_reuses =
        telemetry.generic_descriptor_span_reuses.load(
            std::memory_order_acquire);
    stats.frozen_native_direct_packets =
        telemetry.frozen_native_direct_packets.load(
            std::memory_order_acquire);
    stats.frozen_range_lookups =
        telemetry.frozen_range_lookups.load(std::memory_order_acquire);
    stats.frozen_range_unique =
        telemetry.frozen_range_unique.load(std::memory_order_acquire);
    stats.frozen_range_reuses =
        telemetry.frozen_range_reuses.load(std::memory_order_acquire);
    stats.frozen_root_word_lookups =
        telemetry.frozen_root_word_lookups.load(
            std::memory_order_acquire);
    stats.frozen_root_word_reuses =
        telemetry.frozen_root_word_reuses.load(
            std::memory_order_acquire);
    stats.submission_graphics_encoder_boundary_merges =
        telemetry.submission_graphics_encoder_boundary_merges.load(
            std::memory_order_acquire);
    stats.submission_compute_encoder_boundary_merges =
        telemetry.submission_compute_encoder_boundary_merges.load(
            std::memory_order_acquire);
    stats.submission_encoder_boundary_flushes =
        telemetry.submission_encoder_boundary_flushes.load(
            std::memory_order_acquire);
    stats.submitted_generation_shares =
        telemetry.submitted_generation_shares.load(std::memory_order_acquire);
    stats.submitted_generation_deep_copies =
        telemetry.submitted_generation_deep_copies.load(
            std::memory_order_acquire);
  }
  return stats;
}

#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
using GraphicsCommandListComBase = ID3D12GraphicsCommandList6;
#elif defined(__ID3D12GraphicsCommandList5_INTERFACE_DEFINED__)
using GraphicsCommandListComBase = ID3D12GraphicsCommandList5;
#elif defined(__ID3D12GraphicsCommandList4_INTERFACE_DEFINED__)
using GraphicsCommandListComBase = ID3D12GraphicsCommandList4;
#elif defined(__ID3D12GraphicsCommandList2_INTERFACE_DEFINED__)
using GraphicsCommandListComBase = ID3D12GraphicsCommandList2;
#elif defined(__ID3D12GraphicsCommandList1_INTERFACE_DEFINED__)
using GraphicsCommandListComBase = ID3D12GraphicsCommandList1;
#else
using GraphicsCommandListComBase = ID3D12GraphicsCommandList;
#endif

class GraphicsCommandListImpl final
    : public ComObjectWithInitialRef<GraphicsCommandListComBase>,
      public GraphicsCommandList,
      public IMTLD3D12GraphicsCommandListExt {
public:
  ULONG STDMETHODCALLTYPE AddRef() override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListReferenceCount);
    return ComObjectWithInitialRef<GraphicsCommandListComBase>::AddRef();
  }

  ULONG STDMETHODCALLTYPE Release() override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListReferenceCount);
    return ComObjectWithInitialRef<GraphicsCommandListComBase>::Release();
  }

  ~GraphicsCommandListImpl() override {
    if (allocator_ && !closed_)
      allocator_->EndCommandListRecording(this);
  }

  GraphicsCommandListImpl(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state,
                          HRESULT *status)
      : device_(device), node_mask_(node_mask), type_(type),
        initial_pipeline_state_(initial_pipeline_state) {
    if (status)
      *status = S_OK;
    if (command_allocator) {
      auto *allocator_state =
          dynamic_cast<CommandAllocatorObject *>(command_allocator);
      if (!allocator_state || allocator_state->GetParentDevice() != device ||
          allocator_state->GetCommandListType() != type_ ||
          !allocator_state->BeginCommandListRecording(this)) {
        if (status)
          *status = E_INVALIDARG;
        return;
      }
      allocator_ = allocator_state;
    }
    if (!IsPipelineStateCompatible(initial_pipeline_state_.ptr())) {
      if (status)
        *status = E_INVALIDARG;
      return;
    }
    current_pipeline_state_ = initial_pipeline_state_;
    if (current_pipeline_state_)
      RecordPipelineState(current_pipeline_state_.ptr(), true);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) || riid == __uuidof(ID3D12CommandList) ||
        riid == __uuidof(ID3D12GraphicsCommandList)) {
      *ppvObject = ref(AsGraphicsCommandList());
      return S_OK;
    }

#ifdef __ID3D12GraphicsCommandList1_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList1)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList1 *>(
          static_cast<GraphicsCommandListComBase *>(this)));
      return S_OK;
    }
#endif

#ifdef __ID3D12GraphicsCommandList2_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList2)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList2 *>(
          static_cast<GraphicsCommandListComBase *>(this)));
      return S_OK;
    }
#endif

#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList3)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList3 *>(
          static_cast<GraphicsCommandListComBase *>(this)));
      return S_OK;
    }
    if (riid == __uuidof(ID3D12GraphicsCommandList4)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList4 *>(this));
      return S_OK;
    }
#endif

#ifdef __ID3D12GraphicsCommandList5_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList5)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList5 *>(
          static_cast<GraphicsCommandListComBase *>(this)));
      return S_OK;
    }
#endif

#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
    if (riid == __uuidof(ID3D12GraphicsCommandList6)) {
      *ppvObject = ref(static_cast<ID3D12GraphicsCommandList6 *>(this));
      return S_OK;
    }
#endif

    if (riid == __uuidof(IMTLD3D12GraphicsCommandListExt)) {
      *ppvObject = ref(static_cast<IMTLD3D12GraphicsCommandListExt *>(this));
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12GraphicsCommandList), riid))
      WARN("D3D12GraphicsCommandList: unknown interface query ", str::format(riid));

    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    if (guid == dxmt::d3d12::test::kExecutionPathConfigGuid) {
      if (!data_size)
        return E_INVALIDARG;
      if (!test_path_configured_) {
        *data_size = 0;
        return DXGI_ERROR_NOT_FOUND;
      }
      const UINT required = sizeof(test_path_config_);
      if (!data) {
        *data_size = required;
        return S_OK;
      }
      if (*data_size < required) {
        *data_size = required;
        return DXGI_ERROR_MORE_DATA;
      }
      std::memcpy(data, &test_path_config_, required);
      *data_size = required;
      return S_OK;
    }
    if (guid == dxmt::d3d12::test::kExecutionPathStatsGuid) {
      if (!data_size)
        return E_INVALIDARG;
      if (!compiled_commands_) {
        *data_size = 0;
        return DXGI_ERROR_NOT_FOUND;
      }
      const auto stats = BuildExecutionPathTestStats(*compiled_commands_);
      const UINT required = sizeof(stats);
      if (!data) {
        *data_size = required;
        return S_OK;
      }
      if (*data_size < required) {
        *data_size = required;
        return DXGI_ERROR_MORE_DATA;
      }
      std::memcpy(data, &stats, required);
      *data_size = required;
      return S_OK;
    }
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    if (guid == dxmt::d3d12::test::kExecutionPathConfigGuid) {
      using dxmt::d3d12::test::ExecutionPathConfig;
      using dxmt::d3d12::test::ExecutionPathFlagInjectEmptyFallbackSegment;
      using dxmt::d3d12::test::ExecutionPathFlagInjectEmptyNativeSegment;
      using dxmt::d3d12::test::ExecutionPathMode;
      if (closed_)
        return E_INVALIDARG;
      if (!data && data_size == 0) {
        const HRESULT result = test_path_configured_ ? S_OK : S_FALSE;
        test_path_config_ = {};
        test_path_configured_ = false;
        return result;
      }
      if (!data || data_size != sizeof(ExecutionPathConfig))
        return E_INVALIDARG;
      ExecutionPathConfig config = {};
      std::memcpy(&config, data, sizeof(config));
      constexpr UINT kKnownFlags =
          ExecutionPathFlagInjectEmptyNativeSegment |
          ExecutionPathFlagInjectEmptyFallbackSegment;
      if (config.struct_size != sizeof(ExecutionPathConfig) ||
          static_cast<UINT>(config.mode) >
              static_cast<UINT>(ExecutionPathMode::Fallback) ||
          (config.flags & ~kKnownFlags))
        return E_INVALIDARG;
      test_path_config_ = config;
      test_path_configured_ = true;
      return S_OK;
    }
    if (guid == dxmt::d3d12::test::kExecutionPathStatsGuid)
      return E_INVALIDARG;
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid, const IUnknown *data) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    if (guid == dxmt::d3d12::test::kExecutionPathConfigGuid ||
        guid == dxmt::d3d12::test::kExecutionPathStatsGuid)
      return E_INVALIDARG;
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    return device_->QueryInterface(riid, device);
  }

  D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    return type_;
  }

  HRESULT STDMETHODCALLTYPE Close() override {
    dxmt::perf::ScopedCodeTimer control_timer(
        dxmt::PerfCodePath::CommandListCloseControl);
    if (closed_)
      return E_FAIL;
    if (!recording_error_) {
      dxmt::perf::ScopedCodeTimer build_timer(
          dxmt::PerfCodePath::CommandListCloseBuildCompiled);
      const bool force_compatibility =
          test_path_configured_ &&
          test_path_config_.mode ==
              dxmt::d3d12::test::ExecutionPathMode::CompatibilityCompiled;
      auto compiled = BuildCompiledCommandList(
          records_, device_->GetMTLDevice(), force_compatibility,
          recorded_graphics_packet_count_, recorded_compute_packet_count_,
          recorded_barrier_record_count_, recorded_barrier_count_);
      if (test_path_configured_) {
        ApplyExecutionPathTestConfig(*compiled, records_, test_path_config_);
        if (test_path_config_.mode ==
                dxmt::d3d12::test::ExecutionPathMode::NativeCompiled &&
            !compiled->test_native_requirement_satisfied)
          recording_error_ = E_FAIL;
      }
      const bool needs_fallback_records = std::any_of(
          compiled->segments.begin(), compiled->segments.end(),
          [](const CompiledCommandSegment &segment) {
            return segment.kind == CompiledCommandSegmentKind::Typed ||
                   segment.kind == CompiledCommandSegmentKind::Fallback;
          });
      const bool needs_bundle_records =
          type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE;
      std::shared_ptr<const std::vector<CommandRecord>> retained_records;
      if (needs_fallback_records || needs_bundle_records) {
        retained_records =
            std::make_shared<const std::vector<CommandRecord>>(
                std::move(records_));
      } else {
        records_ = {};
      }
      if (needs_fallback_records)
        compiled->fallback_records = retained_records;
      compiled_commands_ = std::move(compiled);
      // Bundles still expose their source generation for expansion into the
      // parent list. Ordinary direct/compute lists release it at Close.
      closed_records_ = needs_bundle_records ? std::move(retained_records)
                                             : nullptr;
    }
    closed_ = true;
    if (allocator_) {
      dxmt::perf::ScopedCodeTimer allocator_timer(
          dxmt::PerfCodePath::CommandListCloseAllocatorRelease);
      allocator_->EndCommandListRecording(this);
    }
    HRESULT hr = recording_error_ ? recording_error_ : S_OK;
    if (apitrace_lifecycle_recording_enabled_ &&
        dxmt::apitrace::d3d_enabled()) {
      dxmt::perf::ScopedCodeTimer apitrace_timer(
          dxmt::PerfCodePath::CommandListCloseApitrace);
      dxmt::apitrace::record_close_command_list(this, hr);
    }
    if (recording_error_)
      return recording_error_;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Reset(ID3D12CommandAllocator *allocator,
                                  ID3D12PipelineState *initial_state) override {
    dxmt::perf::ScopedCodeTimer control_timer(
        dxmt::PerfCodePath::CommandListResetControl);
    if (!closed_)
      return E_FAIL;
    if (recording_error_)
      return E_FAIL;
    if (!allocator)
      return WARN_E_INVALIDARG(__func__);

    auto allocator_state = dynamic_cast<CommandAllocatorObject *>(allocator);
    if (!allocator_state || allocator_state->GetParentDevice() != device_.ptr() ||
        allocator_state->GetCommandListType() != type_)
      return WARN_E_INVALIDARG(__func__);
    if (!IsPipelineStateCompatible(initial_state))
      return WARN_E_INVALIDARG(__func__);

    {
      dxmt::perf::ScopedCodeTimer allocator_timer(
          dxmt::PerfCodePath::CommandListResetAllocator);
      if (allocator_.ptr() == allocator_state) {
        allocator_->EndCommandListRecording(this);
        if (!allocator_state->BeginCommandListRecording(this))
          return WARN_E_INVALIDARG(__func__);
      } else {
        if (!allocator_state->BeginCommandListRecording(this))
          return WARN_E_INVALIDARG(__func__);
        if (allocator_)
          allocator_->EndCommandListRecording(this);
      }
    }
    allocator_ = allocator_state;
    initial_pipeline_state_ = initial_state;
    current_pipeline_state_ = initial_pipeline_state_;
    compute_root_signature_ = nullptr;
    graphics_root_signature_ = nullptr;
    {
      dxmt::perf::ScopedCodeTimer state_clear_timer(
          dxmt::PerfCodePath::CommandListResetStateClear);
      records_.clear();
      closed_records_.reset();
      recorded_graphics_packet_count_ = 0;
      recorded_compute_packet_count_ = 0;
      recorded_barrier_record_count_ = 0;
      recorded_barrier_count_ = 0;
      compiled_commands_.reset();
      test_path_config_ = {};
      test_path_configured_ = false;
      pending_render_pass_resolves_.clear();
      active_queries_.clear();
      render_pass_active_ = false;
      ClearRecordedStateCache();
    }
    closed_ = false;
    submitted_ = false;
    recording_error_ = S_OK;
    if (current_pipeline_state_) {
      if (apitrace_lifecycle_recording_enabled_) {
        dxmt::perf::ScopedCodeTimer apitrace_timer(
            dxmt::PerfCodePath::CommandListResetApitrace);
        g_current_command_record_d3d_sequence =
            dxmt::apitrace::record_reset_command_list(
                this, allocator, initial_state, S_OK);
      }
      if (current_pipeline_state_) {
        dxmt::perf::ScopedCodeTimer pipeline_timer(
            dxmt::PerfCodePath::CommandListResetInitialPipeline);
        RecordPipelineState(current_pipeline_state_.ptr(), true);
      }
    } else if (apitrace_lifecycle_recording_enabled_) {
      dxmt::perf::ScopedCodeTimer apitrace_timer(
          dxmt::PerfCodePath::CommandListResetApitrace);
      dxmt::apitrace::record_reset_command_list(
          this, allocator, initial_state, S_OK);
    }
    return S_OK;
  }

  IMTLD3D12Device *GetParentDevice() const override {
    return device_.ptr();
  }

  bool IsClosed() const override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    return closed_;
  }

  D3D12_COMMAND_LIST_TYPE GetCommandListType() const override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    return type_;
  }

  std::shared_ptr<const std::vector<CommandRecord>>
  GetCommandRecordGeneration() const override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    return closed_records_;
  }

  std::shared_ptr<const CompiledCommandList> GetCompiledCommands()
      const override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    return compiled_commands_;
  }

  void SetApitraceLifecycleRecordingEnabled(bool enabled) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    apitrace_lifecycle_recording_enabled_ = enabled;
  }

  HRESULT MarkSubmittedToQueue(
      D3D12_COMMAND_LIST_TYPE queue_type,
      std::vector<SubmittedCommandAllocatorUse> &allocator_uses) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListObjectApi);
    if (!closed_ || type_ != queue_type)
      return WARN_E_INVALIDARG(__func__);

    submitted_ = true;
    if (allocator_) {
      allocator_uses.push_back(allocator_->MarkCommandListSubmitted());
    }
    return S_OK;
  }

  void STDMETHODCALLTYPE
  TemporalUpscale(const MTL_TEMPORAL_UPSCALE_D3D12_DESC *desc) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListTemporalUpscale);
    if (!desc || !desc->Color || !desc->Depth || !desc->MotionVector ||
        !desc->Output)
      return;

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_temporal_upscale(
            this, desc->InputContentWidth, desc->InputContentHeight,
            desc->AutoExposure, desc->InReset, desc->DepthReversed,
            desc->MotionVectorInDisplayRes, desc->Color, desc->Depth,
            desc->MotionVector, desc->Output, desc->MotionVectorScaleX,
            desc->MotionVectorScaleY, desc->PreExposure, desc->ExposureTexture,
            desc->JitterOffsetX, desc->JitterOffsetY);
    TemporalUpscaleRecord record = {};
    record.input_content_width = desc->InputContentWidth;
    record.input_content_height = desc->InputContentHeight;
    const D3D12_RESOURCE_DESC motion_desc = desc->MotionVector->GetDesc();
    record.motion_vector_width = static_cast<UINT>(motion_desc.Width);
    record.motion_vector_height = motion_desc.Height;
    record.auto_exposure = desc->AutoExposure;
    record.in_reset = desc->InReset;
    record.depth_reversed = desc->DepthReversed;
    record.motion_vector_in_display_res = desc->MotionVectorInDisplayRes;
    record.color = desc->Color;
    record.depth = desc->Depth;
    record.motion_vector = desc->MotionVector;
    record.output = desc->Output;
    record.motion_vector_scale_x = desc->MotionVectorScaleX;
    record.motion_vector_scale_y = desc->MotionVectorScaleY;
    record.pre_exposure = desc->PreExposure;
    record.exposure_texture = desc->ExposureTexture;
    record.jitter_offset_x = desc->JitterOffsetX;
    record.jitter_offset_y = desc->JitterOffsetY;
    AddRecord(std::move(record));
  }

  HRESULT STDMETHODCALLTYPE CheckFeatureSupport(MTL_D3D12_FEATURE feature,
                                                void *feature_support_data,
                                                UINT feature_support_data_size) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListFeatureSupport);
    if (!feature_support_data)
      return E_INVALIDARG;

    switch (feature) {
    case MTL_D3D12_FEATURE_METALFX_TEMPORAL_SCALER:
      if (feature_support_data_size != sizeof(BOOL))
        return E_INVALIDARG;
      *reinterpret_cast<BOOL *>(feature_support_data) =
          device_->GetMTLDevice().supportsFXTemporalScaler();
      return S_OK;
    }

    return E_INVALIDARG;
  }

  void STDMETHODCALLTYPE ClearState(ID3D12PipelineState *pipeline_state) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListClearState);
    if (DropBundleCommand("ClearState"))
      return;
    if (RejectCommandListType("ClearState", kDirectList | kComputeList))
      return;
    if (!IsPipelineStateCompatible(pipeline_state))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_state(this, pipeline_state);
    current_pipeline_state_ = pipeline_state;
    compute_root_signature_ = nullptr;
    graphics_root_signature_ = nullptr;
    ClearRecordedStateCache();
    AddRecord(ClearStateRecord{current_pipeline_state_});
    recorded_pipeline_state_ = current_pipeline_state_;
  }
  void STDMETHODCALLTYPE DrawInstanced(UINT vertex_count_per_instance, UINT instance_count,
                                       UINT start_vertex_location, UINT start_instance_location) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListDrawInstanced);
    if (RejectCommandListType("DrawInstanced", kDirectList | kBundleList))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_draw_instanced(
            this, vertex_count_per_instance, instance_count,
            start_vertex_location, start_instance_location);
    AddRecord(DrawInstancedRecord{
        vertex_count_per_instance, instance_count, start_vertex_location,
        start_instance_location});
  }
  void STDMETHODCALLTYPE DrawIndexedInstanced(UINT index_count_per_instance, UINT instance_count,
                                              UINT start_vertex_location, INT base_vertex_location,
                                              UINT start_instance_location) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListDrawIndexedInstanced);
    if (RejectCommandListType("DrawIndexedInstanced",
                              kDirectList | kBundleList))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_draw_indexed_instanced(
            this, index_count_per_instance, instance_count,
            start_vertex_location, base_vertex_location, start_instance_location);
    AddRecord(DrawIndexedInstancedRecord{
        index_count_per_instance, instance_count, start_vertex_location,
        base_vertex_location, start_instance_location});
  }
  void STDMETHODCALLTYPE Dispatch(UINT x, UINT y, UINT z) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListDispatch);
    if (RejectCommandListType(
            "Dispatch", kDirectList | kBundleList | kComputeList))
      return;
    constexpr UINT kMaxDispatchDimension = 65535;
    if (x > kMaxDispatchDimension || y > kMaxDispatchDimension ||
        z > kMaxDispatchDimension) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_dispatch(this, x, y, z);
    AddRecord(DispatchRecord{x, y, z});
  }
  void STDMETHODCALLTYPE CopyBufferRegion(ID3D12Resource *dst_buffer, UINT64 dst_offset,
                                          ID3D12Resource *src_buffer, UINT64 src_offset,
                                          UINT64 byte_count) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListCopyBuffer);
    if (DropBundleCommand("CopyBufferRegion"))
      return;
    if (RejectCommandListType(
            "CopyBufferRegion", kDirectList | kComputeList | kCopyList))
      return;
    RecordCopyBufferRegion("CopyBufferRegion", dst_buffer, dst_offset, src_buffer,
                           src_offset, byte_count);
  }
  void STDMETHODCALLTYPE CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *dst, UINT dst_x,
                                           UINT dst_y, UINT dst_z,
                                           const D3D12_TEXTURE_COPY_LOCATION *src,
                                           const D3D12_BOX *src_box) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListCopyTexture);
    if (DropBundleCommand("CopyTextureRegion"))
      return;
    if (RejectCommandListType(
            "CopyTextureRegion", kDirectList | kComputeList | kCopyList))
      return;
    if (!dst || !src || !dst->pResource || !src->pResource)
      return;
    const auto *dst_resource = dynamic_cast<Resource *>(dst->pResource);
    const auto *src_resource = dynamic_cast<Resource *>(src->pResource);
    if (!dst_resource || !src_resource ||
        dst_resource->GetParentDevice() != device_.ptr() ||
        src_resource->GetParentDevice() != device_.ptr()) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    SnapshotCopyTextureSourceBuffer(src, src_box);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_copy_texture_region(
            this, dst, dst_x, dst_y, dst_z, src, src_box);
    CopyTextureRegionRecord record = {};
    record.dst = StoreTextureCopyLocation(*dst);
    record.dst_x = dst_x;
    record.dst_y = dst_y;
    record.dst_z = dst_z;
    record.src = StoreTextureCopyLocation(*src);
    if (src_box)
      record.src_box = *src_box;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE CopyResource(ID3D12Resource *dst_resource,
                                      ID3D12Resource *src_resource) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListCopyResource);
    if (DropBundleCommand("CopyResource"))
      return;
    if (RejectCommandListType(
            "CopyResource", kDirectList | kComputeList | kCopyList))
      return;
    if (!dst_resource || !src_resource) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    const auto *dst = dynamic_cast<Resource *>(dst_resource);
    const auto *src = dynamic_cast<Resource *>(src_resource);
    if (!dst || !src || dst->GetParentDevice() != device_.ptr() ||
        src->GetParentDevice() != device_.ptr()) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    const auto &dst_desc = dst->GetResourceDesc();
    const auto &src_desc = src->GetResourceDesc();
    if (dst_desc.Dimension != src_desc.Dimension ||
        dst_desc.Width != src_desc.Width ||
        dst_desc.Height != src_desc.Height ||
        dst_desc.DepthOrArraySize != src_desc.DepthOrArraySize ||
        dst_desc.MipLevels != src_desc.MipLevels ||
        !AreDXGIFormatsInSameTypeGroup(dst_desc.Format, src_desc.Format) ||
        dst_desc.SampleDesc.Count != src_desc.SampleDesc.Count ||
        dst_desc.SampleDesc.Quality != src_desc.SampleDesc.Quality) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_copy_resource(this, dst_resource, src_resource);
    AddRecord(CopyResourceRecord{dst_resource, src_resource});
  }
  void STDMETHODCALLTYPE CopyTiles(ID3D12Resource *tiled_resource,
                                   const D3D12_TILED_RESOURCE_COORDINATE *tile_region_start_coordinate,
                                   const D3D12_TILE_REGION_SIZE *tile_region_size,
                                   ID3D12Resource *buffer, UINT64 buffer_offset,
                                   D3D12_TILE_COPY_FLAGS flags) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListCopyTiles);
    if (DropBundleCommand("CopyTiles"))
      return;
    if (RejectCommandListType(
            "CopyTiles", kDirectList | kComputeList | kCopyList))
      return;
    if (!tiled_resource || !tile_region_start_coordinate || !tile_region_size ||
        !buffer)
      return;
    const auto *tiled = dynamic_cast<Resource *>(tiled_resource);
    const auto *linear = dynamic_cast<Resource *>(buffer);
    const auto *tiling = tiled ? tiled->GetTiling() : nullptr;
    constexpr UINT kDirectionFlags =
        static_cast<UINT>(
            D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) |
        static_cast<UINT>(
            D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    constexpr UINT kAllowedFlags =
        kDirectionFlags |
        static_cast<UINT>(D3D12_TILE_COPY_FLAG_NO_HAZARD);
    const UINT flag_bits = static_cast<UINT>(flags);
    const bool buffer_to_tiled =
        flag_bits & static_cast<UINT>(
                        D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
    const bool tiled_to_buffer =
        flag_bits & static_cast<UINT>(
                        D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
    if (!tiled || !linear || tiled->GetParentDevice() != device_.ptr() ||
        linear->GetParentDevice() != device_.ptr() || !tiled->IsReserved() ||
        !tiling || linear->IsReserved() ||
        linear->GetResourceDesc().Dimension !=
            D3D12_RESOURCE_DIMENSION_BUFFER ||
        (flag_bits & ~kAllowedFlags) || buffer_to_tiled == tiled_to_buffer ||
        tile_region_start_coordinate->Subresource >=
            tiling->subresources.size()) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    const auto &subresource =
        tiling->subresources[tile_region_start_coordinate->Subresource];
    if (subresource.start_tile_index == D3D12_PACKED_TILE ||
        tile_region_start_coordinate->X >= subresource.width_in_tiles ||
        tile_region_start_coordinate->Y >= subresource.height_in_tiles ||
        tile_region_start_coordinate->Z >= subresource.depth_in_tiles) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    UINT64 tile_count = tile_region_size->NumTiles;
    if (tile_region_size->UseBox) {
      if (!tile_region_size->Width || !tile_region_size->Height ||
          !tile_region_size->Depth ||
          tile_region_size->Width >
              subresource.width_in_tiles - tile_region_start_coordinate->X ||
          tile_region_size->Height >
              subresource.height_in_tiles - tile_region_start_coordinate->Y ||
          tile_region_size->Depth >
              subresource.depth_in_tiles - tile_region_start_coordinate->Z) {
        recording_error_ = E_INVALIDARG;
        return;
      }
      tile_count = UINT64(tile_region_size->Width) *
                   tile_region_size->Height * tile_region_size->Depth;
    } else {
      const UINT64 start_tile =
          UINT64(subresource.start_tile_index) +
          (UINT64(tile_region_start_coordinate->Z) *
               subresource.height_in_tiles +
           tile_region_start_coordinate->Y) *
              subresource.width_in_tiles +
          tile_region_start_coordinate->X;
      if (!tile_count || start_tile >= tiling->total_tile_count ||
          tile_count > tiling->total_tile_count - start_tile) {
        recording_error_ = E_INVALIDARG;
        return;
      }
    }
    const UINT64 buffer_width = linear->GetResourceDesc().Width;
    if (buffer_offset > buffer_width ||
        tile_count >
            (buffer_width - buffer_offset) /
                D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_copy_tiles(
            this, tiled_resource, tile_region_start_coordinate,
            tile_region_size, buffer, buffer_offset, flags);
    if (g_apitrace_record_diag_log_count.fetch_add(1, std::memory_order_relaxed) < 8) {
      WARN("D3D12GraphicsCommandList: CopyTiles queued through minimal tiled-resource path"
           " tiledResource=", tiled_resource,
           " buffer=", buffer,
           " bufferOffset=", buffer_offset,
           " flags=", flags,
           " subresource=", tile_region_start_coordinate->Subresource,
           " x=", tile_region_start_coordinate->X,
           " y=", tile_region_start_coordinate->Y,
           " z=", tile_region_start_coordinate->Z,
           " useBox=", tile_region_size->UseBox,
           " numTiles=", tile_region_size->NumTiles,
           " width=", tile_region_size->Width,
           " height=", tile_region_size->Height,
           " depth=", tile_region_size->Depth);
    }
    CopyTilesRecord record = {};
    record.tiled_resource = tiled_resource;
    record.start = *tile_region_start_coordinate;
    record.size = *tile_region_size;
    record.buffer = buffer;
    record.buffer_offset = buffer_offset;
    record.flags = flags;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ResolveSubresource(ID3D12Resource *dst_resource, UINT dst_sub_resource,
                                            ID3D12Resource *src_resource, UINT src_sub_resource,
                                            DXGI_FORMAT format) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListResolve);
    if (DropBundleCommand("ResolveSubresource"))
      return;
    if (RejectCommandListType("ResolveSubresource", kDirectList))
      return;
    if (!ValidateResolveSubresource(dst_resource, dst_sub_resource, 0, 0,
                                    src_resource, src_sub_resource, nullptr,
                                    format, true)) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_resolve_subresource(
            this, dst_resource, dst_sub_resource, src_resource, src_sub_resource,
            static_cast<uint32_t>(format));
    ResolveSubresourceRecord record = {};
    record.dst = dst_resource;
    record.dst_subresource = dst_sub_resource;
    record.src = src_resource;
    record.src_subresource = src_sub_resource;
    record.format = format;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListPrimitiveTopology);
    if (RejectCommandListType("IASetPrimitiveTopology",
                              kDirectList | kBundleList))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_ia_set_primitive_topology(
            this, static_cast<uint32_t>(primitive_topology));
    AddRecord(PrimitiveTopologyRecord{primitive_topology});
  }
  void STDMETHODCALLTYPE RSSetViewports(UINT viewport_count, const D3D12_VIEWPORT *viewports) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListViewports);
    if (DropBundleCommand("RSSetViewports"))
      return;
    if (RejectCommandListType("RSSetViewports", kDirectList))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_rs_set_viewports(this, viewport_count, viewports);
    ViewportRecord record = {};
    if (viewports && viewport_count)
      record.viewports.assign(viewports, viewports + viewport_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE RSSetScissorRects(UINT rect_count, const D3D12_RECT *rects) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListScissors);
    if (DropBundleCommand("RSSetScissorRects"))
      return;
    if (RejectCommandListType("RSSetScissorRects", kDirectList))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_rs_set_scissor_rects(this, rect_count, rects);
    ScissorRecord record = {};
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE OMSetBlendFactor(const FLOAT blend_factor[4]) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListBlendFactor);
    if (RejectCommandListType("OMSetBlendFactor",
                              kDirectList | kBundleList))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_om_set_blend_factor(this, blend_factor);
    BlendFactorRecord record = {};
    if (blend_factor)
      std::copy(blend_factor, blend_factor + 4, record.blend_factor.begin());
    AddRecord(record);
  }
  void STDMETHODCALLTYPE OMSetStencilRef(UINT stencil_ref) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListStencilRef);
    if (RejectCommandListType("OMSetStencilRef",
                              kDirectList | kBundleList))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_om_set_stencil_ref(this, stencil_ref);
    AddRecord(StencilRefRecord{stencil_ref});
  }
  void STDMETHODCALLTYPE SetPipelineState(ID3D12PipelineState *pipeline_state) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListPipelineState);
    if (RejectCommandListType(
            "SetPipelineState", kDirectList | kBundleList | kComputeList))
      return;
    if (!IsPipelineStateCompatible(pipeline_state))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_pipeline_state(this, pipeline_state);
    current_pipeline_state_ = pipeline_state;
    RecordPipelineState(current_pipeline_state_.ptr(), false);
  }
  void STDMETHODCALLTYPE ResourceBarrier(UINT barrier_count, const D3D12_RESOURCE_BARRIER *barriers) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListResourceBarrier);
    if (DropBundleCommand("ResourceBarrier"))
      return;
    if (RejectCommandListType(
            "ResourceBarrier", kDirectList | kComputeList | kCopyList))
      return;
    if (!barriers || !barrier_count)
      return;

    const auto belongs_to_device = [this](ID3D12Resource *resource) {
      if (!resource)
        return true;
      const auto *state = dynamic_cast<Resource *>(resource);
      return state && state->GetParentDevice() == device_.ptr();
    };
    for (UINT i = 0; i < barrier_count; i++) {
      const auto &barrier = barriers[i];
      bool resources_valid = false;
      if (IsValidResourceBarrier(barrier)) {
        switch (barrier.Type) {
        case D3D12_RESOURCE_BARRIER_TYPE_TRANSITION:
          resources_valid = belongs_to_device(barrier.Transition.pResource);
          break;
        case D3D12_RESOURCE_BARRIER_TYPE_ALIASING:
          resources_valid =
              belongs_to_device(barrier.Aliasing.pResourceBefore) &&
              belongs_to_device(barrier.Aliasing.pResourceAfter);
          break;
        case D3D12_RESOURCE_BARRIER_TYPE_UAV:
          resources_valid = belongs_to_device(barrier.UAV.pResource);
          break;
        default:
          break;
        }
      }
      if (!resources_valid) {
        recording_error_ = E_INVALIDARG;
        return;
      }
    }

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_resource_barrier(this, barrier_count, barriers);

    ResourceBarrierRecord record = {};
    record.barriers.reserve(barrier_count);
    for (UINT i = 0; i < barrier_count; i++)
      record.barriers.push_back(StoreResourceBarrier(barriers[i]));
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ExecuteBundle(ID3D12GraphicsCommandList *command_list) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListExecuteBundle);
    if (DropBundleCommand("ExecuteBundle"))
      return;
    if (type_ != D3D12_COMMAND_LIST_TYPE_DIRECT) {
      recording_error_ = E_INVALIDARG;
      WARN("D3D12GraphicsCommandList: ExecuteBundle requires a direct command list");
      return;
    }
    auto *bundle = dynamic_cast<GraphicsCommandList *>(command_list);
    if (!bundle || bundle->GetCommandListType() != D3D12_COMMAND_LIST_TYPE_BUNDLE) {
      recording_error_ = E_INVALIDARG;
      WARN("D3D12GraphicsCommandList: ExecuteBundle called with non-bundle command list");
      return;
    }
    if (bundle->GetParentDevice() != device_.ptr()) {
      recording_error_ = E_INVALIDARG;
      WARN("D3D12GraphicsCommandList: ExecuteBundle called with cross-device bundle");
      return;
    }
    if (!bundle->IsClosed()) {
      recording_error_ = E_INVALIDARG;
      WARN("D3D12GraphicsCommandList: ExecuteBundle called with an open bundle");
      return;
    }
    const auto bundle_generation = bundle->GetCommandRecordGeneration();
    if (!bundle_generation)
      return;
    const auto &bundle_records = *bundle_generation;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_execute_bundle(this, command_list);
    records_.insert(records_.end(), bundle_records.begin(), bundle_records.end());
    ClearRecordedStateCache();
    current_pipeline_state_ = nullptr;
  }
  void STDMETHODCALLTYPE SetDescriptorHeaps(UINT heap_count, ID3D12DescriptorHeap *const *heaps) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListDescriptorHeaps);
    if (RejectCommandListType(
            "SetDescriptorHeaps", kDirectList | kBundleList | kComputeList))
      return;
    if (heaps && heap_count) {
      for (UINT i = 0; i < heap_count; i++) {
        const auto *heap = dynamic_cast<DescriptorHeap *>(heaps[i]);
        if (!heap || heap->GetParentDevice() != device_.ptr())
          return;
      }
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_descriptor_heaps(
            this, heap_count, reinterpret_cast<const void *const *>(heaps));
    ClearRootDescriptorTableCache();
    DescriptorHeapsRecord record = {};
    if (heaps && heap_count) {
      record.heaps.reserve(heap_count);
      for (UINT i = 0; i < heap_count; i++)
        record.heaps.push_back(heaps[i]);
    }
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetComputeRootSignature(ID3D12RootSignature *root_signature) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootSignature);
    auto *state = GetDXMTRootSignature(root_signature);
    if (root_signature &&
        (!state || state->GetParentDevice() != device_.ptr()))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_signature(this, true, root_signature);
    if (compute_root_signature_.ptr() == root_signature)
      return;
    compute_root_signature_ = root_signature;
    ClearComputeRootParameterCache();
    AddRecord(RootSignatureRecord{true, root_signature});
  }

  void STDMETHODCALLTYPE SetGraphicsRootSignature(ID3D12RootSignature *root_signature) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootSignature);
    auto *state = GetDXMTRootSignature(root_signature);
    if (root_signature &&
        (!state || state->GetParentDevice() != device_.ptr()))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_signature(this, false, root_signature);
    if (graphics_root_signature_.ptr() == root_signature)
      return;
    graphics_root_signature_ = root_signature;
    ClearGraphicsRootParameterCache();
    AddRecord(RootSignatureRecord{false, root_signature});
  }
  void STDMETHODCALLTYPE SetComputeRootDescriptorTable(UINT root_parameter_index,
                                                       D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootDescriptorTable);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor_table(
            this, true, root_parameter_index, base_descriptor);
    RootDescriptorTableRecord record = {
        true, root_parameter_index, base_descriptor};
    if (RootDescriptorTableUnchanged(record))
      return;
    AddRecord(record);
  }
  void STDMETHODCALLTYPE SetGraphicsRootDescriptorTable(UINT root_parameter_index,
                                                        D3D12_GPU_DESCRIPTOR_HANDLE base_descriptor) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootDescriptorTable);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor_table(
            this, false, root_parameter_index, base_descriptor);
    RootDescriptorTableRecord record = {
        false, root_parameter_index, base_descriptor};
    if (RootDescriptorTableUnchanged(record))
      return;
    AddRecord(record);
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstant(UINT root_parameter_index, UINT data,
                                                     UINT dst_offset) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootConstants);
    if (DropOutOfRangeRootConstants("SetComputeRoot32BitConstant",
                                    root_parameter_index, 1, dst_offset))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_32bit_constants(
            this, true, root_parameter_index, 1, &data, dst_offset);
    RootConstantsRecord record = {};
    record.compute = true;
    record.root_parameter_index = root_parameter_index;
    record.dst_offset = dst_offset;
    record.values.push_back(data);
    if (RootConstantsUnchanged(record))
      return;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstant(UINT root_parameter_index, UINT data,
                                                      UINT dst_offset) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootConstants);
    if (DropOutOfRangeRootConstants("SetGraphicsRoot32BitConstant",
                                    root_parameter_index, 1, dst_offset))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_32bit_constants(
            this, false, root_parameter_index, 1, &data, dst_offset);
    RootConstantsRecord record = {};
    record.compute = false;
    record.root_parameter_index = root_parameter_index;
    record.dst_offset = dst_offset;
    record.values.push_back(data);
    if (RootConstantsUnchanged(record))
      return;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetComputeRoot32BitConstants(UINT root_parameter_index, UINT constant_count,
                                                      const void *data, UINT dst_offset) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootConstants);
    if (DropOutOfRangeRootConstants("SetComputeRoot32BitConstants",
                                    root_parameter_index, constant_count,
                                    dst_offset))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_32bit_constants(
            this, true, root_parameter_index, constant_count,
            static_cast<const UINT *>(data), dst_offset);
    RootConstantsRecord record = {};
    record.compute = true;
    record.root_parameter_index = root_parameter_index;
    record.dst_offset = dst_offset;
    if (data && constant_count) {
      const auto *values = static_cast<const UINT *>(data);
      record.values.assign(values, values + constant_count);
    }
    if (RootConstantsUnchanged(record))
      return;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetGraphicsRoot32BitConstants(UINT root_parameter_index, UINT constant_count,
                                                       const void *data, UINT dst_offset) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootConstants);
    if (DropOutOfRangeRootConstants("SetGraphicsRoot32BitConstants",
                                    root_parameter_index, constant_count,
                                    dst_offset))
      return;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_32bit_constants(
            this, false, root_parameter_index, constant_count,
            static_cast<const UINT *>(data), dst_offset);
    RootConstantsRecord record = {};
    record.compute = false;
    record.root_parameter_index = root_parameter_index;
    record.dst_offset = dst_offset;
    if (data && constant_count) {
      const auto *values = static_cast<const UINT *>(data);
      record.values.assign(values, values + constant_count);
    }
    if (RootConstantsUnchanged(record))
      return;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SetComputeRootConstantBufferView(UINT root_parameter_index,
                                                          D3D12_GPU_VIRTUAL_ADDRESS address) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootDescriptor);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, true, D3D12_ROOT_PARAMETER_TYPE_CBV, root_parameter_index, address);
    InvalidateRootParameterCache(true, root_parameter_index);
    AddRecord(RootDescriptorRecord{
        true, D3D12_ROOT_PARAMETER_TYPE_CBV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetGraphicsRootConstantBufferView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootDescriptor);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, false, D3D12_ROOT_PARAMETER_TYPE_CBV, root_parameter_index, address);
    InvalidateRootParameterCache(false, root_parameter_index);
    AddRecord(RootDescriptorRecord{
        false, D3D12_ROOT_PARAMETER_TYPE_CBV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetComputeRootShaderResourceView(UINT root_parameter_index,
                                                          D3D12_GPU_VIRTUAL_ADDRESS address) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootDescriptor);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, true, D3D12_ROOT_PARAMETER_TYPE_SRV, root_parameter_index, address);
    InvalidateRootParameterCache(true, root_parameter_index);
    AddRecord(RootDescriptorRecord{
        true, D3D12_ROOT_PARAMETER_TYPE_SRV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetGraphicsRootShaderResourceView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootDescriptor);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, false, D3D12_ROOT_PARAMETER_TYPE_SRV, root_parameter_index, address);
    InvalidateRootParameterCache(false, root_parameter_index);
    AddRecord(RootDescriptorRecord{
        false, D3D12_ROOT_PARAMETER_TYPE_SRV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetComputeRootUnorderedAccessView(UINT root_parameter_index,
                                                           D3D12_GPU_VIRTUAL_ADDRESS address) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootDescriptor);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, true, D3D12_ROOT_PARAMETER_TYPE_UAV, root_parameter_index, address);
    InvalidateRootParameterCache(true, root_parameter_index);
    AddRecord(RootDescriptorRecord{
        true, D3D12_ROOT_PARAMETER_TYPE_UAV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE SetGraphicsRootUnorderedAccessView(UINT root_parameter_index,
                                                            D3D12_GPU_VIRTUAL_ADDRESS address) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRootDescriptor);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_root_descriptor(
            this, false, D3D12_ROOT_PARAMETER_TYPE_UAV, root_parameter_index, address);
    InvalidateRootParameterCache(false, root_parameter_index);
    AddRecord(RootDescriptorRecord{
        false, D3D12_ROOT_PARAMETER_TYPE_UAV, root_parameter_index, address});
  }
  void STDMETHODCALLTYPE IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *view) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListIndexBuffer);
    if (RejectCommandListType("IASetIndexBuffer",
                              kDirectList | kBundleList))
      return;
    if (view)
      SnapshotGpuVirtualBufferRange(view->BufferLocation, view->SizeInBytes);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_ia_set_index_buffer(this, view);
    IndexBufferRecord record = {};
    if (view)
      record.view = *view;
    if (IndexBufferUnchanged(record))
      return;
    AddRecord(record);
  }
  void STDMETHODCALLTYPE IASetVertexBuffers(UINT start_slot, UINT view_count,
                                            const D3D12_VERTEX_BUFFER_VIEW *views) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListVertexBuffers);
    if (RejectCommandListType("IASetVertexBuffers",
                              kDirectList | kBundleList))
      return;
    for (UINT index = 0; views && index < view_count; ++index)
      SnapshotGpuVirtualBufferRange(views[index].BufferLocation, views[index].SizeInBytes);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_ia_set_vertex_buffers(
            this, start_slot, view_count, views);
    VertexBuffersRecord record = {};
    record.start_slot = start_slot;
    record.view_count = view_count;
    if (views && view_count)
      record.views.assign(views, views + view_count);
    if (VertexBuffersUnchanged(record))
      return;
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE SOSetTargets(UINT start_slot, UINT view_count,
                                      const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListStreamOutput);
    if (DropBundleCommand("SOSetTargets"))
      return;
    if (RejectCommandListType("SOSetTargets", kDirectList))
      return;
    if (!view_count)
      return;

    // StreamOutputTier is reported as NOT_SUPPORTED. Do not accept a command
    // that would otherwise be silently dropped and leave stale output data.
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: stream output targets are unsupported");
  }
  void STDMETHODCALLTYPE OMSetRenderTargets(UINT render_target_descriptor_count,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *render_target_descriptors,
                                            WINBOOL single_descriptor_handle,
                                            const D3D12_CPU_DESCRIPTOR_HANDLE *depth_stencil_descriptor) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRenderTargets);
    if (DropBundleCommand("OMSetRenderTargets"))
      return;
    if (RejectCommandListType("OMSetRenderTargets", kDirectList))
      return;
    const auto belongs_to_device = [this](const DescriptorRecord &descriptor) {
      if (!descriptor.resource)
        return true;
      const auto *resource =
          dynamic_cast<Resource *>(descriptor.resource.ptr());
      return resource && resource->GetParentDevice() == device_.ptr();
    };
    RenderTargetsRecord record = {};
    if (render_target_descriptors && render_target_descriptor_count) {
      record.render_targets.reserve(render_target_descriptor_count);
      if (single_descriptor_handle) {
        auto base = GetDescriptorRecordRangeFromCpuHandle(
            render_target_descriptors[0], D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            render_target_descriptor_count, "OMSetRenderTargets");
        if (base) {
          for (UINT i = 0; i < render_target_descriptor_count; i++) {
            if (!belongs_to_device(base.get()[i])) {
              recording_error_ = E_INVALIDARG;
              return;
            }
            record.render_targets.push_back(base.get()[i]);
          }
        }
      } else {
        for (UINT i = 0; i < render_target_descriptor_count; i++) {
          auto descriptor =
              GetDescriptorRecordFromCpuHandle(
                  render_target_descriptors[i],
                  D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
          if (descriptor) {
            if (!belongs_to_device(*descriptor)) {
              recording_error_ = E_INVALIDARG;
              return;
            }
            record.render_targets.push_back(*descriptor);
          }
        }
      }
    }
    if (depth_stencil_descriptor) {
      if (auto descriptor =
              GetDescriptorRecordFromCpuHandle(
                  *depth_stencil_descriptor,
                  D3D12_DESCRIPTOR_HEAP_TYPE_DSV)) {
        if (!belongs_to_device(*descriptor)) {
          recording_error_ = E_INVALIDARG;
          return;
        }
        record.depth_stencil = *descriptor;
      }
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_om_set_render_targets(
            this, render_target_descriptor_count, render_target_descriptors,
            single_descriptor_handle, depth_stencil_descriptor);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags,
                                               FLOAT depth, UINT8 stencil, UINT rect_count,
                                               const D3D12_RECT *rects) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListClearDepthStencil);
    if (DropBundleCommand("ClearDepthStencilView"))
      return;
    if (RejectCommandListType("ClearDepthStencilView", kDirectList))
      return;
    auto descriptor = GetDescriptorRecordFromCpuHandle(
        dsv, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    if (!descriptor)
      return;
    if (descriptor->resource) {
      const auto *resource =
          dynamic_cast<Resource *>(descriptor->resource.ptr());
      if (!resource || resource->GetParentDevice() != device_.ptr()) {
        recording_error_ = E_INVALIDARG;
        return;
      }
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_depth_stencil_view(
            this, dsv, flags, depth, stencil, rect_count, rects);
    ClearDepthStencilRecord record = {};
    record.descriptor = *descriptor;
    record.flags = flags;
    record.depth = depth;
    record.stencil = stencil;
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT color[4],
                                               UINT rect_count, const D3D12_RECT *rects) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListClearRenderTarget);
    if (DropBundleCommand("ClearRenderTargetView"))
      return;
    if (RejectCommandListType("ClearRenderTargetView", kDirectList))
      return;
    auto descriptor = GetDescriptorRecordFromCpuHandle(
        rtv, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!descriptor)
      return;
    if (descriptor->resource) {
      const auto *resource =
          dynamic_cast<Resource *>(descriptor->resource.ptr());
      if (!resource || resource->GetParentDevice() != device_.ptr()) {
        recording_error_ = E_INVALIDARG;
        return;
      }
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_render_target_view(
            this, rtv, color, rect_count, rects);
    ClearRenderTargetRecord record = {};
    record.descriptor = *descriptor;
    if (color)
      std::copy(color, color + 4, record.color.begin());
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
                                                      D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                                      ID3D12Resource *resource, const UINT values[4],
                                                      UINT rect_count, const D3D12_RECT *rects) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListClearUav);
    if (DropBundleCommand("ClearUnorderedAccessViewUint"))
      return;
    if (RejectCommandListType(
            "ClearUnorderedAccessViewUint", kDirectList | kComputeList))
      return;
    if (!resource || !values)
      return;
    auto *clear_resource = dynamic_cast<Resource *>(resource);
    if (!clear_resource || clear_resource->GetParentDevice() != device_.ptr()) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    auto descriptor =
        GetDescriptorRecordFromGpuHandle(gpu_handle,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!descriptor)
      descriptor =
          GetDescriptorRecordFromCpuHandle(cpu_handle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!descriptor || descriptor->type != DescriptorRecordType::UnorderedAccessView)
      return;
    if (descriptor->resource.ptr() != resource) {
      WARN("D3D12CommandList: ClearUnorderedAccessViewUint resource does not match descriptor");
      return;
    }

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_unordered_access_view_uint(
            this, gpu_handle, cpu_handle, resource, values, rect_count, rects);
    ClearUnorderedAccessRecord record = {};
    record.descriptor = *descriptor;
    record.resource = resource;
    record.integer = true;
    std::copy(values, values + 4, record.uint_values.begin());
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle,
                                                       D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle,
                                                       ID3D12Resource *resource, const float values[4],
                                                       UINT rect_count, const D3D12_RECT *rects) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListClearUav);
    if (DropBundleCommand("ClearUnorderedAccessViewFloat"))
      return;
    if (RejectCommandListType(
            "ClearUnorderedAccessViewFloat", kDirectList | kComputeList))
      return;
    if (!resource || !values)
      return;
    auto *clear_resource = dynamic_cast<Resource *>(resource);
    if (!clear_resource || clear_resource->GetParentDevice() != device_.ptr()) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    auto descriptor =
        GetDescriptorRecordFromGpuHandle(gpu_handle,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!descriptor)
      descriptor =
          GetDescriptorRecordFromCpuHandle(cpu_handle,
                                           D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (!descriptor || descriptor->type != DescriptorRecordType::UnorderedAccessView)
      return;
    if (descriptor->resource.ptr() != resource) {
      WARN("D3D12CommandList: ClearUnorderedAccessViewFloat resource does not match descriptor");
      return;
    }

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_clear_unordered_access_view_float(
            this, gpu_handle, cpu_handle, resource, values, rect_count, rects);
    ClearUnorderedAccessRecord record = {};
    record.descriptor = *descriptor;
    record.resource = resource;
    std::copy(values, values + 4, record.float_values.begin());
    if (rects && rect_count)
      record.rects.assign(rects, rects + rect_count);
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE DiscardResource(ID3D12Resource *resource, const D3D12_DISCARD_REGION *region) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListDiscard);
    if (DropBundleCommand("DiscardResource"))
      return;
    if (RejectCommandListType(
            "DiscardResource", kDirectList | kComputeList))
      return;
    if (!resource)
      return;
    auto *discard_resource = dynamic_cast<Resource *>(resource);
    if (!discard_resource ||
        discard_resource->GetParentDevice() != device_.ptr()) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_discard_resource(
            this, resource, region ? region->FirstSubresource : 0,
            region ? region->NumSubresources : 0,
            region ? region->NumRects : 0,
            region ? region->pRects : nullptr);
    DiscardResourceRecord record = {};
    record.resource = resource;
    if (region) {
      record.first_subresource = region->FirstSubresource;
      record.subresource_count = region->NumSubresources;
      if (region->pRects && region->NumRects)
        record.rects.assign(region->pRects, region->pRects + region->NumRects);
    }
    AddRecord(std::move(record));
  }
  void STDMETHODCALLTYPE BeginQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListQueryBeginEnd);
    if (DropBundleCommand("BeginQuery"))
      return;
    if (RejectCommandListType("BeginQuery", kDirectList))
      return;
    auto *query_heap = dynamic_cast<QueryHeap *>(heap);
    if (!query_heap || query_heap->GetParentDevice() != device_.ptr() ||
        index >= query_heap->GetDesc().Count ||
        type == D3D12_QUERY_TYPE_TIMESTAMP ||
        !IsQueryTypeCompatible(query_heap->GetDesc().Type, type)) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    const auto key = std::make_pair(query_heap, index);
    if (active_queries_.find(key) != active_queries_.end()) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    active_queries_.emplace(key, type);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_begin_query(
            this, heap, static_cast<uint32_t>(type), index);
    AddRecord(BeginQueryRecord{heap, type, index});
  }
  void STDMETHODCALLTYPE EndQuery(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type, UINT index) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListQueryBeginEnd);
    if (DropBundleCommand("EndQuery"))
      return;
    if (RejectCommandListType(
            "EndQuery", kDirectList | kComputeList | kCopyList))
      return;
    auto *query_heap = dynamic_cast<QueryHeap *>(heap);
    if (!query_heap || query_heap->GetParentDevice() != device_.ptr() ||
        index >= query_heap->GetDesc().Count ||
        !IsQueryTypeCompatible(query_heap->GetDesc().Type, type)) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    if (type != D3D12_QUERY_TYPE_TIMESTAMP) {
      const auto key = std::make_pair(query_heap, index);
      const auto active = active_queries_.find(key);
      if (active != active_queries_.end() && active->second != type) {
        recording_error_ = E_INVALIDARG;
        return;
      }
      if (active != active_queries_.end())
        active_queries_.erase(active);
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_end_query(
            this, heap, static_cast<uint32_t>(type), index);
    AddRecord(EndQueryRecord{heap, type, index});
  }
  void STDMETHODCALLTYPE ResolveQueryData(ID3D12QueryHeap *heap, D3D12_QUERY_TYPE type,
                                          UINT start_index, UINT query_count,
                                          ID3D12Resource *dst_buffer,
                                          UINT64 aligned_dst_buffer_offset) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListQueryResolve);
    if (DropBundleCommand("ResolveQueryData"))
      return;
    if (RejectCommandListType(
            "ResolveQueryData", kDirectList | kComputeList | kCopyList))
      return;
    auto *query_heap = dynamic_cast<QueryHeap *>(heap);
    auto *destination = dynamic_cast<Resource *>(dst_buffer);
    const bool range_valid =
        query_heap && query_heap->GetParentDevice() == device_.ptr() &&
        start_index <= query_heap->GetDesc().Count &&
        query_count <= query_heap->GetDesc().Count - start_index;
    const bool destination_valid =
        destination && destination->GetParentDevice() == device_.ptr() &&
        destination->GetResourceDesc().Dimension ==
            D3D12_RESOURCE_DIMENSION_BUFFER &&
        !(aligned_dst_buffer_offset & (sizeof(UINT64) - 1)) &&
        aligned_dst_buffer_offset <= destination->GetResourceDesc().Width &&
        UINT64(query_count) <=
            (destination->GetResourceDesc().Width - aligned_dst_buffer_offset) /
                sizeof(UINT64);
    if (!query_count)
      return;
    if (!query_heap || !IsQueryTypeCompatible(query_heap->GetDesc().Type, type) ||
        !range_valid || !destination_valid) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_resolve_query_data(
            this, heap, static_cast<uint32_t>(type), start_index, query_count,
            dst_buffer, aligned_dst_buffer_offset);
    AddRecord(ResolveQueryDataRecord{
        reinterpret_cast<uintptr_t>(this), heap, type, start_index,
        query_count, dst_buffer, aligned_dst_buffer_offset});
  }
  void STDMETHODCALLTYPE SetPredication(ID3D12Resource *buffer, UINT64 aligned_buffer_offset,
                                        D3D12_PREDICATION_OP operation) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListPredication);
    if (DropBundleCommand("SetPredication"))
      return;
    if (RejectCommandListType(
            "SetPredication", kDirectList | kComputeList))
      return;
    if (buffer) {
      const auto *predicate = dynamic_cast<Resource *>(buffer);
      if (!predicate || predicate->GetParentDevice() != device_.ptr() ||
          predicate->GetResourceDesc().Dimension !=
              D3D12_RESOURCE_DIMENSION_BUFFER ||
          (aligned_buffer_offset & (sizeof(UINT64) - 1)) ||
          aligned_buffer_offset > predicate->GetResourceDesc().Width ||
          sizeof(UINT64) >
              predicate->GetResourceDesc().Width - aligned_buffer_offset ||
          (operation != D3D12_PREDICATION_OP_EQUAL_ZERO &&
           operation != D3D12_PREDICATION_OP_NOT_EQUAL_ZERO)) {
        recording_error_ = E_INVALIDARG;
        return;
      }
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_set_predication(
            this, buffer, aligned_buffer_offset, static_cast<uint32_t>(operation));
    AddRecord(PredicationRecord{buffer, aligned_buffer_offset, operation});
  }
  void STDMETHODCALLTYPE SetMarker(UINT metadata, const void *data, UINT size) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListMarkerEvent);
  }
  void STDMETHODCALLTYPE BeginEvent(UINT metadata, const void *data, UINT size) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListMarkerEvent);
  }
  void STDMETHODCALLTYPE EndEvent() override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListMarkerEvent);
  }
  void STDMETHODCALLTYPE ExecuteIndirect(ID3D12CommandSignature *command_signature,
                                         UINT max_command_count, ID3D12Resource *arg_buffer,
                                         UINT64 arg_buffer_offset, ID3D12Resource *count_buffer,
                                         UINT64 count_buffer_offset) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListExecuteIndirect);
    if (RejectCommandListType(
            "ExecuteIndirect", kDirectList | kBundleList | kComputeList))
      return;
    if (!command_signature || !arg_buffer || !max_command_count)
      return;
    const auto *signature =
        dynamic_cast<CommandSignature *>(command_signature);
    const auto *arguments = dynamic_cast<Resource *>(arg_buffer);
    const auto *count = dynamic_cast<Resource *>(count_buffer);
    const bool arguments_valid =
        signature && arguments &&
        arguments->GetParentDevice() == device_.ptr() &&
        arguments->GetResourceDesc().Dimension ==
            D3D12_RESOURCE_DIMENSION_BUFFER &&
        !(arg_buffer_offset & (sizeof(UINT) - 1)) &&
        arg_buffer_offset <= arguments->GetResourceDesc().Width &&
        UINT64(max_command_count) <=
            (arguments->GetResourceDesc().Width - arg_buffer_offset) /
                signature->GetDesc().ByteStride;
    const bool count_valid =
        !count_buffer ||
        (count && count->GetParentDevice() == device_.ptr() &&
         count->GetResourceDesc().Dimension ==
             D3D12_RESOURCE_DIMENSION_BUFFER &&
         !(count_buffer_offset & (sizeof(UINT) - 1)) &&
         count_buffer_offset <= count->GetResourceDesc().Width &&
         sizeof(UINT) <= count->GetResourceDesc().Width - count_buffer_offset);
    if (!signature || signature->GetParentDevice() != device_.ptr() ||
        !arguments_valid || !count_valid ||
        (type_ == D3D12_COMMAND_LIST_TYPE_BUNDLE && count_buffer)) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_execute_indirect(
            this, command_signature, max_command_count, arg_buffer,
            arg_buffer_offset, count_buffer, count_buffer_offset);
    AddRecord(ExecuteIndirectRecord{
        command_signature, max_command_count, arg_buffer, arg_buffer_offset,
        count_buffer, count_buffer_offset});
  }

#ifdef __ID3D12GraphicsCommandList1_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE AtomicCopyBufferUINT(
      ID3D12Resource *dst_buffer, UINT64 dst_offset,
      ID3D12Resource *src_buffer, UINT64 src_offset,
      UINT dependent_resource_count,
      ID3D12Resource *const *dependent_resources,
      const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    if (DropBundleCommand("AtomicCopyBufferUINT"))
      return;
    // A normal blit copy cannot provide AtomicCopy's dependent-range ordering
    // and atomic publication contract. Fail Close explicitly rather than
    // recording a semantically different CopyBufferRegion.
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: AtomicCopyBufferUINT is unsupported");
  }

  void STDMETHODCALLTYPE AtomicCopyBufferUINT64(
      ID3D12Resource *dst_buffer, UINT64 dst_offset,
      ID3D12Resource *src_buffer, UINT64 src_offset,
      UINT dependent_resource_count,
      ID3D12Resource *const *dependent_resources,
      const D3D12_SUBRESOURCE_RANGE_UINT64 *dependent_sub_resource_ranges) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    if (DropBundleCommand("AtomicCopyBufferUINT64"))
      return;
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: AtomicCopyBufferUINT64 is unsupported");
  }

  void STDMETHODCALLTYPE OMSetDepthBounds(FLOAT min, FLOAT max) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListDepthBounds);
    depth_bounds_min_ = min;
    depth_bounds_max_ = max;
    if (min == 0.0f && max == 1.0f)
      return;

    // DepthBoundsTestSupported is reported as FALSE. Default bounds are a
    // harmless no-op; non-default bounds must not be silently ignored.
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: non-default depth bounds are unsupported");
  }

  void STDMETHODCALLTYPE SetSamplePositions(
      UINT sample_count, UINT pixel_count,
      D3D12_SAMPLE_POSITION *sample_positions) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListSamplePositions);
    const bool reset = sample_count == 0 && pixel_count == 0 && !sample_positions;
    if (!reset) {
      recording_error_ = E_NOTIMPL;
      WARN("D3D12GraphicsCommandList: programmable sample positions are unsupported");
      return;
    }
  }

  void STDMETHODCALLTYPE ResolveSubresourceRegion(
      ID3D12Resource *dst_resource, UINT dst_sub_resource_idx, UINT dst_x,
      UINT dst_y, ID3D12Resource *src_resource, UINT src_sub_resource_idx,
      D3D12_RECT *src_rect, DXGI_FORMAT format, D3D12_RESOLVE_MODE mode) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListResolve);
    if (DropBundleCommand("ResolveSubresourceRegion"))
      return;
    if (RejectCommandListType("ResolveSubresourceRegion", kDirectList))
      return;
    if (mode == D3D12_RESOLVE_MODE_DECOMPRESS) {
      recording_error_ = E_NOTIMPL;
      WARN("D3D12GraphicsCommandList: ResolveSubresourceRegion decompress mode is unsupported");
      return;
    }
    if (!ValidateResolveSubresource(
            dst_resource, dst_sub_resource_idx, dst_x, dst_y, src_resource,
            src_sub_resource_idx, src_rect, format, false)) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_resolve_subresource_region(
            this, dst_resource, dst_sub_resource_idx, dst_x, dst_y,
            src_resource, src_sub_resource_idx, src_rect,
            static_cast<uint32_t>(format), static_cast<uint32_t>(mode));
    ResolveSubresourceRecord record = {};
    record.dst = dst_resource;
    record.dst_subresource = dst_sub_resource_idx;
    record.dst_x = dst_x;
    record.dst_y = dst_y;
    record.src = src_resource;
    record.src_subresource = src_sub_resource_idx;
    if (src_rect)
      record.src_rect = *src_rect;
    record.format = format;
    record.mode = mode;
    AddRecord(std::move(record));
  }

  void STDMETHODCALLTYPE SetViewInstanceMask(UINT mask) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListViewInstanceMask);
    view_instance_mask_ = mask;
  }
#endif

#ifdef __ID3D12GraphicsCommandList2_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE WriteBufferImmediate(
      UINT count, const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *parameters,
      const D3D12_WRITEBUFFERIMMEDIATE_MODE *modes) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListWriteBufferImmediate);
    if (!count)
      return;
    if (!parameters) {
      recording_error_ = E_INVALIDARG;
      return;
    }

    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_write_buffer_immediate(
            this, count, parameters, modes);
    WriteBufferImmediateRecord record = {};
    record.operations.reserve(count);
    for (UINT index = 0; index < count; ++index) {
      const auto mode = modes ? modes[index]
                              : D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT;
      if (mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_DEFAULT &&
          mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_IN &&
          mode != D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT) {
        recording_error_ = E_INVALIDARG;
        return;
      }

      const auto address = parameters[index].Dest;
      UINT64 offset = 0;
      auto *resource =
          LookupBufferResourceByGpuVirtualAddress(address, &offset);
      if ((address & (sizeof(UINT) - 1)) || !resource ||
          resource->GetParentDevice() != device_.ptr() ||
          resource->GetResourceDesc().Dimension !=
              D3D12_RESOURCE_DIMENSION_BUFFER ||
          offset > resource->GetResourceDesc().Width ||
          sizeof(UINT) > resource->GetResourceDesc().Width - offset) {
        recording_error_ = E_INVALIDARG;
        return;
      }
      record.operations.push_back({resource->GetD3D12Resource(), offset,
                                   parameters[index].Value, mode});
    }
    AddRecord(std::move(record));
  }
#endif

#ifdef __ID3D12GraphicsCommandList3_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE SetProtectedResourceSession(
      ID3D12ProtectedResourceSession *protected_resource_session) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListProtectedSession);
    if (protected_resource_session)
      WARN("D3D12GraphicsCommandList: protected resource sessions are "
           "unsupported");
  }
#endif

#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE BeginRenderPass(
      UINT render_targets_count,
      const D3D12_RENDER_PASS_RENDER_TARGET_DESC *render_targets,
      const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth_stencil,
      D3D12_RENDER_PASS_FLAGS flags) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRenderPassBegin);
    if (DropBundleCommand("BeginRenderPass"))
      return;
    if (RejectCommandListType("BeginRenderPass", kDirectList))
      return;
    if (render_pass_active_ ||
        (render_targets_count && !render_targets) ||
        render_targets_count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    if (flags & ~(D3D12_RENDER_PASS_FLAG_NONE |
                  D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES)) {
      // TODO(d3d12): model suspended/resumed render passes instead of
      // flattening them into ordinary target binds.
      WARN("D3D12GraphicsCommandList: suspended/resumed render passes are "
           "unsupported");
      recording_error_ = E_NOTIMPL;
      return;
    }
    render_pass_active_ = true;
    pending_render_pass_resolves_.clear();

    std::vector<dxmt::apitrace::RenderPassRenderTargetDesc> apitrace_render_targets;
    ApitraceRenderPassResolveSubresources apitrace_render_target_resolves;
    if (render_targets && render_targets_count) {
      apitrace_render_targets.reserve(render_targets_count);
      apitrace_render_target_resolves.resize(render_targets_count);
      for (UINT i = 0; i < render_targets_count; i++) {
        apitrace_render_targets.push_back(ToApitraceRenderPassRenderTarget(
            render_targets[i], apitrace_render_target_resolves[i]));
      }
    }
    std::optional<dxmt::apitrace::RenderPassDepthStencilDesc> apitrace_depth_stencil;
    std::array<std::vector<dxmt::apitrace::RenderPassResolveSubresourceDesc>, 2>
        apitrace_depth_stencil_resolves;
    if (depth_stencil) {
      apitrace_depth_stencil = ToApitraceRenderPassDepthStencil(
          *depth_stencil, apitrace_depth_stencil_resolves);
    }
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_begin_render_pass(
            this, static_cast<uint32_t>(apitrace_render_targets.size()),
            apitrace_render_targets.data(),
            apitrace_depth_stencil ? &*apitrace_depth_stencil : nullptr,
            static_cast<uint32_t>(flags));
    RenderTargetsRecord record = {};
    if (render_targets && render_targets_count) {
      record.render_targets.reserve(render_targets_count);
      for (UINT i = 0; i < render_targets_count; i++) {
        if (auto descriptor =
                GetDescriptorRecordFromCpuHandle(
                    render_targets[i].cpuDescriptor,
                    D3D12_DESCRIPTOR_HEAP_TYPE_RTV))
          record.render_targets.push_back(*descriptor);
      }
    }
    if (depth_stencil) {
      if (auto descriptor =
              GetDescriptorRecordFromCpuHandle(
                  depth_stencil->cpuDescriptor,
                  D3D12_DESCRIPTOR_HEAP_TYPE_DSV))
        record.depth_stencil = *descriptor;
    }
    AddRecord(std::move(record));

    if (render_targets && render_targets_count) {
      for (UINT i = 0; i < render_targets_count; i++)
        AddRenderPassRenderTargetAccess(render_targets[i]);
    }

    if (depth_stencil)
      AddRenderPassDepthStencilAccess(*depth_stencil);
  }

  void STDMETHODCALLTYPE EndRenderPass() override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListRenderPassEnd);
    if (DropBundleCommand("EndRenderPass"))
      return;
    if (RejectCommandListType("EndRenderPass", kDirectList))
      return;
    if (!render_pass_active_) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    render_pass_active_ = false;
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_end_render_pass(this);
    for (const auto &resolve : pending_render_pass_resolves_) {
      ResolveSubresourceRecord record = {};
      record.dst = resolve.dst;
      record.dst_subresource = resolve.dst_subresource;
      record.dst_x = resolve.dst_x;
      record.dst_y = resolve.dst_y;
      record.src = resolve.src;
      record.src_subresource = resolve.src_subresource;
      record.src_rect = resolve.src_rect;
      record.format = resolve.format;
      record.mode = resolve.mode;
      AddRecord(std::move(record));
    }
    pending_render_pass_resolves_.clear();
  }

  void STDMETHODCALLTYPE InitializeMetaCommand(
      ID3D12MetaCommand *meta_command,
      const void *initialization_parameters_data,
      SIZE_T initialization_parameters_data_size_in_bytes) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: meta commands are unsupported");
  }

  void STDMETHODCALLTYPE ExecuteMetaCommand(
      ID3D12MetaCommand *meta_command, const void *execution_parameters_data,
      SIZE_T execution_parameters_data_size_in_bytes) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: meta commands are unsupported");
  }

  void STDMETHODCALLTYPE BuildRaytracingAccelerationStructure(
      const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC *desc,
      UINT postbuild_info_descs_count,
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *postbuild_info_descs) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: raytracing acceleration structures are "
         "unsupported");
  }

  void STDMETHODCALLTYPE EmitRaytracingAccelerationStructurePostbuildInfo(
      const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC *desc,
      UINT src_acceleration_structures_count,
      const D3D12_GPU_VIRTUAL_ADDRESS *src_acceleration_structure_data) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: raytracing acceleration structures are "
         "unsupported");
  }

  void STDMETHODCALLTYPE CopyRaytracingAccelerationStructure(
      D3D12_GPU_VIRTUAL_ADDRESS dst_acceleration_structure_data,
      D3D12_GPU_VIRTUAL_ADDRESS src_acceleration_structure_data,
      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE mode) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: raytracing acceleration structures are "
         "unsupported");
  }

  void STDMETHODCALLTYPE SetPipelineState1(ID3D12StateObject *state_object) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListPipelineState);
    if (state_object) {
      recording_error_ = E_NOTIMPL;
      WARN("D3D12GraphicsCommandList: state objects are unsupported");
    }
  }

  void STDMETHODCALLTYPE DispatchRays(const D3D12_DISPATCH_RAYS_DESC *desc) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: ray dispatch is unsupported");
  }
#endif

#ifdef __ID3D12GraphicsCommandList5_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE RSSetShadingRate(
      D3D12_SHADING_RATE base_shading_rate,
      const D3D12_SHADING_RATE_COMBINER *combiners) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    if (base_shading_rate != D3D12_SHADING_RATE_1X1 || combiners) {
      recording_error_ = E_NOTIMPL;
      WARN("D3D12GraphicsCommandList: variable rate shading is unsupported");
    }
  }

  void STDMETHODCALLTYPE RSSetShadingRateImage(
      ID3D12Resource *shading_rate_image) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    if (shading_rate_image) {
      recording_error_ = E_NOTIMPL;
      WARN("D3D12GraphicsCommandList: shading rate images are unsupported");
    }
  }
#endif

#ifdef __ID3D12GraphicsCommandList6_INTERFACE_DEFINED__
  void STDMETHODCALLTYPE DispatchMesh(
      UINT thread_group_count_x,
      UINT thread_group_count_y,
      UINT thread_group_count_z) override {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListUnsupportedFeature);
    if (!thread_group_count_x || !thread_group_count_y ||
        !thread_group_count_z)
      return;
    recording_error_ = E_NOTIMPL;
    WARN("D3D12GraphicsCommandList: mesh shaders are unsupported");
  }
#endif

private:
  struct RootConstantsCacheEntry {
    UINT dst_offset = 0;
    std::vector<UINT> values;
  };

  static bool SameIndexBufferView(const D3D12_INDEX_BUFFER_VIEW &a,
                                  const D3D12_INDEX_BUFFER_VIEW &b) {
    return a.BufferLocation == b.BufferLocation &&
           a.SizeInBytes == b.SizeInBytes &&
           a.Format == b.Format;
  }

  static bool SameVertexBufferView(const D3D12_VERTEX_BUFFER_VIEW &a,
                                   const D3D12_VERTEX_BUFFER_VIEW &b) {
    return a.BufferLocation == b.BufferLocation &&
           a.SizeInBytes == b.SizeInBytes &&
           a.StrideInBytes == b.StrideInBytes;
  }

  ID3D12GraphicsCommandList *AsGraphicsCommandList() {
    return static_cast<ID3D12GraphicsCommandList *>(
        static_cast<GraphicsCommandListComBase *>(this));
  }

  void ClearRootConstantCache() {
    compute_root_constants_cache_.clear();
    graphics_root_constants_cache_.clear();
  }

  void ClearRootDescriptorTableCache() {
    compute_root_descriptor_table_cache_.clear();
    graphics_root_descriptor_table_cache_.clear();
  }

  void ClearComputeRootParameterCache() {
    compute_root_constants_cache_.clear();
    compute_root_descriptor_table_cache_.clear();
  }

  void ClearGraphicsRootParameterCache() {
    graphics_root_constants_cache_.clear();
    graphics_root_descriptor_table_cache_.clear();
  }

  void ClearInputAssemblerCache() {
    index_buffer_cache_.reset();
    for (auto &view : vertex_buffer_cache_)
      view.reset();
  }

  void ClearRecordedPipelineStateCache() {
    recorded_pipeline_state_ = nullptr;
  }

  void ClearRecordedBindingStateCache() {
    ClearRootConstantCache();
    ClearRootDescriptorTableCache();
    ClearInputAssemblerCache();
  }

  void ClearRecordedStateCache() {
    ClearRecordedPipelineStateCache();
    ClearRecordedBindingStateCache();
  }

  void RecordPipelineState(ID3D12PipelineState *pipeline_state, bool force) {
    if (!force && recorded_pipeline_state_.ptr() == pipeline_state)
      return;
    AddRecord(PipelineStateRecord{pipeline_state});
    recorded_pipeline_state_ = pipeline_state;
  }

  void InvalidateRootParameterCache(bool compute, UINT root_parameter_index) {
    if (compute) {
      compute_root_constants_cache_.erase(root_parameter_index);
      compute_root_descriptor_table_cache_.erase(root_parameter_index);
    } else {
      graphics_root_constants_cache_.erase(root_parameter_index);
      graphics_root_descriptor_table_cache_.erase(root_parameter_index);
    }
  }

  bool RootConstantsUnchanged(const RootConstantsRecord &record) {
    auto &cache = record.compute ? compute_root_constants_cache_
                                 : graphics_root_constants_cache_;
    auto it = cache.find(record.root_parameter_index);
    if (it != cache.end() &&
        it->second.dst_offset == record.dst_offset &&
        it->second.values == record.values)
      return true;

    cache[record.root_parameter_index] =
        RootConstantsCacheEntry{record.dst_offset, record.values};
    auto &table_cache = record.compute ? compute_root_descriptor_table_cache_
                                       : graphics_root_descriptor_table_cache_;
    table_cache.erase(record.root_parameter_index);
    return false;
  }

  bool RootDescriptorTableUnchanged(const RootDescriptorTableRecord &record) {
    auto &cache = record.compute ? compute_root_descriptor_table_cache_
                                 : graphics_root_descriptor_table_cache_;
    auto it = cache.find(record.root_parameter_index);
    if (it != cache.end() &&
        it->second.ptr == record.base_descriptor.ptr)
      return true;

    cache[record.root_parameter_index] = record.base_descriptor;
    auto &constants_cache = record.compute ? compute_root_constants_cache_
                                           : graphics_root_constants_cache_;
    constants_cache.erase(record.root_parameter_index);
    return false;
  }

  bool IndexBufferUnchanged(const IndexBufferRecord &record) {
    if (index_buffer_cache_.has_value() != record.view.has_value()) {
      index_buffer_cache_ = record.view;
      return false;
    }

    if (!record.view)
      return true;

    if (SameIndexBufferView(*index_buffer_cache_, *record.view))
      return true;

    index_buffer_cache_ = record.view;
    return false;
  }

  bool VertexBuffersUnchanged(const VertexBuffersRecord &record) {
    if (record.view_count == 0)
      return true;

    if (record.start_slot >= vertex_buffer_cache_.size() ||
        record.view_count > vertex_buffer_cache_.size() - record.start_slot ||
        record.views.size() != record.view_count)
      return false;

    bool unchanged = true;
    for (UINT i = 0; i < record.view_count; i++) {
      const auto slot = record.start_slot + i;
      const auto &view = record.views[i];
      const auto &cached = vertex_buffer_cache_[slot];
      if (!cached || !SameVertexBufferView(*cached, view)) {
        unchanged = false;
        break;
      }
    }

    if (unchanged)
      return true;

    for (UINT i = 0; i < record.view_count; i++)
      vertex_buffer_cache_[record.start_slot + i] = record.views[i];
    return false;
  }

  bool IsPipelineStateCompatible(ID3D12PipelineState *pipeline_state) const {
    if (!pipeline_state)
      return true;

    const auto *state = dynamic_cast<PipelineState *>(pipeline_state);
    if (!state || state->GetParentDevice() != device_.ptr())
      return false;

    switch (type_) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
    case D3D12_COMMAND_LIST_TYPE_BUNDLE:
      return true;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
      return state->GetType() == PipelineStateType::Compute;
    default:
      return false;
    }
  }

  // Snapshot a copy SOURCE buffer's read region into the apitrace stream, as if it had been
  // Unmapped just before this copy. This covers persistently mapped upload buffers that are written
  // directly and later consumed by copy commands. No-op for non-CPU-mapped sources.
  void SnapshotCopySourceBuffer(ID3D12Resource *src_buffer, UINT64 src_offset, UINT64 byte_count) {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListSnapshotCapture);
    if (!src_buffer || byte_count == 0 || !dxmt::apitrace::d3d_enabled())
      return;
    auto *res = dynamic_cast<Resource *>(src_buffer);
    if (!res)
      return;
    auto *alloc = res->GetBufferAllocation();
    if (!alloc)
      return;
    auto *mapped = static_cast<const char *>(alloc->mappedMemory(0));
    if (!mapped)
      return;  // not CPU-mapped (DEFAULT heap): no CPU-visible source data to capture
    const UINT64 width = res->GetResourceDesc().Width;
    if (src_offset >= width)
      return;
    const UINT64 clamped_byte_count = std::min<UINT64>(byte_count, width - src_offset);
    const UINT64 end = src_offset + clamped_byte_count;
    if (end <= src_offset)
      return;
    const char *span_data = mapped + res->GetHeapOffset() + src_offset;
    const size_t span_len = static_cast<size_t>(end - src_offset);

    const uint64_t fingerprint = FnvHashSpan(span_data, span_len);
    const CopySnapshotKey key{src_buffer, src_offset, end};
    {
      std::lock_guard<std::mutex> lock(g_copy_snapshot_dedup_mutex);
      auto it = g_copy_snapshot_dedup.find(key);
      if (it != g_copy_snapshot_dedup.end() && it->second == fingerprint)
        return; // unchanged since last emit -> already in trace
      // Bound memory on pathologically long traces. Clearing is correctness-safe; it only forces
      // re-emitting spans seen before the clear.
      constexpr size_t kMaxDedupEntries = 2u * 1024u * 1024u;
      if (g_copy_snapshot_dedup.size() >= kMaxDedupEntries)
        g_copy_snapshot_dedup.clear();
      g_copy_snapshot_dedup[key] = fingerprint;
    }

    dxmt::apitrace::record_resource_snapshot(
        src_buffer, 0, src_offset, end, span_data, span_len);
  }

  void SnapshotCopyTextureSourceBuffer(const D3D12_TEXTURE_COPY_LOCATION *src,
                                       const D3D12_BOX *src_box) {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListSnapshotCapture);
    if (!src || !src->pResource ||
        src->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT)
      return;

    const auto &placed = src->PlacedFootprint;
    const auto &fp = placed.Footprint;
    const UINT64 width64 = std::max<UINT64>(fp.Width, 1);
    const UINT height = std::max<UINT>(fp.Height, 1);
    const UINT depth = std::max<UINT>(fp.Depth, 1);
    const UINT width = static_cast<UINT>(std::min<UINT64>(width64, UINT64(UINT_MAX)));

    UINT top = 0;
    UINT front = 0;
    UINT bottom = height;
    UINT back = depth;
    if (src_box) {
      top = std::min(src_box->top, bottom);
      front = std::min(src_box->front, back);
      bottom = std::min(src_box->bottom, bottom);
      back = std::min(src_box->back, back);
      const UINT left = std::min(src_box->left, width);
      const UINT right = std::min(src_box->right, width);
      if (right <= left)
        return;
    }
    if (bottom <= top || back <= front)
      return;

    DXGIFormatFootprintLayout layout = {};
    if (!GetDXGIFormatFootprintLayout(static_cast<uint32_t>(fp.Format), layout) ||
        layout.blockHeight == 0) {
      const UINT64 rows = static_cast<UINT64>(height) * depth;
      SnapshotCopySourceBuffer(
          src->pResource,
          placed.Offset,
          static_cast<UINT64>(fp.RowPitch) * rows);
      return;
    }

    const UINT64 block_height = std::max<uint32_t>(layout.blockHeight, 1u);
    const UINT64 footprint_rows =
        (static_cast<UINT64>(height) + block_height - 1) / block_height;
    const UINT64 row_begin = static_cast<UINT64>(top) / block_height;
    const UINT64 row_end =
        std::min<UINT64>((static_cast<UINT64>(bottom) + block_height - 1) / block_height,
                         footprint_rows);
    if (row_end <= row_begin)
      return;

    const UINT64 row_pitch = fp.RowPitch;
    const UINT64 slice_pitch = row_pitch * footprint_rows;
    const UINT64 bytes_per_slice = (row_end - row_begin) * row_pitch;
    for (UINT z = front; z < back; ++z) {
      const UINT64 offset =
          placed.Offset + static_cast<UINT64>(z) * slice_pitch + row_begin * row_pitch;
      SnapshotCopySourceBuffer(src->pResource, offset, bytes_per_slice);
    }
  }

  void SnapshotGpuVirtualBufferRange(D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 byte_count) {
    dxmt::perf::ScopedCodeTimer perf_timer(
        dxmt::PerfCodePath::CommandListSnapshotCapture);
    if (address == 0 || byte_count == 0 || !dxmt::apitrace::d3d_enabled())
      return;
    UINT64 offset = 0;
    Resource *resource = LookupBufferResourceByGpuVirtualAddress(address, &offset);
    if (!resource)
      return;
    SnapshotCopySourceBuffer(resource->GetD3D12Resource(), offset, byte_count);
  }

  bool ValidateResolveSubresource(
      ID3D12Resource *dst_resource, UINT dst_subresource, UINT dst_x,
      UINT dst_y, ID3D12Resource *src_resource, UINT src_subresource,
      const D3D12_RECT *src_rect, DXGI_FORMAT format, bool full_resource) const {
    auto *dst = dynamic_cast<Resource *>(dst_resource);
    auto *src = dynamic_cast<Resource *>(src_resource);
    if (!dst || !src || dst->GetParentDevice() != device_.ptr() ||
        src->GetParentDevice() != device_.ptr())
      return false;
    const auto &dst_desc = dst->GetResourceDesc();
    const auto &src_desc = src->GetResourceDesc();
    if (dst_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        src_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        dst_desc.Dimension != src_desc.Dimension ||
        dst_desc.SampleDesc.Count != 1 || src_desc.SampleDesc.Count <= 1 ||
        format == DXGI_FORMAT_UNKNOWN ||
        (!IsDXGIFormatPlaneCompatible(dst_desc.Format, format, 0) &&
         !AreDXGIFormatsInSameTypeGroup(dst_desc.Format, format)) ||
        (!IsDXGIFormatPlaneCompatible(src_desc.Format, format, 0) &&
         !AreDXGIFormatsInSameTypeGroup(src_desc.Format, format)))
      return false;

    const auto subresource_count = [](const D3D12_RESOURCE_DESC &desc) {
      const UINT array_count =
          desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D
              ? 1u
              : UINT(desc.DepthOrArraySize);
      const UINT plane_count =
          std::max(1u, GetDXGIFormatTraits(desc.Format).planeCount);
      return UINT64(desc.MipLevels) * array_count * plane_count;
    };
    if (dst_subresource >= subresource_count(dst_desc) ||
        src_subresource >= subresource_count(src_desc))
      return false;
    const UINT dst_mip = dst_subresource % dst_desc.MipLevels;
    const UINT src_mip = src_subresource % src_desc.MipLevels;
    const UINT64 dst_width = std::max<UINT64>(1, dst_desc.Width >> dst_mip);
    const UINT64 src_width = std::max<UINT64>(1, src_desc.Width >> src_mip);
    const UINT dst_height = std::max<UINT>(1, dst_desc.Height >> dst_mip);
    const UINT src_height = std::max<UINT>(1, src_desc.Height >> src_mip);
    if (full_resource)
      return dst_width == src_width && dst_height == src_height;

    const int64_t left = src_rect ? src_rect->left : 0;
    const int64_t top = src_rect ? src_rect->top : 0;
    const int64_t right = src_rect ? src_rect->right : int64_t(src_width);
    const int64_t bottom = src_rect ? src_rect->bottom : int64_t(src_height);
    if (left < 0 || top < 0 || right <= left || bottom <= top ||
        uint64_t(right) > src_width || uint64_t(bottom) > src_height)
      return false;
    const uint64_t width = uint64_t(right - left);
    const uint64_t height = uint64_t(bottom - top);
    return uint64_t(dst_x) <= dst_width && width <= dst_width - dst_x &&
           uint64_t(dst_y) <= dst_height && height <= dst_height - dst_y;
  }

  void RecordCopyBufferRegion(const char *method, ID3D12Resource *dst_buffer,
                              UINT64 dst_offset, ID3D12Resource *src_buffer,
                              UINT64 src_offset, UINT64 byte_count) {
    if (!byte_count)
      return;
    auto *dst = dynamic_cast<Resource *>(dst_buffer);
    auto *src = dynamic_cast<Resource *>(src_buffer);
    if (!dst || !src || dst->GetParentDevice() != device_.ptr() ||
        src->GetParentDevice() != device_.ptr() ||
        dst->GetResourceDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        src->GetResourceDesc().Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
        dst_offset > dst->GetResourceDesc().Width ||
        byte_count > dst->GetResourceDesc().Width - dst_offset ||
        src_offset > src->GetResourceDesc().Width ||
        byte_count > src->GetResourceDesc().Width - src_offset ||
        (dst_buffer == src_buffer && dst_offset < src_offset + byte_count &&
         src_offset < dst_offset + byte_count)) {
      recording_error_ = E_INVALIDARG;
      return;
    }
    SnapshotCopySourceBuffer(src_buffer, src_offset, byte_count);
    g_current_command_record_d3d_sequence =
        dxmt::apitrace::record_copy_buffer_region(
            method, this, dst_buffer, dst_offset, src_buffer, src_offset,
            byte_count);
    AddRecord(CopyBufferRegionRecord{
        dst_buffer, dst_offset, src_buffer, src_offset, byte_count});
  }

  static constexpr UINT CommandListTypeFlag(D3D12_COMMAND_LIST_TYPE type) {
    return 1u << static_cast<UINT>(type);
  }

  static constexpr UINT kDirectList =
      1u << static_cast<UINT>(D3D12_COMMAND_LIST_TYPE_DIRECT);
  static constexpr UINT kBundleList =
      1u << static_cast<UINT>(D3D12_COMMAND_LIST_TYPE_BUNDLE);
  static constexpr UINT kComputeList =
      1u << static_cast<UINT>(D3D12_COMMAND_LIST_TYPE_COMPUTE);
  static constexpr UINT kCopyList =
      1u << static_cast<UINT>(D3D12_COMMAND_LIST_TYPE_COPY);

  bool RejectCommandListType(const char *command, UINT allowed_types) {
    if (allowed_types & CommandListTypeFlag(type_))
      return false;
    if (recording_error_ == S_OK)
      recording_error_ = E_FAIL;
    WARN("D3D12GraphicsCommandList: ", command,
         " is not valid for command list type ", static_cast<UINT>(type_));
    return true;
  }

  bool DropBundleCommand(const char *command) const {
    if (type_ != D3D12_COMMAND_LIST_TYPE_BUNDLE)
      return false;
    WARN("D3D12GraphicsCommandList: ", command,
         " is not valid in a bundle and was dropped");
    return true;
  }

  // D3D12 requires DestOffsetIn32BitValues + Num32BitValuesToSet to stay
  // inside the range the root signature declares for the parameter. The bound
  // root signature is not always known here (a bundle may inherit it), but a
  // root signature is capped at D3D12_MAX_ROOT_COST DWORDs and each 32-bit
  // root constant costs one DWORD, so that cap holds for every legal call
  // regardless of which signature is bound. Beyond it the call is an
  // application bug -- a debug-layer error on Windows -- and pSrcData is not
  // trustworthy either, so drop the command before reading or recording it.
  bool DropOutOfRangeRootConstants(const char *command,
                                   UINT root_parameter_index,
                                   UINT constant_count, UINT dst_offset) const {
    if (uint64_t(dst_offset) + constant_count <= D3D12_MAX_ROOT_COST)
      return false;
    static std::atomic<uint32_t> log_count = 0;
    if (log_count.fetch_add(1, std::memory_order_relaxed) < 8) {
      WARN("D3D12GraphicsCommandList: ", command,
           " destination range is outside the root signature and was dropped"
           " rootParameterIndex=", root_parameter_index,
           " num32BitValuesToSet=", constant_count,
           " destOffsetIn32BitValues=", dst_offset);
    }
    return true;
  }

  template <typename T>
  void AddRecord(T &&payload) {
    if (closed_)
      return;
    // Track command-list recording on the calling thread.
    // ScopedFrameTimer is a no-op unless this thread has frame stats bound (the
    // present thread), so a non-zero CommandListRecord total proves recording
    // contributes to the measured frame wall.
    dxmt::perf::ScopedFrameTimer perf_timer(
        dxmt::perf::FrameTimeBucket::CommandListRecord);
    dxmt::perf::ScopedCodeTimer code_timer(
        dxmt::PerfCodePath::CommandListRecordVectorAppend);
    using RecordType = std::remove_cvref_t<T>;
    constexpr auto compile_kind = [] {
      if constexpr (std::is_same_v<RecordType, DrawInstancedRecord> ||
                    std::is_same_v<RecordType,
                                   DrawIndexedInstancedRecord>)
        return CommandRecordCompileKind::Graphics;
      if constexpr (std::is_same_v<RecordType, DispatchRecord>)
        return CommandRecordCompileKind::Compute;
      if constexpr (std::is_same_v<RecordType, ResourceBarrierRecord>)
        return CommandRecordCompileKind::Barrier;
      return CommandRecordCompileKind::Other;
    }();
    if constexpr (std::is_same_v<RecordType, DrawInstancedRecord> ||
                  std::is_same_v<RecordType, DrawIndexedInstancedRecord>) {
      recorded_graphics_packet_count_++;
    } else if constexpr (std::is_same_v<RecordType, DispatchRecord>) {
      recorded_compute_packet_count_++;
    } else if constexpr (std::is_same_v<RecordType, ResourceBarrierRecord>) {
      recorded_barrier_record_count_++;
      recorded_barrier_count_ += static_cast<UINT>(payload.barriers.size());
    }
    records_.push_back(CommandRecord{
        g_current_command_record_d3d_sequence, compile_kind,
        std::forward<T>(payload)});
  }

#ifdef __ID3D12GraphicsCommandList4_INTERFACE_DEFINED__
  void AddRenderPassRenderTargetAccess(
      const D3D12_RENDER_PASS_RENDER_TARGET_DESC &render_target) {
    auto descriptor =
        GetDescriptorRecordFromCpuHandle(render_target.cpuDescriptor,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    if (!descriptor)
      return;

    switch (render_target.BeginningAccess.Type) {
    case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR: {
      ClearRenderTargetRecord clear = {};
      clear.descriptor = *descriptor;
      std::copy(std::begin(render_target.BeginningAccess.Clear.ClearValue.Color),
                std::end(render_target.BeginningAccess.Clear.ClearValue.Color),
                clear.color.begin());
      AddRecord(std::move(clear));
      break;
    }
    case D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD:
      AddRenderPassDiscard(*descriptor);
      break;
    default:
      if (!IsRenderPassPreserveOrNoAccess(render_target.BeginningAccess.Type))
        WARN("D3D12GraphicsCommandList: unsupported render pass RTV beginning "
             "access type ",
             render_target.BeginningAccess.Type);
      break;
    }

    AddRenderPassEndingAccess(render_target.EndingAccess);
  }

  void AddRenderPassDepthStencilAccess(
      const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC &depth_stencil) {
    auto descriptor =
        GetDescriptorRecordFromCpuHandle(depth_stencil.cpuDescriptor,
                                         D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    if (!descriptor)
      return;

    ClearDepthStencilRecord clear = {};
    clear.descriptor = *descriptor;
    clear.flags = D3D12_CLEAR_FLAGS(0);
    if (depth_stencil.DepthBeginningAccess.Type ==
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
      clear.flags =
          static_cast<D3D12_CLEAR_FLAGS>(clear.flags | D3D12_CLEAR_FLAG_DEPTH);
      clear.depth =
          depth_stencil.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth;
    } else if (depth_stencil.DepthBeginningAccess.Type ==
               D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD) {
      AddRenderPassDiscard(*descriptor);
    } else if (!IsRenderPassPreserveOrNoAccess(
                   depth_stencil.DepthBeginningAccess.Type)) {
      WARN("D3D12GraphicsCommandList: unsupported render pass depth beginning "
           "access type ",
           depth_stencil.DepthBeginningAccess.Type);
    }

    if (depth_stencil.StencilBeginningAccess.Type ==
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
      clear.flags =
          static_cast<D3D12_CLEAR_FLAGS>(clear.flags | D3D12_CLEAR_FLAG_STENCIL);
      clear.stencil =
          depth_stencil.StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil;
    } else if (depth_stencil.StencilBeginningAccess.Type ==
               D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD) {
      AddRenderPassDiscard(*descriptor);
    } else if (!IsRenderPassPreserveOrNoAccess(
                   depth_stencil.StencilBeginningAccess.Type)) {
      WARN("D3D12GraphicsCommandList: unsupported render pass stencil "
           "beginning access type ",
           depth_stencil.StencilBeginningAccess.Type);
    }

    if (clear.flags != D3D12_CLEAR_FLAGS(0))
      AddRecord(std::move(clear));

    AddRenderPassEndingAccess(depth_stencil.DepthEndingAccess);
    AddRenderPassEndingAccess(depth_stencil.StencilEndingAccess);
  }

  void AddRenderPassDiscard(const DescriptorRecord &descriptor) {
    if (!descriptor.resource)
      return;
    DiscardResourceRecord discard = {};
    discard.resource = descriptor.resource.ptr();
    AddRecord(std::move(discard));
  }

  void AddRenderPassEndingAccess(
      const D3D12_RENDER_PASS_ENDING_ACCESS &ending_access) {
    switch (ending_access.Type) {
    case D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE:
      AddRenderPassResolve(ending_access.Resolve);
      break;
    case D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD:
      // D3D12 render-pass discard only says prior contents do not need to be
      // preserved after the pass. The current backend has no end-store action,
      // so this remains a conservative no-op.
      break;
    default:
      if (!IsRenderPassPreserveOrNoAccess(ending_access.Type))
        WARN("D3D12GraphicsCommandList: unsupported render pass ending access "
             "type ",
             ending_access.Type);
      break;
    }
  }

  void AddRenderPassResolve(
      const D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_PARAMETERS &resolve) {
    if (!resolve.pSrcResource || !resolve.pDstResource ||
        !resolve.SubresourceCount || !resolve.pSubresourceParameters)
      return;

    if (resolve.ResolveMode == D3D12_RESOLVE_MODE_DECOMPRESS) {
      recording_error_ = E_NOTIMPL;
      WARN("D3D12GraphicsCommandList: render pass decompress resolve is unsupported");
      return;
    }

    for (UINT i = 0; i < resolve.SubresourceCount; i++) {
      const auto &subresource = resolve.pSubresourceParameters[i];
      pending_render_pass_resolves_.push_back(PendingRenderPassResolve{
          resolve.pSrcResource, resolve.pDstResource,
          subresource.SrcSubresource, subresource.DstSubresource,
          subresource.DstX, subresource.DstY, subresource.SrcRect,
          resolve.Format, resolve.ResolveMode});
    }
  }
#endif

  Com<IMTLD3D12Device> device_;
  UINT node_mask_;
  D3D12_COMMAND_LIST_TYPE type_;
  Com<CommandAllocatorObject, false> allocator_;
  Com<ID3D12PipelineState> initial_pipeline_state_;
  Com<ID3D12PipelineState> current_pipeline_state_;
  Com<ID3D12PipelineState> recorded_pipeline_state_;
  Com<ID3D12RootSignature> compute_root_signature_;
  Com<ID3D12RootSignature> graphics_root_signature_;
  std::unordered_map<UINT, RootConstantsCacheEntry> compute_root_constants_cache_;
  std::unordered_map<UINT, RootConstantsCacheEntry> graphics_root_constants_cache_;
  std::unordered_map<UINT, D3D12_GPU_DESCRIPTOR_HANDLE> compute_root_descriptor_table_cache_;
  std::unordered_map<UINT, D3D12_GPU_DESCRIPTOR_HANDLE> graphics_root_descriptor_table_cache_;
  std::optional<D3D12_INDEX_BUFFER_VIEW> index_buffer_cache_;
  std::array<std::optional<D3D12_VERTEX_BUFFER_VIEW>,
             D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT>
      vertex_buffer_cache_;
  ComPrivateData private_data_;
  std::vector<CommandRecord> records_;
  std::shared_ptr<const std::vector<CommandRecord>> closed_records_;
  UINT recorded_graphics_packet_count_ = 0;
  UINT recorded_compute_packet_count_ = 0;
  UINT recorded_barrier_record_count_ = 0;
  UINT recorded_barrier_count_ = 0;
  std::shared_ptr<const CompiledCommandList> compiled_commands_;
  dxmt::d3d12::test::ExecutionPathConfig test_path_config_ = {};
  bool test_path_configured_ = false;
  FLOAT depth_bounds_min_ = 0.0f;
  FLOAT depth_bounds_max_ = 1.0f;
  UINT view_instance_mask_ = 0xffffffffu;
  std::vector<PendingRenderPassResolve> pending_render_pass_resolves_;
  std::map<std::pair<QueryHeap *, UINT>, D3D12_QUERY_TYPE> active_queries_;
  bool render_pass_active_ = false;
  bool closed_ = false;
  bool submitted_ = false;
  bool apitrace_lifecycle_recording_enabled_ = true;
  HRESULT recording_error_ = S_OK;
  std::string name_;
};

class CommandSignatureImpl final
    : public ComObjectWithInitialRef<ID3D12CommandSignature>,
      public CommandSignature {
public:
  CommandSignatureImpl(IMTLD3D12Device *device,
                       const D3D12_COMMAND_SIGNATURE_DESC &desc,
                       ID3D12RootSignature *root_signature)
      : device_(device), root_signature_(root_signature), desc_(desc) {
    arguments_.assign(desc.pArgumentDescs,
                      desc.pArgumentDescs + desc.NumArgumentDescs);
    desc_.pArgumentDescs = arguments_.empty() ? nullptr : arguments_.data();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) override {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D12Object) ||
        riid == __uuidof(ID3D12DeviceChild) ||
        riid == __uuidof(ID3D12Pageable) ||
        riid == __uuidof(ID3D12CommandSignature)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D12CommandSignature), riid))
      WARN("D3D12CommandSignature: unknown interface query ",
           str::format(riid));
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT *data_size,
                                           void *data) override {
    return private_data_.getData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID guid, UINT data_size,
                                           const void *data) override {
    return private_data_.setData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID guid,
                                                   const IUnknown *data) override {
    return private_data_.setInterface(guid, data);
  }

  HRESULT STDMETHODCALLTYPE SetName(const WCHAR *name) override {
    name_ = name ? str::fromws(name) : std::string();
    return private_data_.setName(name);
  }

  HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void **device) override {
    return device_->QueryInterface(riid, device);
  }

  IMTLD3D12Device *GetParentDevice() const override { return device_.ptr(); }

  const D3D12_COMMAND_SIGNATURE_DESC &GetDesc() const override {
    return desc_;
  }

  const std::vector<D3D12_INDIRECT_ARGUMENT_DESC> &GetArguments() const override {
    return arguments_;
  }

  ID3D12RootSignature *GetRootSignature() const override {
    return root_signature_.ptr();
  }

private:
  Com<IMTLD3D12Device> device_;
  Com<ID3D12RootSignature> root_signature_;
  ComPrivateData private_data_;
  D3D12_COMMAND_SIGNATURE_DESC desc_ = {};
  std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments_;
  std::string name_;
};

} // namespace

std::shared_ptr<SubmittedCompiledCommandListPlan>
PrepareSubmittedCompiledCommandList(
    std::shared_ptr<const CompiledCommandList> compiled) {
  return PrepareSubmittedCompiledCommandListImpl(std::move(compiled));
}

Com<ID3D12GraphicsCommandList>
CreateGraphicsCommandList(IMTLD3D12Device *device, UINT node_mask,
                          D3D12_COMMAND_LIST_TYPE type,
                          ID3D12CommandAllocator *command_allocator,
                          ID3D12PipelineState *initial_pipeline_state,
                          HRESULT *status) {
  auto *list = new GraphicsCommandListImpl(
      device, node_mask, type, command_allocator, initial_pipeline_state, status);
  if (status && FAILED(*status)) {
    list->ReleasePrivate();
    return nullptr;
  }
  return Com<ID3D12GraphicsCommandList>::transfer(list);
}

Com<ID3D12CommandSignature>
CreateCommandSignature(IMTLD3D12Device *device,
                       const D3D12_COMMAND_SIGNATURE_DESC *desc,
                       ID3D12RootSignature *root_signature) {
  return Com<ID3D12CommandSignature>::transfer(
      new CommandSignatureImpl(device, *desc, root_signature));
}

} // namespace dxmt::d3d12
