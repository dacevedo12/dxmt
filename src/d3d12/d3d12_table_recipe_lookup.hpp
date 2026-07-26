#pragma once

// Process-wide memoized lookup of the descriptor-table binding recipe.
//
// This used to live inside CommandQueueImpl
// (d3d12_command_queue_native_binding.inc). The recipe is keyed purely by
// (pipeline shader cache key, serialized root signature, compute flag) and is
// backed by a process-wide cache plus the on-disk recipe cache, so nothing here
// touches the command queue instance and it can be compiled and analysed on its
// own.

#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_root_signature.hpp"

namespace dxmt::d3d12 {

// Returns the cached descriptor-table binding recipe for
// (pipeline, root, compute), building or loading it on first use. The returned
// reference is owned by the process-wide cache and stays valid for the lifetime
// of the process.
[[nodiscard]] const DescriptorTableBindingRecipe &
GetDescriptorTableBindingRecipe(const PipelineState &pipeline,
                                const RootSignature &root, bool compute);

} // namespace dxmt::d3d12
