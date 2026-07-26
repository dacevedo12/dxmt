#include "d3d12_bindless_mirror_fill.hpp"

#include <atomic>
#include <cstdint>

namespace dxmt::d3d12 {

namespace {

// Cap on mirror verify mismatch reports so one broken slot cannot flood the log.
constexpr uint32_t kBindlessMirrorVerifyLogLimit = 50;

} // namespace

bool
BindlessMirrorVerifyShouldLog() {
  static std::atomic<uint32_t> count = 0;
  return BindlessMirrorVerifyEnabled() &&
         count.fetch_add(1, std::memory_order_relaxed) <
             kBindlessMirrorVerifyLogLimit;
}

} // namespace dxmt::d3d12
