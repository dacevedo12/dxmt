#include "d3d12_render_encoder_state.hpp"

#include "d3d12_command_list.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

namespace dxmt::d3d12 {
namespace {

template <typename Values>
bool
DynamicStateValuesEqual(const Values &lhs, const Values &rhs) {
  return lhs.size() == rhs.size() &&
         (!lhs.size() ||
          std::memcmp(lhs.data(), rhs.data(),
                      lhs.size() * sizeof(typename Values::value_type)) == 0);
}

} // namespace

void
EncodeRenderPipelineStateIfChanged(ArgumentEncodingContext &enc,
                                   WMT::RenderPipelineState metal_pso) {
  auto *render_encoder = enc.currentRenderEncoder();
  if (render_encoder->last_pso.handle == metal_pso.handle)
    return;

  auto &set_pso = enc.encodeRenderCommand<wmtcmd_render_setpso>();
  set_pso.type = WMTRenderCommandSetPSO;
  set_pso.pso = metal_pso;
  render_encoder->last_pso = metal_pso;
}

void
EncodeDynamicRenderState(ArgumentEncodingContext &enc,
                         const std::vector<D3D12_VIEWPORT> &viewports,
                         const std::vector<D3D12_RECT> &scissors,
                         const std::array<FLOAT, 4> &blend_factor,
                         UINT stencil_ref) {
  auto viewport_values_equal = [](const std::vector<WMTViewport> &lhs,
                                  const std::vector<WMTViewport> &rhs) {
    return DynamicStateValuesEqual(lhs, rhs);
  };
  auto scissor_values_equal = [](const std::vector<WMTScissorRect> &lhs,
                                 const std::vector<WMTScissorRect> &rhs) {
    return DynamicStateValuesEqual(lhs, rhs);
  };
  auto *render_encoder = enc.currentRenderEncoder();
  auto &cache = render_encoder->dynamic_state_cache;
  const auto stencil_ref_u8 = static_cast<uint8_t>(stencil_ref);
  if (!cache.blend_valid || cache.blend_factor != blend_factor ||
      cache.stencil_ref != stencil_ref_u8) {
    auto &blend = enc.encodeRenderCommand<wmtcmd_render_setblendcolor>();
    blend.type = WMTRenderCommandSetBlendFactorAndStencilRef;
    blend.red = blend_factor[0];
    blend.green = blend_factor[1];
    blend.blue = blend_factor[2];
    blend.alpha = blend_factor[3];
    blend.stencil_ref = stencil_ref_u8;
    cache.blend_factor = blend_factor;
    cache.stencil_ref = stencil_ref_u8;
    cache.blend_valid = true;
  }

  std::vector<WMTViewport> viewport_values;
  viewport_values.resize(viewports.size());
  for (size_t i = 0; i < viewports.size(); i++) {
    const auto &viewport = viewports[i];
    viewport_values[i] = {viewport.TopLeftX, viewport.TopLeftY,
                          viewport.Width,    viewport.Height,
                          viewport.MinDepth, viewport.MaxDepth};
  }
  if (!cache.viewports_valid ||
      !viewport_values_equal(cache.viewports, viewport_values)) {
    auto &viewport_cmd = enc.encodeRenderCommand<wmtcmd_render_setviewports>();
    viewport_cmd.type = WMTRenderCommandSetViewports;
    auto *viewport_data = static_cast<WMTViewport *>(
        enc.allocate_cpu_heap(sizeof(WMTViewport) * viewport_values.size(),
                              alignof(WMTViewport)));
    if (!viewport_values.empty()) {
      std::memcpy(viewport_data, viewport_values.data(),
                  sizeof(WMTViewport) * viewport_values.size());
    }
    viewport_cmd.viewports.set(viewport_data);
    viewport_cmd.viewport_count = viewport_values.size();
    cache.viewports = std::move(viewport_values);
    cache.viewports_valid = true;
  }

  // Metal requires one scissor per viewport. D3D12 ignores extra scissors
  // and disables viewport slots without a corresponding scissor.
  std::vector<WMTScissorRect> scissor_values(viewports.size());
  const uint64_t target_width = render_encoder->render_target_width;
  const uint64_t target_height = render_encoder->render_target_height;
  const size_t active_scissor_count =
      std::min(scissors.size(), viewports.size());
  for (size_t i = 0; i < active_scissor_count; i++) {
    const auto &rect = scissors[i];
    const int64_t left = std::max<int64_t>(0, rect.left);
    const int64_t top = std::max<int64_t>(0, rect.top);
    const int64_t right = std::max<int64_t>(left, rect.right);
    const int64_t bottom = std::max<int64_t>(top, rect.bottom);
    const uint64_t x = std::min<uint64_t>(left, target_width);
    const uint64_t y = std::min<uint64_t>(top, target_height);
    const uint64_t clamped_right = std::min<uint64_t>(right, target_width);
    const uint64_t clamped_bottom = std::min<uint64_t>(bottom, target_height);
    scissor_values[i] = {x, y, clamped_right - x, clamped_bottom - y};
  }
  if (!cache.scissors_valid ||
      !scissor_values_equal(cache.scissors, scissor_values)) {
    auto &scissor_cmd =
        enc.encodeRenderCommand<wmtcmd_render_setscissorrects>();
    scissor_cmd.type = WMTRenderCommandSetScissorRects;
    auto *scissor_data = static_cast<WMTScissorRect *>(
        enc.allocate_cpu_heap(sizeof(WMTScissorRect) * scissor_values.size(),
                              alignof(WMTScissorRect)));
    if (!scissor_values.empty()) {
      std::memcpy(scissor_data, scissor_values.data(),
                  sizeof(WMTScissorRect) * scissor_values.size());
    }
    scissor_cmd.scissor_rects.set(scissor_data);
    scissor_cmd.rect_count = scissor_values.size();
    cache.scissors = std::move(scissor_values);
    cache.scissors_valid = true;
  }
}

void
EncodeDynamicRenderState(
    ArgumentEncodingContext &enc,
    const CompiledImmutableVector<D3D12_VIEWPORT> &viewports,
    const CompiledImmutableVector<D3D12_RECT> &scissors,
    const std::array<FLOAT, 4> &blend_factor, UINT stencil_ref) {
  EncodeDynamicRenderState(enc, viewports.view(), scissors.view(),
                           blend_factor, stencil_ref);
}

void
EncodeCompiledDynamicRenderState(
    ArgumentEncodingContext &enc,
    const CompiledDynamicRenderStateRecipe &recipe) {
  auto values_equal = [](const auto &lhs, const auto &rhs) {
    using Value = typename std::decay_t<decltype(lhs)>::value_type;
    return lhs.size() == rhs.size() &&
           (!lhs.size() ||
            std::memcmp(lhs.data(), rhs.data(),
                        lhs.size() * sizeof(Value)) == 0);
  };
  auto *render_encoder = enc.currentRenderEncoder();
  auto &cache = render_encoder->dynamic_state_cache;
  if (!cache.blend_valid || cache.blend_factor != recipe.blend_factor ||
      cache.stencil_ref != recipe.stencil_ref) {
    auto &blend = enc.encodeRenderCommand<wmtcmd_render_setblendcolor>();
    blend.type = WMTRenderCommandSetBlendFactorAndStencilRef;
    blend.red = recipe.blend_factor[0];
    blend.green = recipe.blend_factor[1];
    blend.blue = recipe.blend_factor[2];
    blend.alpha = recipe.blend_factor[3];
    blend.stencil_ref = recipe.stencil_ref;
    cache.blend_factor = recipe.blend_factor;
    cache.stencil_ref = recipe.stencil_ref;
    cache.blend_valid = true;
  }
  if (!cache.viewports_valid ||
      !values_equal(cache.viewports, recipe.viewports)) {
    auto &command = enc.encodeRenderCommand<wmtcmd_render_setviewports>();
    command.type = WMTRenderCommandSetViewports;
    auto *data = static_cast<WMTViewport *>(enc.allocate_cpu_heap(
        sizeof(WMTViewport) * recipe.viewports.size(), alignof(WMTViewport)));
    if (!recipe.viewports.empty())
      std::memcpy(data, recipe.viewports.data(),
                  sizeof(WMTViewport) * recipe.viewports.size());
    command.viewports.set(data);
    command.viewport_count = recipe.viewports.size();
    cache.viewports = recipe.viewports;
    cache.viewports_valid = true;
  }
  if (!cache.scissors_valid ||
      !values_equal(cache.scissors, recipe.scissors)) {
    auto &command = enc.encodeRenderCommand<wmtcmd_render_setscissorrects>();
    command.type = WMTRenderCommandSetScissorRects;
    auto *data = static_cast<WMTScissorRect *>(enc.allocate_cpu_heap(
        sizeof(WMTScissorRect) * recipe.scissors.size(),
        alignof(WMTScissorRect)));
    if (!recipe.scissors.empty())
      std::memcpy(data, recipe.scissors.data(),
                  sizeof(WMTScissorRect) * recipe.scissors.size());
    command.scissor_rects.set(data);
    command.rect_count = recipe.scissors.size();
    cache.scissors = recipe.scissors;
    cache.scissors_valid = true;
  }
}

} // namespace dxmt::d3d12
