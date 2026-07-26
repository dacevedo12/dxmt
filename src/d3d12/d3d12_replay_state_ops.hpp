#pragma once

#include "dxmt_command_queue.hpp"

#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// Records that the replay deliberately split two passes apart. The chunk is
// unused; it only keeps the call site symmetrical with the other pass helpers.
void EmitPassSeparator(CommandChunk *);

// Writes `new_values` into a root-constant slot starting at `dst_offset`,
// growing the slot with zeroes when it has never been written that far.
void ApplyRootConstants(std::vector<UINT> &values, UINT dst_offset,
                        const std::vector<UINT> &new_values);

// Evaluates a predication predicate against the value read back from the
// predication buffer. Unsupported operations warn and let the command run.
[[nodiscard]] bool PredicationValuePasses(D3D12_PREDICATION_OP operation,
                                          uint64_t value);

} // namespace dxmt::d3d12
