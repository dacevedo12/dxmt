#include "d3d12_submission_batch_shape.hpp"

#include "d3d12_command_list.hpp"
#include "d3d12_replay_binding_types.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

size_t
LastCompiledSubmittedListIndex(
    const std::vector<std::shared_ptr<const SubmittedCompiledCommandListPlan>>
        &plans) {
  size_t last_compiled_list_index = SIZE_MAX;
  for (size_t index = 0; index < plans.size(); ++index) {
    const auto &candidate = plans[index];
    if (candidate && candidate->generation)
      last_compiled_list_index = index;
  }
  return last_compiled_list_index;
}

const CompiledCommandList *
SubmittedCompiledGeneration(const SubmittedCompiledCommandListPlan *submitted) {
  return submitted && submitted->generation ? submitted->generation.get()
                                            : nullptr;
}

const CompiledCommandDescriptorSnapshots *
SubmittedDescriptorSnapshotsAt(
    const std::vector<CompiledCommandDescriptorSnapshots> &snapshots,
    size_t index) {
  return index < snapshots.size() ? &snapshots[index] : nullptr;
}

CompiledCommandListShape
DescribeCompiledCommandList(const CompiledCommandList *compiled) {
  CompiledCommandListShape shape = {};
  if (!compiled)
    return shape;
  shape.record_count = compiled->record_count;
  shape.segments = compiled->segments.size();
  shape.graphics_packets = compiled->graphics_packets.size();
  shape.compute_packets = compiled->compute_packets.size();
  return shape;
}

} // namespace dxmt::d3d12
