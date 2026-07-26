#pragma once

#include "d3d12_command_list.hpp"
#include "d3d12_replay_binding_types.hpp"
#include "d3d12_replay_compiled_payload_types.hpp"
#include "dxmt_context.hpp"

#include <memory>

namespace dxmt::d3d12 {

// Appends one encoder resource access to a compiled direct access list,
// merging the access flags into an existing entry that already describes the
// exact same stage / kind / resource / range / view.
void AddCompiledDirectEncoderAccess(
    CompiledDirectAccessList &list,
    CompiledDirectAccessList::EncoderAccess access);

// Reports whether any pre-raster access in the list would read a resource the
// current render pass still has to write, which forces the pass to be split.
// Fragment and compute stage accesses never require the reverse boundary.
[[nodiscard]] bool CompiledDirectAccessListRequiresReverseBoundary(
    ArgumentEncodingContext &enc, const CompiledDirectAccessList &list);

// Publishes the residency and hazard dependencies of a compiled packet into the
// current encoder. `test_telemetry` may be null.
void PublishCompiledDirectAccessListForEncode(
    ArgumentEncodingContext &enc, const CompiledDirectAccessList &list,
    CompiledCommandTestTelemetry *test_telemetry = nullptr);

// Retains the frozen native descriptor backend of a submission for the current
// GPU sequence, exactly once per sequence. A null store is ignored.
void RetainFrozenNativeStoreForGpu(
    ArgumentEncodingContext &enc,
    const std::shared_ptr<SubmittedFrozenNativeDescriptorStore> &store);

} // namespace dxmt::d3d12
