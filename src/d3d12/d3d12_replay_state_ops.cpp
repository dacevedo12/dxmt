#include "d3d12_replay_state_ops.hpp"

#include "dxmt_perf_stats.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <atomic>

namespace dxmt::d3d12 {

void
EmitPassSeparator(CommandChunk *) {
  auto *stats = dxmt::perf::currentFrameStatistics();
  if (stats)
    stats->blit_separator_pass_count++;
}

void
ApplyRootConstants(std::vector<UINT> &values, UINT dst_offset,
                   const std::vector<UINT> &new_values) {
  // No root signature is reachable from here, but D3D12 caps a whole root
  // signature at D3D12_MAX_ROOT_COST DWORDs -- CreateRootSignatureFromBlob
  // refuses anything above it -- and every 32-bit root constant costs one
  // DWORD, so no legal (dst_offset, count) pair can reach past that cap.
  // Evaluating the window in 64-bit and refusing the rest keeps the std::copy
  // below inside `values`: the previous 32-bit add wrapped for a hostile
  // dst_offset, producing a `required_size` far smaller than `dst_offset`.
  // ExecuteIndirect reaches this with DestOffsetIn32BitValues taken straight
  // from the application's command signature, so the offset is untrusted.
  const uint64_t required_size = uint64_t(dst_offset) + new_values.size();
  if (required_size > D3D12_MAX_ROOT_COST) {
    static std::atomic<uint32_t> log_count = 0;
    if (log_count.fetch_add(1, std::memory_order_relaxed) < 8) {
      WARN("D3D12CommandQueue: root constants destination range is outside the"
           " root signature and was dropped"
           " num32BitValuesToSet=", new_values.size(),
           " destOffsetIn32BitValues=", dst_offset);
    }
    return;
  }
  if (values.size() < required_size)
    values.resize(size_t(required_size), 0);
  std::copy(new_values.begin(), new_values.end(),
            values.begin() + dst_offset);
}

bool
PredicationValuePasses(D3D12_PREDICATION_OP operation, uint64_t value) {
  switch (operation) {
  case D3D12_PREDICATION_OP_EQUAL_ZERO:
    return value != 0;
  case D3D12_PREDICATION_OP_NOT_EQUAL_ZERO:
    return value == 0;
  default:
    WARN("D3D12CommandQueue: unsupported predication operation ",
         uint32_t(operation),
         "; command will be executed");
    return true;
  }
}

} // namespace dxmt::d3d12
