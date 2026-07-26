#pragma once

// Uploads of capture-time frozen binding bytes into per-submission argument
// buffer slices.
//
// These helpers used to live inside CommandQueueImpl
// (d3d12_command_queue_binding_snapshot.inc /
// d3d12_command_queue_native_binding.inc). The only piece of the command queue
// they ever used was the DXMT command queue that owns the argument-buffer ring,
// which is now passed in, so they can be compiled and analysed on their own.

#include "d3d12_bindless_mirror_plan.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"
#include "dxmt_gpu_lifetime.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace dxmt::d3d12 {

// Argument-buffer bytes larger than this are refused: a corrupted freeze must
// not memcpy multi-GB into a bad mapping.
// Written as a size_t product rather than `16u * 1024u * 1024u`: a byte count
// is a size_t, and a byte count assembled out of 32-bit factors is exactly the
// shape that starts wrapping once someone raises the cap.
inline constexpr size_t kMaxArgumentUploadBytes = size_t{16} * 1024 * 1024;

// Copies `size` bytes into a freshly allocated argument-buffer slice for the
// encoder's current sequence. Returns an empty slice for an empty or
// oversized upload, or when the allocation/write failed.
[[nodiscard]] AllocatedArgumentBufferSlice
UploadArgumentBufferBytes(::dxmt::CommandQueue &queue, ArgumentEncodingContext &enc,
                          const void *bytes, size_t size,
                          size_t alignment = 64);

// Copies the native root-table base words into an argument-buffer slice.
// Returns an empty slice when there are no words or the upload failed.
[[nodiscard]] AllocatedArgumentBufferSlice
UploadNativeRootTableBases(::dxmt::CommandQueue &queue, ArgumentEncodingContext &enc,
                           const std::vector<uint32_t> &root_bases);

// Uploads one frozen bindless stage to GPU argument-buffer slices and returns
// the root_offsets slice for slot 28. Texture/sampler windows are written into
// *window for slots 29/30.
[[nodiscard]] AllocatedArgumentBufferSlice UploadFrozenBindlessStageTables(
    ::dxmt::CommandQueue &queue, ArgumentEncodingContext &enc,
    const FrozenBindlessStageTables &frozen,
    BindlessMirrorWindow *window = nullptr);

} // namespace dxmt::d3d12
