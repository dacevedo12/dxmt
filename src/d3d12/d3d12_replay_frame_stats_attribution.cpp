#include "d3d12_replay_frame_stats_attribution.hpp"

namespace dxmt::d3d12 {

bool ReplayPacketClassificationEnabled(const ReplayState &state) {
  return dxmt::perf::enabled() &&
         state.compiled_fallback_reason ==
             dxmt::CompiledFallbackReason::Unknown;
}

void RecordReplayGraphicsPacketClassification(FrameStatistics *stats,
                                              ReplayGraphicsCommandKind kind,
                                              bool compiled_candidate,
                                              bool use_geometry,
                                              bool use_tessellation) {
  if (compiled_candidate && !use_geometry && !use_tessellation) {
    dxmt::perf::recordCompiledGraphicsPackets(stats, 1);
    return;
  }

  const auto reason = ReplayGraphicsCommandKindIsIndirect(kind)
                          ? dxmt::CompiledFallbackReason::Indirect
                      : (use_geometry || use_tessellation)
                          ? dxmt::CompiledFallbackReason::GeometryOrTessellation
                          : dxmt::CompiledFallbackReason::LegacyPath;
  dxmt::perf::recordFallbackGraphicsPackets(stats, 1, reason);
}

void RecordReplayComputePacketClassification(FrameStatistics *stats,
                                             bool compiled_candidate) {
  if (compiled_candidate)
    dxmt::perf::recordCompiledComputePackets(stats, 1);
  else
    dxmt::perf::recordFallbackComputePackets(
        stats, 1, dxmt::CompiledFallbackReason::LegacyPath);
}

void RecordReplayGraphicsPassPlanStats(FrameStatistics *stats,
                                       const ReplayGraphicsPassPlan &plan,
                                       dxmt::clock::duration build_time) {
  dxmt::perf::recordCompiledPassBuildTime(stats, build_time);
  stats->frame_replay_compiled_graphics_candidates +=
      plan.compiled_candidate_count;
  stats->frame_replay_compiled_graphics_legacy += plan.compiled_legacy_count;
  stats->frame_replay_compiled_graphics_barriers += plan.compiled_barrier_count;
  stats->frame_replay_compiled_graphics_gs_ts += plan.compiled_gs_ts_count;
  stats->frame_replay_compiled_graphics_indirect +=
      plan.compiled_indirect_count;
}

} // namespace dxmt::d3d12
