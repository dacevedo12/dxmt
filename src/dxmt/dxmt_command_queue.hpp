#pragma once

#include "Metal.hpp"
#include "dxmt_allocation.hpp"
#include "dxmt_capture.hpp"
#include "dxmt_command.hpp"
#include "dxmt_command_list.hpp"
#include "dxmt_context.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_statistics.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_cpu_fence.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <span>
#include <vector>

namespace dxmt {

template <typename T> class moveonly_list {
public:
  moveonly_list(T *storage, size_t size) : storage(storage), size_(size) {
    for (unsigned i = 0; i < size_; i++) {
      new (storage + i) T();
    }
  }

  moveonly_list(const moveonly_list &copy) = delete;
  moveonly_list(moveonly_list &&move) {
    storage = move.storage;
    move.storage = nullptr;
    size_ = move.size_;
    move.size_ = 0;
  };

  ~moveonly_list() {
    for (unsigned i = 0; i < size_; i++) {
      storage[i].~T();
    }
  };

  T &
  operator[](int index) {
    return storage[index];
  }

  std::span<T>
  span() const {
    return std::span<T>(storage, size_);
  }

  T *
  data() const {
    return storage;
  }

  size_t
  size() const {
    return size_;
  }

private:
  T *storage;
  size_t size_;
};

constexpr uint32_t kCommandChunkCount = 32;

class CommandQueue;

class CommandChunk {
public:
  CommandChunk(const CommandChunk &) = delete; // delete copy constructor

  void *
  allocate_cpu_heap(size_t size, size_t alignment);

  template <CommandWithContext<ArgumentEncodingContext> F>
  void
  emitcc(F &&func) {
    list_enc.emit(std::forward<F>(func), allocate_cpu_heap(list_enc.calculateCommandSize<F>(), 16));
  }

  void
  encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc);

  uint64_t chunk_id;
  uint64_t chunk_event_id;
  uint64_t frame_;
  uint64_t signal_frame_latency_fence_;
  clock::time_point publish_time;
  clock::time_point encode_begin_time;
  clock::time_point encode_end_time;
  clock::time_point metal_commit_time;
  clock::time_point finish_begin_time;
  clock::time_point finish_complete_time;
  QueryReadbacks readback;
  std::vector<std::function<void()>> completion_callbacks;
  std::vector<std::function<void()>> deferred_readbacks;
  uint64_t resource_initializer_event_id;

private:
  CommandQueue *queue;
  WMT::Reference<WMT::CommandBuffer> attached_cmdbuf;
  
  CommandList<ArgumentEncodingContext> list_enc;
  AllocationRefTracking ref_tracker;

  friend class CommandQueue;

public:
  CommandChunk() {}

  void
  reset() {
    signal_frame_latency_fence_ = ~0ull;
    publish_time = {};
    encode_begin_time = {};
    encode_end_time = {};
    metal_commit_time = {};
    finish_begin_time = {};
    finish_complete_time = {};
    readback = {};
    for (auto &callback : completion_callbacks)
      callback();
    completion_callbacks.clear();
    deferred_readbacks.clear();
    list_enc.reset();
    ref_tracker.clear();
    attached_cmdbuf = nullptr;
  }
};

class CommandQueue {

private:
  void CommitChunkInternal(CommandChunk &chunk);

  uint32_t EncodingThread();

  uint32_t WaitForFinishThread();

  uint32_t ReadbackThread();

  void EnqueueReadbacks(CommandChunk &chunk);

  std::atomic_uint64_t ready_for_encode = 1; // we start from 1, so 0 is always coherent
  std::atomic_uint64_t ready_for_commit = 1;
  std::atomic_uint64_t chunk_ongoing = 0;
  CpuFence cpu_coherent;
  CpuFence frame_latency_fence_;
  std::atomic_bool stopped = false;
  dxmt::mutex readback_mutex_;
  dxmt::condition_variable readback_cond_;
  std::vector<std::function<void()>> pending_readbacks_;
  const bool apitrace_enabled_;

  std::array<CommandChunk, kCommandChunkCount> chunks;
  uint64_t encoder_seq = 1;
  uint64_t frame_count = 0;
  uint32_t max_latency_ = 3;
  uint64_t diag_last_gpu_end_ns_ = 0;

  dxmt::thread encodeThread;
  dxmt::thread finishThread;
  dxmt::thread readbackThread;
  WMT::Device device;
  WMT::Reference<WMT::CommandQueue> commandQueue;

  obj_handle_t shared_event_listener;
  dxmt::thread event_listener_thread;

  friend class CommandChunk;
  uint64_t
  GetNextEncoderId() {
    return encoder_seq++;
  }

  RingBumpState<StagingBufferBlockAllocator> staging_allocator;
  RingBumpState<GpuPrivateBufferBlockAllocator> copy_temp_allocator;
  RingBumpState<StagingBufferBlockAllocator, kCommandChunkGPUHeapSize> argbuf_allocator;
  RingBumpState<HostBufferBlockAllocator, kCommandChunkGPUHeapSize> argbuf_shadow_allocator;
  RingBumpState<HostBufferBlockAllocator, kCommandChunkCPUHeapSize> cpu_command_allocator;
  RingBumpState<HostBufferBlockAllocator, 0x1000 /* 4kB */> reftracker_storage_allocator;
  CaptureState capture_state;

public:
  InternalCommandLibrary cmd_library;
  ArgumentEncodingContext argument_encoding_ctx;
  WMT::Reference<WMT::SharedEvent> event;
  std::uint64_t current_event_seq_id = 0;
  FrameStatisticsContainer statistics;
  ResourceInitializer initializer;

  CommandQueue(WMT::Device device);

  ~CommandQueue();

  CommandChunk *
  CurrentChunk() {
    auto id = ready_for_encode.load(std::memory_order_relaxed);
    return &chunks[id % kCommandChunkCount];
  };

  uint64_t
  CoherentSeqId() {
    return cpu_coherent.signaledValue();
  };

  uint64_t
  CurrentSeqId() {
    return ready_for_encode.load(std::memory_order_relaxed);
  };

  uint64_t
  GetNextEventSeqId() {
    return ++current_event_seq_id;
  };

  uint64_t
  GetCurrentEventSeqId() {
    return current_event_seq_id;
  };


  uint64_t
  SignaledEventSeqId() {
    return event.signaledValue();
  };

  obj_handle_t GetSharedEventListener() {
    return shared_event_listener;
  }

  bool
  apitraceEnabled() const {
    return apitrace_enabled_;
  }

  /**
  This is not thread-safe!
  CurrentChunk & CommitCurrentChunk should be called on the same thread

  */
  void CommitCurrentChunk();

  uint64_t CurrentFrameSeq() {
    return frame_count + 1;
  }

  FrameStatistics& CurrentFrameStatistics() {
    return statistics.at(frame_count);
  }

  void
  PresentBoundary();

  uint32_t GetMaxLatency() { return max_latency_; }

  void SetMaxLatency(uint32_t value) { max_latency_ = value; };

  void
  WaitCPUFence(uint64_t seq);

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateStagingBuffer1(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset, block.gpu_address};
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    return {block.buffer, offset, block.gpu_address};
  }

  AllocatedArgumentBufferSlice
  AllocateArgumentBuffer(uint64_t seq, size_t size, size_t alignment = 64) {
    if (!size)
      return {};
    auto [block, offset] = argbuf_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    if constexpr (sizeof(void *) == 4) {
      auto [shadow_block, shadow_offset] = argbuf_shadow_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
      return {ptr_add(shadow_block.ptr, shadow_offset), block.buffer, offset,
              block.gpu_address, size, true};
    } else {
      return {ptr_add(block.mapped_address, offset), block.buffer, offset,
              block.gpu_address, size, false};
    }
  }

  void *
  AllocateCommandData(size_t size, size_t alignment) {
    auto [block, offset] =
        cpu_command_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    return ptr_add(block.ptr, offset);
  }

  void Retain(uint64_t seq, Allocation *allocation);
};

} // namespace dxmt
