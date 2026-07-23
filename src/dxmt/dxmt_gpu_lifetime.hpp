#pragma once

#include "Metal.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace dxmt {

/**
 * Host-mapped GPU argument-buffer slice with fail-closed ownership semantics.
 *
 * A valid slice always has:
 *  - non-null host mapping of `length` bytes
 *  - a retaining Metal buffer reference (keeps Shared host mapping alive for
 *    the slice lifetime; generation pin is an additional GPU-lifetime retain)
 *
 * Business code must write via write()/fill_zero()/map(), not raw memcpy into
 * the mapped pointer (architecture tests enforce this outside allowlisted
 * internals).
 */
struct AllocatedArgumentBufferSlice {
  void *mapped = nullptr;
  /** Retaining handle: non-retaining WMT::Buffer allowed AV when ring free
   *  dropped the only strong ref while encode still wrote the mapping. */
  WMT::Reference<WMT::Buffer> gpu_buffer;
  uint64_t offset = 0;
  uint64_t gpu_address = 0;
  uint64_t length = 0;
  bool needs_flush = false;

  bool valid() const {
    return mapped != nullptr && gpu_buffer && length != 0;
  }

  explicit operator bool() const { return valid(); }

  bool write(size_t byte_offset, const void *src, size_t nbytes) {
    if (!nbytes)
      return valid() || !src;
    if (!valid() || !src)
      return false;
    if (byte_offset > length || nbytes > length - byte_offset)
      return false;
    std::memcpy(static_cast<char *>(mapped) + byte_offset, src, nbytes);
    return true;
  }

  bool fill_zero(size_t byte_offset = 0,
                 size_t nbytes = std::numeric_limits<size_t>::max()) {
    if (!valid())
      return false;
    if (byte_offset > length)
      return false;
    if (nbytes == std::numeric_limits<size_t>::max())
      nbytes = static_cast<size_t>(length - byte_offset);
    if (nbytes > length - byte_offset)
      return false;
    if (!nbytes)
      return true;
    std::memset(static_cast<char *>(mapped) + byte_offset, 0, nbytes);
    return true;
  }

  template <typename T>
  T *map(size_t element_offset, size_t element_count) {
    if (!valid() || !element_count)
      return nullptr;
    if (element_count >
        (std::numeric_limits<size_t>::max() / sizeof(T)))
      return nullptr;
    const size_t byte_offset = element_offset * sizeof(T);
    const size_t nbytes = element_count * sizeof(T);
    if (byte_offset > length || nbytes > length - byte_offset)
      return nullptr;
    return reinterpret_cast<T *>(static_cast<char *>(mapped) + byte_offset);
  }

  void flush_if_needed() {
    if (valid() && needs_flush)
      gpu_buffer.updateContents(offset, mapped, length);
  }
};

/**
 * Bindless mirror slot index safety: all arithmetic is uint64 and checked
 * before any host write. Shared by live and freeze bindless paths.
 */
inline bool
MirrorSlotInCapacity(uint32_t compact_base, uint32_t local,
                     uint32_t capacity) {
  if (!capacity)
    return false;
  const uint64_t idx =
      uint64_t(compact_base) + uint64_t(local);
  return idx < uint64_t(capacity);
}

inline bool
MirrorWriteU64(uint64_t *base, size_t qword_count, uint64_t index,
               uint64_t value) {
  if (!base || index >= qword_count)
    return false;
  base[index] = value;
  return true;
}

/** Sampler mirror layout: capacity slots × 3 qwords (handle, cube, lod). */
inline bool
MirrorWriteSamplerSlot(uint64_t *dst, uint32_t capacity, uint32_t compact_base,
                       uint32_t local, uint64_t handle, uint64_t cube_handle,
                       uint64_t lod_bias) {
  if (!dst || !MirrorSlotInCapacity(compact_base, local, capacity))
    return false;
  const uint64_t slot = uint64_t(compact_base) + uint64_t(local);
  const size_t qword_count = size_t(capacity) * 3u;
  if (!MirrorWriteU64(dst, qword_count, slot, handle))
    return false;
  if (!MirrorWriteU64(dst, qword_count, uint64_t(capacity) + slot, cube_handle))
    return false;
  return MirrorWriteU64(dst, qword_count,
                        uint64_t(capacity) * 2u + slot, lod_bias);
}

/**
 * Texture mirror layout: field_pairs × (capacity slots × 2 qwords handle/meta).
 */
inline bool
MirrorWriteTextureSlot(uint64_t *dst, uint32_t capacity, uint32_t field_pairs,
                       uint32_t compact_base, uint32_t local, uint64_t handle,
                       uint64_t metadata) {
  if (!dst || !field_pairs ||
      !MirrorSlotInCapacity(compact_base, local, capacity))
    return false;
  const uint64_t slot = uint64_t(compact_base) + uint64_t(local);
  const size_t qword_count =
      size_t(field_pairs) * size_t(capacity) * 2u;
  for (uint32_t pair = 0; pair < field_pairs; pair++) {
    const uint64_t pair_base =
        uint64_t(pair) * 2u * uint64_t(capacity);
    if (!MirrorWriteU64(dst, qword_count, pair_base + slot, handle))
      return false;
    if (!MirrorWriteU64(dst, qword_count,
                        pair_base + uint64_t(capacity) + slot, metadata))
      return false;
  }
  return true;
}

} // namespace dxmt
