#!/usr/bin/env python3

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
DXMT_QUEUE = (ROOT / "src/dxmt/dxmt_command_queue.cpp").read_text()
DXMT_QUEUE_HPP = (ROOT / "src/dxmt/dxmt_command_queue.hpp").read_text()
DXMT_CONTEXT = (ROOT / "src/dxmt/dxmt_context.cpp").read_text()
WINEMETAL = (ROOT / "src/winemetal4/unix/winemetal_unix.c").read_text()


def braced_body(source: str, marker: str) -> str:
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing source marker: {marker}")
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body after source marker: {marker}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"unterminated body after source marker: {marker}")


class ResidencyArchitecturePolicyTest(unittest.TestCase):
    def test_queue_owns_and_attaches_exactly_one_persistent_set(self):
        self.assertEqual(DXMT_QUEUE.count("newResidencySet("), 1)
        self.assertEqual(DXMT_QUEUE.count(".requestResidency()"), 1)
        self.assertEqual(DXMT_QUEUE.count(".addResidencySet("), 1)
        self.assertIn("persistent_residency_set_(device.newResidencySet(", DXMT_QUEUE)
        self.assertIn(
            "persistent_residency_set_.requestResidency()", DXMT_QUEUE
        )
        self.assertIn(
            "commandQueue.addResidencySet(persistent_residency_set_)",
            DXMT_QUEUE,
        )

    def test_command_buffer_encode_does_not_rebuild_residency_sets(self):
        encode = braced_body(DXMT_QUEUE, "CommandQueue::CommitChunkInternal(")
        for forbidden in (
            "newResidencySet(",
            "requestResidency(",
            "addResidencySet(",
            "useResidencySet(",
        ):
            self.assertNotIn(forbidden, encode)

    def test_completion_retires_resources_without_committing_the_set(self):
        completion = braced_body(DXMT_QUEUE, "CommandQueue::WaitForFinishThread(")
        retire = braced_body(
            DXMT_QUEUE, "CommandQueue::RetirePersistentResidencyRemovals("
        )
        self.assertNotIn("persistent_residency_set_.commit()", completion)
        self.assertNotIn("FlushPersistentResidency(", completion)
        self.assertNotIn("persistent_residency_set_.commit()", retire)
        signal = completion.index("cpu_coherent.signal(")
        retire_call = completion.index("RetirePersistentResidencyRemovals(")
        deferred = completion.index("CompleteDeferredReleases(")
        self.assertLess(signal, retire_call)
        self.assertLess(retire_call, deferred)

    def test_temporary_releases_are_keyed_to_completed_sequences(self):
        retain = braced_body(
            DXMT_QUEUE,
            "CommandQueue::RetainUntilGpuComplete(uint64_t sequence",
        )
        complete = braced_body(
            DXMT_QUEUE, "CommandQueue::CompleteDeferredReleases("
        )
        self.assertIn("deferred_releases_[sequence]", retain)
        self.assertIn("sequence <= deferred_release_completed_seq_", retain)
        self.assertIn("it->first <= deferred_release_completed_seq_", complete)
        self.assertIn("releases.push_back(std::move(release))", complete)

    def test_persistent_membership_is_reference_counted(self):
        add = braced_body(DXMT_QUEUE, "CommandQueue::AddPersistentResidency(")
        remove = braced_body(
            DXMT_QUEUE,
            "CommandQueue::RemovePersistentResidencyAfterCompletion(\n"
            "    WMT::Object allocation, uint64_t sequence)",
        )
        self.assertIn("entry.ref_count++", add)
        self.assertIn("entry->second.ref_count--", remove)
        self.assertRegex(
            remove,
            re.compile(
                r"if\s*\(entry->second\.ref_count\)\s*return\s*;",
                re.MULTILINE,
            ),
        )

    def test_winemetal_has_no_command_buffer_residency_path(self):
        for forbidden in (
            "_residencySet",
            "prepareResidencyForCommit",
            "releaseSparseResidencyAllocations",
        ):
            self.assertNotIn(forbidden, WINEMETAL)
        self.assertNotRegex(
            WINEMETAL,
            re.compile(r"@synchronized\s*\(\s*(?:_owner|queue)\s*\)"),
        )

    def test_sparse_mapping_is_ordered_by_queue_events(self):
        mapping = braced_body(
            WINEMETAL, "_MTLCommandQueue_updateSparseTextureMappings("
        )
        wait = mapping.index("waitForEvent:")
        update = mapping.index("updateTextureMappings:")
        signal = mapping.index("signalEvent:")
        self.assertLess(wait, update)
        self.assertLess(update, signal)
        self.assertIn(
            "[queue.sparseTrackedTextures removeObject:texture]", mapping
        )
        logical_remove = mapping.index(
            "[queue.sparseTrackedTextures removeObject:texture]"
        )
        retire_texture = mapping.index(
            "[retiredAllocations addObject:(id<MTLAllocation>)texture]"
        )
        self.assertLess(signal, logical_remove)
        self.assertLess(logical_remove, retire_texture)

    def test_command_chunk_owns_resources_from_recording_to_completion(self):
        self.assertIn(
            "std::vector<WMT::Reference<WMT::Resource>> "
            "retained_metal_resources",
            DXMT_QUEUE_HPP,
        )
        self.assertIn(
            "std::unordered_set<obj_handle_t> "
            "retained_metal_resource_handles",
            DXMT_QUEUE_HPP,
        )
        encode = braced_body(DXMT_QUEUE, "CommandChunk::encode(")
        begin = encode.index("beginCommandBufferResourceLifetime(")
        replay = encode.index("list_enc.execute(enc)")
        flush = encode.index("enc.flushCommands(")
        end = encode.index("endCommandBufferResourceLifetime()")
        self.assertLess(begin, replay)
        self.assertLess(replay, flush)
        self.assertLess(flush, end)

    def test_recording_deduplicates_then_owns_resources_without_residency_pin(self):
        # GPTK RetainResourceForCurrentCommandBuffer: chunk list only.
        retain = braced_body(
            DXMT_CONTEXT,
            "ArgumentEncodingContext::tryRetainResourceForCurrentCommandBuffer(",
        )
        deduplicate = retain.index(
            "current_command_buffer_resource_handles_->insert(resource.handle)"
        )
        own = retain.index(
            "current_command_buffer_resources_->emplace_back(resource)"
        )
        self.assertLess(deduplicate, own)
        self.assertIn("hasActiveCommandBufferGeneration()", retain)
        # Must not call Metal residency / registerResource on the pin path.
        self.assertNotIn("registerResource(", retain)
        self.assertNotIn("[set containsAllocation", retain)
        self.assertNotIn("current_command_buffer_.registerResource", retain)

    def test_unix_register_resource_is_encode_local_not_membership_locked(self):
        self.assertNotIn("retainAllocationForLifetime", WINEMETAL)
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        register = braced_body(
            implementation,
            "- (void)registerResourceForCurrentCommandBuffer:",
        )
        self.assertIn("dxmt_metal4_backing_allocation(allocation)", register)
        self.assertIn("_queueResidencyAllocationSet", register)
        # No global residency ops lock / external set scan on this path.
        self.assertNotIn("dxmt_residency_membership_lock_get", register)
        self.assertNotIn("dxmt_residency_set_ops_lock_get", register)
        self.assertNotIn("[set containsAllocation", register)
        self.assertNotIn("externalPersistentResidencySets", register)
        self.assertNotIn(
            "[_retainedTemporaryResources addObject:allocation]", register
        )
        temporary = braced_body(
            implementation,
            "- (void)retainTemporaryResourceForCurrentCommandBuffer:",
        )
        self.assertIn(
            "[_retainedTemporaryResources addObject:allocation]", temporary
        )
        self.assertIn(
            "[self registerResourceForCurrentCommandBuffer:allocation]",
            temporary,
        )

    def test_residency_set_ops_serialized_under_single_ops_lock(self):
        self.assertIn("dxmt_residency_set_ops_lock", WINEMETAL)
        for marker in (
            "_MTLResidencySet_addAllocation(",
            "_MTLResidencySet_removeAllocation(",
            "_MTLResidencySet_commit(",
        ):
            body = braced_body(WINEMETAL, marker)
            self.assertIn("dxmt_residency_membership_lock_get", body)
        prepare = braced_body(
            WINEMETAL,
            "prepareCommandResidencyForAllocations:(NSArray *)allocations\n"
            "                                  completionValue:(uint64_t)completionValue {",
        )
        self.assertIn("dxmt_residency_set_ops_lock_get", prepare)
        self.assertIn("[_sparseResidencyLock lock]", prepare)
        # Lock order: sparse then ops.
        sparse = prepare.index("[_sparseResidencyLock lock]")
        ops = prepare.index("dxmt_residency_set_ops_lock_get")
        self.assertLess(sparse, ops)
        retire = braced_body(WINEMETAL, "- (void)retireCompletedResidency {")
        self.assertIn("dxmt_residency_set_ops_lock_get", retire)
        self.assertIn("[opsLock unlock]", retire)

    def test_commit_transfers_an_immutable_lifetime_snapshot(self):
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        commit = braced_body(implementation, "- (uint64_t)commitLocked")
        # Accept either "NSArray *name = [container copy]" or the hoisted
        # closed-loop form "name = [container copy]".
        snapshot = re.search(
            r"(?:NSArray\s*\*\s*)?(\w*(?:Retained|Lifetime|Resource)\w*)"
            r"\s*=\s*\[\s*(\w+)\s+copy\s*\]",
            commit,
            re.IGNORECASE,
        )
        self.assertIsNotNone(
            snapshot,
            "commit must copy the current lifetime generation into an "
            "immutable completion-owned snapshot",
        )
        snapshot_name, current_container = snapshot.groups()
        after_snapshot = commit[snapshot.end() :]
        self.assertRegex(
            after_snapshot,
            re.compile(
                rf"{re.escape(current_container)}\s*=\s*"
                r"\[\[\s*NSMutableArray\s+alloc\s*\]\s+init\s*\]"
            ),
            "commit must immediately replace the recording lifetime "
            "container after taking the snapshot",
        )
        self.assertIn(f"[{snapshot_name} release]", commit)

    def test_feedback_hands_off_lifetime_snapshot_without_draining_it(self):
        """CompletionQueue only latches + transfers; finish thread drains.

        Freeing temporary GPU resources on com.Metal4.CompletionQueue is unsafe
        under Wine (no TEB) and was a hang/crash root cause.
        """
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        commit = braced_body(implementation, "- (uint64_t)commitLocked")
        snapshot = re.search(
            r"(?:NSArray\s*\*\s*)?(\w*(?:Retained|Lifetime|Resource)\w*)"
            r"\s*=\s*\[\s*\w+\s+copy\s*\]",
            commit,
            re.IGNORECASE,
        )
        self.assertIsNotNone(snapshot)
        snapshot_name = snapshot.group(1)
        feedback = braced_body(commit, "addFeedbackHandler:^")
        finalizer = feedback[feedback.index("@finally") :]
        # Handoff to pendingLifetimeSnapshot, then drop the block-local retain.
        self.assertIn("pendingLifetimeSnapshot", finalizer)
        self.assertIn(f"[{snapshot_name} release]", finalizer)
        # Must not drain on the completion queue itself.
        self.assertNotIn("drainPendingLifetimeOwnership", finalizer)
        self.assertNotIn("releaseRetainedAllocations", implementation)

    def test_feedback_handler_publishes_success_or_error_terminal_state(self):
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        commit = braced_body(implementation, "- (uint64_t)commitLocked")
        feedback = braced_body(commit, "addFeedbackHandler:^")
        finalizer = feedback[feedback.index("@finally") :]
        self.assertRegex(
            finalizer,
            re.compile(
                r"internalStatus\s*=\s*"
                r"error\s*\?\s*DXMTMetal4CommandBufferStateError"
                r"\s*:\s*DXMTMetal4CommandBufferStateCompleted"
            ),
        )
        handoff = finalizer.index("pendingLifetimeSnapshot")
        complete = finalizer.index("feedbackComplete = YES")
        broadcast = finalizer.index("feedbackCondition broadcast")
        unlock = finalizer.index("feedbackCondition unlock")
        # Publish latch after handoff while still holding the condition lock.
        self.assertLess(handoff, complete)
        self.assertLess(complete, broadcast)
        self.assertLess(broadcast, unlock)
        self.assertIn("feedbackCondition lock", finalizer)

    def test_queue_preflight_rejection_publishes_completion_latch(self):
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        commit = braced_body(implementation, "- (uint64_t)commitLocked")
        rejection = braced_body(commit, "if (queueError)")
        lock = rejection.index("feedbackCondition lock")
        error = rejection.index(
            "internalStatus = DXMTMetal4CommandBufferStateError"
        )
        complete = rejection.index("feedbackComplete = YES")
        broadcast = rejection.index("feedbackCondition broadcast")
        unlock = rejection.index("feedbackCondition unlock")
        self.assertLess(lock, error)
        self.assertLess(error, complete)
        self.assertLess(complete, broadcast)
        self.assertLess(broadcast, unlock)

    def test_wait_until_completed_waits_for_timeline_and_feedback_latch(self):
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        wait = braced_body(implementation, "- (void)waitUntilCompleted")
        timeline_wait = wait.index("waitUntilSignaledValue:")
        latch_lock = wait.rindex("feedbackCondition lock")
        latch_loop = wait.index("while (!_feedbackComplete)")
        latch_wait = wait.index("feedbackCondition wait")
        latch_unlock = wait.rindex("feedbackCondition unlock")
        self.assertLess(timeline_wait, latch_lock)
        self.assertLess(latch_lock, latch_loop)
        self.assertLess(latch_loop, latch_wait)
        self.assertLess(latch_wait, latch_unlock)

    def test_wait_until_completed_drains_lifetime_after_latch(self):
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        wait = braced_body(implementation, "- (void)waitUntilCompleted")
        latch_loop = wait.index("while (!_feedbackComplete)")
        latch_wait = wait.index("feedbackCondition wait")
        drain = wait.index("drainPendingLifetimeOwnership")
        recycle = wait.index("recycleMetal4ObjectsIfNeeded")
        self.assertLess(latch_loop, latch_wait)
        self.assertLess(latch_wait, drain)
        self.assertLess(drain, recycle)
        # Waiter is the sole drain site for the temporary snapshot.
        self.assertNotIn("releaseRetainedAllocations", wait)
        self.assertNotIn("removeAllObjects", wait)
        self.assertNotIn("ResidencySet", wait)
        self.assertNotIn("DXMTMetal4CommandBufferStateCompleted", wait)
        self.assertNotIn("_feedbackComplete = YES", wait)
        # drainPendingLifetimeOwnership body must free pendingLifetimeSnapshot.
        drain_body = braced_body(
            implementation, "- (void)drainPendingLifetimeOwnership"
        )
        self.assertIn("pendingLifetimeSnapshot", drain_body)
        self.assertIn("[snapshot release]", drain_body)

    def test_prepare_residency_is_add_only_and_retire_is_separate(self):
        # Locate the method implementation explicitly (selector also appears
        # in the @interface declaration, which would confuse braced_body).
        marker = (
            "- (uint64_t)prepareCommandResidencyForAllocations:"
            "(NSArray *)allocations\n"
            "                                  completionValue:(uint64_t)completionValue {"
        )
        start = WINEMETAL.find(marker)
        self.assertGreaterEqual(start, 0, "prepare implementation missing")
        brace = WINEMETAL.find("{", start)
        depth = 0
        prepare = ""
        for index in range(brace, len(WINEMETAL)):
            if WINEMETAL[index] == "{":
                depth += 1
            elif WINEMETAL[index] == "}":
                depth -= 1
                if depth == 0:
                    prepare = WINEMETAL[brace + 1 : index]
                    break
        self.assertIn("path=add-only", prepare)
        self.assertNotIn("retireCompletedResidency", prepare)
        self.assertIn("- (void)retireCompletedResidency {", WINEMETAL)
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        commit = braced_body(implementation, "- (uint64_t)commitLocked")
        retire = commit.index("retireCompletedResidency")
        first_lock = commit.index("submissionLock lock")
        self.assertLess(
            retire,
            first_lock,
            "transient residency retirement must run outside submissionLock",
        )

    def test_commit_closed_loop_tracks_submission_and_timeline(self):
        implementation = WINEMETAL[
            WINEMETAL.index("@implementation DXMTMetal4CommandBuffer") :
        ]
        commit = braced_body(implementation, "- (uint64_t)commitLocked")
        self.assertIn("completionTimelineOwned", commit)
        self.assertIn("metalSubmitted", commit)
        self.assertIn("completionTimelineSignaled", commit)
        self.assertIn("pre-submit-abort", commit)
        self.assertIn("DXMTMetal4CommitDomain", commit)
        finally_body = commit[commit.rindex("@finally") :]
        self.assertIn("metalSubmitted", finally_body)
        self.assertIn("signaledValue = completionValue", finally_body)
        self.assertIn("feedbackComplete = YES", finally_body)

    def test_metal4_object_pool_recycle_after_gpu_complete(self):
        self.assertIn("metal4CommandBufferPool", WINEMETAL)
        self.assertIn("metal4AllocatorPool", WINEMETAL)
        self.assertIn("acquireCommandAllocator", WINEMETAL)
        self.assertIn("recycleCommandBuffer:", WINEMETAL)
        marker = "- (void)recycleMetal4ObjectsIfNeeded {\n"
        start = WINEMETAL.find(marker)
        self.assertGreaterEqual(start, 0, "recycle implementation missing")
        brace = WINEMETAL.find("{", start)
        depth = 0
        recycle = ""
        for index in range(brace, len(WINEMETAL)):
            if WINEMETAL[index] == "{":
                depth += 1
            elif WINEMETAL[index] == "}":
                depth -= 1
                if depth == 0:
                    recycle = WINEMETAL[brace + 1 : index]
                    break
        self.assertIn("[_allocator reset]", recycle)
        self.assertIn("DXMTMetal4CommandBufferStateCompleted", recycle)
        self.assertIn("recycleCommandBuffer:", recycle)


if __name__ == "__main__":
    unittest.main()
