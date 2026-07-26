#include "d3d12_replay_mismatch_barrier_policy.hpp"

namespace dxmt::d3d12 {

bool
ShouldEmitReadAfterWriteMismatchBarrier(D3D12_RESOURCE_STATES current,
                                        D3D12_RESOURCE_STATES desired) {
  return IsReadOnlyResourceState(desired) && StateHasWriteAccess(current);
}

bool
ShouldEmitWriteAfterReadMismatchBarrier(D3D12_RESOURCE_STATES current,
                                        D3D12_RESOURCE_STATES desired) {
  return IsReadOnlyResourceState(current) && StateHasWriteAccess(desired);
}

bool
ShouldEmitWriteAfterWriteMismatchBarrier(D3D12_RESOURCE_STATES current,
                                         D3D12_RESOURCE_STATES desired) {
  return current != desired && StateHasWriteAccess(current) &&
         StateHasWriteAccess(desired);
}

} // namespace dxmt::d3d12
