#pragma once

// Namespace-level replay-state fragment types (pending query work and resolved
// vertex/index buffer bindings).
//
// These definitions used to be nested inside the anonymous-namespace class
// CommandQueueImpl (d3d12_command_queue_replay_types.inc). They are the parts
// of the per-command-list ReplayState that never mention the queue class, so
// hoisting them to dxmt::d3d12 lets the query-resolve and vertex/index
// resolution helpers move into independently compiled translation units.

#include "Metal.hpp"
#include "d3d12_command_list.hpp"
#include "d3d12_query.hpp"
#include "d3d12_queue_diagnostics_env.hpp"
#include "d3d12_queue_replay_helpers.hpp"
#include "d3d12_resource.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_perf_stats.hpp"
#include "util_noexcept.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

struct PendingTimestampResolve {
  struct Sample {
    Rc<TimestampQuery> query;
#if DXMT_DX12_METAL4
    WMT::Reference<WMT::CounterHeap> heap;
    uint64_t heap_entry_size = 0;
#endif
    uint64_t index = ~0ull;
  };
  uintptr_t command_list_identity = 0;
  Com<ID3D12QueryHeap> heap;
  Com<ID3D12Resource> dst_buffer;
  std::vector<Sample> samples;
  UINT start_index = 0;
  UINT query_count = 0;
  UINT64 dst_offset = 0;
  UINT64 byte_count = 0;
};

struct PendingTimestampMarker {
  Com<ID3D12QueryHeap> heap;
  Rc<TimestampQuery> query;
  D3D12_QUERY_TYPE type = D3D12_QUERY_TYPE_TIMESTAMP;
  UINT index = 0;
};

struct PendingCpuQueryResolve {
  ResolveQueryDataRecord record;
  QueryResolveSnapshot snapshot;
};

class DeferredCpuQueryResolveTarget final : public CpuQueryResolveTarget {
public:
  DeferredCpuQueryResolveTarget(
      uintptr_t command_list_identity, Com<ID3D12QueryHeap> query_heap,
      D3D12_QUERY_TYPE type, UINT start_index, UINT query_count,
      UINT64 dst_buffer_offset, uintptr_t queue_identity,
      QueryResolveSnapshot snapshot) noexcept
      : command_list_identity_(command_list_identity),
        query_heap_(std::move(query_heap)), type_(type),
        start_index_(start_index), query_count_(query_count),
        dst_buffer_offset_(dst_buffer_offset),
        queue_identity_(queue_identity), snapshot_(std::move(snapshot)) {}

  DeferredCpuQueryResolveTarget(
      const DeferredCpuQueryResolveTarget &) = delete;
  DeferredCpuQueryResolveTarget &
  operator=(const DeferredCpuQueryResolveTarget &) = delete;
  ~DeferredCpuQueryResolveTarget() noexcept override = default;

  void Resolve(Resource &resource) noexcept override {
    dxmt::invokeNoexcept("D3D12 deferred CPU query resolve",
                         [this, &resource]() {
      if (type_ == D3D12_QUERY_TYPE_TIMESTAMP) {
        g_timestamp_cpu_map_materialized_fallbacks.fetch_add(
            1, std::memory_order_relaxed);
        dxmt::perf::recordTimestampCpuMaterialized();
      }
      ResolveQueryDataToCpuBufferStatic(
          reinterpret_cast<const void *>(command_list_identity_),
          query_heap_.ptr(), type_, start_index_, query_count_, &resource,
          resource.GetD3D12Resource(), dst_buffer_offset_, snapshot_,
          "cpu-deferred-map", queue_identity_);
    });
  }

private:
  const uintptr_t command_list_identity_;
  Com<ID3D12QueryHeap> query_heap_;
  const D3D12_QUERY_TYPE type_;
  const UINT start_index_;
  const UINT query_count_;
  const UINT64 dst_buffer_offset_;
  const uintptr_t queue_identity_;
  QueryResolveSnapshot snapshot_;
};

struct ResolvedReplayVertexBuffer {
  bool valid = false;
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
  UINT64 binding_offset = 0;
  Com<ID3D12Resource> d3d_resource;
  Resource *resource = nullptr;
  Rc<Buffer> buffer;
};

struct ResolvedReplayIndexBuffer {
  bool valid = false;
  D3D12_GPU_VIRTUAL_ADDRESS address = 0;
  UINT64 binding_offset = 0;
  Com<ID3D12Resource> d3d_resource;
  Resource *resource = nullptr;
  Rc<BufferAllocation> allocation;
};

} // namespace dxmt::d3d12
