#!/usr/bin/env python3
"""Architecture policy: bindless tables freeze at capture, encode only uploads."""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
QUEUE = (ROOT / "src/d3d12/d3d12_command_queue.cpp").read_text()


class BindlessMaterializeArchitectureTest(unittest.TestCase):
    def test_snapshot_struct_owns_frozen_cpu_tables(self):
        self.assertIn("struct FrozenBindlessStageTables", QUEUE)
        self.assertIn("frozen_bindless_vertex", QUEUE)
        self.assertIn("frozen_bindless_pixel", QUEUE)
        self.assertIn("frozen_bindless_compute", QUEUE)
        self.assertNotIn("bindless_root_offsets_vertex", QUEUE)
        self.assertNotIn("bindless_root_offsets_pixel", QUEUE)

    def test_capture_freezes_tables(self):
        self.assertIn("FreezeAllBindlessStageTables(*snapshot, pipeline, compute)", QUEUE)
        self.assertIn("void FreezeBindlessStageTables(", QUEUE)

    def test_encode_uploads_only(self):
        self.assertIn("UploadFrozenBindlessStageTables", QUEUE)
        # No encode-time recipe rebuild entry point.
        self.assertNotIn("BuildBindlessRootOffsetsFromSnapshot", QUEUE)
        encode = QUEUE.split("EncodeShaderBindingsForStageBindlessSnapshot")[1]
        encode = encode.split("void BindRootConstantsSnapshot")[0]
        self.assertIn("UploadFrozenBindlessStageTables", encode)
        self.assertNotIn("GetBoundDescriptorRecordInRangeFromHeap", encode)
        self.assertIn("FrozenBindlessTablesForStage", encode)

    def test_upload_validates_mapped_pointer(self):
        self.assertIn("UploadArgumentBufferBytes", QUEUE)
        self.assertRegex(
            QUEUE,
            re.compile(
                r"UploadArgumentBufferBytes[\s\S]{0,800}if \(!slice\.valid\(\)",
                re.M,
            ),
        )
        self.assertIn("slice.write(", QUEUE)


if __name__ == "__main__":
    unittest.main()
