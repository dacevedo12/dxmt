#pragma once

// Build of the compiled native-descriptor-table binding recipe.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_compiled_encode.inc). The recipe is a pure function of
// the pipeline's shader reflection, the frozen native descriptor store and the
// per-stage root-base bindings handed to it, so it never touches the command
// queue instance and can be compiled and analysed on its own.

#include "d3d12_frozen_native_descriptor.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"

#include <initializer_list>
#include <memory>
#include <utility>

namespace dxmt::d3d12 {

// Lowers the frozen native descriptor store plus the per-stage root-base
// bindings into the ordered argument-buffer bind program consumed by
// EncodeCompiledNativeBindingRecipe. Returns an empty pointer when the frozen
// store was never finalized.
[[nodiscard]] std::shared_ptr<const CompiledNativeBindingRecipe>
BuildCompiledNativeBindingRecipe(
    const PipelineState &pipeline, bool compute,
    const SubmittedFrozenNativeDescriptorStore &frozen,
    std::initializer_list<
        std::pair<PipelineStage, const CompiledNativeStageBinding *>>
        stages);

} // namespace dxmt::d3d12
