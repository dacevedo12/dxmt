#include "d3d12_replay_perf_timers.hpp"

namespace dxmt::d3d12 {

PerDrawSubTimers &perDrawSubTimers() {
  static thread_local PerDrawSubTimers t;
  return t;
}

} // namespace dxmt::d3d12
