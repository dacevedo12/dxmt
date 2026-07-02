#pragma once

#include <algorithm>
#include <cstdint>

#include "d3d9.h"

// Pure map-mode / lock-flag / dirty-range logic for vertex and index
// buffers, factored out of d3d9_buffer.cpp so the host unit tier can
// exercise it without a Metal device. References: DXVK
// d3d9_common_buffer.h, DXVK d3d9_device.cpp, wined3d buffer.c.

namespace dxmt {

// How a buffer's app-locked pointer relates to the memory the GPU reads.
// Direct: the pointer aliases the placed Shared backing the GPU samples;
// a plain Lock of an in-flight backing must sync against the GPU. Buffer:
// the app writes a host mirror not registered with Metal, and Unlock
// copies the dirty range into a GPU-only backing, so a Lock never waits.
enum class D3D9BufferMapMode : uint8_t {
  Direct,
  Buffer,
};

// Only a DEFAULT-pool DYNAMIC buffer is directly mapped; every other
// pool/usage combination is staged.
inline D3D9BufferMapMode
determine_buffer_map_mode(D3DPOOL pool, DWORD usage) {
  if (pool == D3DPOOL_DEFAULT && (usage & D3DUSAGE_DYNAMIC))
    return D3D9BufferMapMode::Direct;
  return D3D9BufferMapMode::Buffer;
}

// Lock-flag sanitisation. The runtime silently drops flags that the
// pool/usage combination does not honour before any of them take effect.
inline DWORD
sanitize_buffer_lock_flags(DWORD flags, D3DPOOL pool, DWORD usage) {
  // DISCARD is honoured only when it rides alone.
  if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) != D3DLOCK_DISCARD)
    flags &= ~D3DLOCK_DISCARD;
  // DISCARD and NOOVERWRITE are honoured only in the DEFAULT pool.
  if (pool != D3DPOOL_DEFAULT)
    flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
  // DONOTWAIT is honoured only for non-DYNAMIC buffers.
  if (usage & D3DUSAGE_DYNAMIC)
    flags &= ~D3DLOCK_DONOTWAIT;
  // READONLY is honoured only in the MANAGED pool.
  if ((flags & D3DLOCK_READONLY) && pool != D3DPOOL_MANAGED)
    flags &= ~D3DLOCK_READONLY;
  return flags;
}

// The byte span of a staged buffer that Lock calls have dirtied and
// Unlock must upload.
struct D3D9BufferRange {
  uint32_t min = 0;
  uint32_t max = 0;

  bool
  empty() const {
    return min == max;
  }

  void
  clear() {
    min = 0;
    max = 0;
  }

  // Grow to also cover r. A previously empty range becomes exactly r.
  void
  conjoin(D3D9BufferRange r) {
    if (empty()) {
      *this = r;
    } else {
      min = std::min(r.min, min);
      max = std::max(r.max, max);
    }
  }
};

// Whether a staged Lock honours the app's [offset, size) sub-range for
// dirty tracking. A DISCARD or a zero SizeToLock dirties the whole
// buffer; otherwise only MANAGED and DYNAMIC respect the sub-range.
inline bool
buffer_lock_respects_bounds(DWORD flags, UINT size_to_lock, D3DPOOL pool, DWORD usage) {
  return !(flags & D3DLOCK_DISCARD) && size_to_lock != 0 &&
         (pool == D3DPOOL_MANAGED || (usage & D3DUSAGE_DYNAMIC));
}

// The range a single staged Lock dirties, before conjoining with any
// prior dirty range. Post-sanitisation flags. OffsetToLock / SizeToLock
// are neither clamped nor validated by the runtime, so the offset is
// clamped to the buffer end (a past-end lock degenerates to an empty
// range) and the size to what remains; without this an offset past the
// end wraps the subtraction and the upload runs off the buffer.
inline D3D9BufferRange
buffer_lock_dirty_range(
    DWORD flags, UINT offset_to_lock, UINT size_to_lock, UINT buffer_size, D3DPOOL pool, DWORD usage
) {
  if (buffer_lock_respects_bounds(flags, size_to_lock, pool, usage)) {
    uint32_t offset = std::min<uint32_t>(offset_to_lock, buffer_size);
    uint32_t size = std::min<uint32_t>(size_to_lock, buffer_size - offset);
    return {offset, offset + size};
  }
  return {0, buffer_size};
}

// Whether a staged Lock updates the dirty range at all. Post-sanitisation
// flags, so READONLY here is already the effective (MANAGED-only) flag.
// DEFAULT ignores NO_DIRTY_UPDATE.
inline bool
buffer_lock_updates_dirty(DWORD flags, D3DPOOL pool) {
  return (pool == D3DPOOL_DEFAULT || !(flags & D3DLOCK_NO_DIRTY_UPDATE)) && !(flags & D3DLOCK_READONLY);
}

// Nested-lock counter. D3D9 permits nested Lock/Unlock on one buffer; the
// staged upload fires only when the outer Unlock returns the count to
// zero. Unlock is clamped so an unbalanced Unlock cannot underflow.
struct D3D9BufferLockCount {
  uint32_t count = 0;

  uint32_t
  increment() {
    return ++count;
  }

  uint32_t
  decrement() {
    if (count == 0)
      return 0;
    return --count;
  }
};

// Whether Unlock should upload the dirty range: only on the outer unlock
// (the decrement returned the count to zero) and only if some Lock
// actually dirtied a span.
inline bool
buffer_unlock_should_upload(uint32_t lock_count_after_decrement, bool dirty_empty) {
  return lock_count_after_decrement == 0 && !dirty_empty;
}

} // namespace dxmt
