#pragma once

// Pixel-shader MSAA-SRV demote mask derived from live replay state.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). It resolves descriptors purely
// from the bound heaps named by the ReplayState handed to it, so it never
// touches the command queue instance and can be compiled and analysed on its
// own. The snapshot-only counterpart lives in d3d12_snapshot_binding_query.hpp.

#include "d3d12_pipeline.hpp"
#include "d3d12_replay_queue_state_types.hpp"

#include <cstdint>
#include <utility>

namespace dxmt::d3d12 {

// Demote mask pair for the currently bound pixel shader: every
// multisample-declared SRV argument whose bound descriptor resolves to a
// single-sample texture gets its binding slot marked.
[[nodiscard]] std::pair<uint64_t, uint64_t>
PixelShaderSingleSampleMsaaSRVDemoteMask(const ReplayState &state,
                                         const PipelineState &pipeline);

} // namespace dxmt::d3d12
