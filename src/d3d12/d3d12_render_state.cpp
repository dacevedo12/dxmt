#include "d3d12_render_state.hpp"

#include "d3d12_command_list.hpp"
#include "log/log.hpp"

#include <algorithm>
#include <utility>

namespace dxmt::d3d12 {
namespace {

D3D12_RECT ScissorRectFromViewport(const D3D12_VIEWPORT &viewport) {
  D3D12_RECT rect = {};
  rect.left = static_cast<LONG>(std::max(0.0f, viewport.TopLeftX));
  rect.top = static_cast<LONG>(std::max(0.0f, viewport.TopLeftY));
  rect.right =
      static_cast<LONG>(std::max(0.0f, viewport.TopLeftX + viewport.Width));
  rect.bottom =
      static_cast<LONG>(std::max(0.0f, viewport.TopLeftY + viewport.Height));
  return rect;
}

void WarnMissingViewport(const char *context) {
  WARN("D3D12CommandQueue: ", context,
       " skipped because no viewport was set");
}

void WarnDefaultScissorRects(const char *context) {
  WARN("D3D12CommandQueue: ", context,
       " used viewport-sized default scissor rects");
}

} // namespace

bool
ResolveDynamicRasterRects(std::vector<D3D12_VIEWPORT> &viewports,
                          std::vector<D3D12_RECT> &scissors,
                          const char *context) {
  if (viewports.empty()) {
    WarnMissingViewport(context);
    return false;
  }

  if (!scissors.empty())
    return true;

  scissors.reserve(viewports.size());
  for (const auto &viewport : viewports)
    scissors.push_back(ScissorRectFromViewport(viewport));
  WarnDefaultScissorRects(context);
  return true;
}

bool
ResolveDynamicRasterRects(const CompiledImmutableVector<D3D12_VIEWPORT> &viewports,
                          CompiledImmutableVector<D3D12_RECT> &scissors,
                          const char *context) {
  if (viewports.empty()) {
    WarnMissingViewport(context);
    return false;
  }

  if (!scissors.empty())
    return true;

  std::vector<D3D12_RECT> resolved_scissors;
  resolved_scissors.reserve(viewports.size());
  for (const auto &viewport : viewports)
    resolved_scissors.push_back(ScissorRectFromViewport(viewport));
  scissors = std::move(resolved_scissors);
  WarnDefaultScissorRects(context);
  return true;
}

uint32_t
InputSlotMask(const PipelineGraphicsState *graphics_state) {
  if (!graphics_state)
    return 0;

  uint32_t slot_mask = 0;
  for (const auto &element : graphics_state->input_elements) {
    if (element.InputSlot < kInputSlotMaskBitCount)
      slot_mask |= 1u << element.InputSlot;
  }
  return slot_mask;
}

} // namespace dxmt::d3d12
