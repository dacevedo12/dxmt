#pragma once

// Root/table binding debug logging and the process-wide shader-digest memo it
// uses to decide whether a pipeline stage is selected by the DXMT_DIAG_SHADER
// filter.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_descriptor_binding.inc). They read only the pipeline,
// the descriptor record and the argument handed to them, so they never touch
// the command queue instance and can be compiled and analysed on their own.

#include "airconv_dx12_metal4.h"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "dxmt_context.hpp"

#include <string>

#include <d3d12.h>

namespace dxmt::d3d12 {

// SHA-1 of `stage`'s bytecode, memoized per process against the cached-shader
// object that owns the bytecode. Empty when the stage has no shader.
[[nodiscard]] std::string ShaderDigestForStage(const PipelineState &pipeline,
                                               PipelineStage stage);

// True when either the pipeline cache key or `stage`'s shader digest is
// selected by the diagnostic shader filter. `shader_digest`, when non-null,
// receives the digest regardless of the result.
[[nodiscard]] bool
D3D12DiagPipelineStageSelected(const PipelineState &pipeline,
                               PipelineStage stage,
                               std::string *shader_digest = nullptr);

// Sampled log line for one root or descriptor-table binding, plus the
// periodic/anomaly-triggered selected-descriptor consistency report. No-op
// unless the binding diagnostics are enabled.
void DebugLogRootBinding(const char *kind, const PipelineState &pipeline,
                         bool compute, PipelineStage stage, UINT root_index,
                         UINT slot, UINT shader_register, UINT register_space,
                         UINT64 size, D3D12_GPU_VIRTUAL_ADDRESS address,
                         const DescriptorRecord *descriptor = nullptr,
                         const DXMT12_MTL4_SHADER_ARGUMENT *argument = nullptr);

} // namespace dxmt::d3d12
