#include "d3d12_resource_state_semantics.hpp"

#include "dxmt_context.hpp"
#include "log/log.hpp"

namespace dxmt::d3d12 {

bool
StateHasWriteAccess(D3D12_RESOURCE_STATES state) {
  constexpr uint32_t kWriteStates =
      uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
      uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
      uint32_t(D3D12_RESOURCE_STATE_STREAM_OUT) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_DEST) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_DEST);
  return (uint32_t(state) & kWriteStates) != 0;
}

bool
StateHasReadAccess(D3D12_RESOURCE_STATES state) {
  constexpr uint32_t kReadStates =
      uint32_t(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
      uint32_t(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
      uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_SOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_READ) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_SOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_PREDICATION);
  return (uint32_t(state) & kReadStates) != 0;
}

int
ResourceAccessForState(D3D12_RESOURCE_STATES state) {
  int access = 0;
  if (StateHasReadAccess(state))
    access |= ResourceAccess::Read;
  if (StateHasWriteAccess(state))
    access |= ResourceAccess::Write;
  if (uint32_t(state) & uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
    access |= ResourceAccess::UAV;
  return access;
}

bool
IsReadOnlyResourceState(D3D12_RESOURCE_STATES state) {
  return StateHasReadAccess(state) && !StateHasWriteAccess(state);
}

bool
IsSingleWriteResourceState(D3D12_RESOURCE_STATES state) {
  const auto bits = uint32_t(state);
  return StateHasWriteAccess(state) && !(bits & (bits - 1));
}

bool
IsAlwaysDecayEligibleResource(const Resource &resource) {
  const auto &desc = resource.GetResourceDesc();
  return desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
         (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS);
}

bool
IsImplicitPromotionCompatibleState(const Resource &resource,
                                   D3D12_RESOURCE_STATES state) {
  if (state == D3D12_RESOURCE_STATE_COMMON)
    return true;

  if (IsReadOnlyResourceState(state))
    return true;

  if (state == D3D12_RESOURCE_STATE_COPY_SOURCE ||
      state == D3D12_RESOURCE_STATE_COPY_DEST)
    return true;

  if (IsSingleWriteResourceState(state) &&
      IsAlwaysDecayEligibleResource(resource))
    return true;

  return false;
}

bool
IsDecayEligibleResourceState(const Resource &resource,
                             D3D12_COMMAND_LIST_TYPE queue_type,
                             D3D12_RESOURCE_STATES state,
                             bool implicitly_promoted) {
  if (state == D3D12_RESOURCE_STATE_COMMON)
    return false;

  if (queue_type == D3D12_COMMAND_LIST_TYPE_COPY)
    return true;

  if (IsAlwaysDecayEligibleResource(resource))
    return true;

  return implicitly_promoted && IsReadOnlyResourceState(state);
}

bool
IsImplicitPromotionCompatibleResource(const Resource &resource,
                                      D3D12_RESOURCE_STATES current,
                                      D3D12_RESOURCE_STATES before) {
  if (current != D3D12_RESOURCE_STATE_COMMON)
    return false;

  return IsImplicitPromotionCompatibleState(resource, before);
}

bool
IsTransitionBeforeStateCompatible(D3D12_COMMAND_LIST_TYPE queue_type,
                                  D3D12_RESOURCE_STATES current,
                                  D3D12_RESOURCE_STATES before) {
  if (current == before)
    return true;

  if (IsReadOnlyResourceState(current) && IsReadOnlyResourceState(before)) {
    const auto current_bits = uint32_t(current);
    const auto before_bits = uint32_t(before);
    // Accept either subset direction:
    // - before ⊆ current: app names a subset of tracked combined read state
    // - current ⊆ before: tracker lagged extra read bits the app used (e.g.
    //   expected NON_PIXEL=64 while StateBefore is PIXEL|NON_PIXEL=192).
    // Observed on Wukong hang frames as "state mismatch expected=64 before=192
    // after=8" immediately before DEVICE_REMOVED.
    return ((current_bits & before_bits) == before_bits) ||
           ((before_bits & current_bits) == current_bits);
  }

  if (queue_type == D3D12_COMMAND_LIST_TYPE_COPY &&
      before == D3D12_RESOURCE_STATE_COMMON &&
      IsReadOnlyResourceState(current)) {
    constexpr uint32_t kShaderResourceStates =
        uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
        uint32_t(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return (uint32_t(current) & kShaderResourceStates) != 0;
  }

  return false;
}

bool
IsKnownResourceState(D3D12_RESOURCE_STATES state) {
  constexpr uint32_t kKnownStates =
      uint32_t(D3D12_RESOURCE_STATE_COMMON) |
      uint32_t(D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER) |
      uint32_t(D3D12_RESOURCE_STATE_INDEX_BUFFER) |
      uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET) |
      uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_READ) |
      uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_STREAM_OUT) |
      uint32_t(D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_DEST) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_SOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_DEST) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_SOURCE) |
      uint32_t(D3D12_RESOURCE_STATE_PREDICATION);
  return (uint32_t(state) & ~kKnownStates) == 0;
}

void
WarnUnsupportedResourceState(D3D12_RESOURCE_STATES state, const char *context) {
  constexpr uint32_t kWriteStates =
      uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) |
      uint32_t(D3D12_RESOURCE_STATE_RENDER_TARGET) |
      uint32_t(D3D12_RESOURCE_STATE_DEPTH_WRITE) |
      uint32_t(D3D12_RESOURCE_STATE_STREAM_OUT) |
      uint32_t(D3D12_RESOURCE_STATE_COPY_DEST) |
      uint32_t(D3D12_RESOURCE_STATE_RESOLVE_DEST);
  if (!IsKnownResourceState(state)) {
    WARN("D3D12CommandQueue: unsupported resource state bits in ", context,
         " state=", uint32_t(state));
  }
  const auto writes = uint32_t(state) & kWriteStates;
  if (writes && (StateHasReadAccess(state) || (writes & (writes - 1)))) {
    WARN("D3D12CommandQueue: conservative handling for combined write state in ",
         context, " state=", uint32_t(state));
  }
}

} // namespace dxmt::d3d12
