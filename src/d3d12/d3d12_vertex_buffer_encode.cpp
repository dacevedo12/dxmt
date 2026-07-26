#include "d3d12_vertex_buffer_encode.hpp"

#include "d3d12_command_list.hpp"

namespace dxmt::d3d12 {

void
EncodeCompiledVertexBindingRecipe(ArgumentEncodingContext &enc,
                                  const CompiledVertexBindingRecipe &recipe,
                                  uint64_t &argbuf_offset,
                                  uint32_t dirty_mask) {
  for (UINT slot = 0; slot < kInputSlotMaskBitCount; ++slot) {
    if ((dirty_mask & recipe.cleared_slot_mask) & (1u << slot))
      enc.bindVertexBuffer(slot, 0, 0, Rc<Buffer>());
  }
  for (const auto &binding : recipe.bindings) {
    if (binding.slot < kInputSlotMaskBitCount &&
        !(dirty_mask & (1u << binding.slot)))
      continue;
    enc.bindVertexBuffer(binding.slot, binding.offset, binding.stride,
                         Rc<Buffer>(binding.buffer));
  }
  if (!recipe.slot_mask)
    return;
  const auto table_size = uint64_t(__builtin_popcount(recipe.slot_mask)) *
                          kVertexBufferTableEntrySize;
  const auto offset = AllocateArgumentBuffer(argbuf_offset, table_size);
  enc.encodeVertexBuffers<PipelineKind::Ordinary>(recipe.slot_mask, offset);
}

} // namespace dxmt::d3d12
