#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d12.h>
#include <string>

namespace dxmt::d3d12 {

struct DrawInstancedRecord;
struct DrawIndexedInstancedRecord;

// Draw-state logs only carry the leading characters of a shader cache key;
// that is enough to correlate records without bloating every line.
inline constexpr size_t kShaderCacheKeyPrefixLength = 16;

[[nodiscard]] std::string ShaderCacheKeyPrefix(const std::string &cache_key);

// One past the highest set input slot in `slot_mask`, i.e. how many low slots
// have to be walked. Zero when no slot is set.
[[nodiscard]] uint32_t InputSlotMaskWidth(uint32_t slot_mask) noexcept;

// The first occurrences of a selected pipeline are all sampled; afterwards
// only powers of two are, so long runs decay to a logarithmic trickle.
inline constexpr uint32_t kTargetOccurrenceDenseSamples = 8;

// `occurrence` is 1-based; 0 means "not a selected pipeline" and never samples.
[[nodiscard]] bool IsSampledTargetOccurrence(uint32_t occurrence) noexcept;

// Accumulates the anomalies a single draw state exhibits: a bit set for
// machine filtering plus the comma-joined reason list for the log line.
struct DrawStateAnomaly {
  static constexpr uint32_t kMissingColorAttachment = 1u << 0;
  static constexpr uint32_t kColorFormatMismatch = 1u << 1;
  static constexpr uint32_t kDepthFormatMismatch = 1u << 2;
  static constexpr uint32_t kMissingVertexView = 1u << 3;
  static constexpr uint32_t kUnresolvedVertexView = 1u << 4;
  static constexpr uint32_t kVertexViewOutOfBounds = 1u << 5;
  static constexpr uint32_t kVertexFetchOutOfBounds = 1u << 6;
  static constexpr uint32_t kMissingIndexView = 1u << 7;
  static constexpr uint32_t kUnresolvedIndexView = 1u << 8;
  static constexpr uint32_t kIndexFetchOutOfBounds = 1u << 9;

  uint32_t flags = 0;
  std::string reasons;

  void Add(uint32_t flag, const char *reason);
};

// True when the bound vertex view reaches past the end of its resource.
[[nodiscard]] bool VertexViewExceedsResource(UINT64 resource_offset,
                                             UINT64 view_size,
                                             UINT64 resource_width) noexcept;

// True when a non-indexed draw fetches vertices past the end of the bound
// view. A zero stride or vertex count fetches nothing and never exceeds.
[[nodiscard]] bool NonIndexedVertexFetchExceedsView(
    const DrawInstancedRecord &draw, UINT stride, UINT view_size) noexcept;

// True when an indexed draw reads indices past the end of the bound index
// view, including the case of an unusable (zero-sized) index format.
[[nodiscard]] bool
IndexedFetchExceedsView(const DrawIndexedInstancedRecord &indexed_draw,
                        UINT index_size, UINT view_size) noexcept;

// Byte offset into the vertex view at which a readback should start so that
// the sampled window covers the vertices the draw actually fetches.
[[nodiscard]] UINT64
ReadbackVertexOffset(const DrawInstancedRecord *draw,
                     const DrawIndexedInstancedRecord *indexed_draw,
                     UINT stride) noexcept;

} // namespace dxmt::d3d12
