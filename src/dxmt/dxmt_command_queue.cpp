#include "dxmt_command_queue.hpp"
#include "Metal.hpp"
#include "dxmt_apitrace.hpp"
#include "dxmt_apitrace_d3d.hpp"
#include "dxmt_statistics.hpp"
#include "util_env.hpp"
#include "util_win32_compat.h"
#include <algorithm>
#include <atomic>

#define ASYNC_ENCODING 1

namespace dxmt {

static bool
DxmtQueueDiagEnabled() {
  static const bool enabled =
      !env::getEnvVar("DXMT_DIAG_DXMT_QUEUE").empty();
  return enabled;
}

static bool
DxmtQueueDiagVerboseEnabled() {
  static const bool enabled =
      !env::getEnvVar("DXMT_DIAG_DXMT_QUEUE_VERBOSE").empty();
  return enabled;
}

static bool
DxmtQueueDiagShouldLog(std::atomic<uint32_t> &counter) {
  if (!DxmtQueueDiagVerboseEnabled())
    return false;

  const auto index = counter.fetch_add(1, std::memory_order_relaxed);
  return index < 512 || (index % 256) == 0;
}

static double
DxmtQueueDiagDurationMs(clock::duration duration) {
  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(duration).count();
}

static double
DxmtQueueDiagElapsedMs(clock::time_point start, clock::time_point end) {
  if (start == clock::time_point{} || end == clock::time_point{} || end < start)
    return 0.0;
  return DxmtQueueDiagDurationMs(end - start);
}

static double
DxmtQueueDiagNsToMs(uint64_t ns) {
  return static_cast<double>(ns) / 1000000.0;
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
  chunk.publish_time = clock::now();
  chunk.resource_initializer_event_id = initializer.flushToWait();
  statistics.command_buffer_count++;
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
  CommitChunkInternal(chunk);
#endif

}

void
CommandQueue::PresentBoundary() {
  const auto frame_begin_time = statistics.at(frame_count).begin_time;
  statistics.compute(frame_count);
  const auto boundary_time = clock::now();
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
  statistics.at(frame_count).reset();
  statistics.at(frame_count).begin_time = clock::now();
  // After present N-th frame (N starts from 1), wait for (N - max_latency)-th frame to finish rendering
  if (likely(frame_count > max_latency_)) {
    auto t0 = clock::now();
    frame_latency_fence_.wait(frame_count - max_latency_);
    auto t1 = clock::now();
    statistics.at(frame_count).present_latency_interval += (t1 - t0);
    if (DxmtQueueDiagEnabled()) {
      WARN_FILE_ONLY("DXMT queue diagnostic: FrameLatencyWait"
           " frame=", frame_count,
           " target=", frame_count - max_latency_,
           " signaled=", frame_latency_fence_.signaledValue(),
           " waitMs=", DxmtQueueDiagDurationMs(t1 - t0),
           " latency=", max_latency_);
    }
  }
  statistics.at(frame_count).latency = max_latency_;
}

void
CommandQueue::WaitCPUFence(uint64_t seq) {
  if (cpu_coherent.signaledValue() >= seq)
    return;

  const auto t0 = clock::now();
  cpu_coherent.wait(seq);
  const auto t1 = clock::now();
  if (DxmtQueueDiagEnabled()) {
    WARN_FILE_ONLY("DXMT queue diagnostic: WaitCPUFence"
         " target=", seq,
         " coherent=", cpu_coherent.signaledValue(),
         " waitMs=", DxmtQueueDiagDurationMs(t1 - t0));
  }
}

void
CommandQueue::CommitChunkInternal(CommandChunk &chunk) {
  auto pool = WMT::MakeAutoreleasePool();
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
    cmdbuf.encodeWaitForEvent(initializer.event(), chunk.resource_initializer_event_id);
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
  cmdbuf.commit();
  chunk.metal_commit_time = clock::now();
  if (DxmtQueueDiagShouldLog(internal_log_count)) {
    WARN_FILE_ONLY("DXMT queue diagnostic: CommitChunkInternal after metal commit"
         " chunk=", chunk.chunk_id,
         " cmdbuf=", cmdbuf.handle,
         " status=", static_cast<uint32_t>(cmdbuf.status()));
  }

  ready_for_commit.fetch_add(1, std::memory_order_release);
  ready_for_commit.notify_one();
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
  while (!stopped.load()) {
    while (!stopped.load() &&
           ready_for_encode.load(std::memory_order_acquire) == internal_seq) {
      dxmt::this_thread::yield();
    }
    if (stopped.load())
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
  while (!stopped.load()) {
    while (!stopped.load() &&
           ready_for_commit.load(std::memory_order_acquire) == internal_seq) {
      dxmt::this_thread::yield();
    }
    if (stopped.load())
      break;
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
    if (chunk.attached_cmdbuf.status() == WMTCommandBufferStatusError) {
      ERR("Device error at frame ", chunk.frame_, ": ", chunk.attached_cmdbuf.error().description().getUTF8String());
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
           " callbacks=", chunk.completion_callbacks.size());
      if (gpu_end)
        diag_last_gpu_end_ns_ = std::max(diag_last_gpu_end_ns_, gpu_end);
    }

    if (chunk.signal_frame_latency_fence_ != ~0ull)
      frame_latency_fence_.signal(chunk.signal_frame_latency_fence_);

    chunk.readback.visibility = {};
    chunk.readback.timestamp = {};

    std::vector<std::function<void()>> completion_callbacks;
    completion_callbacks.swap(chunk.completion_callbacks);

    EnqueueReadbacks(chunk);
    chunk.reset();

    // Completion callbacks can re-enter D3D12 and wait on this queue. Publish
    // command-buffer completion after releasing command-list storage so the CPU
    // command heap cannot be recycled while chunk.reset() still walks it.
    cpu_coherent.signal(internal_seq);
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
    for (auto &callback : completion_callbacks)
      callback();
    const auto callbacks_end_time = clock::now();
    if (DxmtQueueDiagEnabled() && !completion_callbacks.empty()) {
      WARN_FILE_ONLY("DXMT queue diagnostic: CompletionCallbacks"
           " seq=", internal_seq,
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
