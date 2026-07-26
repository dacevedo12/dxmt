#pragma once

#include "d3d12_argument_buffer_layout.hpp"
#include "d3d12_render_state.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_context.hpp"

#include <cstdint>

namespace dxmt::d3d12 {

struct CompiledVertexBindingRecipe;

// Each vertex buffer entry of the Metal argument table is a 16 byte
// {address, length} pair.
inline constexpr uint64_t kVertexBufferTableEntrySize = 16;

// Rebinds the vertex buffers of a compiled draw. `dirty_mask` selects the input
// slots that actually changed since the encoder's last compiled binding
// program; slots outside the mask keep their current binding.
void EncodeCompiledVertexBindingRecipe(ArgumentEncodingContext &enc,
                                       const CompiledVertexBindingRecipe &recipe,
                                       uint64_t &argbuf_offset,
                                       uint32_t dirty_mask = UINT32_MAX);

// Rebinds every input slot referenced by the pipeline's input layout from the
// live replay state, then emits the vertex buffer argument table.
//
// `State` is any replay/binding state exposing `vertex_buffers` and
// `resolved_vertex_buffers`.
template <typename State>
void
EncodeVertexBuffers(ArgumentEncodingContext &enc, const State &state,
                    const PipelineGraphicsState *graphics_state,
                    uint64_t &argbuf_offset, PipelineKind pipeline_kind) {
  if (!graphics_state)
    return;

  const uint32_t slot_mask = InputSlotMask(graphics_state);
  if (!slot_mask)
    return;

  const auto max_slot = kInputSlotMaskBitCount - __builtin_clz(slot_mask);
  for (UINT slot = 0; slot < max_slot; slot++) {
    if (!(slot_mask & (1u << slot)))
      continue;
    enc.bindVertexBuffer(slot, 0, 0, Rc<Buffer>());
    const auto &vertex_buffer = state.vertex_buffers[slot];
    if (!vertex_buffer)
      continue;
    const auto &view = *vertex_buffer;
    const auto &resolved = state.resolved_vertex_buffers[slot];
    if (!resolved.valid || resolved.address != view.BufferLocation ||
        !resolved.buffer)
      continue;
    enc.bindVertexBuffer(slot, resolved.binding_offset, view.StrideInBytes,
                         Rc<Buffer>(resolved.buffer));
  }

  const auto table_size =
      uint64_t(__builtin_popcount(slot_mask)) * kVertexBufferTableEntrySize;
  const auto offset = AllocateArgumentBuffer(argbuf_offset, table_size);
  if (pipeline_kind == PipelineKind::Geometry)
    enc.encodeVertexBuffers<PipelineKind::Geometry>(slot_mask, offset);
  else if (pipeline_kind == PipelineKind::Tessellation)
    enc.encodeVertexBuffers<PipelineKind::Tessellation>(slot_mask, offset);
  else
    enc.encodeVertexBuffers<PipelineKind::Ordinary>(slot_mask, offset);
}

} // namespace dxmt::d3d12
