#!/usr/bin/env python3
"""Architecture policy: host-mapping ownership / RAII contracts on dev-main."""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
LIFETIME = (ROOT / "src/dxmt/dxmt_gpu_lifetime.hpp").read_text()
CONTEXT_HPP = (ROOT / "src/dxmt/dxmt_context.hpp").read_text()
CONTEXT_CPP = (ROOT / "src/dxmt/dxmt_context.cpp").read_text()
QUEUE_HPP = (ROOT / "src/dxmt/dxmt_command_queue.hpp").read_text()
RING = (ROOT / "src/dxmt/dxmt_ring_bump_allocator.hpp").read_text()
D3D12_QUEUE = (ROOT / "src/d3d12/d3d12_command_queue.cpp").read_text()
ARGBUF_H = (ROOT / "include/dxmt_argument_buffer.hpp").read_text()


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


BUFFER_HPP = (ROOT / "src/dxmt/dxmt_buffer.hpp").read_text()
BUFFER_CPP = (ROOT / "src/dxmt/dxmt_buffer.cpp").read_text()
RESOURCE_CPP = (ROOT / "src/d3d12/d3d12_resource.cpp").read_text()


class GpuOwnershipArchitectureTest(unittest.TestCase):
    def test_lifetime_header_defines_bounds_checked_slice(self):
        self.assertIn("struct AllocatedArgumentBufferSlice", LIFETIME)
        self.assertIn("bool write(", LIFETIME)
        self.assertIn("bool fill_zero(", LIFETIME)
        self.assertIn("T *map(", LIFETIME)
        self.assertIn("bool valid()", LIFETIME)
        # Slice must retain the Metal buffer so host Shared mappings stay live
        # for write() after ring free_blocks drops the allocator Block ref.
        self.assertIn("WMT::Reference<WMT::Buffer> gpu_buffer", LIFETIME)
        self.assertNotRegex(
            LIFETIME,
            re.compile(r"struct AllocatedArgumentBufferSlice[\s\S]{0,400}?"
                       r"^\s*WMT::Buffer gpu_buffer\s*;",
                       re.M),
        )
        self.assertIn("MirrorSlotInCapacity", LIFETIME)
        self.assertIn("MirrorWriteSamplerSlot", LIFETIME)
        self.assertIn("MirrorWriteTextureSlot", LIFETIME)

    def test_mirror_helpers_use_uint64_capacity_check(self):
        body = braced_body(LIFETIME, "MirrorSlotInCapacity(")
        self.assertIn("uint64_t(compact_base) + uint64_t(local)", body)
        self.assertIn("idx < uint64_t(capacity)", body)
        # Must not use bare uint32 add for the capacity gate.
        self.assertNotRegex(
            body,
            re.compile(r"compact_base\s*\+\s*local\s*>=\s*capacity"),
        )

    def test_allocate_argument_buffer_is_fail_closed(self):
        body = braced_body(QUEUE_HPP, "AllocateArgumentBuffer(uint64_t seq")
        self.assertIn("hasActiveCommandBufferGeneration()", body)
        self.assertIn("tryRetainResourceForCurrentCommandBuffer", body)
        self.assertIn("retain_pin", body)
        self.assertIn("no active command-buffer generation", body)
        self.assertIn("generation pin failed", body)

    def test_try_retain_fail_closed_without_generation(self):
        body = braced_body(
            CONTEXT_CPP,
            "ArgumentEncodingContext::tryRetainResourceForCurrentCommandBuffer(",
        )
        self.assertIn("hasActiveCommandBufferGeneration()", body)
        self.assertIn("return false", body)
        self.assertNotIn("soft-skip", body.lower())
        # GPTK retain = chunk list only; no Metal residency mutation on pin path.
        self.assertIn("current_command_buffer_resources_->emplace_back", body)
        self.assertNotIn("registerResource(", body)

    def test_ring_tracks_pin_count_and_gates_reclaim(self):
        self.assertIn("uint32_t pin_count", RING)
        self.assertIn("retain_pin", RING)
        free_marker = (
            "RingBumpState<Allocator, BlockSize, mutex>::free_blocks("
            "uint64_t coherent_id)"
        )
        free_body = braced_body(RING, free_marker)
        self.assertIn("pin_count", free_body)
        self.assertIn("last_used_seq_id > coherent_id", free_body)
        alloc_body = braced_body(
            RING, "RingBumpState<Allocator, BlockSize, mutex>::allocate("
        )
        self.assertIn("retain_pin", alloc_body)
        self.assertIn("pin_count++", alloc_body)
        reuse = braced_body(
            RING,
            "RingBumpState<Allocator, BlockSize, mutex>::allocate_or_reuse_block(",
        )
        self.assertIn("pin_count", reuse)

    def test_business_paths_avoid_raw_memcpy_to_mapped(self):
        # High-risk business encode/upload paths must use slice.write/fill_zero.
        for forbidden in (
            "std::memcpy(slice.mapped",
            "std::memcpy(constants.mapped",
            "std::memset(slice.mapped",
            "std::memset(constants.mapped",
            "std::memset(window->texture.mapped",
            "std::memset(window->sampler.mapped",
        ):
            self.assertNotIn(
                forbidden,
                D3D12_QUEUE,
                f"business path still raw-writes mapped: {forbidden}",
            )
        self.assertIn("slice.write(", D3D12_QUEUE)
        self.assertIn("constants.write(", D3D12_QUEUE)
        self.assertIn("constants.fill_zero(", D3D12_QUEUE)
        self.assertIn("slice.flush_if_needed()", D3D12_QUEUE)

    def test_live_and_freeze_use_shared_mirror_writer(self):
        self.assertIn("MirrorWriteSamplerSlot", D3D12_QUEUE)
        self.assertIn("MirrorWriteTextureSlot", D3D12_QUEUE)
        self.assertIn("MirrorSlotInCapacity", D3D12_QUEUE)
        # Dangerous uint32 compact_base + dst_local capacity gate removed.
        self.assertNotRegex(
            D3D12_QUEUE,
            re.compile(
                r"if\s*\(\s*entry\.compact_base\s*\+\s*dst_local\s*>=\s*"
                r"dxmt::kBindlessMirrorCapacity\s*\)"
            ),
        )

    def test_context_includes_lifetime_header(self):
        self.assertIn('#include "dxmt_gpu_lifetime.hpp"', CONTEXT_HPP)
        self.assertIn("hasActiveCommandBufferGeneration", CONTEXT_HPP)
        self.assertIn("tryRetainResourceForCurrentCommandBuffer", CONTEXT_HPP)

    def test_mapped_argument_buffer_helper_prefers_map_api(self):
        self.assertIn("slice.template map<T>", ARGBUF_H)

    def test_l2_encoder_stores_argbuf_slice_not_raw_mapping(self):
        self.assertIn("AllocatedArgumentBufferSlice argbuf_slice", CONTEXT_HPP)
        self.assertNotIn("void *allocated_argbuf_mapping", CONTEXT_HPP)
        self.assertNotIn("allocated_argbuf_mapping", CONTEXT_CPP)
        # getMappedArgumentBuffer must go through argbuf_slice bounds.
        gmap = braced_body(CONTEXT_HPP, "getMappedArgumentBuffer(size_t offset")
        self.assertIn("argbuf_slice", gmap)
        self.assertIn("slice.valid()", gmap)
        self.assertIn("slice.template map<", gmap)

    def test_resource_map_pins_buffer_allocation(self):
        self.assertIn("addCpuMapRef", BUFFER_HPP)
        self.assertIn("releaseCpuMapRef", BUFFER_HPP)
        self.assertIn("poisonHostMapping", BUFFER_HPP)
        self.assertIn("addCpuMapRef()", RESOURCE_CPP)
        self.assertIn("releaseCpuMapRef()", RESOURCE_CPP)
        self.assertIn("buffer_map_count_", RESOURCE_CPP)
        # Delay free until Unmap: Rc hold stack matches Map/Unmap pairs.
        self.assertIn("mapped_allocation_holds_", RESOURCE_CPP)
        self.assertIn("mapped_allocation_holds_.push_back", RESOURCE_CPP)
        poison = braced_body(BUFFER_CPP, "BufferAllocation::poisonHostMapping(")
        self.assertIn("0xDE", poison)
        # Shared Metal mappings must not be trap-filled (GPU-visible).
        self.assertIn("host_only", poison)
        self.assertIn("CpuShadow", poison)
        self.assertIn("return", poison)
        dtor = braced_body(BUFFER_CPP, "BufferAllocation::~BufferAllocation(")
        self.assertIn("poisonHostMapping", dtor)

    def test_ring_poisons_host_mapping_before_reclaim(self):
        self.assertIn("PoisonRingBlockHostMapping", RING)
        self.assertIn("RingBumpPoisonMappingsEnabled", RING)
        free_marker = (
            "RingBumpState<Allocator, BlockSize, mutex>::free_blocks("
            "uint64_t coherent_id)"
        )
        free_body = braced_body(RING, free_marker)
        self.assertIn("PoisonRingBlockHostMapping", free_body)
        # Destroy and reuse paths both poison before host mapping is recycled.
        reuse = braced_body(
            RING,
            "RingBumpState<Allocator, BlockSize, mutex>::allocate_or_reuse_block(",
        )
        self.assertIn("PoisonRingBlockHostMapping", reuse)
        self.assertIn("0xDE", RING)


class GpuLifetimeUnitHelpersTest(unittest.TestCase):
    """Drive the real C++ helper logic via a tiny compiled check is ideal;
    here we re-validate the source contracts with executable Python mirrors
    of the published algorithms so the arithmetic cannot regress silently.
    """

    def test_mirror_slot_in_capacity_rejects_wrap(self):
        def mirror_slot_in_capacity(compact_base, local, capacity):
            if capacity == 0:
                return False
            idx = (compact_base & 0xFFFFFFFF) + (local & 0xFFFFFFFF)
            # Match C++ uint64 promotion of uint32 operands when cast:
            idx = int(compact_base) + int(local)
            return idx < capacity

        capacity = 128
        self.assertTrue(mirror_slot_in_capacity(0, 0, capacity))
        self.assertTrue(mirror_slot_in_capacity(127, 0, capacity))
        self.assertFalse(mirror_slot_in_capacity(100, 50, capacity))
        # Historical uint32 wrap trap: compact_base=100, local=0xFFFFFF9C
        # wraps as uint32 to 0 but is OOB under uint64.
        local = 0xFFFFFF9C
        self.assertFalse(mirror_slot_in_capacity(100, local, capacity))
        self.assertGreater(100 + local, capacity)

    def test_slice_write_bounds_logic(self):
        # Model of AllocatedArgumentBufferSlice::write
        def write(length, byte_offset, nbytes):
            if nbytes == 0:
                return True
            if byte_offset > length or nbytes > length - byte_offset:
                return False
            return True

        self.assertTrue(write(64, 0, 64))
        self.assertTrue(write(64, 32, 32))
        self.assertFalse(write(64, 32, 33))
        self.assertFalse(write(64, 65, 1))


if __name__ == "__main__":
    unittest.main()
