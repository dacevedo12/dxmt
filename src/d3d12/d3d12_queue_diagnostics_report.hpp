#pragma once

#include "d3d12_command_list.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_replay_diagnostics.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_context.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// "[a,b,c]" rendering of a root-base/root-constant word list.
[[nodiscard]] std::string
D3D12DiagRootBaseWords(const std::vector<uint32_t> &words);

// Space separated hex dump, capped at 64 bytes.
[[nodiscard]] std::string D3D12DiagHexBytes(const uint8_t *bytes, size_t size);

// Comma separated float dump, capped at 16 floats.
[[nodiscard]] std::string D3D12DiagFloatWords(const uint8_t *bytes, size_t size);

// Comma separated float4 at `byte_offset`, or "oob" when out of range.
[[nodiscard]] std::string
D3D12DiagFloatsAt(const uint8_t *bytes, size_t size, size_t byte_offset);

// Comma separated index dump for a 16- or 32-bit index buffer, capped at 32.
[[nodiscard]] std::string
D3D12DiagIndexWords(const uint8_t *bytes, size_t size, DXGI_FORMAT format);

// Returns a CPU pointer into `allocation` at `offset` and reports how many
// bytes are readable in `available`, or nullptr when the allocation is not CPU
// visible / the offset is out of range.
[[nodiscard]] const uint8_t *
D3D12DiagMappedAllocationBytes(BufferAllocation *allocation, uint64_t offset,
                               uint64_t requested, uint64_t &available);

// Dumps the native descriptor tables and root bases of one encoded packet under
// DXMT_DIAG_ROOT_CAUSE_DENSE.
void D3D12DiagLogNativePacket(
    const char *kind, uint64_t frame, uint64_t sequence,
    uint64_t record_serial, const PipelineState &pipeline,
    obj_handle_t metal_pso,
    const std::vector<CompiledCommandRootDescriptorTable> &tables,
    std::initializer_list<
        std::pair<const char *, const CompiledNativeStageBinding *>> stages);

// Creates a visibility query for a sampled draw and fills `retirement`, or
// returns nullptr when this draw is not sampled.
[[nodiscard]] Rc<VisibilityResultQuery> D3D12DiagCreateDrawVisibilityQuery(
    const char *kind, const std::string &pso, uint64_t d3d_sequence,
    uint32_t vertex_count, uint32_t index_count, uint32_t instance_count,
    DrawVisibilityRetirementWork *retirement);

// Dumps the resolved inputs (IA, root tables, root constants, root descriptors)
// of a compiled graphics packet for the configured target PSO.
void D3D12DiagLogCompiledTargetInputs(
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const PipelineState &pipeline, uint64_t frame, uint64_t record_serial);

[[nodiscard]] const char *D3D12FillModeName(D3D12_FILL_MODE mode);
[[nodiscard]] const char *D3D12CullModeName(D3D12_CULL_MODE mode);
[[nodiscard]] const char *
D3D12TextureCopyTypeName(D3D12_TEXTURE_COPY_TYPE type);
[[nodiscard]] const char *PipelineStageName(PipelineStage stage);

} // namespace dxmt::d3d12
