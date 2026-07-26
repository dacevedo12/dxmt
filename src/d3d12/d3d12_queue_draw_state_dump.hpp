#pragma once

// The DXMT_DIAG_DRAW_STATE dump: one sampled multi-line report of the D3D12
// pipeline description, render-pass attachments, raster state and IA bindings
// a replayed draw is about to use, plus the draw-state anomaly audit that
// precedes it.
//
// This used to be CommandQueueImpl::DebugLogDrawState in
// d3d12_command_queue_debug_dump.inc. The only queue member it ever reached
// for was the Metal device handle used to translate DXGI formats, which is now
// a parameter, so the whole report compiles on its own.

#include "Metal.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_pass_types.hpp"
#include "d3d12_replay_queue_state_types.hpp"

#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct DrawInstancedRecord;
struct DrawIndexedInstancedRecord;

// Rate-limited (or target-PSO sampled) draw-state report. Does nothing unless
// draw-state diagnostics are enabled. Exactly one of `draw` / `indexed_draw`
// is expected to be non-null; both may be null for indirect draws.
void LogReplayDrawState(WMT::Device device, const char *kind,
                        const ReplayState &state, PipelineState &pipeline,
                        const PipelineMetalGraphicsState &metal,
                        const ReplayRenderPassAttachments &attachments,
                        const std::vector<D3D12_VIEWPORT> &viewports,
                        const std::vector<D3D12_RECT> &scissors,
                        const DrawInstancedRecord *draw,
                        const DrawIndexedInstancedRecord *indexed_draw,
                        UINT64 index_resource_offset, UINT64 index_offset);

} // namespace dxmt::d3d12
