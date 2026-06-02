#pragma once

#include "Metal.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_math.hpp"
#include <cstring>
#include <mutex>
#include <queue>

namespace dxmt {

// Default staging block size. 32 MB suits x86_64 / arm64 where the
// virtual-address space is effectively unbounded; the d3d9 build under
// Rosetta x86_32 + WoW64 lives inside a 2-3 GB user VA ceiling that a
// memory-heavy title can approach. Each block is host RAM + Metal
// address-space registration + a pre-fault memset,
// so 32 MB per block magnifies VA pressure quickly once the ring grows
// past a couple of blocks. Drop to 8 MB on i386: rings hold more,
// smaller blocks; pre-fault cost shrinks proportionally.
#ifdef __i386__
constexpr size_t kStagingBlockSize = 0x800000; // 8MB
#else
constexpr size_t kStagingBlockSize = 0x2000000; // 32MB
#endif
constexpr size_t kStagingBlockSizeForDeferredContext = 0x200000; // 2MB
constexpr size_t kStagingBlockLifetime = 300;
constexpr size_t kRingBumpGuardBytes = 64;
constexpr uint8_t kRingBumpGuardPattern = 0x5a;

inline bool
RingBumpGuardEnabled() {
  static const bool enabled = []() {
    auto value = env::getEnvVar("DXMT_DIAG_CPU_HEAP_GUARD");
    return value == "1" || value == "true" || value == "yes" ||
           value == "trace";
  }();
  return enabled;
}

template <typename Allocator, size_t BlockSize = kStagingBlockSize, class mutex = dxmt::mutex> class RingBumpState {

public:

  static constexpr size_t block_size = BlockSize;

  RingBumpState(Allocator &&allocator) : allocator_(std::move(allocator)) {}

  std::pair<typename Allocator::Block &, uint64_t>
  allocate(uint64_t seq_id, uint64_t coherent_id, size_t size, size_t alignment);

  void free_blocks(uint64_t coherent_id);

  // Pre-allocate `count` blocks of the ring's natural BlockSize and push
  // them onto the FIFO with last_used_seq_id=0 so the first call to
  // `allocate` reuses them rather than allocating fresh on the calling
  // thread. The Rosetta x86_32 first-touch cliff for a 32 MB Metal-
  // registered buffer is ~1.2 s; without this, that cost lands on a
  // d3d9 hot-path call (CreateVertexBuffer / DrawIndexedPrimitive) as
  // a visible stutter. With 2 blocks pre-allocated and GPU completion
  // recycling them, gameplay never allocates a fresh block.
  // Call from device construction time, where the cost hides inside
  // overall game startup. No locks: caller must serialize against
  // any concurrent `allocate` (typically the device isn't published
  // yet during ctor).
  void
  preallocate(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      fifo.push({
          .allocated_size  = 0,
          .total_size      = BlockSize,
          .last_used_seq_id = 0,
          .inc_time_to_live = 0,
          .block            = allocator_.allocate(BlockSize),
      });
    }
  }

private:
  struct Allocation {
    size_t allocated_size;
    size_t total_size;
    uint64_t last_used_seq_id;
    uint64_t inc_time_to_live;
    Allocator::Block block;
  };

  Allocation &allocate_or_reuse_block(uint64_t seq_id, uint64_t coherent_id, size_t block_size);

  bool check_guard(const Allocation &allocation, const char *context) const;

  void fill_guard(Allocation &allocation);

  std::pair<typename Allocator::Block &, uint64_t>
  suballocate(Allocation &allocation, size_t size, size_t alignment) {
    auto offset = align(allocation.allocated_size, alignment);
    allocation.allocated_size = offset + size;
    return {allocation.block, offset};
  };

  std::queue<Allocation> fifo;
  mutex mutex_;
  Allocator allocator_;
};

class GpuPrivateBufferBlockAllocator {
public:
  GpuPrivateBufferBlockAllocator(WMT::Device device, WMTResourceOptions block_options) {
    device_ = device;
    buffer_info_.memory.set(nullptr);
    buffer_info_.options = block_options;
  }

  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address;

    Block() = default;
    Block(const Block &copy) = delete;
    Block(Block &&move) = default;
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    buffer_info_.length = block_size;
    block.buffer = device_.newBuffer(buffer_info_);
    block.gpu_address = buffer_info_.gpu_address;
    return block;
  };

private:
  WMT::Device device_;
  WMTBufferInfo buffer_info_;
};

class StagingBufferBlockAllocator {
public:
  StagingBufferBlockAllocator(WMT::Device device, WMTResourceOptions block_options, bool placed_buffer = true) {
    device_ = device;
    buffer_info_ = block_options;
    placed_buffer_ = placed_buffer;
  }

  class Block {
  public:
    WMT::Reference<WMT::Buffer> buffer;
    uint64_t gpu_address;
    void *mapped_address;
    bool owns_mapped_address = false;

    ~Block() {
      if (owns_mapped_address && mapped_address) {
        free(mapped_address);
        mapped_address = nullptr;
      }
    };

    Block() = default;

    Block(const Block &) = delete;
    Block(Block &&move) {
      buffer = std::move(move.buffer);
      gpu_address = move.gpu_address;
      mapped_address = move.mapped_address;
      owns_mapped_address = move.owns_mapped_address;
      move.mapped_address = nullptr;
      move.owns_mapped_address = false;
    };
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    block.mapped_address = placed_buffer_ ? malloc(block_size) : nullptr;
    block.owns_mapped_address = placed_buffer_;
    WMTBufferInfo info;
    info.options = buffer_info_;
    info.memory.set(block.mapped_address);
    info.length = block_size;
    block.buffer = device_.newBuffer(info);
    block.gpu_address = info.gpu_address;
    if (!placed_buffer_)
      block.mapped_address = info.memory.get_accessible_or_null();
    // Pre-fault all pages of the malloc'd backing so subsequent CPU
    // writes from the d3d9 calling thread don't pay per-page first-
    // touch cost. On Apple Silicon + Rosetta x86_32, first-touch faults
    // on Metal-registered host buffers can cost 100+ ms each: a
    // consumer (e.g. BuildDrawCapture) that walks ~10 KB per call
    // through a fresh ring block stalls for seconds on the first
    // page of each call. Eager memset pays the cost once at block
    // allocation (~50 ms for a 32 MB block at native ARM speed)
    // instead of streaming it across every consumer.
    if (placed_buffer_ && block.mapped_address)
      std::memset(block.mapped_address, 0, block_size);
    return block;
  };

private:
  WMT::Device device_;
  WMTResourceOptions buffer_info_;
  bool placed_buffer_;
};

class HostBufferBlockAllocator {
public:
  class Block {
  public:
    void *ptr;

    Block() = default;

    Block(const Block &) = delete;
    Block(Block &&move) {
      ptr = move.ptr;
      move.ptr = nullptr;
    };

    ~Block() {
      if (ptr) {
        free(ptr);
        ptr = nullptr;
      }
    };
  };

  Block
  allocate(size_t block_size) {
    Block block{};
    block.ptr = malloc(block_size);
    return block;
  };
};

template <typename Block>
void *
RingBumpBlockHostPtr(Block &block) {
  if constexpr (requires { block.ptr; }) {
    return block.ptr;
  } else {
    return nullptr;
  }
}

template <typename Block>
constexpr bool
RingBumpBlockSupportsGuard() {
  return requires(Block &block) { block.ptr; };
}

template <typename Allocator>
constexpr bool
RingBumpAllocatorSupportsGuard() {
  return RingBumpBlockSupportsGuard<typename Allocator::Block>();
}

template <typename Allocator, size_t BlockSize, class mutex>
bool
RingBumpState<Allocator, BlockSize, mutex>::check_guard(
    const Allocation &allocation, const char *context) const {
  if (!RingBumpGuardEnabled() || !RingBumpAllocatorSupportsGuard<Allocator>())
    return true;

  auto *base = static_cast<const uint8_t *>(
      RingBumpBlockHostPtr(const_cast<typename Allocator::Block &>(allocation.block)));
  if (!base)
    return true;

  if (allocation.total_size < kRingBumpGuardBytes) {
    ERR("RingBumpState metadata corrupted context=", context,
        " blockSize=", allocation.total_size,
        " allocatedSize=", allocation.allocated_size,
        " lastUsedSeq=", allocation.last_used_seq_id);
    return false;
  }

  const auto user_size = allocation.total_size - kRingBumpGuardBytes;
  const auto *guard = base + user_size;
  for (size_t i = 0; i < kRingBumpGuardBytes; i++) {
    if (guard[i] != kRingBumpGuardPattern) {
      ERR("RingBumpState guard corrupted context=", context,
          " blockSize=", allocation.total_size,
          " userSize=", user_size,
          " allocatedSize=", allocation.allocated_size,
          " lastUsedSeq=", allocation.last_used_seq_id,
          " guardOffset=", i,
          " value=", uint32_t(guard[i]));
      return false;
    }
  }

  return true;
}

template <typename Allocator, size_t BlockSize, class mutex>
void
RingBumpState<Allocator, BlockSize, mutex>::fill_guard(Allocation &allocation) {
  if (!RingBumpGuardEnabled() || !RingBumpAllocatorSupportsGuard<Allocator>())
    return;

  auto *base = static_cast<uint8_t *>(RingBumpBlockHostPtr(allocation.block));
  if (!base)
    return;

  std::memset(base + allocation.total_size - kRingBumpGuardBytes,
              kRingBumpGuardPattern, kRingBumpGuardBytes);
}

template <typename Allocator, size_t BlockSize, class mutex>
std::pair<typename Allocator::Block &, uint64_t>
RingBumpState<Allocator, BlockSize, mutex>::allocate(
    uint64_t seq_id, uint64_t coherent_id, size_t size, size_t alignment
) {
  std::lock_guard<mutex> lock(mutex_);
  const auto guard_bytes =
      RingBumpGuardEnabled() && RingBumpAllocatorSupportsGuard<Allocator>()
          ? kRingBumpGuardBytes
          : 0;
  while (!fifo.empty()) {
    auto &latest = fifo.back();
    check_guard(latest, "allocate_latest");
    if ((align(latest.allocated_size, alignment) + size) >
        latest.total_size - guard_bytes) {
      break;
    }
    // Only ever advance last_used_seq_id, never regress. The d3d9 chunk
    // pipeline calls allocate() concurrently from two threads with
    // different seq_id sources: the encode thread passes the chunk's
    // snapshotted seq (older), the calling thread passes the live
    // m_currentCmdSeq (newer). The prior unconditional write let the
    // encode-thread Resolve at seq=N stomp a block whose last_used was
    // just bumped to N+5 by a calling-thread allocate, which then made
    // free_blocks(coherent_id in [N..N+4]) drop a block the calling
    // thread had pinned to chunks N+1..N+5: UAF on the placed-buffer
    // backing once those chunks reached the GPU.
    if (seq_id > latest.last_used_seq_id)
      latest.last_used_seq_id = seq_id;
    return suballocate(latest, size, alignment);
  }
  return suballocate(
      allocate_or_reuse_block(
          seq_id, coherent_id, std::max(size + guard_bytes, BlockSize) // in case required size is larger than block size
      ),
      size, alignment
  );
};

template <typename Allocator, size_t BlockSize, class mutex>
void
RingBumpState<Allocator, BlockSize, mutex>::free_blocks(uint64_t coherent_id) {
  std::lock_guard<mutex> lock(mutex_);
  while (!fifo.empty()) {
    auto &front = fifo.front();
    check_guard(front, "free_blocks");
    if (front.last_used_seq_id > coherent_id)
      break;
    auto expired = (coherent_id - front.last_used_seq_id) > kStagingBlockLifetime ||
                   front.inc_time_to_live > kStagingBlockLifetime || coherent_id == -1ull;
    auto adhoc = front.total_size != BlockSize;
    if (expired || adhoc) {
      // can be deallocated
      fifo.pop();
      continue;
    }
    front.inc_time_to_live++;
    break;
  }
};

template <typename Allocator, size_t BlockSize, class mutex>
RingBumpState<Allocator, BlockSize, mutex>::Allocation &
RingBumpState<Allocator, BlockSize, mutex>::allocate_or_reuse_block(
    uint64_t seq_id, uint64_t coherent_id, size_t block_size
) {
  while (!fifo.empty()) {
    auto &front = fifo.front();
    if (front.last_used_seq_id < coherent_id) {
      check_guard(front, "reuse_front");
      if (front.total_size != BlockSize) {
        fifo.pop();
        continue;
      } else if (front.total_size >= block_size) {
        front.last_used_seq_id = seq_id;
        front.allocated_size = 0;
        front.inc_time_to_live = 0;
        fifo.push(std::move(front));
        fifo.pop();
        fill_guard(fifo.back());
        return fifo.back();
      }
      WARN("forced to allocate new block of size ", block_size);
    }
    break;
  }
  fifo.push({
      .allocated_size = 0,
      .total_size = block_size,
      .last_used_seq_id = seq_id,
      .inc_time_to_live = 0,
      .block = allocator_.allocate(block_size),
  });
  fill_guard(fifo.back());
  return fifo.back();
};

} // namespace dxmt
