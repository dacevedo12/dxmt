#pragma once

#include "Metal.hpp"
#include "rc/util_rc_ptr.hpp"
#include "wsi_platform.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace dxmt {
class VisibilityResultOffsetBumpState {
public:
  void
  beginEncoder() {
    assert(!within_encoder);
    assert(!current_data_is_dirty);
    assert(!~previous_offset);
    within_encoder = true;
  }

  bool
  tryGetNextWriteOffset(bool has_active_occlusion_queries, uint64_t &offset) {
    assert(within_encoder);
    offset = getNextWriteOffset(has_active_occlusion_queries);
    if (offset == previous_offset) {
      return false;
    }
    previous_offset = offset;
    return true;
  }

  uint64_t
  getNextReadOffset() {
    if (within_encoder) {
      if (current_data_is_dirty) {
        current_data_is_dirty = false;
        return ++next_offset;
      }
    }
    assert(!current_data_is_dirty);
    return next_offset;
  }

  void
  endEncoder() {
    assert(within_encoder);
    within_encoder = false;
    previous_offset = ~0uLL;
    if (current_data_is_dirty) {
      next_offset++;
      current_data_is_dirty = false;
    }
  }

  uint64_t
  reset() {
    assert(!within_encoder && "encoder still active");
    assert(!current_data_is_dirty && "encoder still active");
    auto ret = next_offset;
    next_offset = 0;
    return ret;
  }

private:
  uint64_t
  getNextWriteOffset(bool has_active_occlusion_queries) {
    if (has_active_occlusion_queries) {
      current_data_is_dirty = true;
      return next_offset;
    }
    return ~0uLL;
  };

  bool within_encoder = false;
  bool current_data_is_dirty = false;
  uint64_t previous_offset = ~0uLL;
  uint64_t next_offset = 0;
};

class VisibilityResultQuery {
public:
  void
  incRef() {
    refcount_.fetch_add(1u, std::memory_order_relaxed);
  }
  void
  decRef() {
    if (refcount_.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
      delete this;
  }

  void
  begin(uint64_t seqId, unsigned offset) {
    accumulated_value_.store(0, std::memory_order_relaxed);
    seq_id_begin = seqId;
    occlusion_counter_begin = offset;
    seq_id_end = ~0uLL;
    occlusion_counter_end = ~0uLL;
  }

  void
  end(uint64_t seqId, unsigned offset) {
    seq_id_end = seqId;
    occlusion_counter_end = offset;
    if (seq_id_begin == seq_id_end && occlusion_counter_begin == occlusion_counter_end) {
      seq_id_issued.store(seq_id_end, std::memory_order_release);
    }
  }

  uint64_t queryEndAt() {
    return seq_id_end;
  };

  void
  issue(uint64_t seqId, uint64_t const *readbackBuffer, unsigned numResults) {
    assert(seqId >= seq_id_begin);
    assert(seqId <= seq_id_end);
    uint64_t const *start = seqId == seq_id_begin ? readbackBuffer + occlusion_counter_begin : readbackBuffer;
    uint64_t const *end = seqId == seq_id_end ? readbackBuffer + occlusion_counter_end : readbackBuffer + numResults;
    assert(start <= end);
    uint64_t issued_value = 0;
    while (start != end)
      issued_value += *start++;
    accumulated_value_.fetch_add(issued_value, std::memory_order_relaxed);
    seq_id_issued.store(seqId, std::memory_order_release);
  }

  // Resolve a query whose end chunk carried no visibility results (no readback
  // was created to issue it): nothing to accumulate, so stamp the issued
  // watermark to the query's own end. MUST run at finish time in seq order: an
  // encode-time stamp would be clobbered backward by the later finish-thread
  // issue() of an earlier, still-in-flight count>0 chunk. Same-chunk empty
  // windows are already stamped in end(); this covers the cross-chunk case.
  void
  markIssuedEmpty() {
    seq_id_issued = seq_id_end;
  }

  bool
  getValue(uint64_t *value) {
    if (seq_id_end <= seq_id_issued.load(std::memory_order_acquire)) {
      *value = accumulated_value_.load(std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  void
  reset() {
    accumulated_value_.store(0, std::memory_order_relaxed);
    seq_id_begin = ~0uLL;
    occlusion_counter_begin = ~0uLL;
    seq_id_end = ~0uLL;
    occlusion_counter_end = ~0uLL;
    seq_id_issued.store(0, std::memory_order_relaxed);
  };

private:
  std::atomic<uint64_t> accumulated_value_ = {0};
  uint64_t seq_id_begin = ~0uLL;
  uint64_t occlusion_counter_begin = ~0uLL;
  uint64_t seq_id_end = ~0uLL;
  uint64_t occlusion_counter_end = ~0uLL;
  std::atomic<uint64_t> seq_id_issued = {0};
  std::atomic<uint32_t> refcount_ = {0u};
};

class VisibilityResultReadback {
public:
  VisibilityResultReadback(
      WMT::Device device, uint64_t seq_id, uint64_t num_results, std::vector<Rc<VisibilityResultQuery>> &queries
  ) :
      seq_id(seq_id),
      num_results(num_results),
      queries(queries) {
        visibility_result_heap_info.options = WMTResourceHazardTrackingModeUntracked;
        visibility_result_heap_info.memory.set(nullptr);
#ifdef __i386__
        visibility_result_heap_info.memory.set(wsi::aligned_malloc(num_results * sizeof(uint64_t), DXMT_PAGE_SIZE));
#endif
        visibility_result_heap_info.length = num_results * sizeof(uint64_t);
        visibility_result_heap = device.newBuffer(visibility_result_heap_info);
      }
  ~VisibilityResultReadback() {
    for (auto query : queries) {
      query->issue(seq_id, (uint64_t *)visibility_result_heap_info.memory.get(), num_results);
    }
    if (retire_residency_)
      retire_residency_();
#ifdef __i386__
    wsi::aligned_free(visibility_result_heap_info.memory.get());
#endif
  }

  void
  setResidencyRetirement(std::function<void()> retirement) {
    retire_residency_ = std::move(retirement);
  }

  VisibilityResultReadback(const VisibilityResultReadback &) = delete;
  VisibilityResultReadback(VisibilityResultReadback &&) = delete;

  uint64_t seq_id;
  uint64_t num_results;
  std::vector<Rc<VisibilityResultQuery>> queries;
  WMTBufferInfo visibility_result_heap_info;
  WMT::Reference<WMT::Buffer> visibility_result_heap;

private:
  std::function<void()> retire_residency_;
};

/**
 * TODO: rename the whole file to dxmt_query.hpp
 */

class TimestampQuery {
public:
  ~TimestampQuery() {
#if DXMT_DX12_METAL4
    if (release_sample_ && sample_index_ != ~0ull)
      release_sample_(sample_index_);
#endif
  }

  void
  incRef() {
    refcount_.fetch_add(1u, std::memory_order_relaxed);
  }
  void
  decRef() {
    if (refcount_.fetch_sub(1u, std::memory_order_acq_rel) == 1u)
      delete this;
  }

  bool
  getValue(uint64_t *value) {
    if (!cached_value_ready_.load(std::memory_order_acquire))
      return false;

    *value = cached_value_.load(std::memory_order_relaxed);
    return true;
  }

  void
  issue(uint64_t sampled_data) {
    cached_value_.store(sampled_data, std::memory_order_relaxed);
    cached_value_ready_.store(true, std::memory_order_release);
  }

  uint64_t
  sampleIndex() const {
    return sample_index_;
  }

  uint64_t
  sampleSequence() const {
    return sample_sequence_;
  }

  void
  setSampleSequence(uint64_t sample_sequence) {
    sample_sequence_ = sample_sequence;
  }

  void
  setSampleIndex(uint64_t sample_index) {
    sample_index_ = sample_index;
  }

  void
  setSampleLocation(uint64_t sample_sequence, uint64_t sample_index) {
    sample_sequence_ = sample_sequence;
    sample_index_ = sample_index;
  }

#if DXMT_DX12_METAL4
  void
  setResolveSource(const WMT::Reference<WMT::CounterHeap> &heap,
                   uint64_t heap_entry_size,
                   std::function<void(uint64_t)> release_sample = {}) {
    resolve_heap_ = heap;
    resolve_heap_entry_size_ = heap_entry_size;
    release_sample_ = std::move(release_sample);
  }

  WMT::Reference<WMT::CounterHeap>
  resolveHeap() const {
    return resolve_heap_;
  }

  uint64_t
  resolveHeapEntrySize() const {
    return resolve_heap_entry_size_;
  }
#endif

private:
  std::atomic<uint64_t> cached_value_ = {0};
  std::atomic<bool> cached_value_ready_ = {false};
  uint64_t sample_sequence_ = ~0ull;
  uint64_t sample_index_ = ~0ull;
#if DXMT_DX12_METAL4
  WMT::Reference<WMT::CounterHeap> resolve_heap_;
  uint64_t resolve_heap_entry_size_ = 0;
  std::function<void(uint64_t)> release_sample_;
#endif
  std::atomic<uint32_t> refcount_ = {0u};
};

using TimestampQueryList = std::vector<std::pair<Rc<TimestampQuery>, uint64_t>>;

class TimestampReadbackSBuf {
public:
  TimestampReadbackSBuf(
      WMT::Device device, WMT::CommandBuffer cmdbuf, uint64_t num_samples, TimestampQueryList &&queries
  ) :
      queries_(std::move(queries)) {
    (void)cmdbuf;
#if DXMT_DX12_METAL4
    (void)num_samples;
    timestamp_context_ = device.newTimestampContext();
    assert(timestamp_context_);
#else
    sample_buffer_ = device.newCounterSampleBuffer(num_samples, true);
    num_samples_ = num_samples;
#endif
  }

  ~TimestampReadbackSBuf() {
    // TODO: small_vector opt
#if DXMT_DX12_METAL4
    std::unordered_map<obj_handle_t, std::vector<std::pair<Rc<TimestampQuery>, uint64_t>>> groups;
    for (const auto &[query, sample_index] : queries_) {
      auto heap = query->resolveHeap();
      if (!heap || !query->resolveHeapEntrySize())
        continue;
      groups[heap.handle].push_back({query, sample_index});
    }

    for (auto &[heap_handle, group] : groups) {
      if (group.empty())
        continue;
      uint64_t heap_entry_size = group.front().first->resolveHeapEntrySize();
      uint64_t first = group.front().second;
      uint64_t last = first;
      for (const auto &[query, sample_index] : group) {
        first = std::min(first, sample_index);
        last = std::max(last, sample_index);
      }

      WMT::CounterHeap heap(heap_handle);
      std::vector<uint8_t> resolved((last - first + 1) * heap_entry_size);
      heap.resolveCounterRange(first, last - first + 1,
                               resolved.data(), resolved.size());
      for (const auto &[query, sample_index] : group) {
        auto *entry = reinterpret_cast<const WMTMTL4TimestampHeapEntry *>(
            resolved.data() + (sample_index - first) * heap_entry_size);
        query->issue(entry->timestamp);
      }
    }
    timestamp_context_.destroy();
#else
    std::vector<uint64_t> results(num_samples_);
    sample_buffer_.resolveCounterRange(0, num_samples_, results.data(), num_samples_ * sizeof(uint64_t));
    for (const auto &[query, sample_index] : queries_) {
      query->issue(results[sample_index]);
    }
#endif
  }

  TimestampReadbackSBuf(const TimestampReadbackSBuf &) = delete;
  TimestampReadbackSBuf(TimestampReadbackSBuf &&) = delete;

#if DXMT_DX12_METAL4
  WMT::TimestampContext timestampContext() {
    return timestamp_context_;
  }
#else
  WMT::CounterSampleBuffer
  sampleBuffer() {
    return sample_buffer_;
  };
#endif

private:
  TimestampQueryList queries_;
#if DXMT_DX12_METAL4
  WMT::TimestampContext timestamp_context_;
#else
  WMT::Reference<WMT::CounterSampleBuffer> sample_buffer_;
  uint64_t num_samples_;
#endif
};

class TimestampReadbackCBuf {
public:
  TimestampReadbackCBuf(
      WMT::Device device, WMT::CommandBuffer cmdbuf, uint64_t num_samples, TimestampQueryList &&queries
  ) :
      queries_(std::move(queries)),
      num_samples_(num_samples),
      cmdbuf_(cmdbuf) {}

  ~TimestampReadbackCBuf() {
    // TODO: small_vector opt
    std::vector<uint64_t> results(num_samples_);
    /**
    There is no implicit relationship between `gpuEndTime` and order of commit, but we still want a later issued query
    to return a timestamp greater or equal to previous ones, so check and use maximum.

    `thread_local` makes sense because this destructor is only called on 1. finishing thread 2. (abnormal) device
    destruction
    */
    thread_local uint64_t latest_ts_on_finish_thread = 0;
    latest_ts_on_finish_thread = std::max(cmdbuf_.gpuEndTime(), latest_ts_on_finish_thread);
    std::fill(results.begin(), results.end(), latest_ts_on_finish_thread);
    for (const auto &[query, sample_index] : queries_) {
      query->issue(results[sample_index]);
    }
  }

  TimestampReadbackCBuf(const TimestampReadbackCBuf &) = delete;
  TimestampReadbackCBuf(TimestampReadbackCBuf &&) = delete;

#if DXMT_DX12_METAL4
  WMT::CounterHeap
  counterHeap() {
    return {};
  };

  uint64_t
  heapEntrySize() const {
    return 0;
  }

  WMT::TimestampContext timestampContext() {
    return {};
  }
#else
  WMT::CounterSampleBuffer
  sampleBuffer() {
    return {};
  };
#endif

private:
  TimestampQueryList queries_;
  uint64_t num_samples_;
  WMT::CommandBuffer cmdbuf_;
};

// Timestamp counters need per-sample values. The command-buffer end-time fallback
// collapses all samples in one command buffer to the same timestamp, which makes
// begin/end deltas always resolve to zero.
using TimestampReadback = TimestampReadbackSBuf;

class TimestampQueryState {
public:
  TimestampQueryState(WMT::Device device) : device_(device) {}

  uint64_t
  addQuery(TimestampQuery *query) {
    auto sample_index = num_samples_;
    addQueryAt(query, sample_index);
    return sample_index;
  }

  uint64_t
  addQueryAt(TimestampQuery *query, uint64_t sample_index) {
    query->setSampleIndex(sample_index);
    queries_.push_back({query, sample_index});
    num_samples_ = std::max(num_samples_, sample_index + 1);
    return sample_index;
  }

  void
  coalesceQuery(TimestampQuery *query) {
    assert(num_samples_);
    query->setSampleIndex(num_samples_ - 1);
    queries_.push_back({query, num_samples_ - 1});
  }

  std::unique_ptr<TimestampReadback>
  flush(WMT::CommandBuffer cmdbuf) {
    if (num_samples_ == 0)
      return {};

    auto ret = std::make_unique<TimestampReadback>(device_, cmdbuf, num_samples_, std::move(queries_));
    num_samples_ = 0;
    queries_ = {};
    return ret;
  }

private:
  uint64_t num_samples_ = 0;
  TimestampQueryList queries_;
  WMT::Device device_;
};

struct QueryReadbacks {
  std::unique_ptr<VisibilityResultReadback> visibility;
  std::unique_ptr<TimestampReadback> timestamp;
  // Occlusion queries whose end chunk carried no visibility results, so no
  // VisibilityResultReadback exists to resolve them. The queue's finish-thread
  // completion handler stamps these in seq order, after earlier chunks'
  // readbacks have accumulated their slices.
  std::vector<Rc<VisibilityResultQuery>> visibility_empty_ends;
  std::vector<std::function<void()>> diagnostics;
};

} // namespace dxmt
