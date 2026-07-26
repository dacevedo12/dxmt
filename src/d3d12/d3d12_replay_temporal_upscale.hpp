#pragma once

// TemporalUpscale() replay: validation, scaler cache lookup / creation and the
// encoded MetalFX temporal scaler dispatch.
//
// This used to be a private member of CommandQueueImpl
// (d3d12_command_queue_replay_records.inc). The two pieces of queue identity
// it reached for through `this` — device_->GetMTLDevice() and the queue-owned
// temporal_scaler_cache_ — are now explicit parameters.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_temporal_scaler.hpp"
#include "dxmt_command_queue.hpp"

#include <vector>

namespace dxmt::d3d12 {

void ReplayTemporalUpscale(WMT::Device device,
                           std::vector<CachedTemporalScaler> &scaler_cache,
                           CommandChunk *chunk,
                           const TemporalUpscaleRecord &record);

} // namespace dxmt::d3d12
