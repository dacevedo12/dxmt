#pragma once

// Encode of a compiled-direct draw/dispatch binding payload: the native binding
// recipe, the vertex binding recipe and the per-stage bindless snapshot wiring,
// each gated on the dirty fields the encoder's binding delta reports.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_compiled_encode.inc). A compiled payload is
// self-contained by construction, so the only queue-owned things left are the
// Metal device and the argument-buffer ring owner.

#include "Metal.hpp"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

/** Rebinds a compiled-direct graphics draw. A null `binding_delta` means "every
 *  field is dirty"; otherwise only the recipes whose fields the delta marks are
 *  replayed. `draw_diag`, when null, is replaced by a throwaway local — the
 *  compiled-direct path never folds it into the process counters itself. */
void EncodeCompiledGraphicsBindings(
    WMT::Device device, ::dxmt::CommandQueue &queue,
    ArgumentEncodingContext &enc,
    const CompiledDirectGraphicsBindingPayload &payload,
    PipelineState &pipeline, uint64_t &argbuf_offset,
    const CompiledBindingDelta *binding_delta = nullptr,
    BindlessMirrorDrawDiag *draw_diag = nullptr,
    CompiledCommandTestTelemetry *test_telemetry = nullptr);

/** Compute counterpart of EncodeCompiledGraphicsBindings. */
void EncodeCompiledComputeBindings(
    WMT::Device device, ::dxmt::CommandQueue &queue,
    ArgumentEncodingContext &enc,
    const CompiledDirectComputeBindingPayload &payload,
    PipelineState &pipeline,
    const CompiledBindingDelta *binding_delta = nullptr,
    CompiledCommandTestTelemetry *test_telemetry = nullptr);

} // namespace dxmt::d3d12
