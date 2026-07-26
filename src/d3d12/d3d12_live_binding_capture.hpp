#pragma once

// Capture of live replay state into a GraphicsBindingSnapshot: root
// descriptors, root 32-bit constants and vertex buffers.
//
// These helpers used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). Each of them only reads the
// ReplayState and pipeline handed to it and appends to the snapshot, so none of
// them touch the command queue instance and the whole set can be compiled and
// analysed on its own.

#include "d3d12_descriptor_heap.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "d3d12_root_signature.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Appends the snapshot entries for the root descriptor bound at `root_index`.
// No-op when the root index is out of range or nothing is bound there.
void CaptureGraphicsRootDescriptor(GraphicsBindingSnapshot &snapshot,
                                   const ReplayState &state,
                                   const PipelineState &pipeline,
                                   UINT root_index,
                                   const RootSignatureParameter &parameter,
                                   DescriptorRecordType type,
                                   bool compute = false);

// Appends the snapshot entries for the root 32-bit constants at `root_index`.
// An unset slot still records the declared constant count with no values.
void CaptureGraphicsRootConstants(GraphicsBindingSnapshot &snapshot,
                                  const ReplayState &state,
                                  const PipelineState &pipeline,
                                  UINT root_index,
                                  const RootSignatureParameter &parameter,
                                  bool compute = false);

// Records the vertex buffers bound to the input slots the graphics pipeline
// actually declares, together with the slot mask, into the snapshot.
void CaptureGraphicsVertexBuffers(
    GraphicsBindingSnapshot &snapshot, const ReplayState &state,
    const PipelineGraphicsState *graphics_state);

} // namespace dxmt::d3d12
