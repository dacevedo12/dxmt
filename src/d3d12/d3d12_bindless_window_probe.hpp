#pragma once

// Bindless-window payload construction and the per-slot diagnostic probes that
// compare a descriptor's expected payload against what the per-draw window
// actually carries.
//
// These used to live inside CommandQueueImpl
// (d3d12_command_queue_bindless_mirror.inc and
// d3d12_command_queue_binding_plans.inc). The only queue-instance state they
// ever touched was the Metal device handle and the current frame sequence used
// in the log lines; both are parameters here, so the whole set can be compiled
// and analysed on its own.

#include "Metal.hpp"
#include "airconv_dx12_metal4.h"
#include "d3d12_binding_diagnostics.hpp"
#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_descriptor_heap.hpp"
#include "d3d12_descriptor_mirror.hpp"
#include "d3d12_pipeline.hpp"
#include "dxmt_context.hpp"

#include <cstdint>
#include <optional>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Mirror payload a texture SRV/UAV descriptor would publish, or nullopt when
// the descriptor does not resolve to a texture view.
[[nodiscard]] std::optional<DescriptorTextureSlotPayload>
BuildBindlessTextureWindowPayload(WMT::Device device,
                                  const DescriptorRecord &record,
                                  const DXMT12_MTL4_SHADER_ARGUMENT &argument);

// Reports a shader argument whose bindless root offset could not be resolved.
// Rate limited per process; bumps `draw_diag->root_offset_missing`.
void DiagnoseBindlessRootOffsetGap(uint64_t frame_seq, const char *path,
                                   const PipelineState *pipeline,
                                   PipelineStage stage,
                                   const DXMT12_MTL4_SHADER_ARGUMENT &arg,
                                   uint32_t issue_flags,
                                   BindlessMirrorDrawDiag *draw_diag);

// Compares the texture payload a descriptor should publish against the window
// contents. Returns true when the comparison found a problem.
[[nodiscard]] bool
ProbeBindlessMirrorTextureBinding(WMT::Device device, uint64_t frame_seq,
                                  const BindlessMirrorDiagProbe &probe,
                                  BindlessMirrorDrawDiag *draw_diag);

// Sampler counterpart of ProbeBindlessMirrorTextureBinding.
[[nodiscard]] bool
ProbeBindlessMirrorSamplerBinding(uint64_t frame_seq,
                                  const BindlessMirrorDiagProbe &probe,
                                  BindlessMirrorDrawDiag *draw_diag);

} // namespace dxmt::d3d12
