#pragma once

// GPU-completion handlers for the diagnostic retirement payloads declared in
// d3d12_replay_diagnostics.hpp.
//
// These used to be `static` members of CommandQueueImpl (the `Retire()`
// overload set in d3d12_command_queue_queue_work.inc). They never touched the
// queue instance, and every type they mention now lives at namespace scope, so
// they hoist unchanged. The queue keeps thin `Retire()` wrappers so that the
// std::visit-based RetirementVisitor keeps resolving the whole overload set,
// including the payloads whose types are still nested in CommandQueueImpl.

#include "d3d12_replay_diagnostics.hpp"
#include "dxmt_command_queue.hpp"

namespace dxmt::d3d12 {

// The CBV readback dump additionally spells out two float4 elements that the
// traced composite shader reaches by dynamic indexing, past the small bound
// view. Constant-buffer elements are 16 bytes wide.
inline constexpr size_t kCbvReadbackElementSize = 16;
inline constexpr size_t kCbvReadbackProbeElement18ByteOffset =
    18 * kCbvReadbackElementSize;
inline constexpr size_t kCbvReadbackProbeElement68ByteOffset =
    68 * kCbvReadbackElementSize;

// Logs the resolved occlusion-query sample count of a traced draw. Reports a
// zero count when the submission did not complete or the query is missing.
void RetireDrawVisibilityWork(DrawVisibilityRetirementWork &work,
                              GpuCompletionStatus status) noexcept;

// Dumps the index data a traced draw actually fetched. Skipped unless the
// submission completed.
void RetireIndexReadbackWork(IndexReadbackRetirementWork &work,
                             GpuCompletionStatus status) noexcept;

// Dumps the vertex data a traced draw actually fetched. Skipped unless the
// submission completed.
void RetireVertexReadbackWork(VertexReadbackRetirementWork &work,
                              GpuCompletionStatus status) noexcept;

// Dumps and/or apitrace-snapshots the constant-buffer bytes a traced draw
// actually read. Skipped unless the submission completed.
void RetireConstantBufferReadbackWork(
    ConstantBufferReadbackRetirementWork &work,
    GpuCompletionStatus status) noexcept;

} // namespace dxmt::d3d12
