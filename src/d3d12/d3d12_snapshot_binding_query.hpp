#pragma once

// Read-only queries over an already captured GraphicsBindingSnapshot.
//
// These helpers used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). They never consult live replay
// state or the command queue instance, so they can be compiled and analysed on
// their own.

#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"

#include <cstdint>
#include <utility>

namespace dxmt::d3d12 {

// The frozen bindless tables belonging to `stage`. Stages other than
// Pixel/Compute share the vertex tables, matching the freeze side.
[[nodiscard]] const FrozenBindlessStageTables &
FrozenBindlessTablesForStage(const GraphicsBindingSnapshot &snapshot,
                             PipelineStage stage);

[[nodiscard]] FrozenBindlessStageTables &
MutableFrozenBindlessTablesForStage(GraphicsBindingSnapshot &snapshot,
                                    PipelineStage stage);

// Pixel-shader MSAA-SRV demote mask pair derived purely from the submitted
// snapshot: every multisample-declared SRV argument whose captured descriptor
// resolves to a single-sample texture gets its binding slot marked.
[[nodiscard]] std::pair<uint64_t, uint64_t>
PixelShaderSingleSampleMsaaSRVDemoteMask(
    const GraphicsBindingSnapshot &snapshot, const PipelineState &pipeline);

} // namespace dxmt::d3d12
