#pragma once

#include "Metal.hpp"
#include "dxmt_allocation.hpp"
#include "dxmt_capture.hpp"
#include "dxmt_command.hpp"
#include "dxmt_command_list.hpp"
#include "dxmt_context.hpp"
#include "dxmt_thread_safety.h"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_sampler.hpp"
#include "dxmt_statistics.hpp"
#include "util_noexcept.hpp"
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
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace dxmt {

enum class GpuCompletionStatus : uint8_t {
  Pending,
  Complete,
  Failed,
};

class GpuCompletionTarget {
public:
  GpuCompletionTarget() = default;
  GpuCompletionTarget(const GpuCompletionTarget &) = delete;
  GpuCompletionTarget &operator=(const GpuCompletionTarget &) = delete;
  virtual ~GpuCompletionTarget() noexcept = default;
  virtual void CompleteGpuWork(GpuCompletionStatus status) noexcept = 0;
};

class DeviceErrorTarget {
public:
  virtual ~DeviceErrorTarget() noexcept = default;
  virtual void CompleteDeviceError() noexcept = 0;
};

template <typename T> class moveonly_list {
public:
  moveonly_list(T *storage, size_t size) : storage(storage), size_(size) {
    for (unsigned i = 0; i < size_; i++) {
      new (storage + i) T();
    }
  }

  moveonly_list(const moveonly_list &copy) = delete;
  moveonly_list(moveonly_list &&move) noexcept {
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
class ResidencyOwnership;
class DeviceResidency;

template <typename Domain> class ResidencyAllocation {
public:
  WMT::Object
  object() const {
    return allocation_;
  }

  ResidencyProvenance
  provenance() const {
    return provenance_;
  }

private:
  friend class ResidencyOwnership;

  explicit ResidencyAllocation(WMT::Object allocation,
                               ResidencyProvenance provenance)
      : allocation_(allocation), provenance_(provenance) {}

  WMT::Object allocation_;
  ResidencyProvenance provenance_;
};

struct LifetimeResidencyDomain final {};
struct ReplayAllocatorResidencyDomain final {};
struct ReplayTemporaryResidencyDomain final {};

using LifetimeResidencyAllocation =
    ResidencyAllocation<LifetimeResidencyDomain>;
using ReplayAllocatorResidencyAllocation =
    ResidencyAllocation<ReplayAllocatorResidencyDomain>;
using ReplayTemporaryResidencyAllocation =
    ResidencyAllocation<ReplayTemporaryResidencyDomain>;

class ResidencyOwnership final {
public:
  static LifetimeResidencyAllocation
  Lifetime(WMT::Object allocation, ResidencyProvenance provenance = {}) {
    return LifetimeResidencyAllocation(allocation, provenance);
  }

  static ReplayAllocatorResidencyAllocation
  ReplayAllocatorBlock(WMT::Object allocation) {
    return ReplayAllocatorResidencyAllocation(
        allocation,
        ResidencyProvenance{.kind = ResidencyProvenanceKind::ReplayAllocator});
  }

  static ReplayTemporaryResidencyAllocation
  ReplayTemporary(WMT::Object allocation) {
    return ReplayTemporaryResidencyAllocation(
        allocation,
        ResidencyProvenance{.kind = ResidencyProvenanceKind::ReplayTemporary});
  }
};

class LifetimeResidencyRegistration {
public:
  ~LifetimeResidencyRegistration() noexcept;

  LifetimeResidencyRegistration(const LifetimeResidencyRegistration &) =
      delete;
  LifetimeResidencyRegistration &
  operator=(const LifetimeResidencyRegistration &) = delete;

private:
  friend class CommandQueue;

  LifetimeResidencyRegistration(
      std::shared_ptr<DeviceResidency> owner,
      LifetimeResidencyAllocation allocation);

  std::shared_ptr<DeviceResidency> owner_;
  WMT::Reference<WMT::Object> allocation_;
};

class ReplayResidencyRegistration {
public:
  ~ReplayResidencyRegistration() noexcept;

  ReplayResidencyRegistration(const ReplayResidencyRegistration &) = delete;
  ReplayResidencyRegistration &
  operator=(const ReplayResidencyRegistration &) = delete;

private:
  friend class CommandQueue;

  enum class Kind : uint8_t {
    Allocator,
    Temporary,
  };

  ReplayResidencyRegistration(
      std::shared_ptr<DeviceResidency> owner, WMT::Object allocation,
      ResidencyProvenance provenance, Kind kind);

  std::shared_ptr<DeviceResidency> owner_;
  WMT::Reference<WMT::Object> allocation_;
  Kind kind_;
};

using GpuRetainedOwner =
    std::variant<Rc<Sampler>, WMT::Reference<WMT::Resource>,
                 WMT::Reference<WMT::Buffer>,
                 std::shared_ptr<LifetimeResidencyRegistration>,
                 std::shared_ptr<ReplayResidencyRegistration>>;

/*
 * The device residency set has exactly one synchronization owner.  A dedicated
 * capability type makes that boundary visible to Clang instead of relying on
 * std::lock_guard inference through the Wine SRW-lock wrapper.
 */
class DXMT_CAPABILITY("device-residency") DeviceResidencyMutex final {
public:
  void lock() DXMT_ACQUIRE() { mutex_.lock(); }
  void unlock() DXMT_RELEASE() { mutex_.unlock(); }

private:
  dxmt::mutex mutex_;
};

class DXMT_SCOPED_CAPABILITY DeviceResidencyLock final {
public:
  explicit DeviceResidencyLock(DeviceResidencyMutex &mutex)
      DXMT_ACQUIRE(mutex)
      : mutex_(mutex) {
    mutex_.lock();
  }

  ~DeviceResidencyLock() DXMT_RELEASE() { mutex_.unlock(); }

  DeviceResidencyLock(const DeviceResidencyLock &) = delete;
  DeviceResidencyLock &operator=(const DeviceResidencyLock &) = delete;

private:
  DeviceResidencyMutex &mutex_;
};

/*
 * Device-wide backend submission owner. A D3D12 device may expose multiple
 * CommandQueue workers, but Metal residency mutations and command-buffer
 * commits that consume the shared set must form one narrow serialization
 * domain. The owner is acquired first to order snapshots across queues; the
 * PE journal mutex is then held only long enough to move a snapshot and is
 * released before any Metal call.
 */
class DXMT_CAPABILITY("residency-submission-owner")
    DeviceResidencySubmissionOwner final {
public:
  void lock() DXMT_ACQUIRE() { mutex_.lock(); }
  void unlock() DXMT_RELEASE() { mutex_.unlock(); }

private:
  dxmt::mutex mutex_;
};

class DXMT_SCOPED_CAPABILITY DeviceResidencySubmissionScope final {
public:
  explicit DeviceResidencySubmissionScope(
      DeviceResidencySubmissionOwner &owner) DXMT_ACQUIRE(owner)
      : owner_(owner) {
    owner_.lock();
  }

  ~DeviceResidencySubmissionScope() DXMT_RELEASE() { owner_.unlock(); }

  DeviceResidencySubmissionScope(const DeviceResidencySubmissionScope &) =
      delete;
  DeviceResidencySubmissionScope &
  operator=(const DeviceResidencySubmissionScope &) = delete;

private:
  DeviceResidencySubmissionOwner &owner_;
};

class DeviceResidency final {
public:
  explicit DeviceResidency(WMT::Device device);

  DeviceResidency(const DeviceResidency &) = delete;
  DeviceResidency &operator=(const DeviceResidency &) = delete;

  void AttachQueue(WMT::CommandQueue queue) DXMT_EXCLUDES(mutex_);

  std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t>
  StatsForTesting() DXMT_EXCLUDES(mutex_);

private:
  enum class Kind : uint8_t {
    Lifetime,
    ReplayAllocator,
    ReplayTemporary,
  };

  struct RetainedAllocation {
    RetainedAllocation(WMT::Object value, ResidencyProvenance source)
        : object(value), provenance(source) {}

    WMT::Reference<WMT::Object> object;
    const ResidencyProvenance provenance;
  };

  using AllocationOwner = std::shared_ptr<RetainedAllocation>;

  struct Entry {
    AllocationOwner allocation;
    uint32_t lifetime_references = 0;
    uint32_t replay_allocator_references = 0;
    uint32_t replay_temporary_references = 0;

    uint64_t referenceCount() const {
      return uint64_t(lifetime_references) +
             uint64_t(replay_allocator_references) +
             uint64_t(replay_temporary_references);
    }
  };

  struct PendingMutation {
    AllocationOwner allocation;
    bool make_resident = false;
  };

  struct SubmissionBatch {
    std::deque<AllocationOwner> additions;
    std::deque<AllocationOwner> removals;
    std::deque<AllocationOwner> retired_backend_owners;
    uint64_t generation = 0;
    bool request_residency = false;
  };

  struct SubmissionResult {
    bool succeeded = true;
    uint64_t elapsed_us = 0;
    std::vector<WMT::Reference<WMT::Object>> retired_allocations;
  };

  void Add(WMT::Object allocation, ResidencyProvenance provenance, Kind kind)
      DXMT_EXCLUDES(mutex_);
  void Remove(WMT::Object allocation, Kind kind) noexcept
      DXMT_EXCLUDES(mutex_);
  SubmissionResult ApplyForSubmission()
      DXMT_REQUIRES(submission_owner_) DXMT_EXCLUDES(mutex_);

  void AddLocked(const AllocationOwner &allocation, Kind kind)
      DXMT_REQUIRES(mutex_);
  void RemoveLocked(WMT::Object allocation, Kind kind,
                    AllocationOwner *retired) noexcept
      DXMT_REQUIRES(mutex_);
  void QueueDesiredStateLocked(const AllocationOwner &allocation,
                               bool make_resident)
      DXMT_REQUIRES(mutex_);
  SubmissionBatch PrepareSubmissionBatchLocked() DXMT_REQUIRES(mutex_);

  DeviceResidencyMutex mutex_;
  // The documented ordering -- backend submission owner first, PE journal
  // mutex only inside it -- is a real deadlock constraint, so state it where
  // the compiler can check it rather than only in prose. Clang verifies member
  // lock ordering under -Wthread-safety-beta; a path that takes these in
  // reverse order fails the build instead of deadlocking at runtime.
  DeviceResidencySubmissionOwner submission_owner_ DXMT_ACQUIRED_BEFORE(mutex_);
  WMT::Reference<WMT::ResidencySet> set_;
  std::unordered_map<obj_handle_t, Entry> entries_ DXMT_GUARDED_BY(mutex_);
  std::unordered_map<obj_handle_t, PendingMutation> pending_mutations_
      DXMT_GUARDED_BY(mutex_);
  std::unordered_map<obj_handle_t, AllocationOwner> backend_entries_
      DXMT_GUARDED_BY(mutex_);
  uint64_t desired_generation_ DXMT_GUARDED_BY(mutex_) = 0;
  bool residency_requested_ DXMT_GUARDED_BY(mutex_) = false;
  std::atomic_uint64_t applied_generation_ = 0;
  std::atomic_uint64_t commit_count_ = 0;

  friend class LifetimeResidencyRegistration;
  friend class ReplayResidencyRegistration;
  friend class CommandQueue;
};

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
  addCompletionTarget(std::shared_ptr<GpuCompletionTarget> target) {
    if (!target)
      return;
    std::lock_guard<dxmt::mutex> lock(completion_targets_mutex_);
    completion_targets.push_back(std::move(target));
  }

  size_t
  completionTargetCount() {
    std::lock_guard<dxmt::mutex> lock(completion_targets_mutex_);
    return completion_targets.size();
  }

  void
  retainTimestampQueries(std::vector<Rc<TimestampQuery>> queries) {
    for (auto &query : queries)
      retained_timestamp_queries.push_back(std::move(query));
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
  std::vector<std::shared_ptr<GpuCompletionTarget>> completion_targets;
  dxmt::mutex completion_targets_mutex_;
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
  std::vector<Rc<TimestampQuery>> retained_timestamp_queries;
  std::vector<WMT::Reference<WMT::Object>>
      retired_residency_allocations;
  
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
    std::vector<std::shared_ptr<GpuCompletionTarget>> completion_targets;
    {
      std::lock_guard<dxmt::mutex> lock(completion_targets_mutex_);
      completion_targets.swap(this->completion_targets);
    }
    for (const auto &target : completion_targets)
      target->CompleteGpuWork(GpuCompletionStatus::Failed);
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
    retained_timestamp_queries.clear();
    retired_residency_allocations.clear();
  }
};

class CommandQueue {

private:
  void CommitChunkInternal(CommandChunk &chunk);

  void FlushFinalFrameStatistics();

  void ReleaseCompletedGpuOwners(uint64_t completed_seq);

  void DrainRetainedGpuOwners();

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
  dxmt::mutex device_error_targets_mutex_;
  uint64_t next_device_error_target_id_ = 1;
  std::unordered_map<uint64_t, std::weak_ptr<DeviceErrorTarget>>
      device_error_targets_;
  dxmt::mutex readback_mutex_;
  dxmt::condition_variable readback_cond_;
  std::vector<DiagnosticReadback> pending_readbacks_;
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
  std::shared_ptr<DeviceResidency> device_residency_;
  dxmt::mutex retained_gpu_owner_mutex_;
  uint64_t retained_gpu_owner_completed_seq_ = 0;
  std::unordered_map<uint64_t, std::vector<GpuRetainedOwner>>
      deferred_retained_owners_;

  obj_handle_t shared_event_listener;
  dxmt::thread event_listener_thread;

  friend class CommandChunk;
  friend class LifetimeResidencyRegistration;
  friend class ReplayResidencyRegistration;
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

  CommandQueue(WMT::Device device,
               std::shared_ptr<DeviceResidency> device_residency);

  ~CommandQueue() noexcept;

  bool HasDeviceError() const {
    return device_error_.load(std::memory_order_acquire);
  }

  void MarkDeviceError();

  uint64_t RegisterDeviceErrorTarget(
      const std::shared_ptr<DeviceErrorTarget> &target);

  void UnregisterDeviceErrorTarget(uint64_t target_id);

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

  std::shared_ptr<LifetimeResidencyRegistration>
  RegisterLifetimeResidency(LifetimeResidencyAllocation allocation);

  std::shared_ptr<ReplayResidencyRegistration>
  RegisterReplayAllocatorResidency(
      ReplayAllocatorResidencyAllocation allocation);

  void RetainReplayTemporaryResidencyUntilGpuComplete(
      ReplayTemporaryResidencyAllocation allocation, uint64_t sequence);

  // Test-only synchronized accounting used by the D3D12 residency oracle.
  std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t>
  PersistentResidencyStatsForTesting();

  // App-thread lifetime retention, paired with the last published sequence.
  void
  RetainGpuOwner(GpuRetainedOwner owner);

  // Keep an object alive through completion of an exact encoded sequence.
  void
  RetainGpuOwner(uint64_t sequence, GpuRetainedOwner owner);

  void
  RetainGpuOwners(uint64_t sequence,
                  std::vector<GpuRetainedOwner> owners);

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
    if (!block.residency)
      block.residency = RegisterReplayAllocatorResidency(
          ResidencyOwnership::ReplayAllocatorBlock(block.buffer));
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateStagingBuffer1(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(ready_for_encode, cpu_coherent.signaledValue(), size, alignment);
    if (!block.residency)
      block.residency = RegisterReplayAllocatorResidency(
          ResidencyOwnership::ReplayAllocatorBlock(block.buffer));
    return {block.buffer, offset, block.gpu_address};
  }

  std::pair<WMT::Buffer, uint64_t>
  AllocateTempBuffer(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    if (!block.residency)
      block.residency = RegisterReplayAllocatorResidency(
          ResidencyOwnership::ReplayAllocatorBlock(block.buffer));
    return {block.buffer, offset};
  }

  AllocatedTempBufferSlice
  AllocateTempBuffer1(uint64_t seq, size_t size, size_t alignment) {
    auto [block, offset] = copy_temp_allocator.allocate(seq, cpu_coherent.signaledValue(), size, alignment);
    if (!block.residency)
      block.residency = RegisterReplayAllocatorResidency(
          ResidencyOwnership::ReplayAllocatorBlock(block.buffer));
    return {block.buffer, offset, block.gpu_address};
  }

  AllocatedArgumentBufferSlice
  AllocateArgumentBuffer(uint64_t seq, size_t size, size_t alignment = 64) {
    if (!size)
      return {};
    // Fail-closed ownership: host-mapped argbuf slices require an active
    // encode generation so the Metal buffer is retained until GPU complete.
    if (!argument_encoding_ctx.hasActiveCommandBufferGeneration()) {
      ERR("DXMT AllocateArgumentBuffer: no active command-buffer generation "
          "size=",
          size, " seq=", seq);
      return {};
    }
    auto [block, offset] = argbuf_allocator.allocate(
        seq, cpu_coherent.signaledValue(), size, alignment,
        /*retain_pin=*/true);
    // Snapshot under the allocation's returned Block& before any other queue
    // work can race free_blocks/reuse. Slice holds a retaining Reference so
    // the MTLBuffer (and host mapping) stays live for write()/encode.
    WMT::Reference<WMT::Buffer> buffer_ref(block.buffer);
    const uint64_t gpu_address = block.gpu_address;
    void *const block_mapped = block.mapped_address;
    if (!buffer_ref) {
      ERR("DXMT AllocateArgumentBuffer: failed to create GPU buffer size=",
          size, " seq=", seq);
      return {};
    }
    if (!block.residency)
      block.residency = RegisterReplayAllocatorResidency(
          ResidencyOwnership::ReplayAllocatorBlock(buffer_ref));
    // GPTK-style CB retain (vector + chunk lifetime only — not residency set
    // mutation). Host write safety also relies on slice's retaining Reference
    // and the block-owned device residency registration above.
    if (!argument_encoding_ctx.tryRetainResourceForCurrentCommandBuffer(
            buffer_ref)) {
      ERR("DXMT AllocateArgumentBuffer: generation pin failed size=", size,
          " seq=", seq);
      return {};
    }
    void *mapped = nullptr;
    bool needs_flush = false;
    if constexpr (sizeof(void *) == 4) {
      auto [shadow_block, shadow_offset] = argbuf_shadow_allocator.allocate(
          seq, cpu_coherent.signaledValue(), size, alignment,
          /*retain_pin=*/true);
      mapped = ptr_add(shadow_block.ptr, shadow_offset);
      needs_flush = true;
    } else {
      mapped = ptr_add(block_mapped, offset);
    }
    if (!mapped) {
      ERR("DXMT AllocateArgumentBuffer: null host mapping size=", size,
          " seq=", seq, " offset=", offset);
      return {};
    }
    return {mapped, std::move(buffer_ref), offset, gpu_address, size,
            needs_flush};
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
