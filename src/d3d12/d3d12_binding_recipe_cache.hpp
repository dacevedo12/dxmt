#pragma once

// On-disk cache for the descriptor-table binding recipe, plus the periodic
// summary log for the binding-recipe diagnostic counters.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_plans.inc). The cache is keyed purely by
// (shader cache key, serialized root signature, compute flag) and the counters
// are process-wide, so nothing here touches the command queue instance and it
// can be compiled and analysed on its own.

#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_root_signature.hpp"
#include "sha1/sha1_util.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace dxmt::d3d12 {

struct DescriptorTableBindingRecipeBlobHeader {
  uint32_t magic = 0x42524344; // DCRB
  uint32_t version = 1;
  uint32_t entry_size = sizeof(DescriptorTableBindingRecipeEntry);
  uint32_t entry_count = 0;
};

inline constexpr uint64_t kD3D12BindingRecipeCacheVersion = 5;

// Emits one INFO line summarizing the binding-recipe cache counters. No-op
// unless the binding-recipe cache diagnostic is enabled or no calls happened.
void LogBindingRecipeDiagSummary(const char *reason);

// Rate-limited LogBindingRecipeDiagSummary: logs for the first few calls and
// then once every cache-summary interval.
void MaybeLogBindingRecipeDiagSummary(uint64_t calls, const char *reason);

[[nodiscard]] std::string BuildDescriptorTableBindingRecipeCachePath();

// Cache key covering the pipeline's shader cache key and the serialized root
// signature blob.
[[nodiscard]] Sha1Digest
BuildDescriptorTableBindingRecipeKey(const PipelineState &pipeline,
                                     const RootSignature &root, bool compute);

// Reads a previously stored recipe. Returns nullopt when the shader cache is
// disabled, the entry is missing, or the blob header does not match.
[[nodiscard]] std::optional<DescriptorTableBindingRecipe>
LoadDescriptorTableBindingRecipe(const Sha1Digest &key);

void
StoreDescriptorTableBindingRecipe(const Sha1Digest &key,
                                  const DescriptorTableBindingRecipe &recipe);

} // namespace dxmt::d3d12
