#pragma once

// Policy deciding when a resource access whose tracked D3D12 state does not
// match the desired state still needs an inferred Metal visibility barrier.
//
// These predicates used to be private members of the anonymous-namespace class
// CommandQueueImpl (d3d12_command_queue_replay_records.inc). They are pure
// functions of two state bitmasks, so hoisting them to dxmt::d3d12 lets them be
// compiled and analyzed independently.

#include "d3d12_resource_state_semantics.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// Read-after-write: a read-only access follows a state that can write.
[[nodiscard]] bool
ShouldEmitReadAfterWriteMismatchBarrier(D3D12_RESOURCE_STATES current,
                                        D3D12_RESOURCE_STATES desired);

// Write-after-read: a writable access follows a read-only state.
[[nodiscard]] bool
ShouldEmitWriteAfterReadMismatchBarrier(D3D12_RESOURCE_STATES current,
                                        D3D12_RESOURCE_STATES desired);

// Write-after-write: two different states that can both write.
[[nodiscard]] bool
ShouldEmitWriteAfterWriteMismatchBarrier(D3D12_RESOURCE_STATES current,
                                         D3D12_RESOURCE_STATES desired);

} // namespace dxmt::d3d12
