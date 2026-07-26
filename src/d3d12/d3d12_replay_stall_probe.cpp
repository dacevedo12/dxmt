#include "d3d12_replay_stall_probe.hpp"

#include "d3d12_queue_diagnostics_env.hpp"
#include "dxmt_perf_stats.hpp"

namespace dxmt::d3d12 {

StallProbe &stallProbe() {
  static thread_local StallProbe p;
  return p;
}

bool StallDiagEnabled() {
  return D3D12RecordLoopStallEnabled() || dxmt::perf::enabled();
}

} // namespace dxmt::d3d12
