#include "d3d12_binding_debug_log.hpp"

#include "d3d12_descriptor_diagnostics.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_diagnostics_report.hpp"
#include "d3d12_selected_descriptor_diag.hpp"
#include "d3d12_shader_binding.hpp"
#include "log/log.hpp"
#include "sha1/sha1_util.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace dxmt::d3d12 {

std::string
ShaderDigestForStage(const PipelineState &pipeline, PipelineStage stage) {
  const auto *shader = FindShaderForStage(pipeline, stage);
  if (!shader || !shader->cached_shader)
    return {};
  struct CachedDigest {
    std::weak_ptr<PipelineCachedShader> owner;
    std::string digest;
  };
  static std::mutex digest_mutex;
  static std::unordered_map<const PipelineCachedShader *, CachedDigest>
      digest_cache;
  const auto owner = shader->cached_shader;
  {
    std::lock_guard lock(digest_mutex);
    const auto it = digest_cache.find(owner.get());
    if (it != digest_cache.end()) {
      const auto live_owner = it->second.owner.lock();
      if (live_owner && live_owner.get() == owner.get())
        return it->second.digest;
    }
  }
  const auto &bytecode = shader->bytecode();
  const auto digest =
      Sha1HashState::compute(bytecode.data(), bytecode.size()).string();
  {
    std::lock_guard lock(digest_mutex);
    digest_cache[owner.get()] = CachedDigest{owner, digest};
  }
  return digest;
}

bool
D3D12DiagPipelineStageSelected(const PipelineState &pipeline,
                               PipelineStage stage,
                               std::string *shader_digest) {
  const auto digest = ShaderDigestForStage(pipeline, stage);
  if (shader_digest)
    *shader_digest = digest;
  return D3D12DiagShaderKeySelected(pipeline.GetShaderCacheKey()) ||
         (!digest.empty() && D3D12DiagShaderKeySelected(digest));
}

void
DebugLogRootBinding(const char *kind, const PipelineState &pipeline,
                    bool compute, PipelineStage stage, UINT root_index,
                    UINT slot, UINT shader_register, UINT register_space,
                    UINT64 size, D3D12_GPU_VIRTUAL_ADDRESS address,
                    const DescriptorRecord *descriptor,
                    const DXMT12_MTL4_SHADER_ARGUMENT *argument) {
  if (!D3D12DiagBindingsEnabled())
    return;

  const auto &cache_key = pipeline.GetShaderCacheKey();
  std::string shader_digest;
  if (!D3D12DiagPipelineStageSelected(pipeline, stage, &shader_digest))
    return;

  static std::atomic<uint64_t> consistency_sample_count = 0;
  const auto consistency_occurrence =
      consistency_sample_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool periodic_consistency_sample =
      consistency_occurrence <= 64 ||
      (consistency_occurrence & (consistency_occurrence - 1)) == 0 ||
      (consistency_occurrence % 4096) == 0;
  // A stale-version mismatch is the highest-value transient and only costs
  // one mirror lock to test. Deep payload inspection is otherwise sampled.
  const bool version_mismatch =
      descriptor && descriptor->mirror &&
      descriptor->heap_index < descriptor->mirror->numDescriptors() &&
      descriptor->mirror->slotStaleVersion(descriptor->heap_index) !=
          descriptor->slot_version;
  const bool consistency_sampled =
      descriptor && (periodic_consistency_sample || version_mismatch);
  const auto consistency =
      consistency_sampled
          ? DiagnoseSelectedDescriptor(*descriptor, argument)
          : SelectedDescriptorConsistency{};
  if (consistency_sampled && consistency.flags) {
    static std::atomic<uint32_t> anomaly_log_count = 0;
    if (D3D12DiagShouldLog(anomaly_log_count, true)) {
      WARN("D3D12 diagnostic: selected descriptor inconsistency",
           " pso=", pipeline.GetShaderCacheKey(),
           " shader=", shader_digest,
           " stage=", PipelineStageName(stage),
           " kind=", kind,
           " root=", root_index,
           " slot=", slot,
           " register=", shader_register,
           " heapIndex=", descriptor->heap_index,
           " flags=0x", std::hex, consistency.flags, std::dec,
           " reasons=", consistency.reasons,
           " legalNull=", consistency.legal_null,
           " recordVersion=", descriptor->slot_version.epoch, ":",
           descriptor->slot_version.sequence,
           " staleVersion=", consistency.stale_version.epoch, ":",
           consistency.stale_version.sequence,
           " filledVersion=", consistency.filled_version.epoch, ":",
           consistency.filled_version.sequence,
           " needsFill=", consistency.needs_fill,
           " expectedKind=", uint32_t(consistency.expected_kind),
           " actualKind=", uint32_t(consistency.actual_kind),
           " tableGpuVa=", consistency.table.gpu_va,
           " tableTexture=", consistency.table.texture_view_id,
           " tableMetadata=", consistency.table.metadata,
           " textureHandle=", consistency.texture.handle,
           " textureMetadata=", consistency.texture.metadata,
           " nativeResource=", consistency.native.resource_index,
           " nativeFlags=0x", std::hex, consistency.native.flags,
           " nativeDiag=0x", consistency.native_diag_flags, std::dec,
           " nativeOffset=", consistency.native.byte_offset,
           " nativeSize=", consistency.native.byte_size);
    }
  }

  static std::atomic<uint32_t> log_count = 0;
  if (!D3D12DiagShouldLog(log_count, true))
    return;

  const auto key_size = std::min<size_t>(cache_key.size(), 16);
  std::string key_prefix(cache_key.c_str(), cache_key.c_str() + key_size);
  INFO("D3D12 diagnostic: root binding",
       " kind=", kind,
       " pso=", key_prefix,
       " shader=", shader_digest,
       " pipeline=", compute ? "compute" : "graphics",
       " stage=", PipelineStageName(stage),
       " root=", root_index,
       " slot=", slot,
       " register=", shader_register,
       " space=", register_space,
       " size=", uint64_t(size),
       " address=", uint64_t(address),
       " descriptor=", static_cast<const void *>(descriptor),
       " descriptorType=",
       descriptor ? uint32_t(descriptor->type) : uint32_t(UINT_MAX),
       " hasDesc=", descriptor && descriptor->has_desc ? 1 : 0,
       " heapIndex=", descriptor ? descriptor->heap_index : UINT_MAX,
       " heapCount=", descriptor ? descriptor->heap_count : 0,
       " slotVersion=",
       descriptor ? descriptor->slot_version.epoch : 0, ":",
       descriptor ? descriptor->slot_version.sequence : 0,
       " classification=",
       descriptor ? (!consistency_sampled
                         ? "not-sampled"
                         : consistency.legal_null
                               ? "legal-null"
                               : consistency.flags ? "anomalous"
                                                   : "consistent")
                  : "not-descriptor",
       " consistencyFlags=0x", std::hex, consistency.flags, std::dec,
       " resource=",
       descriptor ? static_cast<const void *>(descriptor->resource.ptr())
                  : nullptr,
       " format=",
       descriptor ? uint32_t(D3D12DiagDescriptorFormat(*descriptor))
                  : uint32_t(DXGI_FORMAT_UNKNOWN));
}

} // namespace dxmt::d3d12
