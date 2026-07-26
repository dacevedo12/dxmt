#pragma once

#include "d3d12_resource.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// True when the state bitmask contains any writable usage.
[[nodiscard]] bool StateHasWriteAccess(D3D12_RESOURCE_STATES state);

// True when the state bitmask contains any readable usage.
[[nodiscard]] bool StateHasReadAccess(D3D12_RESOURCE_STATES state);

// Translates a D3D12 state bitmask into DXMT ResourceAccess flags.
[[nodiscard]] int ResourceAccessForState(D3D12_RESOURCE_STATES state);

[[nodiscard]] bool IsReadOnlyResourceState(D3D12_RESOURCE_STATES state);

// True when the state has exactly one write bit set.
[[nodiscard]] bool IsSingleWriteResourceState(D3D12_RESOURCE_STATES state);

// Buffers and simultaneous-access textures always decay back to COMMON.
[[nodiscard]] bool IsAlwaysDecayEligibleResource(const Resource &resource);

// D3D12 implicit state promotion rules for a target state.
[[nodiscard]] bool
IsImplicitPromotionCompatibleState(const Resource &resource,
                                   D3D12_RESOURCE_STATES state);

// D3D12 implicit state decay rules at the end of a command list.
[[nodiscard]] bool IsDecayEligibleResourceState(const Resource &resource,
                                                D3D12_COMMAND_LIST_TYPE queue_type,
                                                D3D12_RESOURCE_STATES state,
                                                bool implicitly_promoted);

// True when a resource tracked in `current` may be implicitly promoted to
// `before` instead of failing the transition check.
[[nodiscard]] bool
IsImplicitPromotionCompatibleResource(const Resource &resource,
                                      D3D12_RESOURCE_STATES current,
                                      D3D12_RESOURCE_STATES before);

// True when a barrier's StateBefore is compatible with the tracked state.
[[nodiscard]] bool
IsTransitionBeforeStateCompatible(D3D12_COMMAND_LIST_TYPE queue_type,
                                  D3D12_RESOURCE_STATES current,
                                  D3D12_RESOURCE_STATES before);

// True when every bit of the state is one DXMT models.
[[nodiscard]] bool IsKnownResourceState(D3D12_RESOURCE_STATES state);

// Warns once per call site about unmodelled or conservatively handled states.
void WarnUnsupportedResourceState(D3D12_RESOURCE_STATES state,
                                  const char *context);

} // namespace dxmt::d3d12
