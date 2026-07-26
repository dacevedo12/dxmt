#include "d3d12_draw_state_audit.hpp"

#include "d3d12_command_list.hpp"
#include "d3d12_render_state.hpp"

#include <algorithm>

namespace dxmt::d3d12 {

std::string ShaderCacheKeyPrefix(const std::string &cache_key) {
  const auto *key = cache_key.c_str();
  const auto key_size =
      std::min<size_t>(cache_key.size(), kShaderCacheKeyPrefixLength);
  return std::string(key, key + key_size);
}

uint32_t InputSlotMaskWidth(uint32_t slot_mask) noexcept {
  return slot_mask ? kInputSlotMaskBitCount - __builtin_clz(slot_mask) : 0u;
}

bool IsSampledTargetOccurrence(uint32_t occurrence) noexcept {
  return occurrence && (occurrence <= kTargetOccurrenceDenseSamples ||
                        (occurrence & (occurrence - 1)) == 0);
}

void DrawStateAnomaly::Add(uint32_t flag, const char *reason) {
  flags |= flag;
  if (!reasons.empty())
    reasons += ',';
  reasons += reason;
}

bool VertexViewExceedsResource(UINT64 resource_offset, UINT64 view_size,
                               UINT64 resource_width) noexcept {
  return resource_offset > resource_width ||
         view_size > resource_width - resource_offset;
}

bool NonIndexedVertexFetchExceedsView(const DrawInstancedRecord &draw,
                                      UINT stride, UINT view_size) noexcept {
  if (!stride || !draw.vertex_count_per_instance)
    return false;
  const uint64_t last_vertex = uint64_t(draw.start_vertex_location) +
                               draw.vertex_count_per_instance - 1;
  const uint64_t required = (last_vertex + 1) * uint64_t(stride);
  return required > view_size;
}

bool IndexedFetchExceedsView(const DrawIndexedInstancedRecord &indexed_draw,
                             UINT index_size, UINT view_size) noexcept {
  const uint64_t required = (uint64_t(indexed_draw.start_index_location) +
                             indexed_draw.index_count_per_instance) *
                            index_size;
  return !index_size || required > view_size;
}

UINT64 ReadbackVertexOffset(const DrawInstancedRecord *draw,
                            const DrawIndexedInstancedRecord *indexed_draw,
                            UINT stride) noexcept {
  return draw ? uint64_t(draw->start_vertex_location) * stride
              : indexed_draw && indexed_draw->base_vertex_location > 0
                    ? uint64_t(indexed_draw->base_vertex_location) * stride
                    : 0;
}

} // namespace dxmt::d3d12
