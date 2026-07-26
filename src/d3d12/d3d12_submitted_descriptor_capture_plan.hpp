#pragma once

// Work that CaptureSubmittedDescriptorSnapshots() does *before* it takes the
// heap-mirror locks: which mirrors participate in the submission, a memo for
// the immutable reflection recipes, and the upper bound used to size the
// descriptor record store.
//
// These were open-coded lambdas and local structs inside that member in
// d3d12_command_queue_pass_queue.inc. None of them touches the queue: they
// read the compiled command list and call the free GetPipelineState() /
// GetRootSignature() / GetDescriptorTableBindingRecipe() helpers. Keeping them
// out of the locked region is deliberate — database and cache work must not
// extend the writer-blocking interval — and hoisting them to dxmt::d3d12 makes
// that separation checkable in isolation.

#include "airconv_dx12_metal4.h"
#include "d3d12_command_list.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_root_signature.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace dxmt::d3d12 {

/** Every shader-visible heap mirror referenced by a compiled-path packet of
 *  this submission, sorted and deduplicated so the caller can lock each one
 *  exactly once. Packets that already fell back are skipped: they never reach
 *  descriptor capture. */
[[nodiscard]] std::vector<DescriptorHeapMirror *>
CollectSubmissionDescriptorHeapMirrors(const CompiledCommandList &compiled);

/** Per-submission memo for GetDescriptorTableBindingRecipe(). A recipe is a
 *  pure function of the binding layout, the shader ABI, the root signature and
 *  the stage, so packets sharing those four reuse one lookup instead of
 *  re-entering the global recipe cache thousands of times per Execute. */
class SubmissionDescriptorRecipeCache {
public:
  /** Bounded pre-sizing: the memo is keyed by layout, not by packet, so the
   *  packet count is only an upper bound worth honoring for small lists. */
  void Reserve(size_t packet_count);

  const DescriptorTableBindingRecipe &
  Get(PipelineState &pipeline, RootSignature &root, bool compute,
      const CompiledCommandPipelineMetadata &metadata);

private:
  struct Key {
    uint64_t binding_layout_fingerprint = 0;
    DXMT12_MTL4_SHADER_ABI_VERSION shader_abi_version =
        DXMT12_MTL4_SHADER_ABI_BINDLESS_MIRROR;
    RootSignature *root = nullptr;
    bool compute = false;

    bool operator==(const Key &other) const {
      return binding_layout_fingerprint == other.binding_layout_fingerprint &&
             shader_abi_version == other.shader_abi_version &&
             root == other.root && compute == other.compute;
    }
  };

  struct KeyHash {
    size_t operator()(const Key &key) const {
      auto hash = std::hash<uint64_t>{}(key.binding_layout_fingerprint);
      hash ^= std::hash<uint32_t>{}(
                  static_cast<uint32_t>(key.shader_abi_version)) +
              0x9e3779b9u + (hash << 6) + (hash >> 2);
      hash ^= std::hash<RootSignature *>{}(key.root) + 0x9e3779b9u +
              (hash << 6) + (hash >> 2);
      hash ^= size_t(key.compute) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
      return hash;
    }
  };

  std::unordered_map<Key, const DescriptorTableBindingRecipe *, KeyHash>
      entries_;
};

/** Total number of reflected descriptor entries this submission could capture,
 *  counting only packets that actually reach descriptor capture. It is a safe
 *  upper bound, not an estimate of unique slots. Populates `cache` on the way,
 *  so the capture loops hit the memo warm. */
[[nodiscard]] size_t ComputeSubmittedDescriptorCaptureCapacity(
    const CompiledCommandList &compiled,
    SubmissionDescriptorRecipeCache &cache);

} // namespace dxmt::d3d12
