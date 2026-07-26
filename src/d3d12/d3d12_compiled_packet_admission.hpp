#pragma once

// Whether a Close-time compiled packet is emitted directly, silently skipped,
// or handed to the compatibility replay path.
//
// This is the prologue of QueueCompiledGraphicsPacket /
// QueueCompiledComputePacket in d3d12_command_queue_execute.inc: a chain of
// checks over the compiled packet, its submission-time preparation and the
// optional indirect node, none of which ever read the queue instance. Isolating
// the classification from the emit code that follows it is what lets a path
// sensitive analyzer see the branch conditions without expanding the whole
// queue class.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_command_list.hpp"
#include "d3d12_indirect_topology.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"

#include <d3d12.h>

namespace dxmt::d3d12 {

// True when the graphics packet has to be emitted, with `indirect_operation` /
// `indirect_desc` / `indirect_argument` resolved for an indirect node.
//
// False means the caller reports `reason` and emits nothing:
// CompiledCommandFallbackReason::None for a packet that is a legitimate no-op
// (zero vertex or instance count), any other value for a packet that has to go
// through compatibility replay. The three out-parameters are left untouched in
// that case except where the signature resolver itself wrote them.
[[nodiscard]] bool AdmitCompiledGraphicsPacket(
    const CompiledGraphicsPacket &packet,
    const SubmittedCompiledGraphicsPacket &prepared,
    const ExecuteIndirectRecord *indirect,
    DirectIndirectOperation &indirect_operation,
    const D3D12_COMMAND_SIGNATURE_DESC *&indirect_desc,
    const D3D12_INDIRECT_ARGUMENT_DESC *&indirect_argument,
    CompiledCommandFallbackReason &reason);

// Compute counterpart. Only a Dispatch command signature is admitted, and a
// direct dispatch with an empty thread group grid is the no-op case.
[[nodiscard]] bool AdmitCompiledComputePacket(
    const CompiledComputePacket &packet,
    const SubmittedCompiledComputePacket &prepared,
    const ExecuteIndirectRecord *indirect,
    const D3D12_COMMAND_SIGNATURE_DESC *&indirect_desc,
    const D3D12_INDIRECT_ARGUMENT_DESC *&indirect_argument,
    CompiledCommandFallbackReason &reason);

} // namespace dxmt::d3d12
