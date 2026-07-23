#pragma once

#include "Metal.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_math.hpp"
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>

namespace dxmt {

constexpr size_t kStagingBlockSize = 0x2000000; // 32MB
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

/**
 * Poison host mappings before reclaiming ring blocks so late writers hit a
 * deterministic trap (0xDE fill) rather than a recycled page. Disable with
 * DXMT_DIAG_POISON_MAPPINGS=0. Enabled by default while ownership bugs are
 * still under investigation; also forced on for dense hang/root-cause diags.
 */
inline bool
RingBumpPoisonMappingsEnabled() {
  static const bool enabled = []() {
    auto explicit_value = env::getEnvVar("DXMT_DIAG_POISON_MAPPINGS");
    if (explicit_value == "0" || explicit_value == "false" ||
        explicit_value == "no" || explicit_value == "off")
      return false;
    if (explicit_value == "1" || explicit_value == "true" ||
        explicit_value == "yes" || explicit_value == "on")
      return true;
    auto hang = env::getEnvVar("DXMT_DIAG_GPU_HANG_DENSE");
    auto root = env::getEnvVar("DXMT_DIAG_ROOT_CAUSE_DENSE");
    if (hang == "1" || hang == "true" || hang == "yes" || hang == "on" ||
        root == "1" || root == "true" || root == "yes" || root == "on")
      return true;
    // Default on: BMW host-mapping AVs need attributable failures.
    return true;
  }();
  return enabled;
}

template <typename Block>
void
PoisonRingBlockHostMapping(Block &block, size_t nbytes) {
  if (!RingBumpPoisonMappingsEnabled() || !nbytes)
    return;
  // Only trap-fill host memory WE own (malloc). Metal Shared mappings from
  // get_accessible_or_null() can already be unmapped when the last MTLBuffer
  // retain drops — memset there is itself an AV, and concurrent encode may
  // still hold a raw mapped pointer into the same range.
  if constexpr (requires {
                  block.owns_mapped_address;
                  block.mapped_address;
                }) {
    if (block.owns_mapped_address && block.mapped_address)
      std::memset(block.mapped_address, 0xDE, nbytes);
  } else if constexpr (requires { block.ptr; }) {
    if (block.ptr)
      std::memset(block.ptr, 0xDE, nbytes);
  }
}

template <typename Allocator, size_t BlockSize = kStagingBlockSize, class mutex = dxmt::mutex> class RingBumpState {

public:

  static constexpr size_t block_size = BlockSize;

  RingBumpState(Allocator &&allocator) : allocator_(std::move(allocator)) {}

  std::pair<typename Allocator::Block &, uint64_t>
  allocate(uint64_t seq_id, uint64_t coherent_id, size_t size, size_t alignment,
           bool retain_pin = false);

  void free_blocks(uint64_t coherent_id);

  void setReleaseCallback(
      std::function<void(const typename Allocator::Block &, uint64_t)>
          callback) {
    std::lock_guard<mutex> lock(mutex_);
    release_callback_ = std::move(callback);
  }

private:
  struct Allocation {
    size_t allocated_size;
    size_t total_size;
    uint64_t last_used_seq_id;
    uint64_t inc_time_to_live;
    /**
     * Active host-mapping leases (e.g. argument-buffer slices). free_blocks /
     * reuse must not destroy the block while pin_count > 0 for an unfinished
     * generation (last_used_seq_id > coherent_id). When the generation
     * completes (last_used <= coherent), pins are dropped then reclaim runs.
     */
    uint32_t pin_count = 0;
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
  std::function<void(const typename Allocator::Block &, uint64_t)>
      release_callback_;
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
    uint64_t seq_id, uint64_t coherent_id, size_t size, size_t alignment,
    bool retain_pin
) {
  if (!alignment || (alignment & (alignment - 1)))
    throw std::invalid_argument("RingBumpState alignment must be a power of two");

  std::lock_guard<mutex> lock(mutex_);
  const auto guard_bytes =
      RingBumpGuardEnabled() && RingBumpAllocatorSupportsGuard<Allocator>()
          ? kRingBumpGuardBytes
          : 0;
  if (size > std::numeric_limits<size_t>::max() - guard_bytes)
    throw std::overflow_error("RingBumpState allocation size overflow");

  auto finish = [&](Allocation &allocation)
      -> std::pair<typename Allocator::Block &, uint64_t> {
    allocation.last_used_seq_id = seq_id;
    if (retain_pin) {
      if (allocation.pin_count == std::numeric_limits<uint32_t>::max())
        throw std::overflow_error("RingBumpState pin_count overflow");
      allocation.pin_count++;
    }
    return suballocate(allocation, size, alignment);
  };

  while (!fifo.empty()) {
    auto &latest = fifo.back();
    check_guard(latest, "allocate_latest");
    if (latest.total_size < guard_bytes ||
        latest.allocated_size >
            std::numeric_limits<size_t>::max() - (alignment - 1)) {
      break;
    }
    const auto offset = align(latest.allocated_size, alignment);
    const auto available = latest.total_size - guard_bytes;
    if (offset > available || size > available - offset)
      break;

    return finish(latest);
  }
  return finish(allocate_or_reuse_block(
      seq_id, coherent_id,
      std::max(size + guard_bytes,
               BlockSize) // in case required size is larger than block size
      ));
};

template <typename Allocator, size_t BlockSize, class mutex>
void
RingBumpState<Allocator, BlockSize, mutex>::free_blocks(uint64_t coherent_id) {
  std::lock_guard<mutex> lock(mutex_);
  while (!fifo.empty()) {
    auto &front = fifo.front();
    check_guard(front, "free_blocks");
    if (front.last_used_seq_id > coherent_id) {
      // Unfinished generation: pins remain; do not reclaim past this block.
      break;
    }
    // last_used <= coherent: GPU of last host writer completed. Host encode for
    // that generation is also complete (encode precedes GPU). Drop pin leases
    // then reclaim. Never reclaim while pin_count > 0 before the drop — that
    // would free host mappings under concurrent allocate(retain_pin) on this
    // block if last_used were stale (defense in depth).
    if (front.pin_count > 0)
      front.pin_count = 0;
    auto expired = (coherent_id - front.last_used_seq_id) > kStagingBlockLifetime ||
                   front.inc_time_to_live > kStagingBlockLifetime || coherent_id == -1ull;
    auto adhoc = front.total_size != BlockSize;
    if (expired || adhoc) {
      // Poison owned host pages only (see PoisonRingBlockHostMapping).
      PoisonRingBlockHostMapping(front.block, front.total_size);
      if (release_callback_)
        release_callback_(front.block, front.last_used_seq_id);
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
      // Do not destroy or reuse a block that still has active host leases
      // for a generation that has not been reclaimed yet.
      if (front.pin_count > 0) {
        // last_used < coherent implies GPU of last use completed; drop pins.
        front.pin_count = 0;
      }
      if (front.total_size != BlockSize) {
        PoisonRingBlockHostMapping(front.block, front.total_size);
        if (release_callback_)
          release_callback_(front.block, front.last_used_seq_id);
        fifo.pop();
        continue;
      } else if (front.total_size >= block_size) {
        // Reuse: poison previous generation contents before suballoc reset.
        PoisonRingBlockHostMapping(front.block, front.total_size);
        front.last_used_seq_id = seq_id;
        front.allocated_size = 0;
        front.inc_time_to_live = 0;
        front.pin_count = 0;
        fifo.push(std::move(front));
        fifo.pop();
        fill_guard(fifo.back());
        return fifo.back();
      }
      WARN("forced to allocate new block of size ", block_size);
    } else if (front.pin_count > 0 && front.last_used_seq_id > coherent_id) {
      // Still leased by an unfinished generation — stop reclaim walk.
      break;
    }
    break;
  }
  fifo.push({
      .allocated_size = 0,
      .total_size = block_size,
      .last_used_seq_id = seq_id,
      .inc_time_to_live = 0,
      .pin_count = 0,
      .block = allocator_.allocate(block_size),
  });
  fill_guard(fifo.back());
  return fifo.back();
};

} // namespace dxmt
