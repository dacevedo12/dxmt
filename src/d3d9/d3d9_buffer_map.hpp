#pragma once

#include <algorithm>
#include <cstdint>

#include "d3d9.h"

// Pure map-mode / lock-flag / dirty-range logic for vertex and index
// buffers, factored out of d3d9_buffer.cpp so the host unit tier can
// exercise it without a Metal device. References: DXVK
// d3d9_common_buffer.h, DXVK d3d9_device.cpp, wined3d buffer.c.

namespace dxmt {

// THE STORAGE CONTRACT, and it admits no exceptions: no pointer this
// implementation hands the application ever aliases memory the GPU can wire.
// Every buffer is staged. Lock returns a host mirror we own and Metal has
// never seen; the allocation a draw reads is Metal-owned, never handed out,
// and is refreshed from that mirror.
//
// DXVK's DetermineMapMode (d3d9_common_buffer.cpp) sends DEFAULT+DYNAMIC down
// its allowDirectBufferMapping arm instead, aliasing the Lock pointer onto the
// backing queued draws read in place, and we followed that shape until two
// platform facts ruled it out.
//
// First, such a pointer livelocks. Once the GPU has used the wrapping buffer
// its pages are wired, and a store from translated code into a wired page that
// carries translation state is rejected and re-executed at fault-service
// cadence for as long as the wrap lives: 5,979 ns per store, against 0.25 ns
// for the same store once any one of those conditions is absent. The
// application is the one writer whose pages cannot be vetted, and it keeps
// writing after Unlock, so there is no window in which handing it wrapped
// memory is safe.
//
// Second, the alternative that would avoid the wrap is unreachable here.
// Aliasing memory Metal itself allocates is what DXVK effectively does through
// Vulkan, but Metal allocates above 4 GB in practice, so a 32-bit guest cannot
// address it. Giving that guest a lock pointer at all requires wrapping pages
// it can address, which is exactly the livelock. The divergence from DXVK is
// forced by the address space, not chosen.
//
// Staging costs one copy per refresh, which on unified memory is DRAM
// bandwidth rather than a transfer, and it buys back guest address space: the
// allocation a draw reads is Metal-owned and charges none, where in-place
// renames charged it per allocation in flight. Should a native or 64-bit guest
// ever remove both premises, shared-storage direct mapping becomes the correct
// answer again and this is the comment to re-examine.

// Lock-flag sanitisation. The runtime silently drops flags that the
// pool/usage/device combination does not honour before any of them take
// effect.
inline DWORD
sanitize_buffer_lock_flags(DWORD flags, D3DPOOL pool, DWORD usage, bool swvp_only) {
  // DISCARD is honoured only when it rides alone.
  if ((flags & (D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE | D3DLOCK_READONLY)) != D3DLOCK_DISCARD)
    flags &= ~D3DLOCK_DISCARD;
  // DISCARD and NOOVERWRITE are honoured only in the DEFAULT pool, and never
  // on a software-vertex-processing-only device: native keeps the lock
  // pointer stable and the contents intact there (DXVK d3d9_device.cpp
  // strips both the same way; wine's d3d9 device tests assert the pinned
  // behaviour).
  if (pool != D3DPOOL_DEFAULT || swvp_only)
    flags &= ~(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
  // DONOTWAIT is honoured only for non-DYNAMIC buffers.
  if (usage & D3DUSAGE_DYNAMIC)
    flags &= ~D3DLOCK_DONOTWAIT;
  // READONLY is honoured only in the MANAGED pool.
  if ((flags & D3DLOCK_READONLY) && pool != D3DPOOL_MANAGED)
    flags &= ~D3DLOCK_READONLY;
  return flags;
}

// SizeToLock IS NOT TRACKED, deliberately. The runtime hands back
// base + OffsetToLock and validates nothing, and shipped titles lock a small
// window then write far past it: one such title lost 233,077 of 524,288 index
// bytes that way, rendering a black world with no error anywhere. Windows
// tolerates it because the system-memory copy the application writes IS the
// buffer there, so a write outside the announced window still reaches the draw.
// Applications also keep writing after Unlock, which no bookkeeping finalised at
// Unlock can survive. So a write lock marks the buffer dirty and nothing more;
// what a draw reads is decided from the draw, not from what the lock announced.
//
// Whether a Lock dirties the buffer at all. Post-sanitisation flags, so
// READONLY here is already the effective (MANAGED-only) flag.
// D3DLOCK_NO_DIRTY_UPDATE is a texture-only flag: D3D9 buffers always mark a
// write-lock dirty regardless of it (wined3d buffer.c invalidates on any write
// map and never consults the flag; only the texture map path checks it).
inline bool
buffer_lock_updates_dirty(DWORD flags) {
  return !(flags & D3DLOCK_READONLY);
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

} // namespace dxmt
