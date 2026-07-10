#include "dxmt_command_queue.hpp"
#include "Metal.hpp"
#include "dxmt_apitrace.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_perf_stats.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_lifecycle_telemetry.hpp"
#include "util_win32_compat.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

#define ASYNC_ENCODING 1

namespace dxmt {

static bool
DxmtQueueDiagEnabledEnv(const char *name) {
  auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" ||
         value == "on" || value == "trace";
}

static bool
DxmtQueueDiagEnabled() {
  static const bool enabled = DxmtQueueDiagEnabledEnv("DXMT_DIAG_DXMT_QUEUE");
  return enabled;
}

static bool
DxmtQueueLifecycleEnabled() {
  static const bool enabled =
      DxmtQueueDiagEnabledEnv("DXMT_DIAG_QUEUE_LIFECYCLE");
  return enabled;
}

static void
DxmtQueueLifecycleLog(const CommandQueue *queue, const char *stage,
                      uint64_t pair_id, uint64_t queue_pair_id,
                      uint64_t frame_id, uint64_t chunk_id,
                      obj_handle_t command_buffer, uint64_t wait_sequence,
                      uint64_t dependency_sequence) {
  if (!DxmtQueueLifecycleEnabled())
    return;
  WARN_FILE_ONLY("DXMT queue lifecycle:"
       " eventSeq=", lifecycle::nextEventSequence(),
       " pairSeq=", pair_id,
       " queuePairSeq=", queue_pair_id,
       " queue=", reinterpret_cast<uintptr_t>(queue),
       " thread=", lifecycle::threadId(),
       " stage=", stage,
       " frame=", frame_id,
       " chunk=", chunk_id,
       " cmdbuf=", command_buffer,
       " waitSeq=", wait_sequence,
       " dependencySeq=", dependency_sequence);
}

static bool
DxmtQueueDiagVerboseEnabled() {
  static const bool enabled =
      DxmtQueueDiagEnabledEnv("DXMT_DIAG_DXMT_QUEUE_VERBOSE");
  return enabled;
}

static bool
DxmtQueueDiagShouldLog(std::atomic<uint32_t> &counter) {
  if (!DxmtQueueDiagVerboseEnabled())
    return false;

  const auto index = counter.fetch_add(1, std::memory_order_relaxed);
  return index < 512 || (index % 256) == 0;
}

static bool
DxmtQueueDiagShouldLogIdleWait(std::atomic<uint32_t> &counter) {
  if (!DxmtQueueDiagEnabled())
    return false;

  const auto index = counter.fetch_add(1, std::memory_order_relaxed);
  return index < 64 || (index % 256) == 0;
}

static double
DxmtQueueDiagDurationMs(clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

static uint64_t
DxmtQueueDiagDurationUs(clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

static clock::duration
DxmtQueueDiagElapsed(clock::time_point start, clock::time_point end) {
  if (start == clock::time_point{} || end == clock::time_point{} || end < start)
    return clock::duration{};
  return end - start;
}

static double
DxmtQueueDiagElapsedMs(clock::time_point start, clock::time_point end) {
  return DxmtQueueDiagDurationMs(DxmtQueueDiagElapsed(start, end));
}

static double
DxmtQueueDiagNsToMs(uint64_t ns) {
  return static_cast<double>(ns) / 1000000.0;
}

constexpr uint64_t kPersistentResidencyInitialCapacity = 4096;
constexpr auto kPersistentResidencyFlushDelay = std::chrono::milliseconds(1);

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue->AllocateCommandData(size, alignment);
}

void
CommandChunk::encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc) {
  enc.$$setEncodingContext(chunk_id, frame_);
  enc.beginCommandBufferResourceLifetime(
      cmdbuf, retained_metal_resources, retained_metal_resource_handles);
  auto &statistics = enc.currentFrameStatistics();
  CommandBufferDiagnosticInfo diagnostic = {};
  diagnostic.frame_id = frame_;
  diagnostic.chunk_id = chunk_id;
  diagnostic.resource_initializer_event_id = resource_initializer_event_id;
  const auto barrier_only_before = statistics.blit_barrier_only_pass_count;

  auto t0 = clock::now();
  list_enc.execute(enc);
  encode_prepare_end_time = clock::now();
  attached_cmdbuf = cmdbuf;
  auto t1 = encode_prepare_end_time;
  readback = enc.flushCommands(cmdbuf, chunk_id, chunk_event_id, &diagnostic);
  enc.endCommandBufferResourceLifetime();
  auto t2 = clock::now();

  diagnostic.sparse_mapping_call_count =
      sparse_mapping_diagnostic.sparse_mapping_call_count;
  diagnostic.sparse_mapping_operation_count =
      sparse_mapping_diagnostic.sparse_mapping_operation_count;
  diagnostic.sparse_mapping_map_count =
      sparse_mapping_diagnostic.sparse_mapping_map_count;
  diagnostic.sparse_mapping_unmap_count =
      sparse_mapping_diagnostic.sparse_mapping_unmap_count;
  diagnostic.sparse_mapping_failure_count =
      sparse_mapping_diagnostic.sparse_mapping_failure_count;
  diagnostic.sparse_mapping_barrier_count =
      sparse_mapping_diagnostic.sparse_mapping_barrier_count;
  diagnostic.sparse_resource_identity =
      sparse_mapping_diagnostic.sparse_resource_identity;
  diagnostic.sparse_texture_handle =
      sparse_mapping_diagnostic.sparse_texture_handle;
  diagnostic.sparse_heap_handle = sparse_mapping_diagnostic.sparse_heap_handle;
  diagnostic.sparse_gpu_resource_id =
      sparse_mapping_diagnostic.sparse_gpu_resource_id;
  diagnostic.sparse_mapping_generation_begin =
      sparse_mapping_diagnostic.sparse_mapping_generation_begin;
  diagnostic.sparse_mapping_generation_end =
      sparse_mapping_diagnostic.sparse_mapping_generation_end;

  diagnostic.barrier_only_pass_count =
      statistics.blit_barrier_only_pass_count - barrier_only_before;
  perf_input_encoder_count = diagnostic.input_encoder_count;
  perf_encoded_encoder_count = diagnostic.encoded_encoder_count;
  perf_render_encoder_count = diagnostic.render_encoder_count;
  perf_compute_encoder_count = diagnostic.compute_encoder_count;
  perf_blit_encoder_count = diagnostic.blit_encoder_count;
  const auto barrier_marker =
      env::getEnvVar("DXMT_TEST_D3D12_BARRIER_ONLY_MARKER");
  if (!barrier_marker.empty()) {
    if (FILE *marker = fopen(barrier_marker.c_str(), "a")) {
      fprintf(marker, "barrierOnly=%u\n",
              diagnostic.barrier_only_pass_count);
      fclose(marker);
    }
  }
  const uint64_t d3d_sequence = dxmt::apitrace::d3d_enabled()
                                    ? dxmt::apitrace::current_d3d_sequence()
                                    : chunk_id;
  diagnostic.d3d_sequence_begin = d3d_sequence;
  diagnostic.d3d_sequence_end = d3d_sequence;
#if DXMT_DX12_METAL4
  WMTCommandBufferDiagnosticInfo wmt_diagnostic = {};
  wmt_diagnostic.frame_id = diagnostic.frame_id;
  wmt_diagnostic.chunk_id = diagnostic.chunk_id;
  wmt_diagnostic.d3d_sequence_begin = diagnostic.d3d_sequence_begin;
  wmt_diagnostic.d3d_sequence_end = diagnostic.d3d_sequence_end;
  wmt_diagnostic.resource_initializer_event_id =
      diagnostic.resource_initializer_event_id;
  wmt_diagnostic.input_encoder_count = diagnostic.input_encoder_count;
  wmt_diagnostic.encoded_encoder_count = diagnostic.encoded_encoder_count;
  wmt_diagnostic.render_encoder_count = diagnostic.render_encoder_count;
  wmt_diagnostic.compute_encoder_count = diagnostic.compute_encoder_count;
  wmt_diagnostic.blit_encoder_count = diagnostic.blit_encoder_count;
  wmt_diagnostic.other_encoder_count = diagnostic.other_encoder_count;
  wmt_diagnostic.present_encoder_count = diagnostic.present_encoder_count;
  wmt_diagnostic.clear_encoder_count = diagnostic.clear_encoder_count;
  wmt_diagnostic.resolve_encoder_count = diagnostic.resolve_encoder_count;
  wmt_diagnostic.scaler_encoder_count = diagnostic.scaler_encoder_count;
  wmt_diagnostic.signal_event_count = diagnostic.signal_event_count;
  wmt_diagnostic.wait_event_count = diagnostic.wait_event_count;
  wmt_diagnostic.timestamp_encoder_count = diagnostic.timestamp_encoder_count;
  wmt_diagnostic.barrier_only_pass_count = diagnostic.barrier_only_pass_count;
  wmt_diagnostic.fence_wait_count = diagnostic.fence_wait_count;
  wmt_diagnostic.fence_update_count = diagnostic.fence_update_count;
  wmt_diagnostic.prior_local_fence_wait_count =
      diagnostic.prior_local_fence_wait_count;
  wmt_diagnostic.future_local_fence_wait_count =
      diagnostic.future_local_fence_wait_count;
  wmt_diagnostic.same_encoder_fence_wait_count =
      diagnostic.same_encoder_fence_wait_count;
  wmt_diagnostic.external_fence_wait_count =
      diagnostic.external_fence_wait_count;
  wmt_diagnostic.repeated_fence_update_count =
      diagnostic.repeated_fence_update_count;
  wmt_diagnostic.render_valid_cross_stage_count =
      diagnostic.render_valid_cross_stage_count;
  wmt_diagnostic.render_same_stage_wait_count =
      diagnostic.render_same_stage_wait_count;
  wmt_diagnostic.render_reverse_stage_wait_count =
      diagnostic.render_reverse_stage_wait_count;
  wmt_diagnostic.local_fence_id_count = diagnostic.local_fence_id_count;
  wmt_diagnostic.bound_fence_slot_count = diagnostic.bound_fence_slot_count;
  wmt_diagnostic.encoded_fence_wait_count =
      diagnostic.encoded_fence_wait_count;
  wmt_diagnostic.skipped_external_fence_wait_count =
      diagnostic.skipped_external_fence_wait_count;
  wmt_diagnostic.fence_edge_count = diagnostic.fence_edge_count;
  wmt_diagnostic.fence_edge_overflow_count =
      diagnostic.fence_edge_overflow_count;
  wmt_diagnostic.encoder_diagnostic_count =
      diagnostic.encoder_diagnostic_count;
  wmt_diagnostic.encoder_diagnostic_overflow_count =
      diagnostic.encoder_diagnostic_overflow_count;
  wmt_diagnostic.sparse_mapping_call_count =
      diagnostic.sparse_mapping_call_count;
  wmt_diagnostic.sparse_mapping_operation_count =
      diagnostic.sparse_mapping_operation_count;
  wmt_diagnostic.sparse_mapping_map_count = diagnostic.sparse_mapping_map_count;
  wmt_diagnostic.sparse_mapping_unmap_count =
      diagnostic.sparse_mapping_unmap_count;
  wmt_diagnostic.sparse_mapping_failure_count =
      diagnostic.sparse_mapping_failure_count;
  wmt_diagnostic.sparse_mapping_barrier_count =
      diagnostic.sparse_mapping_barrier_count;
  wmt_diagnostic.sparse_resource_identity =
      diagnostic.sparse_resource_identity;
  wmt_diagnostic.sparse_texture_handle = diagnostic.sparse_texture_handle;
  wmt_diagnostic.sparse_heap_handle = diagnostic.sparse_heap_handle;
  wmt_diagnostic.sparse_gpu_resource_id = diagnostic.sparse_gpu_resource_id;
  wmt_diagnostic.sparse_mapping_generation_begin =
      diagnostic.sparse_mapping_generation_begin;
  wmt_diagnostic.sparse_mapping_generation_end =
      diagnostic.sparse_mapping_generation_end;
  wmt_diagnostic.sparse_access_count = diagnostic.sparse_access_count;
  wmt_diagnostic.sparse_access_flags = diagnostic.sparse_access_flags;
  wmt_diagnostic.sparse_access_level = diagnostic.sparse_access_level;
  wmt_diagnostic.sparse_access_slice = diagnostic.sparse_access_slice;
  wmt_diagnostic.sparse_access_descriptor =
      diagnostic.sparse_access_descriptor;
  wmt_diagnostic.sparse_access_resource_identity =
      diagnostic.sparse_access_resource_identity;
  wmt_diagnostic.sparse_access_texture_handle =
      diagnostic.sparse_access_texture_handle;
  wmt_diagnostic.sparse_access_gpu_resource_id =
      diagnostic.sparse_access_gpu_resource_id;
  wmt_diagnostic.sparse_access_encoder_id =
      diagnostic.sparse_access_encoder_id;
  for (uint32_t i = 0;
       i < diagnostic.fence_edge_count &&
       i < WMT_COMMAND_BUFFER_FENCE_EDGE_CAPACITY;
       i++) {
    const auto &src = diagnostic.fence_edges[i];
    auto &dst = wmt_diagnostic.fence_edges[i];
    dst.producer_id = src.producer_id;
    dst.consumer_id = src.consumer_id;
    dst.producer_index = src.producer_index;
    dst.consumer_index = src.consumer_index;
    dst.slot = src.slot;
    dst.flags = src.flags;
  }
  for (uint32_t i = 0;
       i < diagnostic.encoder_diagnostic_count &&
       i < WMT_COMMAND_BUFFER_ENCODER_DIAGNOSTIC_CAPACITY;
       i++) {
    const auto &src = diagnostic.encoders[i];
    auto &dst = wmt_diagnostic.encoders[i];
    dst.encoder_id = src.encoder_id;
    dst.vertex_encoder_id = src.vertex_encoder_id;
    dst.wait_hash = src.wait_hash;
    dst.update_hash = src.update_hash;
    dst.pipeline_handle = src.pipeline_handle;
    dst.argument_buffer_handle = src.argument_buffer_handle;
    dst.argument_buffer_offset = src.argument_buffer_offset;
    dst.argument_buffer_size = src.argument_buffer_size;
    dst.primary_resource = src.primary_resource;
    dst.secondary_resource = src.secondary_resource;
    dst.resource_plan_hash = src.resource_plan_hash;
    dst.encoder_index = src.encoder_index;
    dst.type = src.type;
    dst.wait_count = src.wait_count;
    dst.update_count = src.update_count;
    dst.flags = src.flags;
    dst.resource_plan_count = src.resource_plan_count;
  }
  cmdbuf.setDiagnosticInfo(wmt_diagnostic);
  const auto diagnostic_marker =
      env::getEnvVar("DXMT_TEST_COMMAND_BUFFER_DIAGNOSTIC_MARKER");
  if (!diagnostic_marker.empty()) {
    if (FILE *marker = fopen(diagnostic_marker.c_str(), "a")) {
      fprintf(marker, "%u %u %u %u %u %u %u %u\n",
              diagnostic.input_encoder_count,
              diagnostic.encoded_encoder_count,
              diagnostic.blit_encoder_count,
              diagnostic.barrier_only_pass_count,
              diagnostic.fence_wait_count,
              diagnostic.fence_update_count,
              diagnostic.external_fence_wait_count,
              diagnostic.skipped_external_fence_wait_count);
      fclose(marker);
    }
  }
#endif

  auto execute_elapsed = t1 - t0;
  auto flush_elapsed = t2 - t1;
  statistics.encode_prepare_interval += execute_elapsed;
  statistics.encode_flush_interval += flush_elapsed;
};

CommandQueue::CommandQueue(WMT::Device device) :
    apitrace_enabled_(dxmt::apitrace::enabled()),
    encodeThread([this]() { this->EncodingThread(); }),
    finishThread([this]() { this->WaitForFinishThread(); }),
    readbackThread([this]() { this->ReadbackThread(); }),
    device(device),
    commandQueue(device.newCommandQueue(kCommandChunkCount)),
    // Growth hint only; the residency set can grow as allocations are added.
    persistent_residency_set_(device.newResidencySet(kPersistentResidencyInitialCapacity)),
    persistent_residency_thread_([this]() { this->PersistentResidencyThread(); }),
    shared_event_listener(SharedEventListener_create()),
    event_listener_thread([this]() { SharedEventListener_start(this->shared_event_listener); }),
    staging_allocator({
        device, WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                    WMTResourceStorageModeManaged, false
    }),
    copy_temp_allocator({device, WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}),
    argbuf_allocator({
        device,
        WMTResourceHazardTrackingModeUntracked | WMTResourceCPUCacheModeDefaultCache | WMTResourceStorageModeShared,
        false
    }),
    argbuf_shadow_allocator({}),
    cpu_command_allocator({}),
    reftracker_storage_allocator({}),
    cmd_library(device),
    argument_encoding_ctx(*this, device, cmd_library),
    initializer(device) {
  auto retire_cached_block =
      [this](const auto &block, uint64_t sequence) {
        this->RetireCachedAllocationResidency(block.buffer, sequence);
      };
  staging_allocator.setReleaseCallback(retire_cached_block);
  copy_temp_allocator.setReleaseCallback(retire_cached_block);
  argbuf_allocator.setReleaseCallback(retire_cached_block);
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.queue = this;
    chunk.reset();
  };
  const auto initial_frame_time = clock::now();
  statistics.at(frame_count).begin_time = initial_frame_time;
  perf::setCurrentFrameStatistics(&statistics.at(frame_count));
  event = device.newSharedEvent();
  persistent_residency_set_.requestResidency();
  commandQueue.addResidencySet(persistent_residency_set_);

}

CommandQueue::~CommandQueue() {
  auto pool = WMT::MakeAutoreleasePool();
  TRACE("Destructing command queue");
  stopped.store(true, std::memory_order_release);
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_all();
  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_all();
  SharedEventListener_destroy(shared_event_listener);
  encodeThread.join();
  finishThread.join();
  readback_cond_.notify_all();
  readbackThread.join();
  staging_allocator.free_blocks(UINT64_MAX);
  copy_temp_allocator.free_blocks(UINT64_MAX);
  argbuf_allocator.free_blocks(UINT64_MAX);
  argument_encoding_ctx.RetirePersistentResidency(
      cpu_coherent.signaledValue());
  FlushPersistentResidency();
  {
    std::lock_guard<dxmt::mutex> lock(persistent_residency_mutex_);
    persistent_residency_stop_ = true;
  }
  persistent_residency_cond_.notify_all();
  persistent_residency_thread_.join();
  FlushPersistentResidency();
  FlushFinalFrameStatistics();
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.reset();
  };
  DrainDeferredReleases();
  event_listener_thread.join();
  if (apitrace_enabled_)
    dxmt::apitrace::shutdown();
  perf::flushFinal(frame_count);
  if (perf::currentFrameStatistics() == &statistics.at(frame_count))
    perf::setCurrentFrameStatistics(nullptr);
  TRACE("Destructed command queue");
}

void
CommandQueue::MarkDeviceError() {
  if (device_error_.exchange(true, std::memory_order_acq_rel))
    return;

  std::vector<std::shared_ptr<std::function<void()>>> callbacks;
  {
    std::lock_guard<dxmt::mutex> lock(device_error_callbacks_mutex_);
    callbacks.reserve(device_error_callbacks_.size());
    for (auto &[callback_id, weak_callback] : device_error_callbacks_) {
      (void)callback_id;
      if (auto callback = weak_callback.lock())
        callbacks.push_back(std::move(callback));
    }
    device_error_callbacks_.clear();
  }

  for (const auto &callback : callbacks)
    (*callback)();
}

uint64_t
CommandQueue::RegisterDeviceErrorCallback(
    const std::shared_ptr<std::function<void()>> &callback) {
  if (!callback)
    return 0;

  bool invoke_now = false;
  uint64_t callback_id = 0;
  {
    std::lock_guard<dxmt::mutex> lock(device_error_callbacks_mutex_);
    if (HasDeviceError())
      invoke_now = true;
    else {
      callback_id = next_device_error_callback_id_++;
      device_error_callbacks_.emplace(callback_id, callback);
    }
  }

  if (invoke_now)
    (*callback)();
  return callback_id;
}

void
CommandQueue::UnregisterDeviceErrorCallback(uint64_t callback_id) {
  if (!callback_id)
    return;
  std::lock_guard<dxmt::mutex> lock(device_error_callbacks_mutex_);
  device_error_callbacks_.erase(callback_id);
}

void
CommandQueue::FlushFinalFrameStatistics() {
  if (!perf::enabled())
    return;

  auto &frame = statistics.at(frame_count);
  const bool has_work =
      frame.command_buffer_count || frame.frame_execute_command_lists_interval != clock::duration{} ||
      frame.frame_queue_signal_interval != clock::duration{} ||
      frame.frame_queue_wait_interval != clock::duration{} ||
      frame.frame_cmdlist_record_interval != clock::duration{} ||
      frame.flush_chunk_count || frame.flush_total_interval != clock::duration{};
  if (!has_work)
    return;

  const auto boundary_time = clock::now();
  frame.frame_wall_interval = DxmtQueueDiagElapsed(frame.begin_time, boundary_time);
  statistics.compute(frame_count);
  const auto frame_wall_us = frame.begin_time == clock::time_point{}
                                 ? 0
                                 : DxmtQueueDiagDurationUs(boundary_time - frame.begin_time);
  const auto final_frame = frame_count + 1;
  perf::recordFrameBoundary(final_frame, frame, statistics.average(), frame_wall_us);
  frame_count = final_frame;
}

void
CommandQueue::CommitCurrentChunk() {
  CommitCurrentChunkForFrame(frame_count);
}

void
CommandQueue::CommitCurrentChunkForFrame(uint64_t frame_id) {
  auto &frame_statistics = statistics.at(frame_id);
  const auto lifecycle_pair_id = lifecycle::nextPairSequence();
  const auto queue_lifecycle_id =
      lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
  const auto pending_chunk_id =
      ready_for_encode.load(std::memory_order_relaxed);
  DxmtQueueLifecycleLog(this, "publish.enter", lifecycle_pair_id,
                        queue_lifecycle_id, frame_id, pending_chunk_id, 0,
                        chunk_ongoing.load(std::memory_order_relaxed),
                        cpu_coherent.signaledValue());
#if ASYNC_ENCODING
  auto t0 = clock::now();
  for (;;) {
    auto ongoing = chunk_ongoing.load(std::memory_order_acquire);
    while (ongoing >= kCommandChunkCount - 1) {
      const auto wait_pair = lifecycle::nextPairSequence();
      const auto wait_queue_pair =
          lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
      DxmtQueueLifecycleLog(this, "publish.backpressure-wait.enter",
                            wait_pair, wait_queue_pair, frame_id,
                            pending_chunk_id, 0, ongoing,
                            cpu_coherent.signaledValue());
      chunk_ongoing.wait(ongoing, std::memory_order_acquire);
      DxmtQueueLifecycleLog(
          this, "publish.backpressure-wait.leave", wait_pair,
          wait_queue_pair, frame_id, pending_chunk_id, 0, ongoing,
          cpu_coherent.signaledValue());
      ongoing = chunk_ongoing.load(std::memory_order_acquire);
    }
    if (chunk_ongoing.compare_exchange_weak(
            ongoing, ongoing + 1, std::memory_order_acq_rel,
            std::memory_order_acquire))
      break;
  }
  auto t1 = clock::now();
  frame_statistics.commit_interval += (t1 - t0);
#endif

  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % kCommandChunkCount];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.lifecycle_pair_id = lifecycle_pair_id;
  chunk.queue_lifecycle_id = queue_lifecycle_id;
  chunk.frame_ = frame_id;
  chunk.frame_begin_time = frame_statistics.begin_time;
  chunk.publish_time = clock::now();
  chunk.resource_initializer_event_id = initializer.flushToWait();
  frame_statistics.command_buffer_count++;
  static std::atomic<uint32_t> commit_log_count = 0;
  if (DxmtQueueDiagShouldLog(commit_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitCurrentChunk publish"
         " chunk=", chunk_id,
         " slot=", chunk_id % kCommandChunkCount,
         " frame=", chunk.frame_,
         " event=", chunk.chunk_event_id,
         " initEvent=", chunk.resource_initializer_event_id,
         " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
         " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
         " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
         " coherent=", cpu_coherent.signaledValue());
  }
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();
  DxmtQueueLifecycleLog(this, "publish.leave", lifecycle_pair_id,
                        queue_lifecycle_id, chunk.frame_, chunk.chunk_id, 0,
                        ready_for_encode.load(std::memory_order_relaxed),
                        chunk.chunk_event_id);
  if (DxmtQueueDiagShouldLog(commit_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitCurrentChunk notified"
         " chunk=", chunk_id,
         " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
         " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
         " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
         " coherent=", cpu_coherent.signaledValue());
  }

#else
  ready_for_encode.fetch_add(1, std::memory_order_relaxed);
  DxmtQueueLifecycleLog(this, "publish.leave", lifecycle_pair_id,
                        queue_lifecycle_id, chunk.frame_, chunk.chunk_id, 0,
                        ready_for_encode.load(std::memory_order_relaxed),
                        chunk.chunk_event_id);
  CommitChunkInternal(chunk);
#endif

}

void
CommandQueue::PresentBoundary() {
  const auto frame_begin_time = statistics.at(frame_count).begin_time;
  const auto boundary_time = clock::now();
  // Present-to-Present wall time of the frame that just ended. Guarded because
  // begin_time is only stamped after the first boundary, so frame 1 would
  // otherwise contribute time-since-boot. Recorded into the ending frame's ring
  // slot so it feeds the windowed average like the other per-frame intervals.
  const auto frame_wall = DxmtQueueDiagElapsed(frame_begin_time, boundary_time);
  statistics.at(frame_count).frame_wall_interval = frame_wall;
  statistics.compute(frame_count);
  if (DxmtQueueDiagEnabled()) {
    const auto &frame = statistics.at(frame_count);
    const auto &average = statistics.average();
    WARN_FILE_ONLY("DXMT queue diagnostic: FrameBoundary"
         " frame=", frame_count,
         " cmdbufs=", frame.command_buffer_count,
         " avgCmdbufs=", average.command_buffer_count,
         " renderPasses=", frame.render_pass_count,
         " renderOpt=", frame.render_pass_optimized,
         " renderCmd=", frame.render_command_count,
         " renderDraw=", frame.render_draw_count,
         " renderIndexed=", frame.render_indexed_draw_count,
         " renderIndirect=", frame.render_indirect_draw_count,
         " renderMesh=", frame.render_mesh_draw_count,
         " renderTile=", frame.render_tile_dispatch_count,
         " renderColorLoad=", frame.render_color_load_count,
         " renderColorStore=", frame.render_color_store_count,
         " renderDepthLoad=", frame.render_depth_load_count,
         " renderDepthStore=", frame.render_depth_store_count,
         " renderStencilLoad=", frame.render_stencil_load_count,
         " renderStencilStore=", frame.render_stencil_store_count,
         " renderAttachments=", frame.render_attachment_count,
         " renderAttachmentReuse=", frame.render_attachment_reuse_count,
         " renderAttachmentReload=", frame.render_attachment_reload_count,
         " renderStoreThenLoad=", frame.render_attachment_store_then_load_count,
         " switchR2C=", frame.encoder_switch_render_compute_count,
         " switchC2R=", frame.encoder_switch_compute_render_count,
         " switchOther=", frame.encoder_switch_to_other_count,
         " computePasses=", frame.compute_pass_count,
         " computeOpt=", frame.compute_pass_optimized,
         " computePassDispatch=", frame.compute_pass_with_dispatch_count,
         " computePassNoDispatch=", frame.compute_pass_without_dispatch_count,
         " computeCommands=", frame.compute_command_count,
         " computeDispatch=", frame.compute_dispatch_count,
         " computeDispatchIndirect=", frame.compute_dispatch_indirect_count,
         " computeBarriers=", frame.compute_memory_barrier_count,
         " computePSO=", frame.compute_pso_bind_count,
         " computeSetBuf=", frame.compute_set_buffer_count,
         " computeSetTex=", frame.compute_set_texture_count,
         " computeSetBytes=", frame.compute_set_bytes_count,
         " computeUseRes=", frame.compute_use_resource_count,
         " computeWaitFence=", frame.compute_fence_wait_count,
         " computeUpdateFence=", frame.compute_fence_update_count,
         " blitPasses=", frame.blit_pass_count,
         " blitPassOptimized=", frame.blit_pass_optimized,
         " blitPassCmd=", frame.blit_pass_with_commands_count,
         " blitPassEmpty=", frame.blit_pass_empty_count,
         " blitPassDeferredFenceOnly=", frame.blit_pass_deferred_fence_only_count,
         " blitPassMergedFenceOnly=", frame.blit_pass_merged_fence_only_count,
         " blitPassWaitFence=", frame.blit_pass_with_fence_wait_count,
         " blitPassUpdateFence=", frame.blit_pass_with_fence_update_count,
         " blitFenceWaitEntries=", frame.blit_fence_wait_entry_count,
         " blitFenceUpdateEntries=", frame.blit_fence_update_entry_count,
         " blitCommands=", frame.blit_command_count,
         " blitB2B=", frame.blit_copy_buffer_to_buffer_count,
         " blitB2T=", frame.blit_copy_buffer_to_texture_count,
         " blitT2B=", frame.blit_copy_texture_to_buffer_count,
         " blitT2T=", frame.blit_copy_texture_to_texture_count,
         " blitMips=", frame.blit_generate_mipmaps_count,
         " blitWaitFence=", frame.blit_fence_wait_count,
         " blitUpdateFence=", frame.blit_fence_update_count,
         " blitFill=", frame.blit_fill_buffer_count,
         " blitResolveCounters=", frame.blit_resolve_counters_count,
         " presentPasses=", frame.present_pass_count,
         " clearPasses=", frame.clear_pass_count,
         " flushChunks=", frame.flush_chunk_count,
         " flushEmpty=", frame.flush_empty_chunk_count,
         " flushEventOnly=", frame.flush_event_only_chunk_count,
         " flushSignal=", frame.flush_signal_chunk_count,
         " flushWait=", frame.flush_wait_chunk_count,
         " flushPresent=", frame.flush_present_chunk_count,
         " flushEncoders=", frame.flush_encoder_count,
         " encodedEncoders=", frame.flush_encoded_encoder_count,
         " flushNull=", frame.flush_null_encoder_count,
         " flushRender=", frame.flush_render_encoder_count,
         " flushCompute=", frame.flush_compute_encoder_count,
         " flushBlit=", frame.flush_blit_encoder_count,
         " flushTimestamp=", frame.flush_timestamp_encoder_count,
         " avgFlushChunks=", average.flush_chunk_count,
         " avgRenderPasses=", average.render_pass_count,
         " avgRenderColorLoad=", average.render_color_load_count,
         " avgRenderColorStore=", average.render_color_store_count,
         " avgRenderDepthLoad=", average.render_depth_load_count,
         " avgRenderDepthStore=", average.render_depth_store_count,
         " avgRenderAttachmentReload=", average.render_attachment_reload_count,
         " avgRenderStoreThenLoad=", average.render_attachment_store_then_load_count,
         " avgSwitchR2C=", average.encoder_switch_render_compute_count,
         " avgSwitchC2R=", average.encoder_switch_compute_render_count,
         " avgFlushTotalMs=", DxmtQueueDiagDurationMs(average.flush_total_interval),
         " avgFlushRenderMs=", DxmtQueueDiagDurationMs(average.flush_render_interval),
         " avgFlushComputeMs=", DxmtQueueDiagDurationMs(average.flush_compute_interval),
         " avgFlushBlitMs=", DxmtQueueDiagDurationMs(average.flush_blit_interval),
         " avgFlushEventMs=", DxmtQueueDiagDurationMs(average.flush_event_interval),
         " avgFlushSampleMs=", DxmtQueueDiagDurationMs(average.flush_sample_interval),
         " avgFlushSignalMs=", DxmtQueueDiagDurationMs(average.flush_signal_interval),
         " avgFlushCleanupMs=", DxmtQueueDiagDurationMs(average.flush_cleanup_interval),
         " maxFlushChunkMs=", DxmtQueueDiagDurationMs(frame.flush_max_chunk_interval),
         " avgCommitMs=", DxmtQueueDiagDurationMs(average.commit_interval),
         " avgEncodePrepareMs=", DxmtQueueDiagDurationMs(average.encode_prepare_interval),
         " avgEncodeFlushMs=", DxmtQueueDiagDurationMs(average.encode_flush_interval),
         " avgDrawableMs=", DxmtQueueDiagDurationMs(average.drawable_blocking_interval),
         " avgPresentLatencyMs=", DxmtQueueDiagDurationMs(average.present_latency_interval),
         " frameWallMs=", DxmtQueueDiagElapsedMs(frame_begin_time, boundary_time),
         " latency=", frame.latency);
  }
  frame_count++;
  const auto frame_wall_us = frame_begin_time == clock::time_point{}
                                 ? 0
                                 : DxmtQueueDiagDurationUs(
                                       boundary_time - frame_begin_time);
  perf::recordFrameBoundary(
      frame_count, statistics.at(frame_count - 1), statistics.average(),
      frame_wall_us);
  statistics.at(frame_count).reset();
  const auto next_frame_begin_time = clock::now();
  statistics.at(frame_count).begin_time = next_frame_begin_time;
  perf::setCurrentFrameStatistics(&statistics.at(frame_count));
  statistics.at(frame_count).latency = max_latency_;
}

void
CommandQueue::WaitCPUFence(uint64_t seq) {
  if (cpu_coherent.signaledValue() >= seq)
    return;

  const auto pair_id = lifecycle::nextPairSequence();
  const auto queue_pair_id =
      lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
  DxmtQueueLifecycleLog(this, "cpu-fence-wait.enter", pair_id, queue_pair_id,
                        ~0ull, CurrentSeqId(), 0, seq,
                        cpu_coherent.signaledValue());
  const auto t0 = clock::now();
  cpu_coherent.wait(seq);
  const auto t1 = clock::now();
  DxmtQueueLifecycleLog(this, "cpu-fence-wait.leave", pair_id, queue_pair_id,
                        ~0ull, CurrentSeqId(), 0, seq,
                        cpu_coherent.signaledValue());
  const auto wait_us =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  perf::recordWaitCpuFence(uint64_t(wait_us));
  if (DxmtQueueDiagEnabled()) {
    WARN_FILE_ONLY("DXMT queue diagnostic: WaitCPUFence"
         " target=", seq,
         " coherent=", cpu_coherent.signaledValue(),
         " waitMs=", DxmtQueueDiagDurationMs(t1 - t0));
  }
}

void
CommandQueue::AddPersistentResidency(WMT::Object allocation) {
  if (!allocation)
    return;

  std::lock_guard<dxmt::mutex> lock(persistent_residency_mutex_);
  auto &entry = persistent_residency_entries_[allocation.handle];
  if (!entry.allocation) {
    entry.allocation = allocation;
    persistent_residency_set_.addAllocation(allocation);
    persistent_residency_dirty_ = true;
  }
  entry.ref_count++;
  entry.pending_remove = false;
  entry.remove_after_seq = 0;
}

void
CommandQueue::EnsureCachedAllocationResidency(WMT::Resource allocation) {
  if (!allocation)
    return;

  std::lock_guard<dxmt::mutex> lock(persistent_residency_mutex_);
  if (!persistent_residency_cached_allocations_
           .emplace(allocation.handle, true)
           .second)
    return;
  auto &entry = persistent_residency_entries_[allocation.handle];
  if (!entry.allocation) {
    entry.allocation = allocation;
    persistent_residency_set_.addAllocation(allocation);
    persistent_residency_dirty_ = true;
  }
  entry.ref_count++;
  entry.pending_remove = false;
  entry.remove_after_seq = 0;
}

void
CommandQueue::RetireCachedAllocationResidency(WMT::Resource allocation,
                                              uint64_t sequence) {
  if (!allocation)
    return;
  {
    std::lock_guard<dxmt::mutex> lock(persistent_residency_mutex_);
    if (!persistent_residency_cached_allocations_.erase(allocation.handle))
      return;
  }
  RemovePersistentResidencyAfterCompletion(allocation, sequence);
}

std::tuple<uint32_t, uint64_t, uint32_t, uint32_t, uint64_t>
CommandQueue::PersistentResidencyStatsForTesting() {
  std::lock_guard<dxmt::mutex> lock(persistent_residency_mutex_);
  uint64_t total_ref_count = 0;
  uint32_t pending_removal_count = 0;
  for (const auto &[handle, entry] : persistent_residency_entries_) {
    (void)handle;
    total_ref_count += entry.ref_count;
    pending_removal_count += entry.pending_remove ? 1 : 0;
  }
  return {
      static_cast<uint32_t>(persistent_residency_entries_.size()),
      total_ref_count,
      pending_removal_count,
      static_cast<uint32_t>(persistent_residency_cached_allocations_.size()),
      persistent_residency_commit_count_};
}

void
CommandQueue::RemovePersistentResidencyAfterCompletion(WMT::Object allocation) {
  // Descriptor writes happen independently of queue submission. Retire against
  // the last sequence that was actually published, rather than the current
  // (possibly forever empty) chunk.
  const auto next_seq = ready_for_encode.load(std::memory_order_acquire);
  RemovePersistentResidencyAfterCompletion(
      allocation, next_seq ? next_seq - 1 : 0);
}

void
CommandQueue::RemovePersistentResidencyAfterCompletion(
    WMT::Object allocation, uint64_t sequence) {
  if (!allocation)
    return;

  bool notify_flush = false;
  {
    std::lock_guard<dxmt::mutex> lock(persistent_residency_mutex_);
    auto entry = persistent_residency_entries_.find(allocation.handle);
    if (entry == persistent_residency_entries_.end())
      return;
    if (entry->second.ref_count)
      entry->second.ref_count--;
    if (entry->second.ref_count)
      return;

    // Removal is safe immediately when the sequence that could have referenced
    // the allocation has already completed. RetirePersistentResidencyRemovals()
    // handles the complementary race where completion happens just after this
    // check while holding the same mutex.
    if (sequence <= cpu_coherent.signaledValue()) {
      // Metal residency removals are only effective after commit. Keep an
      // explicit strong reference through that commit. A short-lived worker
      // coalesces descriptor churn while still guaranteeing that an idle queue
      // eventually publishes the removal and drops even a single large
      // allocation.
      persistent_residency_retired_allocations_.emplace_back(
          WMT::Object(entry->second.allocation.handle));
      persistent_residency_set_.removeAllocation(entry->second.allocation);
      persistent_residency_dirty_ = true;
      persistent_residency_entries_.erase(entry);
      if (!persistent_residency_flush_requested_) {
        persistent_residency_flush_requested_ = true;
        notify_flush = true;
      }
    } else {
      entry->second.pending_remove = true;
      entry->second.remove_after_seq =
          std::max(entry->second.remove_after_seq, sequence);
    }
  }
  if (notify_flush)
    persistent_residency_cond_.notify_one();
}

void
CommandQueue::RetainUntilGpuComplete(std::function<void()> release) {
  const auto next_seq = ready_for_encode.load(std::memory_order_acquire);
  RetainUntilGpuComplete(next_seq ? next_seq - 1 : 0, std::move(release));
}

void
CommandQueue::RetainUntilGpuComplete(uint64_t sequence,
                                     std::function<void()> release) {
  if (!release)
    return;

  std::function<void()> release_now;
  {
    std::lock_guard<dxmt::mutex> lock(deferred_release_mutex_);
    deferred_release_completed_seq_ = std::max(
        deferred_release_completed_seq_, cpu_coherent.signaledValue());
    if (sequence <= deferred_release_completed_seq_)
      release_now = std::move(release);
    else
      deferred_releases_[sequence].push_back(std::move(release));
  }
  if (release_now)
    release_now();
}

uint64_t
CommandQueue::FlushPersistentResidency() {
  std::deque<WMT::Reference<WMT::Object>> retired_allocations;
  clock::time_point begin;
  clock::time_point end;
  {
    std::lock_guard<dxmt::mutex> lock(persistent_residency_mutex_);
    if (!persistent_residency_dirty_) {
      persistent_residency_flush_requested_ = false;
      return 0;
    }

    begin = clock::now();
    persistent_residency_set_.commit();
    end = clock::now();
    persistent_residency_commit_count_++;
    persistent_residency_dirty_ = false;
    persistent_residency_flush_requested_ = false;
    retired_allocations.swap(persistent_residency_retired_allocations_);
  }
  return std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
}

void
CommandQueue::RetirePersistentResidencyRemovals(uint64_t completed_seq) {
  bool notify_flush = false;
  {
    std::lock_guard<dxmt::mutex> lock(persistent_residency_mutex_);
    bool removed = false;
    for (auto it = persistent_residency_entries_.begin();
         it != persistent_residency_entries_.end();) {
      auto &entry = it->second;
      if (entry.pending_remove && entry.ref_count == 0 &&
          entry.remove_after_seq <= completed_seq) {
        persistent_residency_retired_allocations_.emplace_back(
            WMT::Object(entry.allocation.handle));
        persistent_residency_set_.removeAllocation(entry.allocation);
        persistent_residency_dirty_ = true;
        removed = true;
        it = persistent_residency_entries_.erase(it);
      } else {
        ++it;
      }
    }
    if (removed || !persistent_residency_retired_allocations_.empty()) {
      // The finish thread only publishes retirement. ResidencySet mutation is
      // committed by the dedicated maintenance worker (or by the encode
      // submit path before a command-buffer commit), so completion callbacks
      // never enter the backend residency commit path.
      if (!persistent_residency_flush_requested_) {
        persistent_residency_flush_requested_ = true;
        notify_flush = true;
      }
    }
  }
  if (notify_flush)
    persistent_residency_cond_.notify_one();
}

void
CommandQueue::PersistentResidencyThread() {
  env::setThreadName("dxmt-residency-thread");

  for (;;) {
    std::deque<WMT::Reference<WMT::Object>> retired_allocations;
    std::unique_lock<dxmt::mutex> lock(persistent_residency_mutex_);
    persistent_residency_cond_.wait(lock, [this]() {
      return persistent_residency_stop_ ||
             persistent_residency_flush_requested_;
    });
    if (persistent_residency_stop_)
      return;

    // Keep the request armed during this delay, so additional removals join
    // the same batch without repeatedly waking the worker.
    persistent_residency_cond_.wait_for(
        lock, kPersistentResidencyFlushDelay,
        [this]() { return persistent_residency_stop_; });
    if (persistent_residency_stop_)
      return;

    // This is a long-lived Wine/engine thread. Bound every flush with its own
    // autorelease pool so Metal/Foundation temporaries cannot accumulate for
    // the lifetime of the queue.
    auto pool = WMT::MakeAutoreleasePool();
    if (persistent_residency_dirty_) {
      persistent_residency_set_.commit();
      persistent_residency_commit_count_++;
      persistent_residency_dirty_ = false;
      retired_allocations.swap(persistent_residency_retired_allocations_);
    }
    persistent_residency_flush_requested_ = false;
    lock.unlock();
    // Resource releases may enter backend/object lifetime code. Keep them
    // outside the residency mutex so no callback can invert this lock order.
    retired_allocations.clear();
  }
}

void
CommandQueue::CompleteDeferredReleases(uint64_t completed_seq) {
  std::vector<std::function<void()>> releases;
  {
    std::lock_guard<dxmt::mutex> lock(deferred_release_mutex_);
    deferred_release_completed_seq_ =
        std::max(deferred_release_completed_seq_, completed_seq);
    for (auto it = deferred_releases_.begin();
         it != deferred_releases_.end();) {
      if (it->first <= deferred_release_completed_seq_) {
        for (auto &release : it->second)
          releases.push_back(std::move(release));
        it = deferred_releases_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto &release : releases)
    release();
}

void
CommandQueue::DrainDeferredReleases() {
  std::vector<std::function<void()>> releases;
  {
    std::lock_guard<dxmt::mutex> lock(deferred_release_mutex_);
    for (auto &[sequence, callbacks] : deferred_releases_) {
      (void)sequence;
      for (auto &callback : callbacks)
        releases.push_back(std::move(callback));
    }
    deferred_releases_.clear();
  }
  for (auto &release : releases)
    release();
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk) {
  auto pool = WMT::MakeAutoreleasePool();
  DxmtQueueLifecycleLog(this, "encode.enter", chunk.lifecycle_pair_id,
                        chunk.queue_lifecycle_id, chunk.frame_, chunk.chunk_id,
                        0, chunk.chunk_id, chunk.resource_initializer_event_id);
  static std::atomic<uint32_t> internal_log_count = 0;
  if (DxmtQueueDiagShouldLog(internal_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitChunkInternal begin"
         " chunk=", chunk.chunk_id,
         " slot=", chunk.chunk_id % kCommandChunkCount,
         " frame=", chunk.frame_,
         " event=", chunk.chunk_event_id,
         " initEvent=", chunk.resource_initializer_event_id,
         " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
         " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
         " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
         " coherent=", cpu_coherent.signaledValue());
  }

  switch (capture_state.getNextAction(chunk.frame_)) {
  case CaptureState::NextAction::StartCapture: {
    WMTCaptureInfo info;
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    info.capture_object = device;
    info.destination = WMTCaptureDestinationGPUTraceDocument;
    char filename[1024];
    std::time_t now;
    std::time(&now);
    std::strftime(filename, 1024, "_%H'%M'%S_%m-%d-%y.gputrace", std::localtime(&now));
    auto fileUrl = env::getUnixPath(env::getExeBaseName() + "_F." + std::to_string(chunk.frame_) + filename);
    WARN("A new capture will be saved to ", fileUrl);
    info.output_url.set(fileUrl.c_str());

    capture_mgr.startCapture(info);
    break;
  }
  case CaptureState::NextAction::StopCapture: {
    auto capture_mgr = WMT::CaptureManager::sharedCaptureManager();
    capture_mgr.stopCapture();
    break;
  }
  case CaptureState::NextAction::Nothing: {
    if (capture_state.shouldCaptureNextFrame()) {
      capture_state.scheduleNextFrameCapture(chunk.frame_ + 1);
    }
    break;
  }
  }

  auto cmdbuf = commandQueue.commandBuffer();
  chunk.attached_cmdbuf = cmdbuf;
  chunk.encode_begin_time = clock::now();
  if (DxmtQueueDiagShouldLog(internal_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitChunkInternal commandBuffer"
         " chunk=", chunk.chunk_id,
         " cmdbuf=", cmdbuf.handle,
         " status=", static_cast<uint32_t>(cmdbuf.status()));
  }
  if (apitrace_enabled_) {
    uint64_t d3d_seq = dxmt::apitrace::d3d_enabled()
                           ? dxmt::apitrace::current_d3d_sequence()
                           : chunk.chunk_id;
    dxmt::apitrace::set_current_d3d_sequence(d3d_seq);
    dxmt::apitrace::on_command_buffer_begin(cmdbuf.handle, chunk.frame_);
  }
  if (chunk.resource_initializer_event_id) {
    auto initializer_event = initializer.event();
    if (perf::enabled()) {
      const auto initializer_current = initializer_event.signaledValue();
      WARN_FILE_ONLY("DXMT queue perf dependency:"
           " chunk=", chunk.chunk_id,
           " frame=", chunk.frame_,
           " cmdbuf=", cmdbuf.handle,
           " initEventObject=", initializer_event.handle,
           " initTarget=", chunk.resource_initializer_event_id,
           " initCurrent=", initializer_current,
           " blockedAtEncode=", initializer_current < chunk.resource_initializer_event_id);
    }
    cmdbuf.encodeWaitForEvent(initializer_event, chunk.resource_initializer_event_id);
  }
  chunk.encode(chunk.attached_cmdbuf, this->argument_encoding_ctx);
  chunk.encode_end_time = clock::now();
  if (DxmtQueueDiagShouldLog(internal_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitChunkInternal encoded"
         " chunk=", chunk.chunk_id,
         " cmdbuf=", cmdbuf.handle,
         " status=", static_cast<uint32_t>(cmdbuf.status()));
  }
  if (apitrace_enabled_) {
    dxmt::apitrace::on_command_buffer_commit(cmdbuf.handle);
  }
  if (DxmtQueueDiagShouldLog(internal_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitChunkInternal before metal commit"
         " chunk=", chunk.chunk_id,
         " cmdbuf=", cmdbuf.handle,
         " status=", static_cast<uint32_t>(cmdbuf.status()));
  }
  const auto commit_begin = clock::now();
  const uint64_t persistent_residency_submit_us = FlushPersistentResidency();
  uint64_t metal_residency_submit_us = 0;
  cmdbuf.commitAndGetStats(&metal_residency_submit_us);
  chunk.metal_commit_time = clock::now();
  const uint64_t residency_submit_us =
      persistent_residency_submit_us + metal_residency_submit_us;
  perf::recordResidencySubmitTime(perf::currentFrameStatistics(),
                                  std::chrono::microseconds(residency_submit_us));
  perf::recordMetalCommandBufferCommit(
      std::chrono::duration_cast<std::chrono::microseconds>(
          chunk.metal_commit_time - commit_begin).count());
  if (DxmtQueueDiagShouldLog(internal_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitChunkInternal after metal commit"
         " chunk=", chunk.chunk_id,
         " cmdbuf=", cmdbuf.handle,
         " status=", static_cast<uint32_t>(cmdbuf.status()));
  }

  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_one();
  DxmtQueueLifecycleLog(this, "encode.leave", chunk.lifecycle_pair_id,
                        chunk.queue_lifecycle_id, chunk.frame_, chunk.chunk_id,
                        cmdbuf.handle,
                        ready_for_commit.load(std::memory_order_relaxed),
                        chunk.chunk_event_id);
  if (DxmtQueueDiagShouldLog(internal_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitChunkInternal notified finish"
         " chunk=", chunk.chunk_id,
         " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
         " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
         " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
         " coherent=", cpu_coherent.signaledValue());
  }
}

uint32_t
CommandQueue::EncodingThread() {
#if ASYNC_ENCODING
  env::setThreadName("dxmt-encode-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load(std::memory_order_acquire)) {
    auto ready = ready_for_encode.load(std::memory_order_acquire);
    while (!stopped.load(std::memory_order_acquire) && ready == internal_seq) {
      static std::atomic<uint32_t> encode_wait_log_count = 0;
      const auto wait_begin_time = clock::now();
      const auto wait_pair = lifecycle::nextPairSequence();
      const auto wait_queue_pair =
          lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
      DxmtQueueLifecycleLog(
          this, "encode-worker-wait.enter", wait_pair, wait_queue_pair,
          ~0ull, internal_seq, 0, internal_seq, ready);
      const bool log_wait = DxmtQueueDiagShouldLogIdleWait(encode_wait_log_count);
      if (log_wait) {
        WARN_FILE_ONLY("DXMT queue diagnostic: EncodingThread idle wait begin"
             " internal_seq=", internal_seq,
             " readyEncode=", ready,
             " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
             " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
             " stopped=", stopped.load(std::memory_order_relaxed));
      }
      ready_for_encode.wait(ready, std::memory_order_acquire);
      ready = ready_for_encode.load(std::memory_order_acquire);
      DxmtQueueLifecycleLog(
          this, "encode-worker-wait.leave", wait_pair, wait_queue_pair,
          ~0ull, internal_seq, 0, internal_seq, ready);
      if (log_wait) {
        const auto wait_end_time = clock::now();
        WARN_FILE_ONLY("DXMT queue diagnostic: EncodingThread idle wait wake"
             " internal_seq=", internal_seq,
             " readyEncode=", ready,
             " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
             " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
             " stopped=", stopped.load(std::memory_order_relaxed),
             " waitMs=", DxmtQueueDiagDurationMs(wait_end_time - wait_begin_time));
      }
    }
    if (stopped.load(std::memory_order_acquire))
      break;
    // perform...
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    chunk.finish_begin_time = clock::now();
    static std::atomic<uint32_t> encode_log_count = 0;
    if (DxmtQueueDiagShouldLog(encode_log_count)) {
      WARN_FILE_ONLY("DXMT queue diagnostic: EncodingThread begin"
           " seq=", internal_seq,
           " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
           " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
           " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
           " coherent=", cpu_coherent.signaledValue());
    }
    CommitChunkInternal(chunk);
    if (DxmtQueueDiagShouldLog(encode_log_count)) {
      WARN_FILE_ONLY("DXMT queue diagnostic: EncodingThread end"
           " seq=", internal_seq,
           " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
           " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
           " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
           " coherent=", cpu_coherent.signaledValue());
    }
    internal_seq++;
  }
  TRACE("encoder thread gracefully terminates");
#endif
  return 0;
}

uint32_t
CommandQueue::WaitForFinishThread() {
  env::setThreadName("dxmt-finish-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  uint64_t perf_gpu_frame = ~0ull;
  uint64_t perf_gpu_command_buffers = 0;
  uint64_t perf_gpu_start_ns = 0;
  uint64_t perf_gpu_end_ns = 0;
  uint64_t perf_gpu_busy_sum_ns = 0;
  std::vector<std::pair<uint64_t, uint64_t>> perf_gpu_intervals;
  uint64_t perf_commit_to_complete_sum_us = 0;
  uint64_t perf_commit_to_complete_max_us = 0;
  clock::time_point perf_frame_begin_time;
  clock::time_point perf_first_publish_time;
  clock::time_point perf_last_publish_time;
  clock::time_point perf_last_commit_time;
  uint64_t perf_publish_gap_max_us = 0;
  uint64_t perf_publish_to_encode_sum_us = 0;
  uint64_t perf_publish_to_encode_max_us = 0;
  uint64_t perf_encode_sum_us = 0;
  uint64_t perf_encode_max_us = 0;
  uint64_t perf_encode_prepare_sum_us = 0;
  uint64_t perf_encode_prepare_max_us = 0;
  uint64_t perf_encode_flush_sum_us = 0;
  uint64_t perf_encode_flush_max_us = 0;
  uint64_t perf_encode_to_commit_sum_us = 0;
  uint64_t perf_encode_to_commit_max_us = 0;
  uint64_t perf_max_encode_chunk = 0;
  uint32_t perf_max_encode_input_encoders = 0;
  uint32_t perf_max_encode_encoded_encoders = 0;
  uint32_t perf_max_encode_render_encoders = 0;
  uint32_t perf_max_encode_compute_encoders = 0;
  uint32_t perf_max_encode_blit_encoders = 0;
  auto flush_gpu_frame = [&]() {
    if (perf_gpu_frame == ~0ull || !perf_gpu_command_buffers)
      return;
    std::sort(perf_gpu_intervals.begin(), perf_gpu_intervals.end());
    uint64_t gpu_active_union_ns = 0;
    uint64_t union_begin_ns = 0;
    uint64_t union_end_ns = 0;
    for (const auto &[begin_ns, end_ns] : perf_gpu_intervals) {
      if (!union_end_ns || begin_ns > union_end_ns) {
        gpu_active_union_ns += union_end_ns - union_begin_ns;
        union_begin_ns = begin_ns;
        union_end_ns = end_ns;
      } else {
        union_end_ns = std::max(union_end_ns, end_ns);
      }
    }
    gpu_active_union_ns += union_end_ns - union_begin_ns;
    const uint64_t gpu_envelope_ns =
        perf_gpu_end_ns > perf_gpu_start_ns
            ? perf_gpu_end_ns - perf_gpu_start_ns
            : 0;
    const uint64_t gpu_idle_ns =
        gpu_envelope_ns > gpu_active_union_ns
            ? gpu_envelope_ns - gpu_active_union_ns
            : 0;
    perf::recordGpuFrameEncodeBreakdown(
        perf_gpu_frame, statistics.at(perf_gpu_frame));
    const auto elapsed_us = [](clock::time_point begin,
                               clock::time_point end) -> uint64_t {
      if (begin == clock::time_point{} || end <= begin)
        return 0;
      return std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
          .count();
    };
    perf::recordGpuFrameCompletion(
        perf_gpu_frame, perf_gpu_command_buffers, perf_gpu_start_ns,
        perf_gpu_end_ns, perf_gpu_busy_sum_ns, gpu_active_union_ns, gpu_idle_ns,
        perf_commit_to_complete_sum_us, perf_commit_to_complete_max_us,
        elapsed_us(perf_frame_begin_time, perf_first_publish_time),
        elapsed_us(perf_first_publish_time, perf_last_publish_time),
        perf_publish_gap_max_us, perf_publish_to_encode_sum_us,
        perf_publish_to_encode_max_us, perf_encode_sum_us, perf_encode_max_us,
        perf_encode_prepare_sum_us, perf_encode_prepare_max_us,
        perf_encode_flush_sum_us, perf_encode_flush_max_us,
        perf_encode_to_commit_sum_us, perf_encode_to_commit_max_us,
        elapsed_us(perf_first_publish_time, perf_last_commit_time),
        perf_max_encode_chunk, perf_max_encode_input_encoders,
        perf_max_encode_encoded_encoders, perf_max_encode_render_encoders,
        perf_max_encode_compute_encoders, perf_max_encode_blit_encoders);
    perf_gpu_command_buffers = 0;
    perf_gpu_start_ns = 0;
    perf_gpu_end_ns = 0;
    perf_gpu_busy_sum_ns = 0;
    perf_gpu_intervals.clear();
    perf_commit_to_complete_sum_us = 0;
    perf_commit_to_complete_max_us = 0;
    perf_frame_begin_time = {};
    perf_first_publish_time = {};
    perf_last_publish_time = {};
    perf_last_commit_time = {};
    perf_publish_gap_max_us = 0;
    perf_publish_to_encode_sum_us = 0;
    perf_publish_to_encode_max_us = 0;
    perf_encode_sum_us = 0;
    perf_encode_max_us = 0;
    perf_encode_prepare_sum_us = 0;
    perf_encode_prepare_max_us = 0;
    perf_encode_flush_sum_us = 0;
    perf_encode_flush_max_us = 0;
    perf_encode_to_commit_sum_us = 0;
    perf_encode_to_commit_max_us = 0;
    perf_max_encode_chunk = 0;
    perf_max_encode_input_encoders = 0;
    perf_max_encode_encoded_encoders = 0;
    perf_max_encode_render_encoders = 0;
    perf_max_encode_compute_encoders = 0;
    perf_max_encode_blit_encoders = 0;
  };
  while (!stopped.load(std::memory_order_acquire)) {
    auto ready = ready_for_commit.load(std::memory_order_acquire);
    while (!stopped.load(std::memory_order_acquire) && ready == internal_seq) {
      static std::atomic<uint32_t> finish_wait_log_count = 0;
      const auto wait_begin_time = clock::now();
      const auto wait_pair = lifecycle::nextPairSequence();
      const auto wait_queue_pair =
          lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
      DxmtQueueLifecycleLog(
          this, "finish-worker-wait.enter", wait_pair, wait_queue_pair,
          ~0ull, internal_seq, 0, internal_seq, ready);
      const bool log_wait = DxmtQueueDiagShouldLogIdleWait(finish_wait_log_count);
      if (log_wait) {
        WARN_FILE_ONLY("DXMT queue diagnostic: FinishThread idle wait begin"
             " internal_seq=", internal_seq,
             " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
             " readyCommit=", ready,
             " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
             " stopped=", stopped.load(std::memory_order_relaxed));
      }
      ready_for_commit.wait(ready, std::memory_order_acquire);
      ready = ready_for_commit.load(std::memory_order_acquire);
      DxmtQueueLifecycleLog(
          this, "finish-worker-wait.leave", wait_pair, wait_queue_pair,
          ~0ull, internal_seq, 0, internal_seq, ready);
      if (log_wait) {
        const auto wait_end_time = clock::now();
        WARN_FILE_ONLY("DXMT queue diagnostic: FinishThread idle wait wake"
             " internal_seq=", internal_seq,
             " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
             " readyCommit=", ready,
             " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
             " stopped=", stopped.load(std::memory_order_relaxed),
             " waitMs=", DxmtQueueDiagDurationMs(wait_end_time - wait_begin_time));
      }
    }
    if (stopped.load(std::memory_order_acquire))
      break;
    // Finish callbacks and Metal error/log inspection may create autoreleased
    // Foundation objects. Drain them once per completed command buffer rather
    // than retaining them for this thread's lifetime.
    auto pool = WMT::MakeAutoreleasePool();
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    static std::atomic<uint32_t> finish_log_count = 0;
    if (DxmtQueueDiagShouldLog(finish_log_count)) {
      WARN_FILE_ONLY("DXMT queue diagnostic: FinishThread begin"
           " seq=", internal_seq,
           " chunk=", chunk.chunk_id,
           " cmdbuf=", chunk.attached_cmdbuf.handle,
           " status=", static_cast<uint32_t>(chunk.attached_cmdbuf.status()),
           " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
           " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
           " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
           " coherent=", cpu_coherent.signaledValue());
    }
    if (chunk.attached_cmdbuf.status() <= WMTCommandBufferStatusScheduled) {
      const auto wait_pair = lifecycle::nextPairSequence();
      const auto wait_queue_pair =
          lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
      DxmtQueueLifecycleLog(
          this, "metal-completion-wait.enter", wait_pair, wait_queue_pair,
          chunk.frame_, chunk.chunk_id, chunk.attached_cmdbuf.handle,
          internal_seq, chunk.chunk_event_id);
      if (DxmtQueueDiagShouldLog(finish_log_count)) {
        WARN_FILE_ONLY("DXMT queue diagnostic: FinishThread wait begin"
             " seq=", internal_seq,
             " chunk=", chunk.chunk_id,
             " cmdbuf=", chunk.attached_cmdbuf.handle,
             " status=", static_cast<uint32_t>(chunk.attached_cmdbuf.status()));
      }
      auto wait_begin_time = clock::now();
      chunk.attached_cmdbuf.waitUntilCompleted();
      auto wait_end_time = clock::now();
      DxmtQueueLifecycleLog(
          this, "metal-completion-wait.leave", wait_pair, wait_queue_pair,
          chunk.frame_, chunk.chunk_id, chunk.attached_cmdbuf.handle,
          internal_seq, chunk.chunk_event_id);
      if (DxmtQueueDiagShouldLog(finish_log_count)) {
        WARN_FILE_ONLY("DXMT queue diagnostic: FinishThread wait end"
             " seq=", internal_seq,
             " chunk=", chunk.chunk_id,
             " cmdbuf=", chunk.attached_cmdbuf.handle,
             " status=", static_cast<uint32_t>(chunk.attached_cmdbuf.status()),
             " waitMs=", DxmtQueueDiagDurationMs(wait_end_time - wait_begin_time));
      }
    }
    chunk.finish_complete_time = clock::now();
    if (perf::enabled()) {
      const auto gpu_start = chunk.attached_cmdbuf.gpuStartTime();
      const auto gpu_end = chunk.attached_cmdbuf.gpuEndTime();
      if (chunk.frame_ != perf_gpu_frame) {
        flush_gpu_frame();
        perf_gpu_frame = chunk.frame_;
      }
      perf_gpu_command_buffers++;
      if (perf_frame_begin_time == clock::time_point{})
        perf_frame_begin_time = chunk.frame_begin_time;
      if (perf_first_publish_time == clock::time_point{})
        perf_first_publish_time = chunk.publish_time;
      if (perf_last_publish_time != clock::time_point{} &&
          chunk.publish_time > perf_last_publish_time) {
        const auto gap_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                chunk.publish_time - perf_last_publish_time)
                .count();
        perf_publish_gap_max_us =
            std::max(perf_publish_gap_max_us, uint64_t(gap_us));
      }
      perf_last_publish_time =
          std::max(perf_last_publish_time, chunk.publish_time);
      perf_last_commit_time =
          std::max(perf_last_commit_time, chunk.metal_commit_time);
      const auto accumulate_phase = [](clock::time_point begin,
                                       clock::time_point end, uint64_t &sum,
                                       uint64_t &maximum) {
        if (begin == clock::time_point{} || end <= begin)
          return;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin)
                .count();
        sum += uint64_t(elapsed);
        maximum = std::max(maximum, uint64_t(elapsed));
      };
      accumulate_phase(chunk.publish_time, chunk.encode_begin_time,
                       perf_publish_to_encode_sum_us,
                       perf_publish_to_encode_max_us);
      accumulate_phase(chunk.encode_begin_time, chunk.encode_end_time,
                       perf_encode_sum_us, perf_encode_max_us);
      const auto chunk_encode_us =
          chunk.encode_end_time > chunk.encode_begin_time
              ? std::chrono::duration_cast<std::chrono::microseconds>(
                    chunk.encode_end_time - chunk.encode_begin_time)
                    .count()
              : 0;
      if (chunk_encode_us > 0 && uint64_t(chunk_encode_us) >= perf_encode_max_us) {
        perf_max_encode_chunk = chunk.chunk_id;
        perf_max_encode_input_encoders = chunk.perf_input_encoder_count;
        perf_max_encode_encoded_encoders = chunk.perf_encoded_encoder_count;
        perf_max_encode_render_encoders = chunk.perf_render_encoder_count;
        perf_max_encode_compute_encoders = chunk.perf_compute_encoder_count;
        perf_max_encode_blit_encoders = chunk.perf_blit_encoder_count;
      }
      accumulate_phase(chunk.encode_begin_time, chunk.encode_prepare_end_time,
                       perf_encode_prepare_sum_us,
                       perf_encode_prepare_max_us);
      accumulate_phase(chunk.encode_prepare_end_time, chunk.encode_end_time,
                       perf_encode_flush_sum_us, perf_encode_flush_max_us);
      accumulate_phase(chunk.encode_end_time, chunk.metal_commit_time,
                       perf_encode_to_commit_sum_us,
                       perf_encode_to_commit_max_us);
      if (gpu_start && gpu_end > gpu_start) {
        if (!perf_gpu_start_ns || gpu_start < perf_gpu_start_ns)
          perf_gpu_start_ns = gpu_start;
        perf_gpu_end_ns = std::max(perf_gpu_end_ns, gpu_end);
        perf_gpu_busy_sum_ns += gpu_end - gpu_start;
        perf_gpu_intervals.emplace_back(gpu_start, gpu_end);
      }
      if (chunk.metal_commit_time != clock::time_point{}) {
        const auto completion_us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                chunk.finish_complete_time - chunk.metal_commit_time)
                .count();
        const auto nonnegative_completion_us =
            completion_us > 0 ? uint64_t(completion_us) : 0;
        perf_commit_to_complete_sum_us += nonnegative_completion_us;
        perf_commit_to_complete_max_us = std::max(
            perf_commit_to_complete_max_us, nonnegative_completion_us);
      }
    }
    if (chunk.attached_cmdbuf.status() == WMTCommandBufferStatusError) {
      MarkDeviceError();
      ERR("Device error at frame ", chunk.frame_, ": ", chunk.attached_cmdbuf.error().description().getUTF8String());
      const auto error_marker =
          env::getEnvVar("DXMT_TEST_COMMAND_BUFFER_ERROR_MARKER");
      if (!error_marker.empty()) {
        if (FILE *marker = fopen(error_marker.c_str(), "a")) {
          fputs("error\n", marker);
          fclose(marker);
        }
      }
    }
    if (auto logs = chunk.attached_cmdbuf.logs()) {
      for (auto &log : logs.elements()) {
        ERR("Frame ", chunk.frame_, ": ", log.description().getUTF8String());
      }
    }
    if (DxmtQueueDiagVerboseEnabled()) {
      const auto kernel_start = chunk.attached_cmdbuf.kernelStartTime();
      const auto kernel_end = chunk.attached_cmdbuf.kernelEndTime();
      const auto gpu_start = chunk.attached_cmdbuf.gpuStartTime();
      const auto gpu_end = chunk.attached_cmdbuf.gpuEndTime();
      const auto gpu_span = gpu_end > gpu_start ? gpu_end - gpu_start : 0;
      const auto kernel_span = kernel_end > kernel_start ? kernel_end - kernel_start : 0;
      const auto queue_gap = gpu_start > diag_last_gpu_end_ns_ && diag_last_gpu_end_ns_
                                 ? gpu_start - diag_last_gpu_end_ns_
                                 : 0;
      WARN_FILE_ONLY("DXMT queue diagnostic: CommandBufferTiming"
           " seq=", internal_seq,
           " chunk=", chunk.chunk_id,
           " frame=", chunk.frame_,
           " cmdbuf=", chunk.attached_cmdbuf.handle,
           " status=", static_cast<uint32_t>(chunk.attached_cmdbuf.status()),
           " gpuStartMs=", DxmtQueueDiagNsToMs(gpu_start),
           " gpuEndMs=", DxmtQueueDiagNsToMs(gpu_end),
           " gpuSpanMs=", DxmtQueueDiagNsToMs(gpu_span),
           " kernelStartMs=", DxmtQueueDiagNsToMs(kernel_start),
           " kernelEndMs=", DxmtQueueDiagNsToMs(kernel_end),
           " kernelSpanMs=", DxmtQueueDiagNsToMs(kernel_span),
           " queueGapMs=", DxmtQueueDiagNsToMs(queue_gap),
           " publishToEncodeMs=", DxmtQueueDiagElapsedMs(chunk.publish_time, chunk.encode_begin_time),
           " encodeWallMs=", DxmtQueueDiagElapsedMs(chunk.encode_begin_time, chunk.encode_end_time),
           " encodeToCommitMs=", DxmtQueueDiagElapsedMs(chunk.encode_end_time, chunk.metal_commit_time),
           " commitToFinishBeginMs=", DxmtQueueDiagElapsedMs(chunk.metal_commit_time, chunk.finish_begin_time),
           " commitToCompleteMs=", DxmtQueueDiagElapsedMs(chunk.metal_commit_time, chunk.finish_complete_time),
           " publishToCompleteMs=", DxmtQueueDiagElapsedMs(chunk.publish_time, chunk.finish_complete_time),
           " event=", chunk.chunk_event_id,
           " initEvent=", chunk.resource_initializer_event_id,
           " callbacks=", chunk.completionCallbackCount());
      if (gpu_end)
        diag_last_gpu_end_ns_ = std::max(diag_last_gpu_end_ns_, gpu_end);
    }

    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    chunk.readback.visibility = {};
    chunk.readback.timestamp = {};

    std::vector<std::function<void()>> completion_callbacks;
    {
      std::lock_guard<dxmt::mutex> lock(chunk.completion_callbacks_mutex_);
      completion_callbacks.swap(chunk.completion_callbacks);
    }
    const auto completion_chunk_id = chunk.chunk_id;
    const auto completion_frame = chunk.frame_;
    const auto completion_pair_id = chunk.lifecycle_pair_id;
    const auto completion_queue_pair_id = chunk.queue_lifecycle_id;
    const auto completion_cmdbuf = chunk.attached_cmdbuf.handle;

    DxmtQueueLifecycleLog(
        this, "completion-publish.enter", completion_pair_id,
        completion_queue_pair_id, completion_frame, completion_chunk_id,
        completion_cmdbuf, internal_seq, chunk.chunk_event_id);
    EnqueueReadbacks(chunk);
    chunk.reset();

    // Completion callbacks can re-enter D3D12 and wait on this queue. Publish
    // command-buffer completion after releasing command-list storage so the CPU
    // command heap cannot be recycled while chunk.reset() still walks it.
    cpu_coherent.signal(internal_seq);
    RetirePersistentResidencyRemovals(internal_seq);
    CompleteDeferredReleases(internal_seq);
    DxmtQueueLifecycleLog(
        this, "completion-publish.leave", completion_pair_id,
        completion_queue_pair_id, completion_frame, completion_chunk_id,
        completion_cmdbuf, internal_seq, cpu_coherent.signaledValue());
    if (DxmtQueueDiagShouldLog(finish_log_count)) {
      WARN_FILE_ONLY("DXMT queue diagnostic: FinishThread signaled"
           " seq=", internal_seq,
           " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
           " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
           " callbacks=", completion_callbacks.size(),
           " ongoingBeforeDec=", chunk_ongoing.load(std::memory_order_relaxed),
           " coherent=", cpu_coherent.signaledValue());
    }
    const auto callbacks_begin_time = clock::now();
    const auto callbacks_pair = lifecycle::nextPairSequence();
    const auto callbacks_queue_pair =
        lifecycle_sequence_.fetch_add(1, std::memory_order_relaxed);
    DxmtQueueLifecycleLog(
        this, "completion-callbacks.enter", callbacks_pair,
        callbacks_queue_pair, completion_frame, completion_chunk_id,
        completion_cmdbuf, internal_seq, completion_callbacks.size());
    for (auto &callback : completion_callbacks)
      callback();
    const auto callbacks_end_time = clock::now();
    DxmtQueueLifecycleLog(
        this, "completion-callbacks.leave", callbacks_pair,
        callbacks_queue_pair, completion_frame, completion_chunk_id,
        completion_cmdbuf, internal_seq, completion_callbacks.size());
    if (DxmtQueueDiagEnabled() && !completion_callbacks.empty()) {
      WARN_FILE_ONLY("DXMT queue diagnostic: CompletionCallbacks"
           " seq=", internal_seq,
           " chunk=", completion_chunk_id,
           " frame=", completion_frame,
           " count=", completion_callbacks.size(),
           " elapsedMs=", DxmtQueueDiagDurationMs(callbacks_end_time - callbacks_begin_time));
    }

    chunk_ongoing.fetch_sub(1, std::memory_order_release);
    chunk_ongoing.notify_one();
    if (DxmtQueueDiagShouldLog(finish_log_count)) {
      WARN_FILE_ONLY("DXMT queue diagnostic: FinishThread end"
           " seq=", internal_seq,
           " readyEncode=", ready_for_encode.load(std::memory_order_relaxed),
           " readyCommit=", ready_for_commit.load(std::memory_order_relaxed),
           " ongoing=", chunk_ongoing.load(std::memory_order_relaxed),
           " coherent=", cpu_coherent.signaledValue());
    }

    staging_allocator.free_blocks(internal_seq);
    copy_temp_allocator.free_blocks(internal_seq);
    argbuf_allocator.free_blocks(internal_seq);
    argbuf_shadow_allocator.free_blocks(internal_seq);
    cpu_command_allocator.free_blocks(internal_seq);

    internal_seq++;
  }
  flush_gpu_frame();
  TRACE("finishing thread gracefully terminates");
  return 0;
}

void
CommandQueue::EnqueueReadbacks(CommandChunk &chunk) {
  if (chunk.readback.diagnostics.empty() && chunk.deferred_readbacks.empty())
    return;

  std::vector<std::function<void()>> callbacks;
  callbacks.reserve(chunk.readback.diagnostics.size() + chunk.deferred_readbacks.size());
  for (auto &diagnostic : chunk.readback.diagnostics)
    callbacks.push_back(std::move(diagnostic));
  chunk.readback.diagnostics.clear();
  chunk.readback.visibility = {};
  chunk.readback.timestamp = {};
  for (auto &readback : chunk.deferred_readbacks)
    callbacks.push_back(std::move(readback));
  chunk.deferred_readbacks.clear();

  {
    std::lock_guard lock(readback_mutex_);
    for (auto &callback : callbacks)
      pending_readbacks_.push_back(std::move(callback));
  }
  readback_cond_.notify_one();
}

uint32_t
CommandQueue::ReadbackThread() {
  env::setThreadName("dxmt-readback-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    std::vector<std::function<void()>> callbacks;
    {
      std::unique_lock lock(readback_mutex_);
      readback_cond_.wait(lock, [this]() {
        return stopped.load(std::memory_order_acquire) || !pending_readbacks_.empty();
      });
      callbacks.swap(pending_readbacks_);
    }

    auto pool = WMT::MakeAutoreleasePool();
    for (auto &callback : callbacks)
      callback();

    if (stopped.load(std::memory_order_acquire)) {
      std::lock_guard lock(readback_mutex_);
      if (pending_readbacks_.empty())
        break;
    }
  }
  return 0;
}

void CommandQueue::Retain(uint64_t seq, Allocation* allocation) {
  auto &chunk = chunks[seq % kCommandChunkCount];
  auto &tracker = chunk.ref_tracker;
  constexpr size_t block_size = decltype(reftracker_storage_allocator)::block_size;
  while (unlikely(!tracker.track(allocation))) {
    auto [temp_buffer, _] = reftracker_storage_allocator.allocate(seq, cpu_coherent.signaledValue(), block_size, 1);
    tracker.addStorage(temp_buffer.ptr, block_size);
  }
};

} // namespace dxmt
