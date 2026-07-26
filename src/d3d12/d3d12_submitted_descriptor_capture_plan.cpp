#include "d3d12_submitted_descriptor_capture_plan.hpp"

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_table_recipe_lookup.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

std::vector<DescriptorHeapMirror *>
CollectSubmissionDescriptorHeapMirrors(const CompiledCommandList &compiled) {
  std::vector<DescriptorHeapMirror *> submission_mirrors;
  auto collect_mirrors = [&](const auto &packet) {
    auto collect_heap = [&](ID3D12DescriptorHeap *heap_object) {
      auto *heap = dynamic_cast<DescriptorHeap *>(heap_object);
      auto *mirror = heap ? heap->GetMirror() : nullptr;
      if (mirror)
        submission_mirrors.push_back(mirror);
    };
    collect_heap(packet.descriptor_heaps.cbv_srv_uav.ptr());
    collect_heap(packet.descriptor_heaps.sampler.ptr());
  };
  for (const auto &packet : compiled.graphics_packets)
    if (packet.compatibility_reason == CompiledCommandFallbackReason::None)
      collect_mirrors(packet);
  for (const auto &packet : compiled.compute_packets)
    if (packet.compatibility_reason == CompiledCommandFallbackReason::None)
      collect_mirrors(packet);
  std::sort(submission_mirrors.begin(), submission_mirrors.end());
  submission_mirrors.erase(
      std::unique(submission_mirrors.begin(), submission_mirrors.end()),
      submission_mirrors.end());
  return submission_mirrors;
}

void SubmissionDescriptorRecipeCache::Reserve(size_t packet_count) {
  entries_.reserve(std::min<size_t>(packet_count, 128));
}

const DescriptorTableBindingRecipe &SubmissionDescriptorRecipeCache::Get(
    PipelineState &pipeline, RootSignature &root, bool compute,
    const CompiledCommandPipelineMetadata &metadata) {
  const Key key = {metadata.binding_layout_fingerprint,
                   metadata.shader_abi_version, &root, compute};
  auto it = entries_.find(key);
  if (it == entries_.end())
    it = entries_
             .emplace(key,
                      &GetDescriptorTableBindingRecipe(pipeline, root, compute))
             .first;
  return *it->second;
}

size_t ComputeSubmittedDescriptorCaptureCapacity(
    const CompiledCommandList &compiled,
    SubmissionDescriptorRecipeCache &cache) {
  size_t descriptor_capture_capacity = 0;
  for (const auto &packet : compiled.graphics_packets) {
    if (packet.compatibility_reason != CompiledCommandFallbackReason::None)
      continue;
    auto *pipeline = packet.pipeline.metadata.pipeline;
    if (!pipeline)
      pipeline = GetPipelineState(packet.pipeline.pipeline_state.ptr());
    auto *root = GetRootSignature(packet.pipeline.root_signature.ptr());
    if (pipeline && root &&
        (pipeline->UsesBindlessMirror() ||
         packet.pipeline.metadata.uses_native_descriptor_table_abi))
      descriptor_capture_capacity +=
          cache.Get(*pipeline, *root, false, packet.pipeline.metadata)
              .entries.size();
  }
  for (const auto &packet : compiled.compute_packets) {
    if (packet.compatibility_reason != CompiledCommandFallbackReason::None)
      continue;
    auto *pipeline = packet.pipeline.metadata.pipeline;
    if (!pipeline)
      pipeline = GetPipelineState(packet.pipeline.pipeline_state.ptr());
    auto *root = GetRootSignature(packet.pipeline.root_signature.ptr());
    if (pipeline && root &&
        (pipeline->UsesBindlessMirror() ||
         packet.pipeline.metadata.uses_native_descriptor_table_abi))
      descriptor_capture_capacity +=
          cache.Get(*pipeline, *root, true, packet.pipeline.metadata)
              .entries.size();
  }
  return descriptor_capture_capacity;
}

} // namespace dxmt::d3d12
