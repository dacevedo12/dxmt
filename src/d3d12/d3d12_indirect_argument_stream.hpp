#pragma once

// Byte-level layout of the ExecuteIndirect() argument stream.
//
// Everything here works on data the application writes into a GPU buffer, so
// the range arithmetic below is the only guard between a malformed command
// signature or argument buffer and an out-of-range read. It used to be spread
// through CommandQueueImpl (d3d12_command_queue_indirect.inc); as its own
// translation unit the static analyzer can solve the constraints end to end.

#include "d3d12_command_list.hpp"
#include "d3d12_indirect_topology.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <d3d12.h>

namespace dxmt::d3d12 {

// How ExecuteIndirect() lowers a command signature.
struct ExecuteIndirectDispatchPlan {
  // False when the signature cannot be replayed at all; the reason has already
  // been logged.
  bool valid = false;
  // None means the signature carries state arguments and needs the CPU
  // fallback; otherwise the direct GPU path may be attempted first.
  DirectIndirectOperation direct_operation = DirectIndirectOperation::None;
};

[[nodiscard]] ExecuteIndirectDispatchPlan
PlanExecuteIndirectDispatch(const CommandSignature &signature);

// Byte extents the direct ExecuteIndirect() path reads and writes.
struct IndirectArgumentStreamLayout {
  // False when the single argument does not fit the signature stride; the
  // reason has already been logged.
  bool valid = false;
  UINT argument_size = 0;
  // Bytes covered in the application's argument buffer, i.e. from the first
  // argument through the end of the last one. The stride padding after the
  // final command is deliberately not included.
  UINT64 source_length = 0;
  // Bytes of the tightly packed stream the count predicate expands into.
  UINT64 counted_length = 0;
};

/** `command_count` must be at least one; ExecuteIndirect() with a zero maximum
 *  command count is dropped before it gets here. */
[[nodiscard]] IndirectArgumentStreamLayout
ComputeIndirectArgumentStreamLayout(const D3D12_INDIRECT_ARGUMENT_DESC &argument,
                                    UINT byte_stride, UINT command_count);

/** Offset of one command's arguments. Once the count predicate has expanded the
 *  stream the commands are packed at `argument_size`, otherwise they sit at the
 *  signature stride starting from `arg_base_offset`. */
[[nodiscard]] UINT64 IndirectCommandArgumentOffset(bool prepared_counted_stream,
                                                   UINT64 arg_base_offset,
                                                   UINT command_index,
                                                   UINT byte_stride,
                                                   UINT argument_size);

// One argument inside a CPU-read command, already bounds checked.
struct IndirectArgumentSpan {
  bool valid = false;
  UINT size = 0;
  const uint8_t *bytes = nullptr;
};

/** Locates `argument` at `argument_offset` inside a single command of
 *  `command_size` bytes. Returns an invalid span (having logged) when the
 *  argument type is unsupported or runs past the command, which means the whole
 *  ExecuteIndirect() has to be dropped. */
[[nodiscard]] IndirectArgumentSpan
LocateIndirectArgument(const uint8_t *command, size_t command_size,
                       size_t argument_offset,
                       const D3D12_INDIRECT_ARGUMENT_DESC &argument);

// The three operation arguments, decoded from the argument buffer bytes. The
// caller has already checked that the span holds the whole structure.
[[nodiscard]] DrawInstancedRecord
DecodeIndirectDrawArguments(const uint8_t *bytes);

[[nodiscard]] DrawIndexedInstancedRecord
DecodeIndirectDrawIndexedArguments(const uint8_t *bytes);

[[nodiscard]] DispatchRecord DecodeIndirectDispatchArguments(const uint8_t *bytes);

} // namespace dxmt::d3d12
