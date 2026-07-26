#pragma once

// Replay-perf accounting for graphics binding snapshot requests and captures.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc). They only fold their arguments
// into the per-thread replay diagnostic ledger, which is itself a namespace
// level accessor now, so they never touch the command queue instance and can be
// compiled and analysed on their own.

#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"
#include "dxmt_descriptor_revision.hpp"

namespace dxmt::d3d12 {

// Counts one snapshot request and classifies it by which generation moved since
// the previous request: graphics bindings, descriptor content, both or neither.
// Also advances the state's last-request generation markers.
void RecordGraphicsBindingSnapshotRequestPerf(
    ReplayState &state,
    dxmt::DescriptorContentRevision descriptor_content_revision);

// Folds one capture's entry/descriptor/vertex-buffer tallies into the ledger.
void RecordGraphicsBindingSnapshotCapturePerf(
    const GraphicsBindingSnapshotCaptureStats &capture_stats);

} // namespace dxmt::d3d12
