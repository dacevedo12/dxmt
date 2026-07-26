#pragma once

// The Metal draw commands one replay draw packet turns into, once the shared
// encoder state (PSO, dynamic state, bindings) has already been applied.
//
// These are the tails of EncodeReplayDraw*Packet in
// d3d12_command_queue_draw_encode.inc. The head of each of those members is
// still stuck on the queue class through EncodeReplayDrawCommonState(), but the
// tail only reads the packet and writes commands into the encoder, which is
// what makes the geometry / tessellation / plain-draw lowering — the part with
// the arithmetic worth analyzing on its own — independently compilable.
//
// `queue` is only read for the frame sequence the bindless-mirror diagnostic
// logs, and is read at exactly the point the member did, because the encoder
// thread races the recording thread's frame counter.
//
// Unqualified calls from inside CommandQueueImpl still resolve here, because
// the class lives in this same dxmt::d3d12 namespace.

#include "d3d12_replay_draw_packet_types.hpp"
#include "dxmt_command_queue.hpp"
#include "dxmt_context.hpp"

#include <cstdint>

namespace dxmt {
struct FrameStatistics;
}

namespace dxmt::d3d12 {

// Emits the draw of a non-indexed packet, opening and closing its visibility
// query around it. Returns early — leaving the query open, as the member did —
// whenever the packet's tessellation or geometry metadata cannot be lowered.
void EncodeReplayDrawInstancedBody(ArgumentEncodingContext &enc,
                                   dxmt::CommandQueue &queue,
                                   ReplayDrawInstancedPacket &packet,
                                   uint64_t &argbuf_offset,
                                   dxmt::FrameStatistics *perf_stats);

// Indexed counterpart. The index buffer is taken from the packet's allocation,
// which the caller has already retained.
void EncodeReplayDrawIndexedInstancedBody(
    ArgumentEncodingContext &enc, dxmt::CommandQueue &queue,
    ReplayDrawIndexedInstancedPacket &packet, uint64_t &argbuf_offset,
    dxmt::FrameStatistics *perf_stats);

// Indirect counterpart: resolves the argument buffer access and emits the
// (indexed) indirect draw. The caller has already checked that the packet has
// a primitive type; an indexed packet without an index allocation is dropped.
void EncodeReplayDrawIndirectCompiledBody(
    ArgumentEncodingContext &enc, ReplayDrawIndirectCompiledPacket &packet);

} // namespace dxmt::d3d12
