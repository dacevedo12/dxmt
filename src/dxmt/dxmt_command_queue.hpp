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
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

constexpr uint32_t kCommandChunkCount = 128;

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

  void beginSparseMappingDiagnostic(uint64_t resource_identity,
                                    uint64_t texture_handle,
                                    uint64_t heap_handle,
                                    uint64_t gpu_resource_id,
                                    uint64_t generation,
                                    uint32_t operation_count,
                                    uint32_t map_count,
                                    uint32_t unmap_count,
                                    uint32_t barrier_count) {
    sparse_mapping_diagnostic.sparse_mapping_call_count++;
    sparse_mapping_diagnostic.sparse_mapping_operation_count += operation_count;
    sparse_mapping_diagnostic.sparse_mapping_map_count += map_count;
    sparse_mapping_diagnostic.sparse_mapping_unmap_count += unmap_count;
    sparse_mapping_diagnostic.sparse_mapping_barrier_count += barrier_count;
    sparse_mapping_diagnostic.sparse_resource_identity = resource_identity;
    sparse_mapping_diagnostic.sparse_texture_handle = texture_handle;
    sparse_mapping_diagnostic.sparse_heap_handle = heap_handle;
    sparse_mapping_diagnostic.sparse_gpu_resource_id = gpu_resource_id;
    sparse_mapping_diagnostic.sparse_mapping_generation_begin = generation;
    sparse_mapping_diagnostic.sparse_mapping_generation_end = generation;
  }

  void completeSparseMappingDiagnostic(uint64_t generation, bool success) {
    sparse_mapping_diagnostic.sparse_mapping_generation_end = generation;
    if (!success)
      sparse_mapping_diagnostic.sparse_mapping_failure_count++;
  }

  void
  addCompletionCallback(std::function<void()> callback) {
    std::lock_guard<dxmt::mutex> lock(completion_callbacks_mutex_);
    completion_callbacks.push_back(std::move(callback));
  }

  size_t
  completionCallbackCount() {
    std::lock_guard<dxmt::mutex> lock(completion_callbacks_mutex_);
    return completion_callbacks.size();
  }

  uint64_t chunk_id;
  uint64_t chunk_event_id;
  uint64_t lifecycle_pair_id;
  uint64_t queue_lifecycle_id;
  uint64_t frame_;
  uint64_t signal_frame_latency_fence_;
  clock::time_point frame_begin_time;
  clock::time_point publish_time;
  clock::time_point encode_begin_time;
  clock::time_point encode_prepare_end_time;
  clock::time_point encode_end_time;
  clock::time_point metal_commit_time;
  clock::time_point finish_begin_time;
  clock::time_point finish_complete_time;
  QueryReadbacks readback;
  std::vector<std::function<void()>> completion_callbacks;
  dxmt::mutex completion_callbacks_mutex_;
  std::vector<std::function<void()>> deferred_readbacks;
  uint64_t resource_initializer_event_id;
  CommandBufferDiagnosticInfo sparse_mapping_diagnostic = {};
  uint32_t perf_input_encoder_count = 0;
  uint32_t perf_encoded_encoder_count = 0;
  uint32_t perf_render_encoder_count = 0;
  uint32_t perf_compute_encoder_count = 0;
  uint32_t perf_blit_encoder_count = 0;

private:
  CommandQueue *queue;
  WMT::Reference<WMT::CommandBuffer> attached_cmdbuf;
  std::vector<WMT::Reference<WMT::Resource>> retained_metal_resources;
  std::unordered_set<obj_handle_t> retained_metal_resource_handles;
  
  CommandList<ArgumentEncodingContext> list_enc;
  AllocationRefTracking ref_tracker;

  friend class CommandQueue;

public:
  CommandChunk() {}

  void
  reset() {
    lifecycle_pair_id = 0;
    queue_lifecycle_id = 0;
    signal_frame_latency_fence_ = ~0ull;
    frame_begin_time = {};
    publish_time = {};
    encode_begin_time = {};
    encode_prepare_end_time = {};
    encode_end_time = {};
    metal_commit_time = {};
    finish_begin_time = {};
    finish_complete_time = {};
    readback = {};
    std::vector<std::function<void()>> callbacks;
    {
      std::lock_guard<dxmt::mutex> lock(completion_callbacks_mutex_);
      callbacks.swap(completion_callbacks);
    }
    for (auto &callback : callbacks)
      callback();
    deferred_readbacks.clear();
    sparse_mapping_diagnostic = {};
    perf_input_encoder_count = 0;
    perf_encoded_encoder_count = 0;
    perf_render_encoder_count = 0;
    perf_compute_encoder_count = 0;
    perf_blit_encoder_count = 0;
    list_enc.reset();
    ref_tracker.clear();
    attached_cmdbuf = nullptr;
    retained_metal_resources.clear();
    retained_metal_resource_handles.clear();
  }
};

class CommandQueue {

private:
  void CommitChunkInternal(CommandChunk &chunk);

  void FlushFinalFrameStatistics();

  void RetirePersistentResidencyRemovals(uint64_t completed_seq);

  void PersistentResidencyThread();

  void EnsureCachedAllocationResidency(WMT::Resource resource);

  void RetireCachedAllocationResidency(WMT::Resource resource,
                                       uint64_t sequence);

  void CompleteDeferredReleases(uint64_t completed_seq);

  void DrainDeferredReleases();

  uint32_t EncodingThread();

  uint32_t WaitForFinishThread();

  uint32_t ReadbackThread();

  void EnqueueReadbacks(CommandChunk &chunk);

  std::atomic_uint64_t ready_for_encode = 1; // we start from 1, so 0 is always coherent
  std::atomic_uint64_t ready_for_commit = 1;
  std::atomic_uint64_t chunk_ongoing = 0;
  std::atomic_uint64_t lifecycle_sequence_ = 1;
  CpuFence cpu_coherent;
  CpuFence frame_latency_fence_;
  std::atomic_bool stopped = false;
  std::atomic_bool device_error_ = false;
  dxmt::mutex device_error_callbacks_mutex_;
  uint64_t next_device_error_callback_id_ = 1;
  std::unordered_map<uint64_t, std::weak_ptr<std::function<void()>>>
      device_error_callbacks_;
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
  struct PersistentResidencyEntry {
    WMT::Reference<WMT::Object> allocation;
    uint32_t ref_count = 0;
    uint64_t remove_after_seq = 0;
    bool pending_remove = false;
  };
  dxmt::mutex persistent_residency_mutex_;
  WMT::Reference<WMT::ResidencySet> persistent_residency_set_;
  bool persistent_residency_dirty_ = false;
  uint64_t persistent_residency_commit_count_ = 0;
  std::unordered_map<obj_handle_t, PersistentResidencyEntry> persistent_residency_entries_;
  std::unordered_map<obj_handle_t, bool>
      persistent_residency_cached_allocations_;
  // Strong references for allocations removed from the userspace set but not
  // yet committed to its kernel mirror. The maintenance worker guarantees an
  // idle queue flushes this list after a short coalescing delay.
  std::deque<WMT::Reference<WMT::Object>>
      persistent_residency_retired_allocations_;
  dxmt::condition_variable persistent_residency_cond_;
  bool persistent_residency_flush_requested_ = false;
  bool persistent_residency_stop_ = false;
  dxmt::thread persistent_residency_thread_;
  dxmt::mutex deferred_release_mutex_;
  uint64_t deferred_release_completed_seq_ = 0;
  std::unordered_map<uint64_t, std::vector<std::function<void()>>>
      deferred_releases_;

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

  bool HasDeviceError() const {
    return device_error_.load(std::memory_order_acquire);
  }

  void MarkDeviceError();

  uint64_t RegisterDeviceErrorCallback(
      const std::shared_ptr<std::function<void()>> &callback);

  void UnregisterDeviceErrorCallback(uint64_t callback_id);

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

  void CommitCurrentChunkForFrame(uint64_t frame_id);

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

  // Highest frame seq whose present chunk has retired, signaled on the frame
  // latency fence. d3d9's D3DPRESENT_DONOTWAIT probe peeks this to decide
  // whether the end-of-Present frame-latency throttle would block, and returns
  // D3DERR_WASSTILLDRAWING instead of entering the wait.
  uint64_t FrameLatencySignaled() { return frame_latency_fence_.signaledValue(); }

  // Block until the present chunk from max_latency frames back has retired,
  // capping how far the calling thread runs ahead of the GPU. Present pacing is
  // applied per frontend rather than in PresentBoundary; the other back ends
  // pace through their own swapchain fence or present semaphore, so d3d9 rides
  // the frame-latency fence it stamps on each present chunk.
  void WaitFrameLatency(uint64_t frame_seq) {
    if (frame_seq > max_latency_)
      frame_latency_fence_.wait(frame_seq - max_latency_);
  }

  void
  WaitCPUFence(uint64_t seq);

  void
  AddPersistentResidency(WMT::Object resource);

  // Test-only synchronized accounting used by the D3D12 residency oracle.
  std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t>
  PersistentResidencyStatsForTesting();

  // App-thread retirement: waits only for work that has actually been
  // published. This avoids pinning descriptor history to an empty next chunk.
  void
  RemovePersistentResidencyAfterCompletion(WMT::Object resource);

  // Encode/submission-thread retirement: `sequence` is the exact chunk that
  // may reference the resource.
  void
  RemovePersistentResidencyAfterCompletion(WMT::Object resource,
                                             uint64_t sequence);

  // App-thread lifetime retirement, paired with the last published sequence.
  void
  RetainUntilGpuComplete(std::function<void()> release);

  // Keep an object alive through completion of an exact encoded sequence.
  void
  RetainUntilGpuComplete(uint64_t sequence, std::function<void()> release);

  uint64_t
  FlushPersistentResidency();

#if DXMT_DX12_METAL4
  bool
  UpdateSparseTextureMappings(
      WMT::Texture texture, WMT::Heap heap,
      const WMTSparseTextureMappingOperation *operations,
      uint64_t operation_count) {
    return commandQueue.updateSparseTextureMappings(texture, heap, operations,
                                                    operation_count);
  }
#endif

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    EnsureCachedAllocationResidency(block.buffer);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateStagingBuffer1(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    EnsureCachedAllocationResidency(block.buffer);
    return {block.buffer, offset, block.gpu_address};
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    EnsureCachedAllocationResidency(block.buffer);
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    EnsureCachedAllocationResidency(block.buffer);
    return {block.buffer, offset, block.gpu_address};
  }

  AllocatedArgumentBufferSlice
  AllocateArgumentBuffer(uint64_t seq, size_t size, size_t alignment = 64) {
    if (!size)
      return {};
    auto [block, offset] = argbuf_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    EnsureCachedAllocationResidency(block.buffer);
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
