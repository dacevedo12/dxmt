#pragma once

// Dense correctness check of one stage's frozen native descriptor tables:
// does every descriptor the shader reflects as a texture/buffer actually hold
// a backend slot of that kind and shape?
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_native_binding.inc). It reads only the compiled tables,
// the pipeline reflection and the memoized stage plan handed to it, so it never
// touches the command queue instance and can be compiled and analysed on its
// own.

#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_pipeline.hpp"
#include "dxmt_context.hpp"

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Scans up to 4096 descriptors of `plan` for `stage` and reports argument/slot
// kind, texture array-ness and native buffer descriptor mismatches. No-op
// unless the dense correctness diagnostic is enabled, or when the stage has no
// reflected resource arguments. `kind`, `frame`, `sequence`, `record_serial`
// and `metal_pso` only label the log lines.
void DiagnoseCompiledNativeStageDescriptors(
    const std::vector<CompiledCommandRootDescriptorTable> &tables,
    const PipelineState &pipeline, const NativeRootBaseStagePlan *plan,
    PipelineStage stage, const char *kind, uint64_t frame, uint64_t sequence,
    uint64_t record_serial, obj_handle_t metal_pso);

} // namespace dxmt::d3d12
