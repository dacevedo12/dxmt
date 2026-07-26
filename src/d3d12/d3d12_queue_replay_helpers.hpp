#pragma once

#include "d3d12_command_list.hpp"
#include "d3d12_pipeline.hpp"
#include "d3d12_query.hpp"
#include "d3d12_resource.hpp"
#include "d3d12_root_signature.hpp"
#include "d3d12_tile_mapping.hpp"
#include "dxmt_command_queue.hpp"

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Non-RTTI downcast via private IID (no AddRef) — replaces dynamic_cast on the
// replay hot path. A non-dxmt object returns E_NOINTERFACE, preserving
// dynamic_cast's null-on-mismatch semantics.
[[nodiscard]] Resource *GetResource(ID3D12Resource *resource);
[[nodiscard]] PipelineState *
GetPipelineState(ID3D12PipelineState *pipeline_state);
[[nodiscard]] RootSignature *
GetRootSignature(ID3D12RootSignature *root_signature);

// Copies `size` bytes out of a CPU-visible buffer resource, warning with
// `context` and returning false when that is not possible.
[[nodiscard]] bool ReadBufferBytes(ID3D12Resource *resource, UINT64 offset,
                                   void *dst, UINT64 size, const char *context);

// True when [offset, offset+size) fits inside the buffer resource.
[[nodiscard]] bool ValidateBufferRange(Resource *resource, UINT64 offset,
                                       UINT64 size, const char *context);

// Writes a resolved query snapshot into the destination buffer on the CPU.
void ResolveQueryDataToCpuBufferStatic(
    const void *command_list_identity, ID3D12QueryHeap *query_heap,
    D3D12_QUERY_TYPE type, UINT start_index, UINT query_count, Resource *dst,
    ID3D12Resource *dst_identity, UINT64 dst_buffer_offset,
    const QueryResolveSnapshot &snapshot, const char *context,
    uintptr_t queue_id);

void ResolveQueryDataToCpuBufferStatic(const ResolveQueryDataRecord &record,
                                       const QueryResolveSnapshot &snapshot,
                                       const char *context, uintptr_t queue_id);

// Emits the blit-pass resource-state barrier a sparse texture needs after its
// tile mappings change.
void EmitSparseTextureMappingBarrier(
    CommandChunk *chunk, Rc<Texture> texture,
    std::vector<SparseTileBarrierSubresource> subresources);

} // namespace dxmt::d3d12
