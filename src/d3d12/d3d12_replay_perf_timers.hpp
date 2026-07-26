#pragma once

// PERF DIAG: per-draw sub-function time accumulators (us), reset at replay
// start and printed in the replay breakdown, to rank descriptor-access vs
// attachments vs state-clone within the ~59us/draw.
//
// PerDrawSubTimers and perDrawSubTimers() used to be a private nested struct
// and a private static member function of class CommandQueueImpl
// (d3d12_command_queue_pass_queue.inc). They are a pure thread-local counter
// ledger: nothing here reads a queue instance member, yet every replay
// fragment pokes at them, which made them the single most frequent blocker for
// moving replay logic into standalone translation units.
//
// The ledger is one `thread_local` instance per thread for the whole program,
// exactly as before: the static member function was implicitly inline, so its
// function-local `static thread_local` already had a single program-wide
// definition. The definition now lives in d3d12_replay_perf_timers.cpp, which
// keeps that guarantee independent of inlining.
//
// Unqualified `perDrawSubTimers()` calls inside CommandQueueImpl still resolve
// here, because the class lives in this same dxmt::d3d12 namespace.

#include <cstdint>
#include <string>

namespace dxmt::d3d12 {

struct PerDrawSubTimers { uint64_t desc = 0, attach = 0, clone = 0, stateUpd = 0;
  uint64_t drawCount = 0, psoRootUnchanged = 0, fullBindUnchanged = 0;
  uint64_t recordDrawUs = 0, recordDrawCount = 0;
  uint64_t recordDrawIndexedUs = 0, recordDrawIndexedCount = 0;
  uint64_t recordDispatchUs = 0, recordDispatchCount = 0;
  uint64_t recordPipelineStateUs = 0, recordPipelineStateCount = 0;
  uint64_t recordDescriptorHeapsUs = 0, recordDescriptorHeapsCount = 0;
  uint64_t recordRootSignatureUs = 0, recordRootSignatureCount = 0;
  uint64_t recordRootTableUs = 0, recordRootTableCount = 0;
  uint64_t recordRootDescriptorUs = 0, recordRootDescriptorCount = 0;
  uint64_t recordRootConstantsUs = 0, recordRootConstantsCount = 0;
  uint64_t recordVertexIndexStateUs = 0, recordVertexIndexStateCount = 0;
  uint64_t recordRenderTargetsUs = 0, recordRenderTargetsCount = 0;
  uint64_t recordResourceBarrierUs = 0, recordResourceBarrierCount = 0;
  uint64_t recordCopyClearResolveUs = 0, recordCopyClearResolveCount = 0;
  uint64_t recordQueryUs = 0, recordQueryCount = 0;
  uint64_t recordExecuteIndirectUs = 0, recordExecuteIndirectCount = 0;
  uint64_t recordTemporalUpscaleUs = 0, recordTemporalUpscaleCount = 0;
  uint64_t bindingGenBumps = 0, bindingGenPipeline = 0,
           bindingGenDescriptorHeaps = 0, bindingGenVertexBuffers = 0,
           bindingGenRootSignature = 0, bindingGenRootDescriptorTable = 0,
           bindingGenRootDescriptor = 0, bindingGenRootConstants = 0,
           bindingGenIndirectVertexBuffer = 0,
           bindingGenIndirectRootConstants = 0,
           bindingGenIndirectRootDescriptor = 0;
  uint64_t snapshotRequests = 0, snapshotCacheHits = 0,
           snapshotCacheMisses = 0, snapshotPassthrough = 0,
           snapshotGraphicsGenChanges = 0, snapshotDescriptorGenChanges = 0,
           snapshotBothGenChanges = 0, snapshotNoGenChanges = 0;
  uint64_t snapshotCapturedEntries = 0, snapshotCapturedDescriptors = 0,
           snapshotCapturedMissingDescriptors = 0,
           snapshotCapturedRootDescriptors = 0,
           snapshotCapturedRootConstants = 0,
           snapshotCapturedVertexBuffers = 0,
           snapshotCapturedBindless = 0;
  // One-shot replay-breakdown coverage (DXMT_DIAG_REPLAY_BREAKDOWN): attribute
  // the per-pass/per-barrier fixed cost that dominates replay (decoupled from
  // draw count). Times accumulate across the WHOLE replay regardless of where
  // the flush was triggered (Clear/Copy/Resolve in the record loop, or the
  // tail flush). Reset per compiled-node stream, dumped in the breakdown line.
  uint64_t flushBlitUs = 0, flushComputeUs = 0, flushGraphicsUs = 0,
           flushBarrierUs = 0, emitTsMarkersUs = 0, buildPlanUs = 0;
  uint64_t flushBlitCalls = 0, flushComputeCalls = 0, flushGraphicsCalls = 0,
           flushBarrierCalls = 0, emitTsMarkersCalls = 0;
  // CPU attribution for the CopyTextureRegion -> QueueBlitCommand ->
  // FlushBlitBatch path. These aggregate counters intentionally avoid
  // per-copy logging so the diagnostic does not reshape the workload.
  uint64_t blitBarrierTakeUs = 0, blitBarrierLookupUs = 0,
           blitBarrierScanUs = 0,
           blitBarrierRebuildMatchedUs = 0,
           blitBarrierRebuildRemainingUs = 0,
           blitBarrierAssignUs = 0, blitSeparatorUs = 0,
           blitEmitCommandUs = 0, blitBatchResetUs = 0;
  uint64_t queueBlitHazardUs = 0, queueBlitTrackUs = 0,
           queueBlitAppendUs = 0;
  uint64_t copyTextureLookupUs = 0, copyTextureEnsureAllocationUs = 0,
           copyTexturePrepareUs = 0, copyTextureQueueUs = 0;
  uint64_t queueBlitCalls = 0, queueBlitHazardFlushes = 0,
           copyTextureCalls = 0, copyTextureQueued = 0;
  uint64_t blitBarrierPendingEntries = 0,
           blitBarrierMatchedEntries = 0,
           blitBarrierRemainingEntries = 0,
           blitBarrierPendingEntriesMax = 0;
  uint64_t blitBatchCommands = 0, blitBatchCommandsMax = 0,
           blitBatchReads = 0, blitBatchWrites = 0;
  uint64_t flushPassBatchesCalls = 0, flushPassBatchesEmpty = 0;
  uint64_t resAccessCalls = 0, resAccessSteadyNoop = 0;
  uint64_t descAccessHits = 0, descAccessMiss = 0;
  uint64_t supersededStateRecordsSkipped = 0;
  uint64_t graphicsPsoUnavailable = 0, computePsoUnavailable = 0;
  uint64_t bindlessReplayStateCacheHit = 0;
  uint64_t bindlessReplayStateCacheMiss = 0;
  std::string firstComputePsoUnavailableKey;
  // Stage-2 (passthrough-barrier) feasibility probe: sync points DXMT
  // AUTO-INFERS (mismatchBarriers) vs barrier transitions the app EXPLICITLY
  // declares (appBarrierTransitions). auto≈0 relative to explicit ⇒ FH4's
  // barriers are passthrough-complete ⇒ stage-2 full parallelism reachable.
  uint64_t mismatchBarriers = 0, appBarrierTransitions = 0;
  // Bindless graphics fast path: per-draw descriptor hazard passes skipped
  // because explicit D3D12 barriers and encode-time Metal resource access
  // carry synchronization/residency. Pairs with descAccessHits/Miss.
  uint64_t descAccessPassthrough = 0; };

// The per-thread replay diagnostic ledger. Same single thread_local instance
// the queue-private accessor returned.
PerDrawSubTimers &perDrawSubTimers();

} // namespace dxmt::d3d12
