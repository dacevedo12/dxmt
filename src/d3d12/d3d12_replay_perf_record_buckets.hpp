#pragma once

// PERF DIAG: per-CommandRecord-overload replay time attribution.
//
// RecordReplayRecordPerfBucket<T>() used to be a private static member
// template of class CommandQueueImpl (d3d12_command_queue_pass_queue.inc). It
// reads no CommandQueueImpl instance member and never names `this`: it only
// folds one duration into the thread-local PerDrawSubTimers ledger, selecting
// the bucket from the CommandRecord payload type.
//
// It lives in its own header rather than in d3d12_replay_perf_timers.hpp on
// purpose: the bucket selection needs the full CommandRecord payload type set
// from d3d12_command_list.hpp, and that ledger header is deliberately kept
// dependency-free so the many replay fragments that only need the counters do
// not pull the command-list definitions in with them.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_command_list.hpp"
#include "d3d12_replay_perf_timers.hpp"

#include <cstdint>
#include <type_traits>

namespace dxmt::d3d12 {

template <typename T>
void RecordReplayRecordPerfBucket(uint64_t us) {
  auto &timers = perDrawSubTimers();
  if constexpr (std::is_same_v<T, DrawInstancedRecord>) {
    timers.recordDrawUs += us;
    timers.recordDrawCount++;
  } else if constexpr (std::is_same_v<T, DrawIndexedInstancedRecord>) {
    timers.recordDrawIndexedUs += us;
    timers.recordDrawIndexedCount++;
  } else if constexpr (std::is_same_v<T, DispatchRecord>) {
    timers.recordDispatchUs += us;
    timers.recordDispatchCount++;
  } else if constexpr (std::is_same_v<T, PipelineStateRecord> ||
                       std::is_same_v<T, ClearStateRecord>) {
    timers.recordPipelineStateUs += us;
    timers.recordPipelineStateCount++;
  } else if constexpr (std::is_same_v<T, DescriptorHeapsRecord>) {
    timers.recordDescriptorHeapsUs += us;
    timers.recordDescriptorHeapsCount++;
  } else if constexpr (std::is_same_v<T, RootSignatureRecord>) {
    timers.recordRootSignatureUs += us;
    timers.recordRootSignatureCount++;
  } else if constexpr (std::is_same_v<T, RootDescriptorTableRecord>) {
    timers.recordRootTableUs += us;
    timers.recordRootTableCount++;
  } else if constexpr (std::is_same_v<T, RootDescriptorRecord>) {
    timers.recordRootDescriptorUs += us;
    timers.recordRootDescriptorCount++;
  } else if constexpr (std::is_same_v<T, RootConstantsRecord>) {
    timers.recordRootConstantsUs += us;
    timers.recordRootConstantsCount++;
  } else if constexpr (std::is_same_v<T, VertexBuffersRecord> ||
                       std::is_same_v<T, IndexBufferRecord> ||
                       std::is_same_v<T, PrimitiveTopologyRecord> ||
                       std::is_same_v<T, ViewportRecord> ||
                       std::is_same_v<T, ScissorRecord> ||
                       std::is_same_v<T, BlendFactorRecord> ||
                       std::is_same_v<T, StencilRefRecord>) {
    timers.recordVertexIndexStateUs += us;
    timers.recordVertexIndexStateCount++;
  } else if constexpr (std::is_same_v<T, RenderTargetsRecord>) {
    timers.recordRenderTargetsUs += us;
    timers.recordRenderTargetsCount++;
  } else if constexpr (std::is_same_v<T, ResourceBarrierRecord>) {
    timers.recordResourceBarrierUs += us;
    timers.recordResourceBarrierCount++;
  } else if constexpr (std::is_same_v<T, CopyBufferRegionRecord> ||
                       std::is_same_v<T, CopyTextureRegionRecord> ||
                       std::is_same_v<T, CopyResourceRecord> ||
                       std::is_same_v<T, CopyTilesRecord> ||
                       std::is_same_v<T, ResolveSubresourceRecord> ||
                       std::is_same_v<T, ClearRenderTargetRecord> ||
                       std::is_same_v<T, ClearDepthStencilRecord> ||
                       std::is_same_v<T, ClearUnorderedAccessRecord> ||
                       std::is_same_v<T, DiscardResourceRecord> ||
                       std::is_same_v<T, WriteBufferImmediateRecord>) {
    timers.recordCopyClearResolveUs += us;
    timers.recordCopyClearResolveCount++;
  } else if constexpr (std::is_same_v<T, BeginQueryRecord> ||
                       std::is_same_v<T, EndQueryRecord> ||
                       std::is_same_v<T, ResolveQueryDataRecord> ||
                       std::is_same_v<T, PredicationRecord>) {
    timers.recordQueryUs += us;
    timers.recordQueryCount++;
  } else if constexpr (std::is_same_v<T, ExecuteIndirectRecord>) {
    timers.recordExecuteIndirectUs += us;
    timers.recordExecuteIndirectCount++;
  } else if constexpr (std::is_same_v<T, TemporalUpscaleRecord>) {
    timers.recordTemporalUpscaleUs += us;
    timers.recordTemporalUpscaleCount++;
  } else {
    static_assert(!sizeof(T), "CommandRecord payload lacks replay perf bucket");
  }
}

} // namespace dxmt::d3d12
