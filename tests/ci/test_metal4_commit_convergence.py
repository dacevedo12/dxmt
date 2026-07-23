#!/usr/bin/env python3
"""Architecture policy: Metal4 commit converged toward D3DMetal (no GPU wait under lock)."""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
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
                return source[brace + 1 : index]
    raise AssertionError(f"unterminated body after source marker: {marker}")


class Metal4CommitConvergenceTest(unittest.TestCase):
    def setUp(self):
        # Prefer the real implementation (reentrancy guard), not a declaration.
        self.commit = braced_body(
            WINEMETAL, "DXMT Metal4 reentrant commit rejected"
        )
        # braced_body starts at first '{' after marker — that's the fprintf string
        # brace. Locate the method body instead:
        marker = "- (uint64_t)commitLocked {\n  if (dxmt_metal4_commit_locked_depth > 0)"
        start = WINEMETAL.find(marker)
        self.assertGreaterEqual(start, 0, "commitLocked implementation missing")
        brace = WINEMETAL.find("{", start)
        depth = 0
        for index in range(brace, len(WINEMETAL)):
            if WINEMETAL[index] == "{":
                depth += 1
            elif WINEMETAL[index] == "}":
                depth -= 1
                if depth == 0:
                    self.commit = WINEMETAL[brace + 1 : index]
                    break

    def test_reentrant_commit_is_rejected(self):
        self.assertIn("dxmt_metal4_commit_locked_depth", WINEMETAL)
        self.assertIn("reentrant commit rejected", self.commit)

    def test_cpu_throttle_wait_is_outside_submission_lock(self):
        unlock = self.commit.index("submissionLock unlock")
        wait = self.commit.index("waitUntilSignaledValue:throttleTarget")
        relock = self.commit.index("submissionLock lock", unlock)
        self.assertLess(unlock, wait)
        self.assertLess(wait, relock)
        first_lock = self.commit.index("submissionLock lock")
        critical = self.commit[first_lock:unlock]
        self.assertNotIn("waitUntilSignaledValue:", critical)

    def test_phase_preflight_only_after_ownership(self):
        first_lock = self.commit.index("submissionLock lock")
        pre_lock = self.commit[:first_lock]
        self.assertNotIn("DXMTMetal4QueueMonitorPhaseCommitPreflight", pre_lock)
        post = self.commit[first_lock:]
        self.assertIn("DXMTMetal4QueueMonitorPhaseCommitPreflight", post)

    def test_retire_completed_residency_before_submission_lock(self):
        first_lock = self.commit.index("submissionLock lock")
        pre_lock = self.commit[:first_lock]
        self.assertIn("retireCompletedResidency", pre_lock)

    def test_closed_loop_publishes_on_pre_submit_abort(self):
        self.assertIn("completionTimelineOwned", self.commit)
        self.assertIn("metalSubmitted = YES", self.commit)
        finally_body = self.commit[self.commit.rindex("@finally") :]
        self.assertIn("!metalSubmitted", finally_body)
        self.assertIn("feedbackComplete = YES", finally_body)
        self.assertIn("signaledValue = completionValue", finally_body)


if __name__ == "__main__":
    unittest.main()
