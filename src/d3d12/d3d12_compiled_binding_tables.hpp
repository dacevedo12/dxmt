#pragma once

#include "Metal.hpp"
#include "dxmt_context.hpp"

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace dxmt {
struct FrameStatistics;
}

namespace dxmt::d3d12 {

struct CompiledCommandRootDescriptorTable;

// Finds the resolved compiled root descriptor table bound at `root_index` for
// `heap_type`. Returns nullptr when the compiled command has no such table.
[[nodiscard]] const CompiledCommandRootDescriptorTable *
FindCompiledRootTable(
    const std::vector<CompiledCommandRootDescriptorTable> &tables,
    UINT root_index, D3D12_DESCRIPTOR_HEAP_TYPE heap_type);

// Accumulates per-frame counters describing how many compiled root descriptor
// tables were backed by a native descriptor heap. `stats` may be null.
void RecordCompiledDescriptorBackendStats(
    dxmt::FrameStatistics *stats,
    const std::vector<CompiledCommandRootDescriptorTable> &tables);

// A compiled dirty mask only describes the delta against the compiled command's
// own base state. It can be reused when the encoder still holds that exact base
// identity; otherwise every slot has to be treated as dirty.
[[nodiscard]] uint64_t ResolveCompiledDirtyMask(const void *cached_identity,
                                                const void *base_identity,
                                                uint64_t compiled_mask);

[[nodiscard]] uint32_t ResolveCompiledDirtyMask(const void *cached_identity,
                                                const void *base_identity,
                                                uint32_t compiled_mask);

// Keeps a directly bound Metal buffer alive until the current submission has
// been consumed by the GPU. A null buffer is ignored.
void RetainDirectBufferForGpu(ArgumentEncodingContext &enc,
                              WMT::Buffer buffer);

} // namespace dxmt::d3d12
