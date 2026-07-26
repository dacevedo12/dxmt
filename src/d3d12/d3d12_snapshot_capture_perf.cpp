#include "d3d12_snapshot_capture_perf.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_replay_perf_timers.hpp"

namespace dxmt::d3d12 {

void RecordGraphicsBindingSnapshotRequestPerf(
    ReplayState &state,
    dxmt::DescriptorContentRevision descriptor_content_revision) {
  if (!ReplayPerfEnabled())
    return;
  auto &timers = perDrawSubTimers();
  timers.snapshotRequests++;
  if (state.has_last_snapshot_request_generation) {
    const bool graphics_changed =
        state.last_snapshot_request_graphics_generation !=
        state.graphics_binding_generation;
    const bool descriptor_changed =
        state.last_snapshot_request_descriptor_revision !=
        descriptor_content_revision;
    if (graphics_changed && descriptor_changed)
      timers.snapshotBothGenChanges++;
    else if (graphics_changed)
      timers.snapshotGraphicsGenChanges++;
    else if (descriptor_changed)
      timers.snapshotDescriptorGenChanges++;
    else
      timers.snapshotNoGenChanges++;
  }
  state.last_snapshot_request_graphics_generation =
      state.graphics_binding_generation;
  state.last_snapshot_request_descriptor_revision =
      descriptor_content_revision;
  state.has_last_snapshot_request_generation = true;
}

void RecordGraphicsBindingSnapshotCapturePerf(
    const GraphicsBindingSnapshotCaptureStats &capture_stats) {
  if (!ReplayPerfEnabled())
    return;
  auto &timers = perDrawSubTimers();
  timers.snapshotCapturedEntries += capture_stats.entries;
  timers.snapshotCapturedDescriptors += capture_stats.descriptors;
  timers.snapshotCapturedMissingDescriptors +=
      capture_stats.missing_descriptors;
  timers.snapshotCapturedRootDescriptors += capture_stats.root_descriptors;
  timers.snapshotCapturedRootConstants += capture_stats.root_constants;
  timers.snapshotCapturedVertexBuffers += capture_stats.vertex_buffers;
  timers.snapshotCapturedBindless += capture_stats.bindless;
}

} // namespace dxmt::d3d12
