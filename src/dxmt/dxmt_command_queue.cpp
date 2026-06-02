#include "dxmt_command_queue.hpp"
#include "Metal.hpp"
#include "dxmt_apitrace.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <atomic>

#define ASYNC_ENCODING 1

namespace dxmt {

void *
CommandChunk::allocate_cpu_heap(size_t size, size_t alignment) {
  return queue->AllocateCommandData(size, alignment);
}

void
CommandChunk::encode(WMT::CommandBuffer cmdbuf, ArgumentEncodingContext &enc) {
  enc.$$setEncodingContext(chunk_id, frame_);
  auto &statistics = enc.currentFrameStatistics();

  auto t0 = clock::now();
  list_enc.execute(enc);
  attached_cmdbuf = cmdbuf;
  auto t1 = clock::now();
  readback = enc.flushCommands(cmdbuf, chunk_id, chunk_event_id);
  auto t2 = clock::now();

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
  readback_cond_.notify_all();
  readbackThread.join();
  for (unsigned i = 0; i < kCommandChunkCount; i++) {
    auto &chunk = chunks[i];
    chunk.reset();
  };
  event_listener_thread.join();
  if (apitrace_enabled_)
    dxmt::apitrace::shutdown();
  TRACE("Destructed command queue");
}

void
CommandQueue::CommitCurrentChunk() {
  auto& statistics = CurrentFrameStatistics();
#if ASYNC_ENCODING
  auto t0 = clock::now();
  for (;;) {
    auto ongoing = chunk_ongoing.load(std::memory_order_acquire);
    while (ongoing >= kCommandChunkCount - 1) {
      dxmt::this_thread::yield();
      ongoing = chunk_ongoing.load(std::memory_order_acquire);
    }
    if (chunk_ongoing.compare_exchange_weak(
            ongoing, ongoing + 1, std::memory_order_acq_rel,
            std::memory_order_acquire))
      break;
  }
  auto t1 = clock::now();
  statistics.commit_interval += (t1 - t0);
#endif

  auto chunk_id = ready_for_encode.load(std::memory_order_relaxed);
  auto &chunk = chunks[chunk_id % kCommandChunkCount];
  chunk.chunk_id = chunk_id;
  chunk.chunk_event_id = GetNextEventSeqId();
  chunk.frame_ = frame_count;
  chunk.resource_initializer_event_id = initializer.flushToWait();
  statistics.command_buffer_count++;
#if ASYNC_ENCODING
  ready_for_encode.fetch_add(1, std::memory_order_release);
  ready_for_encode.notify_one();

#else
  ready_for_encode.fetch_add(1, std::memory_order_relaxed);
  CommitChunkInternal(chunk);
#endif

  cpu_command_allocator.free_blocks(cpu_coherent.signaledValue());
}

void
CommandQueue::WaitCPUFence(uint64_t seq) {
  cpu_coherent.wait(seq);
}

void
CommandQueue::PresentBoundary() {
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
CommandQueue::CommitChunkInternal(CommandChunk &chunk) {
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

  auto cmdbuf = commandQueue.commandBuffer();
  chunk.attached_cmdbuf = cmdbuf;
  if (apitrace_enabled_) {
    uint64_t d3d_seq = dxmt::apitrace::d3d_enabled()
                           ? dxmt::apitrace::current_d3d_sequence()
                           : chunk.chunk_id;
    dxmt::apitrace::set_current_d3d_sequence(d3d_seq);
    dxmt::apitrace::on_command_buffer_begin(cmdbuf.handle, chunk.frame_);
  }
  if (chunk.resource_initializer_event_id) {
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
  }
  chunk.encode(chunk.attached_cmdbuf, this->argument_encoding_ctx);
  if (apitrace_enabled_) {
    dxmt::apitrace::on_command_buffer_commit(cmdbuf.handle);
  }
  cmdbuf.commit();

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
    while (!stopped.load() &&
           ready_for_encode.load(std::memory_order_acquire) == internal_seq) {
      dxmt::this_thread::yield();
    }
    if (stopped.load())
      break;
    // perform...
    auto &chunk = chunks[internal_seq % kCommandChunkCount];
    CommitChunkInternal(chunk);
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
    while (!stopped.load() &&
           ready_for_commit.load(std::memory_order_acquire) == internal_seq) {
      dxmt::this_thread::yield();
    }
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

    chunk.readback.visibility = {};
    chunk.readback.timestamp = {};
    for (auto &callback : chunk.completion_callbacks)
      callback();
    chunk.completion_callbacks.clear();

    EnqueueReadbacks(chunk);
    chunk.reset();
    cpu_coherent.signal(internal_seq);
    chunk_ongoing.fetch_sub(1, std::memory_order_release);
    chunk_ongoing.notify_one();

    staging_allocator.free_blocks(internal_seq);
    copy_temp_allocator.free_blocks(internal_seq);
    argbuf_allocator.free_blocks(internal_seq);
    argbuf_shadow_allocator.free_blocks(internal_seq);

    internal_seq++;
  }
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
