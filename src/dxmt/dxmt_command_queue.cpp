#include "dxmt_command_queue.hpp"
#include "Metal.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <atomic>

#define ASYNC_ENCODING 1

namespace dxmt {

static bool
DiagEnabledEnv(const char *name) {
  auto value = env::getEnvVar(name);
  return value == "1" || value == "true" || value == "yes" || value == "trace";
}

static bool
DiagCommandQueueEnabled() {
  return DiagEnabledEnv("DXMT_DIAG_COMMAND_QUEUE") || DiagEnabledEnv("DXMT_DIAG_SWAPCHAIN");
}

static double
DiagMillis(clock::duration duration) {
  return duration.count() / 1000000.0;
}

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue->AllocateCommandData(size, alignment);
}

void
CommandChunk::encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc) {
  enc.$$setEncodingContext(chunk_id, frame_);
  auto &statistics = enc.currentFrameStatistics();

  auto t0 = clock::now();
  auto execution_profile = list_enc.execute(enc, DiagCommandQueueEnabled());
  attached_cmdbuf = cmdbuf;
  auto t1 = clock::now();
  readback = enc.flushCommands(cmdbuf, chunk_id, chunk_event_id);
  auto t2 = clock::now();

  auto execute_elapsed = t1 - t0;
  auto flush_elapsed = t2 - t1;
  auto total_elapsed = t2 - t0;
  statistics.encode_prepare_interval += execute_elapsed;
  statistics.encode_flush_interval += flush_elapsed;

  auto total_ms = DiagMillis(total_elapsed);
  if (DiagCommandQueueEnabled() || total_ms > 8.0) {
    INFO("DXMT: CommandChunk encode frame=", frame_, " chunk=", chunk_id,
         " totalMs=", total_ms,
         " executeMs=", DiagMillis(execute_elapsed),
         " flushMs=", DiagMillis(flush_elapsed),
         " renderPasses=", statistics.render_pass_count,
         " computePasses=", statistics.compute_pass_count,
         " blitPasses=", statistics.blit_pass_count,
         " clearPasses=", statistics.clear_pass_count,
         " drawableMs=", DiagMillis(statistics.drawable_blocking_interval),
         " commandCount=", execution_profile.command_count,
         " slowCommands=", execution_profile.slow_command_count,
         " maxCommandMs=", DiagMillis(execution_profile.max_command_duration),
         " maxCommandIndex=", execution_profile.max_command_index,
         " maxCommand=", execution_profile.max_command_name ? execution_profile.max_command_name : "");
  }
};

CommandQueue::CommandQueue(WMT::Device device) :
    encodeThread([this]() { this->EncodingThread(); }),
    finishThread([this]() { this->WaitForFinishThread(); }),
    device(device),
    commandQueue(device.newCommandQueue(kCommandChunkCount)),
    shared_event_listener(SharedEventListener_create()),
    event_listener_thread([this]() { SharedEventListener_start(this->shared_event_listener); }),
    staging_allocator({
        device, WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceHazardTrackingModeUntracked |
                    WMTResourceStorageModeManaged, false
    }),
    copy_temp_allocator({device, WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModePrivate}),
    argbuf_allocator({
        device,
        WMTResourceHazardTrackingModeUntracked | WMTResourceCPUCacheModeWriteCombined | WMTResourceStorageModeShared
    }),
    cpu_command_allocator({}),
    reftracker_storage_allocator({}),
    cmd_library(device),
    argument_encoding_ctx(*this, device, cmd_library),
    initializer(device) {
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.queue = this;
    chunk.reset();
  };
  event = device.newSharedEvent();

  std::string env = env::getEnvVar("DXMT_CAPTURE_FRAME");

  if (!env.empty()) {
    try {
      capture_state.scheduleNextFrameCapture(std::stoull(env));
    } catch (const std::invalid_argument &) {
    }
  }
}

CommandQueue::~CommandQueue() {
  TRACE("Destructing command queue");
  stopped.store(true);
  ready_for_encode++;
  ready_for_encode.notify_one();
  ready_for_commit++;
  ready_for_commit.notify_one();
  SharedEventListener_destroy(shared_event_listener);
  encodeThread.join();
  finishThread.join();
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.reset();
  };
  event_listener_thread.join();
  TRACE("Destructed command queue");
}

void
CommandQueue::CommitCurrentChunk() {
  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % kCommandChunkCount];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.frame_ = frame_count;
  chunk.resource_initializer_event_id = initializer.flushToWait();
  auto& statistics = CurrentFrameStatistics();
  statistics.command_buffer_count++;
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();

  auto t0 = clock::now();
  chunk_ongoing.wait(kCommandChunkCount - 1, std::memory_order_acquire);
  chunk_ongoing.fetch_add(1, std::memory_order_relaxed);
  auto t1 = clock::now();
  statistics.commit_interval += (t1 - t0);

#else
  CommitChunkInternal(chunk, ready_for_encode.fetch_add(1, std::memory_order_relaxed));
#endif

  cpu_command_allocator.free_blocks(cpu_coherent.signaledValue());
}

void
CommandQueue::PresentBoundary() {
  auto frame = frame_count;
  auto &completed = statistics.at(frame);
  auto frame_command_cpu = completed.commit_interval + completed.encode_prepare_interval + completed.encode_flush_interval;
  auto frame_command_cpu_ms = DiagMillis(frame_command_cpu);

  if (DiagCommandQueueEnabled() || frame_command_cpu_ms > 12.0) {
    INFO("DXMT: Frame command stats frame=", frame,
         " commandBuffers=", completed.command_buffer_count,
         " commandCpuMs=", frame_command_cpu_ms,
         " commitWaitMs=", DiagMillis(completed.commit_interval),
         " executeMs=", DiagMillis(completed.encode_prepare_interval),
         " flushMs=", DiagMillis(completed.encode_flush_interval),
         " drawableMs=", DiagMillis(completed.drawable_blocking_interval),
         " presentLatencyMs=", DiagMillis(completed.present_latency_interval),
         " syncCount=", completed.sync_count,
         " syncMs=", DiagMillis(completed.sync_interval),
         " eventStalls=", completed.event_stall,
         " renderPasses=", completed.render_pass_count,
         " computePasses=", completed.compute_pass_count,
         " blitPasses=", completed.blit_pass_count,
         " clearPasses=", completed.clear_pass_count,
         " bindingUploads=", completed.shader_binding_upload_count,
         " dirtyCB=", completed.shader_binding_dirty_cbuffer_count,
         " dirtySampler=", completed.shader_binding_dirty_sampler_count,
         " dirtySRV=", completed.shader_binding_dirty_srv_count,
         " dirtyUAV=", completed.shader_binding_dirty_uav_count,
         " cleanUAV=", completed.shader_binding_clean_uav_count);
  }

  statistics.compute(frame_count);
  frame_count++;
  statistics.at(frame_count).reset();
  // After present N-th frame (N starts from 1), wait for (N - max_latency)-th frame to finish rendering
  if (likely(frame_count > max_latency_)) {
    auto t0 = clock::now();
    frame_latency_fence_.wait(frame_count - max_latency_);
    auto t1 = clock::now();
    statistics.at(frame_count).present_latency_interval += (t1 - t0);
  }
  statistics.at(frame_count).latency = max_latency_;
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk, uint64_t seq) {

  auto commit_t0 = clock::now();
  auto pool = WMT::MakeAutoreleasePool();

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

  auto command_buffer_t0 = clock::now();
  auto cmdbuf = commandQueue.commandBuffer();
  auto command_buffer_t1 = clock::now();
  chunk.attached_cmdbuf = cmdbuf;
  if (chunk.resource_initializer_event_id) {
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
  }
  auto encode_t0 = clock::now();
  chunk.encode(chunk.attached_cmdbuf, this->argument_encoding_ctx);
  auto encode_t1 = clock::now();
  cmdbuf.commit();
  auto commit_t1 = clock::now();

  auto total_ms = DiagMillis(commit_t1 - commit_t0);
  if (DiagCommandQueueEnabled() || total_ms > 8.0) {
    INFO("DXMT: CommitChunkInternal frame=", chunk.frame_, " chunk=", chunk.chunk_id,
         " seq=", seq,
         " totalMs=", total_ms,
         " commandBufferMs=", DiagMillis(command_buffer_t1 - command_buffer_t0),
         " encodeMs=", DiagMillis(encode_t1 - encode_t0),
         " metalCommitMs=", DiagMillis(commit_t1 - encode_t1),
         " waitsInitializer=", chunk.resource_initializer_event_id != 0);
  }

  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_one();
}

uint32_t
CommandQueue::EncodingThread() {
#if ASYNC_ENCODING
  env::setThreadName("dxmt-encode-thread");
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  uint64_t internal_seq = 1;
  while (!stopped.load()) {
    ready_for_encode.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    // perform...
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    CommitChunkInternal(chunk, internal_seq);
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
  while (!stopped.load()) {
    ready_for_commit.wait(internal_seq, std::memory_order_acquire);
    if (stopped.load())
      break;
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    if (chunk.attached_cmdbuf.status() <= WMTCommandBufferStatusScheduled) {
      chunk.attached_cmdbuf.waitUntilCompleted();
    }
    if (chunk.attached_cmdbuf.status() == WMTCommandBufferStatusError) {
      ERR("Device error at frame ", chunk.frame_, ": ", chunk.attached_cmdbuf.error().description().getUTF8String());
    }
    if (auto logs = chunk.attached_cmdbuf.logs()) {
      for (auto &log : logs.elements()) {
        ERR("Frame ", chunk.frame_, ": ", log.description().getUTF8String());
      }
    }

    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    chunk.reset();
    cpu_coherent.signal(internal_seq);
    chunk_ongoing.fetch_sub(1, std::memory_order_release);
    chunk_ongoing.notify_one();

    staging_allocator.free_blocks(internal_seq);
    copy_temp_allocator.free_blocks(internal_seq);
    argbuf_allocator.free_blocks(internal_seq);

    internal_seq++;
  }
  TRACE("finishing thread gracefully terminates");
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
